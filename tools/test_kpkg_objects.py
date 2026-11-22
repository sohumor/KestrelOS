#!/usr/bin/env python3
"""Check digest-addressed package installs and declared launch grants."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory

from e2e import (Harness, PKG_TIMEOUT, USER_NAME, USER_PASS, qemu_command,
                 t_boot, t_login, t_prompt, wait_prompt)


def check(h, command, status=0, output=None):
    h.send(command + "; echo OBJECT-STATUS-$?")
    if output:
        h.expect(output, timeout=PKG_TIMEOUT)
    h.expect(f"\nOBJECT-STATUS-{status}\n", timeout=PKG_TIMEOUT)
    wait_prompt(h, timeout=PKG_TIMEOUT)


def launch_image(work):
    root = work / "rootfs"
    shutil.copytree("build/rootfs", root)
    payload = work / "payload"
    (payload / "bin").mkdir(parents=True)
    obj = work / "probe.o"
    compiler = shlex.split(os.environ.get("CC", "cc"))
    linker = shlex.split(os.environ.get("LD", "ld"))
    subprocess.run([*compiler, "-m64", "-march=x86-64", "-ffreestanding",
                    "-nostdlib", "-fno-stack-protector", "-fno-pic", "-fno-pie",
                    "-mno-red-zone", "-Wall", "-Wextra", "-Werror",
                    "-Ilibc/include", "-Iabi", "-c", "tools/kpkg_launch_probe.c",
                    "-o", obj], check=True)
    subprocess.run([*linker, "-nostdlib", "-z", "max-page-size=0x1000",
                    "-T", "apps/user.ld", "-o", payload / "bin/kpkg-launch-probe",
                    "build/libc/crt0.o", obj, "build/libc.a"], check=True)
    for label, version, mask, mounts in (("launch", 1, "0x01", "/"),
                                          ("missing", 2, "0x01", "/ /not-mounted"),
                                          ("invalid", 3, "0x80", "/")):
        manifest = work / "manifest"
        manifest.write_text(f"name: launch-probe\nversion: {version}\n"
                            f"launch: /bin/kpkg-launch-probe {mask} {mounts}\n",
                            encoding="ascii")
        archive = root / f"var/pkg/repo/{label}.kpkg"
        result = subprocess.run([sys.executable, "tools/mkpkg.py", "--manifest", manifest,
                                 "--root", payload, "--out", archive],
                                capture_output=True, text=True)
        if label == "invalid":
            assert result.returncode != 0 and "invalid launch capability mask" in result.stderr
        else:
            result.check_returncode()

    fs = work / "fs.img"
    image = work / "os.img"
    subprocess.run([sys.executable, "tools/mkfs.py", "--mode", "/etc/shadow:0600",
                    "--mode", "/var/pkg/db:0700", "--mode", "/tmp:0777",
                    "--mode", "/run:0755", root, fs, "64"], check=True)
    subprocess.run([sys.executable, "tools/mkimage.py", "build/stage1.bin",
                    "build/stage2.bin", "build/kernel.bin", image, fs], check=True)
    return image


def check_launch(h):
    check(h, "kpkg install /var/pkg/repo/launch.kpkg")
    h.send("su " + USER_NAME)
    h.expect("password:")
    h.send(USER_PASS)
    h.expect("su: root -> " + USER_NAME)
    wait_prompt(h)
    check(h, "/bin/kpkg-launch-probe full -f --force",
          output="unscoped access confirmed")
    check(h, "kpkg run launch-probe scoped -f --force", 23, "declared scope applied")
    h.send("exit")
    wait_prompt(h)

    check(h, "kpkg install --force /var/pkg/repo/missing.kpkg",
          output="kpkg: installed launch-probe 2")
    check(h, "kpkg run launch-probe scoped -f --force", 1, "with its launch grant")
    check(h, "kpkg remove launch-probe")
    check(h, "kpkg run launch-probe", 1, "has no installed launch declaration")
    print("PASS kpkg launch: unprivileged run, mount and capability restrictions, missing mount, removal")


def check_image(image):
    sha = Path("build/repo/hello.kpkg").read_bytes()[-32:].hex()
    path = f"/var/pkg/objects/{sha[:8]}/{sha[8:]}"
    command = qemu_command("rtl8139", 2)
    command[command.index("-drive") + 1] = f"file={image},format=raw"
    h = Harness(command)
    try:
        h.start()
        t_boot(h)
        t_login(h)
        t_prompt(h)

        check_launch(h)
        check(h, "kpkg install hello", output=f"kpkg: object {sha}")
        check(h, "kpkg remove hello")
        check(h, "rm /var/pkg/repo/hello.kpkg")
        check(h, "kpkg install hello", output=f"kpkg: object {sha}")
        check(h, "kpkg remove hello")
        check(h, "mv /var/pkg/repo/index.kpi /var/pkg/repo/index.saved")
        check(h, f"kpkg install-object {sha}", output=f"kpkg: object {sha}")
        check(h, "hello object", output="Hello, object!")
        check(h, "kpkg run hello --force", output="Hello, --force!")
        check(h, "kpkg verify hello", output="kpkg: everything matches")

        check(h, "kpkg remove hello")
        check(h, "kpkg remove kestrel-extras")
        check(h, f"kpkg install-object {sha}", 1,
              "offline object install requires installed kestrel-extras")
        check(h, f"rm {path}")
        check(h, f"cp /var/pkg/repo/kestrel-extras.kpkg {path}")
        check(h, f"kpkg install-object {sha}", 1, "does not match requested object")
        check(h, "kpkg install-object ../../bad", 1, "usage: kpkg install-object")
        print("PASS kpkg objects: indexed reuse, offline replay, missing dependency and wrong digest rejection")
    except (OSError, RuntimeError, AssertionError) as exc:
        print(f"FAIL kpkg objects: {exc}")
        h.print_tail()
        return 1
    finally:
        h.kill()
    return 0


if __name__ == "__main__":
    with TemporaryDirectory() as directory:
        raise SystemExit(check_image(launch_image(Path(directory))))
