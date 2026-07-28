# init, services and logging

`/bin/init` is PID 1. The kernel spawns it as the only userspace process
(`kernel/main.c`), and everything else on the machine is one of its
children. It reads `/etc/inittab`, brings the system up in a defined
order, supervises what it started, answers control requests from
`service`, and takes the machine down when `reboot` or `halt` ask it to.

Related programs, all in `/bin`:

| program  | what it does |
|----------|--------------|
| `init`   | PID 1: configuration, supervision, shutdown |
| `service`| inspect and control units (`list`, `status`, `start`, `stop`, `restart`, `reload`, `reset-failed`, `log`) |
| `logger` | supervised service: copies the kernel log ring into `/var/log/messages` |
| `dmesg`  | print the kernel log ring (`-n N`, `-f`) |
| `kill`   | terminate a process by pid |
| `reboot` | orderly restart through init, falling back to `SYS_POWER` |
| `halt`   | orderly stop through init, falling back to `SYS_POWER` |

## /etc/inittab

One directive per line. Blank lines are ignored. A `#` starts a comment
at the beginning of a line or after whitespace, so a value such as
`args=--tag=#1` is preserved.

```
sysinit <path> [args...]    run to completion, in file order, before
                            anything else starts
service <name>              start the service described by
                            /etc/services/<name>.svc
respawn <path> [args...]    keep it running: restart it whenever it exits
once    <path> [args...]    start it once, do not supervise it
```

Any directive may be prefixed with a condition, which makes it easy to
switch behaviour on a marker file laid down by an installer or a test
harness:

```
if-exists  <path> <directive...>
if-missing <path> <directive...>
```

A program may be written as several alternatives separated by `|`. init
runs the first one that exists, which is how the console keeps working
before `/bin/login` is installed:

```
respawn /bin/login|/bin/sh
```

Up to 8 arguments are passed to a program; anything beyond that is
dropped. At most 24 units may be configured.

A unit's *name* is the service name for `service` directives, and the
basename of the chosen program otherwise (`/bin/login|/bin/sh` becomes
`login` or `sh` depending on which one was picked). A clashing name gets
a numeric suffix (`true`, `true2`, ...).

### Startup order

1. every `sysinit` unit, in file order, each run to completion;
2. services, in dependency order (see `after=`);
3. `once` and `respawn` units, in file order.

### If /etc/inittab is missing

init logs a warning and configures a single unit — `respawn
/bin/login|/bin/sh` — so the machine always comes up with a console. The
same fallback applies when the file parses but configures nothing.

## Service files: /etc/services/&lt;name&gt;.svc

`key=value`, one per line, same comment rule as the inittab. Whitespace
around the key and the value is trimmed. Unknown keys and lines without
an `=` are logged as warnings and skipped, so a typo degrades into a
message rather than a boot failure.

| key       | default | meaning |
|-----------|---------|---------|
| `name`    | —       | informational only; the unit name always follows the filename, and a mismatch is warned about |
| `exec`    | —       | absolute path of the program. **Required**: without it the unit is marked `failed` |
| `args`    | empty   | arguments, split on whitespace, at most 8 |
| `restart` | `always`| `always`, `on-failure`, or `never` |
| `respawn` | —       | compatibility spelling: `yes` = `restart=always`, `no` = `restart=never` |
| `after`   | none    | comma- or whitespace-separated ordering dependencies |
| `requires`| none    | hard dependencies, started first and enforced while this unit is active |
| `ready`   | none    | absolute marker path that must appear before startup succeeds |
| `timeout_ms` | `5000` | readiness deadline, from 100 through 60000 ms |
| `stdout`  | console | the program's fd 1 is appended to this file |
| `stderr`  | console | only honoured when it equals `stdout`; see below |
| `enabled` | `yes`   | `no` means "configured but not started" |

A value that is not a recognised boolean keeps the default and logs a
warning.

`stderr=` cannot be redirected independently: `SYS_SPAWN_IO` only
redirects fd 0 and fd 1, and fd 2 always stays on the console. A
`stderr=` that differs from `stdout=` is accepted, logged as
unsupported, and ignored.

### Dependencies and readiness

`after=` and `requires=` accept multiple service names separated by commas,
whitespace, or both. Both participate in topological startup ordering.
A missing `after=` target is only a bad ordering hint, so init warns and
continues. A missing or self-referential `requires=` target makes the unit
fail. Anything still unplaced when no further progress is possible is part
of a cycle and is marked `failed`; the algorithm always terminates.

Before starting a unit, init recursively starts every hard requirement.
A requirement is usable when it is running, or when it is a successful
`restart=never` one-shot. If a hard requirement later becomes unavailable,
init stops its active dependents and marks them failed. A control command
that removes a requirement publishes that dependent failure before its
acknowledgement; the terminating child remains supervised until it is reaped.

When `ready=` is set, init removes any stale marker, spawns the service in
the `starting` state, and waits until the marker appears. Exiting before the
marker or exceeding `timeout_ms` fails startup; a timed-out process is killed
and reaped. This is deliberately a small filesystem-marker protocol rather
than a claim that process creation alone means a service is ready.

## Supervision

Every death of a supervised child is logged with `SYS_LOG`.
`restart=always` restarts after any exit; `restart=on-failure` restarts only
after a nonzero or killed exit; and `restart=never` leaves the unit exited.
The legacy `respawn=yes` path behaves like `restart=always`.

Restarting units use a backoff of **0 s, 1 s, 2 s, then 5 s** for each
further restart. If a unit dies **5 times within 30 seconds** it is
marked `failed`, logged at error level
(`... died 5 times in 30 s: marking it failed, no more restarts`), and is
not restarted again until an operator runs `service start <name>`. A
death more than 30 seconds after the window opened starts a fresh window,
so a service that runs happily for hours and then exits restarts
immediately.

One deliberate exception: an instance that exited **with status 0 after
running for at least one second** is a normal end of life, not a crash.
It restarts immediately and does not count towards the window. Without
that, leaving the console with ctrl-D five times in half a minute would
permanently cost you your shell. Anything that exits non-zero, is killed
(`SYS_KILL` records `128 + signal`), or dies within its first second still
counts — which is exactly what a crash loop looks like.

Unit states, as reported in the state file and by `service`:

| state      | meaning |
|------------|---------|
| `stopped`  | stopped by an operator, or never started |
| `starting` | spawned and waiting for its `ready=` marker |
| `running`  | alive and ready, pid in the state file |
| `waiting`  | dead, waiting for its restart backoff |
| `exited`   | finished and not supervised (`once`, `respawn=no`, `sysinit`) |
| `failed`   | crash loop, missing program, bad `.svc`, dependency loss, or a cycle |
| `disabled` | `enabled=no` |

### Why not SYS_WAITANY

`SYS_WAITANY` is the natural reaper for a supervisor, but it cannot carry
this loop on the current kernel:

* it blocks for as long as *any* child is alive, and init must keep
  polling `/run` for control and shutdown requests;
* `uproc_waitany()` only reports a child that is still linked into the
  task list, and `task_exit()` hands the zombie to the scheduler's
  `reap()`, which unlinks it on the very next `schedule()`. A supervisor
  sleeping in one-tick increments usually misses the window and then
  waits forever on its surviving children.

init therefore detects exits with `SYS_PSINFO` (the pid is gone, or is a
zombie) and collects the status with `SYS_WAITPID`, which reads the same
exit ring and returns immediately for a process that has already been
unlinked. Two kernel changes would let init switch to `SYS_WAITANY`:
record the parent pid alongside the exit code in `uproc.c`'s exit ring so
an exit stays attributable after the task is unlinked, and give
`SYS_WAITANY` a timeout argument (`-1` with `*pid_out == 0` on expiry) so
the supervisor can still poll. Until then the psinfo/waitpid pair is the
only race-free option.

## The /run protocol

init owns four paths. All of them are optional: if `/run` cannot be
created init logs a warning and simply runs without a control interface,
and every consumer copes with the files being absent.

### /run/services.state — published by init

Rewritten whenever anything changes. A `#` header line, then one line per
unit:

```
# kestrel init state: name state pid restarts exit
logger running 6 0 0
sh running 7 2 0
```

Fields are `name state pid restarts exit-code`, separated by single
spaces. `service list` and `service status` read this file and nothing
else. init publishes the table as one fixed-size, journaled in-place write,
so readers see either the preceding complete snapshot or the new complete
snapshot; it never truncates the live file before filling it. A successful
control acknowledgement is written only after the corresponding snapshot
has been published.

### /run/init.cmd — written by `service`, consumed by init

One line: `<start|stop|restart|reload|reset-failed> <name>` followed by a
newline. **The newline is the commit marker** — init ignores the file until
it contains one, so a half-written request is never acted on. init processes
the request, unlinks the file and writes the reply to `/run/init.ack`.

`service` unlinks the stale ack, waits up to 2 s for an in-flight command
file to disappear (then overwrites it anyway, so a client that was killed
mid-request cannot wedge the interface), writes its line, waits up to 5 s
for init to consume it, and prints the ack.

### /run/init.ack — written by init

One line, `ok ...` or `err ...`. `service` exits non-zero on `err`.

### /run/shutdown — written by `reboot`/`halt`, consumed by init

One newline-terminated word: `reboot`, `halt` or `poweroff`. Same
commit-marker rule. Anything else is logged and discarded.

`/run` lives on the real filesystem rather than a tmpfs, so init deletes
`/run/shutdown`, `/run/init.cmd` and `/run/init.ack` at startup. Without
that, a shutdown request left behind by the previous boot would power the
machine off again the moment it came up.

## Shutdown

`reboot` and `halt` check that pid 1 looks like init, write their request
to `/run/shutdown`, and wait about 3 seconds. If the machine is still
running after that — init is wedged, or the request never reached it —
they call `SYS_POWER` directly, so the machine always goes down.

When init sees the request it:

1. stops every running unit in **reverse start order** (the order units
   were first started, recorded per unit), clearing their respawn flag so
   nothing comes back;
2. reaps for up to 3 seconds, logging anything that would not stop;
3. writes the final state file, removes the request, logs
   `system going down now`;
4. calls `SYS_POWER` with `K_POWER_REBOOT` or `K_POWER_HALT`.

## Logging

`SYS_LOG(level, msg)` appends to the kernel ring, tagged with the calling
process's name — init's messages are tagged `init`. The ring holds 256
entries (`kernel/include/klog.h`) and is read back with
`SYS_LOGREAD(index, struct k_logent *)`, index 0 being the oldest
retained entry.

`dmesg` prints the ring:

```
dmesg           everything the ring still holds
dmesg -n 20     the last 20 entries
dmesg -f        follow: keep printing new entries
```

`logger` is a supervised service that makes the ring durable. Every tick
(500 ms by default) it walks the ring and appends every entry it has not
written yet to `/var/log/messages`, in the same one-line format `dmesg`
uses:

```
[1753500000] info  init     (1) started logger (pid 6)
```

The next sequence number to write is kept in `/var/log/.messages.seq`, so
restarting the logger neither duplicates nor loses entries. When
`/var/log/messages` grows past the cap (64 KiB by default) the oldest
half is dropped: the tail is staged in `/var/log/messages.old`, copied
back over the original and the stage file removed, so a crash mid-rotation
can only lose the half that was already being discarded.

```
logger [-f <path>] [-i <interval ms>] [-c <cap KiB>]
```

## service

```
service list                    name, state, pid, restarts, exit code
service status <name>           the state line plus the .svc contents
service start|stop|restart <n>  change a unit's runtime state
service reload <name>           re-read its .svc and restart it if active
service reset-failed <name>     clear crash-loop failure/backoff state
service log <name>              lines of /var/log/messages mentioning <name>
```

`service log` matches the service name anywhere in the line, which picks
up both the service's own entries (tagged with its process name) and
init's entries about it.

If `/run/services.state` is missing, `service` says so rather than
guessing — that means init is not running or could not create `/run`.

## kill

```
kill <pid>...
```

`SYS_KILL` only returns success or failure, so `kill` works out the
reason from `SYS_PSINFO`: a pid that is not in the process table never
existed, a pid owned by another user when the caller is not root is a
permission failure, and pid 1 is refused outright (the kernel refuses it
too). Non-numeric arguments are rejected before any syscall.

## Adding a service

1. write `/etc/services/myservice.svc` (copy `/etc/services/example.svc`);
2. add `service myservice` to `/etc/inittab`;
3. reboot to make the new unit known to PID 1.

For an already-known unit, edit its `.svc` and run `service reload
myservice`. Reload validates and installs that service's current keys,
preserves an operator's runtime enable/disable choice, recomputes dependency
order, and restarts it when active. It does not reparse `/etc/inittab`, add
new unit names, or remove old ones; those table changes still require reboot.
