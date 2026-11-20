/* Host tests for kernel/net_checksum.c. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "net.h"
#include "ip6_text.h"

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

static void test_ipv6_pseudo_header(void)
{
    uint8_t src[16], dst[16];
    /* ICMPv6 echo request fe80::1 -> fe80::2, id 0x1234, seq 1, "abcd". */
    uint8_t msg[12] = { 128, 0, 0, 0, 0x12, 0x34, 0x00, 0x01,
                        'a', 'b', 'c', 'd' };

    CHECK(ip6_text_parse("fe80::1", src) == 0, "parse fe80::1");
    CHECK(ip6_text_parse("fe80::2", dst) == 0, "parse fe80::2");
    uint16_t sum = net_transport_checksum6(src, dst, 58, msg, sizeof(msg));
    CHECK(ntohs(sum) == 0xABB8, "known ICMPv6 checksum");
    memcpy(msg + 2, &sum, sizeof(sum));
    CHECK(net_transport_checksum6(src, dst, 58, msg, sizeof(msg)) == 0,
          "completed ICMPv6 message verifies");
    CHECK(net_transport_checksum6(src, dst, 17, msg, sizeof(msg)) != 0,
          "next header is covered by the pseudo-header");
    dst[15] = 3;
    CHECK(net_transport_checksum6(src, dst, 58, msg, sizeof(msg)) != 0,
          "IPv6 destination is covered by the pseudo-header");
}

static int roundtrip(const char *in, const char *canonical)
{
    uint8_t a[16];
    char out[IP6_TEXT_MAX];
    if (ip6_text_parse(in, a) != 0)
        return 0;
    ip6_text_format(a, out);
    return strcmp(out, canonical) == 0;
}

static void test_ipv6_text(void)
{
    uint8_t a[16];

    CHECK(roundtrip("::", "::"), "unspecified");
    CHECK(roundtrip("::1", "::1"), "loopback");
    CHECK(roundtrip("fe80::", "fe80::"), "trailing gap");
    CHECK(roundtrip("FEC0:0:0:0:5054:FF:FE12:3456", "fec0::5054:ff:fe12:3456"),
          "SLAAC address compresses");
    CHECK(roundtrip("2001:db8:0:0:1:0:0:1", "2001:db8::1:0:0:1"),
          "first longest zero run wins");
    CHECK(roundtrip("2001:db8:0:1:1:1:1:1", "2001:db8:0:1:1:1:1:1"),
          "single zero word is not compressed");
    CHECK(roundtrip("[fec0::2]", "fec0::2"), "bracketed literal");
    CHECK(roundtrip("::ffff:10.0.2.2", "::ffff:a00:202"), "embedded IPv4");
    CHECK(ip6_text_parse("10.0.2.2", a) != 0, "IPv4 is not IPv6");
    CHECK(ip6_text_parse("example.com", a) != 0, "name is not IPv6");
    CHECK(ip6_text_parse("1::2::3", a) != 0, "two gaps rejected");
    CHECK(ip6_text_parse("1:2:3:4:5:6:7", a) != 0, "seven words rejected");
    CHECK(ip6_text_parse("1:2:3:4:5:6:7:8:9", a) != 0, "nine words rejected");
    CHECK(ip6_text_parse("12345::", a) != 0, "five hex digits rejected");
    CHECK(ip6_text_parse("fe80::1:", a) != 0, "trailing colon rejected");
    CHECK(ip6_text_parse("[fe80::1", a) != 0, "unclosed bracket rejected");
}

int main(void)
{
    test_ipv4_header();
    test_udp_pseudo_header();
    test_ipv6_pseudo_header();
    test_ipv6_text();
    if (failures) {
        fprintf(stderr, "%d/%d network checksum checks failed\n",
                failures, checks);
        return 1;
    }
    printf("PASS: %d network checksum checks\n", checks);
    return 0;
}
