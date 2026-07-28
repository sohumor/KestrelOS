#include "kernel.h"
#include "string.h"
#include "timer.h"
#include "proc.h"
#include "kheap.h"
#include "net.h"
#include "spinlock.h"

/* UDP: port bindings with small per-port receive queues.
 *
 * udp_input runs in IRQ context, so queue slot buffers are allocated
 * up front in task context (bind time) and only recycled afterwards --
 * kmalloc is never called from the IRQ path.
 */

#define UDP_PORTS  16
#define UDP_QUEUE  8

struct udp_hdr {
    uint16_t sport;
    uint16_t dport;
    uint16_t len;
    uint16_t csum;
} __attribute__((packed));

struct udp_slot {
    uint8_t *data;   /* NET_UDP_MAX bytes, kmalloc'd at bind time */
    int len;
    uint32_t src_ip;
    uint16_t src_port;
};

struct udp_bind {
    bool used;
    uint16_t port;              /* host order */
    int owner_pid;              /* task that claimed it, 0 = kernel */
    struct udp_slot q[UDP_QUEUE];
    int head;                   /* oldest queued packet */
    int count;
};

static struct udp_bind binds[UDP_PORTS];
static spinlock_t udp_lock = SPINLOCK_INIT;

void udp_init(void)
{
    memset(binds, 0, sizeof(binds));
    /* Keep allocation out of the IRQ receive path. The buffers persist for
     * the lifetime of the network stack and are recycled between owners. */
    for (int i = 0; i < UDP_PORTS; i++)
        for (int j = 0; j < UDP_QUEUE; j++)
            binds[i].q[j].data = kmalloc(NET_UDP_MAX);
}

static struct udp_bind *bind_find_locked(uint16_t port)
{
    for (int i = 0; i < UDP_PORTS; i++)
        if (binds[i].used && binds[i].port == port)
            return &binds[i];
    return 0;
}

/* Task context: reclaim bindings whose owning task is gone. udp_recv()
 * auto-binds and there is no unbind syscall, so without this a handful of
 * `udp listen` runs would exhaust the table and kill UDP receive (and DNS
 * with it) for the rest of the boot. */
static void bind_gc(void)
{
    for (int i = 0; i < UDP_PORTS; i++) {
        uint64_t f = spin_lock_irqsave(&udp_lock);
        int owner = binds[i].used ? binds[i].owner_pid : 0;
        spin_unlock_irqrestore(&udp_lock, f);
        if (owner <= 0 || task_exists(owner))
            continue;
        f = spin_lock_irqsave(&udp_lock);
        if (binds[i].used && binds[i].owner_pid == owner) {
            binds[i].used = false;
            binds[i].owner_pid = 0;
            binds[i].head = 0;
            binds[i].count = 0;
        }
        spin_unlock_irqrestore(&udp_lock, f);
    }
}

/* Task context: find an existing binding or claim a free entry. Slot
 * buffers persist across rebinds so the IRQ path never allocates. */
static struct udp_bind *bind_get(uint16_t port)
{
    int pid = current ? current->pid : 0;
    bind_gc();

    uint64_t f = spin_lock_irqsave(&udp_lock);
    struct udp_bind *b = bind_find_locked(port);
    if (b) {
        if (b->owner_pid != pid)
            b = NULL;
        spin_unlock_irqrestore(&udp_lock, f);
        return b;
    }

    for (int i = 0; i < UDP_PORTS; i++) {
        if (binds[i].used)
            continue;
        b = &binds[i];
        for (int j = 0; j < UDP_QUEUE; j++) {
            if (!b->q[j].data) {
                spin_unlock_irqrestore(&udp_lock, f);
                return 0;
            }
        }
        b->port = port;
        b->owner_pid = pid;
        b->head = 0;
        b->count = 0;
        b->used = true;
        spin_unlock_irqrestore(&udp_lock, f);
        return b;
    }
    spin_unlock_irqrestore(&udp_lock, f);
    return 0;
}

void udp_unbind(uint16_t port)
{
    uint64_t f = spin_lock_irqsave(&udp_lock);
    struct udp_bind *b = bind_find_locked(port);
    if (!b)
    {
        spin_unlock_irqrestore(&udp_lock, f);
        return;
    }
    b->used = false;
    b->owner_pid = 0;
    b->head = 0;
    b->count = 0;
    spin_unlock_irqrestore(&udp_lock, f);
}

/* IRQ context: parse a UDP segment and queue it on its binding. */
void udp_input(uint32_t src_ip_be, uint32_t dst_ip_be,
               const uint8_t *seg, int len)
{
    if (!seg || len < (int)sizeof(struct udp_hdr))
        return;
    const struct udp_hdr *uh = (const struct udp_hdr *)seg;

    int ulen = ntohs(uh->len);
    if (ulen < (int)sizeof(*uh) || ulen > len)
        return;
    if (uh->csum != 0 &&
        net_transport_checksum(src_ip_be, dst_ip_be, 17, seg, ulen) != 0)
        return;
    int plen = ulen - (int)sizeof(*uh);
    if (plen > NET_UDP_MAX)
        return;

    uint64_t f = spin_lock_irqsave(&udp_lock);
    struct udp_bind *b = bind_find_locked(ntohs(uh->dport));
    if (!b || b->count >= UDP_QUEUE) {
        spin_unlock_irqrestore(&udp_lock, f);
        return;                          /* unbound or full: drop */
    }

    struct udp_slot *s = &b->q[(b->head + b->count) % UDP_QUEUE];
    if (!s->data) {
        spin_unlock_irqrestore(&udp_lock, f);
        return;
    }
    memcpy(s->data, seg + sizeof(*uh), plen);
    s->len = plen;
    s->src_ip = src_ip_be;
    s->src_port = ntohs(uh->sport);
    b->count++;
    spin_unlock_irqrestore(&udp_lock, f);
}

int udp_send(uint32_t ip_be, uint16_t sport, uint16_t dport,
             const void *buf, int len)
{
    uint8_t pkt[sizeof(struct udp_hdr) + NET_UDP_MAX];
    struct udp_hdr *uh = (struct udp_hdr *)pkt;

    if (!net_ready() || !buf || len < 0 || len > NET_UDP_MAX)
        return -1;

    uh->sport = htons(sport);
    uh->dport = htons(dport);
    uh->len = htons((uint16_t)(sizeof(*uh) + len));
    uh->csum = 0;
    memcpy(pkt + sizeof(*uh), buf, len);
    uh->csum = net_transport_checksum(net_ip_addr(), ip_be, 17, pkt,
                                      (int)sizeof(*uh) + len);
    if (uh->csum == 0)
        uh->csum = 0xFFFF;  /* RFC 768: computed zero is sent as all ones. */
    return net_ip_send(ip_be, 17, pkt, (int)sizeof(*uh) + len);
}

int udp_recv(uint16_t port, void *buf, int maxlen, int timeout_ms)
{
    if (!net_ready() || !buf || maxlen < 0)
        return -1;
    if (timeout_ms < 0)
        timeout_ms = 0;

    struct udp_bind *b = bind_get(port);
    if (!b)
        return -1;

    uint64_t deadline = timer_ticks() + (uint64_t)(timeout_ms + 9) / 10;
    for (;;) {
        uint64_t f = spin_lock_irqsave(&udp_lock);
        if (b->count > 0) {
            struct udp_slot *s = &b->q[b->head];
            int n = s->len < maxlen ? s->len : maxlen;
            memcpy(buf, s->data, n);
            b->head = (b->head + 1) % UDP_QUEUE;
            b->count--;
            spin_unlock_irqrestore(&udp_lock, f);
            return n;
        }
        spin_unlock_irqrestore(&udp_lock, f);
        if (timer_ticks() >= deadline)
            return -1;
        net_wait_tick();
    }
}
