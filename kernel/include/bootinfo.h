#pragma once

#include <stdint.h>

/* Matches the layout stage 2 builds at physical 0x6000. */

#define E820_USABLE   1
#define E820_RESERVED 2
#define E820_ACPI     3
#define E820_NVS      4
#define E820_BAD      5

struct e820_entry {
    uint64_t base;
    uint64_t len;
    uint32_t type;
    uint32_t attr;
} __attribute__((packed));

struct bootinfo {
    uint16_t e820_count;
    uint8_t  boot_drive;
    uint8_t  _pad;
    uint32_t _pad2;
    struct e820_entry e820[];
} __attribute__((packed));
