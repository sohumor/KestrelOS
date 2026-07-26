# Writing a driver

This document covers section 2 of [MODULARITY.md](MODULARITY.md): the
bus/driver model and the initcall levels that replaced the hard-wired
sequence in `kmain()`.

Before this change, adding a driver meant editing `kernel/main.c` to call
its `init()` by name, in the right place, and editing whatever subsystem
wanted to use it. The driver's knowledge of *which* hardware it drove was
buried inside that init function as a `pci_find(0x10EC, 0x8139)` call, so
nothing else could ask "what claims this card?" and no code that arrived
after boot could claim anything at all.

Now a driver publishes a table of the hardware it handles and a `probe()`,
and the bus does the introduction.

## The three objects

```
struct bus_type    how to find hardware, and how to decide who drives it
struct device      one piece of hardware a bus found
struct driver      code that can operate some devices on one bus
```

They live in `kernel/include/device.h`, the core is `kernel/device.c`, and
the two buses are `kernel/bus_pci.c` and `kernel/bus_platform.c`.

The rule that makes the whole thing worth building is that **binding is
symmetric**:

- `device_register()` offers the new device to every driver already
  registered for its bus.
- `driver_register()` offers the new driver every device on its bus that
  is still unbound.

Neither side has to exist first. That is not symmetry for its own sake: it
is precisely what lets a driver loaded from a module ten minutes after
boot pick up a card that the PCI bus enumerated during boot. Without the
second direction, loadable drivers would only ever work on hardware
hot-plugged after they arrived, which on this machine is no hardware at
all.

## Worked example: the RTL8139 NIC

### 1. Say what hardware you drive

```c
static const struct pci_device_id rtl8139_ids[] = {
    { 0x10EC, 0x8139 },
    { 0, 0 }                    /* terminator: vendor 0 is not a real id */
};
```

That table *is* the driver's hardware knowledge, and it is data rather
than code, so `lsdev` can print it and the bus can match against it
without calling into the driver. `PCI_ANY_ID` in the device field matches
every device id from that vendor.

The e1000 driver's table is the same shape with four entries, which is the
whole reason the two NIC drivers are interchangeable: neither of them
contains a decision about the other.

### 2. Write probe()

`probe()` is the old `init()` with its first ten lines deleted — the ones
that went looking for the card. The bus already found it and hands it over
in `dev->bus_data`:

```c
static int rtl8139_probe(struct device *dev)
{
    const struct pci_devinfo *pd = dev->bus_data;
    struct pci_dev d;

    if (!pd || present)          /* one NIC of this kind is enough */
        return -1;
    pci_devinfo_to_pci_dev(pd, &d);

    if (!(d.bar0 & 1)) {
        kprintf("rtl8139: BAR0 is not an I/O BAR, skipping\n");
        return -1;
    }
    io_base = (uint16_t)(d.bar0 & 0xFFFC);
    pci_enable_bus_master(&d);

    /* ... reset, read the MAC, set up the DMA rings, install the IRQ,
     *     exactly as before ... */

    present = true;
    netdev_register(&rtl_netdev);
    dev->drv_data = &rtl_netdev;
    return 0;
}
```

Rules for `probe()`:

- **Return 0 to claim the device, negative to decline it.** Declining is
  not an error and is not logged as one; the device stays unbound and the
  next driver on the bus gets its turn. "No pointing device attached" and
  "BAR0 is the wrong kind of BAR" are both declines.
- It runs in **task context**, at `INITCALL_DRIVER` or later, after
  paging, the heap, the scheduler and the framebuffer are up. It may
  allocate, install IRQ handlers, spin on hardware and print.
- Store per-device state in `dev->drv_data`. A driver that can only ever
  handle one instance (this one, because `io_base` is a file-scope
  `static`) may keep using file-scope state, but it must then decline the
  second device rather than trample the first — that is what the
  `|| present` above is for.
- Do not call `driver_register()` from a `probe()`. Registering a *child
  device* is allowed (a bridge enumerating what is behind it); the core
  bounds that recursion at 8 levels and complains rather than overflowing
  the stack.

### 3. Write remove()

`remove()` is what makes `rmmod` possible, so write it even if nothing
unloads the driver yet. It must leave the hardware unable to interrupt or
DMA into memory that is about to be handed to someone else:

```c
static void rtl8139_remove(struct device *dev)
{
    outw(io_base + REG_IMR, 0);      /* no more interrupts */
    outb(io_base + REG_CR, 0);       /* stop rx and tx */
    pic_set_mask(rtl_irq_line);
    irq_install_handler(rtl_irq_line, NULL);
    netdev_unregister(&rtl_netdev);
    pmm_free_contig(rx_phys, rx_pages);
    pmm_free_contig(tx_phys, TX_NDESC);
    present = false;
}
```

Order matters: mask the interrupt at the PIC *before* clearing the
handler, and clear the handler before freeing the buffers the handler
would have touched.

### 4. Register the driver at the right level

```c
static struct driver rtl8139_driver = {
    .name = "rtl8139",
    .probe = rtl8139_probe,
    .remove = rtl8139_remove,
    .match_table = rtl8139_ids,
};

static int rtl8139_drv_init(void)
{
    rtl8139_driver.bus = bus_find("pci");
    if (!rtl8139_driver.bus)
        return -1;
    return driver_register(&rtl8139_driver) < 0 ? -1 : 0;
}
initcall(driver, rtl8139_drv_init);
```

`kernel/main.c` is not touched. Nothing in the kernel mentions
`rtl8139_probe`, `rtl8139_ids` or `rtl8139_drv_init`. Deleting the file
from `kernel/` deletes the driver.

## Platform drivers

PCI hardware answers when you ask it what it is. A PIT, an 8042, a UART
and a CMOS chip do not: they exist because the architecture says so.
`kernel/bus_platform.c` holds that knowledge as a table and registers a
device for each entry, so the same interface covers both kinds of
hardware and `lsdev` shows both.

Matching is by name, because there is no vendor id to consult:

```c
static const char *const ps2kbd_match[] = { "ps2kbd", NULL };

static struct driver ps2kbd_driver = {
    .name = "ps2kbd",
    .probe = ps2kbd_probe,
    .remove = ps2kbd_remove,
    .match_table = ps2kbd_match,
};
```

A NULL `match_table` means "match a device whose name equals the driver's
own name", which is the common case spelled shorter.

The device still carries its resources in `dev->bus_data`, as a
`struct platform_devinfo` (`io_base`, `io_len`, `irq`), and a probe should
read them rather than repeat the constants:

```c
static int ps2kbd_probe(struct device *dev)
{
    const struct platform_devinfo *pi = dev->bus_data;

    if (pi && (pi->irq != 1 || pi->io_base != 0x60))
        return -1;
    keyboard_init();
    return 0;
}
```

This is not ceremony. It is the difference between a driver that declines
hardware it does not recognise and one that writes to port 0x60 on a
machine where the keyboard is somewhere else.

A module with fixed hardware of its own calls
`platform_device_add(&dev, &info)` to put it on the bus.

## Initcall levels

`kernel/include/initcall.h` defines four levels. `kmain()` runs them in
order and knows nothing about what is in them:

| Level | What belongs there |
| --- | --- |
| `early` | the device core and buses that need no allocator (`bus_platform`) |
| `core` | bus enumeration: the PCI config-space walk |
| `driver` | `driver_register()` calls — binding happens here |
| `late` | anything that wants a fully populated device tree |

Ordering **within** a level is link order and therefore incidental. Two
initcalls that must run in a fixed order belong in different levels, or in
one initcall that calls both. The kernel currently leans on exactly one
in-level ordering — `ps2kbd` before `ps2mouse`, which falls out of
`keyboard.c` sorting before `mouse.c` — and the PS/2 mouse probe is
written to cope either way, because leaning on it would be a bug waiting
for someone to rename a file.

An initcall returns 0 for success. A non-zero return is logged and
otherwise ignored. **Nothing in an initcall may panic.** A driver that
finds no hardware is not a reason to stop booting; a kernel that refuses
to boot because a NIC is absent is strictly worse than one with no
network.

### How the levels are collected

By default `initcall(level, fn)` drops a function pointer into a
`.initcall.<level>.init` section and `kernel/initcall.c` walks between the
`__initcall_<level>_start` / `_end` symbols that `kernel/linker.ld`
provides. This is the mechanism the kernel uses, and it is the only one
where a file can declare an initcall without anything else in the tree
knowing the file exists.

Building with `-DINITCALL_STATIC_TABLE` selects a fallback: `initcall()`
emits a plain global pointer and `kernel/initcall.c` carries an explicit
table naming every one. It needs no linker-script support, at the cost of
a second place to edit. The table's references are weak, so a row naming a
driver that is not compiled into this kernel resolves to NULL and is
skipped rather than failing the link — otherwise the table would be a
second thing that has to agree with the build configuration, and
disagreeing would break the build rather than lose a driver. The fallback
exists so that the linker-script change can never be the thing that makes
the kernel unbootable.

The fallback has one advantage worth noting: because the table is written
down, `ps2kbd` before `ps2mouse` is stated rather than inherited from the
alphabet.

## Looking at the result

```
$ lsdev
BUS       DEVICE         DRIVER     ID
platform  pit            -
platform  ps2kbd         ps2kbd
platform  ps2mouse       ps2mouse
platform  serial0        -
platform  cmos           -
pci       pci:00:00.0    -          8086:1237 bridge
pci       pci:00:01.0    -          8086:7000 bridge
pci       pci:00:02.0    -          1234:1111 display
pci       pci:00:03.0    rtl8139    10ec:8139 network
9 devices, 2 bound, 7 unbound
```

(The exact PCI complement depends on the machine; that is a QEMU i440FX
guest with `-device rtl8139`.)

`lsdev pci` narrows it to one bus; `lsdev -v` prints the id column for
every device.

A `-` in the DRIVER column means the bus found the hardware and nothing
claimed it. That is the honest state for three groups of devices:

1. Hardware whose driver has not been converted yet — `pit`, `serial0`
   and `cmos` are still initialised the old way, by name, from `kmain()`.
   They are enumerated so that they are visible and so that converting
   them later is a change to one file instead of two. They are the
   obvious next candidates.
2. Hardware with no driver in this kernel at all (the host bridge, the
   IDE controller, the VGA device).
3. Hardware whose driver lives in a module that is not loaded. Load it
   and the DRIVER column fills in without a reboot — that is the
   acceptance test the modularity plan is aiming at.

`lsdev` reads the tree through `SYS_DEVLIST`, one entry per call, for the
same reason `ps` reads processes that way: the list changes underneath
you, and a snapshot of the whole thing would need a size bound that
nothing can honour.

## Checklist for a new driver

1. A `match_table` of the hardware you handle, terminated correctly.
2. A `probe()` that reads `dev->bus_data`, returns 0 to claim and negative
   to decline, and stores state in `dev->drv_data`.
3. A `remove()` that masks the IRQ, clears the handler, stops DMA and
   frees what `probe()` allocated — in that order.
4. A `struct driver` and an initcall at `driver` level that resolves the
   bus with `bus_find()` and calls `driver_register()`.
5. No edit to `kernel/main.c`. If you needed one, the model did not cover
   your case and that is worth saying out loud rather than working around.
