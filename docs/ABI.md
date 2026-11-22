# KestrelOS Userspace ABI

Ground truth header shared by kernel and libc: `abi/kestrel_abi.h`.
Both build paths generate the NASM syscall constants from its
`SYS_*` definitions; assembly stubs use the same numbers as C.
Userspace public headers: `libc/include/{stdio,stdlib,string,kestrel}.h`.
Link script: `apps/user.ld`.

## Calling convention

System calls are made with `int 0x80`:

| register | meaning              |
|----------|----------------------|
| `rax`    | syscall number (in) / return value (out) |
| `rdi`    | arg0 |
| `rsi`    | arg1 |
| `rdx`    | arg2 |
| `r10`    | arg3 |

All other registers are preserved across the syscall. A negative return
value (normally `-1`) indicates an error. Pointers passed to the kernel
must be valid user-space addresses in the current process.

libc exposes the raw entry as:

```c
long syscall(long n, long a, long b, long c, long d);
```

### Program entry

Binaries may be static ELF64 images linked at `USER_LOAD_BASE` (`0x400000`)
or Kestrel ET_DYN images with `DT_NEEDED` dependencies. The kernel records
lazy `PT_LOAD` areas, resolves the supported dynamic relocations, and enters
`_start`. It provides `rdi = argc` and `rsi = argv` (a NUL-terminated
`char *` array on the user stack, `argv[argc] == 0`). An absolute cwd from
`SYS_SPAWN_CAPS` is passed in `rdx` as a NUL-terminated string on the initial
stack; otherwise `rdx` is zero. The existing `--cwd=` argument is still
supplied when it fits, for older binaries. CRT0 exposes the initial
directory through `kestrel_startup_cwd()` (default `/`), calls
`main(argc, argv)`, and passes its return value to `SYS_EXIT`.

## Memory map (per process)

| region | address |
|---|---|
| program/dependency images | static base `0x400000` or assigned ET_DYN bases; pages populated lazily |
| heap | starts after the image, grows via `SYS_BRK`, populated on fault |
| stack | top at `0x00007FFFFFFFE000`; 16-page lazy range |
| kernel | `0xFFFF800000000000` and above; never user-accessible |

## Syscall table

Numbers, argument registers in order (rdi, rsi, rdx, r10).

| # | name | args | returns |
|---|------|------|---------|
| 0 | `SYS_EXIT` | code | does not return |
| 1 | `SYS_WRITE` | fd, buf, len | bytes written; fd 1/2 = console |
| 2 | `SYS_READ` | fd, buf, len | bytes read; fd 0 = console, blocks |
| 3 | `SYS_OPEN` | path, flags | fd or -1 |
| 4 | `SYS_CLOSE` | fd | 0 / -1 |
| 5 | `SYS_SPAWN` | path, argv | pid or -1 |
| 6 | `SYS_WAITPID` | pid | child exit code, blocks |
| 7 | `SYS_GETPID` | — | pid |
| 8 | `SYS_SLEEP_MS` | ms | 0 |
| 9 | `SYS_YIELD` | — | 0 |
| 10 | `SYS_READDIR` | path, index, `struct k_dirent *` | 0, or -1 at end |
| 11 | `SYS_UNLINK` | path | 0 / -1 |
| 12 | `SYS_MKDIR` | path | 0 / -1 |
| 13 | `SYS_STAT` | path, `struct k_stat *` | 0 / -1 |
| 14 | `SYS_BRK` | addr (0 = query) | current program break |
| 15 | `SYS_UPTIME_MS` | — | ms since boot |
| 16 | `SYS_MEMINFO` | `u64 *total_kb`, `u64 *free_kb` | 0 |
| 17 | `SYS_PSINFO` | index, `struct k_psinfo *` | 0, or -1 at end |
| 18 | `SYS_DNS` | name, `u32 *ip_be` | 0 / -1 |
| 19 | `SYS_PING` | ip_be, timeout_ms | rtt ms / -1 |
| 20 | `SYS_UDP_SEND` | ip_be, `dport<<16 \| sport`, buf, len | bytes / -1 |
| 21 | `SYS_UDP_RECV` | port, buf, maxlen, timeout_ms | len / -1 timeout |
| 22 | `SYS_NETINFO` | `struct k_netinfo *` | 0, or -1 if no NIC |
| 23 | `SYS_SEEK` | fd, off, whence (0=SET 1=CUR 2=END) | new pos / -1 |
| 24 | `SYS_READ_NB` | fd, buf, len | bytes read, 0 if none ready |
| 25 | `SYS_RTC` | `struct k_rtc *` | 0 / -1 |
| 26 | `SYS_POWER` | action | does not return on success |
| 27 | `SYS_SPAWN_IO` | path, argv, `struct k_spawn_io *` | pid / -1 |
| 28 | `SYS_PIPE` | `int fd[2]` | 0 / -1 |
| 29 | `SYS_DUP2` | oldfd, newfd | newfd / -1 |
| 30 | `SYS_KILL` | pid, signal | 0 / -1 |
| 31 | `SYS_WAITANY` | `int *pid_out` | exit code / -1 |
| 32 | `SYS_EXEC` | path, argv | no return on success |
| 33 | `SYS_GETUID` | — | uid |
| 34 | `SYS_GETGID` | — | gid |
| 35 | `SYS_SETUID` | uid | 0 / -1 |
| 36 | `SYS_CHMOD` | path, mode | 0 / -1 |
| 37 | `SYS_CHOWN` | path, uid, gid | 0 / -1 |
| 38 | `SYS_TIME` | — | Unix seconds |
| 39 | `SYS_LOG` | level, message | 0 / -1 |
| 40 | `SYS_LOGREAD` | index, `struct k_logent *` | 0 / -1 |
| 41 | `SYS_FBINFO` | `struct k_fbinfo *` | 0 / -1 |
| 42 | `SYS_MOUSE` | `struct k_mouse *` | 0 / -1 |
| 43–47 | `SYS_WIN_*` | create/destroy/flush/event/move arguments | operation result |
| 48 | `SYS_TCP_CONNECT` | ip, port, timeout_ms | handle / -1 |
| 49 | `SYS_TCP_SEND` | handle, buf, len | bytes / -1 |
| 50 | `SYS_TCP_RECV` | handle, buf, max, timeout_ms | bytes, 0 closed, -1 |
| 51 | `SYS_TCP_CLOSE` | handle | 0 / -1 |
| 52 | `SYS_SYNC` | — | 0 / -1 |
| 53 | `SYS_INSMOD` | path | 0 / -1 |
| 54 | `SYS_RMMOD` | name | 0 / -1 |
| 55 | `SYS_MODLIST` | index, `struct k_modinfo *` | 0 / -1 |
| 56 | `SYS_MOUNT` | path, type, device | 0 / -1 |
| 57 | `SYS_UMOUNT` | path | 0 / -1 |
| 58 | `SYS_BLKLIST` | index, `struct k_blkinfo *` | 0 / -1 |
| 59 | `SYS_DEVLIST` | index, `struct k_devinfo *` | 0 / -1 |
| 60 | `SYS_MOUNTLIST` | index, `struct k_mountinfo *` | 0 / -1 |
| 61 | `SYS_GETRANDOM` | buf, len, flags | bytes / -1 |
| 62 | `SYS_SWAPINFO` | `u64 *total_kb`, `u64 *used_kb` | 0 / -1 |
| 63 | `SYS_SIGACTION` | sig, action, old action | 0 / -1 |
| 64 | `SYS_SIGPROCMASK` | how, mask, old mask | 0 / -1 |
| 65 | `SYS_SIGRETURN` | internal restorer | restores interrupted frame |
| 66 | `SYS_CPUINFO` | `struct k_cpuinfo *` | 0 / -1 |
| 83 | `SYS_HANGAR` | op, a, b, c (`K_HANGAR_PARK`/`LIST`/`THUMB`/`RESUME`/`DISCARD`) | bay / pid / 0 / -1 |
| 90 | `SYS_MISSION_SPAWN` | path, argv, `const struct k_mission_grant *`, optional `u32 *mission_id` | pid / -1 |
| 91 | `SYS_MISSION_CTL` | mission id, op, arg | 0 / -1 |
| 92 | `SYS_MISSION_INFO` | live index, `struct k_mission_info *` | 0 / -1 at end |

`K_WIN_OVERLAY` (flag 4) requires `K_WIN_NODECOR` and excludes `K_WIN_DESKTOP`.
Overlay windows stay above normal windows without taking keyboard focus or
participating in Alt-Tab. `KEY_MISSION` (0x90, Ctrl+M) is routed to the desktop.

### Mission grants

See [missions.md](missions.md) for enforcement and lifetime rules. Numbers
78–82 are reserved for IPv6; Hangar park/resume multiplexes through 83 and
mission calls use 90–92.

`k_mission_grant` contains `app[24]`, `title[48]`, `cap_mask`, `path_count`,
`net_mode`, `host_count`, `device_bits`, `ipc_bits`, a zero `reserved` field,
eight `k_mission_path` entries (`prefix[64]`, R/W `mode`, `revoked`), and four
`k_mission_host` entries (`ip_be`, `revoked`). Input strings must terminate,
prefixes must be canonical absolute paths (userspace expands `~`), and input
revocation fields must be zero. Counts bound the active entries. Missions
cannot receive `KCAP_ADMIN`. The effective mask intersects the caller's mask;
an already-mission-scoped caller may only narrow its effective mission grant.

`k_mission_info` contains `id`, `owner_uid`, the effective `grant`, a
channel-bitmap `revoked`, ten `k_mission_counter` entries (`uses`, `denies`,
`last_use_tick`, `last_deny_tick`), `last_deny[40]`, `task_count`, `pid_count`,
and `pids[32]`. Timestamps are monotonic milliseconds; counts include suppressed
pulses. PID lists truncate at 32 while `task_count` remains the full count.
Enumeration is a live snapshot, not a stable cursor across task exits.

Channel indices 0–9 are fs.read, fs.write, net, display, spawn, ipc, audio,
camera, input, random. Device bits 0–3 correspond to audio, camera, input,
random. IPC bits 0–3 are talon, pulse, signal, pipe. Net modes are none (0),
any (1), hosts (2). Paths have READ (1) and WRITE (2) mode bits.

Control operations are `K_MISSION_REVOKE_CAP` (1, arg = channel index),
`K_MISSION_REVOKE_PATH` (2, arg = path index), `K_MISSION_REVOKE_HOST` (3,
arg = host index), and `K_MISSION_END` (4, arg ignored). Only a non-mission
caller that is root or the mission owner may control a mission. Revocation
updates the effective grant, records revoked channels/entries, and clears
corresponding capability bits on every live task in the mission.

`K_TOPIC_MISSION` is `0x40` and is included in `K_TOPIC_ALL` (`0x7f`). Mission
pulse kinds START/USE/DENY/REVOKE/END have values 11–15. `arg0` is the mission
id; `arg1` is the channel (zero for start/end); `tag` is short detail. USE
pulses have a 250 ms per-mission/channel minimum interval; DENY pulses 50 ms.

`open()` flags: `O_RDONLY 0x000`, `O_WRONLY 0x001`, `O_RDWR 0x002`,
`O_CREAT 0x040`, `O_TRUNC 0x200`, `O_APPEND 0x400`.

All network addresses (`ip_be`, fields of `struct k_netinfo`) are
big-endian (network byte order). Ports in the C wrappers are host-order
`uint16_t`; `udp_send()` packs them as `dport<<16 | sport` for arg1.

### Console input model

`SYS_READ` on fd 0 delivers raw bytes, one key per byte, blocking until
at least one is available. Printable keys arrive as ASCII, ctrl-A..Z as
1..26, ESC as 27, Enter as `'\n'`, Backspace as 8. Special keys arrive
as single bytes >= 0x80: `KEY_UP 0x80`, `KEY_DOWN 0x81`, `KEY_LEFT
0x82`, `KEY_RIGHT 0x83`, `KEY_HOME 0x84`, `KEY_END 0x85`, `KEY_PGUP
0x86`, `KEY_PGDN 0x87`, `KEY_DELETE 0x88`. There is no kernel-side line
discipline or echo; `readline()` in libc implements both.

### ABI structs

Defined in `abi/kestrel_abi.h`:

```c
struct k_stat {
    uint32_t size, is_dir, mode, uid, gid, mtime;
};
struct k_dirent {
    char name[60];
    uint32_t size, is_dir, mode, uid, gid, mtime;
};
struct k_psinfo {
    int32_t pid; char name[32]; int32_t state;
    uint32_t uid; int32_t ppid;
};
struct k_cpuinfo {
    uint32_t online, discovered, current_cpu, apic_id;
};
struct k_netinfo {
    uint32_t ip, netmask, gateway, dns;  /* big-endian */
    uint8_t mac[6];
    uint8_t up;                          /* 1 if NIC initialized */
    uint8_t _pad;
};
```

`k_psinfo.state`: `K_STATE_RUNNABLE 0`, `K_STATE_RUNNING 1`,
`K_STATE_SLEEPING 2`, `K_STATE_ZOMBIE 3`, `K_STATE_STOPPED 4`.

## libc surface

### `<stdio.h>`

```c
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int snprintf(char *buf, unsigned long size, const char *fmt, ...);
int vsnprintf(char *buf, unsigned long size, const char *fmt, va_list ap);
int puts(const char *s);   /* appends '\n' */
int putchar(int c);
int getchar(void);         /* raw blocking byte from fd 0; may return KEY_* */
```

The formatter is deliberately smaller than ISO C `printf`. It supports
`%s %c %d %i %u %x %X %p %%`, decimal field width, the `0` and `-`
flags, and `l`/`ll` on integer conversions (both select a 64-bit value on
x86-64). It does not support floating-point conversions or `*` field
width.

Precision has one supported formatting use: bounding a byte string.
`%.Ns` takes a decimal literal limit, `%.*s` takes the limit from one
`int` argument, and a bare `%.s` means zero. A negative `*` precision is
treated as omitted. With a nonnegative precision, `%s` emits at most
that many bytes, stopping earlier at NUL, and does not read beyond the
limit; the input therefore need not be NUL-terminated within the
counted slice. Precision counts bytes, not UTF-8 characters. Field width
is applied to the bounded result, with space padding before it or, for
`-`, after it.

A NULL `%s` argument is rendered as `"(null)"`, and string precision
applies to that replacement. Without precision, a non-NULL `%s`
argument must be NUL-terminated. Precision on the supported integer
conversions is parsed but has no formatting effect; a `*` precision is
still consumed exactly once so later varargs remain aligned. No other
precision semantics are promised.

`snprintf` and `vsnprintf` always NUL-terminate when `size > 0` and
return the number of bytes that would have been written, excluding the
terminating NUL. They may be called with `buf == NULL` when `size == 0`;
the format and arguments are still processed and the full count is
returned without a write. A NULL buffer with nonzero size is outside the
libc contract.

### `<stdlib.h>`

```c
void *malloc(unsigned long size);
void free(void *ptr);
void *calloc(unsigned long n, unsigned long size);
void *realloc(void *ptr, unsigned long size);
int atoi(const char *s);
long atol(const char *s);
void exit(int code);            /* noreturn -> SYS_EXIT */
int abs(int v);
int rand(void);                 /* 0..RAND_MAX (0x7fffffff) */
void srand(unsigned int seed);
```

The heap is built on `SYS_BRK`.

### `<string.h>`

`memset memcpy memmove memcmp strlen strcmp strncmp strcpy strncpy
strcat strncat strchr strrchr strstr strdup` with the usual C semantics;
all size parameters are `unsigned long`. `strdup` allocates with
`malloc`.

### `<kestrel.h>`

Includes `kestrel_abi.h`. One thin wrapper per syscall (signatures in
the header): `open read write close seek read_nb readdir_at unlink_
mkdir_ stat_ spawn waitpid getpid sleep_ms yield_ brk_ uptime_ms
meminfo psinfo dns_resolve ping udp_send udp_recv netinfo`. Names that
would clash with C keywords or POSIX expectations carry a trailing
underscore. `SEEK_SET/SEEK_CUR/SEEK_END` are 0/1/2.

Helpers:

```c
char *readline(char *buf, int n);      /* echo + backspace + ctrl-U;
                                          NULL on ctrl-D at empty line */
uint32_t ip_aton(const char *s);       /* dotted quad -> BE ip, 0 on fail */
char *ip_ntoa(uint32_t ip_be, char buf[16]);
```

Terminal macros (ANSI on fd 1): `term_clear() term_goto(r,c)
term_color(fg) term_reset() term_hide_cursor() term_show_cursor()`, with
`TERM_BLACK..TERM_WHITE` = ANSI SGR 30..37.

## Toolchain

Userspace compile flags: `-m64 -ffreestanding -nostdlib -fno-pic
-fno-pie -Wall -Wextra -O2 -Ilibc/include -Iabi`. Link:
`ld -T apps/user.ld -nostdlib crt0.o <objs> libc.a`. Sections `.eh_frame`,
`.comment` and `.note*` are discarded by the link script.
