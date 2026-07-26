#include "kernel.h"
#include "string.h"
#include "net.h"

/* Minimal DNS A-record resolver over UDP port 53. */

#define DNS_MSG_MAX 512

/* Parse "a.b.c.d" into a network-order u32. Returns 0 / -1. */
static int parse_dotted(const char *name, uint32_t *ip_be)
{
    uint8_t parts[4];
    int p = 0;
    uint32_t v = 0;
    bool have_digit = false;

    for (const char *s = name;; s++) {
        if (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s - '0');
            if (v > 255)
                return -1;
            have_digit = true;
        } else if (*s == '.' || *s == 0) {
            if (!have_digit || p >= 4)
                return -1;
            parts[p++] = (uint8_t)v;
            v = 0;
            have_digit = false;
            if (*s == 0)
                break;
        } else {
            return -1;
        }
    }
    if (p != 4)
        return -1;
    memcpy(ip_be, parts, 4);
    return 0;
}

/* Skip a possibly-compressed DNS name. Returns next offset or -1. */
static int skip_name(const uint8_t *msg, int msglen, int off)
{
    while (off < msglen) {
        uint8_t l = msg[off];
        if (l == 0)
            return off + 1;
        if ((l & 0xC0) == 0xC0)          /* compression pointer ends name */
            return off + 2 <= msglen ? off + 2 : -1;
        if (l > 63)
            return -1;
        off += 1 + l;
    }
    return -1;
}

int dns_resolve(const char *name, uint32_t *ip_be)
{
    static uint16_t counter;

    if (!name || !ip_be || !name[0])
        return -1;
    if (parse_dotted(name, ip_be) == 0)
        return 0;
    if (!net_ready())
        return -1;

    uint8_t q[DNS_MSG_MAX];
    memset(q, 0, 12);
    counter++;
    uint16_t id = (uint16_t)(0x4B00 ^ counter);
    q[0] = (uint8_t)(id >> 8);
    q[1] = (uint8_t)id;
    q[2] = 0x01;                         /* RD */
    q[5] = 1;                            /* QDCOUNT = 1 */

    /* Encode the name as length-prefixed labels. */
    int off = 12;
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        int l = dot ? (int)(dot - s) : (int)strlen(s);
        if (l <= 0 || l > 63 || off + l + 1 > DNS_MSG_MAX - 6)
            return -1;
        q[off++] = (uint8_t)l;
        memcpy(q + off, s, l);
        off += l;
        s += l;
        if (*s == '.')
            s++;
    }
    q[off++] = 0;
    q[off++] = 0; q[off++] = 1;          /* QTYPE = A */
    q[off++] = 0; q[off++] = 1;          /* QCLASS = IN */
    int qlen = off;

    uint16_t sport = (uint16_t)(0xC000 + (counter & 0x0FFF));
    int found = 0;

    for (int attempt = 0; attempt < 2 && !found; attempt++) {
        if (udp_send(net_dns_addr(), sport, 53, q, qlen) < 0)
            continue;

        uint8_t r[DNS_MSG_MAX];
        int n = udp_recv(sport, r, sizeof(r), 1000);
        if (n < 12)
            continue;
        if (r[0] != q[0] || r[1] != q[1])
            continue;                    /* wrong transaction id */
        if (!(r[2] & 0x80))
            continue;                    /* not a response */
        if (r[3] & 0x0F)
            continue;                    /* rcode != 0 */

        int qd = (r[4] << 8) | r[5];
        int an = (r[6] << 8) | r[7];
        int p = 12;

        for (int i = 0; i < qd && p >= 0; i++) {
            p = skip_name(r, n, p);
            if (p < 0 || p + 4 > n)
                p = -1;
            else
                p += 4;                  /* qtype + qclass */
        }
        if (p < 0)
            continue;

        for (int i = 0; i < an; i++) {
            p = skip_name(r, n, p);
            if (p < 0 || p + 10 > n)
                break;
            int type = (r[p] << 8) | r[p + 1];
            int rdlen = (r[p + 8] << 8) | r[p + 9];
            p += 10;
            if (p + rdlen > n)
                break;
            if (type == 1 && rdlen == 4) {
                memcpy(ip_be, r + p, 4);
                found = 1;
                break;
            }
            p += rdlen;                  /* CNAME etc: skip */
        }
    }

    udp_unbind(sport);
    return found ? 0 : -1;
}
