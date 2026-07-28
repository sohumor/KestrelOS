# KestrelOS end-to-end testing

`tools/e2e.py` boots the built OS image (`build/os.img`) in headless QEMU
and drives the shell over the serial console. It is pure Python 3 stdlib —
no dependencies beyond `python3` and `qemu-system-x86_64` on PATH.

## Usage

```sh
make                       # build build/os.img first
python3 tools/e2e.py       # full test sequence
python3 tools/e2e.py --smoke     # boot + shell prompt only
python3 tools/e2e.py --nic e1000 # full suite through the Intel NIC driver
python3 tools/e2e.py --list      # list test names
python3 tools/e2e.py --selftest  # verify harness plumbing (no image needed)
```

Run from the repo root (paths are relative to the cwd).

Exit code 0 means all tests passed (SKIPs allowed); 1 means a failure.
On failure the harness prints the last 40 lines of serial output.

The TCP receive reassembler and Internet checksum code also have fast
host-side sanitizer suites. KFS3 has a separate deterministic crash-injection
suite for journal replay:

```sh
make test-net
make test-kfs
```

They cover reordered and overlapping TCP segments, duplicate data,
receive-window clipping, ring wrap, application reads while a hole is
buffered, 32-bit sequence-number wraparound, known IPv4/UDP checksum vectors,
pseudo-header binding, odd payload lengths, and corruption rejection.
The journal suite covers committed replay, home-block installation,
idempotence, torn transactions, journal clearing, and unsafe targets.
The boot-recovery test injects a committed transaction into a temporary
KFS image, boots it through the real kernel mount path, shuts down cleanly,
then verifies the home block and cleared journal directly on the disk.

## How it works

- QEMU is launched with `-display none -serial stdio`, so the OS serial
  console is attached to the harness's stdin/stdout pipes.
- `--nic rtl8139` (the default) and `--nic e1000` run the same suite through
  each supported NIC driver. `make test-e1000` is the e1000 shorthand.
- QEMU snapshot mode keeps all guest disk writes in a temporary overlay.
  Tests can install packages and create files without modifying
  `build/os.img`, so repeated runs start from the same filesystem state.
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
| uptime        | `uptime`                   | `up H:MM:SS` or `<n> ms`         |
| calc          | `calc 2*(3+4)`             | a line containing exactly `14`   |
| calc-divzero  | `calc 1/0`                 | `divide by zero` (no crash)      |
| cp-wc         | `cp /etc/version /t2.txt`, `wc -c /t2.txt` | `<count> /t2.txt` |
| mv-rm         | `mv /t2.txt /t3.txt`, `cat`, `rm`, `cat` | `KestrelOS 0.1.0`, then `cat: cannot open` |
| mkdir-ls      | `mkdir /d1`, `ls /`        | a `d` row named `d1`             |
| grep          | `grep -n Kestrel /etc/motd`| `<lineno>:...KestrelOS`          |
| head          | `head -n 1 /doc/welcome.md`| `# Welcome to KestrelOS` then the prompt |
| tail          | `tail -n 1 /doc/welcome.md`| `...for the release.` then the prompt |
| tree          | `tree /etc`                | `<n> directories, <n> files` within 10 s |
| du            | `du /etc`                  | `<bytes>  /etc` within 10 s      |
| find          | `find /etc`                | `/etc/version` within 10 s       |
| date          | `date`                     | `YYYY-MM-DD`; SKIP on `no clock` |
| err-cat-missing | `cat /nope`              | `cat: cannot open`, back to prompt |
| err-rm-missing  | `rm /nope`               | `rm: cannot remove`, back to prompt |
| err-unknown-cmd | `notacommand42`          | `sh: command not found`, back to prompt |
| long-line     | 300 junk chars (no spaces) | `sh: command not found` or `sh: too many tokens`, back to prompt |
| service-lifecycle | start, inspect, reload, stop, and reset a hard-dependent service | readiness, dependency, restart, and stopped-state transitions |

Network tests are fail-soft: if the OS prints `network unavailable`
they count as SKIP, not FAIL. `date` is fail-soft the same way when the
kernel reports no RTC.

### Filesystem ordering

Tests that mutate the filesystem run in a fixed order and clean up after
themselves as they go: `cp-wc` creates `/t2.txt`, `mv-rm` renames it to
`/t3.txt` and then deletes it, `mkdir-ls` creates `/d1`. Later tests only
read `/etc` and `/doc`, which nothing writes to. Insert new mutating
tests after `mkdir-ls`, or make them self-contained.

### Recursion guard

`tree`, `du` and `find` use a tighter 10 s `WALK_TIMEOUT` instead of the
20 s default. A runaway-recursion regression in a directory walker shows
up as a timeout on these steps rather than a wedged harness.

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

## Writing a new test

Each test is a `def t_<name>(h)` plus an entry in `TESTS`. The body is
`send` / `expect` / `wait_prompt`, and returning the string `"SKIP"`
marks the test skipped (used for fail-soft cases).

Every `expect()` pattern **must be unique to the command's output**.
`expect` consumes the buffer through the match, so a pattern that also
matches the prompt `kestrel:/$` or the shell's local echo of the command
line will swallow it and desync every later test. This has bitten us
before: a bare `s` in the `uptime` test matched the prompt.

Rules of thumb:

- Anchor numeric output with surrounding newlines: `r"\n14\n"`, not `14`.
- Never expect a substring that also appears in the command you sent —
  the shell echoes typed characters back over the serial line.
- Prefer app-specific error prefixes (`cat: cannot open`,
  `sh: command not found`) over generic words like `error`.
- To assert "and nothing else was printed", require the prompt inside
  the same pattern (`r"# Welcome to KestrelOS\nkestrel:[^\n]*\$"`) and
  skip the trailing `wait_prompt`.
- Non-zero exits make the shell print `[exit N]` before the next prompt;
  that text sits between your match and the prompt and is consumed
  harmlessly by `wait_prompt`.
