# Design notes: from a shell to a desktop system

This document records the decisions behind the second half of KestrelOS —
multi-user support, a service-based init, a package manager, and a graphical
desktop — and, just as importantly, what was deliberately left out.

## What the goal actually required

The stated goal was "users, an init system, a package manager, a UI, logins, a
window manager". Each of those has prerequisites that had to be built first,
and those prerequisites were most of the work:

| Goal | What it actually needs |
|---|---|
| logins, users | file ownership and permissions **in the on-disk format**, per-task credentials, a password database, a hash function |
| window manager | a linear framebuffer, a font renderer, a mouse driver, a compositor, an event-routing model, memory shared between kernel and app |
| init with services | pipes, process control (kill/wait-any), a log that is not the console |
| package manager | an archive format, an integrity check, and TCP if packages come from anywhere but the local disk |
| a browser | TCP, HTTP, an HTML parser, a layout engine, and the GUI stack above |

So the build order was: framebuffer + font + mouse + TCP + filesystem
permissions + pipes + log (wave 1), then the syscall surface (wave 2), then
the user-visible systems (wave 3).

## Decisions

### Graphics mode is set by the bootloader, not the kernel

VBE mode setting requires BIOS calls, which require real mode. Once the kernel
is in long mode there is no going back without a v86 monitor or an emulator for
the video BIOS. So stage 2 negotiates the mode (preferring 1024×768×32 linear)
immediately before the long-mode switch and passes the framebuffer address,
pitch and geometry to the kernel in the boot info block.

The consequence is that the VGA text buffer at 0xB8000 is gone once graphics
are up, so the kernel console had to become a **framebuffer text console**: an
8×16 bitmap font, a character grid, dirty-row flushing, and the same ANSI escape
parser as before. Every existing program keeps working unchanged, and the serial
console — which the test harness drives — is unaffected either way.

If VBE negotiation fails the bootloader leaves the machine in text mode and the
kernel uses the old VGA path. Both paths are live.

### The compositor lives in the kernel

A userspace window server is the more fashionable design, but it needs a mature
IPC layer with shared memory and a fast round trip, and getting that wrong shows
up as a sluggish or deadlocking desktop. The kernel already owns the framebuffer,
the mouse, and the keyboard; putting the compositor there means a window is just
a kernel object with a pixel buffer mapped into the owning process, and drawing
is a plain memory write with no message passing at all.

Applications get a compact set of syscalls (create, flush, event, move,
resize, destroy, theme) and the Avionics `libgui` 2.0 toolkit is layered on top
with anti-aliased fonts, rounded surfaces, gradients, shadows, sparklines, Talon
clipboard helpers, and `gui_editor`. The window manager policy — stacking,
focus, dragging, maximize/restore (`SYS_WIN_RESIZE` + `KEV_RESIZE`), 4 virtual
workspaces (`Alt+1..4`), pinning, close and minimize — is kernel-side too, which
is the classic tradeoff: less flexible than an IPC display server, much
simpler, and impossible to deadlock.

The desktop shell has additional calls (`SYS_WIN_LIST`, `SYS_WIN_CTL`,
`SYS_WM_THEME`): enumerate ordinary same-user windows, request focus, minimize,
restore, maximize, workspace migration, pin, close, or update the active
compositor theme (`k_wm_theme`) and broadcast `KEV_THEME` events. Those calls
are capability-gated: the kernel accepts them only from a process that already
owns a `K_WIN_DESKTOP` surface (or `wm_theme` capability paths), and never
exposes windows belonging to another uid. This makes the dock and workspace switcher real task controllers without making window ids an ambient authority.

### The shell is an Avionics desktop environment, not a mock-up

The background remains a full-screen `K_WIN_DESKTOP` layer. The top telemetry
panel (displaying live CPU/RAM sparklines fed by non-blocking Kernel Pulses via
`SYS_PULSE_POLL`), wallpaper, workspace indicators, dock, and notifications are
painted into that layer, while the **Kestrel Perch** launcher/evaluator,
**Talon Visual Shelf**, and **Quick Control** panel open as interactive overlay
windows that accept keyboard input above ordinary applications.

The shell provides:

- **Kestrel Perch** (`Super`): a searchable application launcher and inline expression evaluator;
- **Talon Visual Shelf** (`Ctrl+T`): structured inspection and recall of typed clipboard parcels (`SYS_PARCEL_PUT`/`GET`/`LIST`);
- **Quick Control** (`Ctrl+Q`): theme presets, network/power actions, and live status;
- a live dock backed by the compositor's real window list across 4 workspaces (`Alt+1..4`) and pinned windows;
- click-to-focus, click-again-to-minimize, click-to-restore, and window maximize/resize (`SYS_WIN_RESIZE` / `KEV_RESIZE`);
- live compositor theme propagation (`SYS_WM_THEME`, `KEV_THEME`, and `K_TOPIC_THEME` pulses);
- per-user preferences (root's defaults live in `/etc/desktop.conf`);
- transient notifications and keyboard task switching with `Alt-Tab`/`Alt-F4`.

The graphical suite includes Settings, System Monitor (`sysmon`, adding a 4th
**Flight Deck** tab with `Events`, `Sockets`, and `Caps & Parcels` sub-views
backed by `/dev/flight`, `/dev/sockets`, `/dev/caps`, and `/dev/parcels`
`@krec` tables), Calculator (`gcalc`), Terminal (`terminal`), Files (`gfiles`),
Clock (`gclock`), Paint (`gpaint`), Notes (`notes`), Sight (`sight`), About
(`about`), and Kestrel Browser (`browser`, with 16x16 tab favicons, `Ctrl-U`
`view-source:`, per-tab zoom `90%`–`150%`, `/home/.browser_session` restore,
interactive `<details>`/`<dialog>`, Web Crypto, Canvas `ImageData`/gradients,
and `/doc/flightdeck.html`). All use `libgui` 2.0 and adopt `SYS_WM_THEME` /
`KEV_THEME` updates immediately without polling config files or restarting.

Window resize support uses `SYS_WIN_RESIZE` (77), which reallocates/remaps the
window's shared pixel backing in `kernel/wm.c` up to `USER_WIN_STRIDE` (16 MiB)
and delivers `KEV_RESIZE` with the new width/height to the owning process so
applications such as `terminal` and `notes` reflow dynamically.

### Permissions and KFS v3 redo journaling

When multi-user permissions arrived in KFS v2, the inode shrank the direct block
table from 12 entries to 10 to fit `uid`/`gid`/`mode`/`mtime` while bumping
the superblock magic so older images are cleanly rejected rather than misread.
KFS v3 (`KFS3`, `0x3353464B`) extends the on-disk format further with a
checksummed 33-block full-data and metadata redo journal, recursive FIFO ticket
mutex (`fs_lock` in `kernel/kfs.c`) serialization for multi-CPU fairness, and
atomic cross-directory `SYS_RENAME` (71).

Permission checks live in the VFS, not in KFS: the filesystem stores the
metadata, the VFS enforces it against the calling task's credentials, and root
(uid 0) bypasses the checks.

### TCP is a real implementation, not a shortcut

Sequence arithmetic uses wrapping comparisons, retransmission backs off, the
pseudo-header checksum is verified on receive, out-of-order segments are queued
and reassembled (`kernel/tcp_reassembly.c`), Selective Acknowledgements (SACK)
are parsed and generated, and `RST` is honoured. Connection pooling in
`libweb/http.c` reuses persistent HTTP/1.1 Keep-Alive sockets across subresource
requests on the same origin.

### There is no Chrome, and there cannot be

Running Chrome means implementing the Linux syscall ABI, glibc, X11 or Wayland,
GPU drivers, and a JIT-capable memory model — vastly more work than this entire
operating system, and none of it would teach anything about how an OS works. The
honest equivalent is to write our own browser: HTTP/1.1 Keep-Alive over our own
TCP and TLS 1.3, our own HTML5 tokenizer, CSS Grid/Flexbox layout, ES6+ runtime,
and Canvas 2D surface rendered into a window by our own compositor. That is what
`browser` is.

## Deliberately bounded

The implementation now includes xAPIC SMP (`proc_mark_current_idle()` for BSP
idle accounting), demand paging with a dynamically sized raw swap extent
(`SWAP_MAX_PAGES` = 4096 past KFS superblock), Kernel Pulses, Talon typed
clipboard parcels, Avionics Flight Recorder (`/dev/flight` + `/bin/flight`),
Aerie live `@krec` kernel tables (`/dev/procs`, `/dev/sockets`, `/dev/caps`,
`/dev/parcels`), Wave 9 DNS TTL cache and network `@krec` telemetry
(`/dev/net`, `/dev/arp`, `/dev/dns` consumed by `/bin/netstat`), capability-scoped
process spawn and monotonic sandboxing (`SYS_SPAWN_CAPS`, `SYS_SANDBOX`),
traditional signals, a dynamic linker, KFS v3 data+metadata journaling and
atomic rename, live window resize and theme broadcasting, TCP out-of-order
reassembly + SACK, verified 128-SAN TLS 1.3, and a kernel ChaCha20 CSPRNG. Its
boundaries remain intentional: legacy-PIC IRQ routing is BSP-centric, the
scheduler uses one global run queue, the dynamic loader accepts only Kestrel's
supported ABI/relocations, KFS transactions are capped at 32 blocks (+1 commit
block), networking is IPv4-only, and none of the security code has had a
professional audit.
