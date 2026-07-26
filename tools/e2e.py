#!/usr/bin/env python3
"""KestrelOS end-to-end test harness.

Boots build/os.img in headless QEMU and drives the shell over serial
(stdio). Python 3 stdlib only.

Usage:
    python3 tools/e2e.py            # full test sequence
    python3 tools/e2e.py --smoke    # boot + prompt only
    python3 tools/e2e.py --list     # list tests without running
    python3 tools/e2e.py --selftest # exercise expect/send plumbing
                                    # against a fake child (no image)
"""

import atexit
import argparse
import collections
import os
import re
import subprocess
import sys
import threading
import time

QEMU_CMD = [
    "qemu-system-x86_64",
    "-drive", "file=build/os.img,format=raw",
    "-no-reboot",
    "-display", "none",
    "-serial", "stdio",
    "-device", "rtl8139,netdev=n0",
    "-netdev", "user,id=n0",
]

DEFAULT_TIMEOUT = 20
BOOT_TIMEOUT = 30
# Recursive walkers (tree/du/find) must finish well inside this; a
# runaway recursion shows up as a timeout instead of a wedged harness.
WALK_TIMEOUT = 10
TAIL_LINES = 40

ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b[@-_]")


def clean(text):
    """Strip ANSI escape sequences and carriage returns."""
    return ANSI_RE.sub("", text).replace("\r", "")


class Harness:
    def __init__(self, cmd):
        self.cmd = cmd
        self.proc = None
        self.buf = ""                 # cleaned, unconsumed text
        self.lock = threading.Lock()
        self.cond = threading.Condition(self.lock)
        self.tail = collections.deque(maxlen=TAIL_LINES)
        self._partial = ""
        self.eof = False

    def start(self):
        self.proc = subprocess.Popen(
            self.cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        atexit.register(self.kill)
        t = threading.Thread(target=self._reader, daemon=True)
        t.start()

    def _reader(self):
        while True:
            # read1 returns as soon as any data is available; a plain
            # buffered read(4096) would block until 4096 bytes or EOF

            chunk = self.proc.stdout.read1(4096)
            if not chunk:
                break
            text = clean(chunk.decode("utf-8", errors="replace"))
            with self.cond:
                self.buf += text
                self._partial += text
                lines = self._partial.split("\n")
                self._partial = lines.pop()
                for line in lines:
                    self.tail.append(line)
                self.cond.notify_all()
        with self.cond:
            if self._partial:
                self.tail.append(self._partial)
                self._partial = ""
            self.eof = True
            self.cond.notify_all()

    def expect(self, pattern, timeout=DEFAULT_TIMEOUT, regex=False):
        """Wait until pattern appears in output; consume through match.

        pattern: literal substring, or regex string if regex=True.
        Returns the re.Match (regex) or the matched string. Raises
        TimeoutError on timeout/EOF.
        """
        rx = re.compile(pattern if regex else re.escape(pattern))
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                m = rx.search(self.buf)
                if m:
                    self.buf = self.buf[m.end():]
                    return m if regex else m.group(0)
                remaining = deadline - time.monotonic()
                if remaining <= 0 or self.eof:
                    raise TimeoutError(
                        "timeout waiting for %r (eof=%s)" % (pattern, self.eof))
                self.cond.wait(min(remaining, 0.5))

    def expect_any(self, patterns, timeout=DEFAULT_TIMEOUT, regex=False):
        """Wait for any of several patterns; returns the matching pattern."""
        rxs = [(p, re.compile(p if regex else re.escape(p)))
               for p in patterns]
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                best = None
                for p, rx in rxs:
                    m = rx.search(self.buf)
                    if m and (best is None or m.start() < best[1].start()):
                        best = (p, m)
                if best:
                    self.buf = self.buf[best[1].end():]
                    return best[0]
                remaining = deadline - time.monotonic()
                if remaining <= 0 or self.eof:
                    raise TimeoutError(
                        "timeout waiting for any of %r (eof=%s)"
                        % (patterns, self.eof))
                self.cond.wait(min(remaining, 0.5))

    def send(self, line):
        self.send_raw(line + "\n")

    def send_raw(self, data):
        self.proc.stdin.write(data.encode("utf-8"))
        self.proc.stdin.flush()

    def print_tail(self):
        print("---- last %d lines of serial output ----" % TAIL_LINES)
        with self.lock:
            for line in list(self.tail):
                print(line)
            if self._partial:
                print(self._partial)
        print("---- end of tail ----")

    def kill(self):
        if self.proc is None or self.proc.poll() is not None:
            return
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass


PROMPT = "kestrel:"
DOTTED_QUAD = r"\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b"


def wait_prompt(h, timeout=DEFAULT_TIMEOUT):
    h.expect_any([r"kestrel:[^\n]*\$"], timeout=timeout, regex=True)


def t_boot(h):
    h.expect("KESTREL READY", timeout=BOOT_TIMEOUT)


def t_prompt(h):
    h.expect("kestrel:/$", timeout=DEFAULT_TIMEOUT)


def t_help(h):
    h.send("help")
    h.expect("commands")
    wait_prompt(h)


def t_echo(h):
    h.send("echo hello world")
    h.expect("hello world")
    wait_prompt(h)


def t_ls_bin(h):
    h.send("ls /bin")
    h.expect("ls")     # alphabetical: ls prints before sh
    h.expect("sh")
    wait_prompt(h)


def t_cat_motd(h):
    h.send("cat /etc/motd")
    h.expect("Kestrel")
    wait_prompt(h)


def t_fs_roundtrip(h):
    h.send("writefile /tmp1.txt")
    time.sleep(0.2)
    h.send("roundtrip-data-123")
    time.sleep(0.2)
    h.send_raw("\x04")
    wait_prompt(h)
    h.send("cat /tmp1.txt")
    h.expect("roundtrip-data-123")
    wait_prompt(h)


def t_ps(h):
    h.send("ps")
    h.expect("sh")
    wait_prompt(h)


def t_free(h):
    h.send("free")
    h.expect_any(["MiB", "KiB"])
    wait_prompt(h)


def t_ping(h):
    h.send("ping 10.0.2.2")
    got = h.expect_any(["reply", "rtt", "network unavailable"])
    wait_prompt(h)
    if got == "network unavailable":
        return "SKIP"


def t_nslookup(h):
    h.send("nslookup example.com")
    got = h.expect_any([DOTTED_QUAD, re.escape("network unavailable")],
                       regex=True)
    wait_prompt(h)
    if got != DOTTED_QUAD:
        return "SKIP"


def t_uptime(h):
    h.send("uptime")
    # "up H:MM:SS" — a bare "s" would match the prompt and eat it
    h.expect_any([r"up \d+:\d\d:\d\d", r"\d+ ?ms"], regex=True)
    wait_prompt(h)


def t_calc(h):
    h.send("calc 2*(3+4)")
    # "\n14\n" cannot match the echoed command line or the prompt
    h.expect(r"\n14\n", regex=True)
    wait_prompt(h)


def t_calc_divzero(h):
    h.send("calc 1/0")
    h.expect("divide by zero")
    wait_prompt(h)


def t_cp_wc(h):
    h.send("cp /etc/version /t2.txt")
    wait_prompt(h)
    h.send("wc -c /t2.txt")
    # wc prints "<count> /t2.txt"; the echoed command has no digits there
    h.expect(r"\n *\d+ /t2\.txt\n", regex=True)
    wait_prompt(h)


def t_mv_rm(h):
    h.send("mv /t2.txt /t3.txt")
    wait_prompt(h)
    h.send("cat /t3.txt")
    h.expect("KestrelOS 0.1.0")     # body of /etc/version, moved twice
    wait_prompt(h)
    h.send("rm /t3.txt")
    wait_prompt(h)
    h.send("cat /t3.txt")
    h.expect("cat: cannot open")    # not present in the echoed command
    wait_prompt(h)


def t_mkdir_ls(h):
    h.send("mkdir /d1")
    wait_prompt(h)
    h.send("ls /")
    # ls prints "d <size>  <name>"; the leading 'd' flag makes it unique
    h.expect(r"\nd +\d+ +d1\n", regex=True)
    wait_prompt(h)


def t_grep(h):
    h.send("grep -n Kestrel /etc/motd")
    # "<lineno>:<line>" — the echoed command has no "<digits>:"
    h.expect(r"\n\d+:[^\n]*KestrelOS", regex=True)
    wait_prompt(h)


def t_head(h):
    h.send("head -n 1 /doc/welcome.md")
    # requiring the prompt immediately after asserts "first line only"
    h.expect(r"# Welcome to KestrelOS\nkestrel:[^\n]*\$", regex=True)


def t_tail(h):
    h.send("tail -n 1 /doc/welcome.md")
    # last line of welcome.md, and nothing after it but the prompt
    h.expect(r"for the release\.\nkestrel:[^\n]*\$", regex=True)


def t_tree(h):
    h.send("tree /etc")
    h.expect(r"\d+ director(y|ies), \d+ files?", regex=True,
             timeout=WALK_TIMEOUT)
    wait_prompt(h, timeout=WALK_TIMEOUT)


def t_du(h):
    h.send("du /etc")
    h.expect(r"\n *\d+  /etc\n", regex=True, timeout=WALK_TIMEOUT)
    wait_prompt(h, timeout=WALK_TIMEOUT)


def t_find(h):
    h.send("find /etc")
    # a path the echoed "find /etc" cannot contain
    h.expect(r"\n/etc/version\n", regex=True, timeout=WALK_TIMEOUT)
    wait_prompt(h, timeout=WALK_TIMEOUT)


def t_date(h):
    h.send("date")
    got = h.expect_any([r"\d{4}-\d\d-\d\d", re.escape("no clock")],
                       regex=True)
    wait_prompt(h)
    if got != r"\d{4}-\d\d-\d\d":
        return "SKIP"


def t_err_cat_missing(h):
    h.send("cat /nope")
    h.expect("cat: cannot open")
    wait_prompt(h)


def t_err_rm_missing(h):
    h.send("rm /nope")
    h.expect("rm: cannot remove")
    wait_prompt(h)


def t_err_unknown_cmd(h):
    h.send("notacommand42")
    # the echoed line is just the word; this prefix is shell-only
    h.expect("sh: command not found")
    wait_prompt(h)


def t_long_line(h):
    # 300 chars: past the shell's 256-byte line buffer. Excess input is
    # dropped by readline, so the shell must still reach a prompt.
    h.send("zq" * 150)
    h.expect_any(["sh: command not found", "sh: too many tokens"])
    wait_prompt(h)


TESTS = [
    ("boot", t_boot),
    ("shell-prompt", t_prompt),
    ("help", t_help),
    ("echo", t_echo),
    ("ls-bin", t_ls_bin),
    ("cat-motd", t_cat_motd),
    ("fs-roundtrip", t_fs_roundtrip),
    ("ps", t_ps),
    ("free", t_free),
    ("ping", t_ping),
    ("nslookup", t_nslookup),
    ("uptime", t_uptime),
    ("calc", t_calc),
    ("calc-divzero", t_calc_divzero),
    ("cp-wc", t_cp_wc),
    ("mv-rm", t_mv_rm),
    ("mkdir-ls", t_mkdir_ls),
    ("grep", t_grep),
    ("head", t_head),
    ("tail", t_tail),
    ("tree", t_tree),
    ("du", t_du),
    ("find", t_find),
    ("date", t_date),
    ("err-cat-missing", t_err_cat_missing),
    ("err-rm-missing", t_err_rm_missing),
    ("err-unknown-cmd", t_err_unknown_cmd),
    ("long-line", t_long_line),
]


def run_tests(h, tests):
    results = []
    failed = False
    for name, fn in tests:
        if failed:
            results.append((name, "SKIP"))
            continue
        try:
            status = fn(h) or "PASS"
        except TimeoutError as e:
            print("FAIL %s: %s" % (name, e))
            h.print_tail()
            status = "FAIL"
            failed = True
        except (OSError, BrokenPipeError) as e:
            print("FAIL %s: qemu pipe error: %s" % (name, e))
            h.print_tail()
            status = "FAIL"
            failed = True
        print("%s %s" % (status, name))
        results.append((name, status))
    return results


def summarize(results):
    npass = sum(1 for _, s in results if s == "PASS")
    nfail = sum(1 for _, s in results if s == "FAIL")
    nskip = sum(1 for _, s in results if s == "SKIP")
    print("summary: %d passed, %d failed, %d skipped, %d total"
          % (npass, nfail, nskip, len(results)))
    return nfail == 0


def selftest():
    """Exercise expect/send plumbing against a fake python child."""
    fake = (
        "import sys\n"
        "sys.stdout.write('\\x1b[32mKESTREL READY\\x1b[0m\\r\\n')\n"
        "sys.stdout.write('kestrel:/$ ')\n"
        "sys.stdout.flush()\n"
        "for line in sys.stdin:\n"
        "    line = line.strip()\n"
        "    if line.startswith('echo '):\n"
        "        sys.stdout.write(line[5:] + '\\r\\n')\n"
        "    elif line == 'quit':\n"
        "        break\n"
        "    sys.stdout.write('kestrel:/$ ')\n"
        "    sys.stdout.flush()\n"
    )
    h = Harness([sys.executable, "-u", "-c", fake])
    h.start()
    try:
        h.expect("KESTREL READY", timeout=10)
        h.expect("kestrel:/$", timeout=10)
        h.send("echo hello world")
        h.expect("hello world", timeout=10)
        h.expect_any([r"kestrel:[^\n]*\$"], timeout=10, regex=True)
        h.send("quit")
    except TimeoutError as e:
        print("FAIL selftest: %s" % e)
        h.print_tail()
        return False
    finally:
        h.kill()
    print("PASS selftest")
    return True


def main():
    ap = argparse.ArgumentParser(description="KestrelOS e2e test harness")
    ap.add_argument("--smoke", action="store_true",
                    help="run boot + prompt tests only")
    ap.add_argument("--list", action="store_true",
                    help="list tests and exit")
    ap.add_argument("--selftest", action="store_true",
                    help="test harness plumbing without an OS image")
    args = ap.parse_args()

    if args.list:
        for name, _ in TESTS:
            print(name)
        return 0

    if args.selftest:
        return 0 if selftest() else 1

    if not os.path.exists("build/os.img"):
        print("error: build/os.img not found (run make first)")
        return 1

    tests = TESTS[:2] if args.smoke else TESTS
    h = Harness(QEMU_CMD)
    try:
        h.start()
    except FileNotFoundError:
        print("error: qemu-system-x86_64 not found in PATH")
        return 1
    try:
        results = run_tests(h, tests)
    finally:
        h.kill()
    return 0 if summarize(results) else 1


if __name__ == "__main__":
    sys.exit(main())
