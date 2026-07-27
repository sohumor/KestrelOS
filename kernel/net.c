#include "kernel.h"
#include "string.h"
#include "timer.h"
#include "proc.h"
#include "pci.h"
#include "netdev.h"
#include "rtl8139.h"
#include "e1000.h"
#include "net.h"
#include "dhcp.h"
#include "tcp.h"

/* Ethernet / ARP / IPv4 / ICMP core.
 *
 * RX runs in IRQ context (net_rx from the NIC ISR): it may transmit
 * replies (ARP reply, ICMP echo reply) but never sleeps or waits.
 * All blocking waits (arp_resolve, icmp_ping, udp_recv) run in task
 * context and spin on net_wait_tick().
 */

#define ETHERTYPE_IP  0x0800
#define ETHERTYPE_ARP 0x0806

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

struct eth_hdr {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t type;
} __attribute__((packed));

struct arp_pkt {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} __attribute__((packed));

struct ip_hdr {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t csum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t csum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

static bool ready;
static uint32_t cfg_ip, cfg_mask, cfg_gw, cfg_dns;   /* network order */

static const uint8_t bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ---- netdev registry: the single active NIC ---- */

static const struct netdev *netdev_active;

void netdev_register(const struct netdev *dev)
{
    netdev_active = dev;
}

const struct netdev *netdev_current(void)
{
    return netdev_active;
}

/* ---- helpers ---- */

uint16_t net_checksum(const void *data, int len)
{
    const uint8_t *p = data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)(p[0] << 8);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    /* Complement, then store big-endian so it can go straight into
     * a header field. Checksumming a valid header then yields 0. */
    return htons((uint16_t)~sum);
}

bool net_ready(void)
{
    return ready && netdev_current() != NULL;
}

uint32_t net_ip_addr(void)
{
    return cfg_ip;
}

uint32_t net_dns_addr(void)
{
    return cfg_dns;
}

void net_poll(void)
{
    const struct netdev *nd = netdev_current();
    if (ready && nd)
        nd->poll();
}

void net_wait_tick(void)
{
    net_poll();
    if (sched_active)
        task_sleep_ticks(1);
    else
        __asm__ volatile("hlt");
}

static bool same_subnet(uint32_t ip)
{
    /* Works directly on network-order values: the mask is bytewise. */
    return (ip & cfg_mask) == (cfg_ip & cfg_mask);
}

/* ---- ethernet ---- */

static int eth_send(const uint8_t *dst_mac, uint16_t ethertype,
                    const void *payload, int len)
{
    uint8_t frame[sizeof(struct eth_hdr) + ETH_MTU];
    struct eth_hdr *eh = (struct eth_hdr *)frame;
    const struct netdev *nd = netdev_current();

    if (!nd || len < 0 || len > ETH_MTU)
        return -1;
    memcpy(eh->dst, dst_mac, 6);
    memcpy(eh->src, nd->mac, 6);
    eh->type = htons(ethertype);
    memcpy(frame + sizeof(*eh), payload, len);
    return nd->send(frame, (int)sizeof(*eh) + len);
}

/* ---- ARP ---- */

#define ARP_CACHE_SIZE 16

struct arp_entry {
    uint32_t ip;         /* network order */
    uint8_t  mac[6];
    bool     valid;
};

static struct arp_entry arp_cache[ARP_CACHE_SIZE];
static int arp_next;

/* Called from IRQ context only (rx path). */
static void arp_learn(uint32_t ip, const uint8_t *mac)
{
    if (ip == 0)
        return;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    struct arp_entry *e = &arp_cache[arp_next];
    arp_next = (arp_next + 1) % ARP_CACHE_SIZE;
    e->valid = false;
    e->ip = ip;
    memcpy(e->mac, mac, 6);
    e->valid = true;
}

static bool arp_lookup(uint32_t ip, uint8_t *mac_out)
{
    uint64_t f = irq_save();
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(mac_out, arp_cache[i].mac, 6);
            irq_restore(f);
            return true;
        }
    }
    irq_restore(f);
    return false;
}

static void arp_send_request(uint32_t ip)
{
    struct arp_pkt a;

    a.htype = htons(1);              /* ethernet */
    a.ptype = htons(ETHERTYPE_IP);
    a.hlen = 6;
    a.plen = 4;
    a.oper = htons(1);               /* request */
    memcpy(a.sha, netdev_current()->mac, 6);
    a.spa = cfg_ip;
    memset(a.tha, 0, 6);
    a.tpa = ip;
    eth_send(bcast_mac, ETHERTYPE_ARP, &a, sizeof(a));
}

/* Task context: resolve the next-hop MAC for dst (gateway if off-subnet).
 * ~1s timeout per try, 3 tries. Returns 0 / -1. */
static int arp_resolve(uint32_t ip, uint8_t *mac_out)
{
    if (ip == 0xFFFFFFFFu || ip == (cfg_ip | ~cfg_mask)) {
        memcpy(mac_out, bcast_mac, 6);
        return 0;
    }

    uint32_t target = same_subnet(ip) ? ip : cfg_gw;

    if (arp_lookup(target, mac_out))
        return 0;

    for (int attempt = 0; attempt < 3; attempt++) {
        arp_send_request(target);
        uint64_t deadline = timer_ticks() + TIMER_HZ;   /* ~1 s */
        while (timer_ticks() < deadline) {
            if (arp_lookup(target, mac_out))
                return 0;
            net_wait_tick();
        }
    }
    kprintf("net: arp resolve failed for %08x\n", ntohl(target));
    return -1;
}

/* IRQ context. */
static void arp_input(const uint8_t *pkt, int len)
{
    if (len < (int)sizeof(struct arp_pkt))
        return;
    const struct arp_pkt *a = (const struct arp_pkt *)pkt;
    if (ntohs(a->htype) != 1 || ntohs(a->ptype) != ETHERTYPE_IP ||
        a->hlen != 6 || a->plen != 4)
        return;

    arp_learn(a->spa, a->sha);

    if (ntohs(a->oper) == 1 && a->tpa == cfg_ip) {
        struct arp_pkt r;
        r.htype = htons(1);
        r.ptype = htons(ETHERTYPE_IP);
        r.hlen = 6;
        r.plen = 4;
        r.oper = htons(2);           /* reply */
        memcpy(r.sha, netdev_current()->mac, 6);
        r.spa = cfg_ip;
        memcpy(r.tha, a->sha, 6);
        r.tpa = a->spa;
        eth_send(a->sha, ETHERTYPE_ARP, &r, sizeof(r));
    }
}

/* ---- IPv4 ---- */

/* Transmit to an already-known next-hop MAC. The RX path must use this:
 * resolving ARP from IRQ context would re-enter rx_drain on the same
 * unconsumed frame and recurse until the kernel stack overflows. */
static int ip_send_mac(const uint8_t *dst_mac, uint32_t dst_ip_be,
                       uint8_t proto, const void *payload, int len)
{
    uint8_t pkt[ETH_MTU];
    struct ip_hdr *ip = (struct ip_hdr *)pkt;
    static uint16_t ip_id;

    if (!ready || len < 0 || len > ETH_MTU - (int)sizeof(*ip))
        return -1;

    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons((uint16_t)(sizeof(*ip) + len));
    ip->id = htons(++ip_id);
    ip->frag = 0;
    ip->ttl = 64;
    ip->proto = proto;
    ip->csum = 0;
    ip->src = cfg_ip;
    ip->dst = dst_ip_be;
    ip->csum = net_checksum(ip, sizeof(*ip));

    memcpy(pkt + sizeof(*ip), payload, len);
    return eth_send(dst_mac, ETHERTYPE_IP, pkt, (int)sizeof(*ip) + len);
}

/* Task context only: may block for seconds inside arp_resolve. */
int net_ip_send(uint32_t dst_ip_be, uint8_t proto,
                const void *payload, int len)
{
    uint8_t dst_mac[6];

    if (!ready || len < 0 || len > ETH_MTU - (int)sizeof(struct ip_hdr))
        return -1;
    if (arp_resolve(dst_ip_be, dst_mac) < 0)
        return -1;
    return ip_send_mac(dst_mac, dst_ip_be, proto, payload, len);
}

/* ---- ICMP ---- */

static volatile bool ping_busy;                  /* one ping at a time */
static volatile bool ping_waiting;
static volatile bool ping_got;
static volatile uint16_t ping_id, ping_seq;      /* host order */
static volatile uint32_t ping_target;            /* network order */
static volatile uint64_t ping_reply_ticks;

/* IRQ context. src_mac is the frame's sender, i.e. our next hop back. */
static void icmp_input(uint32_t src_ip, const uint8_t *src_mac,
                       const uint8_t *pkt, int len)
{
    if (len < (int)sizeof(struct icmp_hdr))
        return;
    if (net_checksum(pkt, len) != 0)
        return;
    const struct icmp_hdr *ic = (const struct icmp_hdr *)pkt;

    if (ic->type == 8 && ic->code == 0) {
        /* Echo request: reply straight to the sender's MAC. Going through
         * net_ip_send() would call arp_resolve() here, and for an
         * off-subnet source that means waiting on the (possibly unknown)
         * gateway from inside the RX interrupt. */
        uint8_t reply[ETH_MTU - 20];
        if (len > (int)sizeof(reply))
            return;
        memcpy(reply, pkt, len);
        struct icmp_hdr *rh = (struct icmp_hdr *)reply;
        rh->type = 0;
        rh->csum = 0;
        rh->csum = net_checksum(reply, len);
        ip_send_mac(src_mac, src_ip, IP_PROTO_ICMP, reply, len);
        return;
    }

    /* Match the address too: id is just a pid and seq is a small counter,
     * so without it any host on the segment can satisfy our wait. */
    if (ic->type == 0 && ping_waiting && src_ip == ping_target &&
        ntohs(ic->id) == ping_id && ntohs(ic->seq) == ping_seq) {
        ping_reply_ticks = timer_ticks();
        ping_got = true;
    }
}

long icmp_ping(uint32_t ip_be, int timeout_ms)
{
    static uint16_t seq_counter;
    uint8_t pkt[sizeof(struct icmp_hdr) + 32];
    struct icmp_hdr *ic = (struct icmp_hdr *)pkt;

    if (!ready)
        return -1;
    if (timeout_ms <= 0)
        timeout_ms = 1000;

    /* The match state is a single set of globals, so a second concurrent
     * ping would steal the first one's reply. Refuse instead. */
    uint64_t f = irq_save();
    if (ping_busy) {
        irq_restore(f);
        return -1;
    }
    ping_busy = true;
    uint16_t myseq = ++seq_counter;
    uint16_t myid = (uint16_t)(current ? current->pid : 1);
    ping_id = myid;
    ping_seq = myseq;
    ping_target = ip_be;
    ping_got = false;
    ping_waiting = true;
    irq_restore(f);

    ic->type = 8;
    ic->code = 0;
    ic->csum = 0;
    ic->id = htons(myid);
    ic->seq = htons(myseq);
    for (int i = 0; i < 32; i++)
        pkt[sizeof(*ic) + i] = (uint8_t)('a' + (i & 15));
    ic->csum = net_checksum(pkt, sizeof(pkt));

    if (net_ip_send(ip_be, IP_PROTO_ICMP, pkt, sizeof(pkt)) < 0) {
        ping_waiting = false;
        ping_busy = false;
        return -1;
    }

    /* Sample the clock only once the request is on the wire: net_ip_send
     * can spend up to 3 s in arp_resolve, which would otherwise be charged
     * to the round-trip time and could expire the deadline before the
     * wait loop runs even once. */
    uint64_t start = timer_ticks();
    uint64_t deadline = start + (uint64_t)(timeout_ms + 9) / 10;
    while (timer_ticks() <= deadline) {
        if (ping_got) {
            uint64_t got = ping_reply_ticks;
            ping_waiting = false;
            ping_busy = false;
            return got > start ? (long)((got - start) * 10) : 0;
        }
        net_wait_tick();
    }
    ping_waiting = false;
    ping_busy = false;
    return -1;
}

/* IRQ context. */
static void ipv4_input(const struct eth_hdr *eh, const uint8_t *pkt, int len)
{
    if (len < (int)sizeof(struct ip_hdr))
        return;
    const struct ip_hdr *ip = (const struct ip_hdr *)pkt;

    int ihl = (ip->ver_ihl & 0x0F) * 4;
    if ((ip->ver_ihl >> 4) != 4 || ihl < 20 || ihl > len)
        return;
    if (net_checksum(ip, ihl) != 0)
        return;
    if (ip->dst != cfg_ip && ip->dst != 0xFFFFFFFFu &&
        ip->dst != (cfg_ip | ~cfg_mask))
        return;
    if (ntohs(ip->frag) & 0x3FFF)    /* MF set or nonzero offset: drop */
        return;

    int total = ntohs(ip->total_len);
    if (total < ihl || total > len)
        return;

    /* Learn sender's MAC so replies never block on ARP in IRQ context. */
    arp_learn(ip->src, eh->src);

    const uint8_t *payload = pkt + ihl;
    int plen = total - ihl;

    if (ip->proto == IP_PROTO_ICMP)
        icmp_input(ip->src, eh->src, payload, plen);
    else if (ip->proto == IP_PROTO_UDP)
        udp_input(ip->src, payload, plen);
    else if (ip->proto == IP_PROTO_TCP)
        tcp_input(ip->src, payload, plen);
}

/* ---- frame entry point (IRQ context) ---- */

void net_rx(const uint8_t *frame, int len)
{
    if (len < (int)sizeof(struct eth_hdr))
        return;
    const struct eth_hdr *eh = (const struct eth_hdr *)frame;
    uint16_t type = ntohs(eh->type);
    const uint8_t *payload = frame + sizeof(*eh);
    int plen = len - (int)sizeof(*eh);

    if (type == ETHERTYPE_ARP)
        arp_input(payload, plen);
    else if (type == ETHERTYPE_IP)
        ipv4_input(eh, payload, plen);
}

void net_configure(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns)
{
    cfg_ip = ip;
    cfg_mask = mask;
    cfg_gw = gw;
    cfg_dns = dns;
}

/* ---- init ---- */

void net_init(void)
{
    pci_init();
    if (!rtl8139_init() && !e1000_init()) {
        kprintf("net: no NIC found, networking disabled\n");
        return;
    }

    udp_init();
    ready = true;

    if (dhcp_discover() == 0) {
        const uint8_t *m = netdev_current()->mac;
        uint32_t ip = ntohl(cfg_ip);
        uint32_t gw = ntohl(cfg_gw);
        uint32_t dns = ntohl(cfg_dns);
        kprintf("net: up, mac %02x:%02x:%02x:%02x:%02x:%02x\n",
                m[0], m[1], m[2], m[3], m[4], m[5]);
        kprintf("net: ip %u.%u.%u.%u gw %u.%u.%u.%u dns %u.%u.%u.%u (dhcp)\n",
                (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
                (gw >> 24) & 0xFF, (gw >> 16) & 0xFF, (gw >> 8) & 0xFF, gw & 0xFF,
                (dns >> 24) & 0xFF, (dns >> 16) & 0xFF, (dns >> 8) & 0xFF, dns & 0xFF);
    } else {
        /* Static QEMU / VirtualBox user-mode networking defaults fallback. */
        cfg_ip = htonl(0x0A00020F);      /* 10.0.2.15 */
        cfg_mask = htonl(0xFFFFFF00);    /* 255.255.255.0 */
        cfg_gw = htonl(0x0A000202);      /* 10.0.2.2 */
        cfg_dns = htonl(0x0A000203);     /* 10.0.2.3 */

        const uint8_t *m = netdev_current()->mac;
        kprintf("net: up, mac %02x:%02x:%02x:%02x:%02x:%02x\n",
                m[0], m[1], m[2], m[3], m[4], m[5]);
        kprintf("net: ip 10.0.2.15/24 gw 10.0.2.2 dns 10.0.2.3 (static fallback)\n");
    }
}

void net_get_info(struct k_netinfo *out)
{
    memset(out, 0, sizeof(*out));
    if (!ready)
        return;
    out->ip = cfg_ip;
    out->netmask = cfg_mask;
    out->gateway = cfg_gw;
    out->dns = cfg_dns;
    memcpy(out->mac, netdev_current()->mac, 6);
    out->up = 1;
}
