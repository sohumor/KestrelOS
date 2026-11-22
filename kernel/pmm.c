#include "kernel.h"
#include "pmm.h"
#include "proc.h"
#include "string.h"
#include "spinlock.h"

/* Bitmap physical allocator. 1 bit per 4 KiB page; supports up to 4 GiB. */

#define MAX_PHYS (4ULL * 1024 * 1024 * 1024)
#define MAX_PAGES (MAX_PHYS / PAGE_SIZE)

static uint8_t bitmap[MAX_PAGES / 8];   /* 128 KiB in .bss */
static uint64_t total_pages, free_count, max_phys, search_from;
static spinlock_t pmm_lock = SPINLOCK_INIT;

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

void pmm_init(const struct boot_memory_range *ranges, size_t count)
{
    memset(bitmap, 0xFF, sizeof(bitmap));

    for (size_t i = 0; i < count; i++) {
        if (ranges[i].base >= MAX_PHYS)
            continue;
        uint64_t start = (ranges[i].base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint64_t end = ranges[i].end & ~(PAGE_SIZE - 1);
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
    uint64_t f = spin_lock_irqsave(&pmm_lock);
    for (uint64_t p = search_from; p < max_phys / PAGE_SIZE; p++) {
        if (!is_used(p)) {
            set_used(p);
            free_count--;
            search_from = p + 1;
            uint64_t phys = p * PAGE_SIZE;
            spin_unlock_irqrestore(&pmm_lock, f);
            memset(P2V(phys), 0, PAGE_SIZE);
            return phys;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, f);
    panic("pmm: out of memory");
}

uint64_t pmm_try_alloc_contig(int npages)
{
    if (npages <= 0)
        return 0;
    uint64_t limit = max_phys / PAGE_SIZE;
    uint64_t f = spin_lock_irqsave(&pmm_lock);
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
        spin_unlock_irqrestore(&pmm_lock, f);
        memset(P2V(phys), 0, (uint64_t)npages * PAGE_SIZE);
        return phys;
    }
    spin_unlock_irqrestore(&pmm_lock, f);
    return 0;
}

uint64_t pmm_alloc_contig(int npages)
{
    uint64_t phys = pmm_try_alloc_contig(npages);
    if (!phys)
        panic("pmm: out of contiguous memory (%d pages)", npages);
    return phys;
}

void pmm_free(uint64_t phys)
{
    /* An unaligned or out-of-bitmap address is a caller bug; without this
     * check the bit update below would silently corrupt adjacent .bss. */
    if ((phys & (PAGE_SIZE - 1)) || phys >= MAX_PHYS)
        panic("pmm: invalid free of %lx", phys);
    uint64_t p = phys / PAGE_SIZE;
    uint64_t f = spin_lock_irqsave(&pmm_lock);
    if (!is_used(p)) {
        spin_unlock_irqrestore(&pmm_lock, f);
        panic("pmm: double free of %lx", phys);
    }
    set_free(p);
    free_count++;
    if (p < search_from)
        search_from = p;
    spin_unlock_irqrestore(&pmm_lock, f);
}

void pmm_free_contig(uint64_t phys, int npages)
{
    /* Bound the entire run up front: a corrupted page count must panic
     * before the run's first valid frames are handed back to the free
     * pool, not halfway through when the bitmap check trips. */
    if (npages <= 0 || phys >= MAX_PHYS ||
        (uint64_t)npages > (MAX_PHYS - phys) / PAGE_SIZE)
        panic("pmm: bad free_contig of %d pages at %lx", npages, phys);
    for (int i = 0; i < npages; i++)
        pmm_free(phys + (uint64_t)i * PAGE_SIZE);
}

uint64_t pmm_total_pages(void)
{
    return __atomic_load_n(&total_pages, __ATOMIC_RELAXED);
}

uint64_t pmm_free_pages(void)
{
    return __atomic_load_n(&free_count, __ATOMIC_RELAXED);
}
uint64_t pmm_max_phys(void) { return max_phys; }
