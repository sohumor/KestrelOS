#!/usr/bin/env python3
"""Test shared IRQ subscriptions using a private guest image."""

import os
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from e2e import Harness, qemu_command, t_boot


def main():
    success = "PASS shared IRQ subscriptions: hardware PIT delivery, legacy coexistence, and removal"
    with TemporaryDirectory() as directory:
        image = Path(directory) / "os.img"
        try:
            subprocess.run(["make", "-f", "Makefile", "-f", "tools/shared_irq_probe.mk",
                            f"PROBE_DIR={directory}",
                            f"CC={os.environ.get('CC', 'gcc -march=x86-64')}",
                            str(image)], check=True)
        except (OSError, subprocess.CalledProcessError) as exc:
            print(f"FAIL shared IRQ probe build: {exc}")
            return 1

        command = qemu_command("rtl8139", 2)
        command[command.index("-drive") + 1] = f"file={image},format=raw"
        h = Harness(command)
        try:
            h.start()
            if h.expect_any([success, "FAIL shared IRQ subscriptions"]) != success:
                raise AssertionError("guest rejected the shared IRQ checks")
            cross_cpu = "PASS cross-CPU IRQ removal:"
            if h.expect_any([cross_cpu, "FAIL cross-CPU IRQ removal"]) != cross_cpu:
                raise AssertionError("guest rejected the cross-CPU IRQ removal checks")
            t_boot(h)
            print(success)
            print(cross_cpu + " removal waited for a live PIT callback on another CPU")
        except (OSError, RuntimeError, AssertionError) as exc:
            print(f"FAIL shared IRQ subscriptions: {exc}")
            h.print_tail()
            return 1
        finally:
            h.kill()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
