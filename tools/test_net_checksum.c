/* Host tests for kernel/net_checksum.c. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "net.h"

static int checks;
static int failures;

#define CHECK(cond, name) do {                                           \
    checks++;                                                            \
    if (!(cond)) {                                                       \
        failures++;                                                      \
        fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);         \
    }                                                                    \
} while (0)

static void test_ipv4_header(void)
{
    /* RFC 1071-style IPv4 header vector; checksum field starts as zero. */
    uint8_t header[20] = {
        0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00,
        0x40, 0x11, 0x00, 0x00, 0xC0, 0xA8, 0x00, 0x01,
        0xC0, 0xA8, 0x00, 0xC7,
    };

    uint16_t sum = net_checksum(header, sizeof(header));
    CHECK(ntohs(sum) == 0xB861, "known IPv4 header checksum");
    memcpy(header + 10, &sum, sizeof(sum));
    CHECK(net_checksum(header, sizeof(header)) == 0,
          "completed IPv4 header verifies");
    header[3] ^= 1;
    CHECK(net_checksum(header, sizeof(header)) != 0,
          "IPv4 corruption is detected");
}

static void test_udp_pseudo_header(void)
{
    /* 192.168.1.10:12345 -> 192.168.1.20:53, payload "hello". */
    uint32_t src = htonl(0xC0A8010A);
    uint32_t dst = htonl(0xC0A80114);
    uint8_t segment[13] = {
        0x30, 0x39, 0x00, 0x35, 0x00, 0x0D, 0x00, 0x00,
        'h', 'e', 'l', 'l', 'o',
    };

    uint16_t sum = net_transport_checksum(src, dst, 17,
                                           segment, sizeof(segment));
    CHECK(ntohs(sum) == 0x0825, "known odd-length UDP checksum");
    memcpy(segment + 6, &sum, sizeof(sum));
    CHECK(net_transport_checksum(src, dst, 17,
                                 segment, sizeof(segment)) == 0,
          "completed UDP segment verifies");
    CHECK(net_transport_checksum(src, htonl(0xC0A80115), 17,
                                 segment, sizeof(segment)) != 0,
          "UDP checksum binds destination address");
    CHECK(net_transport_checksum(src, dst, 6,
                                 segment, sizeof(segment)) != 0,
          "transport checksum binds protocol");
    segment[12] ^= 1;
    CHECK(net_transport_checksum(src, dst, 17,
                                 segment, sizeof(segment)) != 0,
          "UDP payload corruption is detected");
}

int main(void)
{
    test_ipv4_header();
    test_udp_pseudo_header();
    if (failures) {
        fprintf(stderr, "%d/%d network checksum checks failed\n",
                failures, checks);
        return 1;
    }
    printf("PASS: %d network checksum checks\n", checks);
    return 0;
}
