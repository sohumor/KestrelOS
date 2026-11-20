/* IPv6 address text conversion (RFC 4291 section 2.2 input, RFC 5952
 * output), shared by the kernel resolver, libc and host tests. Header-only
 * and dependency-free so it builds freestanding. */
#ifndef KESTREL_IP6_TEXT_H
#define KESTREL_IP6_TEXT_H

#include <stdint.h>

#define IP6_TEXT_MAX 46

static inline int ip6_text_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse "2001:db8::1", "::", "::ffff:1.2.3.4" (optionally in [brackets])
 * into 16 bytes. Returns 0 on success, -1 if s is not an IPv6 literal. */
static inline int ip6_text_parse(const char *s, uint8_t out[16])
{
    uint16_t words[8];
    int n = 0, gap = -1, i, bracket = 0;

    if (!s)
        return -1;
    if (*s == '[') {
        bracket = 1;
        s++;
    }
    if (s[0] == ':') {
        if (s[1] != ':')
            return -1;
        gap = 0;
        s += 2;
    }
    while (*s && !(bracket && *s == ']')) {
        int digits = 0, dots = 0;
        uint32_t v = 0;
        const char *t;

        if (n >= 8)
            return -1;
        for (t = s; ip6_text_hex(*t) >= 0 || *t == '.'; t++)
            if (*t == '.')
                dots++;
        if (dots) {
            /* Embedded dotted quad: must be the last 32 bits. */
            uint8_t q[4];
            int k;
            if (dots != 3 || n > 6)
                return -1;
            for (k = 0; k < 4; k++) {
                int d = 0;
                uint32_t b = 0;
                while (*s >= '0' && *s <= '9' && d < 4) {
                    b = b * 10 + (uint32_t)(*s++ - '0');
                    d++;
                }
                if (d == 0 || d > 3 || b > 255)
                    return -1;
                q[k] = (uint8_t)b;
                if (k < 3 && *s++ != '.')
                    return -1;
            }
            words[n++] = (uint16_t)(q[0] << 8 | q[1]);
            words[n++] = (uint16_t)(q[2] << 8 | q[3]);
            break;
        }
        while (ip6_text_hex(*s) >= 0) {
            if (++digits > 4)
                return -1;
            v = (v << 4) | (uint32_t)ip6_text_hex(*s++);
        }
        if (digits == 0)
            return -1;
        words[n++] = (uint16_t)v;
        if (*s == ':') {
            s++;
            if (*s == ':') {
                if (gap >= 0)
                    return -1;
                gap = n;
                s++;
            } else if (*s == '\0' || *s == ']') {
                return -1;
            }
        }
    }
    if (bracket) {
        if (*s != ']' || s[1] != '\0')
            return -1;
    } else if (*s != '\0') {
        return -1;
    }
    if (gap < 0 ? n != 8 : n > 7)
        return -1;
    for (i = 0; i < 16; i++)
        out[i] = 0;
    if (gap < 0)
        gap = n;
    for (i = 0; i < gap; i++) {
        out[2 * i] = (uint8_t)(words[i] >> 8);
        out[2 * i + 1] = (uint8_t)words[i];
    }
    for (i = gap; i < n; i++) {
        int w = 8 - (n - i);
        out[2 * w] = (uint8_t)(words[i] >> 8);
        out[2 * w + 1] = (uint8_t)words[i];
    }
    return 0;
}

/* Format 16 bytes in RFC 5952 canonical form; buf needs IP6_TEXT_MAX. */
static inline char *ip6_text_format(const uint8_t a[16], char *buf)
{
    static const char hex[] = "0123456789abcdef";
    int best = -1, best_len = 0, i, o = 0;

    for (i = 0; i < 8;) {
        int j = i;
        while (j < 8 && a[2 * j] == 0 && a[2 * j + 1] == 0)
            j++;
        if (j - i > best_len) {
            best = i;
            best_len = j - i;
        }
        i = j > i ? j : i + 1;
    }
    if (best_len < 2)
        best = -1;
    for (i = 0; i < 8; i++) {
        unsigned w;
        int shift, started = 0;
        if (i == best) {
            buf[o++] = ':';
            buf[o++] = ':';
            i += best_len - 1;
            continue;
        }
        if (i && i != best + best_len)
            buf[o++] = ':';
        w = (unsigned)(a[2 * i] << 8 | a[2 * i + 1]);
        for (shift = 12; shift >= 0; shift -= 4) {
            unsigned d = (w >> shift) & 0xF;
            if (d || started || shift == 0) {
                buf[o++] = hex[d];
                started = 1;
            }
        }
    }
    buf[o] = '\0';
    return buf;
}

#endif
