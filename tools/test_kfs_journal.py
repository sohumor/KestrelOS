#!/usr/bin/env python3
"""Crash-injection regression for the KFS3 redo journal."""

import os
import struct
import subprocess
import sys
import tempfile
import zlib

BLOCK = 4096
JOURNAL_MAGIC = 0x314C4E4A
JOURNAL_COMMITTED = 1


def run(cmd, expect=0):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True)
    if (p.returncode == expect):
        return p.stdout
    raise RuntimeError("%r returned %d, expected %d\n%s"
                       % (cmd, p.returncode, expect, p.stdout))


def read_image(path):
    with open(path, "rb") as f:
        return bytearray(f.read())


def write_image(path, image):
    with open(path, "wb") as f:
        f.write(image)


def block(image, n):
    return bytes(image[n * BLOCK:(n + 1) * BLOCK])


def put_block(image, n, data):
    assert len(data) == BLOCK
    image[n * BLOCK:(n + 1) * BLOCK] = data


def inject(image, journal_start, sequence, target, payload, corrupt=False):
    put_block(image, journal_start + 1, payload)
    crc = zlib.crc32(struct.pack("<II", sequence, 1))
    crc = zlib.crc32(struct.pack("<I", target), crc)
    crc = zlib.crc32(payload, crc) & 0xFFFFFFFF
    if corrupt:
        crc ^= 1
    header = bytearray(BLOCK)
    struct.pack_into("<5I", header, 0, JOURNAL_MAGIC, JOURNAL_COMMITTED,
                     sequence, 1, crc)
    struct.pack_into("<I", header, 20, target)
    put_block(image, journal_start, header)


def inode_mtime(image, inode_start):
    return struct.unpack_from("<I", image, inode_start * BLOCK + 16)[0]


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    mkfs = os.path.join(root, "tools", "mkfs.py")
    kfsck = os.path.join(root, "tools", "kfsck.py")
    rootfs = os.path.join(root, "rootfs")
    checks = 0

    with tempfile.TemporaryDirectory(prefix="kestrel-kfs-journal-") as tmp:
        image_path = os.path.join(tmp, "fs.img")
        run([sys.executable, mkfs, rootfs, image_path, "8"])
        image = read_image(image_path)
        sb = struct.unpack_from("<13I", image, 0)
        journal_start = sb[4]
        inode_start = sb[6]

        desired = bytearray(block(image, inode_start))
        struct.pack_into("<I", desired, 16, 123456789)
        inject(image, journal_start, 42, inode_start, desired)
        write_image(image_path, image)

        output = run([sys.executable, kfsck, image_path], expect=1)
        if "needs recovery" not in output:
            raise RuntimeError("committed transaction was not diagnosed")
        checks += 1

        output = run([sys.executable, kfsck, "--recover", image_path])
        if "replayed journal transaction 42" not in output:
            raise RuntimeError("committed transaction was not replayed")
        image = read_image(image_path)
        if inode_mtime(image, inode_start) != 123456789:
            raise RuntimeError("replay did not install the home block")
        if any(block(image, journal_start)):
            raise RuntimeError("replay did not clear the journal header")
        checks += 3

        # Replaying the same committed transaction is idempotent.
        inject(image, journal_start, 43, inode_start, desired)
        write_image(image_path, image)
        run([sys.executable, kfsck, "--recover", image_path])
        run([sys.executable, kfsck, image_path])
        checks += 1

        # A torn payload/header is discarded and must not change home data.
        image = read_image(image_path)
        torn = bytearray(block(image, inode_start))
        struct.pack_into("<I", torn, 16, 222222222)
        inject(image, journal_start, 44, inode_start, torn, corrupt=True)
        write_image(image_path, image)
        run([sys.executable, kfsck, image_path], expect=1)
        output = run([sys.executable, kfsck, "--recover", image_path])
        if "cleared torn journal transaction" not in output:
            raise RuntimeError("torn transaction was not cleared")
        image = read_image(image_path)
        if inode_mtime(image, inode_start) != 123456789:
            raise RuntimeError("torn transaction changed its home block")
        checks += 2

        # A committed transaction may never target its own journal.
        inject(image, journal_start, 45, journal_start, bytes(BLOCK))
        write_image(image_path, image)
        output = run([sys.executable, kfsck, "--recover", image_path], expect=1)
        if "unsafe/duplicate target" not in output:
            raise RuntimeError("unsafe journal target was not refused")
        checks += 1

    print("PASS: %d KFS journal crash-recovery checks" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
