# Building and running KestrelOS

Everything here is verified against the sources in this repository:
`Makefile`, `tools/e2e.py`, `kernel/main.c`, `kernel/net.c` and `apps/*.c`.

- [Prerequisites](#prerequisites)
- [Building](#building)
- [Running in QEMU](#running-in-qemu)
- [Running in VirtualBox](#running-in-virtualbox)
- [Running in VMware](#running-in-vmware)
- [Running in Bochs](#running-in-bochs)
- [Running the tests](#running-the-tests)
- [Expected first-boot output](#expected-first-boot-output)
- [Shell commands](#shell-commands)
- [Troubleshooting](#troubleshooting)

---

## Prerequisites

The build is a Linux build. On Windows use **WSL2 with Ubuntu**; the
repository can live on the Windows filesystem and be built from
`/mnt/c/...`.

```sh
sudo apt update
sudo apt install build-essential nasm make python3 qemu-system-x86
```

| Package            | Provides                                    |
|--------------------|---------------------------------------------|
| `build-essential`  | `gcc`, `ld`, `objcopy`, `make`              |
| `nasm`             | assembles the bootloader, ISR stubs, `crt0` |
| `make`             | drives the build                            |
| `python3`          | `tools/mkfs.py`, `mkimage.py`, `e2e.py`, `kfsck.py` |
| `qemu-system-x86`  | `qemu-system-x86_64` for `make run` / `make test` |

Optional extras:

| Package        | Needed for                                             |
|----------------|--------------------------------------------------------|
| `qemu-utils`   | `qemu-img`, used by `tools/mkvm.sh` to make a VMDK      |
| `zip`          | the compressed image produced by `tools/mkvm.sh`        |
| VirtualBox     | `VBoxManage`, used by `tools/mkvm.sh` to make a VDI      |

No third-party libraries are used or needed — the kernel, the C library
and every application in this tree are built from source in this
repository.

## Building

From the repository root, inside Linux/WSL:

```sh
make                # -> build/os.img, a raw bootable disk image
```

From Windows PowerShell without entering a WSL shell:

```powershell
wsl -d Ubuntu -- sh -c 'cd /mnt/c/Users/<you>/OperatingSystem && make'
```

What `make` does, in order:

1. compiles `kernel/*.c` and assembles `kernel/*.asm`, links them with
   `kernel/linker.ld` into `build/kernel.elf`, then `objcopy -O binary`
   into `build/kernel.bin`;
2. assembles `boot/stage1.asm` (exactly 512 bytes) and `boot/stage2.asm`
   (told the kernel size in sectors via `-DKERNEL_SECTORS=`);
3. compiles `libc/*` and every `apps/*.c` into a static ELF64 binary
   linked at `0x400000` with `apps/user.ld`;
4. copies `rootfs/` plus the app binaries (into `/bin`) into a staging
   tree and runs `tools/mkfs.py` to build a 32 MiB `build/fs.img`;
5. concatenates stage 1, stage 2, the kernel and the filesystem into
   `build/os.img` with `tools/mkimage.py`.

Useful targets (`make help` prints the same list):

| Target             | Effect                                                |
|--------------------|-------------------------------------------------------|
| `make`             | build `build/os.img`                                  |
| `make run`         | boot in QEMU, VGA window plus serial on stdio         |
| `make run-headless`| boot in QEMU, serial only                             |
| `make test`        | full end-to-end suite in headless QEMU                |
| `make smoke`       | boot-only quick check                                 |
| `make fsck`        | validate and list `build/fs.img`                      |
| `make screenshot`  | capture the VGA console to a PNG                      |
| `make vm-images`   | run `tools/mkvm.sh` (VDI / VMDK / zip)                |
| `make clean`       | remove `build/`                                       |

## Running in QEMU

QEMU is the reference target: it emulates the RTL8139 NIC the network
stack drives, so networking works there.

```sh
make run
```

or explicitly:

```sh
qemu-system-x86_64 \
    -drive file=build/os.img,format=raw -m 256M -no-reboot \
    -device rtl8139,netdev=n0 -netdev user,id=n0 \
    -serial stdio
```

Convenience launchers are provided that print the exact command before
running it:

```sh
sh run-qemu.sh                    # Linux / WSL2
sh run-qemu.sh --headless
sh run-qemu.sh --memory 512M --image build/os.img
```

```powershell
.\run-qemu.ps1                    # Windows PowerShell
.\run-qemu.ps1 -Headless
.\run-qemu.ps1 -Memory 512M
```

`run-qemu.ps1` looks for `qemu-system-x86_64` on `PATH`, then in
`C:\Program Files\qemu`, and tells you how to install it if it is
missing. The image itself still has to be built from WSL.

Both launchers pass any additional arguments straight through to QEMU,
so e.g. `sh run-qemu.sh -- -monitor telnet:127.0.0.1:4444,server,nowait`
works.

Notes on the flags:

- `-serial stdio` attaches the serial console to your terminal. The
  kernel mirrors all console output to COM1 and accepts input from it,
  so the OS is fully usable over serial (this is how the test harness
  drives it).
- `-device rtl8139,netdev=n0 -netdev user,id=n0` gives the guest the one
  NIC it knows how to drive, behind QEMU user-mode networking.
- `-no-reboot` makes a triple fault stop QEMU instead of looping.
- `-m 256M` is plenty; the kernel sizes itself from the E820 map.

Ctrl-C in the terminal kills QEMU. In the VGA window use the QEMU menu
or close the window.

## Running in VirtualBox

Convert the raw image, then attach it to a new VM of type
**Other / Unknown (64-bit)** with an IDE controller:

```sh
sh tools/mkvm.sh          # or: make vm-images
# -> build/kestrel.vdi
```

or by hand:

```sh
VBoxManage convertfromraw build/os.img build/kestrel.vdi --format VDI
```

Settings that matter:

- System -> Enable I/O APIC: fine either way; the kernel uses the legacy
  8259 PIC.
- Storage: attach `kestrel.vdi` to the **IDE** controller as the primary
  master. The disk driver is ATA PIO on the primary bus (`kernel/ata.c`)
  and will not find a SATA/AHCI or NVMe disk.
- Network: **VirtualBox does not emulate an RTL8139.** Everything except
  networking works; `ping`, `nslookup` and `udp` will report that the
  network is unavailable.

## Running in VMware

```sh
sh tools/mkvm.sh          # or: make vm-images
# -> build/kestrel.vmdk
```

or by hand:

```sh
qemu-img convert -O vmdk build/os.img build/kestrel.vmdk
```

Create a VM with guest type "Other 64-bit", remove the default disk and
attach `kestrel.vmdk` as an **IDE** disk. The same NIC caveat as
VirtualBox applies.

## Running in Bochs

Bochs is useful because its built-in debugger can single-step the
bootloader. A minimal `bochsrc`:

```
megs: 256
romimage: file=/usr/share/bochs/BIOS-bochs-latest
vgaromimage: file=/usr/share/bochs/VGABIOS-lgpl-latest
ata0-master: type=disk, path="build/os.img", mode=flat
boot: disk
com1: enabled=1, mode=file, dev=bochs-serial.log
display_library: x
```

```sh
bochs -q -f bochsrc
```

Bochs has no RTL8139 either, so networking is unavailable there too.

## Running the tests

```sh
make test           # equivalent to: python3 tools/e2e.py
make smoke          # boot + shell prompt only
python3 tools/e2e.py --list      # list test names
python3 tools/e2e.py --selftest  # check the harness itself, no image needed
```

`tools/e2e.py` boots `build/os.img` in headless QEMU
(`-display none -serial stdio`), waits for `KESTREL READY`, then drives
the shell over the serial console. It is pure Python 3 standard library.
Exit code 0 means everything passed (SKIPs allowed); 1 means a failure,
in which case the last 40 lines of serial output are printed.

The suite covers boot, the shell prompt, `help`, `echo`, `ls /bin`,
`cat /etc/motd`, a `writefile`/`cat` filesystem round trip, `ps`,
`free`, `ping`, `nslookup` and `uptime`. Network tests report SKIP
rather than FAIL when the host has no outbound connectivity.

See [docs/testing.md](testing.md) for the harness internals.

To validate the generated filesystem image on its own:

```sh
make fsck           # python3 tools/kfsck.py -l build/fs.img
```

## Expected first-boot output

On the serial console (and, in colour, on the VGA text console) a
healthy boot looks like this. Numbers vary with the machine; the exact
strings come from `kernel/main.c` and the subsystem initialisers.

```
  KestrelOS 0.1.0 (x86-64)
  from-scratch kernel booted in long mode

mem: 255 MiB usable (6 E820 entries)
irq: gdt/idt/pic up, timer 100 Hz, keyboard + serial input
fpu: x87 + SSE enabled (FXSAVE state per task)
mem: pmm 64000 pages free, paging rebuilt, heap ready
proc: scheduler online
ata: primary master: QEMU HARDDISK, 65536 sectors (32 MiB)
vfs: root filesystem mounted
pci: 00:00.0 vendor 8086 device 1237 class 06.00
...
pci: 5 device function(s) found
rtl8139: io 0xc000 irq 11 mac 52:54:00:12:34:56
net: up, mac 52:54:00:12:34:56
net: ip 10.0.2.15/24 gw 10.0.2.2 dns 10.0.2.3 (static)
syscall: int 0x80 dispatcher + user fault handler installed

KESTREL READY

```

Then PID 1 (`/bin/init`) prints `/etc/motd` — an ASCII kestrel and the
line `KestrelOS -- a from-scratch x86-64 OS` — and spawns `/bin/sh`,
which gives you:

```
kestrel:/$
```

Type `help` for the command list. `init` restarts the shell whenever it
exits, so `exit` (or ctrl-D on an empty line) just gives you a fresh
prompt.

The networking addresses above are static, hard-coded in
`kernel/net.c` to match QEMU user-mode networking defaults
(guest 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3). There is no DHCP
client. On a hypervisor without an RTL8139 you will instead see
`net: no rtl8139 found, networking disabled` and the boot continues
normally.

## Shell commands

The shell (`apps/sh.c`) prompts with `kestrel:<cwd>$`, splits the line on
whitespace into at most 16 tokens (double quotes group words), and
resolves a command name as an absolute path, then `<cwd>/name`, then
`/bin/name`. Paths may be absolute or relative to the current directory.

`/bin/help` is the authoritative in-system list; run it after any change
to `apps/`.

### Builtins

| Builtin       | Description                                          |
|---------------|------------------------------------------------------|
| `cd [dir]`    | change directory; no argument means `/`              |
| `pwd`         | print the current directory                          |
| `exit [code]` | leave the shell (init immediately restarts it)       |
| `help`        | one-line reminder of the builtins                    |

### Programs in `/bin`

| Command                              | Description                                             |
|--------------------------------------|---------------------------------------------------------|
| `help`                               | list every command with a one-line description          |
| `ls [dir]`                           | list a directory: type, size, name; directories first   |
| `cat <file>...`                      | print file contents                                     |
| `head [-n N] [file...]`              | first N lines (default 10); reads the console with no file |
| `wc [-l] [-w] [-c] [file...]`        | count lines, words and bytes                            |
| `hexdump <file>...`                  | canonical hex + ASCII dump, 16 bytes per row            |
| `echo <words...>`                    | print the arguments joined by spaces                    |
| `touch <file>...`                    | create empty files (KFS has no timestamps)              |
| `cp <src> <dst>`                     | copy a file; `<dst>` may be a directory                 |
| `mv <src> <dst>`                     | move/rename a file (copy + unlink; KFS has no rename)   |
| `rm <path>...`                       | remove files                                            |
| `mkdir <path>...`                    | create directories                                      |
| `writefile <path>`                   | type text into a file, finish with ctrl-D               |
| `edit [file]`                        | full-screen text editor; ctrl-S saves, ctrl-Q quits     |
| `ps`                                 | list processes: PID, state, name                        |
| `free`                               | physical memory usage in KiB and MiB                    |
| `uptime`                             | time since boot as `h:mm:ss`                            |
| `sysinfo`                            | neofetch-style summary: logo, kernel, memory, net, uptime |
| `clear`                              | clear the screen                                        |
| `ping <host> [count]`                | ICMP echo round-trip time (default 4 packets)           |
| `nslookup <name>`                    | resolve a hostname via DNS                              |
| `udp send <ip> <port> <message...>`  | send one UDP datagram (source port 40000)               |
| `udp listen <port>`                  | print incoming datagrams until `q`                      |
| `snake`                              | the classic snake game; arrows steer, `q` quits, `r` restarts |

`init` is also in `/bin` but is PID 1, started by the kernel; you do not
run it from the shell.

Two applications take over the whole screen:

- **`edit`** — rows 1-23 are text with horizontal scrolling, row 24 is an
  inverse status bar, row 25 is the message line. Ctrl-S saves, ctrl-Q
  quits (press twice if the buffer is modified).
- **`snake`** — a bordered 78x22 playfield on an 80 ms tick. Arrow keys
  steer, walls kill, food grows the snake, `q` quits, `r` restarts after
  death.

Both work on the VGA console and over serial, since the console layer
understands the ANSI sequences the libc `term_*` helpers emit.

## Troubleshooting

### Black screen / nothing happens after "Booting from Hard Disk"

- **Wrong disk controller.** The ATA PIO driver only probes the primary
  bus. Attach the image as an IDE primary master; SATA/AHCI, SCSI, USB
  and NVMe are not supported. In QEMU this is what plain
  `-drive file=...,format=raw` gives you.
- **Stale or partial image.** Run `make clean && make`. `tools/mkimage.py`
  refuses to build an oversized image, so a successful build means stage 1
  is exactly 512 bytes, stage 2 fits in LBA 1-63 and the kernel fits in
  LBA 64-2047.
- **No serial output either.** Boot with `-serial stdio` and look at the
  first lines. If not even `KestrelOS 0.1.0` appears, the failure is in
  the bootloader; Bochs with its debugger is the fastest way to see where.
- **A 32-bit-only host or emulator.** The kernel requires x86-64 long
  mode. `qemu-system-x86_64` (not `qemu-system-i386`) is required.
- **VM refuses to boot the disk.** Make sure the VM is set to boot from
  hard disk and that the firmware is **BIOS/legacy**, not pure UEFI —
  there is no UEFI loader, only an MBR boot sector.

### No network / `ping` and `nslookup` fail

- **The hypervisor has no RTL8139.** Look for
  `net: no rtl8139 found, networking disabled` in the boot log. Only QEMU
  is known to provide one (`-device rtl8139,netdev=n0 -netdev user,id=n0`).
  VirtualBox, VMware and Bochs do not, and networking is simply absent
  there.
- **The addresses are static.** `kernel/net.c` hard-codes 10.0.2.15/24,
  gateway 10.0.2.2, DNS 10.0.2.3 — the QEMU user-networking defaults.
  Any other network topology needs those constants changed. There is no
  DHCP client.
- **`ping` to an outside host times out.** QEMU user-mode networking does
  not forward ICMP to the internet on every host/platform. `ping 10.0.2.2`
  (the built-in gateway) is the reliable check; the test harness treats an
  external ping failure as a SKIP for the same reason.
- **Check what the kernel thinks.** `sysinfo` prints the NIC MAC and the
  configured addresses, so you can tell "no NIC" from "NIC but no route".

### `vfs: WARNING: no root filesystem` / disk not found

- The kernel could not read a valid KFS superblock from LBA 2048 of the
  primary master. Confirm the boot log line
  `ata: primary master: ..., N sectors` is present: if it says
  `ata: primary master not present` or `ata: no controller on primary bus`,
  the problem is the controller attachment (see above), not the filesystem.
- If ATA is fine but the FS is not, the image may have been built without
  the filesystem stage. Rebuild with `make clean && make` and verify with
  `make fsck`, which runs `tools/kfsck.py` over `build/fs.img` and lists
  the tree.
- A diskless boot is survivable: the kernel prints
  `vfs: no root filesystem, running diskless` and continues, but `init`
  cannot be loaded, so you get no shell.

### `sh: command not found: <name>`

The binary is not in `/bin`. `ls /bin` shows what was baked into the
image; every `apps/*.c` becomes `/bin/<name>` at build time, so a new app
needs a rebuild of the filesystem image (`make`) and a fresh boot.

### The keyboard behaves strangely after using ctrl

Prefer the serial console (`-serial stdio`) if the VGA keyboard gets into
a bad state; a reboot clears it.

### `make test` fails but the OS boots fine by hand

- Make sure `qemu-system-x86_64` is on `PATH` — `tools/e2e.py` invokes it
  by name.
- Run it from the repository root; the harness uses paths relative to the
  current directory.
- `python3 tools/e2e.py --selftest` verifies the harness plumbing without
  an image; if that fails, the problem is the host, not the OS.
