# KestrelOS end-to-end testing

`tools/e2e.py` boots the built OS image (`build/os.img`) in headless QEMU
and drives the shell over the serial console. It is pure Python 3 stdlib —
no dependencies beyond `python3` and `qemu-system-x86_64` on PATH.

## Usage

```sh
make                       # build build/os.img first
python3 tools/e2e.py       # full test sequence
python3 tools/e2e.py --smoke     # boot + shell prompt only
python3 tools/e2e.py --list      # list test names
python3 tools/e2e.py --selftest  # verify harness plumbing (no image needed)
```

Run from the repo root (paths are relative to the cwd).

Exit code 0 means all tests passed (SKIPs allowed); 1 means a failure.
On failure the harness prints the last 40 lines of serial output.

## How it works

- QEMU is launched with `-display none -serial stdio`, so the OS serial
  console is attached to the harness's stdin/stdout pipes.
- A reader thread accumulates output; `expect(pattern, timeout)` strips
  ANSI escape sequences and `\r` before matching (default 20 s per step,
  30 s for boot).
- `send(line)` writes the line plus `\n` to the guest.
- QEMU is killed reliably via an `atexit` hook: `terminate()`, then
  `kill()` after a 5 s grace period.
- Once a test fails, remaining tests are reported as SKIP.

## Test sequence

| test          | drives                     | expects                          |
|---------------|----------------------------|----------------------------------|
| boot          | (nothing)                  | `KESTREL READY` within 30 s      |
| shell-prompt  | (nothing)                  | `kestrel:/$`                     |
| help          | `help`                     | `commands`                       |
| echo          | `echo hello world`         | `hello world`                    |
| ls-bin        | `ls /bin`                  | `sh` and `ls`                    |
| cat-motd      | `cat /etc/motd`            | `Kestrel`                        |
| fs-roundtrip  | `writefile /tmp1.txt`, data line, Ctrl-D, `cat /tmp1.txt` | `roundtrip-data-123` |
| ps            | `ps`                       | `sh`                             |
| free          | `free`                     | `MiB` or `KiB`                   |
| ping          | `ping 10.0.2.2`            | `reply`/`rtt`; SKIP on `network unavailable` |
| nslookup      | `nslookup example.com`     | dotted-quad IP; SKIP on `network unavailable` |
| uptime        | `uptime`                   | `ms` or `s`                      |

Network tests are fail-soft: if the OS prints `network unavailable`
they count as SKIP, not FAIL.

## Makefile wiring

`make test` builds the image and runs the harness:

```make
test: all
	python3 tools/e2e.py
```

## Marker contract

The harness matches literal markers the OS prints (`KESTREL READY`,
prompt format `kestrel:<cwd>$`, `network unavailable`, etc.). If shell
output drifts, update either the shell or the expectations in
`tools/e2e.py` — they must agree.
