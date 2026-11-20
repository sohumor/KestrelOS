#!/usr/bin/env python3
"""Exercise per-window event delivery in a two-CPU QEMU guest."""

from pathlib import Path
import socket
import sys
from tempfile import TemporaryDirectory

from e2e import Harness, qemu_command, t_boot, t_login, t_prompt, wait_prompt


SCRIPT = (
    "var a=Kestrel.openWindow('A',120,80),b=Kestrel.openWindow('B',120,80);",
    "while(a.pollEvent(0)){}while(b.pollEvent(0)){}",
    "print('WINDOW-FIRST');var t=Kestrel.uptime(),e=a.pollEvent(1500);",
    "print(!e&&Kestrel.uptime()-t>=1490?'WINDOW-TIMEOUT':'WINDOW-FAIL');",
    "e=b.pollEvent(0);print(e&&e.type=='key'&&e.key==120?'WINDOW-NOW':'WINDOW-FAIL');",
    "print('WINDOW-SECOND');e=b.pollEvent(5000);",
    "print(e&&e.type=='key'&&e.key==121?'WINDOW-WAKE':'WINDOW-FAIL');",
    "b.close();a.close();",
)


def expect(h, expected):
    marker = rf"\n{expected}(?=\n)"
    result = h.expect_any([marker, r"\nWINDOW-FAIL(?=\n)"], regex=True)
    if result != marker:
        raise AssertionError(f"guest rejected {expected}")


def main():
    with TemporaryDirectory() as work:
        monitor = str(Path(work) / "monitor")
        command = qemu_command("rtl8139", 2)
        command += ["-monitor", f"unix:{monitor},server=on,wait=off"]
        h = Harness(command)
        try:
            h.start()
            t_boot(h)
            t_login(h)
            t_prompt(h)
            for line in SCRIPT:
                h.send(f'echo "{line}" >> /tmp/window-events.js')
                wait_prompt(h)
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as hmp:
                hmp.connect(monitor)
                h.send("kjs /tmp/window-events.js")
                expect(h, "WINDOW-FIRST")
                hmp.sendall(b"sendkey x\n")
                expect(h, "WINDOW-TIMEOUT")
                expect(h, "WINDOW-NOW")
                expect(h, "WINDOW-SECOND")
                hmp.sendall(b"sendkey y\n")
                expect(h, "WINDOW-WAKE")
            wait_prompt(h)
            print("PASS window events: timeout, queued input, and blocking keyboard wake")
        except (OSError, RuntimeError, AssertionError) as exc:
            print(f"FAIL window event wait: {exc}", file=sys.stderr)
            h.print_tail()
            return 1
        finally:
            h.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
