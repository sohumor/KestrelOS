#!/usr/bin/env python3
"""Boot a deliberately interrupted KFS3 transaction through kernel recovery."""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

from e2e import BOOT_TIMEOUT, Harness, t_login, t_prompt


BLOCK = 4096
FS_OFFSET = 2048 * 512
JOURNAL_MAGIC = 0x314C4E4A
JOURNAL_COMMITTED = 1
SEQUENCE = 0xB007
RECOVERED_GID = 4242


def block_offset(block):
    return FS_OFFSET + block * BLOCK


def inject_committed_transaction(fs_path):
    with open(fs_path, "rb") as f:
        image = bytearray(f.read())
    sb = struct.unpack_from("<13I", image, 0)
    journal_start = sb[4]
    inode_start = sb[6]

    payload = bytearray(
        image[inode_start * BLOCK:(inode_start + 1) * BLOCK])
    # Inode 1 begins at this block. Its directory mtime legitimately changes
    # during boot as /run is maintained, so use the valid but otherwise
    # untouched gid field as the replay sentinel.
    struct.pack_into("<I", payload, 12, RECOVERED_GID)

    crc = zlib.crc32(struct.pack("<II", SEQUENCE, 1))
    crc = zlib.crc32(struct.pack("<I", inode_start), crc)
    crc = zlib.crc32(payload, crc) & 0xFFFFFFFF

    header = bytearray(BLOCK)
    struct.pack_into("<5I", header, 0, JOURNAL_MAGIC, JOURNAL_COMMITTED,
                     SEQUENCE, 1, crc)
    struct.pack_into("<I", header, 20, inode_start)
    image[(journal_start + 1) * BLOCK:(journal_start + 2) * BLOCK] = payload
    image[journal_start * BLOCK:(journal_start + 1) * BLOCK] = header

    with open(fs_path, "wb") as f:
        f.write(image)
    return journal_start, inode_start


def assemble(root, fs_path, disk_path):
    subprocess.run(
        [sys.executable, os.path.join(root, "tools", "mkimage.py"),
         os.path.join(root, "build", "stage1.bin"),
         os.path.join(root, "build", "stage2.bin"),
         os.path.join(root, "build", "kernel.bin"), disk_path, fs_path],
        check=True)


def boot_and_halt(disk_path):
    cmd = [
        "qemu-system-x86_64",
        "-drive", "file=%s,format=raw" % disk_path,
        "-no-reboot",
        "-display", "none",
        "-serial", "stdio",
        "-device", "rtl8139,netdev=n0",
        "-netdev", "user,id=n0",
    ]
    h = Harness(cmd)
    try:
        h.start()
        h.expect("replayed journal transaction %d (1 blocks)" % SEQUENCE,
                 timeout=BOOT_TIMEOUT)
        h.expect("KESTREL READY", timeout=BOOT_TIMEOUT)
        t_login(h)
        t_prompt(h)
        h.send("halt")
        h.expect("power: system halted", timeout=10)
        h.proc.wait(timeout=10)
        if h.proc.returncode != 0:
            raise RuntimeError("QEMU exited with status %d" % h.proc.returncode)
    except Exception:
        h.print_tail()
        raise
    finally:
        h.kill()


def verify_disk(disk_path, journal_start, inode_start):
    with open(disk_path, "rb") as f:
        f.seek(block_offset(journal_start))
        header = f.read(BLOCK)
        if any(header):
            raise RuntimeError("kernel recovery did not clear the journal")
        f.seek(block_offset(inode_start) + 12)
        gid = struct.unpack("<I", f.read(4))[0]
    if gid != RECOVERED_GID:
        raise RuntimeError(
            "kernel recovery left root gid %d, expected %d"
            % (gid, RECOVERED_GID))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if shutil.which("qemu-system-x86_64") is None:
        print("error: qemu-system-x86_64 not found in PATH", file=sys.stderr)
        return 2
    required = ("stage1.bin", "stage2.bin", "kernel.bin", "fs.img")
    missing = [name for name in required
               if not os.path.exists(os.path.join(root, "build", name))]
    if missing:
        print("error: missing build artifacts: %s (run make first)"
              % ", ".join(missing), file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(
            prefix="kestrel-kfs-boot-recovery-") as tmp:
        fs_path = os.path.join(tmp, "fs.img")
        disk_path = os.path.join(tmp, "recovery-os.img")
        shutil.copyfile(os.path.join(root, "build", "fs.img"), fs_path)
        journal_start, inode_start = inject_committed_transaction(fs_path)
        assemble(root, fs_path, disk_path)
        boot_and_halt(disk_path)
        verify_disk(disk_path, journal_start, inode_start)

    print("PASS: kernel replayed and cleared a committed KFS3 transaction")
    return 0


if __name__ == "__main__":
    sys.exit(main())
