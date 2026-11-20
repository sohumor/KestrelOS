#!/usr/bin/env python3
"""Check PCI NIC binding and driver priority on a built KestrelOS image."""

import argparse
import re
import sys

from e2e import Harness, qemu_command, t_boot, t_login, t_prompt, wait_prompt


NIC_IDS = {"rtl8139": "10ec:8139", "e1000": "8086:100e"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nic", choices=NIC_IDS, default="rtl8139")
    parser.add_argument("--both", action="store_true",
                        help="attach both types and require RTL8139 to win")
    args = parser.parse_args()
    if args.both and args.nic != "rtl8139":
        parser.error("--both requires --nic rtl8139")

    cmd = qemu_command(args.nic, 2)
    if args.both:
        cmd += ["-netdev", "user,id=n1", "-device", "e1000,netdev=n1"]

    h = Harness(cmd)
    try:
        h.start()
        t_boot(h)
        t_login(h)
        t_prompt(h)
        end = "NIC-BINDING-END"
        h.send("lsdev pci; echo " + end)
        block = h.capture_until(r"\n" + end + r"\n", regex=True)
        expected = [(args.nic, NIC_IDS[args.nic])]
        if args.both:
            expected.append(("-", NIC_IDS["e1000"]))
        for driver, pci_id in expected:
            if not re.search(r"(?m)^pci +pci:[0-9a-f:.]+ +" + driver
                             + " +" + pci_id + r"\b", block):
                raise RuntimeError(f"expected {pci_id} to show driver {driver}")
        wait_prompt(h)
    except (OSError, RuntimeError) as exc:
        print(f"FAIL NIC binding: {exc}", file=sys.stderr)
        h.print_tail()
        return 1
    finally:
        h.kill()
    print(f"PASS PCI NIC binding: {args.nic}" + (" (both NICs)" if args.both else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
