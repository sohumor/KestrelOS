#include "kernel.h"
#include "ahci.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "blockdev.h"
#include "spinlock.h"
#include "string.h"

#define AHCI_VA 0xFFFFFFFFC0020000ULL
#define POLLS 10000000
#define DMA_SECTORS 8

struct ahci_disk {
    volatile uint32_t *port;
    uint64_t mem;
    int failed;
    spinlock_t lock;
    struct blockdev bd;
};

static struct ahci_disk disk;

static int wait_clear(volatile uint32_t *reg, uint32_t mask)
{
    for (int i = 0; i < POLLS; i++) {
        if (!(*reg & mask))
            return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

/* One slot, one PRDT entry, and a page-sized bounce buffer. */
static int command(struct ahci_disk *d, uint8_t op, uint64_t lba,
                   uint32_t count, int write)
{
    volatile uint32_t *p = d->port;
    uint32_t *header = P2V(d->mem);
    uint8_t *table = P2V(d->mem + 2048);
    if (d->failed || wait_clear(&p[8], 0x88) < 0)
        goto fail;
    memset(header, 0, 32);
    memset(table, 0, 256);
    header[0] = 5 | (write ? (1U << 6) : 0) | (count ? (1U << 16) : 0);
    header[2] = (uint32_t)(d->mem + 2048);
    table[0] = 0x27;
    table[1] = 0x80;
    table[2] = op;
    table[4] = lba;
    table[5] = lba >> 8;
    table[6] = lba >> 16;
    table[7] = 0x40;
    table[8] = lba >> 24;
    table[9] = lba >> 32;
    table[10] = lba >> 40;
    table[12] = count;
    table[13] = count >> 8;
    if (count) {
        uint32_t *prdt = (uint32_t *)(table + 128);
        prdt[0] = (uint32_t)(d->mem + PAGE_SIZE);
        prdt[3] = count * 512 - 1;
    }
    p[4] = 0xFFFFFFFF;
    __asm__ volatile("mfence" ::: "memory");
    p[14] = 1;
    for (int i = 0; i < POLLS; i++) {
        if ((p[4] & (1U << 30)) || (p[8] & 1))
            goto fail;
        if (!(p[14] & 1)) {
            __asm__ volatile("mfence" ::: "memory");
            return 0;
        }
        __asm__ volatile("pause");
    }
fail:
    /* Never reuse DMA storage after a timed-out command. */
    d->failed = 1;
    p[6] &= ~1U;
    kprintf("ahci: command failed (is=%x tfd=%x)\n", p[4], p[8]);
    return -1;
}

static int transfer(struct blockdev *bd, uint64_t lba, uint32_t count,
                    void *buf, int write)
{
    struct ahci_disk *d = bd->priv;
    if (lba >= (1ULL << 48) || lba > bd->blocks ||
        count > bd->blocks - lba)
        return -1;
    if (!count)
        return 0;
    uint64_t flags = spin_lock_irqsave(&d->lock);
    int result = 0;
    while (count) {
        uint32_t n = count > DMA_SECTORS ? DMA_SECTORS : count;
        void *bounce = P2V(d->mem + PAGE_SIZE);
        if (write)
            memcpy(bounce, buf, n * 512);
        if (command(d, write ? 0x35 : 0x25, lba, n, write) < 0) {
            result = -1;
            break;
        }
        if (!write)
            memcpy(buf, bounce, n * 512);
        lba += n;
        count -= n;
        buf = (uint8_t *)buf + n * 512;
    }
    if (!result && write)
        result = command(d, 0xEA, 0, 0, 0);
    spin_unlock_irqrestore(&d->lock, flags);
    return result;
}

static int read_blocks(struct blockdev *bd, uint64_t lba, uint32_t n, void *buf)
{
    return transfer(bd, lba, n, buf, 0);
}

static int write_blocks(struct blockdev *bd, uint64_t lba, uint32_t n,
                        const void *buf)
{
    return transfer(bd, lba, n, (void *)buf, 1);
}

void ahci_init(void)
{
    struct pci_dev dev;
    if (!pci_find_class(1, 6, 1, &dev))
        return;
    uint32_t bar = pci_read32(dev.bus, dev.dev, dev.fn, 0x24);
    if (!bar || (bar & 7))
        return;
    uint64_t phys = bar & ~0xFULL;
    for (uint64_t off = 0; off < 0x3000; off += PAGE_SIZE)
        vmm_map_page(vmm_kernel_pml4(), AHCI_VA + off,
                     (phys & ~0xFFFULL) + off, PTE_W | PTE_PCD);
    volatile uint32_t *hba = (void *)(AHCI_VA + (phys & 0xFFF));
    uint16_t cmd = pci_read16(dev.bus, dev.dev, dev.fn, 4);
    pci_write32(dev.bus, dev.dev, dev.fn, 4, cmd | 6);
    if (hba[9] & 1) {
        hba[10] |= 2;
        if (wait_clear(&hba[10], 0x11) < 0) {
            kprintf("ahci: BIOS handoff timed out\n");
            return;
        }
    }
    hba[1] = (hba[1] | (1U << 31)) & ~2U;
    for (int i = 0; i < 32; i++) {
        if (!(hba[3] & (1U << i)))
            continue;
        volatile uint32_t *p = hba + 0x40 + i * 0x20;
        if ((p[10] & 0xF0F) != 0x103 || p[9] != 0x101)
            continue;
        p[6] &= ~1U;
        if (wait_clear(&p[6], 1U << 15) < 0)
            continue;
        p[6] &= ~(1U << 4);
        if (wait_clear(&p[6], 1U << 14) < 0)
            continue;
        uint64_t mem = pmm_alloc_contig(2);
        if (!mem)
            return;
        if (mem + 2 * PAGE_SIZE > (1ULL << 32)) {
            pmm_free_contig(mem, 2);
            return;
        }
        memset(&disk, 0, sizeof(disk));
        disk.port = p;
        disk.mem = mem;
        p[0] = (uint32_t)mem;
        p[1] = 0;
        p[2] = (uint32_t)(mem + 1024);
        p[3] = 0;
        p[5] = 0;
        p[12] = 0xFFFFFFFF;
        p[4] = 0xFFFFFFFF;
        p[6] |= (1U << 4);
        p[6] |= 1;
        if (command(&disk, 0xEC, 0, 1, 0) < 0)
            return;
        uint16_t *id = P2V(mem + PAGE_SIZE);
        uint64_t sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                           ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
        if (!(id[83] & (1U << 10)) || !sectors || sectors > (1ULL << 48) ||
            ((id[106] & 0xD000) == 0x5000 &&
             (id[117] != 256 || id[118] != 0))) {
            kprintf("ahci: unsupported disk geometry\n");
            return;
        }
        strcpy(disk.bd.name, "sda");
        disk.bd.block_size = 512;
        disk.bd.blocks = sectors;
        disk.bd.priv = &disk;
        disk.bd.read = read_blocks;
        disk.bd.write = write_blocks;
        if (blockdev_register(&disk.bd) == 0)
            kprintf("ahci: port %d: %llu sectors\n", i, (unsigned long long)sectors);
        return;
    }
}
