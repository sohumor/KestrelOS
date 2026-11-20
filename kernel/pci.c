#include "io.h"
#include "pci.h"

/* Legacy PCI configuration mechanism #1: address port 0xCF8, data 0xCFC. */

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

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

void pci_enable_bus_master(const struct pci_dev *d)
{
    uint16_t cmd = pci_read16(d->bus, d->dev, d->fn, 0x04);
    cmd |= 0x0005;   /* I/O space enable + bus master */
    /* Write the command register, preserving the status half read-back. */
    uint32_t v = pci_read32(d->bus, d->dev, d->fn, 0x04);
    v = (v & 0xFFFF0000u) | cmd;
    pci_write32(d->bus, d->dev, d->fn, 0x04, v);
}
