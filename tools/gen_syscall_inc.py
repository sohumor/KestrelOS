#!/usr/bin/env python3
"""Generate NASM syscall constants from KestrelOS's shared C ABI header."""

import argparse
from pathlib import Path
import re


def render(header):
    lines = ["; Generated from abi/kestrel_abi.h. Do not edit."]
    names, numbers = set(), set()
    for lineno, line in enumerate(header.read_text(encoding="utf-8").splitlines(), 1):
        if not re.match(r"\s*#\s*define\s+SYS_", line):
            continue
        match = re.fullmatch(
            r"\s*#\s*define\s+(SYS_[A-Z0-9_]+)\s+([0-9]+)\s*(?:/\*.*?\*/\s*)?",
            line)
        if not match:
            raise ValueError(f"{header}:{lineno}: expected a decimal syscall number")
        name, value = match.groups()
        number = int(value)
        if name in names or number in numbers:
            raise ValueError(f"{header}:{lineno}: duplicate syscall: {name} = {number}")
        names.add(name)
        numbers.add(number)
        lines.append(f"%define {name} {number}")
    if not names:
        raise ValueError(f"{header}: no syscall definitions")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("header", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        args.output.write_text(render(args.header), encoding="utf-8")
    except (OSError, ValueError) as exc:
        parser.exit(1, f"{exc}\n")


if __name__ == "__main__":
    main()
