#!/usr/bin/env python3
"""Verify owner-scoped Talon changes in a private guest image."""

import os
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory

from e2e import Harness, qemu_command, t_boot


def main():
    success = "PASS Talon owner stream: UID isolation, live wake, replay gap and timeout"
    with TemporaryDirectory() as directory:
        image = Path(directory) / "os.img"
        try:
            subprocess.run(["make", "-f", "Makefile", "-f", "tools/talon_stream_probe.mk",
                            f"PROBE_DIR={directory}",
                            f"CC={os.environ.get('CC', 'gcc -march=x86-64')}",
                            str(image)], check=True)
        except subprocess.CalledProcessError as exc:
            print(f"FAIL Talon owner stream build: {exc}", file=sys.stderr)
            return 1

        command = qemu_command("rtl8139", 2)
        command[command.index("-drive") + 1] = f"file={image},format=raw"
        h = Harness(command)
        try:
            h.start()
            if h.expect_any([success, "FAIL Talon owner stream"]) != success:
                raise AssertionError("guest rejected the Talon stream checks")
            t_boot(h)
            print(success)
        except (OSError, RuntimeError, AssertionError) as exc:
            print(f"FAIL Talon owner stream: {exc}", file=sys.stderr)
            h.print_tail()
            return 1
        finally:
            h.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
