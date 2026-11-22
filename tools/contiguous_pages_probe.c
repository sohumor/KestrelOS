#include "../kernel/pmm.c"
#include "bios_boot.h"
#include "fb.h"

static uint8_t saved_bitmap[sizeof(bitmap)];

static bool check_bios_decode(void)
{
    static struct bios_bootinfo wire = {
        .e820_count = 3,
        .boot_seed = 17,
        .e820 = {
            {0x100000, 0x200000, BIOS_E820_USABLE, 1},
            {0x300000, 0x100000, 2, 1},
            {0x400000, 0, BIOS_E820_USABLE, 1},
        },
    };
    static struct boot_resources out;

    if (!bios_boot_decode(&wire, &out) || out.memory_count != 1 ||
        out.memory[0].base != 0x100000 || out.memory[0].end != 0x300000 ||
        out.framebuffer.present || out.seed != 17)
        return false;

    wire.e820_count = BIOS_E820_MAX + 1;
    if (bios_boot_decode(&wire, &out))
        return false;
    wire.e820_count = 1;
    wire.e820[0].len = UINT64_MAX;
    return !bios_boot_decode(&wire, &out);
}

void __real_fb_init(const struct boot_framebuffer *fb);

void __wrap_fb_init(const struct boot_framebuffer *fb)
{
    if (check_bios_decode())
        kprintf("PASS BIOS handoff: usable ranges, optional framebuffer, malformed map rejection\n");
    else
        kprintf("FAIL BIOS handoff: decoder rejected valid data or accepted malformed data\n");

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
