/* Host receive-path regression; only privileged IRQ locking is replaced. */
#include <stdio.h>
#include <stdlib.h>
#include "spinlock.h"

static uint64_t host_lock(spinlock_t *lock)
{
    spin_lock(lock);
    return 0;
}

static void host_unlock(spinlock_t *lock, uint64_t flags)
{
    (void)flags;
    spin_unlock(lock);
}

#define spin_lock_irqsave host_lock
#define spin_unlock_irqrestore host_unlock
#include "../kernel/tcp.c"

uint32_t net_ip_addr(void) { return htonl(0x0a00020f); }
static uint64_t now;
static int sent;
static struct tcp_hdr last_sent;
uint64_t timer_ticks(void) { return now; }

/* Stubs for kernel dependencies tcp.c grew after this test was written
 * (stats, krec snapshots, IPv6, scheduler hooks).  None run on the RST
 * receive path under test, but the PE linker needs them resolved. */
void net_stat_inc(int kind, uint32_t bytes) { (void)kind; (void)bytes; }
bool task_exists(int pid) { (void)pid; return true; }
bool net_ready(void) { return true; }
bool net6_source_for(const uint8_t dst[16], uint8_t src_out[16])
{
    (void)dst; memset(src_out, 0, 16); return false;
}
int net6_send(const uint8_t dst[16], uint8_t next_header,
              const void *payload, int len)
{
    (void)dst; (void)next_header; (void)payload; return len;
}
int net_ip_send(uint32_t dst_ip_be, uint8_t proto,
                const void *payload, int len)
{
    (void)dst_ip_be; (void)proto;
    memcpy(&last_sent, payload, sizeof(last_sent));
    sent++;
    return len;
}
void net_wait_tick(void) {}
uint32_t random_u32(void) { return 0x1a2b3c4d; }
void *kmalloc(size_t size) { return malloc(size); }
void kfree(void *ptr) { free(ptr); }
struct task *smp_current_task(void) { return NULL; }
void kprintf(const char *fmt, ...) { (void)fmt; }

void krec_kbuf_init(struct krec_kbuf *b, char *out, uint32_t cap)
{
    b->out = out; b->len = 0; b->cap = cap; b->overflow = false;
}
void krec_kbuf_putc(struct krec_kbuf *b, char c)
{
    if (b->len + 1 >= b->cap) { b->overflow = true; return; }
    b->out[b->len++] = c; b->out[b->len] = '\0';
}
void krec_kbuf_puts(struct krec_kbuf *b, const char *s)
{
    while (*s) krec_kbuf_putc(b, *s++);
}
void krec_kbuf_put_u64(struct krec_kbuf *b, uint64_t v)
{
    char tmp[24]; snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
    krec_kbuf_puts(b, tmp);
}
void krec_kbuf_put_i64(struct krec_kbuf *b, int64_t v)
{
    char tmp[24]; snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    krec_kbuf_puts(b, tmp);
}
void krec_kbuf_put_ip4(struct krec_kbuf *b, uint32_t ip_be)
{
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u",
             ip_be & 0xff, (ip_be >> 8) & 0xff,
             (ip_be >> 16) & 0xff, ip_be >> 24);
    krec_kbuf_puts(b, tmp);
}
uint32_t krec_kbuf_finish(struct krec_kbuf *b) { return b->len; }

static int failures;

static void check_rst(const char *name, uint32_t next, uint32_t seq,
                      int occupied, int reset, int challenge)
{
    static uint8_t rx[TCP_RXBUF];
    memset(conns, 0, sizeof(conns));
    struct tcp_conn *c = &conns[0];
    c->used = true;
    c->state = TCP_ESTABLISHED;
    c->peer_ip = htonl(0x0a000202);
    c->peer_port = 80;
    c->local_port = 50000;
    c->rxbuf = rx;
    c->rcv_nxt = next;
    c->rx_len = occupied;
    struct tcp_hdr packet = {0};
    packet.sport = htons(c->peer_port);
    packet.dport = htons(c->local_port);
    packet.seq = htonl(seq);
    packet.off = 5 << 4;
    packet.flags = TCP_RST;
    packet.csum = net_transport_checksum(c->peer_ip, net_ip_addr(),
                                         IP_PROTO_TCP, &packet, sizeof(packet));
    tcp_input(c->peer_ip, (const uint8_t *)&packet, sizeof(packet));
    int ok = c->reset == reset &&
             c->state == (reset ? TCP_CLOSED : TCP_ESTABLISHED) &&
             c->need_ack == challenge && c->rcv_nxt == next &&
             c->rx_len == occupied;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}

static void check_fin(const char *name, int state, uint32_t next,
                      int offset, uint8_t flags, int expected)
{
    static uint8_t rx[TCP_RXBUF];
    memset(conns, 0, sizeof(conns));
    struct tcp_conn *c = &conns[0];
    c->used = true;
    c->state = TCP_ESTABLISHED;
    c->peer_ip = htonl(0x0a000202);
    c->peer_port = 80;
    c->local_port = 50000;
    c->rxbuf = rx;
    c->rcv_nxt = next - 1;
    struct tcp_hdr packet = {0};
    packet.sport = htons(c->peer_port);
    packet.dport = htons(c->local_port);
    packet.seq = htonl(next - 1);
    packet.off = 5 << 4;
    packet.flags = TCP_FIN | TCP_ACK;
    packet.csum = net_transport_checksum(c->peer_ip, net_ip_addr(),
                                         IP_PROTO_TCP, &packet, sizeof(packet));
    tcp_input(c->peer_ip, (const uint8_t *)&packet, sizeof(packet));
    sent = 0;
    conn_pump(c);
    int ok = sent == 1 && c->peer_fin && c->rcv_nxt == next;
    c->state = state;
    now = 10;
    c->tw_deadline = now + TCP_TW_TICKS;
    uint64_t deadline = c->tw_deadline;
    now++;
    packet.seq = htonl(next + (uint32_t)offset);
    packet.flags = flags;
    packet.csum = 0;
    packet.csum = net_transport_checksum(c->peer_ip, net_ip_addr(),
                                         IP_PROTO_TCP, &packet, sizeof(packet));
    sent = 0;
    tcp_input(c->peer_ip, (const uint8_t *)&packet, sizeof(packet));
    conn_pump(c);
    ok = ok && sent == expected && c->rcv_nxt == next &&
         c->state == state && c->rx_len == 0 &&
         c->tw_deadline == (state == TCP_TIME_WAIT && expected ?
                            now + TCP_TW_TICKS : deadline);
    if (expected)
        ok = ok && last_sent.flags == TCP_ACK && ntohl(last_sent.ack) == next;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}

int main(void)
{
    check_rst("RST below receive window ignored", 10000, 9999, 0, 0, 0);
    check_rst("RST at right window edge ignored", 10000, 10000 + TCP_RXBUF, 0, 0, 0);
    check_rst("RST beyond receive window ignored", 10000, 10000 + TCP_RXBUF + 1, 0, 0, 0);
    check_rst("RST at RCV.NXT resets", 10000, 10000, 0, 1, 0);
    check_rst("in-window non-exact RST challenges", 10000, 10001, 0, 0, 1);
    check_rst("RST uses remaining receive capacity", 10000, 10002, TCP_RXBUF - 2, 0, 0);
    check_rst("wrapped in-window RST challenges", UINT32_MAX - 3, 1, 0, 0, 1);
    check_rst("wrapped old RST ignored", 3, UINT32_MAX, 0, 0, 0);
    check_rst("wrapped exact RST resets", 0, 0, 0, 1, 0);
    check_rst("zero-window non-exact RST ignored", 10000, 10001, TCP_RXBUF, 0, 0);
    check_rst("zero-window exact RST resets", 10000, 10000, TCP_RXBUF, 1, 0);
    check_fin("CLOSE_WAIT re-ACKs consumed FIN", TCP_CLOSE_WAIT, 10000, -1, TCP_FIN | TCP_ACK, 1);
    check_fin("LAST_ACK re-ACKs consumed FIN", TCP_LAST_ACK, 10000, -1, TCP_FIN | TCP_ACK, 1);
    check_fin("FIN_WAIT_1 re-ACKs consumed FIN", TCP_FIN_WAIT_1, 10000, -1, TCP_FIN | TCP_ACK, 1);
    check_fin("TIME_WAIT re-ACKs FIN and restarts linger", TCP_TIME_WAIT, 10000, -1, TCP_FIN | TCP_ACK, 1);
    check_fin("wrapped consumed FIN re-ACKed", TCP_CLOSE_WAIT, 0, -1, TCP_FIN | TCP_ACK, 1);
    check_fin("unrelated old FIN ignored", TCP_TIME_WAIT, 10000, -2, TCP_FIN | TCP_ACK, 0);
    check_fin("future FIN ignored after close", TCP_CLOSE_WAIT, 10000, 1, TCP_FIN | TCP_ACK, 0);
    check_fin("empty ACK does not restart linger", TCP_TIME_WAIT, 10000, -1, TCP_ACK, 0);
    return failures != 0;
}
