#!/usr/bin/env python3
"""Benchmark the real KFS driver on a RAM block device (no VM required).

Run: python3 tools/test_kfs_readdir.py [--baseline] [--source-ref REF]
--baseline reports quadratic scan counts without asserting the linear bound.
"""

import pathlib
import argparse
import subprocess
import tempfile
import os


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--source-ref", help="git revision of kernel/kfs.c to test")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parent.parent
    if args.source_ref:
        source = subprocess.check_output(
            ["git", "show", args.source_ref + ":kernel/kfs.c"], cwd=root, text=True)
    else:
        source = (root / "kernel/kfs.c").read_text()
    needle = "if (readi(fs, ino, off, &de, sizeof(de)) != sizeof(de))"
    assert source.count(needle) == 1
    source = source.replace(needle, "if ((++dirent_reads, readi(fs, ino, off, &de, sizeof(de))) != sizeof(de))")
    with tempfile.TemporaryDirectory(prefix="kfs-readdir-") as tmp:
        path = pathlib.Path(tmp)
        (path / "driver.c").write_text("static unsigned long dirent_reads;\n" + source)
        harness = (root / "tools/test_kfs_readdir.c").read_text()
        (path / "test.c").write_text(harness)
        subprocess.run([args.cc, "-O2", "-g", "-ffunction-sections", "-fdata-sections",
                        "-I" + str(root / "kernel/include"),
                        "-I" + str(root / "abi"),
                        str(path / "test.c"), "-Wl,--gc-sections", "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")] + (["--baseline"] if args.baseline else []),
                       check=True)


if __name__ == "__main__":
    main()
