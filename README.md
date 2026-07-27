# KestrelOS

A test to see how good Claude Fable 5 is, this took around 5-6 hours, over 120 sub-agents and around 6 million tokens. Thanks to Marcy for giving me the idea, much love. 

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
with a full direct map, per-process address spaces, and a kernel heap.

**Processes** — preemptive scheduling, kernel threads, ring-3 processes with
per-task FPU state and credentials, an ELF64 loader, pipes, I/O redirection,
process control, and a 50-plus call syscall interface.

**Storage** — an ATA driver and **KFS**, a custom filesystem with directories,
indirect blocks, and per-file ownership, permissions and timestamps, enforced
by the VFS against each task's credentials. Plus `/dev` with real device files.

**Networking** — PCI enumeration, two NIC drivers (RTL8139 and Intel e1000)
behind a common interface, and a from-scratch stack: Ethernet, ARP, IPv4, ICMP,
UDP, **TCP** and DNS, with an HTTP/1.1 client on top.

**Desktop** — a kernel compositor where each window is an object whose pixel
buffer is mapped into the owning process, with stacking, focus, draggable title
bars and routed input; a userspace widget toolkit; and a desktop shell with a
taskbar, a graphical terminal, file manager, clock, paint program and browser.

**System** — multi-user logins with salted iterated SHA-256 password hashes
(written from the specification), a service-supervising init with dependency
ordering and restart backoff, a package manager with dependency resolution and
integrity verification, and around 70 userspace programs.

## Building

Needs a Linux environment (WSL2 Ubuntu works):

```bash
sudo apt install build-essential nasm make python3 qemu-system-x86
```

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
  -device VGA,vgamem_mb=32 -device e1000,netdev=n0 -netdev user,id=n0
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
libc/      userspace C library
libgui/    userspace GUI toolkit
apps/      userspace programs (each becomes /bin/<name>)
modules/   loadable kernel modules
packages/  sources for the shipped .kpkg packages
rootfs/    files copied into the disk image
tools/     mkfs, image builder, fsck, font generator, test harness, screenshots
docs/      architecture, ABI, filesystem, networking, drivers, running, testing
```

## Documentation

| Document | Contents |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | how the whole system fits together |
| [docs/MODULARITY.md](docs/MODULARITY.md) | the module system and driver model |
| [docs/DESIGN-desktop.md](docs/DESIGN-desktop.md) | why the desktop is built the way it is |
| [docs/ABI.md](docs/ABI.md) | syscall table, memory map, libc surface |
| [docs/kfs.md](docs/kfs.md) | the KFS on-disk format |
| [docs/net.md](docs/net.md) | network stack and NIC drivers |
| [docs/tls.md](docs/tls.md) | TLS 1.3 client, verification, testing and limits |
| [docs/users.md](docs/users.md) | accounts, password hashing, and its limits |
| [docs/packages.md](docs/packages.md) | the .kpkg format and the package manager |
| [docs/browser.md](docs/browser.md) | the HTML engine and what it does not do |
| [docs/RUNNING.md](docs/RUNNING.md) | building and running in each hypervisor |

## Known limitations

Single CPU (no SMP), no swap or demand paging, no signals, no dynamic linking,
no journal in the filesystem, TCP does not reassemble out-of-order segments,
and TLS supports 1.3 only. The from-scratch TLS code is unaudited, and its
entropy is weak on a CPU without a hardware random source. `libtls` and
`tlstest` can make verified HTTPS requests, but the graphical browser still
uses the old flat renderer and is not wired to the new TLS/HTTP/web stack yet.
The password hashing is honest about being non-cryptographic, because the
random number generator behind the salts is not.

**This cannot run Chrome, and never will.** Chrome needs the Linux syscall ABI,
glibc, X11 or Wayland, GPU drivers and a JIT — far more work than this entire
operating system, none of which would teach anything about how one works. The
browser here is our own, and it renders the real web over our own TCP.
