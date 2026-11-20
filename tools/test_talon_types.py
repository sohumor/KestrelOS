#!/usr/bin/env python3
"""Check Talon's type names and MIME metadata through typed records."""

import re
import sys

from e2e import Harness, qemu_command, t_boot, t_login, t_prompt, wait_prompt


def main():
    types = {
        "text": "text/plain",
        "path": "text/x-path",
        "url": "text/uri-list",
        "color": "text/x-color",
        "number": "text/x-number",
        "krec": "text/x-krec",
        "image": "application/octet-stream",
    }
    h = Harness(qemu_command("rtl8139", 2))
    try:
        h.start()
        t_boot(h)
        t_login(h)
        t_prompt(h)
        for name in types:
            h.send(f"talon put -t {name} -l catalog-{name} payload")
            h.expect(rf"\nparcel \d+ \({name}, 7 bytes\)\n", regex=True)
            wait_prompt(h)
        h.send("talon copy -t text -m text/x-custom -l catalog-custom /etc/motd")
        h.expect(r"\nparcel \d+ \(text, \d+ bytes\)\n", regex=True)
        wait_prompt(h)
        h.send("talon put -t invalid payload")
        h.expect("talon: unknown parcel type 'invalid'")
        wait_prompt(h)
        h.send("talon ls -r; echo TALON-CATALOG-END")
        output = h.capture_until(r"\nTALON-CATALOG-END\n", regex=True)
        rows = re.findall(r"(?m)^\d+\t\d+\t(\w+)\t\d+\t\d+\t\d+\t"
                          r"([^\t\n]+)\t(catalog-\w+)$", output)
        # The wire field holds 23 MIME characters plus its terminator.
        expected = {(name, mime[:23], f"catalog-{name}") for name, mime in types.items()}
        expected.add(("text", "text/x-custom", "catalog-custom"))
        if len(rows) != len(expected) or set(rows) != expected:
            raise AssertionError(f"unexpected Talon metadata: {rows!r}")
    except (OSError, RuntimeError, AssertionError) as exc:
        print(f"FAIL Talon type catalog: {exc}", file=sys.stderr)
        h.print_tail()
        return 1
    finally:
        h.kill()
    print("PASS Talon type catalog: seven defaults, explicit MIME, and unknown type")
    return 0


if __name__ == "__main__":
    sys.exit(main())
