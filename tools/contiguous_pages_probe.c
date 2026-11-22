#include "../kernel/pmm.c"
#include "boot_handoff.h"
#include "fb.h"

static uint8_t saved_bitmap[sizeof(bitmap)];

static bool check_boot_handoff(void)
{
    struct {
        struct boot_handoff_v1 header;
        struct boot_handoff_range ranges[1];
    } wire = {
        .header = {
            .magic = BOOT_HANDOFF_MAGIC,
            .version = BOOT_HANDOFF_VERSION,
            .header_size = sizeof(struct boot_handoff_v1),
            .memory_count = 1,
            .flags = BOOT_HANDOFF_FRAMEBUFFER,
            .seed = 17,
            .fb_phys = 0xe0000000,
            .fb_width = 800,
            .fb_height = 600,
            .fb_pitch = 3200,
            .fb_bpp = 32,
        },
        .ranges = {{0x100000, 0x300000}},
    };
    static struct boot_resources out;

    if (!boot_handoff_decode(&wire, sizeof(wire), &out) || out.memory_count != 1 ||
        out.memory[0].base != 0x100000 || out.memory[0].end != 0x300000 ||
        !out.framebuffer.present || out.framebuffer.phys != 0xe0000000 ||
        out.framebuffer.width != 800 || out.framebuffer.height != 600 ||
        out.framebuffer.pitch != 3200 || out.framebuffer.bpp != 32 || out.seed != 17)
        return false;

    wire.header.flags = 0;
    if (!boot_handoff_decode(&wire, sizeof(wire), &out) || out.framebuffer.present ||
        boot_handoff_decode(&wire, sizeof(wire.header) - 1, &out) ||
        boot_handoff_decode(&wire, sizeof(wire) - 1, &out))
        return false;
    wire.header.version++;
    if (boot_handoff_decode(&wire, sizeof(wire), &out))
        return false;
    wire.header.version = BOOT_HANDOFF_VERSION;
    wire.header.memory_count = BOOT_MAX_MEMORY_RANGES + 1;
    if (boot_handoff_decode(&wire, sizeof(wire), &out))
        return false;
    wire.header.memory_count = 1;
    wire.ranges[0].end = wire.ranges[0].base;
    return !boot_handoff_decode(&wire, sizeof(wire), &out);
}

void __real_fb_init(const struct boot_framebuffer *fb);

void __wrap_fb_init(const struct boot_framebuffer *fb)
{
    if (check_boot_handoff())
        kprintf("PASS boot handoff v1: resources decoded, truncated and unsupported data rejected\n");
    else
        kprintf("FAIL boot handoff v1: decoder rejected valid data or accepted malformed data\n");

    if (!fb->present) {
        kprintf("FAIL contiguous pages: guest has no framebuffer\n");
        __real_fb_init(fb);
        return;
    }

    uint64_t shadow = (uint64_t)fb->width * fb->height * 4;
    uint64_t npages = (shadow + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t flags = irq_save();
    uint64_t saved_free_count = free_count;
    memcpy(saved_bitmap, bitmap, sizeof(bitmap));
    uint64_t run = 0;
    for (uint64_t p = 0; p < max_phys / PAGE_SIZE; p++) {
        if (is_used(p)) {
            run = 0;
        } else if (++run == npages) {
            set_used(p);
            free_count--;
            run = 0;
        }
    }
    uint64_t fragmented_count = free_count;
    uint64_t phys = pmm_try_alloc_contig((int)npages);
    bool unavailable = fragmented_count >= npages + 256 &&
                       !phys && free_count == fragmented_count;
    __real_fb_init(fb);
    bool text_fallback = !fb_present() && free_count == fragmented_count;
    memcpy(bitmap, saved_bitmap, sizeof(bitmap));
    free_count = saved_free_count;
    irq_restore(flags);

    __real_fb_init(fb);
    if (unavailable && text_fallback && fb_present())
        kprintf("PASS contiguous pages: fragmentation leaves text console available and clears on retry\n");
    else
        kprintf("FAIL contiguous pages: unavailable=%d fallback=%d restored=%d\n",
                unavailable, text_fallback, fb_present());
}
