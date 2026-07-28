# KestrelOS architecture

How the pieces fit together, from the first instruction the BIOS runs to
a ring-3 process making a system call.

Related documents: [RUNNING.md](RUNNING.md) (build and boot),
[ABI.md](ABI.md) (userspace ABI in detail), [kfs.md](kfs.md) (filesystem
on-disk format), [net.md](net.md) (network stack), [testing.md](testing.md)
(end-to-end harness).

- [Ten-thousand-foot view](#ten-thousand-foot-view)
- [Boot chain](#boot-chain)
- [Disk image layout](#disk-image-layout)
- [Kernel subsystem map](#kernel-subsystem-map)
- [Virtual memory layout](#virtual-memory-layout)
- [Processes and scheduling](#processes-and-scheduling)
- [Process lifecycle](#process-lifecycle)
- [System calls](#system-calls)
- [Filesystem](#filesystem)
- [Network stack](#network-stack)
- [Userspace](#userspace)
- [HOWTO: add a new system call](#howto-add-a-new-system-call)
- [HOWTO: add a new application](#howto-add-a-new-application)

---

## Ten-thousand-foot view

```
   ring 3   +-------------------------------------------------------+
            |  init (pid 1) -> sh -> ls, cat, edit, snake, ping ... |
            |  libc/  (crt0, stdio, stdlib, string, kestrel)        |
            +---------------------------|---------------------------+
                                        |  int 0x80
   ring 0   +---------------------------v---------------------------+
            |  syscall.c  (dispatch, user-memory copy-in/out)       |
            |                                                       |
            |  uproc.c/elf.c   proc.c/switch.asm   vfs.c/kfs.c      |
            |  (ELF64 loader)  (scheduler)         (files)          |
            |                                                       |
            |  pmm.c  vmm.c  kheap.c   net.c/udp.c/dns.c            |
            |  (memory)                (TCP/IP-less IP stack)       |
            |                                                       |
            |  gdt.c idt.c isr.asm pic.c timer.c keyboard.c         |
            |  console.c serial.c  ata.c  pci.c  rtl8139.c  fpu.c   |
            +---------------------------|---------------------------+
                                        |
            +---------------------------v---------------------------+
            |  hardware: CPU, PIT, 8259 PIC, PS/2, VGA text, COM1,  |
            |  ATA primary bus, PCI, RTL8139                        |
            +-------------------------------------------------------+
```

## Boot chain

```
 BIOS
   |  reads LBA 0 into 0x7C00, DL = boot drive, jumps there
   v
 boot/stage1.asm            16-bit real mode, ORG 0x7C00, exactly 512 bytes
   |  - checks for INT 13h extensions (LBA reads)
   |  - INT 13h AH=42h: LBA 1..63  ->  0x7E00
   v  - far jump 0x0000:0x7E00
 boot/stage2.asm            16-bit -> 32-bit -> 64-bit
   |  1. enable A20, enter unreal mode (32-bit segment limits in real mode)
   |  2. INT 15h E820 memory map  -> bootinfo block at 0x6000
   |        u16 e820_count; u8 boot_drive; entries (24 B each) at +8
   |  3. load the kernel: INT 13h reads from LBA 64 in 32-sector chunks
   |     into the 16 KiB buffer at 0x20000, copied up to 0x100000 (1 MiB)
   |  4. build identity + higher-half page tables with 2 MiB pages:
   |        PML4    0x70000
   |        PDPT_LO 0x71000    identity map of low memory
   |        PD_LO   0x72000
   |        PDPT_HI 0x73000    0xFFFFFFFF80000000 -> phys 0
   |  5. load GDT, set CR4.PAE, EFER.LME, CR0.PG -> long mode
   v  6. jump to 0xFFFFFFFF80100000 with rdi = 0x6000 (bootinfo phys)
 kernel/entry.asm           _start: set up the boot stack, zero .bss,
   |                        call kmain(bootinfo_phys)
   v
 kernel/main.c  kmain()
```

`kmain()` brings subsystems up in a fixed order — each one depends on
everything above it:

```
serial_init(); console_init();          output as early as possible
   |
E820 summary from boot_info (P2V of the 0x6000 block)
   |
gdt_init()      64-bit GDT + TSS (kernel/user code/data, ring-3 entry)
idt_init()      256-entry IDT, exception + IRQ stubs from isr.asm
pic_init()      8259 remapped: IRQ 0-15 -> vectors 0x20-0x2F
timer_init(100) PIT at TIMER_HZ = 100 Hz, drives preemption
keyboard_init() PS/2 scancode set 1 -> the shared input queue
serial_init_irq()  COM1 RX interrupts: the serial line is a real console
sti()
   |
fpu_init()      x87 + SSE on, FXSAVE area saved/restored per task
pmm_init()      physical page bitmap built from the E820 map
vmm_init()      real 4-level page tables: kernel higher half + direct map
kheap_init()    kernel heap on top of the pmm
   |
proc_init()     the boot context becomes task 0; scheduler armed
   |
ata_init()      identify the ATA primary master
vfs_init()      mount KFS from LBA 2048 (may fail -> diskless)
   |
net_init()      PCI scan, RTL8139 bring-up, static IP config
syscall_init()  install the int 0x80 dispatcher and ring-3 fault handler
   |
"KESTREL READY"
   |
kthread_create(init_launcher)  -> uproc_spawn("/bin/init")
   |
boot task parks in  for (;;) { sti; hlt; yield(); }
```

## Disk image layout

Built by `tools/mkimage.py` from the stage binaries, `kernel.bin` and
`fs.img`. Sectors are 512 bytes.

```
 LBA 0        +-------------------------------+
              | stage 1 boot sector (MBR)     |  exactly 512 B, ends 0xAA55
 LBA 1        +-------------------------------+
              | stage 2 loader                |  <= 63 sectors
 LBA 64       +-------------------------------+
              | kernel.bin (flat binary)      |  <= 1984 sectors (992 KiB)
 LBA 2048     +-------------------------------+  FS_START_LBA (kernel/include/ata.h)
              | KFS filesystem                |  4096-byte blocks = 8 sectors
              +-------------------------------+
```

## Kernel subsystem map

| Area | Files | Responsibility |
|------|-------|----------------|
| Entry / CPU | `kernel/entry.asm`, `cpu.asm`, `isr.asm`, `switch.asm`, `usermode.asm` | long-mode entry, port/CR helpers, ISR stubs, context switch, `iretq` into ring 3 |
| Descriptors | `gdt.c`, `idt.c` | GDT (`SEL_KCODE 0x08`, `SEL_KDATA 0x10`, `SEL_UCODE 0x18`, `SEL_UDATA 0x20`, `SEL_TSS 0x28`), TSS with the kernel stack for ring transitions, 256 IDT gates, vector `0x80` at DPL 3 |
| Interrupts | `pic.c`, `timer.c` | 8259 remap to 0x20-0x2F, EOI, PIT at 100 Hz |
| Console | `console.c`, `serial.c`, `kprintf.c` | VGA text buffer with colour + ANSI handling, COM1 in/out, formatted kernel printing, `panic()` |
| Input | `keyboard.c`, `input.c` | PS/2 scancode decode (modifiers, extended keys) and COM1 keys funnelled into one blocking input queue with `KEY_*` codes |
| FPU | `fpu.c` | enable x87/SSE, per-task FXSAVE area |
| Memory | `pmm.c`, `vmm.c`, `kheap.c` | physical page bitmap allocator; 4-level paging, per-process PML4s, full physical direct map; kernel heap (`kmalloc`/`kzalloc`/`kfree`) |
| Processes | `proc.c`, `switch.asm` | task structs, round-robin run queue, `SCHED_QUANTUM = 5` ticks, sleep/wake, zombies and reaping |
| Userspace | `uproc.c`, `elf.c`, `usermode.asm`, `syscall.c` | ELF64 loading, argv packaging on the user stack, ring-3 entry, `int 0x80` dispatch, bounded user-memory copies, ring-3 fault handler |
| Storage | `ata.c`, `kfs.c`, `vfs.c` | ATA PIO on the primary bus; KFS inode filesystem; thin absolute-path VFS producing `struct file` |
| Network | `pci.c`, `rtl8139.c`, `net.c`, `udp.c`, `dns.c` | PCI config space and bus scan; RTL8139 RX ring + TX descriptors; Ethernet/ARP/IPv4/ICMP; UDP port bindings; DNS A-record resolver |
| Support | `string.c`, `sched_test.c` | freestanding string/memory routines; scheduler self-test threads |

Headers live in `kernel/include/`, one per subsystem, plus `kernel.h`
(`P2V`/`V2P`, `PAGE_SIZE`, `KERNEL_VERSION`) and `bootinfo.h` (the layout
stage 2 writes at 0x6000).

## Virtual memory layout

The kernel builds proper 4-level page tables in `vmm_init()`, replacing
the bootstrap tables. Every address space (`vmm_new_pml4()`) shares the
kernel half; only the user half is private.

| Virtual range | Contents | Flags |
|---------------|----------|-------|
| `0x0000000000400000` | user program text/rodata/data/bss, load base `USER_LOAD_BASE` (`apps/user.ld`) | user, RW |
| ... `user_brk` | user heap grown by `SYS_BRK` | user, RW |
| `0x00007FFFFFFFE000` | `USER_STACK_TOP`; 16 pages (`USER_STACK_PAGES`) grow downward | user, RW |
| *non-canonical hole* | | |
| `0xFFFF800000000000` | `PHYS_MAP_BASE` — direct map of all physical memory; `P2V(p)`/`V2P(v)` | kernel, RW |
| `0xFFFFFFFF80000000` | `KERNEL_OFFSET` — the higher-half window onto physical 0 | kernel |
| `0xFFFFFFFF80100000` | kernel image (physical 1 MiB), `kernel/linker.ld` | kernel |

The direct map is why the kernel can hand out "virtual pointers" to page
tables and DMA buffers: any physical frame is reachable at
`PHYS_MAP_BASE + phys` without a temporary mapping.

```
 0x0000000000000000  +----------------------+
                     |  (unmapped, NULL)    |
 0x0000000000400000  +----------------------+  user text/data
                     |  user heap (brk)     |
                     |          .           |
                     |          v           |
                     |                      |
                     |          ^           |
                     |  user stack, 16 pg   |
 0x00007FFFFFFFE000  +----------------------+  USER_STACK_TOP
                     ::  non-canonical     ::
 0xFFFF800000000000  +----------------------+  direct map of RAM
                     |                      |
 0xFFFFFFFF80100000  +----------------------+  kernel .text/.data/.bss
                     |  kheap, kstacks      |
                     +----------------------+
```

## Processes and scheduling

`struct task` (`kernel/include/proc.h`) is the only execution abstraction;
kernel threads and user processes are the same type, distinguished by the
`user` flag and a populated user half in `pml4`.

```
struct task {
    int pid; char name[32]; enum task_state state;
    uint64_t rsp;             /* saved kernel stack pointer   */
    uint8_t *kstack;          /* 16 KiB kernel stack          */
    void *fpu_state;          /* 16-byte-aligned FXSAVE area  */
    uint64_t *pml4;           /* address space                */
    bool user; uint64_t sleep_until; int exit_code;
    struct task *parent; int wait_child_pid;
    uint64_t user_brk;
    struct file *files[MAX_OPEN_FILES];   /* 16 fds           */
    struct task *qnext, *allnext;         /* run queue, all   */
};
```

States: `TASK_RUNNABLE`, `TASK_RUNNING`, `TASK_SLEEPING`, `TASK_ZOMBIE`
(mirrored to userspace as `K_STATE_*` through `SYS_PSINFO`).

Scheduling is preemptive round robin over a circular `qnext` list. The
PIT tick decrements the current task's slice; after `SCHED_QUANTUM = 5`
ticks (50 ms at 100 Hz) `schedule()` picks the next runnable task,
switches `pml4` if it differs, swaps FPU state, and hands off to the
assembly `ctx_switch` in `kernel/switch.asm` (a brand-new task starts at
`task_bootstrap` in the same file). `yield()` gives the
rest of the slice away voluntarily; `task_sleep_ticks()` parks a task
until `sleep_until` passes and `task_wake_sleepers()` promotes it back to
runnable.

## Process lifecycle

```
  sh:  spawn("/bin/ls", argv)                       [ring 3]
   |     int 0x80, rax = SYS_SPAWN
   v
  syscall_dispatch  ->  uproc_spawn_from_user()     [ring 0]
   |   copy path + up to UPROC_MAX_ARGS (16) args of UPROC_ARG_MAX (128)
   |   bytes each out of user memory into a kernel package
   v
  uproc_spawn()
   |   kthread_create(user_task_thunk, pkg)  -> new task, RUNNABLE
   |   returns the new pid to the caller immediately
   v
  ... scheduler eventually runs the new task ...
   |
  user_task_thunk()
   |   vfs_open + vfs_read the executable into a kernel buffer
   |   pml4 = vmm_new_pml4()          (kernel half pre-populated)
   |   elf_load(pml4, ...)            PT_LOAD segments -> USER_LOAD_BASE,
   |                                  .bss zeroed, entry + initial brk out
   |   map USER_STACK_PAGES (16) pages below USER_STACK_TOP
   |   current->pml4 = pml4; vmm_switch(pml4)
   |   build_user_stack(): argv strings + pointer array on the user stack
   v
  enter_usermode(entry, rsp, argc, argv)     kernel/usermode.asm
   |   iretq with SEL_UCODE/SEL_UDATA (RPL 3), IF set
   v
  libc/crt0.asm _start:  rdi = argc, rsi = argv
   |   align rsp, call main(argc, argv)
   v
  main() returns  ->  crt0 does  int 0x80 with rax = SYS_EXIT
   |
  syscall_dispatch: uproc_record_exit(pid, code); task_exit(code)
   |   state = TASK_ZOMBIE, exit code kept in the waitpid ring,
   |   parent blocked in SYS_WAITPID is woken
   v
  the scheduler reaps the zombie: free kernel stack, FPU area,
  and vmm_destroy_user(pml4) releases the user half and its page tables
```

A ring-3 exception never reaches `panic()`: `user_fault()` in
`kernel/syscall.c` prints the vector, error code, `rip` (and CR2 for page
faults), records exit code `-1` and calls `task_exit(-1)`, so a crashing
process dies alone.

PID 1 is `/bin/init` (`apps/init.c`): it parses `/etc/inittab`, orders
services by `after=`/`requires=`, waits for explicit readiness markers,
applies per-unit restart policy and crash backoff, exposes the `/run` service
control protocol, and performs reverse-order shutdown. The login console is
one supervised unit rather than a special case.

## System calls

Convention (`abi/kestrel_abi.h`, `libc/syscall.asm`): `int 0x80`,
`rax` = number, arguments in `rdi`, `rsi`, `rdx`, `r10`, return value in
`rax`. A negative return (normally `-1`) is an error. Vector `0x80` is an
interrupt gate at DPL 3, so `IF` is clear on entry; the dispatcher
re-enables interrupts immediately because syscalls may block.

| # | Name | Arguments | Returns |
|---|------|-----------|---------|
| 0 | `SYS_EXIT` | `code` | does not return |
| 1 | `SYS_WRITE` | `fd, buf, len` | bytes written; fd 1/2 = console |
| 2 | `SYS_READ` | `fd, buf, len` | bytes read; fd 0 = console, blocks |
| 3 | `SYS_OPEN` | `path, flags` | fd |
| 4 | `SYS_CLOSE` | `fd` | 0 / -1 |
| 5 | `SYS_SPAWN` | `path, argv` | pid |
| 6 | `SYS_WAITPID` | `pid` | exit code, blocks |
| 7 | `SYS_GETPID` | — | pid |
| 8 | `SYS_SLEEP_MS` | `ms` | 0 |
| 9 | `SYS_YIELD` | — | 0 |
| 10 | `SYS_READDIR` | `path, index, struct k_dirent*` | 0, -1 at end |
| 11 | `SYS_UNLINK` | `path` | 0 / -1 |
| 12 | `SYS_MKDIR` | `path` | 0 / -1 |
| 13 | `SYS_STAT` | `path, struct k_stat*` | 0 / -1 |
| 14 | `SYS_BRK` | `addr` (0 = query) | current break |
| 15 | `SYS_UPTIME_MS` | — | ms since boot |
| 16 | `SYS_MEMINFO` | `u64* total_kb, u64* free_kb` | 0 / -1 |
| 17 | `SYS_PSINFO` | `index, struct k_psinfo*` | 0, -1 at end |
| 18 | `SYS_DNS` | `name, u32* ip_be` | 0 / -1 |
| 19 | `SYS_PING` | `ip_be, timeout_ms` | rtt ms / -1 |
| 20 | `SYS_UDP_SEND` | `ip_be, dport<<16\|sport, buf, len` | 0 / -1 |
| 21 | `SYS_UDP_RECV` | `port, buf, maxlen, timeout_ms` | len / -1 |
| 22 | `SYS_NETINFO` | `struct k_netinfo*` | 0, -1 if no NIC |
| 23 | `SYS_SEEK` | `fd, off, whence (0\|1\|2)` | new position |
| 24 | `SYS_READ_NB` | `fd, buf, len` | bytes read, 0 if none ready |
| 25 | `SYS_RTC` | `struct k_rtc*` | 0 / -1 |
| 26 | `SYS_POWER` | `action (0 = reboot, 1 = halt)` | does not return on success |

`open()` flags: `O_RDONLY 0x000`, `O_WRONLY 0x001`, `O_RDWR 0x002`,
`O_CREAT 0x040`, `O_TRUNC 0x200`, `O_APPEND 0x400`.

Every pointer that crosses the boundary goes through the bounded helpers
in `kernel/syscall.c` — `user_range_ok()`, `copy_from_user()`,
`copy_to_user()`, `copy_str_from_user()` — which walk `current->pml4` and
return `-1` on an unmapped or non-user address instead of faulting.

Special keys arriving on fd 0 use values `>= 0x80` (`KEY_UP` .. `KEY_DELETE`);
ordinary keys are ASCII, with ctrl-X as 1..26 and ESC as 27.

Full details, struct layouts and error semantics: [ABI.md](ABI.md).

## Filesystem

```
  app:  open/read/write/seek/readdir/unlink/mkdir/stat
          |  int 0x80
  syscall.c: per-task fd table (MAX_OPEN_FILES = 16) -> struct file*
          v
  vfs.c    absolute paths only; struct file { inum, pos, flags, refs }
          v
  kfs.c    superblock, block bitmap, 64-byte inodes, direct + one
           indirect block, directory entries; metadata redo journal
          v
  ata.c    PIO reads/writes of 512-byte sectors on the primary master;
           FS block B  ->  LBA 2048 + 8*B
```

KFS is a fixed-layout inode filesystem with 4096-byte blocks: block 0 is
the superblock, then the block bitmap, fixed checksummed metadata journal,
inode table, and data. Regular data is ordered write-through; committed
metadata transactions replay at mount.
Host-side tools `tools/mkfs.py` (build an image from a directory tree)
and `tools/kfsck.py` (validate and list) implement the same format.

Full on-disk format: [kfs.md](kfs.md).

## Network stack

```
     task context                              IRQ context
  ping/nslookup/udp                          RTL8139 IRQ
        |  syscalls                                |
        v                                          v
  net_icmp_ping / dns_resolve / udp_send      rx_drain -> net_rx
        |                                          |
        v                                    +-----+-----+
   net_ip_send  --> arp_resolve              |           |
        |            (wait + retry)        ARP         IPv4
        v                                  reply     demux
   rtl8139_send (4 TX descriptors)                +----+----+
                                                  |         |
                                                ICMP      udp_input
                                             echo/reply   (16 port
                                                          bindings)
```

Layers: PCI enumeration (`pci.c`) finds the RTL8139; the driver
(`rtl8139.c`) sets up an RX ring and four TX descriptors; `net.c`
demultiplexes Ethernet, implements ARP (cache, request, reply), IPv4
build/parse and ICMP echo in both directions; `udp.c` provides send plus
16 port bindings with short queues; `dns.c` resolves A records
(compression-pointer aware). Addresses are static, matching QEMU
user-mode networking (10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3).

Full details, including the IRQ-context constraints: [net.md](net.md).

## Userspace

`libc/` is a small freestanding C library built only from this tree:

| File | Contents |
|------|----------|
| `libc/crt0.asm` | `_start`: zero `rbp`, align `rsp`, `main(argc, argv)`, then `SYS_EXIT` |
| `libc/syscall.asm` | the raw `int 0x80` trampoline |
| `libc/kestrel.c` | one wrapper per syscall, `readline()`, `ip_aton`/`ip_ntoa` |
| `libc/stdio.c` | `printf`/`snprintf`/`puts`/`putchar` on top of `SYS_WRITE` |
| `libc/stdlib.c` | `malloc`/`free`/`calloc`/`realloc` on top of `SYS_BRK`, `atoi`/`atol`, `abs`, `rand`/`srand`, `exit` |
| `libc/string.c` | `str*`/`mem*` |

Headers are `libc/include/{kestrel,stdio,stdlib,string}.h`; the terminal
helpers (`term_clear`, `term_goto`, `term_color`, ...) are macros that
emit ANSI escapes, which both the VGA console and the serial console
understand.

Every `apps/*.c` links against that libc with `apps/user.ld` into a
static ELF64 at `0x400000` and is installed as `/bin/<name>` by the
filesystem build step.

The shell appends one extra argv element, `--cwd=<cwd>`, to every command
it spawns, so path-taking applications can resolve relative arguments
themselves; applications that ignore argv are unaffected. Each such app
carries a small `strip_cwd_arg()` helper.

## HOWTO: add a new system call

Worked example: a hypothetical `SYS_HOSTNAME` that copies a string out to
userspace.

1. **Reserve the number in the ABI** — `abi/kestrel_abi.h`, which both
   sides include. Append; never renumber, the numbers are baked into
   compiled binaries.

   ```c
   #define SYS_HOSTNAME  27  /* (buf, len) -> length copied */
   ```

   Add any new struct here too, so kernel and libc always agree.

2. **Implement the kernel side** — `kernel/syscall.c`. Write a `static
   long sys_hostname(uint64_t ubuf, uint64_t len)` next to its siblings
   and validate *everything* that came from userspace:

   ```c
   static long sys_hostname(uint64_t ubuf, uint64_t len)
   {
       const char *name = "kestrel";
       unsigned long n = strlen(name) + 1;

       if (len < n)
           return -1;
       if (copy_to_user((void *)ubuf, name, n) < 0)
           return -1;
       return (long)n - 1;
   }
   ```

   Never dereference a user pointer directly — use `user_range_ok()`,
   `copy_from_user()`, `copy_to_user()` or `copy_str_from_user()`, and
   bound every length before allocating anything.

3. **Wire it into the dispatcher** — add a `case` to the `switch` in
   `syscall_dispatch()`:

   ```c
   case SYS_HOSTNAME:
       ret = sys_hostname(a1, a2);
       break;
   ```

   Arguments arrive as `a1 = rdi`, `a2 = rsi`, `a3 = rdx`, `a4 = r10`.
   The `default` case already prints and returns `-1` for unknown numbers.

4. **Add the libc wrapper** — declare it in `libc/include/kestrel.h` and
   define it in `libc/kestrel.c`:

   ```c
   int hostname(char *buf, unsigned long len)
   {
       return (int)syscall(SYS_HOSTNAME, (long)buf, (long)len, 0, 0);
   }
   ```

5. **Document it** — the syscall table above and `docs/ABI.md`.

6. **Test it** — add a step to `tools/e2e.py` that drives an application
   using it and expects the output, then `make test`.

Constraints worth remembering: the dispatcher runs with interrupts
enabled and is preemptible, so anything it touches must tolerate a
context switch; a syscall may block (input, sleep, waitpid, network
timeouts) but must never `panic()` on user-supplied input.

## HOWTO: add a new application

1. **Create `apps/<name>.c`** with a `main(int argc, char **argv)`. The
   Makefile globs `apps/*.c`, so no build file needs editing.

   ```c
   /* greet.c - say hello. */

   #include <kestrel.h>
   #include <stdio.h>
   #include <string.h>

   int main(int argc, char **argv)
   {
       /* The shell appends "--cwd=<path>"; drop it if you ignore it. */
       if (argc > 1 && strncmp(argv[argc - 1], "--cwd=", 6) == 0)
           argc--;

       printf("hello, %s\n", argc > 1 ? argv[1] : "world");
       return 0;
   }
   ```

2. **Respect the environment**: only `libc/include/*` and
   `abi/kestrel_abi.h` are available — no host headers, no libm, no
   floating point in the kernel-facing paths. Return 0 for success; the
   shell prints `[exit N]` for anything else.

3. **Handle paths** if the app takes filenames: parse the trailing
   `--cwd=<path>` argument and join relative paths against it, the way
   `apps/cat.c` and `apps/ls.c` do. Absolute paths always work.

4. **Build and run**:

   ```sh
   make            # links build/apps/greet and bakes it into /bin
   make run
   ```

   ```
   kestrel:/$ greet kestrel
   hello, kestrel
   ```

   A new binary only appears after the filesystem image is rebuilt, so a
   plain rebuild plus a fresh boot is required — there is no way to add a
   binary to a running system.

5. **List it in `help`** — add a row to the `commands[]` table in
   `apps/help.c` so `/bin/help` stays complete.

6. **Test it** — add a case to `tools/e2e.py` and a row to the command
   table in [RUNNING.md](RUNNING.md).

Limits to design against: argv is capped at `UPROC_MAX_ARGS` (16)
arguments of `UPROC_ARG_MAX` (128) bytes; a process may hold
`MAX_OPEN_FILES` (16) descriptors; the user stack is 16 pages, so large
buffers belong on the heap (`malloc`, i.e. `SYS_BRK`) rather than on the
stack.
