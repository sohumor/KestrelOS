#pragma once

#include <stdint.h>
#include <stdbool.h>

/* PCI configuration space access via the legacy 0xCF8/0xCFC mechanism. */

struct pci_dev {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  fn;
    uint16_t vendor;
    uint16_t device;
    uint32_t bar0;       /* raw BAR0 (caller masks the type bits) */
    uint8_t  irq_line;   /* legacy interrupt line from config offset 0x3C */
};

/* Scan every bus/device/function once, log each device found. Idempotent. */
void pci_init(void);

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
uint8_t  pci_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off,
                     uint32_t val);

/* Find the first device matching vendor/device. Returns false if absent. */
bool pci_find(uint16_t vendor, uint16_t device, struct pci_dev *out);

/* Set the bus-master (and I/O space) bits in the command register. */
void pci_enable_bus_master(const struct pci_dev *d);
