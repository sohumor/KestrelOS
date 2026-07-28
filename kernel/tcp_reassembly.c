#include "tcp_reassembly.h"

static int bit_test(const struct tcp_reassembly *r, int pos)
{
    return (r->present[pos >> 3] >> (pos & 7)) & 1;
}

static void bit_set(struct tcp_reassembly *r, int pos)
{
    r->present[pos >> 3] |= (uint8_t)(1U << (pos & 7));
}

static void bit_clear(struct tcp_reassembly *r, int pos)
{
    r->present[pos >> 3] &= (uint8_t)~(1U << (pos & 7));
}

void tcp_reassembly_reset(struct tcp_reassembly *r)
{
    if (!r)
        return;
    for (int i = 0; i < (int)sizeof(r->present); i++)
        r->present[i] = 0;
}

int tcp_reassembly_accept(struct tcp_reassembly *r, uint8_t *ring,
                          int head, int *contiguous, uint32_t *next,
                          uint32_t seq, const uint8_t *data, int data_len)
{
    if (!r || !ring || !contiguous || !next || !data || data_len <= 0 ||
        head < 0 || head >= TCP_RXBUF ||
        *contiguous < 0 || *contiguous > TCP_RXBUF)
        return 0;

    /* TCP sequence comparisons use the signed difference so wrap at 2^32
     * behaves normally. A segment is at most one MSS, far below 2^31. */
    int32_t delta = (int32_t)(seq - *next);
    if (delta < 0) {
        uint32_t duplicate = *next - seq;
        if (duplicate >= (uint32_t)data_len)
            return 0;
        data += duplicate;
        data_len -= (int)duplicate;
        seq += duplicate;
        delta = 0;
    }

    int window = TCP_RXBUF - *contiguous;
    if (delta >= window)
        return 0;

    int accepted = data_len;
    if (accepted > window - delta)
        accepted = window - delta;

    /* head + contiguous is invariant when the application consumes data:
     * head advances by exactly as much as contiguous shrinks. It is
     * therefore a stable ring position for rcv_nxt while holes exist. */
    int tail = (head + *contiguous) % TCP_RXBUF;
    for (int i = 0; i < accepted; i++) {
        int pos = (tail + delta + i) % TCP_RXBUF;
        if (!bit_test(r, pos)) {
            ring[pos] = data[i];
            bit_set(r, pos);
        }
    }

    int advanced = 0;
    while (*contiguous < TCP_RXBUF) {
        tail = (head + *contiguous) % TCP_RXBUF;
        if (!bit_test(r, tail))
            break;
        bit_clear(r, tail);
        (*contiguous)++;
        (*next)++;
        advanced++;
    }
    return advanced;
}

void tcp_reassembly_discard_from(struct tcp_reassembly *r, int head,
                                 int contiguous, uint32_t next, uint32_t seq)
{
    if (!r || head < 0 || head >= TCP_RXBUF ||
        contiguous < 0 || contiguous > TCP_RXBUF)
        return;

    int32_t delta = (int32_t)(seq - next);
    int window = TCP_RXBUF - contiguous;
    if (delta < 0 || delta >= window)
        return;

    int tail = (head + contiguous) % TCP_RXBUF;
    for (int off = delta; off < window; off++)
        bit_clear(r, (tail + off) % TCP_RXBUF);
}
