/* Host receive-path regression; only privileged IRQ locking is replaced. */
#include <stdio.h>
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
uint64_t timer_ticks(void) { return 0; }

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
    return failures != 0;
}
