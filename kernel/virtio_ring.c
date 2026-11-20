#include "virtio.h"

uint32_t virtq_bytes(uint16_t n)
{
    return ((16U * n + 6 + 2U * n + 4095) & ~4095U) + 6 + 8U * n;
}

int virtq_init(struct virtq *q, void *mem, uint16_t n)
{
    if (!n || n > VIRTQ_MAX || (n & (n - 1)))
        return -1;
    q->desc = mem;
    q->avail = (void *)((uint8_t *)mem + 16U * n);
    q->used = (void *)((uint8_t *)mem + ((16U * n + 6 + 2U * n + 4095) & ~4095U));
    q->size = q->free_count = n;
    q->free_head = q->last_used = 0;
    q->avail[0] = 1; /* Polling: suppress interrupts. */
    q->avail[1] = q->used[1] = 0;
    for (uint16_t i = 0; i < n; i++)
        q->desc[i].next = i + 1;
    return 0;
}

int virtq_submit(struct virtq *q, const struct virtq_buf *buf, uint16_t n)
{
    if (!n || n > q->free_count)
        return -1;
    uint16_t head = q->free_head, id = head;
    for (uint16_t i = 0; i < n; i++) {
        struct virtq_desc *d = &q->desc[id];
        uint16_t next = d->next;
        d->addr = buf[i].addr;
        d->len = buf[i].len;
        d->flags = (buf[i].flags & 2) | (i + 1 < n ? 1 : 0);
        id = next;
    }
    q->free_head = id;
    q->free_count -= n;
    uint16_t idx = q->avail[1];
    q->avail[2 + idx % q->size] = head;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail[1] = idx + 1;
    return head;
}

int virtq_poll(struct virtq *q, uint32_t *len)
{
    if (q->used[1] == q->last_used)
        return -1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    volatile uint32_t *e = (void *)&q->used[2 + 4 * (q->last_used % q->size)];
    uint32_t head = e[0];
    if (head >= q->size)
        return -2;
    uint16_t id = head, count = 0;
    for (;;) {
        if (id >= q->size || ++count > q->size)
            return -2;
        if (!(q->desc[id].flags & 1))
            break;
        id = q->desc[id].next;
    }
    if (count > q->size - q->free_count)
        return -2;
    *len = e[1];
    q->desc[id].next = q->free_head;
    q->free_head = head;
    q->free_count += count;
    q->last_used++;
    return (int)head;
}
