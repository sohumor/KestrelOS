#!/usr/bin/env python3
"""KestrelOS end-to-end test harness.

Boots build/os.img in headless QEMU, logs in on the serial console and
drives the shell over serial (stdio). Python 3 stdlib only.

The boot now ends at /bin/login, so the suite signs in as root before it
can do anything else; a build with /etc/autologin lands straight on a
shell and is handled too. See t_login.

Writing a test: every expect() pattern must be unique to the command's
OUTPUT. The shell echoes the line you typed and then prints its prompt,
so a pattern that can match either of those consumes the wrong text and
desynchronises everything that follows. Anchoring on a leading "\\n", on
punctuation the command line does not contain, or on a word only the
program prints are the three tricks used throughout.

Usage:
    python3 tools/e2e.py            # full test sequence
    python3 tools/e2e.py --smoke    # boot + prompt only
    python3 tools/e2e.py --nic e1000
    python3 tools/e2e.py --list     # list tests without running
    python3 tools/e2e.py --selftest # exercise expect/send plumbing
                                    # against a fake child (no image)
"""

import atexit
import argparse
import collections
import http.client
import http.server
import os
import re
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import time

QEMU_BASE_CMD = [
    "qemu-system-x86_64",
    "-drive", "file=build/os.img,format=raw",
    # Package/filesystem tests deliberately mutate the guest disk. Keep those
    # writes in a temporary overlay so repeated runs always start from the
    # image produced by make and never contaminate build/os.img.
    "-snapshot",
    "-no-reboot",
    "-display", "none",
    "-serial", "stdio",
]

QEMU_NICS = {
    "rtl8139": "rtl8139",
    "e1000": "e1000",
}


def qemu_command(nic):
    return QEMU_BASE_CMD + [
        "-device", "%s,netdev=n0" % QEMU_NICS[nic],
        "-netdev", "user,id=n0",
    ]


DEFAULT_TIMEOUT = 20
BOOT_TIMEOUT = 30
# Recursive walkers (tree/du/find) must finish well inside this; a
# runaway recursion shows up as a timeout instead of a wedged harness.
WALK_TIMEOUT = 10
# kpkg hashes every file it installs or verifies with a userspace SHA-256,
# which is slow enough on an emulated machine to need its own budget.
PKG_TIMEOUT = 90
# The network tests all SKIP on failure, but a DNS lookup plus a TCP
# handshake to the outside world still has to be given time to fail.
NET_TIMEOUT = 45
TAIL_LINES = 40

# QEMU user networking exposes the host-side slirp endpoint here.  Browser
# transport tests bind ephemeral host ports and reach them through this IP.
QEMU_HOST = "10.0.2.2"
HTTP_BODY_MARKER = "CONTROLLED-HTTP-BODY-OK"
TLS_NEGATIVE_BODY_MARKER = "TLS-NEGATIVE-BODY-MUST-NOT-APPEAR"
TLS_NEGATIVE_REPETITIONS = 10

TLS_NEGATIVE_EXIT_RE = re.compile(r"(?m)^\[exit (-?\d+)\]$")
TLS_NEGATIVE_STATUS_RE = re.compile(
    r"(?m)^BROWSER-TLS-NEG-STATUS-(-?\d+)$")
TLS_NEGATIVE_DIAGNOSTIC_RE = re.compile(
    r"(?i)(certificate|host ?name|trusted root|self[- ]signed|"
    r"does not match)")
TLS_NEGATIVE_REASON_RE = re.compile(
    r"(?i)(kestrel-negative\.invalid|trusted root|self[- ]signed|"
    r"untrusted|not trusted|unknown issuer|"
    r"certificate chain[^\n]{0,48}(?:trust|root|issuer))")
TLS_NEGATIVE_FORBIDDEN_RE = re.compile(
    r"(?i)(uproc:|\bfault\b|\bexception\b|kernel[ -]+panic|"
    r"\bpanic\b|\[exit -1\]|BROWSER-TLS-NEG-STATUS--1|"
    r"\bno error\b)")

ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b[@-_]")
# An escape sequence the current chunk ends in the middle of. QEMU hands us
# whatever has arrived, so a repaint's "\x1b[20C" is regularly split across
# two reads; stripping each half separately would leave "20C" sitting in
# the buffer where a pattern could trip over it.
ANSI_TAIL_RE = re.compile(r"\x1b(\[[0-9;?]*[ -/]*)?$")


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
        self._esc = ""                # half of an escape sequence, held back
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
            raw = self._esc + chunk.decode("utf-8", errors="replace")
            m = ANSI_TAIL_RE.search(raw)
            if m:
                self._esc = raw[m.start():]
                raw = raw[:m.start()]
            else:
                self._esc = ""
            text = clean(raw)
            if not text:
                continue
            with self.cond:
                self.buf += text
                self._partial += text
                lines = self._partial.split("\n")
                self._partial = lines.pop()
                for line in lines:
                    self.tail.append(line)
                self.cond.notify_all()
        with self.cond:
            self._esc = ""            # nothing will ever complete it now
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

    def capture_until(self, pattern, timeout=DEFAULT_TIMEOUT, regex=False):
        """Consume through pattern and return the complete consumed block."""
        rx = re.compile(pattern if regex else re.escape(pattern))
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                m = rx.search(self.buf)
                if m:
                    block = self.buf[:m.end()]
                    self.buf = self.buf[m.end():]
                    return block
                remaining = deadline - time.monotonic()
                if remaining <= 0 or self.eof:
                    raise TimeoutError(
                        "timeout capturing through %r (eof=%s)"
                        % (pattern, self.eof))
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


class _QuietHTTPServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, *args, **kwargs):
        self._get_count = 0
        self._get_count_lock = threading.Lock()
        super().__init__(*args, **kwargs)

    def record_get(self):
        with self._get_count_lock:
            self._get_count += 1

    @property
    def get_count(self):
        with self._get_count_lock:
            return self._get_count

    def handle_error(self, request, client_address):
        # The certificate-negative client deliberately aborts during the
        # handshake.  That expected TLS close is not a harness failure.
        del request, client_address


def _marker_handler(marker):
    body = (
        "<!doctype html><html><head><title>Kestrel controlled fixture</title>"
        "</head><body><h1>%s</h1></body></html>" % marker
    ).encode("ascii")

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            self.server.record_get()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=us-ascii")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, fmt, *args):
            del fmt, args

    return Handler


class BrowserFixtureServers:
    """Controlled plain HTTP and certificate-negative TLS 1.3 endpoints."""

    def __init__(self):
        self.temp = None
        self.http = None
        self.https = None
        self.threads = []
        self.http_url = None
        self.https_url = None

    @staticmethod
    def _serve(server):
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        return thread

    def start(self):
        openssl = shutil.which("openssl")
        if not openssl:
            raise RuntimeError("openssl is required for browser TLS E2E")

        self.temp = tempfile.TemporaryDirectory(
            prefix="kestrel-browser-e2e-")
        cert = os.path.join(self.temp.name, "negative-cert.pem")
        key = os.path.join(self.temp.name, "negative-key.pem")
        cmd = [
            openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-sha256", "-days", "2", "-set_serial", "1",
            "-subj", "/CN=kestrel-negative.invalid",
            "-addext", "subjectAltName=DNS:kestrel-negative.invalid",
            "-addext", "basicConstraints=critical,CA:FALSE",
            "-addext", "keyUsage=critical,digitalSignature,keyEncipherment",
            "-addext", "extendedKeyUsage=serverAuth",
            "-keyout", key, "-out", cert,
        ]
        made = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True)
        if made.returncode != 0:
            raise RuntimeError("cannot generate negative TLS certificate: %s"
                               % made.stderr.strip())

        self.http = _QuietHTTPServer(
            ("0.0.0.0", 0), _marker_handler(HTTP_BODY_MARKER))
        self.https = _QuietHTTPServer(
            ("0.0.0.0", 0), _marker_handler(TLS_NEGATIVE_BODY_MARKER))
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_3
        context.maximum_version = ssl.TLSVersion.TLSv1_3
        context.load_cert_chain(cert, key)
        self.https.socket = context.wrap_socket(
            self.https.socket, server_side=True)

        self.threads.append(self._serve(self.http))
        self.threads.append(self._serve(self.https))
        self.http_url = "http://%s:%d/plain" % (
            QEMU_HOST, self.http.server_address[1])
        self.https_url = "https://%s:%d/negative" % (
            QEMU_HOST, self.https.server_address[1])
        print("browser fixtures: plain=%s tls13-negative=%s "
              "cert-cn=kestrel-negative.invalid"
              % (self.http_url, self.https_url))
        return self

    def close(self):
        for server in (self.http, self.https):
            if server:
                server.shutdown()
                server.server_close()
        for thread in self.threads:
            thread.join(timeout=5)
        self.threads = []
        self.http = None
        self.https = None
        if self.temp:
            self.temp.cleanup()
            self.temp = None


PROMPT = "kestrel:"
DOTTED_QUAD = r"\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b"

SHELL_PROMPT = r"kestrel:[^\n]*\$"
LOGIN_PROMPT = r"\nlogin: "

# /etc/shadow ships these on purpose; see docs/users.md.
ROOT_USER, ROOT_PASS = "root", "root"
USER_NAME, USER_PASS = "kestrel", "kestrel"
MAX_LOGIN_TRIES = 3

# Set by t_pipe once it has found out whether this shell understands '|'.
# The shell grew redirection before it grew pipes, so the tests that need
# one have to ask rather than assume.
have_pipes = False
browser_fixtures = None


def wait_prompt(h, timeout=DEFAULT_TIMEOUT):
    h.expect_any([SHELL_PROMPT], timeout=timeout, regex=True)


def t_boot(h):
    h.expect("KESTREL READY", timeout=BOOT_TIMEOUT)


def t_login(h):
    """Get to a shell, logging in on the console if init asks us to.

    /etc/inittab respawns /bin/login unless /etc/autologin exists, so both
    outcomes are legal and the harness has to handle both: answer the login
    prompt, or find a shell already waiting. Every single test after this
    one needs a shell, which is why it retries and why it is deliberately
    the only test allowed to match a bare prompt.

    On the login path the shell's *first* prompt is left unconsumed, so
    t_prompt has something to match; on the autologin path the prompt has
    already been eaten, so a no-output `cd /` is sent to produce another.
    """
    for _ in range(MAX_LOGIN_TRIES):
        which = h.expect_any([LOGIN_PROMPT, SHELL_PROMPT],
                             timeout=BOOT_TIMEOUT, regex=True)
        if which == SHELL_PROMPT:
            h.send("cd /")
            return
        h.send(ROOT_USER)
        h.expect("password:", timeout=DEFAULT_TIMEOUT)
        h.send(ROOT_PASS)
        # login sleeps 2s after a bad password before asking again.
        got = h.expect_any(["welcome, " + ROOT_USER, "login incorrect"],
                           timeout=DEFAULT_TIMEOUT)
        if got != "login incorrect":
            return
    raise TimeoutError("could not log in as %s after %d tries"
                       % (ROOT_USER, MAX_LOGIN_TRIES))


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


# ---- accounts and permissions ----------------------------------------


def t_whoami(h):
    # "\nroot\n" cannot match the echoed "whoami" or the prompt
    h.send("whoami")
    h.expect(r"\nroot\n", regex=True)
    wait_prompt(h)


def t_id(h):
    h.send("id")
    h.expect(r"uid=0\(root\) gid=0\(root\)", regex=True)
    wait_prompt(h)


def t_permissions(h):
    """An unprivileged account must not be able to read /etc/shadow.

    su drops to uid 1000 and spawns a nested shell; /etc/shadow is mode
    0600 owned by root and there is no setuid bit, so the read has to
    fail. Leaving the nested shell brings the suite back to root.
    """
    h.send("su " + USER_NAME)
    h.expect("password:")
    h.send(USER_PASS)
    h.expect("su: root -> " + USER_NAME)
    wait_prompt(h)

    h.send("whoami")
    h.expect("\n" + USER_NAME + "\n", regex=True)
    wait_prompt(h)

    h.send("id")
    h.expect(r"uid=1000\(kestrel\)", regex=True)
    wait_prompt(h)

    h.send("cat /etc/shadow")
    h.expect("cat: cannot open")
    wait_prompt(h)

    # ... and root can still read it, which is what makes the failure
    # above a permission check rather than a missing file.
    h.send("exit")
    h.expect("su: back to root")
    wait_prompt(h)
    h.send("cat /etc/shadow")
    h.expect(":4096:", timeout=DEFAULT_TIMEOUT)
    wait_prompt(h)


# ---- plumbing: redirection, pipes, /dev -------------------------------


def t_redirect(h):
    """'>' , '>>' and '<' still wire up fd 0/1 through spawn_io()."""
    h.send("echo redirect-marker > /t4.txt")
    wait_prompt(h)
    h.send("cat /t4.txt")
    h.expect(r"\nredirect-marker\n", regex=True)
    wait_prompt(h)
    # "redirect-marker\n" is 16 bytes; wc reading it off fd 0 proves '<'
    h.send("wc -c < /t4.txt")
    h.expect(r"\n *16\n", regex=True)
    wait_prompt(h)
    h.send("echo appended-line >> /t4.txt")
    wait_prompt(h)
    h.send("wc -l /t4.txt")
    h.expect(r"\n *2 /t4\.txt\n", regex=True)
    wait_prompt(h)
    h.send("rm /t4.txt")
    wait_prompt(h)


def t_pipe(h):
    """`ls /bin | wc -l`, if this shell has pipes yet.

    Without them sh hands '|', 'wc' and '-l' to ls as arguments and ls
    rejects '-l' with its usage line, which is what the SKIP branch
    matches. Sets have_pipes for the tests that want to use one.
    """
    global have_pipes

    h.send("ls /bin | wc -l")
    h.expect("| wc -l")             # consume the echoed command line
    got = h.expect_any([r"\n *\d+\n", r"usage: ls \[-a\]"], regex=True)
    wait_prompt(h)
    if got != r"\n *\d+\n":
        return "SKIP"
    have_pipes = True


def t_dev(h):
    """/dev: the devfs mount, an immediate-EOF device and an empty dump.

    /dev/zero is an endless stream, so it is only ever read through a
    pipe - and only when the shell has pipes and hexdump gives up on a
    closed one. Reading it unpiped would wedge the harness.
    """
    h.send("cat /dev/null")
    wait_prompt(h)

    h.send("ls /dev")
    # ls prints "- <size>  <name>"; "zero" appears nowhere in "ls /dev"
    h.expect(r"\n- +\d+ +zero\n", regex=True)
    wait_prompt(h)

    h.send("hexdump /dev/null")
    # an empty file dumps as nothing but the final offset
    h.expect(r"\n00000000\n", regex=True)
    wait_prompt(h)

    if have_pipes:
        h.send("cat /dev/null | wc -c")
        h.expect("| wc -c")
        h.expect(r"\n *0\n", regex=True)
        wait_prompt(h)


# ---- kernel log and services ------------------------------------------


def t_dmesg(h):
    h.send("dmesg -n 5")
    # "[<ticks>] <level> <tag> (<pid>) <msg>" - the brackets and the
    # level word are unique to the log format
    h.expect(r"\n\[ *\d+\] (info|warn|error|debug) ", regex=True)
    wait_prompt(h)


def t_service_list(h):
    h.send("service list")
    h.expect("RESTARTS")            # column header, not in the command
    h.expect(r"\nlogger +\w+", regex=True)
    wait_prompt(h)


def t_service_lifecycle(h):
    h.send("service status readiness")
    h.expect(r"\n  state +exited\n", regex=True)
    h.expect("ready=/run/readiness.ready")
    wait_prompt(h)

    h.send("service start dependent")
    h.expect("ok start dependent (running)")
    wait_prompt(h)

    h.send("service status dependent")
    h.expect(r"\n  state +running\n", regex=True)
    h.expect("requires=readiness")
    wait_prompt(h)

    h.send("service reload dependent")
    h.expect("ok reload dependent (restarting pid")
    wait_prompt(h)
    h.send("sleep 1")
    wait_prompt(h)

    # A hard requirement disappearing must stop and fail its dependents.
    h.send("service stop readiness")
    h.expect("ok stop readiness (was not running)")
    wait_prompt(h)
    h.send("sleep 1")
    wait_prompt(h)

    h.send("service status dependent")
    h.expect(r"\n  state +failed\n", regex=True)
    wait_prompt(h)

    h.send("service reset-failed dependent")
    h.expect("ok reset-failed dependent (stopped)")
    wait_prompt(h)

    # Restore the one-shot requirement and prove its dependent can start
    # again after the failure state has been reset.
    h.send("service start readiness")
    h.expect("ok start readiness (running)")
    wait_prompt(h)
    h.send("sleep 1")
    wait_prompt(h)

    h.send("service start dependent")
    h.expect("ok start dependent (running)")
    wait_prompt(h)

    h.send("service stop dependent")
    h.expect("ok stop dependent (pid")
    wait_prompt(h)
    h.send("sleep 1")
    wait_prompt(h)

    h.send("service status dependent")
    h.expect(r"\n  state +stopped\n", regex=True)
    wait_prompt(h)

    h.send("service reset-failed dependent")
    h.expect("ok reset-failed dependent (stopped)")
    wait_prompt(h)

    # Reload installs configuration but must not turn an inactive unit back
    # on merely because it was manually enabled earlier in the boot.
    h.send("service reload dependent")
    h.expect("ok reload dependent (stopped)")
    wait_prompt(h)

    # A service which never publishes ready= must be killed at its deadline
    # and remain failed until reset-failed.
    h.send("service start readiness-fail")
    h.expect("err start readiness-fail (failed)")
    wait_prompt(h)
    h.send("service status readiness-fail")
    h.expect(r"\n  state +failed\n", regex=True)
    wait_prompt(h)
    h.send("service reset-failed readiness-fail")
    h.expect("ok reset-failed readiness-fail (stopped)")
    wait_prompt(h)


def t_lsmod(h):
    """Loadable modules are landing in this wave; tolerate their absence.

    The existence check is `ls /bin/lsmod` rather than running lsmod, so
    a build without the module tooling SKIPs instead of colliding with
    the shell's "command not found" line.
    """
    h.send("ls /bin/lsmod")
    got = h.expect_any([r"\n1 entry\n", r"ls: cannot access"], regex=True)
    wait_prompt(h)
    if got != r"\n1 entry\n":
        return "SKIP"

    h.send("lsmod")
    h.expect_any([r"sh: command not found",
                  r"no modules loaded",
                  r"\n(Module|MODULE|module|name) +\w",
                  r"\n[a-z0-9_.-]+ +\d+ +\d+",
                  r"lsmod: [^\n]+"], regex=True)
    wait_prompt(h)


# ---- packages ---------------------------------------------------------


def t_kpkg_list(h):
    h.send("kpkg list")
    h.expect_any([r"kpkg: no packages installed",
                  r"\n\d+ packages? installed\n"], regex=True)
    wait_prompt(h)


def t_kpkg_install(h):
    """Install `hello`, which pulls kestrel-extras in as a dependency."""
    h.send("kpkg install hello")
    h.expect("kpkg: installed kestrel-extras", timeout=PKG_TIMEOUT)
    h.expect("kpkg: installed hello", timeout=PKG_TIMEOUT)
    wait_prompt(h, timeout=PKG_TIMEOUT)

    h.send("kpkg list")
    h.expect(r"\n2 packages installed\n", regex=True, timeout=PKG_TIMEOUT)
    wait_prompt(h, timeout=PKG_TIMEOUT)


def t_pkg_hello(h):
    h.send("hello e2e")
    h.expect("Hello, e2e!")
    wait_prompt(h)


def t_pkg_cal(h):
    h.send("cal 7 2026")
    h.expect("July 2026")           # the command line says "7", not "July"
    h.expect("Su Mo Tu We Th Fr Sa")
    h.expect(r"\n[ 0-9]*\b31\b[ 0-9]*\n", regex=True)   # July has 31 days
    wait_prompt(h)


def t_pkg_factor(h):
    h.send("factor 97 360")
    h.expect(r"\n97: 97\n", regex=True)
    # No leading "\n": the previous match consumed the newline these two
    # lines share. "360: 2" cannot occur in the echoed "factor 97 360".
    h.expect(r"360: 2 2 2 3 3 5\n", regex=True)
    wait_prompt(h)


def t_kpkg_verify(h):
    h.send("kpkg verify")
    h.expect("kpkg: everything matches", timeout=PKG_TIMEOUT)
    wait_prompt(h, timeout=PKG_TIMEOUT)


# ---- browser ----------------------------------------------------------


def _validate_tls_negative_block(block, attempt):
    """Validate one complete controlled certificate-rejection transcript."""
    if TLS_NEGATIVE_BODY_MARKER in block:
        raise TimeoutError(
            "certificate-negative attempt %d rendered the server body"
            % attempt)

    forbidden = TLS_NEGATIVE_FORBIDDEN_RE.search(block)
    if forbidden:
        raise TimeoutError(
            "certificate-negative attempt %d contained forbidden output %r"
            % (attempt, forbidden.group(0)))

    exits = TLS_NEGATIVE_EXIT_RE.findall(block)
    if exits != ["1"]:
        raise TimeoutError(
            "certificate-negative attempt %d shell exits were %r, not ['1']"
            % (attempt, exits))

    statuses = TLS_NEGATIVE_STATUS_RE.findall(block)
    if statuses != ["1"]:
        raise TimeoutError(
            "certificate-negative attempt %d status lines were %r, "
            "not ['1']" % (attempt, statuses))

    for line in block.splitlines():
        if (TLS_NEGATIVE_DIAGNOSTIC_RE.search(line) and
                TLS_NEGATIVE_REASON_RE.search(line)):
            return line.strip()
    raise TimeoutError(
        "certificate-negative attempt %d lacked a certificate/hostname "
        "diagnostic containing the SAN or an equivalent trust reason"
        % attempt)


def t_browser_text(h):
    """The browser pipeline, run over a styled local page in text mode.

    rootfs/doc/test.html exercises headings, wrapped body text, inline
    styles, an author stylesheet, entities, lists, <pre>, a table, links
    and an <img> alt.  Text mode cannot expose its color declaration, but
    it does prove that the styled document remains readable and that both
    relative and absolute links survive the shared pipeline.
    """
    h.send("browser -t -l /doc/test.html; "
           "echo BROWSER-LOCAL-STATUS-$?")
    h.expect("Kestrel Renderer Test", timeout=WALK_TIMEOUT)
    h.expect("CSS-AUTHOR-OK", timeout=WALK_TIMEOUT)
    h.expect("RENDERER-OK", timeout=WALK_TIMEOUT)
    h.expect("bullet-alpha", timeout=WALK_TIMEOUT)
    h.expect("PRE-BLOCK", timeout=WALK_TIMEOUT)
    h.expect("cell-body-b", timeout=WALK_TIMEOUT)
    h.expect("[ALT-TEXT]", timeout=WALK_TIMEOUT)
    h.expect("END-OF-PAGE", timeout=WALK_TIMEOUT)
    # -l resolves every href against the page URL
    h.expect(r"\n *\[1\] /doc/welcome\.md\n", regex=True,
             timeout=WALK_TIMEOUT)
    # The previous match consumed the newline shared by these two lines.
    h.expect(r" *\[2\] http://example\.com/\n", regex=True,
             timeout=WALK_TIMEOUT)
    h.expect(r"(?:^|\n)BROWSER-LOCAL-STATUS-0\n", regex=True,
             timeout=WALK_TIMEOUT)
    wait_prompt(h, timeout=WALK_TIMEOUT)


def t_browser_home(h):
    """The start page the desktop's Browser button opens.

    Regression guard: `browser` with no argument used to print usage and
    exit 2, so clicking Browser on the desktop opened nothing at all. The
    windowed no-argument case cannot be driven headlessly, so this checks
    the two things that break it -- the page existing and rendering.
    """
    # Short phrases only: the renderer wraps at 78 columns, so anything
    # long enough to straddle a line break would match nothing.
    h.send("browser -t /doc/home.html; echo BROWSER-HOME-STATUS-$?")
    h.expect("KestrelOS", timeout=WALK_TIMEOUT)
    h.expect("HTML parser", timeout=WALK_TIMEOUT)
    h.expect("Local pages", timeout=WALK_TIMEOUT)
    h.expect("TLS 1.3", timeout=WALK_TIMEOUT)
    h.expect(r"(?:^|\n)BROWSER-HOME-STATUS-0\n", regex=True,
             timeout=WALK_TIMEOUT)
    wait_prompt(h, timeout=WALK_TIMEOUT)


def t_browser_http_controlled(h):
    """Plain HTTP through /bin/browser against a deterministic host page."""
    if not browser_fixtures:
        raise TimeoutError("controlled browser servers were not started")
    end = "BROWSER-HTTP-END"
    h.send("/bin/browser -t %s; echo BROWSER-HTTP-STATUS-$?; echo %s"
           % (browser_fixtures.http_url, end))
    block = h.capture_until(r"\n%s\n" % end, timeout=NET_TIMEOUT, regex=True)
    if HTTP_BODY_MARKER not in block:
        raise TimeoutError("controlled HTTP body marker was absent")
    statuses = re.findall(
        r"(?m)^BROWSER-HTTP-STATUS-(-?\d+)$", block)
    if statuses != ["0"]:
        raise TimeoutError(
            "controlled HTTP status lines were %r, not ['0']" % statuses)
    wait_prompt(h, timeout=NET_TIMEOUT)


def t_browser_tls_certificate_negative(h):
    """Reject a parseable self-signed TLS 1.3 certificate ten times."""
    if not browser_fixtures:
        raise TimeoutError("controlled browser servers were not started")
    initial_gets = browser_fixtures.https.get_count
    if initial_gets != 0:
        raise TimeoutError(
            "certificate-negative HTTPS handler began with %d GET requests"
            % initial_gets)

    for attempt in range(1, TLS_NEGATIVE_REPETITIONS + 1):
        begin = "BROWSER-TLS-NEG-BEGIN-%02d" % attempt
        end = "BROWSER-TLS-NEG-END-%02d" % attempt
        h.send("echo %s; /bin/browser -t %s; "
               "echo BROWSER-TLS-NEG-STATUS-$?; echo %s"
               % (begin, browser_fixtures.https_url, end))
        block = h.capture_until(
            r"\n%s\n" % re.escape(end),
            timeout=NET_TIMEOUT, regex=True)
        # Capture rather than merely consume the prompt so a late process or
        # kernel fault between the end marker and prompt is also inspected.
        block += h.capture_until(
            SHELL_PROMPT, timeout=NET_TIMEOUT, regex=True)

        begins = list(re.finditer(
            r"(?m)^%s$" % re.escape(begin), block))
        ends = re.findall(r"(?m)^%s$" % re.escape(end), block)
        if len(begins) != 1 or len(ends) != 1:
            raise TimeoutError(
                "certificate-negative attempt %d did not produce one "
                "standalone begin/end marker" % attempt)
        # Inspect the entire capture for safety signatures, including output
        # that arrived just before this attempt's begin marker.  Keep status
        # and diagnostic matching bounded to the unique attempt below.
        if TLS_NEGATIVE_BODY_MARKER in block:
            raise TimeoutError(
                "certificate-negative attempt %d capture contained the "
                "server body" % attempt)
        forbidden = TLS_NEGATIVE_FORBIDDEN_RE.search(block)
        if forbidden:
            raise TimeoutError(
                "certificate-negative attempt %d capture contained "
                "forbidden output %r" % (attempt, forbidden.group(0)))
        attempt_block = block[begins[0].end():]
        diagnostic = _validate_tls_negative_block(attempt_block, attempt)

        current_gets = browser_fixtures.https.get_count
        if current_gets != initial_gets or current_gets != 0:
            raise TimeoutError(
                "certificate-negative HTTPS handler received %d GET "
                "requests by attempt %d" % (current_gets, attempt))
        print("  TLS negative %02d/%02d: [exit 1], "
              "BROWSER-TLS-NEG-STATUS-1, HTTPS GETs=0; %s"
              % (attempt, TLS_NEGATIVE_REPETITIONS, diagnostic))

    # Give a worker released by the final connection a scheduling turn, then
    # take the sequence-wide snapshot used in the acceptance record.
    time.sleep(0.05)
    final_gets = browser_fixtures.https.get_count
    if final_gets != initial_gets or final_gets != 0:
        raise TimeoutError(
            "certificate-negative HTTPS handler GET count changed from "
            "%d to %d" % (initial_gets, final_gets))
    print("  TLS negative summary: %d/%d rejected; HTTPS handler GETs=%d"
          % (TLS_NEGATIVE_REPETITIONS, TLS_NEGATIVE_REPETITIONS,
             final_gets))


def t_browser_https(h):
    """The browser itself must fetch and verify a public HTTPS page.

    This is deliberately not a SKIP-on-network-failure probe: verified
    HTTPS through /bin/browser is a Wave 2 acceptance requirement.  Watch
    for an error as well as the success marker so a readable fast failure
    is reported immediately instead of burning the whole timeout.
    """
    h.send("/bin/browser -t https://example.com/; "
           "echo BROWSER-HTTPS-STATUS-$?")
    block = h.capture_until(
        r"(?:^|\n)BROWSER-HTTPS-STATUS-\d+\n",
        timeout=NET_TIMEOUT, regex=True)
    if "Example Domain" not in block:
        raise TimeoutError("browser HTTPS success marker was absent")
    if not re.search(r"(?:^|\n)BROWSER-HTTPS-STATUS-0\n", block):
        raise TimeoutError("browser HTTPS did not report explicit status 0")
    if re.search(r"(?i)\n(?:browser:|cannot load page|"
                 r"certificate(?: |:)|network unavailable|"
                 r"dns lookup failed|cannot connect)[^\n]*\n", block):
        raise TimeoutError("browser HTTPS output also contained a load error")
    wait_prompt(h, timeout=NET_TIMEOUT)


def t_tcp_curl(h):
    """A real TCP fetch. SKIPs like the other network tests: the host may
    have no route out, and that is not a KestrelOS bug."""
    ok = r"(?i)</html>"
    h.send("curl -s http://example.com")
    got = h.expect_any([ok,
                        r"curl: network unavailable",
                        r"curl: cannot resolve",
                        r"curl: cannot connect",
                        r"curl: this build has no HTTP client",
                        r"curl: HTTP \d+",
                        r"\[exit \d+\]"],
                       timeout=NET_TIMEOUT, regex=True)
    wait_prompt(h, timeout=NET_TIMEOUT)
    if got != ok:
        return "SKIP"


TESTS = [
    ("boot", t_boot),
    ("login", t_login),
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
    ("whoami", t_whoami),
    ("id", t_id),
    ("redirect", t_redirect),
    ("pipe", t_pipe),
    ("dev", t_dev),
    ("dmesg", t_dmesg),
    ("service-list", t_service_list),
    ("service-lifecycle", t_service_lifecycle),
    ("permissions", t_permissions),
    ("browser-text", t_browser_text),
    ("browser-home", t_browser_home),
    ("browser-http-controlled", t_browser_http_controlled),
    ("browser-tls-cert-negative", t_browser_tls_certificate_negative),
    ("browser-https", t_browser_https),
    ("kpkg-list", t_kpkg_list),
    ("kpkg-install", t_kpkg_install),
    ("pkg-hello", t_pkg_hello),
    ("pkg-cal", t_pkg_cal),
    ("pkg-factor", t_pkg_factor),
    ("kpkg-verify", t_kpkg_verify),
    ("lsmod", t_lsmod),
    ("tcp-curl", t_tcp_curl),
]

# boot + login + a prompt: the least that proves the machine came up.
SMOKE_TESTS = 3


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
        h.send("echo capture-marker")
        block = h.capture_until(r"capture-marker\n", timeout=10, regex=True)
        if "capture-marker" not in block:
            raise TimeoutError("capture_until lost the matched output")
        h.expect_any([r"kestrel:[^\n]*\$"], timeout=10, regex=True)
        h.send("quit")
    except TimeoutError as e:
        print("FAIL selftest: %s" % e)
        h.print_tail()
        return False
    finally:
        h.kill()

    # A prior consuming assertion can eat the newline shared with the next
    # standalone status line.  The status contract must accept both that
    # buffer-start case and the ordinary newline-delimited case, while
    # rejecting the same marker embedded in the shell's echoed command.
    status = r"(?:^|\n)BROWSER-SELFTEST-STATUS-0\n"

    at_start = Harness([])
    at_start.buf = "preceding-line\nBROWSER-SELFTEST-STATUS-0\n"
    at_start.expect("preceding-line\n", timeout=0)
    at_start.expect(status, timeout=0, regex=True)

    after_newline = Harness([])
    after_newline.buf = "unconsumed-line\nBROWSER-SELFTEST-STATUS-0\n"
    after_newline.expect(status, timeout=0, regex=True)

    echoed = Harness([])
    echoed.buf = (
        "kestrel:/$ echo BROWSER-SELFTEST-STATUS-0\n"
        "kestrel:/$ "
    )
    try:
        echoed.expect(status, timeout=0, regex=True)
    except TimeoutError:
        pass
    else:
        print("FAIL selftest: browser status matched echoed command text")
        return False

    valid_tls_negative = (
        "BROWSER-TLS-NEG-BEGIN-01\n"
        "browser: the certificate is for 'kestrel-negative.invalid', "
        "not '10.0.2.2'\n"
        "[exit 1]\n"
        "BROWSER-TLS-NEG-STATUS-1\n"
        "BROWSER-TLS-NEG-END-01\n"
        "kestrel:/$"
    )
    try:
        _validate_tls_negative_block(valid_tls_negative, 1)
        _validate_tls_negative_block(
            "browser: no trusted root certificate issued 'fixture'\n"
            "[exit 1]\n"
            "BROWSER-TLS-NEG-STATUS-1\n", 2)
    except TimeoutError as e:
        print("FAIL selftest: valid TLS negative transcript rejected: %s" % e)
        return False

    invalid_tls_negative = [
        ("body marker", valid_tls_negative.replace(
            "[exit 1]\n", TLS_NEGATIVE_BODY_MARKER + "\n[exit 1]\n")),
        ("uproc", valid_tls_negative.replace(
            "[exit 1]\n", "uproc: pid 7 terminated\n[exit 1]\n")),
        ("fault", valid_tls_negative.replace(
            "[exit 1]\n", "page fault at 0x18\n[exit 1]\n")),
        ("exception", valid_tls_negative.replace(
            "[exit 1]\n", "exception 14\n[exit 1]\n")),
        ("kernel panic", valid_tls_negative.replace(
            "[exit 1]\n", "kernel panic\n[exit 1]\n")),
        ("exit -1", valid_tls_negative.replace("[exit 1]", "[exit -1]")),
        ("status -1", valid_tls_negative.replace(
            "BROWSER-TLS-NEG-STATUS-1",
            "BROWSER-TLS-NEG-STATUS--1")),
        ("generic no error", valid_tls_negative.replace(
            "[exit 1]\n", "browser: no error\n[exit 1]\n")),
        ("generic certificate text", valid_tls_negative.replace(
            "the certificate is for 'kestrel-negative.invalid', "
            "not '10.0.2.2'",
            "certificate verification failed")),
        ("non-standalone exit", valid_tls_negative.replace(
            "[exit 1]", "shell said [exit 1]")),
        ("wrong positive status", valid_tls_negative.replace(
            "BROWSER-TLS-NEG-STATUS-1",
            "BROWSER-TLS-NEG-STATUS-2")),
    ]
    for name, transcript in invalid_tls_negative:
        try:
            _validate_tls_negative_block(transcript, 99)
        except TimeoutError:
            continue
        print("FAIL selftest: TLS negative validator accepted %s" % name)
        return False
    print("PASS selftest: TLS negative validator accepted 2 valid and "
          "rejected %d invalid transcripts" % len(invalid_tls_negative))

    print("PASS selftest")
    return True


def fixture_selftest():
    """Prove both controlled browser endpoints without booting an image."""
    fixtures = None
    try:
        fixtures = BrowserFixtureServers()
        fixtures.start()
        if fixtures.http.get_count != 0 or fixtures.https.get_count != 0:
            raise RuntimeError("fixture GET counters did not start at zero")

        plain = http.client.HTTPConnection(
            "127.0.0.1", fixtures.http.server_address[1], timeout=5)
        plain.request("GET", "/plain")
        body = plain.getresponse().read().decode("ascii")
        plain.close()
        if HTTP_BODY_MARKER not in body:
            raise RuntimeError("plain HTTP marker missing")
        if fixtures.http.get_count != 1:
            raise RuntimeError("plain HTTP GET count is %d, expected 1"
                               % fixtures.http.get_count)
        if fixtures.https.get_count != 0:
            raise RuntimeError("HTTPS GET count changed before TLS test")

        strict = http.client.HTTPSConnection(
            "127.0.0.1", fixtures.https.server_address[1], timeout=5)
        try:
            strict.request("GET", "/negative")
            strict.getresponse().read()
        except ssl.SSLCertVerificationError:
            pass
        else:
            raise RuntimeError("strict TLS accepted the self-signed cert")
        finally:
            strict.close()
        if fixtures.https.get_count != 0:
            raise RuntimeError(
                "strict certificate rejection reached HTTPS handler "
                "(GET count %d)" % fixtures.https.get_count)

        insecure = http.client.HTTPSConnection(
            "127.0.0.1", fixtures.https.server_address[1], timeout=5,
            context=ssl._create_unverified_context())
        insecure.request("GET", "/negative")
        body = insecure.getresponse().read().decode("ascii")
        insecure.close()
        if TLS_NEGATIVE_BODY_MARKER not in body:
            raise RuntimeError("TLS negative body marker missing")
        if fixtures.https.get_count != 1:
            raise RuntimeError("insecure HTTPS GET count is %d, expected 1"
                               % fixtures.https.get_count)
    except (OSError, RuntimeError, ssl.SSLError) as e:
        print("FAIL fixture-selftest: %s" % e)
        return False
    finally:
        if fixtures:
            fixtures.close()
    print("PASS fixture-selftest: HTTP GETs=1; TLS 1.3 strict rejection "
          "GETs=0; insecure HTTPS GETs=1 and body marker served")
    return True


def main():
    global browser_fixtures

    ap = argparse.ArgumentParser(description="KestrelOS e2e test harness")
    ap.add_argument("--smoke", action="store_true",
                    help="run boot + prompt tests only")
    ap.add_argument("--list", action="store_true",
                    help="list tests and exit")
    ap.add_argument("--selftest", action="store_true",
                    help="test harness plumbing without an OS image")
    ap.add_argument("--fixtures-selftest", action="store_true",
                    help="test controlled HTTP/TLS servers without an image")
    ap.add_argument("--nic", choices=sorted(QEMU_NICS), default="rtl8139",
                    help="emulated NIC to test (default: rtl8139)")
    args = ap.parse_args()

    if args.list:
        for name, _ in TESTS:
            print(name)
        return 0

    if args.selftest:
        return 0 if selftest() else 1
    if args.fixtures_selftest:
        return 0 if fixture_selftest() else 1

    if not os.path.exists("build/os.img"):
        print("error: build/os.img not found (run make first)")
        return 1

    tests = TESTS[:SMOKE_TESTS] if args.smoke else TESTS
    fixtures = None
    if not args.smoke:
        try:
            fixtures = BrowserFixtureServers()
            fixtures.start()
            browser_fixtures = fixtures
        except (OSError, RuntimeError, ssl.SSLError) as e:
            print("error: cannot start controlled browser fixtures: %s" % e)
            if fixtures:
                fixtures.close()
            return 1

    h = Harness(qemu_command(args.nic))
    try:
        h.start()
    except FileNotFoundError:
        print("error: qemu-system-x86_64 not found in PATH")
        if fixtures:
            fixtures.close()
            browser_fixtures = None
        return 1
    try:
        results = run_tests(h, tests)
    finally:
        h.kill()
        if fixtures:
            fixtures.close()
            browser_fixtures = None
    return 0 if summarize(results) else 1


if __name__ == "__main__":
    sys.exit(main())
