#!/usr/bin/env python3
"""Clean-build with GCC 11 and enforce the checked-in warning ceiling.

Run from any directory: python3 tools/check_warnings.py
After a reduction, lower tools/warning-baseline.txt to the reported count.
The complete build output is saved in build.log.
"""

import argparse
import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parent.parent
BASELINE = "tools/warning-baseline.txt"


def warning_lines(log):
    return sorted({line.strip() for line in log.splitlines() if "warning:" in line})


def check_count(count, baseline):
    print(f"Compiler warnings: {count}; baseline: {baseline}")
    if count > baseline:
        print("FAIL: compiler warning baseline exceeded.")
        return 1
    if count < baseline:
        print(f"Warnings decreased! Lower {BASELINE} to {count}; do not raise it.")
    else:
        print("PASS: compiler warning baseline met.")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-ref", help="reject baseline increases against this git ref")
    args = parser.parse_args()
    baseline = int((ROOT / BASELINE).read_text().strip())
    if baseline < 0:
        parser.error("warning baseline must be nonnegative")
    if args.base_ref:
        entry = subprocess.check_output(
            ["git", "ls-tree", args.base_ref, "--", BASELINE], cwd=ROOT, text=True)
        if entry:
            previous = int(subprocess.check_output(
                ["git", "show", f"{args.base_ref}:{BASELINE}"], cwd=ROOT, text=True))
            if baseline > previous:
                print(f"FAIL: baseline may only decrease ({previous} -> {baseline}).")
                return 1
    env = dict(os.environ, LC_ALL="C", GCC_COLORS="")
    subprocess.run(["make", "clean"], cwd=ROOT, env=env, check=True)
    with (ROOT / "build.log").open("w") as log:
        result = subprocess.run(
            ["make", f"-j{os.cpu_count() or 1}",
             "CC=gcc-11 -march=x86-64 -fdiagnostics-color=never"],
            cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    output = (ROOT / "build.log").read_text(errors="replace")
    if result.returncode:
        print(output)
        print(f"FAIL: build exited with status {result.returncode}; see build.log.")
        return result.returncode
    warnings = warning_lines(output)
    for line in warnings:
        print(line)
    return check_count(len(warnings), baseline)


if __name__ == "__main__":
    raise SystemExit(main())
