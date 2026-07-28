#pragma once

#include <stdint.h>

#include "tcp.h"

/* Presence bits for receive-ring slots which hold data beyond rcv_nxt.
 * The bytes themselves live in the normal TCP receive ring. */
struct tcp_reassembly {
    uint8_t present[(TCP_RXBUF + 7) / 8];
};

void tcp_reassembly_reset(struct tcp_reassembly *r);

/* Insert one segment into the receive ring.
 *
 * head/contiguous describe the application-visible bytes already in the
 * ring, and next is the sequence immediately after those bytes. Reordered
 * bytes are retained in unused ring slots. The function advances contiguous
 * and next when the segment fills the current hole, including through any
 * already-buffered bytes which follow it.
 *
 * Returns the number of bytes newly made contiguous, or 0 when the segment is
 * a duplicate or lies outside the current receive window.
 */
int tcp_reassembly_accept(struct tcp_reassembly *r, uint8_t *ring,
                          int head, int *contiguous, uint32_t *next,
                          uint32_t seq, const uint8_t *data, int data_len);

/* Forget buffered bytes at and after seq. Used when a FIN establishes the
 * end of the peer's byte stream before an earlier hole has arrived. */
void tcp_reassembly_discard_from(struct tcp_reassembly *r, int head,
                                 int contiguous, uint32_t next, uint32_t seq);
