# Modularity plan

Up to this point KestrelOS has been built as a monolith with hard-wired
subsystems: `kmain()` calls each driver's init function by name, KFS talks to
the ATA driver directly, the VFS knows about exactly two filesystems, and
every driver is compiled into the kernel binary whether the machine needs it
or not. That was the right way to get a system working. It is the wrong way to
keep growing one.

This document defines what "modular" means here, concretely, and what changes
to make it true.

## 1. Loadable kernel modules

The headline change. A module is a relocatable ELF64 object (`ET_REL`) built
separately from the kernel and loaded at runtime:

```
insmod /lib/modules/rtl8139.kmod
lsmod
rmmod rtl8139
```

What this requires:

- **An exported symbol table.** The kernel records the names and addresses of
  the functions and variables modules may use, via an `EXPORT_SYMBOL(name)`
  macro that emits an entry into a dedicated linker section. Nothing else is
  visible to a module; the export list *is* the kernel's internal API, and
  making it explicit is most of the value of this exercise.
- **A relocating loader.** `ET_REL` objects arrive with unresolved symbols and
  relocation entries. The loader allocates memory for each section, resolves
  every `R_X86_64_64`, `R_X86_64_PC32`, `R_X86_64_PLT32` and `R_X86_64_32S`
  against the export table, and refuses to load a module with an unresolved
  symbol rather than jumping into nothing.
- **Module lifecycle.** Each module declares `module_init` and `module_exit`
  entry points and a metadata block (name, version, description, dependencies).
  Reference counting prevents unloading a module still in use.

## 2. A driver model instead of hard-wired init

Today `kmain()` reads as a list of every driver that exists. Instead:

- A **bus** enumerates devices and offers them to drivers. `pci` walks config
  space; `platform` covers the fixed hardware (PIT, PS/2, serial, CMOS); `input`
  and `block` are logical buses for higher layers.
- A **driver** registers a table of the devices it handles and a `probe()`.
  Binding is by match, not by name: the PCI bus offers 8086:100E to whichever
  driver claims it, which is what makes the NIC drivers interchangeable — and
  loadable.
- **Init ordering** comes from declared levels (`INITCALL_EARLY`, `_CORE`,
  `_DRIVER`, `_LATE`) collected into a linker section, so adding a subsystem no
  longer means editing `kmain()`.

## 3. A real VFS with mountable filesystem types

`vfs.c` currently hardcodes KFS with an `if (devfs_claims(path))` special case
in front of it. Replace with:

- `struct fs_type { name, mount(), ops }` and a registry, so a filesystem
  registers itself (and can therefore live in a module).
- A **mount table** mapping paths to mounted instances, resolved longest-prefix
  first, so `/dev`, `/`, and anything mounted later all work by the same rule
  instead of by a special case.
- `struct file` gains an operations vector, so pipes, devices and regular files
  stop being distinguished by a type tag and a switch.

## 4. A block device layer

KFS calls `ata_read`/`ata_write` directly, which means one filesystem on one
disk forever. Introduce `struct blockdev { name, block_size, count, read(),
write() }`. ATA registers one per drive; a ramdisk registers another. KFS then
mounts *a block device*, not "the disk", which is what makes a second partition,
a second disk, or an in-memory root filesystem possible at all.

## 5. Build configuration

A `config` file (a simple `KEY=y|m|n` list, like a stripped-down Kconfig)
generates `kernel/include/config.h` and the object lists. `y` builds a subsystem
in, `m` builds it as a loadable module, `n` leaves it out entirely. The point is
that "modular" must be checkable: if a driver can be set to `n` and the kernel
still builds and boots, its coupling is genuinely gone.

## Order of work

1. Export table + module loader + `insmod`/`rmmod`/`lsmod` — proves the concept.
2. Block device layer, with ATA and a ramdisk behind it.
3. Filesystem registry and mount table.
4. Bus/driver model and initcall levels, converting `kmain()` to a registry.
5. Build configuration, then convert both NIC drivers to loadable modules as
   the acceptance test for the whole exercise.

## What stays monolithic, and why

The scheduler, memory manager and syscall layer stay compiled in. A module
system exists to decouple *policy from mechanism at the edges* — drivers,
filesystems, protocols — not to make the core swappable at runtime. Making the
page allocator loadable would be architecture theatre: it would add
indirection and risk to the one part of the system that must always be present.
