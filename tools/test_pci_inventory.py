#!/usr/bin/env python3
"""Compare boot PCI discovery with the inventory reported by lsdev."""

import argparse
import re
import sys

from e2e import Harness, qemu_command, t_boot, t_login, t_prompt, wait_prompt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disk", choices=("ata", "ahci"), default="ata")
    args = parser.parse_args()

    h = Harness(qemu_command("rtl8139", 2, args.disk))
    h.disk = args.disk
    try:
        h.start()
        t_boot(h)
        t_login(h)
        t_prompt(h)
        boot = re.findall(r"(?m)^pci: ([0-9a-f:.]+) vendor ([0-9a-f]{4}) "
                          r"device ([0-9a-f]{4}) ", h.boot_output)
        end = "PCI-INVENTORY-END"
        h.send("lsdev pci; echo " + end)
        output = h.capture_until(r"\n" + end + r"\n", regex=True)
        listed = re.findall(r"(?m)^pci +pci:([0-9a-f:.]+) +\S+ +"
                            r"([0-9a-f]{4}):([0-9a-f]{4})\b", output)
        if not boot or listed != boot:
            raise RuntimeError(f"PCI inventory mismatch: boot={boot!r}, lsdev={listed!r}")
        addresses = {address for address, _, _ in listed}
        if len(addresses) != len(listed):
            raise RuntimeError(f"PCI inventory contains duplicate addresses: {listed!r}")
        if not any(not address.endswith(".0") for address in addresses):
            raise RuntimeError("QEMU multifunction PCI device was not enumerated")
        wait_prompt(h)
    except (OSError, RuntimeError, AssertionError) as exc:
        print(f"FAIL PCI inventory ({args.disk}): {exc}", file=sys.stderr)
        h.print_tail()
        return 1
    finally:
        h.kill()
    print(f"PASS PCI inventory ({args.disk}): {len(listed)} functions match boot discovery")
    return 0


if __name__ == "__main__":
    sys.exit(main())
