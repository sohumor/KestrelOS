#include "kernel.h"
#include "string.h"
#include "timer.h"
#include "proc.h"
#include "kheap.h"
#include "net.h"
#include "tcp.h"
#include "tcp_reassembly.h"
#include "spinlock.h"

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
 * Everything touched by both contexts is guarded by tcp_lock. Merely
 * disabling local interrupts is insufficient once tasks can run on other
 * CPUs. Buffers are kmalloc'd once per slot in task context and kept for
 * reuse, so the IRQ path never allocates.
 */

#define IP_PROTO_TCP  6

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

#define TCP_OPT_END            0
#define TCP_OPT_NOP            1
#define TCP_OPT_MSS            2
#define TCP_OPT_SACK_PERMITTED 4
#define TCP_OPT_SACK           5
#define TCP_MAX_SACK_BLOCKS    4

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
    uint32_t snd_max;      /* highest sequence ever transmitted */
    uint32_t snd_wnd;      /* peer's advertised window */
    uint8_t *txbuf;        /* ring, holds tx_len bytes starting at snd_una */
    int      tx_head;
    int      tx_len;
    struct tcp_sack_block tx_sack[TCP_MAX_SACK_BLOCKS];
    int      tx_sack_count;
    int      dup_acks;
    bool     fast_retransmit;
    bool     sack_enabled;
    bool     fin_pending;  /* application asked to close */
    bool     fin_sent;
    uint32_t fin_seq;      /* sequence number our FIN occupies */

    /* receive side */
    uint32_t irs;
    uint32_t rcv_nxt;
    uint8_t *rxbuf;        /* ring */
    int      rx_head;
    int      rx_len;
    struct tcp_reassembly reassembly;
    bool     peer_fin;
    bool     fin_queued;
    uint32_t fin_rcv_seq;
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
static spinlock_t tcp_lock = SPINLOCK_INIT;

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

/* ---- helpers ---- */

/* Free space in the receive ring, i.e. the window we advertise. */
static uint16_t rcv_window(struct tcp_conn *c)
{
    uint64_t f = spin_lock_irqsave(&tcp_lock);
    int freebytes = TCP_RXBUF - c->rx_len;
    spin_unlock_irqrestore(&tcp_lock, f);
    if (freebytes < 0)
        freebytes = 0;
    if (freebytes > 65535)
        freebytes = 65535;
    return (uint16_t)freebytes;
}

/* Task context only (net_ip_send may block in ARP). For a data segment,
 * derive the ring offset from the latest cumulative ACK while holding the
 * lock; an ACK may advance tx_head between the pump's plan and this copy. */
static int seg_send(struct tcp_conn *c, uint8_t flags, uint32_t seq,
                    int dlen, bool syn_options)
{
    uint8_t pkt[sizeof(struct tcp_hdr) + 40 + TCP_MSS];
    struct tcp_hdr *th = (struct tcp_hdr *)pkt;
    int optlen = 0;

    if (dlen < 0 || dlen > TCP_MSS)
        return -1;

    if (syn_options) {
        /* MSS, SACK-Permitted, then two NOPs to align the header. */
        pkt[sizeof(*th) + optlen++] = TCP_OPT_MSS;
        pkt[sizeof(*th) + optlen++] = 4;
        pkt[sizeof(*th) + optlen++] = (uint8_t)(TCP_MSS >> 8);
        pkt[sizeof(*th) + optlen++] = (uint8_t)(TCP_MSS & 0xFF);
        pkt[sizeof(*th) + optlen++] = TCP_OPT_SACK_PERMITTED;
        pkt[sizeof(*th) + optlen++] = 2;
        pkt[sizeof(*th) + optlen++] = TCP_OPT_NOP;
        pkt[sizeof(*th) + optlen++] = TCP_OPT_NOP;
    } else if ((flags & TCP_ACK) && c->sack_enabled) {
        struct tcp_sack_block blocks[TCP_MAX_SACK_BLOCKS];
        uint64_t sf = spin_lock_irqsave(&tcp_lock);
        int n = tcp_reassembly_sack_blocks(&c->reassembly, c->rx_head,
                                           c->rx_len, c->rcv_nxt, blocks,
                                           TCP_MAX_SACK_BLOCKS);
        spin_unlock_irqrestore(&tcp_lock, sf);
        if (n > 0) {
            /* Two NOPs make 2 + (2 + 8*n) a 32-bit multiple. */
            pkt[sizeof(*th) + optlen++] = TCP_OPT_NOP;
            pkt[sizeof(*th) + optlen++] = TCP_OPT_NOP;
            pkt[sizeof(*th) + optlen++] = TCP_OPT_SACK;
            pkt[sizeof(*th) + optlen++] = (uint8_t)(2 + n * 8);
            for (int i = 0; i < n; i++) {
                uint32_t left = htonl(blocks[i].left);
                uint32_t right = htonl(blocks[i].right);
                memcpy(pkt + sizeof(*th) + optlen, &left, 4);
                optlen += 4;
                memcpy(pkt + sizeof(*th) + optlen, &right, 4);
                optlen += 4;
            }
        }
    }

    int hlen = (int)sizeof(*th) + optlen;
    uint8_t *payload = pkt + hlen;

    th->sport = htons(c->local_port);
    th->dport = htons(c->peer_port);
    th->off = (uint8_t)((hlen / 4) << 4);
    th->flags = flags;
    th->win = htons(rcv_window(c));
    th->csum = 0;
    th->urg = 0;

    /* Snapshot the ring with interrupts masked: an ACK arriving mid-copy
     * would advance tx_head and let a concurrent tcp_send() overwrite the
     * bytes we are still reading. At most one MSS, so the window is short. */
    uint64_t f = spin_lock_irqsave(&tcp_lock);
    if (dlen > 0 && seq_lt(seq, c->snd_una)) {
        uint32_t already_acked = c->snd_una - seq;
        if (already_acked >= (uint32_t)dlen) {
            spin_unlock_irqrestore(&tcp_lock, f);
            return 0;
        }
        seq += already_acked;
        dlen -= (int)already_acked;
    }
    th->ack = (flags & TCP_ACK) ? htonl(c->rcv_nxt) : 0;
    if (dlen > 0) {
        int txoff = (int)(seq - c->snd_una);
        if (txoff < 0 || txoff + dlen > c->tx_len) {
            spin_unlock_irqrestore(&tcp_lock, f);
            return -1;
        }
        int pos = (c->tx_head + txoff) % TCP_TXBUF;
        int first = TCP_TXBUF - pos;
        if (first > dlen)
            first = dlen;
        memcpy(payload, c->txbuf + pos, (size_t)first);
        if (dlen > first)
            memcpy(payload + first, c->txbuf, (size_t)(dlen - first));
    }
    th->seq = htonl(seq);
    spin_unlock_irqrestore(&tcp_lock, f);

    th->csum = net_transport_checksum(net_ip_addr(), c->peer_ip,
                                      IP_PROTO_TCP, pkt, hlen + dlen);
    return net_ip_send(c->peer_ip, IP_PROTO_TCP, pkt, hlen + dlen);
}

/* Task context. Buffers are deliberately kept for reuse by the next
 * connection so that the IRQ path never has to allocate. */
static void conn_release_locked(struct tcp_conn *c)
{
    c->used = false;
    c->detached = false;
    c->state = TCP_CLOSED;
    c->owner_pid = 0;
    c->peer_ip = 0;
    c->peer_port = 0;
    c->local_port = 0;
    c->tx_head = c->tx_len = 0;
    c->tx_sack_count = c->dup_acks = 0;
    c->fast_retransmit = c->sack_enabled = false;
    c->rx_head = c->rx_len = 0;
    c->fin_pending = c->fin_sent = false;
    c->peer_fin = c->fin_queued = c->need_ack = c->reset = false;
    c->rto_deadline = 0;
    c->retries = 0;
}

static void conn_release(struct tcp_conn *c)
{
    uint64_t f = spin_lock_irqsave(&tcp_lock);
    if (c->pumping) {
        c->detached = true;
        c->state = TCP_CLOSED;
    } else {
        conn_release_locked(c);
    }
    spin_unlock_irqrestore(&tcp_lock, f);
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
    uint64_t f = spin_lock_irqsave(&tcp_lock);
    struct tcp_conn *c = &conns[handle];
    if (!c->used || c->detached) {
        spin_unlock_irqrestore(&tcp_lock, f);
        return 0;
    }
    if (c->owner_pid > 0 && current && current->pid != c->owner_pid) {
        spin_unlock_irqrestore(&tcp_lock, f);
        return 0;
    }
    spin_unlock_irqrestore(&tcp_lock, f);
    return c;
}

/* ---- IRQ side ---- */

static void sack_prune(struct tcp_conn *c, uint32_t ack)
{
    int out = 0;
    for (int i = 0; i < c->tx_sack_count; i++) {
        struct tcp_sack_block b = c->tx_sack[i];
        if (seq_leq(b.right, ack))
            continue;
        if (seq_lt(b.left, ack))
            b.left = ack;
        c->tx_sack[out++] = b;
    }
    c->tx_sack_count = out;
}

static void process_ack(struct tcp_conn *c, uint32_t ack, uint16_t win)
{
    c->snd_wnd = win;

    if (seq_leq(ack, c->snd_una) || seq_lt(c->snd_max, ack))
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
    if (seq_lt(c->snd_nxt, ack))
        c->snd_nxt = ack;
    sack_prune(c, ack);
    c->dup_acks = 0;
    c->fast_retransmit = false;

    c->retries = 0;
    c->rto_ticks = TCP_RTO_MIN;
    c->rto_deadline = (c->snd_una != c->snd_max)
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

static int parse_options(const uint8_t *seg, int hlen,
                         int *sack_permitted,
                         struct tcp_sack_block *blocks, int max_blocks)
{
    int count = 0;
    int off = (int)sizeof(struct tcp_hdr);
    if (sack_permitted)
        *sack_permitted = 0;

    while (off < hlen) {
        uint8_t kind = seg[off];
        if (kind == TCP_OPT_END)
            break;
        if (kind == TCP_OPT_NOP) {
            off++;
            continue;
        }
        if (off + 2 > hlen)
            break;
        int olen = seg[off + 1];
        if (olen < 2 || off + olen > hlen)
            break;
        if (kind == TCP_OPT_SACK_PERMITTED && olen == 2 &&
            sack_permitted)
            *sack_permitted = 1;
        if (kind == TCP_OPT_SACK && olen >= 10 &&
            ((olen - 2) % 8) == 0 && blocks) {
            for (int p = off + 2; p + 7 < off + olen &&
                                      count < max_blocks; p += 8) {
                uint32_t left, right;
                memcpy(&left, seg + p, 4);
                memcpy(&right, seg + p + 4, 4);
                blocks[count].left = ntohl(left);
                blocks[count].right = ntohl(right);
                count++;
            }
        }
        off += olen;
    }
    return count;
}

static void process_sack(struct tcp_conn *c, uint32_t ack,
                         const struct tcp_sack_block *blocks, int count)
{
    c->tx_sack_count = 0;
    if (!c->sack_enabled || seq_lt(ack, c->snd_una) ||
        seq_lt(c->snd_max, ack))
        return;

    for (int i = 0; i < count; i++) {
        uint32_t left = blocks[i].left;
        uint32_t right = blocks[i].right;
        if (!seq_lt(left, right) || seq_lt(left, ack) ||
            seq_lt(c->snd_max, right))
            continue;
        int pos = c->tx_sack_count;
        while (pos > 0 && seq_lt(left, c->tx_sack[pos - 1].left)) {
            c->tx_sack[pos] = c->tx_sack[pos - 1];
            pos--;
        }
        c->tx_sack[pos].left = left;
        c->tx_sack[pos].right = right;
        if (c->tx_sack_count < TCP_MAX_SACK_BLOCKS)
            c->tx_sack_count++;
    }

    /* Three duplicate cumulative ACKs with a SACK range beyond the hole
     * trigger selective fast retransmission from snd_una. */
    if (ack == c->snd_una && c->tx_sack_count > 0) {
        if (++c->dup_acks >= 3) {
            c->fast_retransmit = true;
            c->dup_acks = 0;
        }
    }
}

static uint32_t skip_sacked(struct tcp_conn *c, uint32_t seq)
{
    for (int i = 0; i < c->tx_sack_count; i++)
        if (!seq_lt(seq, c->tx_sack[i].left) &&
            seq_lt(seq, c->tx_sack[i].right))
            return c->tx_sack[i].right;
    return seq;
}

static int bytes_before_sack(struct tcp_conn *c, uint32_t seq, int n)
{
    for (int i = 0; i < c->tx_sack_count; i++) {
        if (seq_lt(seq, c->tx_sack[i].left)) {
            uint32_t gap = c->tx_sack[i].left - seq;
            if (gap < (uint32_t)n)
                return (int)gap;
            break;
        }
    }
    return n;
}

static void accept_data(struct tcp_conn *c, uint32_t seq,
                        const uint8_t *data, int dlen)
{
    if (c->peer_fin) {
        c->need_ack = true;
        return;
    }

    if (c->fin_queued) {
        int32_t before_fin = (int32_t)(c->fin_rcv_seq - seq);
        if (before_fin <= 0) {
            c->need_ack = true;
            return;
        }
        if (dlen > before_fin)
            dlen = before_fin;
    }

    tcp_reassembly_accept(&c->reassembly, c->rxbuf, c->rx_head,
                          &c->rx_len, &c->rcv_nxt, seq, data, dlen);
    c->need_ack = true;
}

static void accept_peer_fin(struct tcp_conn *c)
{
    c->rcv_nxt++;
    c->peer_fin = true;
    c->fin_queued = false;
    c->need_ack = true;
    if (c->state == TCP_ESTABLISHED) {
        c->state = TCP_CLOSE_WAIT;
    } else if (c->state == TCP_FIN_WAIT_2) {
        c->state = TCP_TIME_WAIT;
        c->tw_deadline = timer_ticks() + TCP_TW_TICKS;
    }
    /* FIN_WAIT_1: stay put; process_ack() promotes us to TIME_WAIT once
     * our own FIN is acknowledged. */
}

void tcp_input(uint32_t src_ip_be, const uint8_t *seg, int len)
{
    if (!seg || len < (int)sizeof(struct tcp_hdr))
        return;

    const struct tcp_hdr *th = (const struct tcp_hdr *)seg;
    int hlen = (th->off >> 4) * 4;
    if (hlen < (int)sizeof(*th) || hlen > len)
        return;
    if (net_transport_checksum(src_ip_be, net_ip_addr(), IP_PROTO_TCP,
                               seg, len) != 0)
        return;

    uint64_t lock_flags = spin_lock_irqsave(&tcp_lock);
    struct tcp_conn *c = conn_lookup(src_ip_be, ntohs(th->sport),
                                     ntohs(th->dport));
    if (!c || !c->rxbuf)
        goto out;

    uint32_t seq = ntohl(th->seq);
    uint32_t ack = ntohl(th->ack);
    uint8_t fl = th->flags;
    const uint8_t *data = seg + hlen;
    int dlen = len - hlen;

    if (fl & TCP_RST) {
        /* An unacknowledged RST during the handshake is not for us. */
        if (c->state == TCP_SYN_SENT && !(fl & TCP_ACK))
            goto out;
        c->reset = true;
        c->state = TCP_CLOSED;
        goto out;
    }

    if (c->state == TCP_SYN_SENT) {
        if ((fl & (TCP_SYN | TCP_ACK)) != (TCP_SYN | TCP_ACK))
            goto out;
        if (ack != c->snd_nxt)        /* not the ACK of our SYN */
            goto out;
        c->irs = seq;
        c->rcv_nxt = seq + 1;         /* the SYN consumes one number */
        c->snd_una = ack;
        c->snd_nxt = ack;
        c->snd_max = ack;
        c->snd_wnd = ntohs(th->win);
        int permitted = 0;
        parse_options(seg, hlen, &permitted, NULL, 0);
        c->sack_enabled = permitted != 0;
        c->state = TCP_ESTABLISHED;
        c->need_ack = true;
        c->retries = 0;
        c->rto_ticks = TCP_RTO_MIN;
        c->rto_deadline = 0;
        goto out;                     /* data on a SYN|ACK is not accepted */
    }

    if (fl & TCP_SYN)                 /* stray SYN on a live connection */
        goto out;

    if (fl & TCP_ACK) {
        struct tcp_sack_block blocks[TCP_MAX_SACK_BLOCKS];
        int nsack = parse_options(seg, hlen, NULL, blocks,
                                  TCP_MAX_SACK_BLOCKS);
        process_ack(c, ack, ntohs(th->win));
        process_sack(c, ack, blocks, nsack);
    }

    /* Remember an in-window FIN even when data in front of it is missing.
     * A later segment which fills the hole can then complete the close
     * without waiting for the peer's FIN retransmission timer. */
    uint32_t fin_seq = seq + (uint32_t)dlen;
    if ((fl & TCP_FIN) && !c->peer_fin) {
        int32_t delta = (int32_t)(fin_seq - c->rcv_nxt);
        int window = TCP_RXBUF - c->rx_len;
        if (delta >= 0 && delta < window &&
            (!c->fin_queued || c->fin_rcv_seq == fin_seq)) {
            c->fin_queued = true;
            c->fin_rcv_seq = fin_seq;
            tcp_reassembly_discard_from(&c->reassembly, c->rx_head,
                                        c->rx_len, c->rcv_nxt, fin_seq);
            c->need_ack = true;
        }
    }

    if (dlen > 0)
        accept_data(c, seq, data, dlen);

    if (c->fin_queued && c->fin_rcv_seq == c->rcv_nxt)
        accept_peer_fin(c);

out:
    spin_unlock_irqrestore(&tcp_lock, lock_flags);
}

/* ---- task side: the transmit pump ---- */

static void conn_pump(struct tcp_conn *c)
{
    uint64_t f = spin_lock_irqsave(&tcp_lock);
    if (!c->used || c->pumping) {
        spin_unlock_irqrestore(&tcp_lock, f);
        return;
    }
    c->pumping = true;
    spin_unlock_irqrestore(&tcp_lock, f);

    uint64_t now = timer_ticks();
    bool syn_send = false;
    bool timed_out = false;
    uint16_t timeout_port = 0;
    int timeout_retries = 0;
    uint32_t syn_seq = 0;

    f = spin_lock_irqsave(&tcp_lock);
    if (c->reset || c->state == TCP_CLOSED) {
        spin_unlock_irqrestore(&tcp_lock, f);
        goto out;
    }

    if (c->state == TCP_TIME_WAIT) {
        if (now >= c->tw_deadline)
            c->state = TCP_CLOSED;
        spin_unlock_irqrestore(&tcp_lock, f);
        goto out;
    }

    if (c->state == TCP_SYN_SENT) {
        if (c->rto_deadline && now >= c->rto_deadline) {
            if (c->retries >= TCP_MAX_TRIES) {
                timeout_port = c->peer_port;
                c->reset = true;
                c->state = TCP_CLOSED;
                timed_out = true;
            } else {
                c->retries++;
                if (c->rto_ticks < TCP_RTO_MAX)
                    c->rto_ticks *= 2;
                c->rto_deadline = timer_ticks() + c->rto_ticks;
                syn_seq = c->iss;
                syn_send = true;
            }
        }
        spin_unlock_irqrestore(&tcp_lock, f);
        if (timed_out)
            kprintf("tcp: connect to port %u timed out\n", timeout_port);
        else if (syn_send)
            seg_send(c, TCP_SYN, syn_seq, 0, true);
        goto out;
    }

    /* Fast/RTO retransmission walks from snd_una but skips every range the
     * peer has reported in SACK blocks. */
    if (c->fast_retransmit) {
        c->snd_nxt = c->snd_una;
        c->fast_retransmit = false;
        c->rto_deadline = timer_ticks() + c->rto_ticks;
    } else if (c->snd_una != c->snd_max && c->rto_deadline &&
               now >= c->rto_deadline) {
        if (c->retries >= TCP_MAX_TRIES) {
            timeout_retries = c->retries;
            c->reset = true;
            c->state = TCP_CLOSED;
            timed_out = true;
        } else {
            c->retries++;
            if (c->rto_ticks < TCP_RTO_MAX)
                c->rto_ticks *= 2;
            c->snd_nxt = c->snd_una;  /* fin_seq is unchanged by this */
            c->rto_deadline = timer_ticks() + c->rto_ticks;
        }
    }
    spin_unlock_irqrestore(&tcp_lock, f);
    if (timed_out) {
        kprintf("tcp: giving up after %d retransmits\n", timeout_retries);
        goto out;
    }

    bool sent_any = false;

    for (int i = 0; i < 8; i++) {
        f = spin_lock_irqsave(&tcp_lock);
        uint32_t una = c->snd_una;
        uint32_t nxt = c->snd_nxt;
        uint32_t skipped = skip_sacked(c, nxt);
        if (skipped != nxt) {
            c->snd_nxt = skipped;
            nxt = skipped;
        }
        int off = (int)(nxt - una);
        int avail = c->tx_len - off;
        int allow = (int)c->snd_wnd - off;
        int n = 0;
        bool stop = avail <= 0;
        if (allow <= 0) {
            /* Zero window. Probe with a single byte once per RTO so the
             * peer's window update cannot be lost silently. */
            if (c->rto_deadline && now < c->rto_deadline)
                stop = true;
            else
                allow = 1;
        }

        if (!stop) {
            n = avail < allow ? avail : allow;
            if (n > TCP_MSS)
                n = TCP_MSS;
            /* A four-block SACK option consumes 36 TCP option bytes. Keep
             * the whole IP packet within the Ethernet MTU even when SACK
             * information is piggybacked on data. */
            if (c->sack_enabled && n > TCP_MSS - 36)
                n = TCP_MSS - 36;
            n = bytes_before_sack(c, nxt, n);
        }
        if (n > 0) {
            /* Publish the sequence range before ringing the NIC doorbell:
             * another CPU may process its ACK immediately. */
            c->snd_nxt = nxt + (uint32_t)n;
            if (seq_lt(c->snd_max, c->snd_nxt))
                c->snd_max = c->snd_nxt;
            c->need_ack = false;
            if (una == nxt)
                c->rto_deadline = timer_ticks() + c->rto_ticks;
        }
        spin_unlock_irqrestore(&tcp_lock, f);

        if (stop)
            break;
        if (n <= 0)
            continue;
        if (seg_send(c, TCP_ACK | TCP_PSH, nxt, n, false) < 0) {
            f = spin_lock_irqsave(&tcp_lock);
            c->need_ack = true;
            spin_unlock_irqrestore(&tcp_lock, f);
            break;
        }
        sent_any = true;
    }

    /* The FIN goes out only once every queued byte has been transmitted;
     * fin_seq is snd_una + tx_len, which an ACK never changes, so a
     * retransmitted FIN always carries the same sequence number. */
    f = spin_lock_irqsave(&tcp_lock);
    uint32_t fseq = c->snd_nxt;
    bool fin_now = c->fin_pending &&
                   c->snd_nxt == c->snd_una + (uint32_t)c->tx_len;
    if (fin_now) {
        c->fin_seq = fseq;
        c->fin_sent = true;
        c->snd_nxt = fseq + 1;
        if (seq_lt(c->snd_max, c->snd_nxt))
            c->snd_max = c->snd_nxt;
        c->need_ack = false;
        c->rto_deadline = timer_ticks() + c->rto_ticks;
    }
    spin_unlock_irqrestore(&tcp_lock, f);

    if (fin_now) {
        if (seg_send(c, TCP_ACK | TCP_FIN, fseq, 0, false) < 0) {
            f = spin_lock_irqsave(&tcp_lock);
            c->need_ack = true;
            spin_unlock_irqrestore(&tcp_lock, f);
        } else {
            sent_any = true;
        }
    }

    f = spin_lock_irqsave(&tcp_lock);
    bool ack_now = !sent_any && c->need_ack;
    uint32_t ack_seq = c->snd_nxt;
    if (ack_now)
        c->need_ack = false;
    spin_unlock_irqrestore(&tcp_lock, f);
    if (ack_now && seg_send(c, TCP_ACK, ack_seq, 0, false) < 0) {
        f = spin_lock_irqsave(&tcp_lock);
        c->need_ack = true;
        spin_unlock_irqrestore(&tcp_lock, f);
    }

out:
    f = spin_lock_irqsave(&tcp_lock);
    c->pumping = false;
    if (c->detached && (c->state == TCP_CLOSED || c->reset))
        conn_release_locked(c);
    spin_unlock_irqrestore(&tcp_lock, f);
}

void tcp_tick(void)
{
    for (int i = 0; i < TCP_CONNS; i++) {
        struct tcp_conn *c = &conns[i];
        uint64_t f = spin_lock_irqsave(&tcp_lock);
        bool used = c->used;
        bool detached = c->detached;
        int owner = c->owner_pid;
        spin_unlock_irqrestore(&tcp_lock, f);
        if (!used)
            continue;

        /* Reclaim connections whose owning task died without closing. */
        if (!detached && owner > 0 && !task_exists(owner)) {
            f = spin_lock_irqsave(&tcp_lock);
            if (c->used && !c->detached && c->owner_pid == owner) {
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
            }
            spin_unlock_irqrestore(&tcp_lock, f);
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
        tcp_reassembly_reset(&c->reassembly);
        c->peer_fin = c->fin_queued = false;
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
        uint64_t f = spin_lock_irqsave(&tcp_lock);
        bool used = c->used;
        int owner = c->owner_pid;
        spin_unlock_irqrestore(&tcp_lock, f);
        if (!used)
            continue;
        bool owner_dead = owner > 0 && !task_exists(owner);
        f = spin_lock_irqsave(&tcp_lock);
        if (c->used && c->owner_pid == owner) {
            if (c->state == TCP_TIME_WAIT &&
                timer_ticks() >= c->tw_deadline)
                c->state = TCP_CLOSED;
            if (owner_dead)
                c->detached = true;
            if (!c->pumping && c->detached &&
                (c->state == TCP_CLOSED || c->reset))
                conn_release_locked(c);
        }
        spin_unlock_irqrestore(&tcp_lock, f);
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

    uint64_t f = spin_lock_irqsave(&tcp_lock);
    for (int i = 0; i < TCP_CONNS; i++) {
        if (!conns[i].used) {
            c = &conns[i];
            handle = i;
            c->used = true;       /* reserve before allocation can sleep */
            c->detached = false;
            c->pumping = false;
            c->state = TCP_CLOSED;
            c->owner_pid = current ? current->pid : 0;
            break;
        }
    }
    spin_unlock_irqrestore(&tcp_lock, f);
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
        conn_release(c);
        return -1;
    }

    f = spin_lock_irqsave(&tcp_lock);
    uint16_t lport = port_alloc();
    spin_unlock_irqrestore(&tcp_lock, f);
    if (lport == 0) {
        conn_release(c);
        return -1;
    }

    /* ISN from the tick counter mixed with the local port: enough to keep
     * two connections on the same port pair from sharing a sequence space. */
    uint32_t isn = (uint32_t)(timer_ticks() * 62500u) ^
                   (((uint32_t)lport << 16) | (uint32_t)lport);

    f = spin_lock_irqsave(&tcp_lock);
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
    c->snd_max = c->snd_nxt;
    c->snd_wnd = TCP_MSS;             /* replaced by the SYN|ACK window */
    c->tx_head = c->tx_len = 0;
    c->tx_sack_count = c->dup_acks = 0;
    c->fast_retransmit = false;
    c->sack_enabled = false;
    c->fin_pending = c->fin_sent = false;
    c->fin_seq = 0;
    c->irs = c->rcv_nxt = 0;
    c->rx_head = c->rx_len = 0;
    tcp_reassembly_reset(&c->reassembly);
    c->peer_fin = c->fin_queued = c->need_ack = c->reset = false;
    c->rto_ticks = TCP_RTO_MIN;
    c->rto_deadline = 0;
    c->retries = 0;
    c->tw_deadline = 0;
    spin_unlock_irqrestore(&tcp_lock, f);

    if (seg_send(c, TCP_SYN, isn, 0, true) < 0) {
        conn_release(c);
        return -1;
    }

    /* Start the clock only now: seg_send() can spend seconds in ARP. */
    f = spin_lock_irqsave(&tcp_lock);
    c->rto_deadline = timer_ticks() + c->rto_ticks;
    spin_unlock_irqrestore(&tcp_lock, f);
    uint64_t deadline = timer_ticks() + (uint64_t)(timeout_ms + 9) / 10;

    for (;;) {
        f = spin_lock_irqsave(&tcp_lock);
        int state = c->state;
        bool reset = c->reset;
        spin_unlock_irqrestore(&tcp_lock, f);
        if (state == TCP_ESTABLISHED) {
            conn_pump(c);             /* flush the handshake ACK */
            return handle;
        }
        if (reset || state == TCP_CLOSED)
            break;
        if (timer_ticks() >= deadline)
            break;
        conn_pump(c);                 /* SYN retransmission */
        net_wait_tick();
    }

    f = spin_lock_irqsave(&tcp_lock);
    bool send_reset = c->used && !c->reset;
    uint32_t reset_seq = c->snd_nxt;
    spin_unlock_irqrestore(&tcp_lock, f);
    if (send_reset)
        seg_send(c, TCP_RST, reset_seq, 0, false);
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
    uint64_t f = spin_lock_irqsave(&tcp_lock);
    bool unavailable = c->reset || c->fin_pending ||
                       (c->state != TCP_ESTABLISHED &&
                        c->state != TCP_CLOSE_WAIT);
    spin_unlock_irqrestore(&tcp_lock, f);
    if (unavailable)
        return -1;

    uint64_t deadline = timer_ticks() + TCP_SEND_TICKS;

    while (done < len) {
        /* tx_head + tx_len is invariant under acknowledgement (head moves
         * up exactly as much as len shrinks), so the append position is
         * stable even if an ACK lands during the copy below. */
        f = spin_lock_irqsave(&tcp_lock);
        bool stopped = c->reset || (c->state != TCP_ESTABLISHED &&
                                    c->state != TCP_CLOSE_WAIT);
        int head = c->tx_head;
        int used = c->tx_len;
        spin_unlock_irqrestore(&tcp_lock, f);
        if (stopped)
            return done > 0 ? done : -1;

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

            f = spin_lock_irqsave(&tcp_lock);
            c->tx_len += n;
            spin_unlock_irqrestore(&tcp_lock, f);
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
        uint64_t f = spin_lock_irqsave(&tcp_lock);
        int avail = c->rx_len;
        int head = c->rx_head;
        bool fin = c->peer_fin;
        bool rst = c->reset;
        int state = c->state;
        spin_unlock_irqrestore(&tcp_lock, f);

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

            f = spin_lock_irqsave(&tcp_lock);
            c->rx_head = (head + n) % TCP_RXBUF;
            c->rx_len -= n;
            c->need_ack = true;       /* window update */
            spin_unlock_irqrestore(&tcp_lock, f);
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

    uint64_t f = spin_lock_irqsave(&tcp_lock);
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
    spin_unlock_irqrestore(&tcp_lock, f);

    /* Push the FIN out and wait briefly for the handshake to finish. What
     * is left (TIME_WAIT in particular) is reaped by tcp_tick/conn_gc. */
    uint64_t deadline = timer_ticks() + TCP_CLOSE_TICKS;
    while (timer_ticks() < deadline) {
        conn_pump(c);
        f = spin_lock_irqsave(&tcp_lock);
        bool done = !c->used || c->reset || c->state == TCP_CLOSED ||
                    c->state == TCP_TIME_WAIT;
        spin_unlock_irqrestore(&tcp_lock, f);
        if (done)
            break;
        net_wait_tick();
    }

    f = spin_lock_irqsave(&tcp_lock);
    bool stuck = c->used && c->detached && !c->reset &&
                 c->state != TCP_CLOSED && c->state != TCP_TIME_WAIT;
    spin_unlock_irqrestore(&tcp_lock, f);
    if (stuck) {
        /* The peer never answered our FIN: tear it down hard. */
        f = spin_lock_irqsave(&tcp_lock);
        uint32_t reset_seq = c->snd_nxt;
        spin_unlock_irqrestore(&tcp_lock, f);
        seg_send(c, TCP_RST, reset_seq, 0, false);
        conn_release(c);
    }
    return 0;
}
