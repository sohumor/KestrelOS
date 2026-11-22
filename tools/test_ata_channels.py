#!/usr/bin/env python3
"""Mount and modify KFS disks on distinct compatibility ATA targets."""

from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory

from e2e import Harness, qemu_command, t_boot, t_login, t_prompt, wait_prompt


TARGETS = ("hdb", "hdc", "hdd")


def make_disks(work):
    drives = []
    for slot, disk in enumerate(TARGETS, start=1):
        root = work / disk
        root.mkdir()
        (root / "identity").write_text(f"ATA-DATA-{disk}\n")
        image = work / f"{disk}.img"
        subprocess.run([sys.executable, "tools/mkfs.py", root, image, "4"],
                       check=True)
        drives.extend(["-drive", f"file={image},format=raw,if=ide,index={slot}"])
    return drives


def run(h, command):
    h.send(f"{command}; echo ATA-STATUS-$?")
    h.expect("\nATA-STATUS-0\n")
    wait_prompt(h)


def read(h, path, expected):
    h.send(f"cat {path}")
    h.expect(f"\n{expected}\n")
    wait_prompt(h)


def check_disks(h):
    h.send("cat /dev/blocks")
    h.expect("\n" + "".join(f"{disk} 512 8192\n" for disk in TARGETS))
    wait_prompt(h)

    for disk in TARGETS:
        path = f"/tmp/{disk}"
        run(h, f"mkdir {path}")
        run(h, f"mount -t kfs {disk} {path}")
        read(h, f"{path}/identity", f"ATA-DATA-{disk}")
        run(h, f"echo ATA-WRITE-{disk} > {path}/written")

    for disk in reversed(TARGETS):
        path = f"/tmp/{disk}"
        run(h, f"mount -u {path}")
        run(h, f"mount -t kfs {disk} {path}")
        read(h, f"{path}/written", f"ATA-WRITE-{disk}")
        run(h, f"mount -u {path}")

    run(h, "cat /etc/version")


def main():
    with TemporaryDirectory() as directory:
        command = qemu_command("rtl8139", 2) + make_disks(Path(directory))
        h = Harness(command)
        try:
            h.start()
            t_boot(h)
            t_login(h)
            t_prompt(h)
            check_disks(h)
            print("PASS ATA channels: independent mounts and persistent writes on hdb, hdc, hdd")
        except (OSError, RuntimeError, AssertionError) as exc:
            print(f"FAIL ATA channels: {exc}", file=sys.stderr)
            h.print_tail()
            return 1
        finally:
            h.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
