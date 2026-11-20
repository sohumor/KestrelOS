#!/usr/bin/env python3
"""Check that ATA PIO claims the platform slot only on an ATA boot."""

import re
import sys

from e2e import Harness, qemu_command, t_boot, t_login, t_prompt


def check(disk, driver, blockdev):
    h = Harness(qemu_command("rtl8139", 2, disk))
    h.disk = disk
    try:
        h.start()
        t_boot(h)
        t_login(h)
        t_prompt(h)
        h.send("lsdev platform; ls /dev; echo ATA-BINDING-END")
        output = h.capture_until(r"\nATA-BINDING-END\n", regex=True)
        drivers = re.findall(r"(?m)^platform +ata-primary +(\S+)", output)
        disks = re.findall(r"(?m)^- +\d+ +(hda|sda)$", output)
        if drivers != [driver] or disks != [blockdev]:
            raise AssertionError(f"expected {driver=} and {blockdev=}; "
                                 f"found {drivers=} and {disks=}")
    except (OSError, RuntimeError, AssertionError) as exc:
        print(f"FAIL ATA binding ({disk}): {exc}", file=sys.stderr)
        h.print_tail()
        return False
    finally:
        h.kill()
    print(f"PASS ATA binding ({disk}): driver {driver}, disk /dev/{blockdev}")
    return True


if __name__ == "__main__":
    results = [check("ata", "ata-pio", "hda"), check("ahci", "-", "sda")]
    sys.exit(0 if all(results) else 1)
