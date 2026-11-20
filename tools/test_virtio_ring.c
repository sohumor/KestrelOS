#include "virtio.h"
#include "virtio_net_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static _Alignas(4096) unsigned char mem[16384];
int main(void)
{
    struct virtq q;
    uint32_t len;
    assert(virtq_bytes(256) == 10246);
    assert(virtq_init(&q, mem, 0) == -1);
    assert(virtq_init(&q, mem, 3) == -1);
    assert(virtq_init(&q, mem, 512) == -1);
    assert(virtq_init(&q, mem, 8) == 0);
    assert((unsigned char *)q.used - mem == 4096);
    assert(virtq_poll(&q, &len) == -1);
    struct virtq_buf b[3] = {{123, 16, 0}, {456, 512, 2}, {789, 1, 2}};
    assert(virtq_submit(&q, b, 0) == -1);
    for (unsigned i = 0; i < 65540; i++) {
        int head = virtq_submit(&q, b, 3);
        assert(head >= 0 && q.free_count == 5);
        assert(q.desc[head].addr == 123 && q.desc[head].flags == 1);
        uint16_t next = q.desc[head].next;
        assert(q.desc[next].flags == 3);
        volatile uint32_t *e = (void *)&q.used[2 + 4 * (q.last_used % q.size)];
        e[0] = head; e[1] = 513;
        q.used[1]++;
        assert(virtq_poll(&q, &len) == head && len == 513);
        assert(q.free_count == 8);
        assert(virtq_poll(&q, &len) == -1);
    }
    assert(virtq_submit(&q, b, 3) >= 0);
    assert(virtq_submit(&q, b, 3) >= 0);
    assert(virtq_submit(&q, b, 3) == -1);
    volatile uint32_t *e = (void *)&q.used[2 + 4 * (q.last_used % q.size)];
    e[0] = 99; q.used[1]++;
    assert(virtq_poll(&q, &len) == -2);
    struct virtq other;
    static _Alignas(4096) unsigned char other_mem[8192];
    assert(virtq_init(&other, other_mem, 8) == 0);
    assert(other.free_count == 8 && q.free_count == 2);
    assert(virtq_submit(&other, b, 1) == 0);
    assert(other.avail[1] == 1 && q.avail[1] != 1);
    uint8_t frame[1536] = {0};
    assert(virtio_net_frame_len(frame, 0, 1) == -1);
    assert(virtio_net_frame_len(frame, 25, 1) == -1);
    assert(virtio_net_frame_len(frame, 26, 1) == 14);
    assert(virtio_net_frame_len(frame, 24, 0) == 14);
    assert(virtio_net_frame_len(frame, 1526, 1) == 1514);
    assert(virtio_net_frame_len(frame, 1527, 1) == -1);
    frame[0] = 1;
    assert(virtio_net_frame_len(frame, 60, 1) == -1);
    frame[0] = 0; frame[1] = 1;
    assert(virtio_net_frame_len(frame, 60, 0) == -1);
    puts("virtio ring tests passed");
    return 0;
}
