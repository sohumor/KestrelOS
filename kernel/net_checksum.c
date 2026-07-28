#include "net.h"

static uint32_t add_bytes(uint32_t sum, const uint8_t *p, int len)
{
    while (len > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)(p[0] << 8);
    return sum;
}

static uint16_t finish_sum(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

uint16_t net_checksum(const void *data, int len)
{
    return finish_sum(add_bytes(0, (const uint8_t *)data, len));
}

uint16_t net_transport_checksum(uint32_t src_be, uint32_t dst_be,
                                uint8_t protocol,
                                const void *segment, int len)
{
    const uint8_t *src = (const uint8_t *)&src_be;
    const uint8_t *dst = (const uint8_t *)&dst_be;
    uint32_t sum = 0;

    /* IPv4 pseudo-header: source, destination, zero, protocol and the
     * 16-bit transport length, followed by the complete segment. */
    sum = add_bytes(sum, src, 4);
    sum = add_bytes(sum, dst, 4);
    sum += protocol;
    sum += (uint16_t)len;
    sum = add_bytes(sum, (const uint8_t *)segment, len);
    return finish_sum(sum);
}
