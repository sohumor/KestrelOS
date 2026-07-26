/* libz: CRC-32 as used by gzip (RFC 1952) and PNG.
 *
 * The generator polynomial is x^32 + x^26 + x^23 + x^22 + x^16 + x^12 +
 * x^11 + x^10 + x^8 + x^7 + x^5 + x^4 + x^2 + x + 1. Because the data is
 * processed least significant bit first, the polynomial is used in its
 * reversed form 0xEDB88320.
 *
 * The 256-entry byte table is derived from that polynomial the first time
 * a CRC is taken rather than being written out here: entry n is the
 * remainder of the byte n shifted through eight divide steps. This costs
 * 2048 shift/xor pairs once per process.
 */

#include <stdint.h>
#include "inflate.h"

#define CRC32_POLY 0xEDB88320U

static uint32_t crc_table[256];
static int crc_table_ready;

static void crc_build_table(void)
{
    uint32_t n, c;
    int k;

    for (n = 0; n < 256; n++) {
        c = n;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? (CRC32_POLY ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
    crc_table_ready = 1;
}

uint32_t crc32_update(uint32_t crc, const void *buf, unsigned long len)
{
    const unsigned char *p = (const unsigned char *)buf;
    uint32_t c;

    if (!crc_table_ready)
        crc_build_table();
    if (!p)
        return crc;

    /* The stored value is the finished CRC, so it is complemented back
     * into the running form and complemented again on the way out. That
     * makes crc32_update() chainable across chunk boundaries. */
    c = crc ^ 0xFFFFFFFFU;
    while (len >= 8) {
        c = crc_table[(c ^ p[0]) & 0xFF] ^ (c >> 8);
        c = crc_table[(c ^ p[1]) & 0xFF] ^ (c >> 8);
        c = crc_table[(c ^ p[2]) & 0xFF] ^ (c >> 8);
        c = crc_table[(c ^ p[3]) & 0xFF] ^ (c >> 8);
        c = crc_table[(c ^ p[4]) & 0xFF] ^ (c >> 8);
        c = crc_table[(c ^ p[5]) & 0xFF] ^ (c >> 8);
        c = crc_table[(c ^ p[6]) & 0xFF] ^ (c >> 8);
        c = crc_table[(c ^ p[7]) & 0xFF] ^ (c >> 8);
        p += 8;
        len -= 8;
    }
    while (len--)
        c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFU;
}

uint32_t crc32_buf(const void *buf, unsigned long len)
{
    return crc32_update(0, buf, len);
}
