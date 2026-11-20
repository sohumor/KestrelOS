#!/usr/bin/env python3
"""Exercise pipe backpressure and peer-close wakeups in an SMP guest."""

import sys

from e2e import Harness, qemu_command, t_boot, t_login, t_prompt, wait_prompt


def main():
    h = Harness(qemu_command("rtl8139", 2))
    try:
        h.start()
        t_boot(h)
        t_login(h)
        t_prompt(h)
        h.send('kjs -e "print(Kestrel.writeFile(\'/tmp/pipe-wait\',\'x\'.repeat(32768)));"')
        h.expect(r"\n32768\n", regex=True)
        wait_prompt(h)

        h.send("cat /tmp/pipe-wait | wc -c")
        h.expect(r"\n *32768\n", regex=True)
        wait_prompt(h)
        print("PASS pipe backpressure: 32768 bytes transferred through a 4096-byte ring")

        # sleep keeps the read end open without draining, so the producer
        # must unblock when that end closes, not when space is freed.
        h.send("cat /tmp/pipe-wait | sleep 1")
        wait_prompt(h)
        h.send("sleep 1 | wc -c")
        h.expect(r"\n *0\n", regex=True)
        wait_prompt(h)
        print("PASS pipe peer close: full writer and empty reader both released")
    except (OSError, RuntimeError, AssertionError) as exc:
        print(f"FAIL pipe wait: {exc}", file=sys.stderr)
        h.print_tail()
        return 1
    finally:
        h.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
