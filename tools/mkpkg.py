#!/usr/bin/env python3
"""Build, inspect and extract KestrelOS .kpkg packages.

Usage:
  mkpkg.py --manifest MANIFEST --root DIR --out PKG.kpkg
  mkpkg.py --list PKG.kpkg
  mkpkg.py --check PKG.kpkg
  mkpkg.py --extract PKG.kpkg --into DIR

The on-disk format is specified in docs/packages.md; this file and
apps/kpkg.c are the two implementations of it.

Build output is deterministic: entries are emitted in sorted path order,
payload offsets follow from that order alone, and no field is taken from
the host clock or the host file mode.

Manifest format (a text file, '#' starts a comment):

    name: hello                     required
    version: 1.0.0                  required
    description: one line           optional
    arch: x86_64                    optional, default x86_64
    depends: foo, bar               optional, comma separated
    maintainer: someone             optional
    default-file-mode: 0644         optional
    default-dir-mode: 0755          optional

    mode /bin/hello 0755            override one file's permission bits
    own  /bin/hello 0:0             override one file's uid[:gid]
    dir  /var/lib/hello 0755        include a directory that is not in DIR
    exclude *.o                     drop matching files (fnmatch on the
                                    destination path or the base name)

Lines with a colon after the first word are metadata; lines whose first
word is one of mode/own/dir/exclude are rules. Rules are applied after
the defaults, so they always win.

Files under /bin and /sbin default to 0755 (the shell has to exec them);
everything else defaults to default-file-mode.
"""
import argparse
import fnmatch
import hashlib
import os
import struct
import sys

MAGIC = b"KPKG"
FORMAT_VERSION = 1
HEADER_SIZE = 64
# magic(4) version header_size meta_off meta_len ftab_off ftab_count
# data_off data_len total_size flags reserved[5]
HEADER_FMT = "<4s15I"
# path[128] mode uid gid size offset type sha256[32] pad[8]
FILE_FMT = "<128s6I32s8x"
FILE_ENTRY_SIZE = 192
DIGEST_SIZE = 32
PAYLOAD_ALIGN = 16
PATH_MAX = 127                     # 128 including the NUL

TYPE_FILE = 0
TYPE_DIR = 1

DEFAULT_FILE_MODE = 0o644
DEFAULT_DIR_MODE = 0o755
EXEC_MODE = 0o755
EXEC_PREFIXES = ("/bin/", "/sbin/")

assert struct.calcsize(HEADER_FMT) == HEADER_SIZE
assert struct.calcsize(FILE_FMT) == FILE_ENTRY_SIZE

META_KEYS = ("name", "version", "description", "arch", "depends",
             "maintainer")
RULE_WORDS = ("mode", "own", "dir", "exclude")


def die(msg):
    raise SystemExit("mkpkg: " + msg)


def align_up(v, a):
    return (v + a - 1) // a * a


def parse_mode(text, where):
    try:
        return int(text, 8) & 0o777
    except ValueError:
        die("%s: bad octal mode %r" % (where, text))


def norm_dest(path, where):
    """Normalize a destination path to an absolute, slash-free-tail form."""
    path = path.replace("\\", "/")
    if not path.startswith("/"):
        path = "/" + path
    parts = []
    for part in path.split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            die("%s: '..' is not allowed in %r" % (where, path))
        if any(c.isspace() for c in part):
            # kpkg's file database is one whitespace-separated line per
            # path, so a path with a space in it would be unparsable.
            die("%s: whitespace is not allowed in %r" % (where, path))
        parts.append(part)
    out = "/" + "/".join(parts)
    if len(out) > PATH_MAX:
        die("%s: path longer than %d bytes: %s" % (where, PATH_MAX, out))
    return out


class Manifest:
    def __init__(self):
        self.meta = {
            "name": "",
            "version": "",
            "description": "",
            "arch": "x86_64",
            "depends": "",
            "maintainer": "",
        }
        self.file_mode = DEFAULT_FILE_MODE
        self.dir_mode = DEFAULT_DIR_MODE
        self.modes = {}            # dest -> mode
        self.owners = {}           # dest -> (uid, gid)
        self.extra_dirs = {}       # dest -> mode
        self.excludes = []

    @property
    def depends(self):
        return [d.strip() for d in self.meta["depends"].split(",") if d.strip()]


def load_manifest(path):
    m = Manifest()
    with open(path, "r", encoding="utf-8") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            where = "%s:%d" % (path, lineno)
            word = line.split()[0]
            if word in RULE_WORDS and not word.endswith(":"):
                apply_rule(m, word, line[len(word):].strip(), where)
                continue
            if ":" not in line:
                die("%s: not a 'key: value' line and not a rule: %r"
                    % (where, line))
            key, value = line.split(":", 1)
            key = key.strip().lower()
            value = value.strip()
            if key in META_KEYS:
                m.meta[key] = value
            elif key == "default-file-mode":
                m.file_mode = parse_mode(value, where)
            elif key == "default-dir-mode":
                m.dir_mode = parse_mode(value, where)
            else:
                die("%s: unknown key %r" % (where, key))
    if not m.meta["name"]:
        die("%s: missing 'name'" % path)
    if not m.meta["version"]:
        die("%s: missing 'version'" % path)
    for ch in m.meta["name"]:
        if not (ch.isalnum() or ch in "-_.+"):
            die("%s: bad character %r in package name" % (path, ch))
    return m


def apply_rule(m, word, rest, where):
    fields = rest.split()
    if word == "exclude":
        if len(fields) != 1:
            die("%s: 'exclude' takes one pattern" % where)
        m.excludes.append(fields[0])
    elif word == "mode":
        if len(fields) != 2:
            die("%s: 'mode <path> <octal>'" % where)
        m.modes[norm_dest(fields[0], where)] = parse_mode(fields[1], where)
    elif word == "own":
        if len(fields) != 2:
            die("%s: 'own <path> <uid>[:<gid>]'" % where)
        spec = fields[1].split(":")
        try:
            uid = int(spec[0])
            gid = int(spec[1]) if len(spec) > 1 else uid
        except ValueError:
            die("%s: bad owner %r" % (where, fields[1]))
        m.owners[norm_dest(fields[0], where)] = (uid, gid)
    elif word == "dir":
        if len(fields) not in (1, 2):
            die("%s: 'dir <path> [octal]'" % where)
        mode = parse_mode(fields[1], where) if len(fields) == 2 else None
        m.extra_dirs[norm_dest(fields[0], where)] = mode


def excluded(m, dest):
    base = dest.rsplit("/", 1)[-1]
    for pat in m.excludes:
        if fnmatch.fnmatch(dest, pat) or fnmatch.fnmatch(base, pat):
            return True
    return False


def collect(root, m):
    """Walk `root` and return a sorted list of entries.

    Each entry is a dict: dest, type, mode, uid, gid, data (bytes) for
    files. Parent directories of every file are included implicitly so
    that kpkg never has to guess.
    """
    entries = {}

    def add_dir(dest, mode=None):
        if dest == "/" or dest in entries:
            if dest in entries and mode is not None:
                entries[dest]["mode"] = mode
            return
        parent = dest.rsplit("/", 1)[0] or "/"
        add_dir(parent)
        entries[dest] = {"dest": dest, "type": TYPE_DIR,
                         "mode": m.dir_mode if mode is None else mode,
                         "uid": 0, "gid": 0, "data": b""}

    root = os.path.abspath(root)
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        rel = os.path.relpath(dirpath, root)
        if rel != ".":
            dest = norm_dest(rel, root)
            if not excluded(m, dest):
                add_dir(dest)
        for name in sorted(filenames):
            src = os.path.join(dirpath, name)
            relf = os.path.relpath(src, root)
            dest = norm_dest(relf, root)
            if excluded(m, dest):
                continue
            with open(src, "rb") as fh:
                data = fh.read()
            mode = m.file_mode
            if any(dest.startswith(p) for p in EXEC_PREFIXES):
                mode = EXEC_MODE
            parent = dest.rsplit("/", 1)[0] or "/"
            add_dir(parent)
            entries[dest] = {"dest": dest, "type": TYPE_FILE, "mode": mode,
                             "uid": 0, "gid": 0, "data": data}

    for dest, mode in sorted(m.extra_dirs.items()):
        add_dir(dest, mode)

    for dest, mode in m.modes.items():
        if dest not in entries:
            die("mode rule for %s, which is not in the package" % dest)
        entries[dest]["mode"] = mode
    for dest, (uid, gid) in m.owners.items():
        if dest not in entries:
            die("own rule for %s, which is not in the package" % dest)
        entries[dest]["uid"] = uid
        entries[dest]["gid"] = gid

    return [entries[k] for k in sorted(entries)]


def build_meta(m, entries):
    install_size = sum(len(e["data"]) for e in entries)
    lines = [
        "format: %d" % FORMAT_VERSION,
        "name: %s" % m.meta["name"],
        "version: %s" % m.meta["version"],
        "arch: %s" % (m.meta["arch"] or "x86_64"),
        "description: %s" % m.meta["description"],
        "depends: %s" % ",".join(m.depends),
        "maintainer: %s" % m.meta["maintainer"],
        "files: %d" % len(entries),
        "size: %d" % install_size,
    ]
    return ("\n".join(lines) + "\n").encode("utf-8")


def build(manifest_path, root, out_path):
    m = load_manifest(manifest_path)
    entries = collect(root, m)
    if not entries:
        die("%s: package would be empty" % manifest_path)
    meta = build_meta(m, entries)

    meta_off = HEADER_SIZE
    ftab_off = align_up(meta_off + len(meta), PAYLOAD_ALIGN)
    data_off = align_up(ftab_off + len(entries) * FILE_ENTRY_SIZE,
                        PAYLOAD_ALIGN)

    payload = bytearray()
    table = bytearray()
    for e in entries:
        data = e["data"]
        if e["type"] == TYPE_DIR:
            offset, size, digest = 0, 0, b"\0" * DIGEST_SIZE
        else:
            offset = len(payload)
            size = len(data)
            digest = hashlib.sha256(data).digest()
            payload += data
            payload += b"\0" * (align_up(size, PAYLOAD_ALIGN) - size)
        path = e["dest"].encode("utf-8")
        if len(path) > PATH_MAX:
            die("path too long: %s" % e["dest"])
        table += struct.pack(FILE_FMT, path, e["mode"], e["uid"], e["gid"],
                             size, offset, e["type"], digest)

    total = data_off + len(payload) + DIGEST_SIZE
    header = struct.pack(HEADER_FMT, MAGIC, FORMAT_VERSION, HEADER_SIZE,
                         meta_off, len(meta), ftab_off, len(entries),
                         data_off, len(payload), total, 0,
                         0, 0, 0, 0, 0)

    image = bytearray(total - DIGEST_SIZE)
    image[0:HEADER_SIZE] = header
    image[meta_off:meta_off + len(meta)] = meta
    image[ftab_off:ftab_off + len(table)] = table
    image[data_off:data_off + len(payload)] = payload
    digest = hashlib.sha256(bytes(image)).digest()

    outdir = os.path.dirname(os.path.abspath(out_path))
    if outdir:
        os.makedirs(outdir, exist_ok=True)
    with open(out_path, "wb") as fh:
        fh.write(image)
        fh.write(digest)

    print("mkpkg: %s %s -> %s (%d files, %d bytes, sha256 %s)"
          % (m.meta["name"], m.meta["version"], out_path, len(entries),
             total, digest.hex()))
    return 0


# ---------------------------------------------------------------- reading

class Package:
    def __init__(self, path, raw, meta, entries, digest):
        self.path = path
        self.raw = raw
        self.meta = meta
        self.entries = entries
        self.digest = digest

    @property
    def name(self):
        return self.meta.get("name", "")

    @property
    def version(self):
        return self.meta.get("version", "")

    @property
    def depends(self):
        return [d.strip() for d in self.meta.get("depends", "").split(",")
                if d.strip()]

    def data(self, entry):
        start = entry["data_off"] + entry["offset"]
        return self.raw[start:start + entry["size"]]


def read_package(path):
    with open(path, "rb") as fh:
        raw = fh.read()
    if len(raw) < HEADER_SIZE + DIGEST_SIZE:
        die("%s: too small to be a package" % path)
    (magic, version, header_size, meta_off, meta_len, ftab_off, ftab_count,
     data_off, data_len, total, _flags, _r0, _r1, _r2, _r3,
     _r4) = struct.unpack(HEADER_FMT, raw[:HEADER_SIZE])
    if magic != MAGIC:
        die("%s: bad magic %r" % (path, magic))
    if version != FORMAT_VERSION:
        die("%s: format version %d, expected %d"
            % (path, version, FORMAT_VERSION))
    if header_size != HEADER_SIZE:
        die("%s: header_size %d, expected %d" % (path, header_size,
                                                 HEADER_SIZE))
    if total != len(raw):
        die("%s: total_size %d but the file is %d bytes"
            % (path, total, len(raw)))
    body_end = total - DIGEST_SIZE
    for label, off, length in (("metadata", meta_off, meta_len),
                               ("file table", ftab_off,
                                ftab_count * FILE_ENTRY_SIZE),
                               ("payload", data_off, data_len)):
        if off < HEADER_SIZE or off + length > body_end:
            die("%s: %s block out of range" % (path, label))

    meta = {}
    for line in raw[meta_off:meta_off + meta_len].decode("utf-8",
                                                         "replace").split("\n"):
        line = line.strip()
        if not line or ":" not in line:
            continue
        key, value = line.split(":", 1)
        meta[key.strip().lower()] = value.strip()

    entries = []
    seen = set()
    for i in range(ftab_count):
        base = ftab_off + i * FILE_ENTRY_SIZE
        (rawpath, mode, uid, gid, size, offset, etype,
         digest) = struct.unpack(FILE_FMT, raw[base:base + FILE_ENTRY_SIZE])
        dest = rawpath.split(b"\0", 1)[0].decode("utf-8", "replace")
        if not dest.startswith("/") or ".." in dest.split("/"):
            die("%s: unsafe path in file table: %r" % (path, dest))
        if dest in seen:
            die("%s: duplicate path in file table: %s" % (path, dest))
        seen.add(dest)
        if etype == TYPE_FILE and offset + size > data_len:
            die("%s: %s runs past the payload" % (path, dest))
        entries.append({"dest": dest, "mode": mode, "uid": uid, "gid": gid,
                        "size": size, "offset": offset, "type": etype,
                        "sha256": digest, "data_off": data_off})

    return Package(path, raw, meta, entries, raw[body_end:])


def check(path):
    pkg = read_package(path)
    want = hashlib.sha256(pkg.raw[:-DIGEST_SIZE]).digest()
    ok = True
    if want != pkg.digest:
        print("FAIL %s: trailer digest %s, computed %s"
              % (path, pkg.digest.hex(), want.hex()))
        ok = False
    else:
        print("ok   %s: sha256 %s" % (path, pkg.digest.hex()))
    for e in pkg.entries:
        if e["type"] != TYPE_FILE:
            continue
        got = hashlib.sha256(pkg.data(e)).digest()
        if got != e["sha256"]:
            print("FAIL %s: %s content digest mismatch" % (path, e["dest"]))
            ok = False
    n_files = sum(1 for e in pkg.entries if e["type"] == TYPE_FILE)
    n_dirs = len(pkg.entries) - n_files
    print("     %s %s: %d files, %d dirs, install size %s bytes, depends %s"
          % (pkg.name, pkg.version, n_files, n_dirs,
             pkg.meta.get("size", "?"), ",".join(pkg.depends) or "-"))
    return 0 if ok else 1


def listing(path):
    pkg = read_package(path)
    for key in ("name", "version", "arch", "description", "depends",
                "maintainer", "files", "size"):
        if key in pkg.meta:
            print("%-12s %s" % (key + ":", pkg.meta[key]))
    print()
    print("%-6s %-5s %-5s %10s  %-16s %s"
          % ("mode", "uid", "gid", "size", "sha256", "path"))
    for e in pkg.entries:
        kind = "d" if e["type"] == TYPE_DIR else "-"
        short = "-" if e["type"] == TYPE_DIR else e["sha256"].hex()[:16]
        print("%s%04o %-5d %-5d %10d  %-16s %s"
              % (kind, e["mode"], e["uid"], e["gid"], e["size"], short,
                 e["dest"]))
    return 0


def extract(path, into):
    pkg = read_package(path)
    want = hashlib.sha256(pkg.raw[:-DIGEST_SIZE]).digest()
    if want != pkg.digest:
        die("%s: digest mismatch, refusing to extract" % path)
    into = os.path.abspath(into)
    for e in pkg.entries:
        target = os.path.join(into, e["dest"].lstrip("/"))
        if e["type"] == TYPE_DIR:
            os.makedirs(target, exist_ok=True)
        else:
            os.makedirs(os.path.dirname(target), exist_ok=True)
            data = pkg.data(e)
            if hashlib.sha256(data).digest() != e["sha256"]:
                die("%s: %s content digest mismatch" % (path, e["dest"]))
            with open(target, "wb") as fh:
                fh.write(data)
        print("%s %04o %s" % ("d" if e["type"] == TYPE_DIR else "f",
                              e["mode"], e["dest"]))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description="build/inspect .kpkg packages")
    ap.add_argument("--manifest")
    ap.add_argument("--root")
    ap.add_argument("--out")
    ap.add_argument("--list", dest="list_pkg")
    ap.add_argument("--check")
    ap.add_argument("--extract")
    ap.add_argument("--into")
    args = ap.parse_args(argv)

    if args.list_pkg:
        return listing(args.list_pkg)
    if args.check:
        return check(args.check)
    if args.extract:
        if not args.into:
            die("--extract needs --into DIR")
        return extract(args.extract, args.into)
    if args.manifest or args.root or args.out:
        if not (args.manifest and args.root and args.out):
            die("building needs --manifest, --root and --out")
        return build(args.manifest, args.root, args.out)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
