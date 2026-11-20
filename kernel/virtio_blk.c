#include "virtio.h"
#include "kernel.h"
#include "pmm.h"
#include "vmm.h"
#include "blockdev.h"
#include "spinlock.h"
#include "string.h"

static struct virtio transport;
static struct virtq queue;
static struct blockdev disk;
static spinlock_t lock;
static uint64_t dma;
static int failed;

static int transfer(struct blockdev *bd, uint64_t lba, uint32_t count,
                    void *buf, int write)
{
    if (lba > bd->blocks || count > bd->blocks - lba) return -1;
    uint64_t flags = spin_lock_irqsave(&lock);
    int result = 0;
    while (count) {
        if (failed) { result = -1; break; }
        uint32_t n = count > 8 ? 8 : count;
        uint32_t *header = P2V(dma);
        volatile uint8_t *status = P2V(dma + 16);
        void *bounce = P2V(dma + PAGE_SIZE);
        header[0] = write ? 1 : 0;
        header[1] = 0;
        header[2] = lba;
        header[3] = lba >> 32;
        *status = 0xff;
        if (write) memcpy(bounce, buf, n * 512);
        struct virtq_buf chain[3] = {
            {dma, 16, 0}, {dma + PAGE_SIZE, n * 512, write ? 0 : 2},
            {dma + 16, 1, 2}
        };
        int head = virtq_submit(&queue, chain, 3), done = -1;
        uint32_t len;
        if (head >= 0) {
            virtio_notify(&transport, &queue);
            for (int i = 0; i < 10000000; i++) {
                done = virtq_poll(&queue, &len);
                if (done != -1) break;
                __asm__ volatile("pause");
            }
        }
        if (head < 0 || done != head) {
            failed = 1;
            virtio_fail(&transport);
            result = -1;
            break;
        }
        if (*status != 0) { result = -1; break; }
        if (!write) memcpy(buf, bounce, n * 512);
        lba += n;
        count -= n;
        buf = (uint8_t *)buf + n * 512;
    }
    spin_unlock_irqrestore(&lock, flags);
    return result;
}

static int read_blocks(struct blockdev *bd, uint64_t lba, uint32_t n, void *buf)
{ return transfer(bd, lba, n, buf, 0); }
static int write_blocks(struct blockdev *bd, uint64_t lba, uint32_t n, const void *buf)
{ return transfer(bd, lba, n, (void *)buf, 1); }

void virtio_blk_init(void)
{
    if (virtio_open(&transport, 2, 0) < 0) return;
    if (virtio_queue(&transport, &queue, 0) < 0) goto fail;
    dma = pmm_alloc_contig(2);
    if (!dma) goto fail;
    if (dma + 2 * PAGE_SIZE > (1ULL << 32)) {
        pmm_free_contig(dma, 2);
        goto fail;
    }
    uint8_t gen;
    do {
        gen = transport.modern ? transport.common[21] : 0;
        disk.blocks = virtio_config32(&transport, 0);
        disk.blocks |= (uint64_t)virtio_config32(&transport, 4) << 32;
    } while (transport.modern && gen != transport.common[21]);
    if (!disk.blocks) goto fail;
    strcpy(disk.name, "vda");
    disk.block_size = 512;
    disk.read = read_blocks;
    disk.write = write_blocks;
    virtio_ready(&transport);
    if (blockdev_register(&disk) < 0) goto fail;
    kprintf("virtio-blk: vda: %llu sectors\n", (unsigned long long)disk.blocks);
    return;
fail:
    virtio_fail(&transport);
}
