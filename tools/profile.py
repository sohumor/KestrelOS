#!/usr/bin/env python3
"""Host-side serial phase timing and QMP statistical guest profiling."""

import argparse
import bisect
import collections
import json
import os
from pathlib import Path
import re
import shlex
import socket
import subprocess
import sys
import threading
import time

import e2e

ROOT = Path(__file__).resolve().parent.parent


def registers(output):
    samples = []
    for block in re.split(r"CPU#\d+", output):
        rip = re.search(r"\b(?:RIP|EIP|IP)=([0-9a-fA-F]+)", block)
        cs = re.search(r"\bCS\s*=\s*([0-9a-fA-F]+)", block)
        hlt = re.search(r"\bHLT=([01])", block)
        if rip and cs and hlt:
            samples.append((int(rip[1], 16), int(cs[1], 16) & 3,
                            hlt[1] == "1"))
    if not samples:
        raise ValueError("QMP info registers returned no CPU states")
    return samples


class Symbols:
    def __init__(self, path):
        result = subprocess.run([os.environ.get("NM", "nm"), "-n", "-S",
                                 "--defined-only", str(path)],
                                check=True, capture_output=True, text=True)
        self.entries = []
        symbols = []
        for line in result.stdout.splitlines():
            fields = line.split()
            if len(fields) == 4:
                symbols.append((int(fields[0], 16), int(fields[1], 16), fields[2], fields[3]))
            elif len(fields) == 3:
                symbols.append((int(fields[0], 16), 0, fields[1], fields[2]))
        boundaries = sorted({entry[0] for entry in symbols})
        for start, size, kind, name in symbols:
            if kind in ("t", "T", "w", "W"):
                following = bisect.bisect_right(boundaries, start)
                end = start + size if size else (boundaries[following]
                      if following < len(boundaries) else start + 1)
                self.entries.append((start, end, name))
        self.entries.sort()
        self.starts = [entry[0] for entry in self.entries]

    def lookup(self, rip):
        index = bisect.bisect_right(self.starts, rip) - 1
        if index >= 0:
            start, end, name = self.entries[index]
            if start <= rip < end:
                return name
        return None


class QMP:
    def __init__(self, port, timeout):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout)
        self.file = self.sock.makefile("rwb")
        greeting = self.read()
        if "QMP" not in greeting:
            raise ValueError("missing QMP greeting")
        self.execute("qmp_capabilities")

    def read(self):
        line = self.file.readline()
        if not line:
            raise EOFError("QMP connection closed")
        return json.loads(line)

    def execute(self, command, arguments=None):
        self.file.write((json.dumps({"execute": command,
                                    "arguments": arguments or {}}) + "\n").encode())
        self.file.flush()
        while True:
            reply = self.read()
            if "error" in reply:
                raise RuntimeError(str(reply["error"]))
            if "return" in reply:
                return reply["return"]

    def close(self):
        self.file.close()
        self.sock.close()


class Serial:
    def __init__(self, proc, stamps):
        self.proc, self.stamps = proc, stamps
        self.condition = threading.Condition()
        self.buffer = ""
        self.eof = False
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        while True:
            chunk = self.proc.stdout.read1(4096)
            with self.condition:
                if not chunk:
                    self.eof = True
                    self.condition.notify_all()
                    return
                now = time.monotonic()
                self.stamps.setdefault("first serial byte", now)
                self.buffer += chunk.decode("utf-8", errors="replace")
                for label, marker in (("KESTREL READY", "KESTREL READY"),
                                      ("login prompt", "\nlogin: ")):
                    if marker in self.buffer:
                        self.stamps.setdefault(label, now)
                self.condition.notify_all()

    def expect(self, pattern, timeout):
        deadline = time.monotonic() + timeout
        with self.condition:
            while True:
                match = re.search(pattern, e2e.clean(self.buffer))
                if match:
                    # Store cleaned text so match offsets remain valid.
                    self.buffer = e2e.clean(self.buffer)[match.end():]
                    return
                remaining = deadline - time.monotonic()
                if self.eof or remaining <= 0:
                    raise TimeoutError("waiting for %r; serial tail: %s" %
                                       (pattern, self.buffer[-1000:]))
                self.condition.wait(remaining)

    def send(self, command):
        self.proc.stdin.write((command + "\n").encode())
        self.proc.stdin.flush()


def collect(args):
    scale = float(os.environ.get("KESTREL_E2E_TIMEOUT_SCALE", "1"))
    if not 0 < scale < float("inf"):
        raise ValueError("KESTREL_E2E_TIMEOUT_SCALE must be positive and finite")
    stamps = {"QEMU start": time.monotonic()}
    counts = collections.Counter()
    functions = {"kernel": collections.Counter(), "user": collections.Counter()}
    kernel = Symbols(ROOT / "build/kernel.elf")
    candidates = list(args.user_elf)
    words = shlex.split(args.cmd)
    if words:
        binary = ROOT / "build/apps" / Path(words[0]).name
        if binary.is_file() and str(binary) not in candidates:
            candidates.insert(0, str(binary))
    users = [(Path(path).name, Symbols(path)) for path in candidates]
    # TCP QMP also works with Windows QEMU. QEMU binds the reserved port below.
    with socket.socket() as reserve:
        reserve.bind(("127.0.0.1", 0))
        port = reserve.getsockname()[1]
    command = e2e.qemu_command("rtl8139", 2) + [
        "-monitor", "none", "-S", "-qmp",
        "tcp:127.0.0.1:%d,server=on,wait=off" % port]
    proc = None
    qmp = None
    stop = threading.Event()
    active = threading.Event()
    errors = []
    sampler = None
    rounds = 0

    def sample():
        nonlocal rounds
        deadline = time.monotonic()
        try:
            while not stop.is_set():
                if active.is_set():
                    states = registers(qmp.execute("human-monitor-command", {
                        "command-line": "info registers -a"}))
                    rounds += 1
                    for rip, cpl, halted in states:
                        bucket = "idle" if halted else "kernel" if cpl == 0 else "user"
                        counts[bucket] += 1
                        if bucket == "idle":
                            continue
                        if bucket == "kernel":
                            name = kernel.lookup(rip) or "[kernel 0x%x]" % rip
                        else:
                            matches = ["%s:%s" % (label, name)
                                       for label, symbols in users
                                       if (name := symbols.lookup(rip))]
                            name = " | ".join(matches) or "[user 0x%x]" % rip
                        functions[bucket][name] += 1
                deadline = max(deadline + 1 / args.hz, time.monotonic())
                stop.wait(max(0, deadline - time.monotonic()))
        except Exception as exc:
            errors.append(exc)

    try:
        stamps["QEMU start"] = time.monotonic()
        proc = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        serial = Serial(proc, stamps)
        deadline = time.monotonic() + 30 * scale
        while True:
            try:
                qmp = QMP(port, 5 * scale)
                break
            except OSError:
                if proc.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("QEMU failed to open QMP: " +
                                       (proc.stderr.read().decode() if proc.poll() is not None else "timeout"))
                stop.wait(0.01)
        if args.window in ("boot", "all"):
            active.set()
        qmp.execute("cont")
        sampler = threading.Thread(target=sample, daemon=True)
        sampler.start()
        serial.expect("KESTREL READY", 30 * scale)
        serial.expect(e2e.LOGIN_PROMPT, 30 * scale)
        serial.send(e2e.ROOT_USER)
        serial.expect("password:", 20 * scale)
        serial.send(e2e.ROOT_PASS)
        serial.expect(e2e.SHELL_PROMPT, 30 * scale)
        stamps["shell prompt"] = time.monotonic()
        if args.window == "boot":
            active.clear()
        stamps["command start"] = time.monotonic()
        if args.window in ("cmd", "all"):
            active.set()
        serial.send(args.cmd)
        serial.expect(e2e.SHELL_PROMPT, 30 * scale)
        stamps["command end"] = time.monotonic()
        active.clear()
    finally:
        stop.set()
        if sampler:
            sampler.join(6 * scale)
        if proc:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5 * scale)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            for pipe in (proc.stdin, proc.stdout, proc.stderr):
                pipe.close()
        if qmp:
            qmp.close()
    if errors:
        raise RuntimeError("sampling failed: %s" % errors[0])
    total = sum(counts.values())
    phases = []
    previous = stamps["QEMU start"]
    for label, stamp in stamps.items():
        phases.append({"phase": label, "elapsed_ms": (stamp - stamps["QEMU start"]) * 1000,
                       "delta_ms": (stamp - previous) * 1000})
        previous = stamp
    return {"command": args.cmd, "window": args.window, "requested_hz": args.hz,
            "rounds": rounds, "total_samples": total, "phases": phases,
            "user_elfs": [str(path) for path in candidates],
            "counts": {key: {"samples": counts[key], "percent": 100 * counts[key] / total if total else 0}
                       for key in ("kernel", "user", "idle")},
            "functions": {key: [{"function": name, "samples": count,
                                 "percent": 100 * count / total if total else 0}
                                for name, count in counter.most_common(args.top)]
                          for key, counter in functions.items()}}


def render(report):
    lines = ["Guest profile: %s (window=%s, requested %.1f Hz)" %
             (report["command"], report["window"], report["requested_hz"]),
             "Phase                         elapsed ms     delta ms"]
    for phase in report["phases"]:
        lines.append("%-28s %11.3f %12.3f" %
                     (phase["phase"], phase["elapsed_ms"], phase["delta_ms"]))
    lines.append("Samples: %d CPU states in %d QMP rounds" %
                 (report["total_samples"], report["rounds"]))
    for bucket, count in report["counts"].items():
        lines.append("  %-6s %7d %6.2f%%" % (bucket, count["samples"], count["percent"]))
    for bucket, entries in report["functions"].items():
        lines.append("Top %s functions (%% of all CPU samples):" % bucket)
        for entry in entries:
            lines.append("  %6.2f%% %7d %s" %
                         (entry["percent"], entry["samples"], entry["function"]))
        if not entries:
            lines.append("  (no samples)")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmd", default="ls /bin")
    parser.add_argument("--hz", type=float, default=200)
    parser.add_argument("--window", choices=("boot", "cmd", "all"), default="cmd")
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument("--user-elf", action="append", default=[])
    parser.add_argument("--json", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    if not 0 < args.hz < float("inf") or args.top < 1:
        parser.error("--hz must be positive and finite; --top must be positive")
    if not args.cmd.strip() or "\n" in args.cmd or "\r" in args.cmd:
        parser.error("--cmd must be one nonempty command line")
    try:
        report = collect(args)
        output = render(report)
        print(output, end="")
        if args.out:
            args.out.write_text(output)
        if args.json:
            args.json.write_text(json.dumps(report, indent=2) + "\n")
    except (OSError, ValueError, RuntimeError, EOFError, subprocess.SubprocessError) as exc:
        print("profile: %s" % exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
