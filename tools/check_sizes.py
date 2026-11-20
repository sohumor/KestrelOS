#!/usr/bin/env python3
"""Enforce AGENTS.md budgets on the kernel and staged disk-image files."""

import argparse
from pathlib import Path
import sys


KERNEL_LIMIT = 900 * 1024
ROOTFS_LIMIT = 37 * 1024 * 1024  # Compare tenths of bytes for exact 3.7 MiB.


def check_sizes(build_dir):
    kernel = build_dir / "kernel.bin"
    rootfs = build_dir / "rootfs"
    if not kernel.is_file() or not rootfs.is_dir():
        print("ERROR: expected kernel.bin and rootfs/ in %s; run the build first"
              % build_dir, file=sys.stderr)
        return 1

    files = sorted(path for path in rootfs.rglob("*") if path.is_file())
    if not files:
        print("ERROR: staged rootfs contains no files", file=sys.stderr)
        return 1

    failed = False
    for path in [kernel] + files:
        size = path.stat().st_size
        if path == kernel:
            valid = size < KERNEL_LIMIT
            budget = "900 KiB (921600 bytes)"
        else:
            valid = size * 10 < ROOTFS_LIMIT
            budget = "3.7 MiB (3879731.2 bytes)"
        print("%s: %s: %d bytes < %s" %
              ("PASS" if valid else "FAIL", path, size, budget))
        failed |= not valid
    return int(failed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"),
                        help="build output directory (default: build)")
    args = parser.parse_args()
    try:
        return check_sizes(args.build_dir)
    except OSError as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
