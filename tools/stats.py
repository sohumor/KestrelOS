#!/usr/bin/env python3
"""Report source-line counts per area of the tree."""
import os
import sys

AREAS = ["boot", "kernel", "libc", "apps", "abi", "tools", "rootfs", "docs"]
EXTS = (".c", ".h", ".asm", ".py", ".ld", ".sh", ".ps1", ".md")


def count(path):
    lines = files = 0
    for root, _, names in os.walk(path):
        if "build" in root.split(os.sep):
            continue
        for name in names:
            if not name.endswith(EXTS):
                continue
            full = os.path.join(root, name)
            try:
                with open(full, "rb") as f:
                    lines += f.read().count(b"\n")
                files += 1
            except OSError:
                pass
    return lines, files


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    total_l = total_f = 0
    print(f"{'area':<10}{'lines':>8}{'files':>8}")
    print("-" * 26)
    for area in AREAS:
        path = os.path.join(root, area)
        if not os.path.isdir(path):
            continue
        lines, files = count(path)
        total_l += lines
        total_f += files
        print(f"{area:<10}{lines:>8}{files:>8}")
    for extra in ("Makefile", "README.md"):
        p = os.path.join(root, extra)
        if os.path.isfile(p):
            with open(p, "rb") as f:
                n = f.read().count(b"\n")
            total_l += n
            total_f += 1
            print(f"{extra:<10}{n:>8}{1:>8}")
    print("-" * 26)
    print(f"{'total':<10}{total_l:>8}{total_f:>8}")


if __name__ == "__main__":
    main()
