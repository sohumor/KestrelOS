#!/usr/bin/env python3
"""Exercise recoverable contiguous allocation before the guest scheduler starts."""

import os
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from e2e import Harness, qemu_command, t_boot


def main():
    success = "PASS contiguous pages: fragmentation leaves text console available and clears on retry"
    with TemporaryDirectory() as directory:
        image = Path(directory) / "os.img"
        try:
            subprocess.run(["make", "-f", "Makefile", "-f", "tools/contiguous_pages_probe.mk",
                            f"PROBE_DIR={directory}",
                            f"CC={os.environ.get('CC', 'gcc -march=x86-64')}",
                            str(image)], check=True)
        except (OSError, subprocess.CalledProcessError) as exc:
            print(f"FAIL contiguous pages probe build: {exc}")
            return 1

        command = qemu_command("rtl8139", 2)
        command[command.index("-drive") + 1] = f"file={image},format=raw"
        h = Harness(command)
        try:
            h.start()
            h.expect("fb: no contiguous memory for a ")
            if h.expect_any([success, "FAIL contiguous pages"]) != success:
                raise AssertionError("guest rejected the allocation or recovery check")
            t_boot(h)
            print(success)
        except (OSError, RuntimeError, AssertionError) as exc:
            print(f"FAIL contiguous pages: {exc}")
            h.print_tail()
            return 1
        finally:
            h.kill()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
