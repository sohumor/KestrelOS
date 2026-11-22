#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOT_MAX_MEMORY_RANGES 128

/* Firmware has released these bytes for general use. End is exclusive. */
struct boot_memory_range {
    uint64_t base, end;
};

struct boot_framebuffer {
    uint64_t phys;
    uint32_t width, height, pitch, bpp;
    bool present;
};

struct boot_resources {
    struct boot_memory_range memory[BOOT_MAX_MEMORY_RANGES];
    size_t memory_count;
    struct boot_framebuffer framebuffer;
    uint32_t seed;
};
