#!/usr/bin/env python3
"""Run Clock's production frame renderer in a private guest."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory

from e2e import Harness, qemu_command, t_boot, t_login, t_prompt


def probe_image(work):
    root = work / "rootfs"
    shutil.copytree("build/rootfs", root)
    obj = work / "clock-panes.o"
    app = root / "bin/clock-panes-probe"
    compiler = shlex.split(os.environ.get("CC", "cc"))
    linker = shlex.split(os.environ.get("LD", "ld"))
    subprocess.run([*compiler, "-m64", "-march=x86-64", "-ffreestanding",
                    "-nostdlib", "-fno-stack-protector", "-fno-pic", "-fno-pie",
                    "-mno-red-zone", "-Wall", "-Wextra", "-Werror",
                    "-Ilibc/include", "-Iabi", "-Ilibgui", "-c",
                    "tools/test_clock_panes.c", "-o", obj], check=True)
    subprocess.run([*linker, "-nostdlib", "-z", "max-page-size=0x1000",
                    "-T", "apps/user.ld", "-o", app, "build/libc/crt0.o", obj,
                    "build/libgui.a", "build/libc.a"], check=True)
    fs = work / "fs.img"
    image = work / "os.img"
    subprocess.run([sys.executable, "tools/mkfs.py", "--mode", "/etc/shadow:0600",
                    "--mode", "/var/pkg/db:0700", "--mode", "/tmp:0777",
                    "--mode", "/run:0755", root, fs, "64"], check=True)
    subprocess.run([sys.executable, "tools/mkimage.py", "build/stage1.bin",
                    "build/stage2.bin", "build/kernel.bin", image, fs], check=True)
    return image


def main():
    success = "PASS Clock paint panes: dial and readout stay inside offset and cropped targets"
    with TemporaryDirectory() as directory:
        image = probe_image(Path(directory))
        command = qemu_command("rtl8139", 2)
        command[command.index("-drive") + 1] = f"file={image},format=raw"
        h = Harness(command)
        try:
            h.start()
            t_boot(h)
            t_login(h)
            t_prompt(h)
            h.send("/bin/clock-panes-probe")
            if h.expect_any([success, "FAIL Clock paint panes"]) != success:
                raise AssertionError("Clock paint bounds or pixels were incorrect")
            print(success)
        except (OSError, RuntimeError, AssertionError) as exc:
            print(f"FAIL Clock paint panes: {exc}", file=sys.stderr)
            h.print_tail()
            return 1
        finally:
            h.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
