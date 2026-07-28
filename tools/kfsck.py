#!/usr/bin/env python3
"""Check a KFS v3 image for consistency; optionally recover/list its tree.

Usage: kfsck.py [--recover] [-l] <image>

Validates the journal, superblock, block bitmap, inode table (including the v2
mode/uid/gid/mtime fields), directory structure and reachability. Exits
nonzero if any corruption is found. With -l the directory tree is printed
with permissions, owner, size and timestamp. --recover replays a valid
committed redo transaction (or clears an incomplete one) before checking.
"""
import struct
import sys
import time
import zlib

BLOCK_SIZE = 4096
MAGIC = 0x3353464B          # "KFS3"
MAGIC_V2 = 0x3253464B       # "KFS2"
MAGIC_V1 = 0x3153464B       # "KFS1"
FEATURE_JOURNAL = 1
JOURNAL_MAGIC = 0x314C4E4A  # "JNL1"
JOURNAL_COMMITTED = 1
JOURNAL_ENTRIES = 32
JOURNAL_BLOCKS = 1 + JOURNAL_ENTRIES
NDIRECT = 10
NINDIRECT = BLOCK_SIZE // 4
INODE_SIZE = 64
NAME_MAX = 59

# type(2) mode(2) size(4) uid(4) gid(4) mtime(4) direct(10*4) indirect(4)
INODE_FMT = "<HHIIII%dII" % NDIRECT
assert struct.calcsize(INODE_FMT) == INODE_SIZE

TYPE_FREE = 0
TYPE_FILE = 1
TYPE_DIR = 2

MODE_MASK = 0o777
UID_MAX = 0xFFFF            # KestrelOS uses small numeric ids only
# Anything past this is a clock that ran away; the FS was written by a
# machine whose RTC was wrong or the field is garbage.
MTIME_MAX = 4102444800      # 2100-01-01T00:00:00Z

errors = []
warnings = []


def err(msg):
    errors.append(msg)
    print("kfsck: ERROR: %s" % msg, file=sys.stderr)


def warn(msg):
    warnings.append(msg)
    print("kfsck: warning: %s" % msg, file=sys.stderr)


class Img:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = bytearray(f.read())
        (self.magic, self.total_blocks, self.bitmap_start, self.bitmap_blocks,
         self.journal_start, self.journal_blocks, self.inode_start,
         self.inode_blocks, self.inode_count, self.data_start, self.root_ino,
         self.free_blocks, self.features) = \
            struct.unpack_from("<13I", self.data, 0)

    def block(self, n):
        return self.data[n * BLOCK_SIZE:(n + 1) * BLOCK_SIZE]

    def inode(self, ino):
        off = self.inode_start * BLOCK_SIZE + (ino - 1) * INODE_SIZE
        raw = self.data[off:off + INODE_SIZE]
        f = struct.unpack(INODE_FMT, raw)
        return {"type": f[0], "mode": f[1], "size": f[2], "uid": f[3],
                "gid": f[4], "mtime": f[5],
                "direct": list(f[6:6 + NDIRECT]), "indirect": f[6 + NDIRECT]}

    def bit(self, blk):
        b = self.data[self.bitmap_start * BLOCK_SIZE + blk // 8]
        return (b >> (blk % 8)) & 1

    def write_block(self, n, data):
        assert len(data) == BLOCK_SIZE
        self.data[n * BLOCK_SIZE:(n + 1) * BLOCK_SIZE] = data

    def save(self):
        with open(self.path, "wb") as f:
            f.write(self.data)


def journal_info(img):
    raw = img.block(img.journal_start)
    magic, state, sequence, count, checksum = struct.unpack_from("<5I", raw, 0)
    if magic == 0 and state == 0 and count == 0:
        return {"status": "clean"}
    if magic != JOURNAL_MAGIC or state != JOURNAL_COMMITTED or \
            count == 0 or count > JOURNAL_ENTRIES:
        return {"status": "incomplete", "sequence": sequence}

    targets = list(struct.unpack_from("<%dI" % JOURNAL_ENTRIES, raw, 20))[:count]
    for i, target in enumerate(targets):
        if target >= img.total_blocks or \
                img.journal_start <= target < img.journal_start + img.journal_blocks:
            return {"status": "unsafe", "target": target}
        if target in targets[:i]:
            return {"status": "unsafe", "target": target}

    payloads = [img.block(img.journal_start + 1 + i) for i in range(count)]
    crc = zlib.crc32(struct.pack("<II", sequence, count))
    crc = zlib.crc32(struct.pack("<%dI" % count, *targets), crc)
    for payload in payloads:
        crc = zlib.crc32(payload, crc)
    crc &= 0xFFFFFFFF
    if crc != checksum:
        return {"status": "torn", "sequence": sequence}
    return {"status": "committed", "sequence": sequence, "count": count,
            "targets": targets, "payloads": payloads}


def recover_journal(img):
    info = journal_info(img)
    status = info["status"]
    if status == "clean":
        return 0
    if status == "unsafe":
        err("journal contains unsafe/duplicate target %d" % info["target"])
        return -1
    if status == "committed":
        for target, payload in zip(info["targets"], info["payloads"]):
            img.write_block(target, payload)
        print("kfsck: replayed journal transaction %d (%d blocks)"
              % (info["sequence"], info["count"]))
    else:
        print("kfsck: cleared %s journal transaction" % status)
    img.write_block(img.journal_start, bytes(BLOCK_SIZE))
    img.save()
    return 1


def check_meta(ino, node):
    """v2 ownership/permission/timestamp sanity."""
    if node["mode"] & ~MODE_MASK:
        err("ino %d: mode 0%o has bits outside 0777" % (ino, node["mode"]))
    if node["uid"] > UID_MAX:
        err("ino %d: uid %d out of range" % (ino, node["uid"]))
    if node["gid"] > UID_MAX:
        err("ino %d: gid %d out of range" % (ino, node["gid"]))
    if node["mtime"] > MTIME_MAX:
        err("ino %d: mtime %d is not a plausible Unix time"
            % (ino, node["mtime"]))
    if node["type"] == TYPE_DIR and not (node["mode"] & 0o100):
        # A directory the owner cannot search is unusable, not corrupt.
        warn("ino %d: directory mode 0%o has no owner search bit"
             % (ino, node["mode"]))
    if node["type"] == TYPE_FILE and not (node["mode"] & 0o400):
        warn("ino %d: file mode 0%o is unreadable by its owner"
             % (ino, node["mode"]))


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


def mode_str(node):
    m = node["mode"]
    out = "d" if node["type"] == TYPE_DIR else "-"
    for shift in (6, 3, 0):
        bits = (m >> shift) & 7
        out += "r" if bits & 4 else "-"
        out += "w" if bits & 2 else "-"
        out += "x" if bits & 1 else "-"
    return out


def time_str(secs):
    if secs == 0:
        return "-"
    return time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(secs))


def describe(node):
    return "%s %d:%d %8d %s" % (mode_str(node), node["uid"], node["gid"],
                                node["size"], time_str(node["mtime"]))


def main():
    args = sys.argv[1:]
    do_list = "-l" in args
    do_recover = "--recover" in args
    args = [a for a in args if a not in ("-l", "--recover")]
    if len(args) != 1:
        print("usage: kfsck.py [--recover] [-l] <image>")
        return 2
    img = Img(args[0])

    # --- superblock ---
    if img.magic in (MAGIC_V1, MAGIC_V2):
        version = 1 if img.magic == MAGIC_V1 else 2
        err("image is KFS v%d; this checker (and the kernel) need v3" % version)
        return 1
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
    if img.journal_start != img.bitmap_start + img.bitmap_blocks:
        err("journal_start %d != bitmap end %d"
            % (img.journal_start, img.bitmap_start + img.bitmap_blocks))
    if img.journal_blocks != JOURNAL_BLOCKS:
        err("journal_blocks %d != required %d"
            % (img.journal_blocks, JOURNAL_BLOCKS))
    if img.inode_start != img.journal_start + img.journal_blocks:
        err("inode_start %d != journal end %d"
            % (img.inode_start, img.journal_start + img.journal_blocks))
    if img.data_start != img.inode_start + img.inode_blocks:
        err("data_start %d != inode end %d"
            % (img.data_start, img.inode_start + img.inode_blocks))
    if img.inode_count > img.inode_blocks * (BLOCK_SIZE // INODE_SIZE):
        err("inode_count %d exceeds inode table capacity" % img.inode_count)
    if img.root_ino != 1:
        err("root_ino is %d, expected 1" % img.root_ino)
    if img.features != FEATURE_JOURNAL:
        err("unsupported feature mask 0x%08X" % img.features)
    if errors:
        return 1

    info = journal_info(img)
    if info["status"] != "clean":
        if not do_recover:
            if info["status"] == "committed":
                err("committed journal transaction %d needs recovery"
                    % info["sequence"])
            elif info["status"] == "unsafe":
                err("journal contains unsafe/duplicate target %d"
                    % info["target"])
            else:
                err("%s journal transaction needs cleanup" % info["status"])
            return 1
        if recover_journal(img) < 0:
            return 1
        img = Img(args[0])

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
            # A free slot must be entirely zero, or a stale mode/uid would
            # be handed to the next file created in it.
            if any(node[k] for k in ("mode", "size", "uid", "gid", "mtime",
                                     "indirect")) or any(node["direct"]):
                err("ino %d is free but not zeroed" % ino)
            continue
        if node["type"] not in (TYPE_FILE, TYPE_DIR):
            err("ino %d has bad type %d" % (ino, node["type"]))
            continue
        check_meta(ino, node)
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
            if len(name.encode("utf-8")) > NAME_MAX:
                err("%s: entry name %r longer than %d bytes"
                    % (path, name, NAME_MAX))
            if name in seen:
                err("%s: duplicate entry '%s'" % (path, name))
            seen.add(name)
            cnode = img.inode(eino)
            if do_list:
                pad = "  " * depth
                slash = "/" if cnode["type"] == TYPE_DIR else ""
                print("%s  %s%s%s" % (describe(cnode), pad, name, slash))
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
            print("%s  /" % describe(img.inode(1)))
        walk(1, 1, "", 1)

    for ino, t in types.items():
        if t != TYPE_FREE and ino not in reached:
            err("ino %d allocated but unreachable from root" % ino)

    if errors:
        print("kfsck: %d error(s)" % len(errors), file=sys.stderr)
        return 1
    print("kfsck: %s: clean (KFS v3 journaled, %d blocks, %d free, "
          "%d inodes in use%s)"
          % (args[0], img.total_blocks, img.free_blocks,
             sum(1 for t in types.values() if t != TYPE_FREE),
             ", %d warning(s)" % len(warnings) if warnings else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
