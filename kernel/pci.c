#include "kernel.h"
#include "io.h"
#include "pci.h"

/* Legacy PCI configuration mechanism #1: address port 0xCF8, data 0xCFC. */

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_MAX_DEVS 32

static struct pci_dev devs[PCI_MAX_DEVS];
static int ndevs;
static bool scanned;

static uint32_t cfg_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
           ((uint32_t)fn << 8) | (off & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    outl(PCI_CONFIG_ADDR, cfg_addr(bus, dev, fn, off));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t v = pci_read32(bus, dev, fn, off);
    return (uint16_t)(v >> ((off & 2) * 8));
}

uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t v = pci_read32(bus, dev, fn, off);
    return (uint8_t)(v >> ((off & 3) * 8));
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off,
                 uint32_t val)
{
    outl(PCI_CONFIG_ADDR, cfg_addr(bus, dev, fn, off));
    outl(PCI_CONFIG_DATA, val);
}

static void check_fn(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint32_t id = pci_read32(bus, dev, fn, 0x00);
    uint16_t vendor = (uint16_t)id;

    if (vendor == 0xFFFF)
        return;

    uint16_t device = (uint16_t)(id >> 16);
    uint32_t cls = pci_read32(bus, dev, fn, 0x08);

    kprintf("pci: %02x:%02x.%x vendor %04x device %04x class %02x.%02x\n",
            bus, dev, fn, vendor, device, cls >> 24, (cls >> 16) & 0xFF);

    if (ndevs < PCI_MAX_DEVS) {
        struct pci_dev *d = &devs[ndevs++];
        d->bus = bus;
        d->dev = dev;
        d->fn = fn;
        d->vendor = vendor;
        d->device = device;
        d->bar0 = pci_read32(bus, dev, fn, 0x10);
        d->irq_line = pci_read8(bus, dev, fn, 0x3C);
    }
}

void pci_init(void)
{
    if (scanned)
        return;
    scanned = true;

    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            if ((pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0) & 0xFFFF)
                == 0xFFFF)
                continue;
            check_fn((uint8_t)bus, (uint8_t)dev, 0);
            /* Header type bit 7 => multifunction device. */
            if (pci_read8((uint8_t)bus, (uint8_t)dev, 0, 0x0E) & 0x80)
                for (int fn = 1; fn < 8; fn++)
                    check_fn((uint8_t)bus, (uint8_t)dev, (uint8_t)fn);
        }
    }
    kprintf("pci: %d device function(s) found\n", ndevs);
}

bool pci_find(uint16_t vendor, uint16_t device, struct pci_dev *out)
{
    pci_init();
    for (int i = 0; i < ndevs; i++) {
        if (devs[i].vendor == vendor && devs[i].device == device) {
            *out = devs[i];
            return true;
        }
    }
    return false;
}

void pci_enable_bus_master(const struct pci_dev *d)
{
    uint16_t cmd = pci_read16(d->bus, d->dev, d->fn, 0x04);
    cmd |= 0x0005;   /* I/O space enable + bus master */
    /* Write the command register, preserving the status half read-back. */
    uint32_t v = pci_read32(d->bus, d->dev, d->fn, 0x04);
    v = (v & 0xFFFF0000u) | cmd;
    pci_write32(d->bus, d->dev, d->fn, 0x04, v);
}
