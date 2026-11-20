#pragma once
#include <stdint.h>
#include "pci.h"

#define VIRTQ_MAX 256
struct virtq_desc { uint64_t addr; uint32_t len; uint16_t flags, next; };
struct virtq_buf { uint64_t addr; uint32_t len; uint16_t flags; };
struct virtq {
    uint64_t phys;
    volatile uint16_t *notify;
    uint16_t index;
    struct virtq_desc *desc;
    volatile uint16_t *avail, *used;
    uint16_t size, free_head, free_count, last_used;
};
uint32_t virtq_bytes(uint16_t n);
int virtq_init(struct virtq *q, void *mem, uint16_t n);
int virtq_submit(struct virtq *q, const struct virtq_buf *buf, uint16_t n);
int virtq_poll(struct virtq *q, uint32_t *len);
struct virtio {
    struct pci_dev pci;
    volatile uint8_t *common, *notify, *config, *isr;
    uint32_t notify_mult;
    uint32_t notify_len;
    uint64_t window;
    uint16_t io;
    uint8_t modern;
};
int virtio_open(struct virtio *v, uint16_t type, uint32_t features);
int virtio_queue(struct virtio *v, struct virtq *q, uint16_t index);
void virtio_ready(struct virtio *v);
void virtio_fail(struct virtio *v);
void virtio_notify(struct virtio *v, struct virtq *q);
uint8_t virtio_config8(struct virtio *v, uint16_t off);
uint32_t virtio_config32(struct virtio *v, uint16_t off);
