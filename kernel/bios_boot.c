#include "bios_boot.h"

_Static_assert(offsetof(struct bios_bootinfo, e820) == 32, "stage 2 entry offset");
_Static_assert(sizeof(((struct bios_bootinfo *)0)->e820[0]) == 24, "E820 entry size");
_Static_assert(BIOS_E820_MAX <= BOOT_MAX_MEMORY_RANGES, "BIOS map capacity");

bool bios_boot_decode(const struct bios_bootinfo *wire, struct boot_resources *out)
{
    if (wire->e820_count > BIOS_E820_MAX)
        return false;

    *out = (struct boot_resources){0};
    for (size_t i = 0; i < wire->e820_count; i++) {
        uint64_t base = wire->e820[i].base, len = wire->e820[i].len;
        if (wire->e820[i].type != BIOS_E820_USABLE || !len)
            continue;
        if (len > UINT64_MAX - base)
            return false;
        out->memory[out->memory_count++] = (struct boot_memory_range){base, base + len};
    }
    if (wire->fb_present) {
        out->framebuffer = (struct boot_framebuffer){
            .phys = wire->fb_phys,
            .width = wire->fb_width,
            .height = wire->fb_height,
            .pitch = wire->fb_pitch,
            .bpp = wire->fb_bpp,
            .present = true,
        };
    }
    out->seed = wire->boot_seed;
    return true;
}
