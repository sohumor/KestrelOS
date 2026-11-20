#include "virtio_net.h"
#include "virtio_net_core.h"
#include "virtio.h"
#include "kernel.h"
#include "pmm.h"
#include "vmm.h"
#include "netdev.h"
#include "net.h"
#include "spinlock.h"
#include "string.h"

#define BUF_SIZE 2048
#define TX_BUFFERS 16

static struct virtio device;
static struct virtq rx, tx;
static uint64_t rx_mem, tx_mem;
static uint16_t tx_slot[VIRTQ_MAX];
static uint8_t busy[TX_BUFFERS];
static int header_size, present;
static spinlock_t rx_lock, tx_lock;

static uint64_t alloc_dma(int pages)
{
    uint64_t mem = pmm_alloc_contig(pages);
    if (mem && mem + pages * PAGE_SIZE > (1ULL << 32)) {
        pmm_free_contig(mem, pages);
        return 0;
    }
    return mem;
}

static void tx_reclaim(void)
{
    uint32_t len;
    int id;
    while ((id = virtq_poll(&tx, &len)) >= 0)
        busy[tx_slot[id]] = 0;
    if (id == -2) {
        present = 0;
        virtio_fail(&device);
    }
}

static int send_frame(const void *frame, int len)
{
    if (!present || len < 14 || len > ETH_MTU + 14) return -1;
    uint64_t flags = spin_lock_irqsave(&tx_lock);
    tx_reclaim();
    int slot;
    for (slot = 0; slot < TX_BUFFERS && busy[slot]; slot++) {}
    if (!present || slot == TX_BUFFERS) {
        spin_unlock_irqrestore(&tx_lock, flags);
        return -1;
    }
    uint64_t phys = tx_mem + slot * BUF_SIZE;
    uint8_t *buf = P2V(phys);
    int padded = len < 60 ? 60 : len;
    memset(buf, 0, header_size + padded);
    memcpy(buf + header_size, frame, len);
    struct virtq_buf b = {phys, header_size + padded, 0};
    int id = virtq_submit(&tx, &b, 1);
    if (id >= 0) {
        busy[slot] = 1;
        tx_slot[id] = slot;
        virtio_notify(&device, &tx);
    }
    spin_unlock_irqrestore(&tx_lock, flags);
    return id < 0 ? -1 : len;
}

static void poll_frames(void)
{
    if (!present) return;
    uint64_t flags = spin_lock_irqsave(&tx_lock);
    tx_reclaim();
    spin_unlock_irqrestore(&tx_lock, flags);
    flags = spin_lock_irqsave(&rx_lock);
    uint32_t len;
    int id = -1, refilled = 0;
    while (present && (id = virtq_poll(&rx, &len)) >= 0) {
        uint64_t phys = rx_mem + id * BUF_SIZE;
        const uint8_t *buf = P2V(phys);
        int frame_len = virtio_net_frame_len(buf, len, device.modern);
        if (frame_len >= 0)
            net_rx(buf + header_size, frame_len);
        struct virtq_buf b = {phys, BUF_SIZE, 2};
        if (virtq_submit(&rx, &b, 1) < 0) {
            present = 0;
            virtio_fail(&device);
            break;
        }
        refilled = 1;
    }
    if (present && id == -2) {
        present = 0;
        virtio_fail(&device);
    }
    if (refilled && present) virtio_notify(&device, &rx);
    spin_unlock_irqrestore(&rx_lock, flags);
}

static struct netdev netdev = {
    .name = "virtio-net", .send = send_frame, .poll = poll_frames,
};

bool virtio_net_init(void)
{
    if (virtio_open(&device, 1, 1U << 5) < 0) return false;
    if (virtio_queue(&device, &rx, 0) < 0 ||
        virtio_queue(&device, &tx, 1) < 0) goto fail;
    header_size = device.modern ? 12 : 10;
    for (int i = 0; i < 6; i++) netdev.mac[i] = virtio_config8(&device, i);
    rx_mem = alloc_dma((rx.size * BUF_SIZE + PAGE_SIZE - 1) / PAGE_SIZE);
    tx_mem = alloc_dma(TX_BUFFERS * BUF_SIZE / PAGE_SIZE);
    if (!rx_mem || !tx_mem) goto fail;
    for (int i = 0; i < rx.size; i++) {
        struct virtq_buf b = {rx_mem + i * BUF_SIZE, BUF_SIZE, 2};
        if (virtq_submit(&rx, &b, 1) < 0) goto fail;
    }
    virtio_ready(&device);
    present = 1;
    virtio_notify(&device, &rx);
    netdev_register(&netdev);
    kprintf("virtio-net: %s, mac %02x:%02x:%02x:%02x:%02x:%02x\n",
            device.modern ? "modern" : "legacy", netdev.mac[0], netdev.mac[1],
            netdev.mac[2], netdev.mac[3], netdev.mac[4], netdev.mac[5]);
    return true;
fail:
    virtio_fail(&device);
    return false;
}
