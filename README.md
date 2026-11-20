# KestrelOS

A test to see how good Claude Fable 5 is, this took around 5-6 hours, over 120 sub-agents and around 6 million tokens. Thanks to Marcy and violet for giving me the idea, much love. 

An operating system written from scratch: no third-party code, no GRUB, no
ported libc, no borrowed drivers. Every byte from the boot sector to the
window manager is in this repository, including the tools that build the disk
image and the font the console renders with.

Target: **x86-64**, BIOS boot, graphics up to **2560×1440**. Verified booting
in QEMU and in real VirtualBox.

```
login: kestrel
password:

welcome, kestrel
kestrel:/$ browser -t http://example.com
Example Domain

This domain is for use in documentation examples without needing permission.
Avoid use in operations.

Learn more
```

That page was fetched over our own TCP implementation and rendered by our own
HTML engine.

## What's in it

**Boot** — a two-stage BIOS bootloader. Stage 1 fits in the 512-byte MBR;
stage 2 enables A20, collects the memory map, loads the kernel through unreal
mode, negotiates a VBE linear framebuffer, builds page tables and enters long
mode directly from real mode.

**Kernel** — higher-half at `0xFFFFFFFF80100000`: a framebuffer console with
its own 8×16 font and an ANSI/VT100 parser, serial console, GDT/IDT/TSS,
exceptions, PIC, PIT, PS/2 keyboard and mouse, CMOS clock, power control, a
kernel log ring, and an in-kernel rescue console for when userspace can't start.

**Memory** — a bitmap physical allocator driven by the E820 map, 4-level paging
with a full direct map, per-process address spaces, lazy executable/heap/stack
faults, dynamic raw swap sizing derived from the KFS superblock (`SWAP_MAX_PAGES` =
4096), and a kernel heap.

**Processes** — ACPI/xAPIC SMP startup (`proc_mark_current_idle()` for per-CPU
idle accounting) and preemptive scheduling across up to 16 CPUs, kernel threads,
ring-3 processes with per-task FPU state and credentials, monotonic process
sandboxing and capability masks (`SYS_SPAWN_CAPS`, `SYS_SANDBOX`), Kernel Pulses
event telemetry (`SYS_PULSE_POLL`), Talon typed clipboard parcels
(`SYS_PARCEL_*`), Avionics Flight Recorder (`/dev/flight` `@krec` + `/bin/flight`),
Aerie kernel `@krec` introspection tables (`/dev/procs`, `/dev/caps`,
`/dev/parcels`), traditional signals, static and dynamically linked ELF64
programs, pipes, I/O redirection, process control, and a 78-call syscall
interface (`0..77`).

**Storage** — an ATA driver and **KFS v3** (`KFS3`), a custom filesystem with a
checksummed 33-block full-data and metadata redo journal, recursive FIFO ticket
`fs_lock` serialization across SMP CPUs, atomic cross-directory `rename`,
indirect blocks, and per-file ownership, permissions and timestamps, enforced by
the VFS against each task's credentials. Plus `/dev` with block/character
devices and live `@krec` kernel table views.

**Networking** — PCI enumeration, two NIC drivers (RTL8139 and Intel e1000)
behind a common interface, DHCP configuration with static QEMU fallback, and a
from-scratch stack: Ethernet, ARP, IPv4, IPv6 (SLAAC, Neighbor Discovery,
ICMPv6), ICMP, UDP, a **DNS TTL cache** covering A and AAAA answers, and **TCP
with out-of-order reassembly and selective acknowledgements (SACK)**, with
persistent HTTP/1.1 Keep-Alive connection pooling and verified TLS 1.3 HTTPS
clients on top (128-SAN X.509 parsing), plus live `/dev/sockets`, `/dev/net`,
`/dev/arp`, and `/dev/dns` `@krec` telemetry consumed by `/bin/netstat`.

**Desktop** — the Avionics kernel compositor where each window's pixel buffer is
mapped into the owning process, featuring 4 workspaces (`Alt+1..4`), window
pinning, live compositor-backed `SYS_WIN_RESIZE` / maximize/restore (`KEV_RESIZE`),
live compositor theme broadcast (`SYS_WM_THEME` / `KEV_THEME`), draggable
title bars, minimize/restore, mouse wheel routing, and same-user task controls.
`libgui` 2.0 provides anti-aliased fonts, rounded controls, gradients, shadows,
sparklines, Talon clipboard integration, and `gui_editor`. The Avionics shell
(`desktop`) adds a top telemetry bar with pulse-driven CPU/RAM sparklines,
**Kestrel Perch** (`Super` launcher and live evaluator), **Talon Visual Shelf**
(`Ctrl+T`), **Quick Control** (`Ctrl+Q`), and applications including Browser,
Terminal, Notes (`notes`), Sight (`sight`), Files (`gfiles`), Paint (`gpaint`),
System Monitor (`sysmon`, with a live **Flight Deck** tab over `/dev/flight`,
`/dev/sockets`, `/dev/caps`, and `/dev/parcels`), Calculator (`gcalc`), Clock
(`gclock`), Settings, and About. `Alt-Tab` switches tasks and `Alt-F4` closes
the focused window.

**Browser** — verified TLS 1.3 HTTPS, persistent cookies/cache/origin storage,
tab favicons (16-entry LRU cache), `Ctrl-U` `view-source:`, per-tab zoom
(`90%`/`100%`/`125%`/`150%`), `/home/.browser_session` save/restore, clickable
`<details>`/`<summary>` and `<dialog open>`, linked/imported CSS (`--custom-props`,
`var()`, `calc()`/`min()`/`max()`/`clamp()`, `light-dark()`, `color-scheme`,
`@media` ranges/`prefers-color-scheme`/`scripting`, CSS Grid with
`grid-template-rows`, full Flexbox wrap/shrink/basis/baseline/`align-self`/`order`,
`aspect-ratio`, `object-fit`, `outline`, `text-overflow: ellipsis`, `word-break`/
`overflow-wrap`/`letter-spacing`, 2D `transform` `translate`/`scale`, `position:
sticky`, `opacity`, AA `border-radius`, `box-shadow`, `linear-gradient()`),
baseline and progressive (`SOF2`) JPEG, PNG, GIF, BMP, `.ico`/`.cur` (PNG +
1/4/8/24/32-bpp DIB), standalone SVG rasterizer, QOI, and Netpbm (`P1`–`P6`)
decoding (`libimg`), multi-subpath inline SVG (`M/L/H/V/C/S/Q/T/Z`,
`currentColor`, `viewBox`, `preserveAspectRatio`), software `<canvas>` 2D with
`CanvasGradient` (`createLinearGradient`/`createRadialGradient`), `ImageData`
(`createImageData`/`getImageData`/`putImageData`), and PNG `toDataURL()`, and
an ES6+ runtime (`libjs`) with real block-scoped `let`/`const` (TDZ and
per-iteration closures), `class`/`extends`/`super`, `async`/`await`,
template/tagged literals, destructuring, spread/rest, `for..of` and `Symbol`,
`?.`/`??`/`**`, `Promise`/fetch/`AbortController`/`AbortSignal`, `Blob`/`FormData`,
full TypedArrays (`Int8Array` through `Float64Array`, including `Uint8ClampedArray`)
+ `DataView`, WHATWG `URL` + `URL.canParse()`, `structuredClone()` (with cycle
preservation), ES2022/2023 helpers (`Object.hasOwn`, `.at()`, `findLast`/
`findLastIndex`, `toReversed`/`toSorted`/`toSpliced`/`with`, `RegExp.prototype.flags`),
`crypto.{getRandomValues,randomUUID,subtle.digest}` (SHA-1/256/384/512),
`Map`/`Set`/`WeakMap`/`WeakSet`, `DOMParser`, `<template>`, `getComputedStyle()`,
`ResizeObserver`/`IntersectionObserver`, `getBoundingClientRect()`,
`scrollTo`/`scrollBy`, `navigator.clipboard`, two-way `dataset`, live `classList`,
a small i32 WebAssembly core, and bounded static/dynamic `import()`. Local
offline showcase pages live at `/doc/home.html`, `/doc/playground.html`, and
`/doc/flightdeck.html`.

**System** — multi-user logins with salted iterated SHA-256 password hashes
(written from the specification), a non-blocking readiness-polling `init`
supervisor with hard dependencies, restart policies and backoff, a `.kpkg`
package manager with dependency resolution and integrity verification, an `@krec`
typed structured record library (`-r` shell pipelines across `ls`, `ps`, `df`,
`dmesg`, `lsdev`, `lsmod`, `netstat`, `service`, `talon`, `inspect`, `flight`),
`where`/`sortby`/`pick` filters, standalone `/bin/kjs`, and 90 userspace programs
in `apps/`. Kernel randomness uses RDSEED/RDRAND and interrupt timing inputs, a
SHA-256 mixing pool, and a ChaCha20 CSPRNG exposed through `getrandom()`,
`/dev/random`, and `/dev/urandom`.

## Building

Cross-platform via `tools/build.py` (supports **Windows** with LLVM/Clang/LLD +
NASM + QEMU, **Linux/WSL2**, and **macOS** cross-toolchains):

```bash
python3 tools/build.py all       # build OS image, host C unit suites, fsck, & E2E
python3 tools/build.py test-host # build & run the 13 host C suites + KFS checks
python3 tools/build.py e2e       # headless QEMU E2E suite over serial/TCP
```

Or on Linux / WSL2 using the `Makefile` (`build-essential nasm make python3
qemu-system-x86`):

```bash
make            # build build/os.img (bootable raw disk image)
make run        # boot it in QEMU
make test       # end-to-end suite, headless, driven over serial
make vm-images  # convert to VirtualBox .vdi / VMware .vmdk
make help       # all targets
```

Log in as `root` / `root` or `kestrel` / `kestrel`. These passwords are
published on purpose — this is a demo system, and `docs/users.md` explains
exactly how weak its security guarantees are.

## Running it in a VM

**QEMU** — 16 MiB of video memory or more gets you 1440p:

```bash
qemu-system-x86_64 -drive file=build/os.img,format=raw -m 512M \
  -smp 4 -device VGA,vgamem_mb=32 \
  -device e1000,netdev=n0 -netdev user,id=n0
```

**VirtualBox** — `make vm-images`, create an "Other/Unknown (64-bit)" VM,
attach `build/kestrel.vdi`, and set the network adapter to **Intel PRO/1000 MT
(82540EM)**. **VMware** — attach `build/kestrel.vmdk`.

See [docs/RUNNING.md](docs/RUNNING.md) for step-by-step instructions and
troubleshooting.

## Repository layout

```
boot/      stage 1 + stage 2 bootloader (NASM)
kernel/    the kernel (C + a little NASM)
abi/       the user/kernel ABI, shared by both sides
libc/      userspace C library (incl. @krec structured records)
libgui/    Avionics 2.0 userspace GUI toolkit + gui_editor
libweb/    HTML5 parser, DOM, CSS engine, layout, paint, Canvas/Web APIs
libjs/     ES6+ JavaScript engine
libtls/    TLS 1.3 client, X.509 validator, ChaCha20/SHA-2/AES-GCM
libimg/    PNG, BMP, GIF, baseline/progressive JPEG, ICO/CUR, SVG, QOI, Netpbm decoders
libz/      deflate/inflate, CRC32, Adler32
apps/      userspace programs (each becomes /bin/<name>)
modules/   loadable kernel modules
packages/  sources for the shipped .kpkg packages
rootfs/    files copied into the disk image
tools/     build.py runner, mkfs, image builder, fsck, host C suites, E2E
docs/      architecture, ABI, filesystem, networking, drivers, running, testing
```

## Documentation

| Document | Contents |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | how the whole system fits together |
| [docs/smp.md](docs/smp.md) | CPU discovery, AP startup and scheduler locking |
| [docs/random.md](docs/random.md) | entropy collection, pool mixing and the kernel CSPRNG |
| [docs/MODULARITY.md](docs/MODULARITY.md) | the module system and driver model |
| [docs/DESIGN-desktop.md](docs/DESIGN-desktop.md) | why the desktop is built the way it is |
| [docs/ABI.md](docs/ABI.md) | syscall table, memory map, libc surface |
| [docs/kfs.md](docs/kfs.md) | the KFS on-disk format |
| [docs/net.md](docs/net.md) | network stack and NIC drivers |
| [docs/tls.md](docs/tls.md) | TLS 1.3 client, verification, testing and limits |
| [docs/users.md](docs/users.md) | accounts, password hashing, and its limits |
| [docs/packages.md](docs/packages.md) | the .kpkg format and the package manager |
| [docs/browser.md](docs/browser.md) | the HTML engine and what it does not do |
| [docs/DESIGN-browser-runtime.md](docs/DESIGN-browser-runtime.md) | browser ownership, data flow, events, and lifecycle state machines |
| [docs/RUNNING.md](docs/RUNNING.md) | building and running in each hypervisor |

## Known limitations

SMP currently uses xAPIC and the legacy PIC: external hardware interrupts land
on the BSP, while reschedule IPIs distribute runnable work. Swap is one raw
extent sized past the KFS superblock (`SWAP_MAX_PAGES` = 4096) with a simple
local second-chance policy, not a general multi-device block pager. The dynamic
linker supports the relocation set emitted by this tree, not arbitrary System V
binaries. Signals are the traditional non-realtime subset. KFS v3 redo
transactions are capped at 32 blocks (plus commit block in the 33-block journal),
networking is IPv4-only, and TLS is 1.3-only. The from-scratch TLS, CSPRNG, and
filesystem code are educational implementations and have not had a professional
security audit.

The browser is intentionally a bounded compatibility subset, not a claim of
support for every website. Its static ES module bindings are copy-by-value rather
than live bindings, literal-string `import()` is eagerly resolved rather than
truly on demand, and computed `import()` or top-level `await` are not supported
(`import.meta.url` is supported, and resolves to the document URL for inline
modules). Inline SVG supports multi-subpath paths (`M/L/H/V/C/S/Q/T/Z`),
`currentColor`, `viewBox`, and `preserveAspectRatio`, while standalone SVG
images (`<img>` / `data:image/svg+xml`) go through `libimg`'s fixed-point SVG
rasterizer (`transform` and `A/a` arcs supported; inline `<svg>` vector
gradients, filter effects, and CSS-style inline SVG transforms remain absent).
Generators, `Proxy`, `eval()`, `Function()`, `BigInt64Array`/`BigUint64Array`,
`SharedArrayBuffer`/`Atomics`, tracing GC, downloadable webfonts, audio/video
codecs, HTTP/2, and HTTP/3 are not implemented. HTTPS currently carries HTTP/1.1
over verified TLS 1.3.

**This cannot run Chrome, and never will.** Chrome needs the Linux syscall ABI,
glibc, X11 or Wayland, GPU drivers and a JIT — far more work than this entire
operating system, none of which would teach anything about how one works. The
browser here is our own, and it renders the real web over our own TCP.
