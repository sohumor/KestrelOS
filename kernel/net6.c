/* IPv6 (RFC 8200), ICMPv6 (RFC 4443), Neighbor Discovery (RFC 4861) and
 * stateless address autoconfiguration (RFC 4862) with RDNSS (RFC 8106).
 *
 * Same model as the IPv4 core in net.c: frames arrive from the NIC in IRQ
 * context, where we only parse, learn neighbors, update configuration and
 * answer synchronously (NS, echo).  Anything that may wait (neighbor
 * resolution, ping, router solicitation) runs in task context and spins on
 * net_wait_tick().
 *
 * One interface, one link-local address (fe80::/64 + modified EUI-64) and at
 * most one SLAAC global address from the first autonomous /64 prefix an RA
 * advertises.  Extension headers other than hop-by-hop, routing and
 * destination options are dropped, as are fragments. */

#include "kernel.h"
#include "string.h"
#include "timer.h"
#include "proc.h"
#include "netdev.h"
#include "net.h"
#include "tcp.h"
#include "spinlock.h"
#include "ip6_text.h"

#define ETHERTYPE_IPV6 0x86DD

#define NH_HOPOPTS  0
#define NH_TCP      6
#define NH_UDP     17
#define NH_ROUTING 43
#define NH_FRAG    44
#define NH_ICMPV6  58
#define NH_DSTOPTS 60

#define ICMP6_ECHO_REQ   128
#define ICMP6_ECHO_REPLY 129
#define ND_RS 133
#define ND_RA 134
#define ND_NS 135
#define ND_NA 136

#define ND_OPT_SLLA   1
#define ND_OPT_TLLA   2
#define ND_OPT_PREFIX 3
#define ND_OPT_RDNSS 25

struct ip6_hdr {
    uint32_t vtc_flow;
    uint16_t plen;
    uint8_t  next;
    uint8_t  hlim;
    uint8_t  src[16];
    uint8_t  dst[16];
} __attribute__((packed));

static const uint8_t all_nodes[16] = { 0xff, 0x02, [15] = 1 };
static const uint8_t all_routers[16] = { 0xff, 0x02, [15] = 2 };
static const uint8_t unspec[16];

static spinlock_t cfg6_lock = SPINLOCK_INIT;
static bool ll_ok, gl_ok, rtr_ok, dns_ok;
static uint8_t ll_addr[16], gl_addr[16], rtr_addr[16], dns6[16];
static uint8_t gl_plen;
static volatile bool ra_seen;

/* ---- helpers ---- */

static bool ip6_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 16) == 0;
}

static bool ip6_is_multicast(const uint8_t *a) { return a[0] == 0xff; }
static bool ip6_is_linklocal(const uint8_t *a)
{
    return a[0] == 0xfe && (a[1] & 0xc0) == 0x80;
}

static void solicited_node(const uint8_t *a, uint8_t out[16])
{
    static const uint8_t pfx[13] = { 0xff, 0x02, 0, 0, 0, 0, 0, 0,
                                     0, 0, 0, 1, 0xff };
    memcpy(out, pfx, 13);
    memcpy(out + 13, a + 13, 3);
}

static void multicast_mac(const uint8_t *a, uint8_t mac[6])
{
    mac[0] = 0x33;
    mac[1] = 0x33;
    memcpy(mac + 2, a + 12, 4);
}

static void eui64(uint8_t out[16], const uint8_t prefix[8])
{
    const uint8_t *m = netdev_current()->mac;
    memcpy(out, prefix, 8);
    out[8] = m[0] ^ 0x02;
    out[9] = m[1];
    out[10] = m[2];
    out[11] = 0xff;
    out[12] = 0xfe;
    out[13] = m[3];
    out[14] = m[4];
    out[15] = m[5];
}

static bool prefix_match(const uint8_t *a, const uint8_t *b, int bits)
{
    int i;
    for (i = 0; i < bits / 8; i++)
        if (a[i] != b[i])
            return false;
    if (bits % 8) {
        uint8_t m = (uint8_t)(0xff << (8 - bits % 8));
        if ((a[i] & m) != (b[i] & m))
            return false;
    }
    return true;
}

static bool is_mine(const uint8_t *a)
{
    return (ll_ok && ip6_eq(a, ll_addr)) || (gl_ok && ip6_eq(a, gl_addr));
}

static bool accept_dst(const uint8_t *d)
{
    uint8_t sn[16];
    if (is_mine(d) || ip6_eq(d, all_nodes))
        return true;
    if (ll_ok) {
        solicited_node(ll_addr, sn);
        if (ip6_eq(d, sn))
            return true;
    }
    if (gl_ok) {
        solicited_node(gl_addr, sn);
        if (ip6_eq(d, sn))
            return true;
    }
    return false;
}

bool net6_source_for(const uint8_t dst[16], uint8_t src_out[16])
{
    bool ok = false;
    uint64_t f = spin_lock_irqsave(&cfg6_lock);
    if (gl_ok && !ip6_is_linklocal(dst) &&
        !(ip6_is_multicast(dst) && (dst[1] & 0x0f) <= 2)) {
        memcpy(src_out, gl_addr, 16);
        ok = true;
    } else if (ll_ok) {
        memcpy(src_out, ll_addr, 16);
        ok = true;
    }
    spin_unlock_irqrestore(&cfg6_lock, f);
    return ok;
}

/* ---- neighbor cache ---- */

#define ND_CACHE_SIZE 16

struct nd_entry {
    uint8_t ip[16];
    uint8_t mac[6];
    bool valid;
};

static struct nd_entry nd_cache[ND_CACHE_SIZE];
static int nd_next;
static spinlock_t nd_lock = SPINLOCK_INIT;

static void nd_learn(const uint8_t *ip, const uint8_t *mac)
{
    if (ip6_is_multicast(ip) || ip6_eq(ip, unspec) || (mac[0] & 1))
        return;
    uint64_t f = spin_lock_irqsave(&nd_lock);
    for (int i = 0; i < ND_CACHE_SIZE; i++) {
        if (nd_cache[i].valid && ip6_eq(nd_cache[i].ip, ip)) {
            memcpy(nd_cache[i].mac, mac, 6);
            spin_unlock_irqrestore(&nd_lock, f);
            return;
        }
    }
    struct nd_entry *e = &nd_cache[nd_next];
    nd_next = (nd_next + 1) % ND_CACHE_SIZE;
    memcpy(e->ip, ip, 16);
    memcpy(e->mac, mac, 6);
    e->valid = true;
    spin_unlock_irqrestore(&nd_lock, f);
}

static bool nd_lookup(const uint8_t *ip, uint8_t *mac_out)
{
    bool found = false;
    uint64_t f = spin_lock_irqsave(&nd_lock);
    for (int i = 0; i < ND_CACHE_SIZE; i++) {
        if (nd_cache[i].valid && ip6_eq(nd_cache[i].ip, ip)) {
            memcpy(mac_out, nd_cache[i].mac, 6);
            found = true;
            break;
        }
    }
    spin_unlock_irqrestore(&nd_lock, f);
    return found;
}

/* ---- transmit ---- */

static int ip6_send_mac(const uint8_t *dst_mac, const uint8_t *src,
                        const uint8_t *dst, uint8_t next, uint8_t hlim,
                        const void *payload, int len)
{
    uint8_t pkt[ETH_MTU];
    struct ip6_hdr *ip = (struct ip6_hdr *)pkt;

    if (len < 0 || len > ETH_MTU - (int)sizeof(*ip))
        return -1;
    ip->vtc_flow = htonl(6u << 28);
    ip->plen = htons((uint16_t)len);
    ip->next = next;
    ip->hlim = hlim;
    memcpy(ip->src, src, 16);
    memcpy(ip->dst, dst, 16);
    memcpy(pkt + sizeof(*ip), payload, len);
    return net_eth_send(dst_mac, ETHERTYPE_IPV6, pkt, (int)sizeof(*ip) + len);
}

/* Fills in the ICMPv6 checksum (bytes 2..3 of msg) before sending. */
static int icmp6_send_mac(const uint8_t *dst_mac, const uint8_t *src,
                          const uint8_t *dst, uint8_t hlim,
                          uint8_t *msg, int len)
{
    msg[2] = msg[3] = 0;
    uint16_t c = net_transport_checksum6(src, dst, NH_ICMPV6, msg, len);
    memcpy(msg + 2, &c, 2);
    return ip6_send_mac(dst_mac, src, dst, NH_ICMPV6, hlim, msg, len);
}

static void put_lla_opt(uint8_t *p, uint8_t type)
{
    p[0] = type;
    p[1] = 1;
    memcpy(p + 2, netdev_current()->mac, 6);
}

static void nd_send_ns(const uint8_t *src, const uint8_t *target)
{
    uint8_t msg[32], dst[16], mac[6];
    memset(msg, 0, sizeof(msg));
    msg[0] = ND_NS;
    memcpy(msg + 8, target, 16);
    put_lla_opt(msg + 24, ND_OPT_SLLA);
    solicited_node(target, dst);
    multicast_mac(dst, mac);
    icmp6_send_mac(mac, src, dst, 255, msg, sizeof(msg));
}

static void nd_send_rs(void)
{
    uint8_t msg[16], mac[6];
    memset(msg, 0, sizeof(msg));
    msg[0] = ND_RS;
    put_lla_opt(msg + 8, ND_OPT_SLLA);
    multicast_mac(all_routers, mac);
    icmp6_send_mac(mac, ll_addr, all_routers, 255, msg, sizeof(msg));
}

static int nd_resolve(const uint8_t *src, const uint8_t *ip, uint8_t *mac)
{
    if (nd_lookup(ip, mac))
        return 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        nd_send_ns(src, ip);
        uint64_t deadline = timer_ticks() + TIMER_HZ;
        while (timer_ticks() < deadline) {
            net_wait_tick();
            if (nd_lookup(ip, mac))
                return 0;
        }
    }
    return -1;
}

int net6_send(const uint8_t dst[16], uint8_t next,
              const void *payload, int len)
{
    uint8_t src[16], hop[16], mac[6];

    if (!net_ready() || !net6_source_for(dst, src))
        return -1;
    if (ip6_is_multicast(dst)) {
        multicast_mac(dst, mac);
        return ip6_send_mac(mac, src, dst, next, 64, payload, len);
    }

    uint64_t f = spin_lock_irqsave(&cfg6_lock);
    bool onlink = ip6_is_linklocal(dst) ||
                  (gl_ok && prefix_match(dst, gl_addr, gl_plen));
    bool have_rtr = rtr_ok;
    memcpy(hop, onlink ? dst : rtr_addr, 16);
    spin_unlock_irqrestore(&cfg6_lock, f);

    if (!onlink && !have_rtr)
        return -1;
    if (nd_resolve(src, hop, mac) < 0) {
        char t[IP6_TEXT_MAX];
        kprintf("net6: neighbor discovery failed for %s\n",
                ip6_text_format(hop, t));
        return -1;
    }
    return ip6_send_mac(mac, src, dst, next, 64, payload, len);
}

/* ---- ICMPv6 echo ---- */

static volatile bool ping6_busy, ping6_waiting, ping6_got;
static volatile uint16_t ping6_id, ping6_seq;
static uint8_t ping6_target[16];
static volatile uint64_t ping6_reply_ticks;
static spinlock_t ping6_lock = SPINLOCK_INIT;

long icmp6_ping(const uint8_t dst[16], int timeout_ms)
{
    static uint16_t seq_counter;
    uint8_t msg[8 + 32];
    uint8_t src[16];

    if (!net_ready() || !net6_source_for(dst, src))
        return -1;
    if (timeout_ms <= 0)
        timeout_ms = 1000;

    uint64_t f = spin_lock_irqsave(&ping6_lock);
    if (ping6_busy) {
        spin_unlock_irqrestore(&ping6_lock, f);
        return -1;
    }
    ping6_busy = true;
    ping6_id = 0x6b36;
    ping6_seq = ++seq_counter;
    memcpy(ping6_target, dst, 16);
    ping6_got = false;
    ping6_waiting = true;
    spin_unlock_irqrestore(&ping6_lock, f);

    msg[0] = ICMP6_ECHO_REQ;
    msg[1] = 0;
    msg[4] = (uint8_t)(ping6_id >> 8);
    msg[5] = (uint8_t)ping6_id;
    msg[6] = (uint8_t)(ping6_seq >> 8);
    msg[7] = (uint8_t)ping6_seq;
    for (int i = 0; i < 32; i++)
        msg[8 + i] = (uint8_t)('a' + (i & 15));
    msg[2] = msg[3] = 0;
    uint16_t c = net_transport_checksum6(src, dst, NH_ICMPV6, msg, sizeof(msg));
    memcpy(msg + 2, &c, 2);

    uint64_t start = timer_ticks();
    long rtt = -1;
    if (net6_send(dst, NH_ICMPV6, msg, sizeof(msg)) >= 0) {
        uint64_t deadline = start + (uint64_t)timeout_ms * TIMER_HZ / 1000 + 1;
        while (timer_ticks() < deadline) {
            net_wait_tick();
            if (current && current->kill_pending)
                break;
            if (ping6_got) {
                rtt = (long)((ping6_reply_ticks - start) * 1000 / TIMER_HZ);
                break;
            }
        }
    }
    f = spin_lock_irqsave(&ping6_lock);
    ping6_waiting = false;
    ping6_busy = false;
    spin_unlock_irqrestore(&ping6_lock, f);
    return rtt;
}

/* ---- ICMPv6 input (IRQ context) ---- */

static void nd_input_ns(const uint8_t *eth_src, const struct ip6_hdr *ip,
                        const uint8_t *m, int len)
{
    const uint8_t *target = m + 8;
    const uint8_t *slla = 0;
    uint8_t na[32], dst[16], mac[6];

    if (len < 24 || ip6_is_multicast(target))
        return;
    for (int o = 24; o + 8 <= len && m[o + 1]; o += m[o + 1] * 8)
        if (m[o] == ND_OPT_SLLA && o + m[o + 1] * 8 <= len)
            slla = m + o + 2;
    if (!is_mine(target))
        return;
    if (slla && !ip6_eq(ip->src, unspec))
        nd_learn(ip->src, slla);

    memset(na, 0, sizeof(na));
    na[0] = ND_NA;
    if (ip6_eq(ip->src, unspec)) {
        na[4] = 0x20;                       /* override, to all-nodes */
        memcpy(dst, all_nodes, 16);
        multicast_mac(dst, mac);
    } else {
        na[4] = 0x60;                       /* solicited | override */
        memcpy(dst, ip->src, 16);
        memcpy(mac, slla ? slla : eth_src, 6);
    }
    memcpy(na + 8, target, 16);
    put_lla_opt(na + 24, ND_OPT_TLLA);
    icmp6_send_mac(mac, target, dst, 255, na, sizeof(na));
}

static void nd_input_na(const uint8_t *eth_src, const uint8_t *m, int len)
{
    const uint8_t *tlla = eth_src;
    if (len < 24 || ip6_is_multicast(m + 8))
        return;
    for (int o = 24; o + 8 <= len && m[o + 1]; o += m[o + 1] * 8)
        if (m[o] == ND_OPT_TLLA && o + m[o + 1] * 8 <= len)
            tlla = m + o + 2;
    nd_learn(m + 8, tlla);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

static void nd_input_ra(const uint8_t *eth_src, const struct ip6_hdr *ip,
                        const uint8_t *m, int len)
{
    if (len < 16 || !ip6_is_linklocal(ip->src))
        return;
    uint16_t lifetime = (uint16_t)(m[6] << 8 | m[7]);
    const uint8_t *slla = eth_src;

    uint64_t f = spin_lock_irqsave(&cfg6_lock);
    for (int o = 16; o + 8 <= len && m[o + 1]; o += m[o + 1] * 8) {
        const uint8_t *opt = m + o;
        int olen = opt[1] * 8;
        if (o + olen > len)
            break;
        if (opt[0] == ND_OPT_SLLA) {
            slla = opt + 2;
        } else if (opt[0] == ND_OPT_PREFIX && olen == 32) {
            uint8_t plen = opt[2], flags = opt[3];
            const uint8_t *pfx = opt + 16;
            /* Autonomous /64 with a nonzero valid lifetime: form an
             * address unless we already hold one. */
            if ((flags & 0x40) && plen == 64 && be32(opt + 4) != 0 &&
                !ip6_is_linklocal(pfx) && !ip6_is_multicast(pfx) && !gl_ok) {
                eui64(gl_addr, pfx);
                gl_plen = plen;
                gl_ok = true;
            }
        } else if (opt[0] == ND_OPT_RDNSS && olen >= 24 &&
                   be32(opt + 4) != 0) {
            memcpy(dns6, opt + 8, 16);
            dns_ok = true;
        }
    }
    if (lifetime) {
        memcpy(rtr_addr, ip->src, 16);
        rtr_ok = true;
    } else if (rtr_ok && ip6_eq(rtr_addr, ip->src)) {
        rtr_ok = false;
    }
    spin_unlock_irqrestore(&cfg6_lock, f);
    nd_learn(ip->src, slla);
    ra_seen = true;
}

static void icmp6_input(const uint8_t *eth_src, const struct ip6_hdr *ip,
                        const uint8_t *m, int len)
{
    if (len < 4)
        return;
    if (net_transport_checksum6(ip->src, ip->dst, NH_ICMPV6, m, len) != 0)
        return;

    uint8_t type = m[0];
    if (type >= ND_RS && type <= ND_NA) {
        if (ip->hlim != 255 || m[1] != 0)
            return;
        if (type == ND_NS)
            nd_input_ns(eth_src, ip, m, len);
        else if (type == ND_NA)
            nd_input_na(eth_src, m, len);
        else if (type == ND_RA)
            nd_input_ra(eth_src, ip, m, len);
        return;
    }

    if (type == ICMP6_ECHO_REQ && len >= 8 && is_mine(ip->dst) &&
        len <= ETH_MTU - (int)sizeof(struct ip6_hdr)) {
        uint8_t reply[ETH_MTU - sizeof(struct ip6_hdr)];
        memcpy(reply, m, len);
        reply[0] = ICMP6_ECHO_REPLY;
        icmp6_send_mac(eth_src, ip->dst, ip->src, 64, reply, len);
        return;
    }

    if (type == ICMP6_ECHO_REPLY && len >= 8) {
        uint16_t id = (uint16_t)(m[4] << 8 | m[5]);
        uint16_t seq = (uint16_t)(m[6] << 8 | m[7]);
        uint64_t f = spin_lock_irqsave(&ping6_lock);
        if (ping6_waiting && ip6_eq(ip->src, ping6_target) &&
            id == ping6_id && seq == ping6_seq) {
            ping6_reply_ticks = timer_ticks();
            ping6_got = true;
        }
        spin_unlock_irqrestore(&ping6_lock, f);
    }
}

/* ---- IPv6 input (IRQ context) ---- */

void ipv6_input(const uint8_t *eth_src, const uint8_t *pkt, int len)
{
    const struct ip6_hdr *ip = (const struct ip6_hdr *)pkt;

    if (!ll_ok || len < (int)sizeof(*ip) || (pkt[0] >> 4) != 6)
        return;
    int plen = ntohs(ip->plen);
    if (plen > len - (int)sizeof(*ip))
        return;
    if (!accept_dst(ip->dst) || ip6_is_multicast(ip->src))
        return;

    const uint8_t *p = pkt + sizeof(*ip);
    uint8_t next = ip->next;
    while (next == NH_HOPOPTS || next == NH_ROUTING || next == NH_DSTOPTS) {
        if (plen < 8)
            return;
        int hl = (p[1] + 1) * 8;
        if (hl > plen)
            return;
        next = p[0];
        p += hl;
        plen -= hl;
    }

    if (!ip6_eq(ip->src, unspec))
        nd_learn(ip->src, eth_src);

    if (next == NH_ICMPV6)
        icmp6_input(eth_src, ip, p, plen);
    else if (next == NH_UDP && is_mine(ip->dst))
        udp_input6(ip->src, ip->dst, p, plen);
    else if (next == NH_TCP && is_mine(ip->dst))
        tcp_input6(ip->src, ip->dst, p, plen);
}

/* ---- configuration ---- */

void net6_get_info(struct k_netinfo6 *out)
{
    memset(out, 0, sizeof(*out));
    uint64_t f = spin_lock_irqsave(&cfg6_lock);
    if (ll_ok) {
        memcpy(out->link_local, ll_addr, 16);
        out->flags |= K_NET6_LINKLOCAL;
    }
    if (gl_ok) {
        memcpy(out->global, gl_addr, 16);
        out->prefix_len = gl_plen;
        out->flags |= K_NET6_GLOBAL;
    }
    if (rtr_ok) {
        memcpy(out->router, rtr_addr, 16);
        out->flags |= K_NET6_ROUTER;
    }
    if (dns_ok) {
        memcpy(out->dns, dns6, 16);
        out->flags |= K_NET6_DNS;
    }
    spin_unlock_irqrestore(&cfg6_lock, f);
}

bool net6_dns_addr(uint8_t out[16])
{
    uint64_t f = spin_lock_irqsave(&cfg6_lock);
    bool ok = dns_ok;
    if (ok)
        memcpy(out, dns6, 16);
    spin_unlock_irqrestore(&cfg6_lock, f);
    return ok;
}

void net6_init(void)
{
    static const uint8_t ll_prefix[8] = { 0xfe, 0x80 };
    char a[IP6_TEXT_MAX], b[IP6_TEXT_MAX];

    if (!net_ready())
        return;
    /* Optimistic DAD (RFC 4429): announce the link-local address and use
     * it immediately; a duplicate EUI-64 on a virtual link is not expected. */
    eui64(ll_addr, ll_prefix);
    ll_ok = true;
    nd_send_ns(unspec, ll_addr);

    for (int attempt = 0; attempt < 3 && !ra_seen; attempt++) {
        nd_send_rs();
        uint64_t deadline = timer_ticks() + TIMER_HZ / 2;
        while (timer_ticks() < deadline && !ra_seen)
            net_wait_tick();
    }

    kprintf("net6: link-local %s\n", ip6_text_format(ll_addr, a));
    if (gl_ok) {
        nd_send_ns(unspec, gl_addr);
        kprintf("net6: global %s/%u via %s (slaac)\n",
                ip6_text_format(gl_addr, a), gl_plen,
                rtr_ok ? ip6_text_format(rtr_addr, b) : "none");
    } else {
        kprintf("net6: no router advertisement, link-local only\n");
    }
    if (dns_ok)
        kprintf("net6: dns %s\n", ip6_text_format(dns6, a));
}
