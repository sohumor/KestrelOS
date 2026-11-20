#include "kernel.h"
#include "string.h"
#include "net.h"
#include "udp.h"
#include "random.h"
#include "proc.h"

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

/* Expand a name to case-folded wire labels. Returns next offset or -1. */
static int read_name(const uint8_t *msg, int msglen, int off, uint8_t *name)
{
    int end = -1, used = 0;
    for (int steps = 0; steps < msglen && off < msglen; steps++) {
        uint8_t l = msg[off++];
        if ((l & 0xC0) == 0xC0) {
            if (off >= msglen)
                return -1;
            if (end < 0)
                end = off + 1;
            off = ((l & 0x3F) << 8) | msg[off];
            continue;
        }
        if (l > 63 || off + l > msglen || used + l + 1 > 255)
            return -1;
        name[used++] = l;
        if (!l)
            return end < 0 ? off : end;
        for (int i = 0; i < l; i++) {
            uint8_t c = msg[off++];
            name[used++] = c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
        }
    }
    return -1;
}

static int parse_response(const uint8_t *r, int n, const uint8_t *q,
                          int qlen, uint32_t *ip_be)
{
    if (n < 12 || r[0] != q[0] || r[1] != q[1] ||
        !(r[2] & 0x80) || (r[2] & 0x7A) || (r[3] & 0x0F) ||
        r[4] != 0 || r[5] != 1)
        return -1;

    uint8_t target[256] = {0}, owner[256] = {0}, next[256] = {0};
    if (read_name(q, qlen, 12, target) < 0)
        return -1;
    int p = read_name(r, n, 12, owner);
    if (p < 0 || p + 4 > n || memcmp(target, owner, 256) ||
        memcmp(r + p, q + qlen - 4, 4))
        return -1;
    int start = p + 4;
    int an = (r[6] << 8) | r[7];

    /* Rescan so a CNAME may precede or follow its target's records. */
    for (int hop = 0; hop <= an; hop++) {
        int alias = 0;
        p = start;
        for (int i = 0; i < an; i++) {
            memset(owner, 0, sizeof(owner));
            p = read_name(r, n, p, owner);
            if (p < 0 || p + 10 > n)
                return -1;
            int type = (r[p] << 8) | r[p + 1];
            int klass = (r[p + 2] << 8) | r[p + 3];
            int rdlen = (r[p + 8] << 8) | r[p + 9];
            p += 10;
            if (p + rdlen > n)
                return -1;
            if (klass == 1 && !memcmp(target, owner, 256)) {
                if (type == 1 && rdlen == 4) {
                    memcpy(ip_be, r + p, 4);
                    return 0;
                }
                if (type == 5) {
                    memset(next, 0, sizeof(next));
                    if (read_name(r, n, p, next) != p + rdlen)
                        return -1;
                    alias = 1;
                }
            }
            p += rdlen;
        }
        if (!alias)
            break;
        memcpy(target, next, sizeof(target));
    }
    return -1;
}

int dns_resolve(const char *name, uint32_t *ip_be)
{
    if (!name || !ip_be || !name[0])
        return -1;
    if (parse_dotted(name, ip_be) == 0)
        return 0;
    if (!net_ready())
        return -1;

    uint8_t q[DNS_MSG_MAX];
    memset(q, 0, 12);
    uint16_t id = (uint16_t)random_u32();
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

    uint16_t sport = 0;
    int bound = 0;
    for (int tries = 0; tries < 16; tries++) {
        uint16_t cand = (uint16_t)(0xC000 + (random_u32() & 0x0FFF));
        if (udp_bind(cand) == 0) {
            sport = cand;
            bound = 1;
            break;
        }
    }
    if (!bound)
        return -1;

    int found = 0;

    for (int attempt = 0; attempt < 2 && !found; attempt++) {
        if (current && current->kill_pending)
            break;
        if (udp_send(net_dns_addr(), sport, 53, q, qlen) < 0)
            continue;

        uint8_t r[DNS_MSG_MAX];
        int n = udp_recv(sport, r, sizeof(r), 1000);
        if (current && current->kill_pending)
            break;
        found = parse_response(r, n, q, qlen, ip_be) == 0;
    }

    udp_unbind(sport);
    return found ? 0 : -1;
}
