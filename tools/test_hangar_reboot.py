#!/usr/bin/env python3
"""Hangar end to end: park apps, power-cycle the VM, resume them.

Boots a private copy of build/os.img (no QEMU snapshot, so the disk
persists between the two boots):

  boot 1  parktest builds state in heap, stack, an SSE register, an open
          file offset, its window and its signal mask; `hangar park`
          seals it into a bay. Apps holding a pipe or a TCP connection
          must be refused with the reason.
  halt    the machine powers off.
  boot 2  the kernel reports the waiting bay; `hangar resume` brings
          parktest back and it verifies every piece of state itself.
"""

import os
import re
import shutil
import socket
import sys
import tempfile
import threading

from e2e import BOOT_TIMEOUT, Harness, find_qemu, t_login, t_prompt, \
    wait_prompt

PARK_TIMEOUT = 120


def qemu_cmd(disk_path):
    return [
        find_qemu(),
        "-accel", "tcg",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-drive", "file=%s,format=raw" % disk_path,
        "-no-reboot",
        "-display", "none",
        "-serial", "stdio",
        "-m", "256M",
        "-device", "rtl8139,netdev=n0",
        "-netdev", "user,id=n0",
    ]


def start_listener():
    """A host TCP endpoint the guest reaches at 10.0.2.2:<port>."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(4)
    held = []

    def accept_loop():
        while True:
            try:
                conn, _ = srv.accept()
            except OSError:
                return
            held.append(conn)

    threading.Thread(target=accept_loop, daemon=True).start()
    return srv, srv.getsockname()[1]


def launch_parktest(h, args=""):
    h.send(("parktest --bg %s &" % args).strip())
    # The shell prompts again before the job prints, so the prompt is
    # already behind us here.
    m = h.expect(r"PARKTEST READY pid=(\d+)", timeout=30, regex=True)
    return int(m.group(1))


def expect_refusal(h, args, needle):
    pid = launch_parktest(h, args)
    h.send("hangar park %d" % pid)
    h.expect("cannot park pid %d" % pid, timeout=PARK_TIMEOUT)
    h.expect(needle, timeout=10)
    wait_prompt(h)
    h.send("kill %d" % pid)
    wait_prompt(h)
    print("PASS: refused to park parktest %s (%s)" % (args, needle))


def halt(h):
    h.send("halt")
    h.expect("power: system halted", timeout=30)
    h.proc.wait(timeout=30)


def boot_one(disk_path, port):
    h = Harness(qemu_cmd(disk_path))
    try:
        h.start()
        h.expect("KESTREL READY", timeout=BOOT_TIMEOUT)
        t_login(h)
        t_prompt(h)

        expect_refusal(h, "--pipe", "has an open pipe")
        expect_refusal(h, "--tcp %d" % port, "open network connection")

        pid = launch_parktest(h)
        h.send("hangar park %d" % pid)
        m = h.expect(r"pid %d parked in bay (\d+)" % pid,
                     timeout=PARK_TIMEOUT, regex=True)
        bay = int(m.group(1))
        wait_prompt(h)
        h.send("hangar")
        h.expect(re.compile(r"%02d\s+parktest\s+Park Test" % bay).pattern,
                 timeout=20, regex=True)
        wait_prompt(h)
        print("PASS: parked parktest (pid %d) in bay %02d" % (pid, bay))
        halt(h)
        return bay
    except Exception:
        h.print_tail()
        raise
    finally:
        h.kill()


def boot_two(disk_path, bay):
    h = Harness(qemu_cmd(disk_path))
    try:
        h.start()
        h.expect("hangar: 1 parked app waiting", timeout=BOOT_TIMEOUT)
        h.expect("KESTREL READY", timeout=BOOT_TIMEOUT)
        t_login(h)
        t_prompt(h)
        h.send("hangar resume %d" % bay)
        ok = r"PARKTEST RESUMED OK pid=\d+"
        which = h.expect_any([ok, r"PARKTEST RESUMED BAD [a-z ]+",
                              r"cannot resume bay"],
                             timeout=PARK_TIMEOUT, regex=True)
        if which != ok:
            raise RuntimeError("parktest did not resume intact")
        h.send("hangar")
        h.expect("hangar: no parked apps", timeout=20)
        wait_prompt(h)
        print("PASS: resumed bay %02d after power cycle with state intact"
              % bay)
        halt(h)
    except Exception:
        h.print_tail()
        raise
    finally:
        h.kill()


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    image = os.path.join(root, "build", "os.img")
    if not os.path.isfile(image):
        print("error: build/os.img missing (run make first)", file=sys.stderr)
        return 2
    srv, port = start_listener()
    try:
        with tempfile.TemporaryDirectory(prefix="kestrel-hangar-") as tmp:
            disk = os.path.join(tmp, "hangar-os.img")
            shutil.copyfile(image, disk)
            bay = boot_one(disk, port)
            boot_two(disk, bay)
    finally:
        srv.close()
    print("PASS: Hangar park / power cycle / resume")
    return 0


if __name__ == "__main__":
    sys.exit(main())
