#!/usr/bin/env python3
"""Build a KFS filesystem image from a host directory tree.

Usage: mkfs.py <rootdir> <out.img> <size_mb>

On-disk format (see docs/kfs.md): 4096-byte blocks, block 0 superblock,
then block bitmap, inode table (64-byte inodes, 1-based), data blocks.
Output is deterministic: directory entries are emitted in sorted order
and inodes/blocks are assigned in preorder traversal order.
"""
import os
import struct
import sys

BLOCK_SIZE = 4096
MAGIC = 0x3153464B          # "KFS1"
NDIRECT = 12
NINDIRECT = BLOCK_SIZE // 4
MAX_FILE_SIZE = (NDIRECT + NINDIRECT) * BLOCK_SIZE
INODE_SIZE = 64
INODE_COUNT = 1024
INODE_BLOCKS = INODE_COUNT * INODE_SIZE // BLOCK_SIZE   # 16
NAME_MAX = 59
ROOT_INO = 1

TYPE_FILE = 1
TYPE_DIR = 2


class Fs:
    def __init__(self, total_blocks):
        self.total_blocks = total_blocks
        self.bitmap_start = 1
        self.bitmap_blocks = (total_blocks + BLOCK_SIZE * 8 - 1) // (BLOCK_SIZE * 8)
        self.inode_start = self.bitmap_start + self.bitmap_blocks
        self.data_start = self.inode_start + INODE_BLOCKS
        if self.data_start >= total_blocks:
            raise SystemExit("mkfs: image too small for metadata")
        self.image = bytearray(total_blocks * BLOCK_SIZE)
        self.next_block = self.data_start
        self.next_ino = 1
        # ino -> [type, nlink, size, direct[12], indirect]
        self.inodes = {}

    def balloc(self):
        if self.next_block >= self.total_blocks:
            raise SystemExit("mkfs: out of blocks (image too small)")
        b = self.next_block
        self.next_block += 1
        return b

    def ialloc(self, typ):
        if self.next_ino > INODE_COUNT:
            raise SystemExit("mkfs: out of inodes (>%d)" % INODE_COUNT)
        ino = self.next_ino
        self.next_ino += 1
        self.inodes[ino] = [typ, 2 if typ == TYPE_DIR else 1, 0,
                            [0] * NDIRECT, 0]
        return ino

    def write_data(self, ino, data):
        if len(data) > MAX_FILE_SIZE:
            raise SystemExit("mkfs: file for inode %d too big (%d > %d)"
                             % (ino, len(data), MAX_FILE_SIZE))
        node = self.inodes[ino]
        node[2] = len(data)
        nblocks = (len(data) + BLOCK_SIZE - 1) // BLOCK_SIZE
        blocks = []
        for i in range(nblocks):
            b = self.balloc()
            chunk = data[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE]
            self.image[b * BLOCK_SIZE:b * BLOCK_SIZE + len(chunk)] = chunk
            blocks.append(b)
        node[3] = (blocks[:NDIRECT] + [0] * NDIRECT)[:NDIRECT]
        if nblocks > NDIRECT:
            ind = self.balloc()
            node[4] = ind
            tab = blocks[NDIRECT:]
            packed = struct.pack("<%dI" % len(tab), *tab)
            self.image[ind * BLOCK_SIZE:ind * BLOCK_SIZE + len(packed)] = packed

    def finalize(self):
        # inode table
        for ino, (typ, nlink, size, direct, indirect) in self.inodes.items():
            raw = struct.pack("<HHI12III", typ, nlink, size, *direct,
                              indirect, 0)
            off = (self.inode_start * BLOCK_SIZE) + (ino - 1) * INODE_SIZE
            self.image[off:off + INODE_SIZE] = raw
        # bitmap: everything below next_block is used
        used = self.next_block
        boff = self.bitmap_start * BLOCK_SIZE
        full, rem = divmod(used, 8)
        self.image[boff:boff + full] = b"\xff" * full
        if rem:
            self.image[boff + full] = (1 << rem) - 1
        # superblock
        sb = struct.pack("<10I", MAGIC, self.total_blocks, self.bitmap_start,
                         self.bitmap_blocks, self.inode_start, INODE_BLOCKS,
                         INODE_COUNT, self.data_start, ROOT_INO,
                         self.total_blocks - used)
        self.image[0:len(sb)] = sb


def pack_dirent(ino, name):
    nb = name.encode("utf-8")
    if len(nb) > NAME_MAX:
        raise SystemExit("mkfs: name too long: %r" % name)
    return struct.pack("<I", ino) + nb.ljust(60, b"\0")


def add_tree(fs, hostdir, ino, parent_ino):
    """Populate directory inode `ino` from host directory `hostdir`."""
    entries = [pack_dirent(ino, "."), pack_dirent(parent_ino, "..")]
    children = []
    for name in sorted(os.listdir(hostdir)):
        path = os.path.join(hostdir, name)
        if os.path.islink(path):
            print("mkfs: skipping symlink %s" % path, file=sys.stderr)
            continue
        if os.path.isdir(path):
            children.append((name, path, True))
        elif os.path.isfile(path):
            children.append((name, path, False))
        else:
            print("mkfs: skipping special file %s" % path, file=sys.stderr)

    child_inos = []
    for name, path, is_dir in children:
        cino = fs.ialloc(TYPE_DIR if is_dir else TYPE_FILE)
        entries.append(pack_dirent(cino, name))
        child_inos.append((cino, path, is_dir))

    fs.write_data(ino, b"".join(entries))

    for cino, path, is_dir in child_inos:
        if is_dir:
            add_tree(fs, path, cino, ino)
        else:
            with open(path, "rb") as f:
                fs.write_data(cino, f.read())


def main():
    if len(sys.argv) != 4:
        print("usage: mkfs.py <rootdir> <out.img> <size_mb>")
        return 1
    rootdir, out_path, size_mb = sys.argv[1], sys.argv[2], int(sys.argv[3])
    if not os.path.isdir(rootdir):
        print("mkfs: %s is not a directory" % rootdir)
        return 1
    total_blocks = size_mb * 1024 * 1024 // BLOCK_SIZE
    fs = Fs(total_blocks)
    root = fs.ialloc(TYPE_DIR)
    assert root == ROOT_INO
    add_tree(fs, rootdir, root, root)   # root's ".." points to itself
    fs.finalize()
    with open(out_path, "wb") as f:
        f.write(fs.image)
    print("mkfs: %s: %d blocks (%d used, %d free), %d/%d inodes"
          % (out_path, total_blocks, fs.next_block,
             total_blocks - fs.next_block, fs.next_ino - 1, INODE_COUNT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
