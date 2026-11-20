#!/usr/bin/env python3
"""Exercise per-window event delivery in a two-CPU QEMU guest."""

import os
from pathlib import Path
import socket
import subprocess
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

VICTIM = (
    "var keep=Kestrel.openWindow('probe-keep',120,80);",
    "var w=Kestrel.openWindow('probe-victim',120,80);while(w.pollEvent(0)){}",
    "print('WINDOW-REUSE-READY');var t=Kestrel.uptime(),e=w.pollEvent(5000);",
    "print(e&&e.type=='close'&&Kestrel.uptime()-t<4500?'WINDOW-OLD-CLOSED':'WINDOW-REUSE-FAIL');",
    "w.close();keep.close();",
)

REPLACEMENT = (
    "var w=Kestrel.openWindow('probe-replacement',120,80),e=w.pollEvent(0);",
    "print(e&&e.type=='focus'?'WINDOW-NEW-FOCUSED':'WINDOW-REUSE-FAIL');w.close();",
)


def expect(h, expected):
    marker = rf"\n{expected}(?=\n)"
    result = h.expect_any([marker, r"\nWINDOW-FAIL(?=\n)"], regex=True)
    if result != marker:
        raise AssertionError(f"guest rejected {expected}")


def write_script(h, name, script):
    for line in script:
        h.send(f'echo "{line}" >> /tmp/{name}.js')
        wait_prompt(h)


def test_reuse(work):
    image = Path(work) / "os.img"
    subprocess.run(["make", "-f", "Makefile", "-f", "tools/window_event_probe.mk",
                    f"PROBE_DIR={work}", f"CC={os.environ.get('CC', 'gcc -march=x86-64')}",
                    str(image)], check=True)
    command = qemu_command("rtl8139", 2)
    command[command.index("-drive") + 1] = f"file={image},format=raw"
    h = Harness(command)
    try:
        h.start()
        t_boot(h)
        t_login(h)
        t_prompt(h)
        write_script(h, "victim", VICTIM)
        write_script(h, "replacement", REPLACEMENT)
        h.send("kjs /tmp/victim.js & kjs /tmp/replacement.js")
        seen = set()
        markers = {rf"(?<=\n){name}(?=\n)" for name in
                   ("WINDOW-SLOT-REUSED", "WINDOW-OLD-CLOSED", "WINDOW-NEW-FOCUSED")}
        failure = r"(?<=\n)WINDOW-REUSE-FAIL(?=\n)"
        while seen != markers:
            matched = h.expect_any(sorted(markers - seen) + [failure], regex=True)
            if matched == failure:
                raise AssertionError("guest rejected window slot reuse")
            seen.add(matched)
        print("PASS window slot reuse: blocked owner released; replacement event retained")
    except (OSError, RuntimeError, AssertionError):
        h.print_tail()
        raise
    finally:
        h.kill()


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
            write_script(h, "window-events", SCRIPT)
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
        try:
            test_reuse(work)
        except (OSError, RuntimeError, AssertionError, subprocess.CalledProcessError) as exc:
            print(f"FAIL window slot reuse: {exc}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
