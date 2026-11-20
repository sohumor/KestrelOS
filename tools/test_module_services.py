#!/usr/bin/env python3
"""Test module service compatibility and provider lifetime in a private guest."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory

from e2e import Harness, qemu_command, t_boot, t_login, t_prompt, wait_prompt


def probe_image(work):
    root = work / "rootfs"
    shutil.copytree("build/rootfs", root)
    modules = root / "lib/modules"
    cc = shlex.split(os.environ.get("CC", "cc"))
    flags = ["-m64", "-march=x86-64", "-ffreestanding", "-nostdlib",
             "-fno-stack-protector", "-fno-pic", "-fno-pie", "-fno-builtin",
             "-mno-red-zone", "-mno-mmx", "-mno-sse", "-mno-sse2",
             "-mcmodel=kernel", "-fno-common", "-fno-asynchronous-unwind-tables",
             "-O2", "-Wall", "-Wextra", "-Werror", "-Ikernel/include", "-Iabi"]
    variants = {
        "service-provider": ["-DPROBE_PROVIDER"],
        "service-reader": [],
        "service-wrong-version": ["-DPROBE_VERSION=2"],
        "service-wrong-size": ["-DPROBE_EXTRA_SIZE=1"],
    }
    for name, defines in variants.items():
        subprocess.run([*cc, *flags, *defines, "-c", "tools/module_service_probe.c",
                        "-o", modules / (name + ".kmod")], check=True)
    fs = work / "fs.img"
    subprocess.run([sys.executable, "tools/mkfs.py", "--mode", "/etc/shadow:0600",
                    "--mode", "/var/pkg/db:0700", "--mode", "/tmp:0777",
                    "--mode", "/run:0755", root, fs, "64"], check=True)
    image = work / "os.img"
    subprocess.run([sys.executable, "tools/mkimage.py", "build/stage1.bin",
                    "build/stage2.bin", "build/kernel.bin", image, fs], check=True)
    return image


def check(h, command, status, output=None):
    h.send(command + "; echo SERVICE-STATUS-$?")
    if output:
        h.expect(output)
    h.expect(f"\nSERVICE-STATUS-{status}\n")
    wait_prompt(h)


def main():
    with TemporaryDirectory() as directory:
        try:
            image = probe_image(Path(directory))
        except (OSError, subprocess.CalledProcessError) as exc:
            print(f"FAIL module services image: {exc}", file=sys.stderr)
            return 1
        command = qemu_command("rtl8139", 2)
        command[command.index("-drive") + 1] = f"file={image},format=raw"
        h = Harness(command)
        try:
            h.start()
            t_boot(h)
            t_login(h)
            t_prompt(h)
            check(h, "insmod service-reader", 1, "service probe: acquire -8")
            check(h, "insmod service-provider", 0)
            check(h, "insmod service-wrong-version", 1, "service probe: acquire -8")
            check(h, "insmod service-wrong-size", 1, "service probe: acquire -8")
            check(h, "insmod service-reader", 0, "service probe: read 42")
            check(h, "rmmod service-provider", 1, "is in use (1 references)")
            check(h, "rmmod service-reader", 0)
            check(h, "rmmod service-provider", 0)
            print("PASS module services: exact contract and provider lifetime")
        except (OSError, RuntimeError, AssertionError) as exc:
            print(f"FAIL module services: {exc}", file=sys.stderr)
            h.print_tail()
            return 1
        finally:
            h.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
