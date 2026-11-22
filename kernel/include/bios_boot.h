#pragma once

#include "bootinfo.h"

#define BIOS_E820_MAX 128
#define BIOS_E820_USABLE 1

/* Stage 2 wire layout at physical 0x6000. */
struct bios_bootinfo {
    uint16_t e820_count;
    uint8_t boot_drive;
    uint8_t fb_present;
    uint32_t fb_width, fb_height, fb_pitch, fb_bpp;
    uint32_t boot_seed;
    uint64_t fb_phys;
    struct {
        uint64_t base, len;
        uint32_t type, attr;
    } __attribute__((packed)) e820[BIOS_E820_MAX];
} __attribute__((packed));

bool bios_boot_decode(const struct bios_bootinfo *wire, struct boot_resources *out);
