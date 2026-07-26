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

Applications get a small set of syscalls (create, flush, event, move, destroy)
and a userspace widget toolkit is layered on top in libgui. The window manager
policy — stacking, focus, dragging, the close box — is kernel-side too, which is
the classic tradeoff: less flexible, much simpler, and impossible to deadlock.

### Permissions are an on-disk format change

The KFS inode had no room for uid/gid/mode/mtime, so v2 shrinks the direct block
table from 12 entries to 10 to make space, and the superblock magic changed so a
v1 image is rejected with a clear message instead of being silently misread. The
maximum file size drops from ~4.2 MB to ~4.2 MB (the indirect block dominates),
which is irrelevant at this scale.

Permission checks live in the VFS, not in KFS: the filesystem stores the
metadata, the VFS enforces it against the calling task's credentials, and root
(uid 0) bypasses the checks.

### TCP is a real implementation, not a shortcut

Sequence arithmetic uses wrapping comparisons, retransmission backs off, the
pseudo-header checksum is verified on receive, and RST is honoured. What it does
*not* do is reassemble out-of-order segments — anything not exactly at `rcv_nxt`
is dropped and left to the peer's retransmit. That is a real limitation and it is
documented rather than hidden; over a local NAT it never triggers.

### There is no Chrome, and there cannot be

Running Chrome means implementing the Linux syscall ABI, glibc, X11 or Wayland,
GPU drivers, and a JIT-capable memory model — vastly more work than this entire
operating system, and none of it would teach anything about how an OS works. The
honest equivalent is to write our own browser: HTTP/1.1 over our own TCP, our own
HTML tokenizer and layout, rendered in a window by our own compositor. That is
what `browser` is.

## Deliberately out of scope

Single CPU (no SMP or per-CPU locking), cooperative-ish kernel preemption rather
than fully reentrant kernel paths, no swap or demand paging from disk, no
signals (process termination is a direct kill), no dynamic linking, no journal in
the filesystem, and no cryptography beyond a password hash — nothing here should
be relied on for secrecy.
