/* Host regression tests for kernel/tcp_reassembly.c.
 *
 * The reassembler is pure ring-buffer logic, so it can be tested much more
 * aggressively here than through a timing-sensitive virtual network. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tcp_reassembly.h"

static int checks;
static int failures;

#define CHECK(cond, name) do {                                           \
    checks++;                                                            \
    if (!(cond)) {                                                       \
        failures++;                                                      \
        fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);         \
    }                                                                    \
} while (0)

static void fresh(struct tcp_reassembly *r, uint8_t *ring)
{
    memset(ring, 0, TCP_RXBUF);
    memset(r, 0xFF, sizeof(*r));
    tcp_reassembly_reset(r);
}

static void test_in_order(void)
{
    struct tcp_reassembly r;
    uint8_t ring[TCP_RXBUF];
    int len = 0;
    uint32_t next = 1000;

    fresh(&r, ring);
    int n = tcp_reassembly_accept(&r, ring, 0, &len, &next, 1000,
                                  (const uint8_t *)"abc", 3);
    CHECK(n == 3, "in-order bytes advance immediately");
    CHECK(len == 3 && next == 1003, "in-order state");
    CHECK(memcmp(ring, "abc", 3) == 0, "in-order payload");
}

static void test_reverse_order(void)
{
    struct tcp_reassembly r;
    uint8_t ring[TCP_RXBUF];
    int len = 0;
    uint32_t next = 2000;

    fresh(&r, ring);
    int n = tcp_reassembly_accept(&r, ring, 0, &len, &next, 2003,
                                  (const uint8_t *)"def", 3);
    CHECK(n == 0 && len == 0 && next == 2000,
          "out-of-order suffix waits for hole");

    n = tcp_reassembly_accept(&r, ring, 0, &len, &next, 2000,
                              (const uint8_t *)"abc", 3);
    CHECK(n == 6, "hole fill drains buffered suffix");
    CHECK(len == 6 && next == 2006, "reverse-order state");
    CHECK(memcmp(ring, "abcdef", 6) == 0, "reverse-order payload");
}

static void test_overlap_and_duplicates(void)
{
    struct tcp_reassembly r;
    uint8_t ring[TCP_RXBUF];
    int len = 0;
    uint32_t next = 3000;

    fresh(&r, ring);
    tcp_reassembly_accept(&r, ring, 0, &len, &next, 3002,
                          (const uint8_t *)"cd", 2);
    tcp_reassembly_accept(&r, ring, 0, &len, &next, 3002,
                          (const uint8_t *)"XY", 2);
    int n = tcp_reassembly_accept(&r, ring, 0, &len, &next, 3000,
                                  (const uint8_t *)"ab", 2);
    CHECK(n == 4, "duplicate suffix is counted once");
    CHECK(memcmp(ring, "abcd", 4) == 0,
          "first buffered copy wins over conflicting duplicate");

    n = tcp_reassembly_accept(&r, ring, 0, &len, &next, 3002,
                              (const uint8_t *)"cdef", 4);
    CHECK(n == 2 && len == 6 && next == 3006,
          "duplicate prefix is trimmed");
    CHECK(memcmp(ring, "abcdef", 6) == 0, "trimmed overlap payload");
}

static void test_window_and_ring_wrap(void)
{
    struct tcp_reassembly r;
    uint8_t ring[TCP_RXBUF];
    int len = TCP_RXBUF - 2;
    uint32_t next = 4000;

    fresh(&r, ring);
    int n = tcp_reassembly_accept(&r, ring, 0, &len, &next, 4000,
                                  (const uint8_t *)"WXYZ", 4);
    CHECK(n == 2 && len == TCP_RXBUF && next == 4002,
          "segment is clipped at receive-window edge");
    CHECK(ring[TCP_RXBUF - 2] == 'W' && ring[TCP_RXBUF - 1] == 'X',
          "window-edge bytes stored");

    fresh(&r, ring);
    len = 2;
    next = 5002;
    ring[TCP_RXBUF - 2] = 'a';
    ring[TCP_RXBUF - 1] = 'b';
    tcp_reassembly_accept(&r, ring, TCP_RXBUF - 2, &len, &next, 5004,
                          (const uint8_t *)"ef", 2);

    /* Simulate the application consuming "ab". The append position remains
     * stable even though both head and contiguous length change. */
    len = 0;
    n = tcp_reassembly_accept(&r, ring, 0, &len, &next, 5002,
                              (const uint8_t *)"cd", 2);
    CHECK(n == 4 && len == 4 && next == 5006,
          "reassembly survives receive-head movement");
    CHECK(memcmp(ring, "cdef", 4) == 0, "wrapped ring payload");
}

static void test_sequence_wrap(void)
{
    struct tcp_reassembly r;
    uint8_t ring[TCP_RXBUF];
    int len = 0;
    uint32_t next = UINT32_MAX - 1;

    fresh(&r, ring);
    tcp_reassembly_accept(&r, ring, 0, &len, &next, 0,
                          (const uint8_t *)"cd", 2);
    int n = tcp_reassembly_accept(&r, ring, 0, &len, &next,
                                  UINT32_MAX - 1,
                                  (const uint8_t *)"ab", 2);
    CHECK(n == 4 && len == 4 && next == 2,
          "sequence comparison wraps at 2^32");
    CHECK(memcmp(ring, "abcd", 4) == 0, "sequence-wrap payload");
}

static void test_rejections(void)
{
    struct tcp_reassembly r;
    uint8_t ring[TCP_RXBUF];
    int len = 0;
    uint32_t next = 6000;

    fresh(&r, ring);
    CHECK(tcp_reassembly_accept(&r, ring, 0, &len, &next, 6000, 0, 1) == 0,
          "null data rejected");
    CHECK(tcp_reassembly_accept(&r, ring, -1, &len, &next, 6000,
                                (const uint8_t *)"x", 1) == 0,
          "invalid ring head rejected");

    len = TCP_RXBUF - 2;
    CHECK(tcp_reassembly_accept(&r, ring, 0, &len, &next, 6002,
                                (const uint8_t *)"x", 1) == 0,
          "data at closed window edge rejected");
}

static void test_fin_boundary_discard(void)
{
    struct tcp_reassembly r;
    uint8_t ring[TCP_RXBUF];
    int len = 0;
    uint32_t next = 7000;

    fresh(&r, ring);
    tcp_reassembly_accept(&r, ring, 0, &len, &next, 7002,
                          (const uint8_t *)"cdef", 4);
    tcp_reassembly_discard_from(&r, 0, len, next, 7004);
    int n = tcp_reassembly_accept(&r, ring, 0, &len, &next, 7000,
                                  (const uint8_t *)"ab", 2);
    CHECK(n == 4 && len == 4 && next == 7004,
          "FIN boundary discards buffered bytes after stream end");
    CHECK(memcmp(ring, "abcd", 4) == 0, "FIN boundary keeps prior bytes");

    n = tcp_reassembly_accept(&r, ring, 0, &len, &next, 7004,
                              (const uint8_t *)"XY", 2);
    CHECK(n == 2 && len == 6 && next == 7006,
          "discarded ring slots can be reused");
}

int main(void)
{
    test_in_order();
    test_reverse_order();
    test_overlap_and_duplicates();
    test_window_and_ring_wrap();
    test_sequence_wrap();
    test_rejections();
    test_fin_boundary_discard();

    if (failures) {
        fprintf(stderr, "%d/%d TCP reassembly checks failed\n",
                failures, checks);
        return 1;
    }
    printf("PASS: %d TCP reassembly checks\n", checks);
    return 0;
}
