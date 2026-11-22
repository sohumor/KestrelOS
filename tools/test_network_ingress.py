#!/usr/bin/env python3
"""Exercise the bounded receive queue and its worker in a private guest."""

import os
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory

from e2e import Harness, qemu_command, t_boot


def main():
    with TemporaryDirectory() as directory:
        disk = Path(directory) / "os.img"
        try:
            subprocess.run(["make", "-f", "Makefile", "-f", "tools/network_ingress_probe.mk",
                            f"PROBE_DIR={directory}",
                            f"CC={os.environ.get('CC', 'gcc -march=x86-64')}",
                            str(disk)], check=True)
        except subprocess.CalledProcessError as exc:
            print(f"FAIL network ingress probe build: {exc}", file=sys.stderr)
            return 1

        command = qemu_command("rtl8139", 2)
        command[command.index("-drive") + 1] = f"file={disk},format=raw"
        h = Harness(command)
        try:
            h.start()
            for label, detail in (
                ("queue", "copies, FIFO wrap, full drop and batch bound"),
                ("worker", "dispatched an arrival without caller polling"),
            ):
                success = f"PASS network ingress {label}: {detail}"
                if h.expect_any([success, f"FAIL network ingress {label}"]) != success:
                    raise AssertionError(f"guest rejected the {label} check")
                print(success)
            t_boot(h)
        except (OSError, RuntimeError, AssertionError) as exc:
            print(f"FAIL network ingress: {exc}", file=sys.stderr)
            h.print_tail()
            return 1
        finally:
            h.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
