# Loadable kernel modules

Section 1 of [MODULARITY.md](MODULARITY.md), built. A module is a
relocatable ELF64 object (`ET_REL`) compiled separately from the kernel and
bound into it at runtime:

```
insmod hello                 # /lib/modules/hello.kmod
lsmod
rmmod hello
```

Three pieces make that work: an **export table** that defines what a module
may reach, a **relocating loader** that binds against it, and a **lifecycle**
that runs init and exit and refuses to unload something still in use.

---

## 1. The export table

`kernel/include/export.h` defines one macro:

```c
EXPORT_SYMBOL(kprintf);
```

which emits a `struct ksym { const char *name; void *addr; }` into the
`.ksyms` section. `kernel/linker.ld` brackets that section with
`__ksyms_start` / `__ksyms_end`, and `ksym_lookup()` scans between them.

Every export lives in **`kernel/ksyms.c`** rather than next to its
definition. That is a deliberate trade: scattering `EXPORT_SYMBOL` through
the subsystems is more local, but then nobody can answer "what is the
kernel's module API?" without grepping the tree. In one file it is a
reviewable list, and adding to it is visibly an act of API design.

What is exported today: `kprintf`/`kvprintf`/`panic`, the heap
(`kmalloc`/`kzalloc`/`kfree`), the page allocator, `vmm_map_page` /
`vmm_kernel_pml4` / `vmm_virt_to_phys`, the string and memory helpers,
`irq_install_handler` and the PIC mask calls, `timer_ticks`/`timer_sleep`,
`kthread_create`/`task_sleep_ticks`/`yield`, `klog_write`/`klog_printf`,
and the block device registration calls.

What is **not** exported: the scheduler internals, the VFS, the syscall
layer, the window manager. Modules decouple drivers at the edges; they are
not a way to replace the core at runtime.

Two things do not need exporting because they have no address to export:
`inb`/`outb`/`inw`/`outw`/`inl`/`outl` in `kernel/include/io.h` are
`static inline`, as are `irq_save`/`irq_restore`, `sti`/`cli` and `invlpg`.
A module that includes the header gets the instructions inlined.

### Weak exports

`blockdev_register` and `blockdev_unregister` are declared `weak` in
`ksyms.c`. If the block layer is not compiled in, their export records
carry a NULL address, `ksym_lookup()` skips them, and a block driver module
fails to load with

```
module: ramdisk: unresolved symbol 'blockdev_register'
```

which is the correct answer. The alternative — resolving to zero and
jumping there later — is the failure mode the whole subsystem exists to
prevent.

---

## 2. Where module code lives, and why it cannot be `kmalloc`

This is the one genuinely hard constraint in the loader.

`-mcmodel=kernel` code reaches its own data with `R_X86_64_32S` (a signed
32-bit absolute) and calls functions with `R_X86_64_PC32` / `PLT32` (a
signed 32-bit displacement). Both require everything involved to sit inside
one 2 GiB window.

The kernel text is at `0xFFFFFFFF80100000`. `kmalloc` returns direct-map
addresses at `0xFFFF800000000000` — about 128 TiB away. A module placed
there would have **every** call into the kernel overflow its PC32
displacement, and every reference to its own strings truncate to a wrong
address. It would load and then execute garbage.

So module memory comes from a dedicated window inside the kernel's own
2 GiB:

```
0xFFFFFFFF80000000  KERNEL_OFFSET .. kernel image, 16 MiB mapped by vmm_init
0xFFFFFFFF81000000  module arena base   (+16 MiB)
0xFFFFFFFF81400000  module arena end    (4 MiB)
```

Any two addresses in the top 2 GiB are within PC32 reach of each other, and
any address in that window truncates to a 32-bit value that sign-extends
back exactly — so both relocation families are correct *by construction*,
not by luck. The 32-bit forms are still range-checked on every entry, and
an out-of-range one is refused rather than truncated.

Backing frames come from the PMM and are mapped into the kernel PML4. Entry
511 of every address space is a copy of the kernel's and points at the same
PDPT page (see `vmm_new_pml4`), so a mapping made here is immediately
visible to every process with no further work.

Frames are mapped on first use and then kept. Unmapping would mean tearing
down page tables shared with every live address space, for no gain at a
4 MiB ceiling; only the arena's page accounting is reclaimed on `rmmod`, so
a load/unload cycle reuses the same frames.

---

## 3. Writing a module

```c
#include "kernel.h"
#include "module.h"

static int hello_init(void)
{
    kprintf("hello: init\n");
    return 0;                    /* non-zero aborts the load */
}

static void hello_exit(void)
{
    kprintf("hello: exit\n");
}

MODULE_NAME("hello");
MODULE_DESC("loader smoke test: logs on load and unload");
MODULE_INIT(hello_init);
MODULE_EXIT(hello_exit);
```

`MODULE_NAME` is required; the other three are optional. A module whose
`init` returns non-zero is unloaded immediately and `insmod` reports the
failure.

### How the declaration is encoded

Each macro emits one tagged `struct module_decl` record into the
`.kmod_decl` section, and the loader finds that section by name and merges
the records it holds:

```c
struct module_decl {
    uint32_t tag;                /* MODDECL_NAME / _DESC / _INIT / _EXIT */
    uint32_t reserved;
    void *value;
};
```

Tagged records rather than one fixed struct, because C cannot have four
independent macros initialise four fields of one object. It also buys real
properties: the macros may appear in any order, any of them may be omitted,
and a future `MODULE_VERSION` stays readable by today's loader, which logs
and ignores tags it does not know.

### Building

Modules are compiled exactly like the kernel and **not linked** — the `.o`
*is* the `.kmod`:

```
gcc -m64 -ffreestanding -nostdlib -fno-stack-protector -fno-pic -fno-pie \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel \
    -fno-common -fno-asynchronous-unwind-tables -Wall -Wextra -O2 \
    -Ikernel/include -Iabi -c modules/hello.c -o build/modules/hello.kmod
```

Two flags beyond the kernel's own:

- `-fno-common`, because a tentative definition would become an
  `SHN_COMMON` symbol, which the loader refuses (it would have to invent an
  allocation policy for it, and gcc 10+ defaults to this anyway).
- `-fno-asynchronous-unwind-tables`, to keep `.eh_frame` out of the image.
  It is `SHF_ALLOC`, so it would otherwise be copied and relocated for
  nothing.

`make` puts the results in `/lib/modules` in the disk image.

---

## 4. The loader

`kernel/module.c`, the `ET_REL` sibling of `elf.c`. In order:

1. **Validate the header.** `ET_REL`, `EM_X86_64`, ELFCLASS64, sane
   `e_shentsize` / `e_shnum` / `e_shstrndx`. Then bounds-check every
   section header against the file size once, so nothing downstream has to.
2. **Plan the layout.** Every `SHF_ALLOC` section gets an offset in one
   contiguous block, honouring `sh_addralign` (the block itself is page
   aligned, so alignments above a page are clamped). Anything above 1 MiB
   of relocated image is refused.
3. **Place it.** One arena allocation, `PROGBITS` copied in, `NOBITS` left
   as the zeroes `arena_alloc` already wrote.
4. **Resolve symbols.** `SHN_ABS` keeps its value; a defined symbol becomes
   `section base + st_value`; `SHN_UNDEF` is looked up first among the
   module's own definitions (an object built with `ld -r` can have both)
   and then in the export table. `SHN_COMMON` is refused. An unresolved
   symbol aborts the load naming the symbol — this is the single most
   important diagnostic in the subsystem, and it goes to both the console
   and the kernel log.
5. **Relocate.** `R_X86_64_64`, `PC32`, `PLT32` (identical to PC32 here:
   there is no PLT, the module is bound directly), `32` and `32S`. Every
   32-bit form is range-checked and refused rather than truncated, and each
   entry's write is bounds-checked against its target section.
6. **Read `.kmod_decl`** and merge the tagged records. String and function
   pointers are checked to land inside the module's own block before they
   are followed.
7. **Register, then run `init`.** The module is on the list before `init`
   runs, so `init` can hand out pointers into itself and have them
   reference-counted. A non-zero return unloads it immediately (without
   running `exit`, which never ran its counterpart).

Failures return a `MODERR_*` code from `module.h` and log a reason. There is
no path that loads a partially bound module.

---

## 5. Lifecycle and reference counting

- Loading a name that is already loaded fails with `MODERR_EXISTS`.
- `module_get(name)` / `module_put(name)` bracket a reference. `rmmod`
  refuses while any are outstanding.
- `rmmod` runs `exit`, unlinks the module, frees its arena block and its
  record.
- `insmod` and `rmmod` are serialised by a single flag, never held across
  a module's own `init` or `exit`.

---

## 6. Tools

| command | syscall | what it does |
|---|---|---|
| `insmod <name\|path>` | `SYS_INSMOD` | a bare name means `/lib/modules/<name>.kmod` |
| `rmmod <name>` | `SYS_RMMOD` | runs `exit`, then frees |
| `lsmod` | `SYS_MODLIST` | name, size, refs, state, description |

All three are root-only: loading code into the kernel is the most
privileged thing a user can ask for.

---

## 7. The modules in the tree

- **`modules/hello.c`** — the smoke test. Deliberately useless, but it
  exercises the whole binding path: a `PLT32` call into `kprintf`, `32S`
  references to its own `.rodata`, a `.bss` variable the loader has to
  place and zero, and `R_X86_64_64` relocations inside `.kmod_decl`.
- **`modules/ramdisk.c`** — a 256 KiB RAM-backed block device registering
  `ram0` with the block layer, allocating through the exported heap and
  unregistering on `rmmod`. It is the case section 4 of MODULARITY.md
  exists for: a root filesystem that was never compiled in.

---

## 8. Limits

| | |
|---|---|
| module arena | 4 MiB, page granular |
| relocated image | 1 MiB per module |
| `.kmod` file | 1 MiB |
| sections | 96 |
| symbols | 4096 |

These are all refusals, not truncations.
