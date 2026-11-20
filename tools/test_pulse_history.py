#!/usr/bin/env python3
"""Check multi-batch Pulse replay through the typed inspector output."""

import re
import sys

from e2e import Harness, qemu_command, t_boot, t_login, t_prompt, wait_prompt


def main():
    h = Harness(qemu_command("rtl8139", 2))
    try:
        h.start()
        t_boot(h)
        t_login(h)
        t_prompt(h)
        for _ in range(9):
            h.send("; ".join(["cat /dev/null"] * 4))
            wait_prompt(h)
        h.send("talon put -t text -l pulse-history cursor-probe")
        h.expect(r"\nparcel \d+ \(text, \d+ bytes\)\n", regex=True)
        wait_prompt(h)

        h.send("inspect -r pulse; echo PULSE-HISTORY-END")
        output = h.capture_until(r"\nPULSE-HISTORY-END\n", timeout=60, regex=True)
        schema = "@krec seq:u64 topic:str kind:str arg0:u64 arg1:u64 tag:str"
        if schema not in output.splitlines():
            raise AssertionError("missing typed Pulse schema")
        rows = re.findall(r"(?m)^(\d+)\t(\w+)\t(\w+)\t\d+\t\d+\t(.*)$",
                          output)
        seqs = [int(row[0]) for row in rows]
        if len(seqs) <= 64 or any(b != a + 1 for a, b in zip(seqs, seqs[1:])):
            raise AssertionError(f"expected consecutive history across batches: {seqs}")
        if sum(row[1:] == ("parcel", "parcel_new", "pulse-history")
               for row in rows) != 1:
            raise AssertionError("expected exactly one typed Talon parcel event")
    except (OSError, RuntimeError, AssertionError) as exc:
        print(f"FAIL Pulse history: {exc}", file=sys.stderr)
        h.print_tail()
        return 1
    finally:
        h.kill()
    print(f"PASS Pulse history: {len(seqs)} ordered records and one typed Talon event")
    return 0


if __name__ == "__main__":
    sys.exit(main())
