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
        "service-managed": ["-DPROBE_MANAGED"],
        "service-managed-fail-init": ["-DPROBE_MANAGED", "-DPROBE_FAIL_INIT"],
        "service-managed-wrong-first": ["-DPROBE_MANAGED", "-DPROBE_FIRST_VERSION=2",
                                        "-DPROBE_SECOND_VERSION=1"],
        "service-managed-wrong-second": ["-DPROBE_MANAGED", "-DPROBE_SECOND_VERSION=2"],
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


def check(h, command, status, output=None, absent=None):
    h.send(command + "; echo SERVICE-STATUS-$?")
    text = h.expect(rf"(?s).*?\nSERVICE-STATUS-{status}\n", regex=True).group()
    if (output and output not in text) or (absent and absent in text):
        raise AssertionError(f"{command}: unexpected output {text!r}")
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
            init_marker = "managed probe: init entered"
            check(h, "insmod service-managed", 1,
                  "required service service-provider (v1, 8 bytes) unavailable", init_marker)
            check(h, "insmod service-provider", 0)
            for mismatch in ("wrong-first", "wrong-second"):
                check(h, f"insmod service-managed-{mismatch}", 1,
                      "required service service-provider (v2, 8 bytes) unavailable", init_marker)
            check(h, "insmod service-managed-fail-init", 1, "managed probe: init read 42")
            check(h, "insmod service-managed", 0, "managed probe: init read 42")
            check(h, "rmmod service-provider", 1, "is in use (1 references)")
            check(h, "rmmod service-managed", 0, "managed probe: exit read 42")
            check(h, "rmmod service-provider", 0)
            print("PASS required module services: pre-init rejection, rollback, provider pinned through exit")
        except (OSError, RuntimeError, AssertionError) as exc:
            print(f"FAIL module services: {exc}", file=sys.stderr)
            h.print_tail()
            return 1
        finally:
            h.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
