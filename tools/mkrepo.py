#!/usr/bin/env python3
"""Build a kpkg repository index from a directory of .kpkg packages.

Usage:
  mkrepo.py --repo DIR [--out DIR/index.kpi] [--quiet]

Every package in DIR is opened, its trailer digest verified, and one
line written to the index. The index is what `kpkg update`, `kpkg
search` and `kpkg install <name>` read, over HTTP or straight off the
disk; the format is specified in docs/packages.md:

  # kpkg repository index v1
  <name> <version> <size> <sha256> <filename> <depends|-> <description...>

Fields are separated by single spaces; the description is the rest of
the line and may contain spaces. `depends` is a comma separated list
with no spaces, or "-" when the package has no dependencies. Lines
starting with '#' and blank lines are comments. Output is sorted by
name, so the index is deterministic.
"""
import argparse
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkpkg                                            # noqa: E402

INDEX_VERSION = 1
INDEX_NAME = "index.kpi"


def die(msg):
    raise SystemExit("mkrepo: " + msg)


def sanitize(text):
    """Index fields are line based, so newlines and stray spaces go."""
    return " ".join(text.replace("\t", " ").split())


def build_index(repo, out_path, quiet=False):
    if not os.path.isdir(repo):
        die("%s: not a directory" % repo)
    names = sorted(f for f in os.listdir(repo) if f.endswith(".kpkg"))
    if not names:
        die("%s: no .kpkg files" % repo)

    rows = []
    seen = {}
    for filename in names:
        path = os.path.join(repo, filename)
        pkg = mkpkg.read_package(path)
        want = hashlib.sha256(pkg.raw[:-mkpkg.DIGEST_SIZE]).digest()
        if want != pkg.digest:
            die("%s: digest mismatch, refusing to index" % path)
        name = sanitize(pkg.name)
        if not name:
            die("%s: package has no name" % path)
        if name in seen:
            die("%s and %s both provide %s" % (seen[name], filename, name))
        seen[name] = filename
        rows.append((name, sanitize(pkg.version), len(pkg.raw),
                     pkg.digest.hex(), filename,
                     ",".join(pkg.depends) or "-",
                     sanitize(pkg.meta.get("description", "")) or "-"))

    rows.sort()
    lines = ["# kpkg repository index v%d" % INDEX_VERSION,
             "# name version size sha256 filename depends description"]
    for row in rows:
        lines.append("%s %s %d %s %s %s %s" % row)
    text = "\n".join(lines) + "\n"

    outdir = os.path.dirname(os.path.abspath(out_path))
    if outdir:
        os.makedirs(outdir, exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)

    if not quiet:
        print("mkrepo: %d package(s) -> %s" % (len(rows), out_path))
        for row in rows:
            print("  %-16s %-8s %8d bytes  %s" % (row[0], row[1], row[2],
                                                  row[4]))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description="build a kpkg repository index")
    ap.add_argument("--repo", required=True,
                    help="directory holding the .kpkg files")
    ap.add_argument("--out", help="index path (default REPO/" + INDEX_NAME + ")")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    out = args.out or os.path.join(args.repo, INDEX_NAME)
    return build_index(args.repo, out, args.quiet)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
