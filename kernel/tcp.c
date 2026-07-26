#include "kernel.h"
#include "string.h"
#include "timer.h"
#include "proc.h"
#include "kheap.h"
#include "net.h"
#include "tcp.h"

/* TCP (client side).
 *
 * Split of responsibilities:
 *   tcp_input()  runs in the NIC IRQ. It validates the segment, advances
 *                the sequence state, copies payload into the connection's
 *                receive ring and raises flags. It never transmits: sending
 *                goes through net_ip_send(), which may block for seconds
 *                inside arp_resolve().
 *   conn_pump()  runs in task context (tcp_tick, or inside the blocking
 *                calls' wait loops) and does every transmission: new data,
 *                retransmissions, FIN, and the ACKs that tcp_input asked
 *                for by setting need_ack.
 *
 * Everything touched by both contexts is guarded with irq_save/irq_restore.
 * Buffers are kmalloc'd once per slot in task context and kept for reuse,
 * so the IRQ path never allocates.
 */

#define IP_PROTO_TCP  6

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

#define TCP_RTO_MIN     50      /* ticks, 100 Hz -> 500 ms */
#define TCP_RTO_MAX     800     /* ticks -> 8 s */
#define TCP_MAX_TRIES   8       /* give up after this many retransmits */
#define TCP_TW_TICKS    100     /* TIME_WAIT linger, ~1 s (see notes) */
#define TCP_CLOSE_TICKS 200     /* how long tcp_close waits, ~2 s */
#define TCP_SEND_TICKS  1000    /* how long tcp_send waits for room, ~10 s */
#define TCP_PORT_FIRST  49152
#define TCP_PORT_LAST   60000

enum tcp_state {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
};

struct tcp_hdr {
    uint16_t sport;
    uint16_t dport;
    uint32_t seq;
    uint32_t ack;
    uint8_t  off;          /* data offset in 32-bit words, << 4 */
    uint8_t  flags;
    uint16_t win;
    uint16_t csum;
    uint16_t urg;
} __attribute__((packed));

struct tcp_conn {
    bool     used;
    bool     detached;     /* handle released, kernel still finishing up */
    bool     pumping;      /* conn_pump() in progress (task context) */
    int      state;
    int      owner_pid;

    uint32_t peer_ip;      /* network order */
    uint16_t peer_port;    /* host order */
    uint16_t local_port;   /* host order */

    /* send side */
    uint32_t iss;
    uint32_t snd_una;      /* oldest unacknowledged sequence number */
    uint32_t snd_nxt;      /* next sequence number to transmit */
    uint32_t snd_wnd;      /* peer's advertised window */
    uint8_t *txbuf;        /* ring, holds tx_len bytes starting at snd_una */
    int      tx_head;
    int      tx_len;
    bool     fin_pending;  /* application asked to close */
    bool     fin_sent;
    uint32_t fin_seq;      /* sequence number our FIN occupies */

    /* receive side */
    uint32_t irs;
    uint32_t rcv_nxt;
    uint8_t *rxbuf;        /* ring */
    int      rx_head;
    int      rx_len;
    bool     peer_fin;
    bool     need_ack;

    /* timers */
    uint32_t rto_ticks;
    uint64_t rto_deadline; /* 0 = disarmed */
    int      retries;
    uint64_t tw_deadline;

    bool     reset;        /* RST received or the connection gave up */
};

static struct tcp_conn conns[TCP_CONNS];
static uint16_t port_next = TCP_PORT_FIRST;

/* ---- sequence arithmetic ----
 * Sequence numbers wrap at 2^32; raw unsigned comparison is wrong across
 * the wrap. Compare the signed difference instead: valid as long as the
 * two values are less than 2^31 apart, which they always are here. */

static inline bool seq_lt(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static inline bool seq_leq(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) <= 0;
}

/* ---- checksum ----
 * Same convention as net_checksum(): the result is already big-endian and
 * ready to store, and checksumming a valid segment yields 0. */

static uint16_t tcp_csum(uint32_t src_be, uint32_t dst_be,
                         const uint8_t *seg, int len)
{
    const uint8_t *s = (const uint8_t *)&src_be;
    const uint8_t *d = (const uint8_t *)&dst_be;
    uint32_t sum = 0;

    /* pseudo-header: src, dst, zero, protocol, TCP length */
    sum += (uint32_t)((s[0] << 8) | s[1]);
    sum += (uint32_t)((s[2] << 8) | s[3]);
    sum += (uint32_t)((d[0] << 8) | d[1]);
    sum += (uint32_t)((d[2] << 8) | d[3]);
    sum += (uint32_t)IP_PROTO_TCP;
    sum += (uint32_t)len;

    for (int i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)((seg[i] << 8) | seg[i + 1]);
    if (len & 1)
        sum += (uint32_t)(seg[len - 1] << 8);

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

/* ---- helpers ---- */

/* Free space in the receive ring, i.e. the window we advertise. */
static uint16_t rcv_window(struct tcp_conn *c)
{
    uint64_t f = irq_save();
    int freebytes = TCP_RXBUF - c->rx_len;
    irq_restore(f);
    if (freebytes < 0)
        freebytes = 0;
    if (freebytes > 65535)
        freebytes = 65535;
    return (uint16_t)freebytes;
}

/* Task context only (net_ip_send may block in ARP).
 * txoff/dlen name a window into the send ring measured from tx_head. */
static int seg_send(struct tcp_conn *c, uint8_t flags, uint32_t seq,
                    int txoff, int dlen, bool with_mss)
{
    uint8_t pkt[sizeof(struct tcp_hdr) + 4 + TCP_MSS];
    struct tcp_hdr *th = (struct tcp_hdr *)pkt;
    int hlen = (int)sizeof(*th) + (with_mss ? 4 : 0);
    uint8_t *payload = pkt + hlen;

    if (dlen < 0 || dlen > TCP_MSS)
        return -1;

    th->sport = htons(c->local_port);
    th->dport = htons(c->peer_port);
    th->seq = htonl(seq);
    th->off = (uint8_t)((hlen / 4) << 4);
    th->flags = flags;
    th->win = htons(rcv_window(c));
    th->csum = 0;
    th->urg = 0;

    if (with_mss) {
        /* kind 2, length 4, MSS 1460 -- exactly one 32-bit word, no pad. */
        pkt[sizeof(*th) + 0] = 2;
        pkt[sizeof(*th) + 1] = 4;
        pkt[sizeof(*th) + 2] = (uint8_t)(TCP_MSS >> 8);
        pkt[sizeof(*th) + 3] = (uint8_t)(TCP_MSS & 0xFF);
    }

    /* Snapshot the ring with interrupts masked: an ACK arriving mid-copy
     * would advance tx_head and let a concurrent tcp_send() overwrite the
     * bytes we are still reading. At most one MSS, so the window is short. */
    uint64_t f = irq_save();
    th->ack = (flags & TCP_ACK) ? htonl(c->rcv_nxt) : 0;
    if (dlen > 0) {
        int pos = (c->tx_head + txoff) % TCP_TXBUF;
        int first = TCP_TXBUF - pos;
        if (first > dlen)
            first = dlen;
        memcpy(payload, c->txbuf + pos, (size_t)first);
        if (dlen > first)
            memcpy(payload + first, c->txbuf, (size_t)(dlen - first));
    }
    irq_restore(f);

    th->csum = tcp_csum(net_ip_addr(), c->peer_ip, pkt, hlen + dlen);
    return net_ip_send(c->peer_ip, IP_PROTO_TCP, pkt, hlen + dlen);
}

/* Task context. Buffers are deliberately kept for reuse by the next
 * connection so that the IRQ path never has to allocate. */
static void conn_release(struct tcp_conn *c)
{
    uint64_t f = irq_save();
    c->used = false;
    c->detached = false;
    c->state = TCP_CLOSED;
    c->owner_pid = 0;
    c->peer_ip = 0;
    c->peer_port = 0;
    c->local_port = 0;
    c->tx_head = c->tx_len = 0;
    c->rx_head = c->rx_len = 0;
    c->fin_pending = c->fin_sent = false;
    c->peer_fin = c->need_ack = c->reset = false;
    c->rto_deadline = 0;
    c->retries = 0;
    irq_restore(f);
}

static bool port_in_use(uint16_t p)
{
    for (int i = 0; i < TCP_CONNS; i++)
        if (conns[i].used && conns[i].local_port == p)
            return true;
    return false;
}

static uint16_t port_alloc(void)
{
    for (int tries = 0; tries < (TCP_PORT_LAST - TCP_PORT_FIRST); tries++) {
        uint16_t p = port_next++;
        if (port_next > TCP_PORT_LAST)
            port_next = TCP_PORT_FIRST;
        if (!port_in_use(p))
            return p;
    }
    return 0;
}

/* IRQ context: match an incoming segment to a connection. */
static struct tcp_conn *conn_lookup(uint32_t src_ip, uint16_t sport,
                                    uint16_t dport)
{
    for (int i = 0; i < TCP_CONNS; i++) {
        struct tcp_conn *c = &conns[i];
        if (c->used && c->state != TCP_CLOSED && c->peer_ip == src_ip &&
            c->peer_port == sport && c->local_port == dport)
            return c;
    }
    return 0;
}

/* Task context: validate a handle coming from the syscall layer. */
static struct tcp_conn *conn_get(int handle)
{
    if (handle < 0 || handle >= TCP_CONNS)
        return 0;
    struct tcp_conn *c = &conns[handle];
    if (!c->used || c->detached)
        return 0;
    if (c->owner_pid > 0 && current && current->pid != c->owner_pid)
        return 0;
    return c;
}

/* ---- IRQ side ---- */

static void process_ack(struct tcp_conn *c, uint32_t ack, uint16_t win)
{
    c->snd_wnd = win;

    if (seq_leq(ack, c->snd_una) || seq_lt(c->snd_nxt, ack))
        return;                       /* duplicate, or beyond what we sent */

    uint32_t acked = ack - c->snd_una;
    bool fin_acked = c->fin_sent && seq_lt(c->fin_seq, ack);
    uint32_t dacked = acked;

    if (fin_acked && dacked > 0)
        dacked--;                     /* one of those "bytes" is the FIN */
    if (dacked > (uint32_t)c->tx_len)
        dacked = (uint32_t)c->tx_len;

    c->tx_head = (c->tx_head + (int)dacked) % TCP_TXBUF;
    c->tx_len -= (int)dacked;
    c->snd_una = ack;

    c->retries = 0;
    c->rto_ticks = TCP_RTO_MIN;
    c->rto_deadline = (c->snd_una != c->snd_nxt)
                          ? timer_ticks() + c->rto_ticks : 0;

    if (fin_acked) {
        c->fin_pending = false;
        if (c->state == TCP_FIN_WAIT_1) {
            /* peer_fin here means a simultaneous close (RFC's CLOSING). */
            c->state = c->peer_fin ? TCP_TIME_WAIT : TCP_FIN_WAIT_2;
        } else if (c->state == TCP_LAST_ACK) {
            c->state = TCP_CLOSED;
        }
        if (c->state == TCP_TIME_WAIT)
            c->tw_deadline = timer_ticks() + TCP_TW_TICKS;
    }
}

/* Only data landing exactly at rcv_nxt is kept: anything else is dropped
 * and an ACK is scheduled so the peer retransmits the hole. */
static void accept_data(struct tcp_conn *c, uint32_t seq,
                        const uint8_t *data, int dlen)
{
    if (seq != c->rcv_nxt || c->peer_fin) {
        c->need_ack = true;
        return;
    }

    int freebytes = TCP_RXBUF - c->rx_len;
    int n = dlen < freebytes ? dlen : freebytes;
    if (n <= 0) {
        c->need_ack = true;           /* window closed: re-advertise 0 */
        return;
    }

    int tail = (c->rx_head + c->rx_len) % TCP_RXBUF;
    int first = TCP_RXBUF - tail;
    if (first > n)
        first = n;
    memcpy(c->rxbuf + tail, data, (size_t)first);
    if (n > first)
        memcpy(c->rxbuf, data + first, (size_t)(n - first));

    c->rx_len += n;
    c->rcv_nxt += (uint32_t)n;
    c->need_ack = true;
}

void tcp_input(uint32_t src_ip_be, const uint8_t *seg, int len)
{
    if (!seg || len < (int)sizeof(struct tcp_hdr))
        return;

    const struct tcp_hdr *th = (const struct tcp_hdr *)seg;
    int hlen = (th->off >> 4) * 4;
    if (hlen < (int)sizeof(*th) || hlen > len)
        return;
    if (tcp_csum(src_ip_be, net_ip_addr(), seg, len) != 0)
        return;

    struct tcp_conn *c = conn_lookup(src_ip_be, ntohs(th->sport),
                                     ntohs(th->dport));
    if (!c || !c->rxbuf)
        return;

    uint32_t seq = ntohl(th->seq);
    uint32_t ack = ntohl(th->ack);
    uint8_t fl = th->flags;
    const uint8_t *data = seg + hlen;
    int dlen = len - hlen;

    if (fl & TCP_RST) {
        /* An unacknowledged RST during the handshake is not for us. */
        if (c->state == TCP_SYN_SENT && !(fl & TCP_ACK))
            return;
        c->reset = true;
        c->state = TCP_CLOSED;
        return;
    }

    if (c->state == TCP_SYN_SENT) {
        if ((fl & (TCP_SYN | TCP_ACK)) != (TCP_SYN | TCP_ACK))
            return;
        if (ack != c->snd_nxt)        /* not the ACK of our SYN */
            return;
        c->irs = seq;
        c->rcv_nxt = seq + 1;         /* the SYN consumes one number */
        c->snd_una = ack;
        c->snd_wnd = ntohs(th->win);
        c->state = TCP_ESTABLISHED;
        c->need_ack = true;
        c->retries = 0;
        c->rto_ticks = TCP_RTO_MIN;
        c->rto_deadline = 0;
        return;                       /* data on a SYN|ACK is not accepted */
    }

    if (fl & TCP_SYN)                 /* stray SYN on a live connection */
        return;

    if (fl & TCP_ACK)
        process_ack(c, ack, ntohs(th->win));

    if (dlen > 0)
        accept_data(c, seq, data, dlen);

    /* Honour the FIN only once every byte in front of it has been taken,
     * otherwise its sequence number is not the one at rcv_nxt. */
    if ((fl & TCP_FIN) && seq + (uint32_t)dlen == c->rcv_nxt) {
        c->rcv_nxt++;
        c->peer_fin = true;
        c->need_ack = true;
        if (c->state == TCP_ESTABLISHED) {
            c->state = TCP_CLOSE_WAIT;
        } else if (c->state == TCP_FIN_WAIT_2) {
            c->state = TCP_TIME_WAIT;
            c->tw_deadline = timer_ticks() + TCP_TW_TICKS;
        }
        /* FIN_WAIT_1: stay put; process_ack() promotes us to TIME_WAIT
         * once our own FIN is acknowledged. */
    }
}

/* ---- task side: the transmit pump ---- */

static void conn_pump(struct tcp_conn *c)
{
    uint64_t f = irq_save();
    if (!c->used || c->pumping) {
        irq_restore(f);
        return;
    }
    c->pumping = true;
    irq_restore(f);

    uint64_t now = timer_ticks();

    if (c->reset || c->state == TCP_CLOSED)
        goto out;

    if (c->state == TCP_TIME_WAIT) {
        if (now >= c->tw_deadline)
            c->state = TCP_CLOSED;
        goto out;
    }

    if (c->state == TCP_SYN_SENT) {
        if (c->rto_deadline && now >= c->rto_deadline) {
            if (c->retries >= TCP_MAX_TRIES) {
                kprintf("tcp: connect to port %u timed out\n", c->peer_port);
                c->reset = true;
                c->state = TCP_CLOSED;
                goto out;
            }
            c->retries++;
            if (c->rto_ticks < TCP_RTO_MAX)
                c->rto_ticks *= 2;
            c->rto_deadline = timer_ticks() + c->rto_ticks;
            seg_send(c, TCP_SYN, c->iss, 0, 0, true);
        }
        goto out;
    }

    /* Retransmission: go back to snd_una and resend from there. There is
     * no selective ack and no reassembly queue, so go-back-N is exact. */
    if (c->snd_una != c->snd_nxt && c->rto_deadline && now >= c->rto_deadline) {
        if (c->retries >= TCP_MAX_TRIES) {
            kprintf("tcp: giving up after %d retransmits\n", c->retries);
            c->reset = true;
            c->state = TCP_CLOSED;
            goto out;
        }
        c->retries++;
        if (c->rto_ticks < TCP_RTO_MAX)
            c->rto_ticks *= 2;
        f = irq_save();
        c->snd_nxt = c->snd_una;      /* fin_seq is unchanged by this */
        c->rto_deadline = timer_ticks() + c->rto_ticks;
        irq_restore(f);
    }

    bool sent_any = false;

    for (int i = 0; i < 8; i++) {
        f = irq_save();
        uint32_t una = c->snd_una;
        uint32_t nxt = c->snd_nxt;
        int off = (int)(nxt - una);
        int avail = c->tx_len - off;
        int allow = (int)c->snd_wnd - off;
        irq_restore(f);

        if (avail <= 0)
            break;
        if (allow <= 0) {
            /* Zero window. Probe with a single byte once per RTO so the
             * peer's window update cannot be lost silently. */
            if (c->rto_deadline && now < c->rto_deadline)
                break;
            allow = 1;
        }

        int n = avail < allow ? avail : allow;
        if (n > TCP_MSS)
            n = TCP_MSS;
        if (seg_send(c, TCP_ACK | TCP_PSH, nxt, off, n, false) < 0)
            break;

        f = irq_save();
        c->snd_nxt = nxt + (uint32_t)n;
        c->need_ack = false;
        if (una == nxt)               /* nothing was in flight: arm the RTO */
            c->rto_deadline = timer_ticks() + c->rto_ticks;
        irq_restore(f);
        sent_any = true;
    }

    /* The FIN goes out only once every queued byte has been transmitted;
     * fin_seq is snd_una + tx_len, which an ACK never changes, so a
     * retransmitted FIN always carries the same sequence number. */
    f = irq_save();
    uint32_t fseq = c->snd_nxt;
    bool fin_now = c->fin_pending &&
                   c->snd_nxt == c->snd_una + (uint32_t)c->tx_len;
    irq_restore(f);

    if (fin_now && seg_send(c, TCP_ACK | TCP_FIN, fseq, 0, 0, false) >= 0) {
        f = irq_save();
        c->fin_seq = fseq;
        c->fin_sent = true;
        c->snd_nxt = fseq + 1;
        c->need_ack = false;
        c->rto_deadline = timer_ticks() + c->rto_ticks;
        irq_restore(f);
        sent_any = true;
    }

    if (!sent_any && c->need_ack) {
        if (seg_send(c, TCP_ACK, c->snd_nxt, 0, 0, false) >= 0)
            c->need_ack = false;
    }

out:
    c->pumping = false;
    if (c->detached && (c->state == TCP_CLOSED || c->reset))
        conn_release(c);
}

void tcp_tick(void)
{
    for (int i = 0; i < TCP_CONNS; i++) {
        struct tcp_conn *c = &conns[i];
        if (!c->used)
            continue;

        /* Reclaim connections whose owning task died without closing. */
        if (!c->detached && c->owner_pid > 0 && !task_find(c->owner_pid)) {
            uint64_t f = irq_save();
            c->detached = true;
            if (c->state == TCP_ESTABLISHED) {
                c->fin_pending = true;
                c->state = TCP_FIN_WAIT_1;
            } else if (c->state == TCP_CLOSE_WAIT) {
                c->fin_pending = true;
                c->state = TCP_LAST_ACK;
            } else if (c->state == TCP_SYN_SENT) {
                c->state = TCP_CLOSED;
            }
            irq_restore(f);
        }

        conn_pump(c);
    }
}

void tcp_init(void)
{
    for (int i = 0; i < TCP_CONNS; i++) {
        struct tcp_conn *c = &conns[i];
        c->used = false;
        c->detached = false;
        c->pumping = false;
        c->state = TCP_CLOSED;
        c->owner_pid = 0;
        c->tx_head = c->tx_len = 0;
        c->rx_head = c->rx_len = 0;
    }
    port_next = TCP_PORT_FIRST;
}

/* ---- public, blocking, task context ---- */

/* Reap slots left behind by TIME_WAIT or by a dead owner. Also a safety
 * net in case tcp_tick() has not been wired to a kernel thread. */
static void conn_gc(void)
{
    for (int i = 0; i < TCP_CONNS; i++) {
        struct tcp_conn *c = &conns[i];
        if (!c->used)
            continue;
        if (c->state == TCP_TIME_WAIT && timer_ticks() >= c->tw_deadline)
            c->state = TCP_CLOSED;
        if (c->owner_pid > 0 && !task_find(c->owner_pid))
            c->detached = true;
        if (c->detached && (c->state == TCP_CLOSED || c->reset))
            conn_release(c);
    }
}

int tcp_connect(uint32_t ip_be, uint16_t port, int timeout_ms)
{
    struct tcp_conn *c = 0;
    int handle = -1;

    if (!net_ready() || ip_be == 0 || port == 0)
        return -1;
    if (timeout_ms <= 0)
        timeout_ms = 5000;

    conn_gc();

    for (int i = 0; i < TCP_CONNS; i++) {
        if (!conns[i].used) {
            c = &conns[i];
            handle = i;
            break;
        }
    }
    if (!c) {
        kprintf("tcp: no free connection slot\n");
        return -1;
    }

    if (!c->rxbuf)
        c->rxbuf = kmalloc(TCP_RXBUF);
    if (!c->txbuf)
        c->txbuf = kmalloc(TCP_TXBUF);
    if (!c->rxbuf || !c->txbuf) {
        kprintf("tcp: out of memory for connection buffers\n");
        return -1;
    }

    uint16_t lport = port_alloc();
    if (lport == 0)
        return -1;

    /* ISN from the tick counter mixed with the local port: enough to keep
     * two connections on the same port pair from sharing a sequence space. */
    uint32_t isn = (uint32_t)(timer_ticks() * 62500u) ^
                   (((uint32_t)lport << 16) | (uint32_t)lport);

    uint64_t f = irq_save();
    c->state = TCP_SYN_SENT;
    c->detached = false;
    c->pumping = false;
    c->owner_pid = current ? current->pid : 0;
    c->peer_ip = ip_be;
    c->peer_port = port;
    c->local_port = lport;
    c->iss = isn;
    c->snd_una = isn;
    c->snd_nxt = isn + 1;             /* the SYN consumes one number */
    c->snd_wnd = TCP_MSS;             /* replaced by the SYN|ACK window */
    c->tx_head = c->tx_len = 0;
    c->fin_pending = c->fin_sent = false;
    c->fin_seq = 0;
    c->irs = c->rcv_nxt = 0;
    c->rx_head = c->rx_len = 0;
    c->peer_fin = c->need_ack = c->reset = false;
    c->rto_ticks = TCP_RTO_MIN;
    c->rto_deadline = 0;
    c->retries = 0;
    c->tw_deadline = 0;
    c->used = true;
    irq_restore(f);

    if (seg_send(c, TCP_SYN, isn, 0, 0, true) < 0) {
        conn_release(c);
        return -1;
    }

    /* Start the clock only now: seg_send() can spend seconds in ARP. */
    c->rto_deadline = timer_ticks() + c->rto_ticks;
    uint64_t deadline = timer_ticks() + (uint64_t)(timeout_ms + 9) / 10;

    for (;;) {
        if (c->state == TCP_ESTABLISHED) {
            conn_pump(c);             /* flush the handshake ACK */
            return handle;
        }
        if (c->reset || c->state == TCP_CLOSED)
            break;
        if (timer_ticks() >= deadline)
            break;
        conn_pump(c);                 /* SYN retransmission */
        net_wait_tick();
    }

    if (c->used && !c->reset)
        seg_send(c, TCP_RST, c->snd_nxt, 0, 0, false);
    conn_release(c);
    return -1;
}

int tcp_send(int handle, const void *buf, int len)
{
    struct tcp_conn *c = conn_get(handle);
    const uint8_t *src = buf;
    int done = 0;

    if (!c || !buf || len < 0)
        return -1;
    if (len == 0)
        return 0;
    if (c->reset || c->fin_pending)
        return -1;
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT)
        return -1;

    uint64_t deadline = timer_ticks() + TCP_SEND_TICKS;

    while (done < len) {
        if (c->reset || (c->state != TCP_ESTABLISHED &&
                         c->state != TCP_CLOSE_WAIT))
            return done > 0 ? done : -1;

        /* tx_head + tx_len is invariant under acknowledgement (head moves
         * up exactly as much as len shrinks), so the append position is
         * stable even if an ACK lands during the copy below. */
        uint64_t f = irq_save();
        int head = c->tx_head;
        int used = c->tx_len;
        irq_restore(f);

        int room = TCP_TXBUF - used;
        if (room > 0) {
            int n = len - done;
            if (n > room)
                n = room;
            int tail = (head + used) % TCP_TXBUF;
            int first = TCP_TXBUF - tail;
            if (first > n)
                first = n;
            memcpy(c->txbuf + tail, src + done, (size_t)first);
            if (n > first)
                memcpy(c->txbuf, src + done + first, (size_t)(n - first));

            f = irq_save();
            c->tx_len += n;
            irq_restore(f);
            done += n;
            conn_pump(c);
            continue;
        }

        conn_pump(c);
        if (timer_ticks() >= deadline)
            break;
        net_wait_tick();
    }

    return done > 0 ? done : -1;
}

int tcp_recv(int handle, void *buf, int max, int timeout_ms)
{
    struct tcp_conn *c = conn_get(handle);

    if (!c || !buf || max < 0)
        return -1;
    if (max == 0)
        return 0;
    if (timeout_ms < 0)
        timeout_ms = 0;

    uint64_t deadline = timer_ticks() + (uint64_t)(timeout_ms + 9) / 10;

    for (;;) {
        uint64_t f = irq_save();
        int avail = c->rx_len;
        int head = c->rx_head;
        bool fin = c->peer_fin;
        bool rst = c->reset;
        int state = c->state;
        irq_restore(f);

        if (avail > 0) {
            /* Safe to copy without masking: the IRQ producer only ever
             * writes past head+avail and only ever grows rx_len. */
            int n = avail < max ? avail : max;
            int first = TCP_RXBUF - head;
            if (first > n)
                first = n;
            memcpy(buf, c->rxbuf + head, (size_t)first);
            if (n > first)
                memcpy((uint8_t *)buf + first, c->rxbuf, (size_t)(n - first));

            f = irq_save();
            c->rx_head = (head + n) % TCP_RXBUF;
            c->rx_len -= n;
            c->need_ack = true;       /* window update */
            irq_restore(f);
            return n;
        }

        if (fin)
            return 0;                 /* peer closed, buffer drained */
        if (rst || state == TCP_CLOSED)
            return -1;

        conn_pump(c);
        if (timer_ticks() >= deadline)
            return -1;
        net_wait_tick();
    }
}

int tcp_close(int handle)
{
    struct tcp_conn *c = conn_get(handle);
    if (!c)
        return -1;

    uint64_t f = irq_save();
    c->detached = true;
    if (c->state == TCP_ESTABLISHED) {
        c->fin_pending = true;
        c->state = TCP_FIN_WAIT_1;
    } else if (c->state == TCP_CLOSE_WAIT) {
        c->fin_pending = true;
        c->state = TCP_LAST_ACK;
    } else if (c->state == TCP_SYN_SENT) {
        c->state = TCP_CLOSED;
    }
    irq_restore(f);

    /* Push the FIN out and wait briefly for the handshake to finish. What
     * is left (TIME_WAIT in particular) is reaped by tcp_tick/conn_gc. */
    uint64_t deadline = timer_ticks() + TCP_CLOSE_TICKS;
    while (timer_ticks() < deadline) {
        conn_pump(c);
        f = irq_save();
        bool done = !c->used || c->reset || c->state == TCP_CLOSED ||
                    c->state == TCP_TIME_WAIT;
        irq_restore(f);
        if (done)
            break;
        net_wait_tick();
    }

    f = irq_save();
    bool stuck = c->used && c->detached && !c->reset &&
                 c->state != TCP_CLOSED && c->state != TCP_TIME_WAIT;
    irq_restore(f);
    if (stuck) {
        /* The peer never answered our FIN: tear it down hard. */
        seg_send(c, TCP_RST, c->snd_nxt, 0, 0, false);
        conn_release(c);
    }
    return 0;
}
