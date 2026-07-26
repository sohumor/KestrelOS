/* libz: Adler-32 as used by the zlib wrapper (RFC 1950).
 *
 * Two running sums modulo 65521, the largest prime below 2^16: s1 is the
 * sum of the bytes plus one, s2 the sum of the successive values of s1.
 * The checksum is s2 in the high half and s1 in the low half.
 *
 * The modulo is deferred: with both sums held in 32 bits, 5552 bytes is
 * the most that can be absorbed before s2 could overflow, since
 * 255*n*(n+1)/2 + (n+1)*65520 must stay below 2^32.
 */

#include <stdint.h>
#include "inflate.h"

#define ADLER_BASE 65521U
#define ADLER_NMAX 5552U

uint32_t adler32_update(uint32_t adler, const void *buf, unsigned long len)
{
    const unsigned char *p = (const unsigned char *)buf;
    uint32_t s1 = adler & 0xFFFFU;
    uint32_t s2 = (adler >> 16) & 0xFFFFU;
    unsigned long k;

    if (!p)
        return 1;

    while (len) {
        k = (len < ADLER_NMAX) ? len : ADLER_NMAX;
        len -= k;
        while (k >= 16) {
            int i;
            for (i = 0; i < 16; i++) {
                s1 += p[i];
                s2 += s1;
            }
            p += 16;
            k -= 16;
        }
        while (k--) {
            s1 += *p++;
            s2 += s1;
        }
        s1 %= ADLER_BASE;
        s2 %= ADLER_BASE;
    }
    return (s2 << 16) | s1;
}

uint32_t adler32_buf(const void *buf, unsigned long len)
{
    return adler32_update(1, buf, len);
}
