#include "kernel.h"
#include "hda.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "proc.h"
#include "timer.h"
#include "spinlock.h"
#include "string.h"

#define HDA_VA 0xFFFFFFFFA1000000ULL
#define PCM_BYTES (16 * 4096u)
#define PCM_FORMAT 0x0011
#define STREAM_TAG 1

struct hda_bdl {
    uint64_t addr;
    uint32_t length;
    uint32_t flags;
};

static volatile uint8_t *regs;
static volatile uint32_t *corb;
static volatile uint64_t *rirb;
static uint16_t corb_wp, rirb_rp, corb_mask, rirb_mask;
static unsigned int sd;
static uint8_t codec, dac, pin, afg;
static uint8_t path[16];
static int path_len;
static uint32_t amp_caps[16];
static uint8_t *pcm;
static uint32_t producer, consumer, queued;
static uint64_t last_progress;
static bool ready, opened, running;
static int volume = 75;
static spinlock_t lock = SPINLOCK_INIT;

static uint8_t r8(unsigned int off) { return regs[off]; }
static uint16_t r16(unsigned int off) { return *(volatile uint16_t *)(regs + off); }
static uint32_t r32(unsigned int off) { return *(volatile uint32_t *)(regs + off); }
static void w8(unsigned int off, uint8_t v) { regs[off] = v; }
static void w16(unsigned int off, uint16_t v) { *(volatile uint16_t *)(regs + off) = v; }
static void w32(unsigned int off, uint32_t v) { *(volatile uint32_t *)(regs + off) = v; }

static bool wait32(unsigned int off, uint32_t mask, uint32_t value)
{
    for (unsigned int i = 0; i < 1000000; i++) {
        if ((r32(off) & mask) == value)
            return true;
        __asm__ volatile("pause");
    }
    kprintf("hda: register timeout off=%x value=%x\n", off, r32(off));
    return false;
}

static bool verb(uint8_t nid, uint16_t op, uint16_t data, uint32_t *reply)
{
    uint16_t next = (corb_wp + 1) & corb_mask;
    corb[next] = ((uint32_t)codec << 28) | ((uint32_t)nid << 20) |
                 ((uint32_t)op << 8) | data;
    __asm__ volatile("mfence" ::: "memory");
    corb_wp = next;
    w16(0x48, next);
    for (unsigned int i = 0; i < 1000000; i++) {
        uint16_t wp = r16(0x58) & rirb_mask;
        while (rirb_rp != wp) {
            rirb_rp = (rirb_rp + 1) & rirb_mask;
            uint64_t entry = rirb[rirb_rp];
            if (!((entry >> 32) & 0x10) && ((entry >> 32) & 15) == codec) {
                *reply = (uint32_t)entry;
                w8(0x5D, 5);
                return true;
            }
        }
        __asm__ volatile("pause");
    }
    kprintf("hda: command timeout nid=%u op=%x wp=%u rp=%u cmask=%u rmask=%u\n",
            nid, op, corb_wp, r16(0x4A), corb_mask, rirb_mask);
    return false;
}

static bool set(uint8_t nid, uint16_t op, uint16_t data)
{
    uint32_t reply;
    return verb(nid, op, data, &reply);
}

static uint32_t param(uint8_t nid, uint8_t id)
{
    uint32_t reply = 0;
    verb(nid, 0xF00, id, &reply);
    return reply;
}

static bool route(uint8_t nid, int depth)
{
    if (depth >= 16)
        return false;
    for (int i = 0; i < depth; i++)
        if (path[i] == nid)
            return false;
    uint32_t caps = param(nid, 9);
    path[depth] = nid;
    if (((caps >> 20) & 15) == 0) {
        dac = nid;
        path_len = depth + 1;
        return true;
    }
    uint32_t list = param(nid, 0x0E);
    unsigned int count = list & 0x7F;
    bool wide = (list & 0x80) != 0;
    unsigned int per = wide ? 2 : 4;
    unsigned int previous = 0;
    for (unsigned int i = 0; i < count; i++) {
        uint32_t entries = 0;
        if (!verb(nid, 0xF02, i - i % per, &entries))
            return false;
        unsigned int entry = (entries >> ((i % per) * (wide ? 16 : 8))) &
                             (wide ? 0xFFFF : 0xFF);
        unsigned int target = entry & (wide ? 0x7FFF : 0x7F);
        unsigned int first = (entry & (wide ? 0x8000 : 0x80)) ? previous + 1 : target;
        for (unsigned int n = first; n <= target && n < 256; n++) {
            if (route((uint8_t)n, depth + 1)) {
                if (((caps >> 20) & 15) != 2 && !set(nid, 0x701, i))
                    return false;
                return true;
            }
        }
        previous = target;
    }
    return false;
}

static bool apply_volume(void)
{
    for (int i = 0; i < path_len; i++) {
        if (!(param(path[i], 9) & 4))
            continue;
        unsigned int steps = (amp_caps[i] >> 8) & 0x7F;
        uint16_t gain = (uint16_t)(steps * volume / 100);
        if (volume == 0)
            gain |= 0x80;
        if (!set(path[i], 0x300, 0xB000 | gain))
            return false;
    }
    return true;
}

static bool setup_codec(void)
{
    uint32_t groups = param(0, 4);
    unsigned int first = (groups >> 16) & 0xFF;
    unsigned int end = first + (groups & 0xFF);
    for (unsigned int g = first; g < end && g < 256; g++) {
        if ((param(g, 5) & 0xFF) != 1)
            continue;
        afg = g;
        if (!set(afg, 0x705, 0))
            continue;
        uint32_t nodes = param(afg, 4);
        unsigned int start = (nodes >> 16) & 0xFF;
        unsigned int limit = start + (nodes & 0xFF);
        for (unsigned int n = start; n < limit && n < 256; n++) {
            if (((param(n, 9) >> 20) & 15) != 4 || !(param(n, 0x0C) & 0x10))
                continue;
            if (!route(n, 0))
                continue;
            pin = n;
            for (int i = 0; i < path_len; i++) {
                uint32_t caps = param(path[i], 9);
                if ((caps >> 8) & 4)
                    set(path[i], 0x705, 0);
                amp_caps[i] = param((caps & 8) ? path[i] : afg, 0x12);
            }
            if (set(dac, 0x706, STREAM_TAG << 4) &&
                set(dac, 0x200, PCM_FORMAT) && set(pin, 0x707, 0x40) &&
                set(pin, 0x70C, 2) && apply_volume())
                return true;
        }
    }
    return false;
}

static uint16_t ring_size(unsigned int off)
{
    uint8_t caps = r8(off);
    uint8_t size = (caps & 0x40) ? 2 : (caps & 0x20) ? 1 : 0;
    w8(off, size);
    return size == 2 ? 255 : size == 1 ? 15 : 1;
}

void hda_init(void)
{
    uint32_t bar = 0;
    uint64_t cp = 0, rp = 0;
    for (unsigned int b = 0; b < 256 && !bar; b++) {
        for (unsigned int d = 0; d < 32 && !bar; d++) {
            if ((pci_read32(b, d, 0, 0) & 0xFFFF) == 0xFFFF)
                continue;
            unsigned int count = (pci_read8(b, d, 0, 0x0E) & 0x80) ? 8 : 1;
            for (unsigned int f = 0; f < count; f++) {
                if ((pci_read32(b, d, f, 8) >> 16) != 0x0403)
                    continue;
                uint32_t addr = pci_read32(b, d, f, 0x10);
                if ((addr & 1) || !(addr & ~15u))
                    continue;
                if ((addr & 6) == 4 && pci_read32(b, d, f, 0x14))
                    continue;
                bar = addr & ~15u;
                uint32_t cmd = pci_read32(b, d, f, 4);
                pci_write32(b, d, f, 4, (cmd & 0xFFFF) | 6);
                break;
            }
        }
    }
    if (!bar) {
        kprintf("hda: no controller\n");
        return;
    }
    for (unsigned int off = 0; off < 0x4000; off += PAGE_SIZE)
        vmm_map_page(vmm_kernel_pml4(), HDA_VA + off, bar + off, PTE_W | PTE_PCD);
    regs = (volatile uint8_t *)HDA_VA;
    w32(0x20, 0);
    w32(8, r32(8) & ~1u);
    if (!wait32(8, 1, 0))
        goto fail;
    timer_sleep(1);
    w32(8, r32(8) | 1);
    if (!wait32(8, 1, 1))
        goto fail;
    timer_sleep(1);
    uint16_t codecs = r16(0x0E);
    w8(0x4C, 0);
    w8(0x5C, 0);
    cp = pmm_alloc_contig(1);
    rp = pmm_alloc_contig(1);
    corb = P2V(cp);
    rirb = P2V(rp);
    w32(0x40, cp); w32(0x44, cp >> 32);
    w32(0x50, rp); w32(0x54, rp >> 32);
    corb_mask = ring_size(0x4E);
    rirb_mask = ring_size(0x5E);
    w16(0x48, 0);
    w16(0x4A, 0x8000);
    w16(0x4A, 0);
    w16(0x58, 0x8000);
    w16(0x5A, 255);
    w8(0x5D, 5);
    w8(0x5C, 2);
    w8(0x4C, 2);
    for (codec = 0; codec < 15; codec++)
        if ((codecs & (1u << codec)) && setup_codec())
            break;
    if (codec == 15 || !((r16(0) >> 12) & 15))
        goto fail;
    sd = 0x80 + ((r16(0) >> 8) & 15) * 0x20;
    w8(sd, 0);
    w8(sd, 1);
    w8(sd, 0);
    if (!wait32(sd, 1, 0))
        goto fail;
    uint64_t pp = pmm_alloc_contig(PCM_BYTES / PAGE_SIZE);
    uint64_t bp = pmm_alloc_contig(1);
    struct hda_bdl *bdl = P2V(bp);
    pcm = P2V(pp);
    for (unsigned int i = 0; i < PCM_BYTES / PAGE_SIZE; i++) {
        bdl[i].addr = pp + i * PAGE_SIZE;
        bdl[i].length = PAGE_SIZE;
    }
    w32(sd + 8, PCM_BYTES);
    w16(sd + 0x0C, PCM_BYTES / PAGE_SIZE - 1);
    w16(sd + 0x12, PCM_FORMAT);
    w32(sd + 0x18, bp); w32(sd + 0x1C, bp >> 32);
    w8(sd + 2, STREAM_TAG << 4);
    ready = true;
    kprintf("hda: codec %u dac nid=%u pin nid=%u\n", codec, dac, pin);
    return;
fail:
    w8(0x4C, 0);
    w8(0x5C, 0);
    if (cp)
        pmm_free(cp);
    if (rp)
        pmm_free(rp);
    kprintf("hda: controller initialization failed gcap=%x codecs=%x corb=%x rirb=%x\n",
            r16(0), r16(0x0E), r16(0x4A), r16(0x58));
}

bool hda_ready(void) { return ready; }

static void stop(void)
{
    w8(sd, r8(sd) & ~2u);
    running = false;
    queued = 0;
}

static void poll_locked(void)
{
    if (!running)
        return;
    uint32_t pos = r32(sd + 4) % PCM_BYTES;
    uint32_t used = (pos + PCM_BYTES - consumer) % PCM_BYTES;
    if (used) {
        uint32_t tail = PCM_BYTES - consumer;
        if (tail > used)
            tail = used;
        memset(pcm + consumer, 0, tail);
        memset(pcm, 0, used - tail);
        consumer = pos;
        queued = used >= queued ? 0 : queued - used;
        last_progress = timer_ticks();
        if (!queued)
            stop();
    }
    if (timer_ticks() - last_progress > TIMER_HZ * 2)
        stop();
}

void hda_poll(void)
{
    if (!ready)
        return;
    uint64_t flags = spin_lock_irqsave(&lock);
    poll_locked();
    spin_unlock_irqrestore(&lock, flags);
}

int hda_open(void)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    int rc = -1;
    if (ready && !opened) {
        opened = true;
        rc = 0;
    }
    spin_unlock_irqrestore(&lock, flags);
    return rc;
}

long hda_write(const void *buf, unsigned long n)
{
    if (!ready || !opened || (n & 3))
        return -1;
    unsigned long done = 0;
    while (done < n) {
        uint64_t flags = spin_lock_irqsave(&lock);
        poll_locked();
        if (!running) {
            producer = consumer = r32(sd + 4) % PCM_BYTES;
            memset(pcm, 0, PCM_BYTES);
        }
        uint32_t count = PCM_BYTES - 4096 - queued;
        if (count > n - done)
            count = n - done;
        if (count > PCM_BYTES - producer)
            count = PCM_BYTES - producer;
        if (count) {
            memcpy(pcm + producer, (const uint8_t *)buf + done, count);
            producer = (producer + count) % PCM_BYTES;
            queued += count;
            done += count;
            __asm__ volatile("mfence" ::: "memory");
            if (!running) {
                w8(sd + 3, 0x1C);
                w8(sd, 2);
                running = true;
                last_progress = timer_ticks();
            }
        }
        spin_unlock_irqrestore(&lock, flags);
        if (!count)
            task_sleep_ticks(1);
    }
    return done;
}

void hda_close(void)
{
    if (!ready)
        return;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&lock);
        poll_locked();
        bool drained = !running;
        if (drained)
            opened = false;
        spin_unlock_irqrestore(&lock, flags);
        if (drained)
            return;
        task_sleep_ticks(1);
    }
}

int hda_volume(void)
{
    return __atomic_load_n(&volume, __ATOMIC_RELAXED);
}

int hda_set_volume(int value)
{
    if (!ready || value < 0 || value > 100)
        return -1;
    uint64_t flags = spin_lock_irqsave(&lock);
    int old = volume;
    volume = value;
    bool ok = apply_volume();
    if (!ok)
        volume = old;
    spin_unlock_irqrestore(&lock, flags);
    return ok ? 0 : -1;
}
