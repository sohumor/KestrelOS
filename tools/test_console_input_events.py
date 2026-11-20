#!/usr/bin/env python3
"""Verify console input wakeups in a private guest image."""

import os
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from e2e import Harness, qemu_command, t_boot


def main():
    success = "PASS console input events: queued input, parked reader wake, and cancellation"
    with TemporaryDirectory() as directory:
        image = Path(directory) / "os.img"
        try:
            subprocess.run(["make", "-f", "Makefile", "-f", "tools/console_input_probe.mk",
                            f"PROBE_DIR={directory}",
                            f"CC={os.environ.get('CC', 'gcc -march=x86-64')}",
                            str(image)], check=True)
        except (OSError, subprocess.CalledProcessError) as exc:
            print(f"FAIL console input probe build: {exc}")
            return 1

        command = qemu_command("rtl8139", 2)
        command[command.index("-drive") + 1] = f"file={image},format=raw"
        h = Harness(command)
        try:
            h.start()
            if h.expect_any([success, "FAIL console input events"]) != success:
                raise AssertionError("guest rejected the console input checks")
            t_boot(h)
            print(success)
        except (OSError, RuntimeError, AssertionError) as exc:
            print(f"FAIL console input events: {exc}")
            h.print_tail()
            return 1
        finally:
            h.kill()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
