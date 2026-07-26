#include "kernel.h"
#include "string.h"
#include "pci.h"
#include "device.h"
#include "initcall.h"

/* The PCI bus type.
 *
 * Enumeration walks configuration space with the accessors in kernel/pci.c
 * and turns every function that answers into a struct device whose
 * bus_data is a struct pci_devinfo. Matching is by {vendor, device} pairs
 * from the driver's match_table, so a driver never mentions a bus/slot/
 * function number and two drivers for the same class of card are
 * interchangeable.
 *
 * Storage is a fixed array rather than the heap: enumeration is allowed to
 * run before kheap_init() and, more importantly, a device object must
 * outlive every driver that might bind to it, including drivers in modules
 * that are loaded and unloaded repeatedly.
 */

#define PCI_MAX_FUNCS 32

static struct bus_type pci_bus;

static struct device    pci_devs[PCI_MAX_FUNCS];
static struct pci_devinfo pci_info[PCI_MAX_FUNCS];
static int pci_ndevs;

/* ---- matching -------------------------------------------------------- */

static int pci_bus_match(struct device *dev, struct driver *drv)
{
    const struct pci_device_id *id = drv->match_table;
    const struct pci_devinfo *pd = dev->bus_data;

    if (!id || !pd)
        return 0;

    for (; id->vendor != 0; id++) {
        if (id->vendor != pd->vendor)
            continue;
        if (id->device == PCI_ANY_ID || id->device == pd->device)
            return 1;
    }
    return 0;
}

/* ---- enumeration ----------------------------------------------------- */

/* "pci:BB:SS.F" -- no snprintf in the kernel, and the name has to be
 * stable and unique, so it is built by hand. */
static void pci_dev_name(char *dst, uint8_t bus, uint8_t slot, uint8_t fn)
{
    static const char hex[] = "0123456789abcdef";

    dst[0] = 'p';
    dst[1] = 'c';
    dst[2] = 'i';
    dst[3] = ':';
    dst[4] = hex[(bus >> 4) & 0xF];
    dst[5] = hex[bus & 0xF];
    dst[6] = ':';
    dst[7] = hex[(slot >> 4) & 0xF];
    dst[8] = hex[slot & 0xF];
    dst[9] = '.';
    dst[10] = hex[fn & 0x7];
    dst[11] = '\0';
}

static void add_function(uint8_t bus, uint8_t slot, uint8_t fn)
{
    uint32_t id = pci_read32(bus, slot, fn, 0x00);

    if ((uint16_t)id == 0xFFFF)
        return;

    if (pci_ndevs >= PCI_MAX_FUNCS) {
        kprintf("bus_pci: more than %d functions, ignoring the rest\n",
                PCI_MAX_FUNCS);
        return;
    }

    /* Claim the slot before registering: device_register() may probe, and
     * a probe is allowed to register a child device, which would land on
     * this same index if it were still free. */
    int idx = pci_ndevs++;
    struct pci_devinfo *pd = &pci_info[idx];
    struct device *dev = &pci_devs[idx];
    uint32_t cls = pci_read32(bus, slot, fn, 0x08);

    pd->bus        = bus;
    pd->slot       = slot;
    pd->fn         = fn;
    pd->vendor     = (uint16_t)id;
    pd->device     = (uint16_t)(id >> 16);
    pd->revision   = (uint8_t)cls;
    pd->prog_if    = (uint8_t)(cls >> 8);
    pd->subclass   = (uint8_t)(cls >> 16);
    pd->class_code = (uint8_t)(cls >> 24);
    pd->bar0       = pci_read32(bus, slot, fn, 0x10);
    pd->irq_line   = pci_read8(bus, slot, fn, 0x3C);

    memset(dev, 0, sizeof(*dev));
    pci_dev_name(dev->name, bus, slot, fn);
    dev->bus = &pci_bus;
    dev->bus_data = pd;

    if (device_register(dev) < 0)
        pci_ndevs = idx;        /* give the slot back */
}

static int pci_bus_enumerate(void)
{
    /* kernel/pci.c still owns the scan that prints one line per function
     * and backs pci_find(); it is idempotent, so calling it here keeps the
     * familiar boot log without scanning the bus twice for anyone else. */
    pci_init();

    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            if ((pci_read32((uint8_t)bus, (uint8_t)slot, 0, 0) & 0xFFFF)
                == 0xFFFF)
                continue;
            add_function((uint8_t)bus, (uint8_t)slot, 0);
            /* Header type bit 7 => multifunction device. */
            if (pci_read8((uint8_t)bus, (uint8_t)slot, 0, 0x0E) & 0x80)
                for (int fn = 1; fn < 8; fn++)
                    add_function((uint8_t)bus, (uint8_t)slot, (uint8_t)fn);
        }
    }

    kprintf("bus_pci: %d device(s) on the pci bus\n", pci_ndevs);
    return pci_ndevs;
}

static struct bus_type pci_bus = {
    .name = "pci",
    .match = pci_bus_match,
    .enumerate = pci_bus_enumerate,
};

/* ---- driver helpers -------------------------------------------------- */

/* PCI drivers want kernel/pci.c's helpers, which speak struct pci_dev.
 * Rather than have every driver hand-copy four fields, hand it over here. */
void pci_devinfo_to_pci_dev(const struct pci_devinfo *pd, struct pci_dev *out)
{
    if (!pd || !out)
        return;
    out->bus      = pd->bus;
    out->dev      = pd->slot;
    out->fn       = pd->fn;
    out->vendor   = pd->vendor;
    out->device   = pd->device;
    out->bar0     = pd->bar0;
    out->irq_line = pd->irq_line;
}

/* ---- init ------------------------------------------------------------ */

static int bus_pci_init(void)
{
    if (bus_register(&pci_bus) < 0)
        return -1;
    pci_bus.enumerated = true;
    pci_bus_enumerate();
    return 0;
}
initcall(core, bus_pci_init);
