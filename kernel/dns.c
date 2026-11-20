#include "kernel.h"
#include "string.h"
#include "net.h"
#include "udp.h"
#include "random.h"
#include "proc.h"
#include "timer.h"
#include "spinlock.h"
#include "flight.h"
#include "kheap.h"

/* Minimal DNS A-record resolver over UDP port 53 with 16-entry TTL cache. */

#define DNS_MSG_MAX    512
#define DNS_CACHE_SIZE 16
#define DNS_NAME_MAX   256
#define DNS_TTL_MIN_S  5U
#define DNS_TTL_MAX_S  3600U
#define DNS_TTL_ZERO_S 60U

struct dns_cache_entry {
    char     name[DNS_NAME_MAX];
    uint32_t ip_be;
    uint64_t expire_tick;
    uint64_t last_used;
    uint64_t hits;
    uint32_t ttl_sec;
    bool     valid;
};

static struct dns_cache_entry dns_cache[DNS_CACHE_SIZE];
static uint64_t dns_lru_seq;
static spinlock_t dns_lock = SPINLOCK_INIT;

static char ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
}

static int dns_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = ascii_lower(*a++);
        char cb = ascii_lower(*b++);
        if (ca != cb)
            return (unsigned char)ca - (unsigned char)cb;
    }
    return (unsigned char)ascii_lower(*a) - (unsigned char)ascii_lower(*b);
}

static uint32_t clamp_dns_ttl_sec(uint32_t raw_ttl)
{
    if (raw_ttl == 0)
        return DNS_TTL_ZERO_S;
    if (raw_ttl < DNS_TTL_MIN_S)
        return DNS_TTL_MIN_S;
    if (raw_ttl > DNS_TTL_MAX_S)
        return DNS_TTL_MAX_S;
    return raw_ttl;
}

static bool dns_cache_lookup(const char *name, uint32_t *ip_be,
                             uint32_t *ttl_sec_out)
{
    uint64_t now = timer_ticks();
    uint64_t f = spin_lock_irqsave(&dns_lock);

    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        struct dns_cache_entry *e = &dns_cache[i];
        if (!e->valid)
            continue;
        if (now >= e->expire_tick) {
            e->valid = false;
            continue;
        }
        if (dns_strcasecmp(e->name, name) == 0) {
            *ip_be = e->ip_be;
            if (ttl_sec_out)
                *ttl_sec_out = e->ttl_sec;
            e->hits++;
            e->last_used = ++dns_lru_seq;
            spin_unlock_irqrestore(&dns_lock, f);
            return true;
        }
    }
    spin_unlock_irqrestore(&dns_lock, f);
    return false;
}

static void dns_cache_store(const char *name, uint32_t ip_be, uint32_t ttl_sec)
{
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen >= DNS_NAME_MAX)
        return;

    uint64_t now = timer_ticks();
    uint64_t ttl_ticks = (uint64_t)ttl_sec * TIMER_HZ;
    uint64_t f = spin_lock_irqsave(&dns_lock);

    struct dns_cache_entry *slot = NULL;
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        struct dns_cache_entry *e = &dns_cache[i];
        if (e->valid && dns_strcasecmp(e->name, name) == 0) {
            slot = e;
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < DNS_CACHE_SIZE; i++) {
            struct dns_cache_entry *e = &dns_cache[i];
            if (!e->valid || now >= e->expire_tick) {
                e->valid = false;
                slot = e;
                break;
            }
        }
    }
    if (!slot) {
        int lru = 0;
        for (int i = 1; i < DNS_CACHE_SIZE; i++) {
            if (dns_cache[i].last_used < dns_cache[lru].last_used)
                lru = i;
        }
        slot = &dns_cache[lru];
    }

    slot->valid = false;
    for (size_t i = 0; i < nlen; i++)
        slot->name[i] = ascii_lower(name[i]);
    slot->name[nlen] = '\0';
    slot->ip_be = ip_be;
    slot->ttl_sec = ttl_sec;
    slot->expire_tick = now + ttl_ticks;
    slot->hits = 0;
    slot->last_used = ++dns_lru_seq;
    slot->valid = true;

    spin_unlock_irqrestore(&dns_lock, f);
}

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
    if (!name || !ip_be || !name[0])
        return -1;
    if (parse_dotted(name, ip_be) == 0)
        return 0;
    if (!net_ready())
        return -1;

    uint32_t cached_ttl = 0;
    if (dns_cache_lookup(name, ip_be, &cached_ttl)) {
        net_stat_inc(NET_STAT_DNS_HIT, 0);
        flight_record(FLIGHT_CAT_NET, "dns_hit", -1,
                      (uint64_t)ntohl(*ip_be), (uint64_t)cached_ttl, name);
        return 0;
    }

    net_stat_inc(NET_STAT_DNS_MISS, 0);

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
        if (l <= 0 || l > 63 || off + l + 1 > DNS_MSG_MAX - 6) {
            flight_record(FLIGHT_CAT_NET, "dns_miss", -1, 0, 0, name);
            return -1;
        }
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
    if (!bound) {
        flight_record(FLIGHT_CAT_NET, "dns_miss", -1, 0, 0, name);
        return -1;
    }

    int found = 0;
    uint32_t resolved_ip = 0;
    uint32_t raw_ttl = 0;

    for (int attempt = 0; attempt < 2 && !found; attempt++) {
        if (current && current->kill_pending)
            break;
        if (udp_send(net_dns_addr(), sport, 53, q, qlen) < 0)
            continue;

        uint8_t r[DNS_MSG_MAX];
        int n = udp_recv(sport, r, sizeof(r), 1000);
        if (current && current->kill_pending)
            break;
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
            uint32_t rr_ttl = ((uint32_t)r[p + 4] << 24) |
                              ((uint32_t)r[p + 5] << 16) |
                              ((uint32_t)r[p + 6] << 8)  |
                               (uint32_t)r[p + 7];
            int rdlen = (r[p + 8] << 8) | r[p + 9];
            p += 10;
            if (p + rdlen > n)
                break;
            if (type == 1 && rdlen == 4) {
                memcpy(&resolved_ip, r + p, 4);
                raw_ttl = rr_ttl;
                found = 1;
                break;
            }
            p += rdlen;                  /* CNAME etc: skip */
        }
    }

    udp_unbind(sport);
    if (found) {
        uint32_t ttl_sec = clamp_dns_ttl_sec(raw_ttl);
        *ip_be = resolved_ip;
        dns_cache_store(name, resolved_ip, ttl_sec);
        flight_record(FLIGHT_CAT_NET, "dns_miss", -1,
                      (uint64_t)ntohl(resolved_ip), (uint64_t)ttl_sec, name);
        return 0;
    }
    flight_record(FLIGHT_CAT_NET, "dns_miss", -1, 0, 0, name);
    return -1;
}

struct dns_snap_row {
    char     name[DNS_NAME_MAX];
    uint32_t ip_be;
    int64_t  ttl_s;
    int64_t  hits;
};

uint32_t dns_snapshot_krec(char *buf, uint32_t cap)
{
    struct krec_kbuf b;

    if (cap == 0)
        return 0;

    krec_kbuf_init(&b, buf, cap);
    krec_kbuf_puts(&b, "@krec name:str ip:ip ttl_s:int hits:int\n");
    if (b.overflow) {
        b.len = 0;
        return krec_kbuf_finish(&b);
    }

    struct dns_snap_row *snap = kmalloc(DNS_CACHE_SIZE * sizeof(*snap));
    if (!snap)
        return krec_kbuf_finish(&b);

    int n = 0;
    uint64_t now = timer_ticks();
    uint64_t f = spin_lock_irqsave(&dns_lock);
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        struct dns_cache_entry *e = &dns_cache[i];
        if (!e->valid)
            continue;
        if (now >= e->expire_tick) {
            e->valid = false;
            continue;
        }
        uint64_t rem_ticks = e->expire_tick - now;
        memcpy(snap[n].name, e->name, DNS_NAME_MAX);
        snap[n].name[DNS_NAME_MAX - 1] = '\0';
        snap[n].ip_be = e->ip_be;
        snap[n].ttl_s = (int64_t)((rem_ticks + TIMER_HZ - 1) / TIMER_HZ);
        snap[n].hits = (int64_t)e->hits;
        n++;
    }
    spin_unlock_irqrestore(&dns_lock, f);

    for (int i = 0; i < n; i++) {
        uint32_t mark = b.len;
        krec_kbuf_escape(&b, snap[i].name);
        krec_kbuf_putc(&b, '\t');
        krec_kbuf_put_ip4(&b, snap[i].ip_be);
        krec_kbuf_putc(&b, '\t');
        krec_kbuf_put_i64(&b, snap[i].ttl_s);
        krec_kbuf_putc(&b, '\t');
        krec_kbuf_put_i64(&b, snap[i].hits);
        krec_kbuf_putc(&b, '\n');
        if (b.overflow) {
            b.len = mark;
            break;
        }
    }

    kfree(snap);
    return krec_kbuf_finish(&b);
}
