#pragma once

#include <stdint.h>
#include <stdbool.h>

/* KestrelOS device model.
 *
 * Three objects, and one rule that ties them together:
 *
 *   struct bus_type   knows how to enumerate hardware and how to decide
 *                     whether a driver handles a device.
 *   struct device     one piece of hardware a bus found.
 *   struct driver     code that can operate some devices on one bus.
 *
 * The rule: binding is symmetric. Registering a driver walks the devices
 * that are still unbound on its bus and probes the matches; registering a
 * device walks the drivers already registered for its bus. Neither side
 * has to exist first, which is exactly what lets a module loaded minutes
 * after boot claim hardware that was enumerated during boot.
 *
 * Everything here is caller-allocated: buses, devices and drivers are
 * static structures the registrant owns for as long as it is registered.
 * The core never calls kmalloc, so it works before the heap exists and
 * cannot fail for lack of memory.
 *
 * Locking: registration and binding run from initcalls and from the module
 * loader, i.e. always in task context. Nothing in this file may be called
 * from an interrupt handler.
 */

#define DEVICE_NAME_MAX 24
#define DRIVER_NAME_MAX 24
#define BUS_NAME_MAX    16

struct device;
struct driver;

struct bus_type {
    char name[BUS_NAME_MAX];

    /* Return non-zero if drv can drive dev. Consults drv->match_table,
     * whose type is private to the bus. NULL means "match everything",
     * which is only ever right for a bus with a single driver. */
    int (*match)(struct device *dev, struct driver *drv);

    /* Discover the hardware on this bus and device_register() each piece
     * found. Returns the number of devices registered, or negative on a
     * hard error. Called once; bus_enumerate_all() skips buses that have
     * already run. */
    int (*enumerate)(void);

    struct bus_type *next;      /* core-owned: global bus list */
    bool enumerated;            /* core-owned */
};

struct device {
    char name[DEVICE_NAME_MAX]; /* unique within the bus, e.g. "pci:00:03.0" */
    struct bus_type *bus;
    void *bus_data;             /* bus-private description (see below) */
    struct driver *drv;         /* NULL until bound */
    void *drv_data;             /* driver-private state, set by probe() */
    struct device *next;        /* core-owned: global device list */
};

struct driver {
    char name[DRIVER_NAME_MAX];
    struct bus_type *bus;

    /* Called once per matching device. Return 0 to claim the device, or
     * negative to decline it (the device stays unbound and is offered to
     * the next driver). probe() runs in task context and may allocate,
     * install IRQ handlers and touch hardware. It must not call
     * driver_register() or driver_unregister(). */
    int (*probe)(struct device *dev);

    /* Undo probe(): free drv_data, quiesce the hardware, remove IRQ
     * handlers. Called on driver_unregister() and device_unregister().
     * May be NULL for a driver that can never be unloaded. */
    void (*remove)(struct device *dev);

    const void *match_table;    /* bus-specific; interpreted by bus->match */

    struct driver *next;        /* core-owned: global driver list */
};

/* ---- bus_data for the PCI bus ---------------------------------------- */

/* One PCI function, as bus_pci.c found it in configuration space. This is
 * what a PCI driver's probe() gets in dev->bus_data. */
struct pci_devinfo {
    uint8_t  bus;               /* bus number 0..255 */
    uint8_t  slot;              /* device number 0..31 */
    uint8_t  fn;                /* function number 0..7 */
    uint8_t  revision;
    uint16_t vendor;
    uint16_t device;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  irq_line;          /* legacy interrupt line, 0/0xFF = unrouted */
    uint32_t bar0;              /* raw BAR0; bit 0 set means an I/O BAR */
};

/* A PCI driver's match_table is an array of these, terminated by an entry
 * with vendor == 0 (an invalid PCI vendor id, so it can never collide). */
struct pci_device_id {
    uint16_t vendor;
    uint16_t device;            /* PCI_ANY_ID matches every device id */
};

#define PCI_ANY_ID 0xFFFF

/* Fill kernel/pci.c's struct pci_dev from a struct pci_devinfo, so a PCI
 * driver can call pci_enable_bus_master() and friends without hand-copying
 * four fields. Defined in kernel/bus_pci.c; the caller includes pci.h. */
struct pci_dev;
void pci_devinfo_to_pci_dev(const struct pci_devinfo *pd, struct pci_dev *out);

/* ---- bus_data for the platform bus ----------------------------------- */

/* Fixed, non-discoverable hardware: the resources are knowledge, not
 * something read out of the machine. */
struct platform_devinfo {
    uint16_t io_base;           /* first I/O port of the register file */
    uint16_t io_len;            /* number of ports, 0 if not port-mapped */
    int8_t   irq;               /* legacy IRQ line, -1 if none */
};

/* A platform driver's match_table is a NULL-terminated array of device
 * names (const char *const []). A NULL table falls back to matching the
 * driver's own name against the device name. */

/* ---- device_list() ---------------------------------------------------- */

/* Flat snapshot of one device, for `lsdev` and the kernel monitor. Kept
 * layout-compatible with struct k_devinfo in abi/kestrel_abi.h. */
struct device_info {
    char bus[BUS_NAME_MAX];
    char name[DEVICE_NAME_MAX];
    char driver[DRIVER_NAME_MAX];   /* "" when the device is unbound */
    uint32_t bound;                 /* 1 if a driver claimed it */
    uint32_t vendor;                /* PCI vendor id, 0 on other buses */
    uint32_t device;                /* PCI device id, 0 on other buses */
    uint32_t class_id;              /* PCI class << 8 | subclass, else 0 */
};

/* ---- core API --------------------------------------------------------- */

/* Register a bus. Returns 0, or -1 if the bus is already registered or the
 * name is empty or duplicated. Does NOT enumerate: call bus->enumerate()
 * (or bus_enumerate_all()) once the machine is ready to be probed. */
int bus_register(struct bus_type *bus);

struct bus_type *bus_find(const char *name);

/* Run enumerate() on every registered bus that has not been enumerated
 * yet. Returns the total number of devices registered. */
int bus_enumerate_all(void);

/* Register a device and immediately offer it to the matching drivers.
 * Returns 1 if a driver claimed it, 0 if it is registered but unbound,
 * -1 if the arguments are unusable or the device is already registered. */
int device_register(struct device *dev);

/* Unbind (calling drv->remove) and drop the device from the list. */
void device_unregister(struct device *dev);

/* Register a driver and offer it every unbound device on its bus.
 * Returns the number of devices bound (0 is not an error: the hardware
 * may simply be absent), or -1 on a bad argument. */
int driver_register(struct driver *drv);

/* Unbind every device this driver claimed, then drop it from the list.
 * After this returns the driver's code may be unmapped. */
void driver_unregister(struct driver *drv);

struct device *device_find(const char *name);

/* Number of registered devices, bound or not. */
int device_count(void);

/* Snapshot device number `index` (0-based, registration order).
 * Returns 0, or -1 once index is past the end. */
int device_list(int index, struct device_info *out);

/* Print the device tree on the kernel console (kmon / boot diagnostics). */
void device_dump(void);

/* Attach a device to the platform bus and register it. For a module that
 * owns fixed hardware kernel/bus_platform.c's table does not list. Both
 * structures stay owned by the caller. Defined in kernel/bus_platform.c. */
int platform_device_add(struct device *dev, struct platform_devinfo *info);
