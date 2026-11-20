#!/usr/bin/env python3
"""Check that e1000 binding prefers a 82540EM over an earlier 82545EM."""

import re
import sys

from e2e import Harness, qemu_command, t_boot, t_login, t_prompt, wait_prompt


def main():
    cmd = qemu_command("e1000", 2)
    cmd[cmd.index("-device") + 1] = "e1000-82545em,netdev=n0"
    cmd += ["-netdev", "user,id=n1", "-device", "e1000,netdev=n1"]
    h = Harness(cmd)
    try:
        h.start()
        t_boot(h)
        t_login(h)
        t_prompt(h)
        end = "E1000-BINDING-END"
        h.send("lsdev pci; echo " + end)
        output = h.capture_until(r"\n" + end + r"\n", regex=True)
        bindings = re.findall(r"(?m)^pci +(pci:[0-9a-f:.]+) +(\S+) +(8086:100[ef])\b",
                              output)
        if [(driver, pci_id) for _, driver, pci_id in bindings] != [
                ("-", "8086:100f"), ("e1000", "8086:100e")]:
            raise RuntimeError(f"unexpected Intel NIC order or binding: {bindings!r}")
        wait_prompt(h)
    except (OSError, RuntimeError) as exc:
        print(f"FAIL e1000 variant priority: {exc}", file=sys.stderr)
        h.print_tail()
        return 1
    finally:
        h.kill()
    print("PASS e1000 variant priority: 82540EM selected ahead of earlier 82545EM")
    return 0


if __name__ == "__main__":
    sys.exit(main())
