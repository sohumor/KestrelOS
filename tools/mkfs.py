#!/usr/bin/env python3
"""Build a KFS v2 filesystem image from a host directory tree.

Usage: mkfs.py [options] <rootdir> <out.img> <size_mb>

Options:
  --mtime SECS          timestamp stamped on every inode (default 0 =
                        "unknown"). Pass a fixed value (e.g. the git commit
                        date) to keep builds reproducible.
  --mode PATH:MODE      override the permission bits of one image path,
                        e.g. --mode /etc/shadow:0600. Repeatable. MODE is
                        octal, with or without a leading 0.
  --own PATH:UID[:GID]  override the owner of one image path,
                        e.g. --own /home/ana:1000:1000. Repeatable.
                        GID defaults to UID.

On-disk format (see docs/kfs.md): 4096-byte blocks, block 0 superblock,
then block bitmap, inode table (64-byte inodes, 1-based), data blocks.

Default ownership and permissions:
  * everything is owned by root:root (uid 0, gid 0);
  * directories are 0755;
  * regular files are 0644;
  * anything under the top-level bin/ directory (at any depth) is 0755,
    because those are the programs the shell has to exec.
--mode / --own are applied last and win over all of the above.

Output is deterministic: directory entries are emitted in sorted order,
inodes/blocks are assigned in preorder traversal order, and no field is
taken from the host clock or the host file mode.
"""
import os
import struct
import sys

BLOCK_SIZE = 4096
MAGIC = 0x3253464B          # "KFS2"
NDIRECT = 10
NINDIRECT = BLOCK_SIZE // 4
MAX_FILE_SIZE = (NDIRECT + NINDIRECT) * BLOCK_SIZE
INODE_SIZE = 64
INODE_COUNT = 1024
INODE_BLOCKS = INODE_COUNT * INODE_SIZE // BLOCK_SIZE   # 16
NAME_MAX = 59
ROOT_INO = 1

# type(2) mode(2) size(4) uid(4) gid(4) mtime(4) direct(10*4) indirect(4)
INODE_FMT = "<HHIIII%dII" % NDIRECT
assert struct.calcsize(INODE_FMT) == INODE_SIZE

TYPE_FILE = 1
TYPE_DIR = 2

MODE_MASK = 0o777
DEFAULT_FILE_MODE = 0o644
DEFAULT_DIR_MODE = 0o755
EXEC_MODE = 0o755
DEFAULT_UID = 0
DEFAULT_GID = 0

# Image paths whose contents are executables and therefore need the x bit.
EXEC_ROOTS = ("bin",)


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
        # ino -> [type, mode, size, uid, gid, mtime, direct[NDIRECT], indirect]
        self.inodes = {}

    def balloc(self):
        if self.next_block >= self.total_blocks:
            raise SystemExit("mkfs: out of blocks (image too small)")
        b = self.next_block
        self.next_block += 1
        return b

    def ialloc(self, typ, mode, uid, gid, mtime):
        if self.next_ino > INODE_COUNT:
            raise SystemExit("mkfs: out of inodes (>%d)" % INODE_COUNT)
        ino = self.next_ino
        self.next_ino += 1
        self.inodes[ino] = [typ, mode & MODE_MASK, 0, uid, gid, mtime,
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
        node[6] = (blocks[:NDIRECT] + [0] * NDIRECT)[:NDIRECT]
        if nblocks > NDIRECT:
            ind = self.balloc()
            node[7] = ind
            tab = blocks[NDIRECT:]
            packed = struct.pack("<%dI" % len(tab), *tab)
            self.image[ind * BLOCK_SIZE:ind * BLOCK_SIZE + len(packed)] = packed

    def finalize(self):
        # inode table
        for ino, (typ, mode, size, uid, gid, mtime, direct, indirect) \
                in self.inodes.items():
            raw = struct.pack(INODE_FMT, typ, mode, size, uid, gid, mtime,
                              *direct, indirect)
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


def norm(path):
    """Normalize an image path to a leading-slash, no-trailing-slash form."""
    parts = [p for p in path.replace("\\", "/").split("/") if p and p != "."]
    return "/" + "/".join(parts)


def default_mode(img_path, is_dir):
    parts = [p for p in img_path.split("/") if p]
    if parts and parts[0] in EXEC_ROOTS:
        return EXEC_MODE
    return DEFAULT_DIR_MODE if is_dir else DEFAULT_FILE_MODE


class Policy:
    """Per-path mode / owner overrides from the command line."""

    def __init__(self):
        self.modes = {}
        self.owners = {}
        self.used = set()

    def add_mode(self, spec):
        if ":" not in spec:
            raise SystemExit("mkfs: --mode wants PATH:MODE, got %r" % spec)
        path, mode = spec.rsplit(":", 1)
        try:
            value = int(mode, 8)
        except ValueError:
            raise SystemExit("mkfs: --mode %r: %r is not octal" % (spec, mode))
        if value & ~MODE_MASK:
            raise SystemExit("mkfs: --mode %r: only 0..0777 is allowed" % spec)
        self.modes[norm(path)] = value

    def add_owner(self, spec):
        bits = spec.split(":")
        if len(bits) not in (2, 3):
            raise SystemExit("mkfs: --own wants PATH:UID[:GID], got %r" % spec)
        path = bits[0]
        try:
            uid = int(bits[1])
            gid = int(bits[2]) if len(bits) == 3 else uid
        except ValueError:
            raise SystemExit("mkfs: --own %r: uid/gid must be numeric" % spec)
        if not (0 <= uid <= 0xFFFF and 0 <= gid <= 0xFFFF):
            raise SystemExit("mkfs: --own %r: uid/gid out of range" % spec)
        self.owners[norm(path)] = (uid, gid)

    def apply(self, img_path, is_dir):
        mode = default_mode(img_path, is_dir)
        uid, gid = DEFAULT_UID, DEFAULT_GID
        if img_path in self.modes:
            mode = self.modes[img_path]
            self.used.add(img_path)
        if img_path in self.owners:
            uid, gid = self.owners[img_path]
            self.used.add(img_path)
        return mode, uid, gid

    def unused(self):
        """Overrides that never matched a path: almost always a typo."""
        return sorted((set(self.modes) | set(self.owners)) - self.used)


def add_tree(fs, hostdir, ino, parent_ino, img_path, policy, mtime):
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
        cpath = norm(img_path + "/" + name)
        mode, uid, gid = policy.apply(cpath, is_dir)
        cino = fs.ialloc(TYPE_DIR if is_dir else TYPE_FILE, mode, uid, gid,
                         mtime)
        entries.append(pack_dirent(cino, name))
        child_inos.append((cino, path, cpath, is_dir))

    fs.write_data(ino, b"".join(entries))

    for cino, path, cpath, is_dir in child_inos:
        if is_dir:
            add_tree(fs, path, cino, ino, cpath, policy, mtime)
        else:
            with open(path, "rb") as f:
                fs.write_data(cino, f.read())


def main():
    args = sys.argv[1:]
    policy = Policy()
    mtime = 0
    positional = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--mode" or a == "--own" or a == "--mtime":
            if i + 1 >= len(args):
                print("mkfs: %s needs an argument" % a, file=sys.stderr)
                return 1
            value = args[i + 1]
            i += 2
        elif a.startswith("--mode=") or a.startswith("--own=") or \
                a.startswith("--mtime="):
            a, value = a.split("=", 1)
            i += 1
        elif a.startswith("-"):
            print("mkfs: unknown option %s" % a, file=sys.stderr)
            return 1
        else:
            positional.append(a)
            i += 1
            continue
        if a == "--mode":
            policy.add_mode(value)
        elif a == "--own":
            policy.add_owner(value)
        else:
            mtime = int(value, 0)
            if not 0 <= mtime <= 0xFFFFFFFF:
                print("mkfs: --mtime out of range", file=sys.stderr)
                return 1

    if len(positional) != 3:
        print("usage: mkfs.py [--mtime SECS] [--mode PATH:MODE] "
              "[--own PATH:UID[:GID]] <rootdir> <out.img> <size_mb>")
        return 1
    rootdir, out_path, size_mb = positional[0], positional[1], int(positional[2])
    if not os.path.isdir(rootdir):
        print("mkfs: %s is not a directory" % rootdir)
        return 1
    total_blocks = size_mb * 1024 * 1024 // BLOCK_SIZE
    fs = Fs(total_blocks)
    rmode, ruid, rgid = policy.apply("/", True)
    root = fs.ialloc(TYPE_DIR, rmode, ruid, rgid, mtime)
    assert root == ROOT_INO
    # root's ".." points to itself
    add_tree(fs, rootdir, root, root, "", policy, mtime)
    fs.finalize()
    with open(out_path, "wb") as f:
        f.write(fs.image)
    for path in policy.unused():
        print("mkfs: warning: override for %s matched nothing" % path,
              file=sys.stderr)
    print("mkfs: %s: KFS v2, %d blocks (%d used, %d free), %d/%d inodes"
          % (out_path, total_blocks, fs.next_block,
             total_blocks - fs.next_block, fs.next_ino - 1, INODE_COUNT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
