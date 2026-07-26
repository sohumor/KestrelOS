# KestrelOS

A small but complete operating system written from scratch: no third-party code,
no GRUB, no ported libc — every byte from the boot sector up is in this repository.

Target: **x86-64** (64-bit long mode), BIOS boot. Runs in QEMU, VirtualBox,
VMware, Bochs, or on real hardware from a raw disk image.

## Features

- **Boot**: two-stage BIOS bootloader (stage 1 fits in the 512-byte MBR; stage 2
  enables A20 + unreal mode, collects the E820 memory map, loads the kernel to
  1 MiB, builds the initial page tables, and enters 64-bit long mode)
- **Kernel**: monolithic higher-half C kernel (linked at `0xFFFFFFFF80100000`) —
  VGA text console, serial console, 64-bit GDT/IDT/TSS, exceptions, PIC,
  PIT timer, PS/2 keyboard
- **Memory**: physical page bitmap allocator, 4-level paging with per-process
  address spaces and a full physical-memory direct map, kernel heap
- **Processes**: preemptive round-robin scheduler, ring-3 usermode, `int 0x80`
  syscall interface, ELF64 loader
- **Storage**: ATA PIO disk driver, block cache, and **KFS** — a custom
  inode-based filesystem with directories, read/write/create/delete
- **Networking**: PCI bus scan, RTL8139 NIC driver, Ethernet/ARP/IPv4/ICMP/UDP
  stack, DNS resolver — `ping` works both ways
- **Userspace**: tiny from-scratch libc, an interactive shell, and a set of
  applications in `/bin` (ls, cat, echo, ps, ping, a text editor, snake, …)

## Building

Requires a Linux environment (WSL2 Ubuntu works) with: `gcc`, `nasm`, `make`,
`python3`, `qemu-system-x86`.

```sh
sudo apt install build-essential nasm make python3 qemu-system-x86
make            # produces build/os.img (raw bootable disk image)
make run        # boot it in QEMU (graphical window + serial on stdio)
make test       # automated end-to-end tests (headless QEMU, drives the shell over serial)
```

## Running in a VM

**QEMU** (recommended):

```sh
qemu-system-x86_64 -drive file=build/os.img,format=raw \
  -device rtl8139,netdev=n0 -netdev user,id=n0 -serial stdio
```

**VirtualBox**: convert the raw image and attach it to an "Other/Unknown
(64-bit)" VM:

```sh
VBoxManage convertfromraw build/os.img kestrel.vdi --format VDI
```

(Networking currently requires an RTL8139 NIC, which VirtualBox does not
emulate — everything else works there.)

## Repository layout

```
boot/     stage 1 + stage 2 bootloader (NASM)
kernel/   the kernel (C + a little NASM)
libc/     userspace C library
apps/     userspace applications (each becomes /bin/<name> on the disk image)
tools/    host-side tools: mkfs.py (builds the KFS image), test harness
docs/     design notes
```

## Disk image layout

| LBA sectors | Contents                     |
|-------------|------------------------------|
| 0           | stage 1 boot sector (MBR)    |
| 1–63        | stage 2 loader               |
| 64–2047     | kernel (flat binary, ≤992 KiB) |
| 2048+       | KFS filesystem               |

## Virtual memory layout

| Virtual address        | Contents                                  |
|------------------------|-------------------------------------------|
| `0x0000000000400000`   | userspace program text/data (per process) |
| `0x00007FFFFFFFF000`   | top of userspace stack (per process)      |
| `0xFFFF800000000000`   | direct map of all physical memory         |
| `0xFFFFFFFF80100000`   | kernel text/data (phys 1 MiB)             |
