#!/usr/bin/env python3
"""Verify that the filtered Flight view preserves a typed kernel record."""

import re
import sys

from e2e import Harness, qemu_command, t_boot, t_login, t_prompt, wait_prompt


SCHEMA = "@krec tick:u64 cpu:int pid:int cat:str event:str arg0:hex detail:str"
RECORD = r"(?m)^\d+\t\d+\t\d+\ttalon\tparcel_put\t[^\t\n]+\tflight-shared$"


def read_event(h, command):
    h.send(command + "; echo FLIGHT-RECORD-END")
    output = h.capture_until(r"\nFLIGHT-RECORD-END\n", regex=True)
    wait_prompt(h)
    if SCHEMA not in output.splitlines():
        raise AssertionError(f"missing Flight schema from {command}")
    rows = re.findall(RECORD, output)
    if len(rows) != 1:
        raise AssertionError(f"expected one Talon event from {command}: {rows!r}")
    return rows[0]


def main():
    h = Harness(qemu_command("rtl8139", 2))
    try:
        h.start()
        t_boot(h)
        t_login(h)
        t_prompt(h)
        h.send("talon put -t number -l flight-shared 42")
        h.expect(r"\nparcel \d+ \(number, 2 bytes\)\n", regex=True)
        wait_prompt(h)

        raw = read_event(h, 'cat /dev/flight | where "detail == flight-shared" | '
                            'where "cat == talon"')
        filtered = read_event(h, "flight --krec --cat talon -n 1 | "
                                 "pick -r tick,cpu,pid,cat,event,arg0,detail")
        if raw != filtered:
            raise AssertionError(f"record changed: kernel={raw!r}, filtered={filtered!r}")
    except (OSError, RuntimeError, AssertionError) as exc:
        print(f"FAIL Flight records: {exc}", file=sys.stderr)
        h.print_tail()
        return 1
    finally:
        h.kill()
    print("PASS Flight records: filtered @krec pipeline preserved the kernel's Talon event")
    return 0


if __name__ == "__main__":
    sys.exit(main())
