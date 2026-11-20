#include "../kernel/pmm.c"
#include "fb.h"

static uint8_t saved_bitmap[sizeof(bitmap)];

void __real_fb_init(struct bootinfo *bi);

void __wrap_fb_init(struct bootinfo *bi)
{
    if (!bi->fb_present) {
        kprintf("FAIL contiguous pages: guest has no framebuffer\n");
        __real_fb_init(bi);
        return;
    }

    uint64_t shadow = (uint64_t)bi->fb_width * bi->fb_height * 4;
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
    __real_fb_init(bi);
    bool text_fallback = !fb_present() && free_count == fragmented_count;
    memcpy(bitmap, saved_bitmap, sizeof(bitmap));
    free_count = saved_free_count;
    irq_restore(flags);

    __real_fb_init(bi);
    if (unavailable && text_fallback && fb_present())
        kprintf("PASS contiguous pages: fragmentation leaves text console available and clears on retry\n");
    else
        kprintf("FAIL contiguous pages: unavailable=%d fallback=%d restored=%d\n",
                unavailable, text_fallback, fb_present());
}
