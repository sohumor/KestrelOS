# Mission manifests

Missions are per-launch, task-scoped grants rather than permanent application
permissions. A launcher describes what an application is doing and grants a
subset of its manifest ceiling. Kernel checks make those limits effective;
pulses and counters let a desktop HUD explain use, denial, and live revocation.
The manifest parser and launch CLI build on this kernel ABI; desktop UI is
the next layer.

## Kernel model

There are at most 16 live missions. IDs are positive and never reused during
a boot. Tasks outside a mission have ID zero and retain the previous capability
and unveil behavior. Ordinary spawn, spawn_io, spawn_caps, and exec replacements
inherit the mission before they become runnable. References include tasks
loading an ELF or failing a launch; the final task's exit ends the mission.
An explicit mission spawn creates a new mission, intersecting capabilities with
the caller. A mission caller can create only a subset of its current effective
grant and can only launch an executable it can already read. It cannot control
missions, including its own. Administrator capabilities are not grantable.

`kernel/mission_core.c` is dependency-free policy and accounting logic. The
kernel glue serializes the table, bounds user input, and integrates with the
existing capability masks and unveil gate, not a second filesystem dispatcher.
The scheduler publishes mission membership/refcounts before runnable tasks;
revoke also removes capability bits from all matching live tasks. End requests
cooperative task termination using the existing kill checkpoints, and marks
tasks spawned concurrently for termination too.

## Paths and devices

Grant prefixes are canonical absolute paths, limited to 63 bytes plus NUL.
Userspace must expand `~` using the launching user's home. Access paths are
canonicalized (repeated slash, dot, and dot-dot) and matched at component
boundaries: `/home/pilot/notes` never permits `/home/pilot/notesX`.
Read and write modes are separate; read/write opens require both. Creating,
truncating, appending, unlinking, mkdir, rename (both paths), chmod, and chown
require write access. The old single unveil prefix remains an additional limit.

Minimal implicit runtime paths are:

* The exact launched executable, read-only, so the kernel can load its ELF.
* `/lib`, read-only, for the ELF loader's `DT_NEEDED` libraries.
* `/etc/desktop.conf` and `/etc/theme`, read-only: these are the actual settings
  paths opened by `libgui/gui.c`. Fonts are compiled in; no font directory or
  broad `/share`/`/etc` grant is necessary.
* `/dev/null`, for discard/empty I/O.

These VFS runtime exceptions permit loader reads even for an empty grant;
ordinary user `open` still requires the existing fs capability bits. Console
stdio already open as descriptors is unchanged. As with existing unveil,
path restrictions apply when a path is accessed, not retroactively to handles
already open. They do not remove already mapped executable pages.

Under `/dev`, ordinary path grants (even `/`) never substitute for device
bits. Audio covers `/dev/audio` and `/dev/mixer`; camera `/dev/camera`; input
`/dev/mouse` and `/dev/keyboard`; random `/dev/random` and `/dev/urandom`.
Absent devices remain valid declared bits. The random bit also gates
`SYS_GETRANDOM`, preventing a syscall bypass of the device policy.

## Network, IPC and observability

Destination checks on TCP connect, UDP send, and ping allow none, any, or one
of four non-revoked IPv4 addresses. DNS remains available with KCAP_NET so
userspace can resolve a name before connecting. Revoking a host prevents new
destination-checked operations; existing TCP handles are not disconnected.
Talon parcels, pulse polling, signals via kill, and pipe creation additionally
require their individual IPC bit.

Channels are fs.read, fs.write, net, display, spawn, ipc and the four device
classes. Counters count policy checks (a syscall can exercise more than one
check), even when its pulse is suppressed. Latest-use/deny timestamps are
monotonic milliseconds. Independent USE/DENY pulse clocks prevent reads or
denial loops from flooding the pulse ring; deny and revoke also produce
`mission` events in the flight recorder's existing process category.

`SYS_MISSION_INFO` exposes bounded snapshots, including the effective grant,
revoked channels/entries, counters, last denial, and up to 32 PIDs. All live
tasks remain scoped even when the PID list is truncated. `/dev/missions` is an
`@krec` view with id, first pid, app, mission text, granted-channel bitmap,
revoked bitmap, aggregate uses/denies, and last denial. It is readable by
ordinary non-mission observers like `/dev/caps`; mission device policy does
not implicitly grant access to these introspection devices.

## Host checks

`make CC=clang test-mission` or `python3 tools/build.py all` runs the same
ASan/UBSan policy tests: valid/empty/malformed grants, cannot-widen subsets,
canonical and boundary-safe path matching, write modes, runtime exceptions,
device restrictions, host checks, revocation, counters and rate limits.
See [ABI.md](ABI.md#mission-grants) for syscall and field details.

## Userspace

Manifests live at `/etc/missions/<id>.mission`. Each is line-oriented
`key: value`, with `#` comments. Required fields are `app`, `exec` (absolute),
and `mission`. Optional fields default to no access: `display` and `spawn`
take `yes`/`no`; `fs.read`/`fs.write` take space-separated prefixes or `none`;
`net` takes `none`, `any`, or `host` followed by names/IPv4 addresses;
`device` takes audio/camera/input/random or `none`; `ipc` takes
talon/pulse/signal/pipe or `none`. Unknown or duplicate keys are errors.
There is no quoting or environment substitution. `~` and `~/...` expand to
the calling UID's home from `/etc/passwd`, not the current directory.

`<mission.h>` exposes `mission_parse` and `mission_build` (host-testable,
with caller-supplied home and resolver), plus `mission_load` and
`mission_build_current` for guest filesystem/home/DNS integration. DNS is
performed when constructing a grant, never by the parser or kernel. The
kernel receives only resolved IPv4 addresses; aliases can narrow a host grant
only if they resolve to an address already in its ceiling.

```
mission list
mission show <id>
mission run <id> [--fs-read P]... [--fs-write P]...
  [--no-net | --net-host H]... [--no-device D]... [--no-ipc] [--bg] [-- args]
mission ps
mission revoke <mid> <channel | path:N | host:N>
mission end <mid>
```

Read/write flags replace that mode's declared prefixes, retaining the other
mode. Every replacement must be contained by a same-mode declared prefix.
Network flags only remove access or select declared destinations; device/IPC
flags only remove bits. No option enables an undeclared capability.
`ps` shows channel states and counters, PIDs, paths/hosts and the latest deny.
`--bg` prints `mission <mid> pid <pid>` without waiting for the child; the
optional fourth spawn syscall argument returns the mission ID atomically
with launch, even if the child exits before userspace can query it.

The host manifest tests cover parsing errors, home expansion, DNS, narrow-only
options and every shipped manifest. Guest tests `mission-grant`, `mission-deny`,
and `mission-revoke-live` cover successful reads, path/host denials with
telemetry, and revocation on the same still-running PID. `missionprobe` is a
small repeated-open/ping helper for these live-policy checks.
