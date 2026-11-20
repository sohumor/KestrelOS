#include "virtio.h"
#include "kernel.h"
#include "pmm.h"
#include "vmm.h"
#include "io.h"

#define VIRTIO_VA 0xFFFFFFFFC0100000ULL
static unsigned windows;
#define C16(v,o) (*(volatile uint16_t *)((v)->common + (o)))
#define C32(v,o) (*(volatile uint32_t *)((v)->common + (o)))
#define C64(v,o) (*(volatile uint64_t *)((v)->common + (o)))

static uint32_t cfg(struct virtio *v, uint8_t o)
{
    return pci_read32(v->pci.bus, v->pci.dev, v->pci.fn, o);
}

static uint8_t cfg8(struct virtio *v, uint8_t o)
{
    return pci_read8(v->pci.bus, v->pci.dev, v->pci.fn, o);
}

static volatile uint8_t *map_cap(struct virtio *v, uint8_t cap, int slot)
{
    uint8_t bar = cfg8(v, cap + 4);
    if (bar > 5)
        return 0;
    /* Reject an upper half of a preceding 64-bit BAR. */
    for (int i = 0; i < bar; i++) {
        uint32_t b = cfg(v, 0x10 + 4 * i);
        if (!(b & 1) && (b & 6) == 4) {
            if (++i == bar)
                return 0;
        }
    }
    uint32_t lo = cfg(v, 0x10 + 4 * bar);
    if (!lo || (lo & 1) || ((lo & 6) != 0 && (lo & 6) != 4))
        return 0;
    uint64_t base = lo & ~15ULL;
    if ((lo & 6) == 4) {
        if (bar == 5)
            return 0;
        base |= (uint64_t)cfg(v, 0x14 + 4 * bar) << 32;
    }
    uint32_t off = cfg(v, cap + 8), len = cfg(v, cap + 12);
    if (!len || len > 0x10000 || base + off < base)
        return 0;
    uint64_t phys = base + off, va = v->window + slot * 0x20000ULL;
    for (uint64_t p = 0; p < (phys & 4095) + len; p += PAGE_SIZE)
        vmm_map_page(vmm_kernel_pml4(), va + p, (phys & ~4095ULL) + p,
                     PTE_W | PTE_PCD);
    return (void *)(va + (phys & 4095));
}

static void status(struct virtio *v, uint8_t s)
{
    if (v->modern)
        v->common[20] = s;
    else
        outb(v->io + 18, s);
}

void virtio_fail(struct virtio *v)
{
    status(v, (v->modern ? v->common[20] : inb(v->io + 18)) | 128);
}
void virtio_ready(struct virtio *v) { status(v, v->modern ? 15 : 7); }

int virtio_open(struct virtio *v, uint16_t type, uint32_t features)
{
    if (!pci_find(0x1af4, 0x1040 + type, &v->pci) &&
        !pci_find(0x1af4, type == 2 ? 0x1001 : 0x1000, &v->pci))
        return -1;
    if (windows >= 8) return -1;
    v->window = VIRTIO_VA + windows++ * 0x80000ULL;
    uint16_t cmd = pci_read16(v->pci.bus, v->pci.dev, v->pci.fn, 4);
    pci_write32(v->pci.bus, v->pci.dev, v->pci.fn, 4, cmd | 7);
    if (cfg(v, 4) & (1U << 20)) {
        uint8_t cap = cfg8(v, 0x34) & ~3;
        for (int n = 0; cap >= 0x40 && cap <= 0xec && n < 48; n++) {
            if (cfg8(v, cap) == 9 && cfg8(v, cap + 2) >= 16) {
                uint8_t t = cfg8(v, cap + 3);
                uint32_t len = cfg(v, cap + 12);
                if (t == 1 && len >= 56) v->common = map_cap(v, cap, 0);
                if (t == 2 && cfg8(v, cap + 2) >= 20) {
                    v->notify = map_cap(v, cap, 1);
                    v->notify_mult = cfg(v, cap + 16);
                    v->notify_len = len;
                }
                if (t == 3) v->isr = map_cap(v, cap, 2);
                if (t == 4) v->config = map_cap(v, cap, 3);
            }
            cap = cfg8(v, cap + 1) & ~3;
        }
    }
    v->modern = v->common && v->notify && v->isr && v->config;
    if (!v->modern) {
        if (v->common || v->pci.device >= 0x1040 || !(v->pci.bar0 & 1))
            return -1;
        v->io = v->pci.bar0 & ~3U;
    }
    status(v, 0);
    int reset = 0;
    for (int i = 0; i < 1000000; i++) {
        if (!(v->modern ? v->common[20] : inb(v->io + 18))) {
            reset = 1;
            break;
        }
    }
    if (!reset) return -1;
    status(v, 1);
    status(v, 3);
    if (v->modern) {
        C32(v, 0) = 1;
        if (!(C32(v, 4) & 1)) goto fail;
        C32(v, 0) = 0;
        if ((C32(v, 4) & features) != features) goto fail;
        C32(v, 8) = 0;
        C32(v, 12) = features;
        C32(v, 8) = 1;
        C32(v, 12) = 1;
    } else {
        if ((inl(v->io) & features) != features) goto fail;
        outl(v->io + 4, features);
    }
    if (v->modern) status(v, 11);
    if (v->modern && !(v->common[20] & 8)) goto fail;
    kprintf("virtio: %s transport\n", v->modern ? "modern" : "legacy");
    return 0;
fail:
    virtio_fail(v);
    return -1;
}

int virtio_queue(struct virtio *v, struct virtq *q, uint16_t index)
{
    uint16_t n;
    if (v->modern) {
        C16(v, 22) = index;
        n = C16(v, 24);
        if (n > VIRTQ_MAX) n = VIRTQ_MAX;
        C16(v, 24) = n;
        C16(v, 26) = 0xffff;
        uint16_t notify_off = C16(v, 30);
        if (v->notify_len < 2 ||
            (uint64_t)notify_off * v->notify_mult > v->notify_len - 2) return -1;
        q->notify = (void *)(v->notify + (uint32_t)notify_off * v->notify_mult);
    } else {
        outw(v->io + 14, index);
        n = inw(v->io + 12);
    }
    if (!n || n > VIRTQ_MAX || (n & (n - 1))) return -1;
    int pages = (virtq_bytes(n) + 4095) / 4096;
    uint64_t mem = pmm_alloc_contig(pages);
    if (!mem) return -1;
    if (mem + pages * PAGE_SIZE > (1ULL << 32)) {
        pmm_free_contig(mem, pages);
        return -1;
    }
    virtq_init(q, P2V(mem), n);
    q->phys = mem;
    q->index = index;
    if (v->modern) {
        C64(v, 32) = mem;
        C64(v, 40) = mem + 16U * n;
        C64(v, 48) = mem + ((16U * n + 6 + 2U * n + 4095) & ~4095U);
        C16(v, 28) = 1;
    } else outl(v->io + 8, mem >> 12);
    return 0;
}

void virtio_notify(struct virtio *v, struct virtq *q)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (v->modern)
        *q->notify = q->index;
    else outw(v->io + 16, q->index);
}

uint8_t virtio_config8(struct virtio *v, uint16_t off)
{
    return v->modern ? v->config[off] : inb(v->io + 20 + off);
}

uint32_t virtio_config32(struct virtio *v, uint16_t off)
{
    if (!v->modern) return inl(v->io + 20 + off);
    return *(volatile uint32_t *)(v->config + off);
}
