#include "kernel.h"
#include "pmm.h"
#include "proc.h"
#include "string.h"

/* Bitmap physical allocator. 1 bit per 4 KiB page; supports up to 4 GiB. */

#define MAX_PHYS (4ULL * 1024 * 1024 * 1024)
#define MAX_PAGES (MAX_PHYS / PAGE_SIZE)

static uint8_t bitmap[MAX_PAGES / 8];   /* 128 KiB in .bss */
static uint64_t total_pages, free_count, max_phys, search_from;

extern uint8_t __kernel_end[];          /* linker symbol (virtual) */

static inline void set_used(uint64_t page)
{
    bitmap[page / 8] |= 1 << (page % 8);
}

static inline void set_free(uint64_t page)
{
    bitmap[page / 8] &= ~(1 << (page % 8));
}

static inline int is_used(uint64_t page)
{
    return bitmap[page / 8] & (1 << (page % 8));
}

void pmm_init(struct bootinfo *bi)
{
    memset(bitmap, 0xFF, sizeof(bitmap));

    for (int i = 0; i < bi->e820_count; i++) {
        struct e820_entry *e = &bi->e820[i];
        if (e->type != E820_USABLE)
            continue;
        uint64_t start = (e->base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint64_t end = (e->base + e->len) & ~(PAGE_SIZE - 1);
        if (end > MAX_PHYS)
            end = MAX_PHYS;
        for (uint64_t a = start; a < end; a += PAGE_SIZE) {
            if (!is_used(a / PAGE_SIZE))
                continue;
            set_free(a / PAGE_SIZE);
            free_count++;
            total_pages++;
        }
        if (end > max_phys)
            max_phys = end;
    }

    /* Reserve: everything below 1 MiB (BIOS, bootinfo, VGA) and the
     * kernel image itself. */
    uint64_t kernel_end_phys =
        ((uint64_t)__kernel_end - KERNEL_OFFSET + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint64_t a = 0; a < kernel_end_phys; a += PAGE_SIZE) {
        if (!is_used(a / PAGE_SIZE)) {
            set_used(a / PAGE_SIZE);
            free_count--;
        }
    }
    search_from = kernel_end_phys / PAGE_SIZE;
}

/* The bitmap, free_count and search_from are shared mutable state and the
 * scheduler is preemptive (and reap() frees from IRQ context), so every
 * scan/claim sequence has to be atomic against a task switch. */

uint64_t pmm_alloc(void)
{
    uint64_t f = irq_save();
    for (uint64_t p = search_from; p < max_phys / PAGE_SIZE; p++) {
        if (!is_used(p)) {
            set_used(p);
            free_count--;
            search_from = p + 1;
            uint64_t phys = p * PAGE_SIZE;
            irq_restore(f);
            memset(P2V(phys), 0, PAGE_SIZE);
            return phys;
        }
    }
    irq_restore(f);
    panic("pmm: out of memory");
}

uint64_t pmm_alloc_contig(int npages)
{
    uint64_t limit = max_phys / PAGE_SIZE;
    uint64_t f = irq_save();
    for (uint64_t p = 256; p + npages <= limit; p++) {
        int ok = 1;
        for (int i = 0; i < npages; i++) {
            if (is_used(p + i)) {
                ok = 0;
                p += i;
                break;
            }
        }
        if (!ok)
            continue;
        for (int i = 0; i < npages; i++) {
            set_used(p + i);
            free_count--;
        }
        uint64_t phys = p * PAGE_SIZE;
        irq_restore(f);
        memset(P2V(phys), 0, (uint64_t)npages * PAGE_SIZE);
        return phys;
    }
    irq_restore(f);
    panic("pmm: out of contiguous memory (%d pages)", npages);
}

void pmm_free(uint64_t phys)
{
    uint64_t p = phys / PAGE_SIZE;
    uint64_t f = irq_save();
    if (!is_used(p)) {
        irq_restore(f);
        panic("pmm: double free of %lx", phys);
    }
    set_free(p);
    free_count++;
    if (p < search_from)
        search_from = p;
    irq_restore(f);
}

void pmm_free_contig(uint64_t phys, int npages)
{
    for (int i = 0; i < npages; i++)
        pmm_free(phys + (uint64_t)i * PAGE_SIZE);
}

uint64_t pmm_total_pages(void) { return total_pages; }
uint64_t pmm_free_pages(void) { return free_count; }
uint64_t pmm_max_phys(void) { return max_phys; }
