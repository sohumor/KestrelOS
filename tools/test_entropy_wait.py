#!/usr/bin/env python3
"""Boot a private guest image with the entropy readiness regression probe."""

import os
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory

from e2e import Harness, qemu_command, t_boot


def main():
    with TemporaryDirectory() as directory:
        image = Path(directory) / "os.img"
        try:
            subprocess.run(["make", "-f", "Makefile", "-f", "tools/entropy_wait_probe.mk",
                            f"PROBE_DIR={directory}",
                            f"CC={os.environ.get('CC', 'gcc -march=x86-64')}",
                            str(image)], check=True)
        except subprocess.CalledProcessError as exc:
            print(f"FAIL entropy probe build: {exc}", file=sys.stderr)
            return 1

        command = qemu_command("rtl8139", 2)
        command[command.index("-drive") + 1] = f"file={image},format=raw"
        h = Harness(command)
        try:
            h.start()
            result = h.expect_any([r"PASS entropy readiness:[^\n]+",
                                  r"FAIL entropy readiness:[^\n]+"], regex=True)
            if result.startswith("FAIL"):
                raise AssertionError("guest rejected the entropy readiness transition")
            t_boot(h)
            print("PASS entropy readiness: reader stayed blocked below threshold and woke at readiness")
        except (OSError, RuntimeError, AssertionError) as exc:
            print(f"FAIL entropy wait: {exc}", file=sys.stderr)
            h.print_tail()
            return 1
        finally:
            h.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
