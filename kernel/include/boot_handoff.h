#pragma once

#include "bootinfo.h"

#define BOOT_HANDOFF_MAGIC 0x544F4F42u
#define BOOT_HANDOFF_VERSION 1u
#define BOOT_HANDOFF_FRAMEBUFFER 1u
#define BOOT_HANDOFF_MAP_LIMIT (1ULL << 30)

/* Little-endian v1 wire header, followed by memory_count usable ranges. */
struct boot_handoff_v1 {
    uint32_t magic;
    uint16_t version, header_size;
    uint32_t memory_count, flags, seed, reserved;
    uint64_t fb_phys;
    uint32_t fb_width, fb_height, fb_pitch, fb_bpp;
} __attribute__((packed));

struct boot_handoff_range {
    uint64_t base, end;
} __attribute__((packed));

bool boot_handoff_decode(const void *wire, size_t size, struct boot_resources *out);
