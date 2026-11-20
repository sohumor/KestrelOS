#!/usr/bin/env python3
"""Test shared IRQ subscriptions using a private guest image."""

import argparse
import os
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from e2e import Harness, qemu_command, t_boot


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpus", type=int, choices=(2, 4), default=2)
    args = parser.parse_args()
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

        command = qemu_command("rtl8139", args.cpus)
        command[command.index("-drive") + 1] = f"file={image},format=raw"
        h = Harness(command)
        try:
            h.start()
            if h.expect_any([success, "FAIL shared IRQ subscriptions"]) != success:
                raise AssertionError("guest rejected the shared IRQ checks")
            cross_cpu = h.expect(r"(?:PASS|FAIL) cross-CPU IRQ removal[^\n]*\n",
                                 regex=True).group(0).strip()
            if cross_cpu.startswith("FAIL"):
                raise AssertionError(cross_cpu)
            t_boot(h)
            print(success)
            print(cross_cpu)
        except (OSError, RuntimeError, AssertionError) as exc:
            print(f"FAIL shared IRQ subscriptions: {exc}")
            h.print_tail()
            return 1
        finally:
            h.kill()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
