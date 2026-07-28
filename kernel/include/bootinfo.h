#pragma once

#include <stdint.h>

/* Matches the layout stage 2 builds at physical 0x6000.
 *
 *   offset  0  u16 e820_count
 *   offset  2  u8  boot_drive
 *   offset  3  u8  fb_present
 *   offset  4  u32 fb_width
 *   offset  8  u32 fb_height
 *   offset 12  u32 fb_pitch      (bytes per scanline)
 *   offset 16  u32 fb_bpp
 *   offset 20  u32 boot_seed     (mixed timing seed; not trusted alone)
 *   offset 24  u64 fb_phys       (linear framebuffer physical address)
 *   offset 32  e820 entries, 24 bytes each
 */

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
    uint8_t  fb_present;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;
    uint32_t fb_bpp;
    uint32_t boot_seed;
    uint64_t fb_phys;
    struct e820_entry e820[];
} __attribute__((packed));
