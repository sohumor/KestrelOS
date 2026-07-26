#!/usr/bin/env python3
"""Check a KFS image for consistency; optionally list its tree.

Usage: kfsck.py [-l] <image>

Validates the superblock, block bitmap, inode table, directory structure
and reachability. Exits nonzero if any corruption is found. With -l the
directory tree is printed with file sizes.
"""
import struct
import sys

BLOCK_SIZE = 4096
MAGIC = 0x3153464B
NDIRECT = 12
NINDIRECT = BLOCK_SIZE // 4
INODE_SIZE = 64
NAME_MAX = 59

TYPE_FREE = 0
TYPE_FILE = 1
TYPE_DIR = 2

errors = []


def err(msg):
    errors.append(msg)
    print("kfsck: ERROR: %s" % msg, file=sys.stderr)


class Img:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        (self.magic, self.total_blocks, self.bitmap_start, self.bitmap_blocks,
         self.inode_start, self.inode_blocks, self.inode_count,
         self.data_start, self.root_ino, self.free_blocks) = \
            struct.unpack_from("<10I", self.data, 0)

    def block(self, n):
        return self.data[n * BLOCK_SIZE:(n + 1) * BLOCK_SIZE]

    def inode(self, ino):
        off = self.inode_start * BLOCK_SIZE + (ino - 1) * INODE_SIZE
        raw = self.data[off:off + INODE_SIZE]
        f = struct.unpack("<HHI12III", raw)
        return {"type": f[0], "nlink": f[1], "size": f[2],
                "direct": list(f[3:15]), "indirect": f[15]}

    def bit(self, blk):
        b = self.data[self.bitmap_start * BLOCK_SIZE + blk // 8]
        return (b >> (blk % 8)) & 1


def inode_blocks_of(img, ino, node):
    """All data blocks of an inode, in file order, plus the indirect block
    (returned separately)."""
    blocks = [b for b in node["direct"] if b]
    nblocks = (node["size"] + BLOCK_SIZE - 1) // BLOCK_SIZE
    # non-sparse: direct pointers beyond size must be zero
    for i, b in enumerate(node["direct"]):
        if (b == 0) != (i >= nblocks):
            err("ino %d: direct[%d]=%d inconsistent with size %d"
                % (ino, i, b, node["size"]))
    if node["indirect"]:
        if nblocks <= NDIRECT:
            err("ino %d: indirect block set but size %d needs none"
                % (ino, node["size"]))
        tab = struct.unpack("<%dI" % NINDIRECT, img.block(node["indirect"]))
        want = nblocks - NDIRECT
        for i, b in enumerate(tab):
            if (b == 0) != (i >= want):
                err("ino %d: indirect[%d]=%d inconsistent with size %d"
                    % (ino, i, b, node["size"]))
            if b:
                blocks.append(b)
    elif nblocks > NDIRECT:
        err("ino %d: size %d needs an indirect block but has none"
            % (ino, node["size"]))
    return blocks, node["indirect"]


def parse_dir(img, ino, node):
    data = bytearray()
    nblocks = (node["size"] + BLOCK_SIZE - 1) // BLOCK_SIZE
    allb = [b for b in node["direct"] if b]
    if node["indirect"]:
        tab = struct.unpack("<%dI" % NINDIRECT, img.block(node["indirect"]))
        allb += [b for b in tab if b]
    for b in allb[:nblocks]:
        data += img.block(b)
    data = data[:node["size"]]
    if node["size"] % 64:
        err("ino %d: dir size %d not a multiple of 64" % (ino, node["size"]))
    ents = []
    for off in range(0, len(data) - len(data) % 64, 64):
        eino = struct.unpack_from("<I", data, off)[0]
        if eino == 0:
            continue
        raw = bytes(data[off + 4:off + 64])
        if b"\0" not in raw:
            err("ino %d: dirent at %d: name not NUL-terminated" % (ino, off))
            continue
        name = raw.split(b"\0")[0].decode("utf-8", "replace")
        ents.append((name, eino))
    return ents


def main():
    args = sys.argv[1:]
    do_list = "-l" in args
    args = [a for a in args if a != "-l"]
    if len(args) != 1:
        print("usage: kfsck.py [-l] <image>")
        return 2
    img = Img(args[0])

    # --- superblock ---
    if img.magic != MAGIC:
        err("bad magic 0x%08X" % img.magic)
        return 1
    if len(img.data) < img.total_blocks * BLOCK_SIZE:
        err("image shorter than total_blocks says (%d < %d)"
            % (len(img.data), img.total_blocks * BLOCK_SIZE))
        return 1
    want_bb = (img.total_blocks + BLOCK_SIZE * 8 - 1) // (BLOCK_SIZE * 8)
    if img.bitmap_start != 1 or img.bitmap_blocks < want_bb:
        err("bad bitmap geometry (start %d, blocks %d, want >= %d)"
            % (img.bitmap_start, img.bitmap_blocks, want_bb))
    if img.inode_start != img.bitmap_start + img.bitmap_blocks:
        err("inode_start %d != bitmap end %d"
            % (img.inode_start, img.bitmap_start + img.bitmap_blocks))
    if img.data_start != img.inode_start + img.inode_blocks:
        err("data_start %d != inode end %d"
            % (img.data_start, img.inode_start + img.inode_blocks))
    if img.inode_count > img.inode_blocks * (BLOCK_SIZE // INODE_SIZE):
        err("inode_count %d exceeds inode table capacity" % img.inode_count)
    if img.root_ino != 1:
        err("root_ino is %d, expected 1" % img.root_ino)
    if errors:
        return 1

    # --- collect all used blocks from inodes ---
    owner = {}          # block -> ino

    def claim(blk, ino):
        if blk < img.data_start or blk >= img.total_blocks:
            err("ino %d references out-of-range block %d" % (ino, blk))
            return
        if blk in owner:
            err("block %d claimed by both ino %d and ino %d"
                % (blk, owner[blk], ino))
            return
        owner[blk] = ino

    types = {}
    for ino in range(1, img.inode_count + 1):
        node = img.inode(ino)
        types[ino] = node["type"]
        if node["type"] == TYPE_FREE:
            continue
        if node["type"] not in (TYPE_FILE, TYPE_DIR):
            err("ino %d has bad type %d" % (ino, node["type"]))
            continue
        blocks, ind = inode_blocks_of(img, ino, node)
        for b in blocks:
            claim(b, ino)
        if ind:
            claim(ind, ino)

    # --- bitmap vs. reality ---
    for blk in range(img.total_blocks):
        used = img.bit(blk)
        should = blk < img.data_start or blk in owner
        if used and not should:
            err("block %d marked used but not referenced" % blk)
        elif should and not used:
            err("block %d in use but marked free" % blk)
    # padding bits past total_blocks must be zero
    for blk in range(img.total_blocks, img.bitmap_blocks * BLOCK_SIZE * 8):
        if img.bit(blk):
            err("bitmap padding bit %d is set" % blk)
            break
    free = img.total_blocks - (img.data_start + len(owner))
    if img.free_blocks != free:
        err("superblock free_blocks %d != actual %d" % (img.free_blocks, free))

    # --- directory structure and reachability ---
    reached = set()

    def walk(ino, parent, path, depth):
        if ino in reached:
            err("dir ino %d reached twice (%s)" % (ino, path))
            return
        reached.add(ino)
        node = img.inode(ino)
        ents = parse_dir(img, ino, node)
        names = [n for n, _ in ents]
        for want in (".", ".."):
            if names.count(want) != 1:
                err("%s (ino %d): missing or duplicated '%s'" % (path, ino, want))
        seen = set()
        for name, eino in ents:
            if eino < 1 or eino > img.inode_count or types.get(eino) == TYPE_FREE:
                err("%s/%s points to bad ino %d" % (path, name, eino))
                continue
            if name == ".":
                if eino != ino:
                    err("%s: '.' points to %d, not %d" % (path, eino, ino))
                continue
            if name == "..":
                if eino != parent:
                    err("%s: '..' points to %d, not %d" % (path, eino, parent))
                continue
            if name in seen:
                err("%s: duplicate entry '%s'" % (path, name))
            seen.add(name)
            cnode = img.inode(eino)
            if do_list:
                pad = "  " * depth
                if cnode["type"] == TYPE_DIR:
                    print("%s%s/" % (pad, name))
                else:
                    print("%s%s  %d" % (pad, name, cnode["size"]))
            if cnode["type"] == TYPE_DIR:
                walk(eino, ino, path + "/" + name, depth + 1)
            else:
                if eino in reached:
                    err("file ino %d hard-linked twice (unsupported)" % eino)
                reached.add(eino)

    if types.get(1) != TYPE_DIR:
        err("root inode is not a directory")
    else:
        if do_list:
            print("/")
        walk(1, 1, "", 1)

    for ino, t in types.items():
        if t != TYPE_FREE and ino not in reached:
            err("ino %d allocated but unreachable from root" % ino)

    if errors:
        print("kfsck: %d error(s)" % len(errors), file=sys.stderr)
        return 1
    print("kfsck: %s: clean (%d blocks, %d free, %d inodes in use)"
          % (args[0], img.total_blocks, img.free_blocks,
             sum(1 for t in types.values() if t != TYPE_FREE)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
