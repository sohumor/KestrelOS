# KestrelOS

A small but complete operating system written from scratch: no third-party
code, no GRUB, no ported libc — every byte from the boot sector up is in this
repository, including the tools that build the disk image.

Target: **x86-64** (64-bit long mode), BIOS boot. Verified booting in QEMU and
in real VirtualBox; the raw image also works with VMware and Bochs.

```
kestrel:/$ ping 10.0.2.2
PING 10.0.2.2 (10.0.2.2): 4 probes
reply from 10.0.2.2: seq=0 time=0ms
...
4 sent, 4 received, 0% loss
kestrel:/$ nslookup example.com
example.com -> 104.20.23.154
```

## What's in it

**Boot** — two-stage BIOS bootloader. Stage 1 fits in the 512-byte MBR and
pulls in stage 2 with INT 13h LBA reads; stage 2 enables A20, collects the
E820 memory map, loads the kernel to 1 MiB through unreal mode, builds the
initial page tables and enters long mode directly from real mode.

**Kernel** — a monolithic higher-half kernel at `0xFFFFFFFF80100000`:
VGA text console with an ANSI/VT100 escape parser, COM1 serial console,
64-bit GDT/IDT/TSS, exception handling with register dumps, PIC, PIT,
PS/2 keyboard, CMOS real-time clock, power control, and an in-kernel rescue
console for when userspace can't start.

**Memory** — bitmap physical page allocator driven by the E820 map, 4-level
paging with a full direct map of physical memory, per-process address spaces,
and a slab-style kernel heap.

**Processes** — preemptive round-robin scheduling on the timer tick, kernel
threads, ring-3 user processes with per-task FPU/SSE state, an ELF64 loader,
and a 28-call `int 0x80` syscall interface.

**Storage** — ATA PIO disk driver and **KFS**, a custom inode-based filesystem
with directories, indirect blocks, and full read/write/create/delete, plus
host-side `mkfs.py` and `kfsck.py`.

**Networking** — PCI enumeration, two NIC drivers (RTL8139 and Intel e1000)
behind a common interface, and a from-scratch Ethernet / ARP / IPv4 / ICMP /
UDP stack with a DNS resolver.

**Userspace** — a small libc (crt0, syscalls, stdio, malloc, string, line
editing) and 38 programs: an interactive shell with history, tab completion
and redirection; coreutils; a full-screen text editor; a network toolkit;
and a snake game.

## Building

Needs a Linux environment (WSL2 Ubuntu works) with `gcc`, `nasm`, `make`,
`python3`, and `qemu-system-x86`:

```bash
sudo apt install build-essential nasm make python3 qemu-system-x86
```

```bash
make            # build build/os.img (bootable raw disk image)
make run        # boot it in QEMU (graphical window + serial on stdio)
make test       # 28-test end-to-end suite, headless, driven over serial
make vm-images  # convert to VirtualBox .vdi / VMware .vmdk
make help       # all targets
```

## Running it in a VM

**QEMU**:

```bash
qemu-system-x86_64 -drive file=build/os.img,format=raw -m 256M \
  -device e1000,netdev=n0 -netdev user,id=n0 -serial stdio
```

**VirtualBox** — `make vm-images`, then create a VM of type "Other/Unknown
(64-bit)", attach `build/kestrel.vdi` to the IDE controller, and set the
network adapter to **Intel PRO/1000 MT Desktop (82540EM)** so the e1000
driver finds it.

**VMware** — attach `build/kestrel.vmdk` to a 64-bit "Other" VM.

See [docs/RUNNING.md](docs/RUNNING.md) for step-by-step instructions,
the full command reference, and troubleshooting.

## Repository layout

```
boot/     stage 1 + stage 2 bootloader (NASM)
kernel/   the kernel (C + a little NASM)
abi/      the user/kernel ABI shared by both sides
libc/     userspace C library
apps/     userspace programs (each becomes /bin/<name>)
rootfs/   files copied into the disk image
tools/    mkfs, image builder, fsck, test harness, screenshot capture
docs/     architecture, ABI, filesystem, networking, running, testing
```

## Documentation

| Document | Contents |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | how the whole system fits together |
| [docs/ABI.md](docs/ABI.md) | syscall table, memory map, libc surface |
| [docs/kfs.md](docs/kfs.md) | the KFS on-disk format |
| [docs/net.md](docs/net.md) | network stack and NIC drivers |
| [docs/RUNNING.md](docs/RUNNING.md) | building and running in each hypervisor |
| [docs/testing.md](docs/testing.md) | the end-to-end test harness |

## Disk image layout

| LBA sectors | Contents |
|---|---|
| 0 | stage 1 boot sector (MBR) |
| 1–63 | stage 2 loader |
| 64–2047 | kernel (flat binary, ≤ 992 KiB) |
| 2048+ | KFS filesystem |

## Virtual memory layout

| Virtual address | Contents |
|---|---|
| `0x0000000000400000` | user program text/data (per process) |
| `0x00007FFFFFFFE000` | top of user stack (per process) |
| `0xFFFF800000000000` | direct map of all physical memory |
| `0xFFFFFFFF80100000` | kernel image (physical 1 MiB) |

## Known limitations

Single CPU (no SMP), no TCP and no DHCP (static addressing), no swap, no
users or permissions, no dynamic linking, and KFS has no journal. These are
scope choices, not bugs — the goal was one complete, readable path from the
boot sector to a shell prompt.
