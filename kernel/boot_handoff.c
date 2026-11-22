#include "boot_handoff.h"

_Static_assert(sizeof(struct boot_handoff_v1) == 48, "handoff v1 header size");
_Static_assert(offsetof(struct boot_handoff_v1, memory_count) == 8, "handoff v1 count");
_Static_assert(offsetof(struct boot_handoff_v1, fb_phys) == 24, "handoff v1 framebuffer");
_Static_assert(sizeof(struct boot_handoff_range) == 16, "handoff v1 range size");

bool boot_handoff_decode(const void *wire, size_t size, struct boot_resources *out)
{
    if (size < sizeof(struct boot_handoff_v1))
        return false;

    const struct boot_handoff_v1 *header = wire;
    size_t count = header->memory_count;
    if (header->magic != BOOT_HANDOFF_MAGIC ||
        header->version != BOOT_HANDOFF_VERSION ||
        header->header_size != sizeof(*header) ||
        header->reserved || (header->flags & ~BOOT_HANDOFF_FRAMEBUFFER) ||
        !count || count > BOOT_MAX_MEMORY_RANGES ||
        size != sizeof(*header) + count * sizeof(struct boot_handoff_range))
        return false;

    const struct boot_handoff_range *ranges = (const void *)(header + 1);
    for (size_t i = 0; i < count; i++)
        if (ranges[i].base >= ranges[i].end)
            return false;

    *out = (struct boot_resources){.memory_count = count, .seed = header->seed};
    for (size_t i = 0; i < count; i++)
        out->memory[i] = (struct boot_memory_range){ranges[i].base, ranges[i].end};
    if (header->flags & BOOT_HANDOFF_FRAMEBUFFER) {
        out->framebuffer = (struct boot_framebuffer){
            .phys = header->fb_phys,
            .width = header->fb_width,
            .height = header->fb_height,
            .pitch = header->fb_pitch,
            .bpp = header->fb_bpp,
            .present = true,
        };
    }
    return true;
}
