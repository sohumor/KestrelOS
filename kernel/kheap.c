#include "kernel.h"
#include "kheap.h"
#include "pmm.h"
#include "proc.h"
#include "string.h"

/* Kernel heap living in the physical direct map.
 * Small allocations (<= 2048) come from power-of-two free lists carved out
 * of 4 KiB pages; large allocations get whole contiguous pages. */

#define MIN_SHIFT 4              /* 16 bytes */
#define MAX_SHIFT 11             /* 2048 bytes */
#define MAGIC_SMALL 0x4B534D4CU  /* 'KSML' */
#define MAGIC_LARGE 0x4B4C5247U  /* 'KLRG' */

struct hdr {
    uint32_t magic;
    uint32_t info;               /* small: size class shift; large: npages */
};

struct freeobj {
    struct freeobj *next;
};

/* freelist[] is shared mutable state: the scheduler is preemptive and
 * schedule()/reap() calls kfree() from IRQ context, so every read-modify-
 * write of a list head runs with interrupts masked. */
static struct freeobj *freelist[MAX_SHIFT + 1];

void kheap_init(void)
{
    /* nothing to do — grown on demand */
}

/* Caller holds the interrupt mask: this relinks the shared list head 256
 * times, which must not be interleaved with another kmalloc/kfree. */
static void refill(int shift)
{
    uint64_t phys = pmm_alloc();
    uint8_t *page = P2V(phys);
    size_t step = (size_t)1 << shift;
    for (size_t off = 0; off + step <= PAGE_SIZE; off += step) {
        struct freeobj *o = (struct freeobj *)(page + off);
        o->next = freelist[shift];
        freelist[shift] = o;
    }
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;
    size_t need = size + sizeof(struct hdr);

    if (need <= ((size_t)1 << MAX_SHIFT)) {
        int shift = MIN_SHIFT;
        while (((size_t)1 << shift) < need)
            shift++;
        uint64_t f = irq_save();
        if (!freelist[shift])
            refill(shift);
        struct freeobj *o = freelist[shift];
        if (!o) {
            irq_restore(f);
            return NULL;
        }
        freelist[shift] = o->next;
        irq_restore(f);
        struct hdr *h = (struct hdr *)o;
        h->magic = MAGIC_SMALL;
        h->info = shift;
        return h + 1;
    }

    int npages = (need + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_contig(npages);
    struct hdr *h = P2V(phys);
    h->magic = MAGIC_LARGE;
    h->info = npages;
    return h + 1;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;
    struct hdr *h = (struct hdr *)ptr - 1;
    if (h->magic == MAGIC_SMALL) {
        int shift = h->info;
        h->magic = 0;
        struct freeobj *o = (struct freeobj *)h;
        uint64_t f = irq_save();
        o->next = freelist[shift];
        freelist[shift] = o;
        irq_restore(f);
    } else if (h->magic == MAGIC_LARGE) {
        h->magic = 0;
        pmm_free_contig(V2P(h), h->info);
    } else {
        panic("kfree: bad magic %x at %p", h->magic, ptr);
    }
}
