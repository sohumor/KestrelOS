#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "kestrel_abi.h"

/* KestrelOS network stack: ethernet / ARP / IPv4 / ICMP / UDP + DNS.
 * All IPv4 addresses are big-endian (network order) u32 values. */

/* ---- byte order ---- */

static inline uint16_t htons(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint16_t ntohs(uint16_t v)
{
    return htons(v);
}

static inline uint32_t htonl(uint32_t v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}

static inline uint32_t ntohl(uint32_t v)
{
    return htonl(v);
}

/* ---- public API ---- */

void net_init(void);
bool net_ready(void);
void net_get_info(struct k_netinfo *out);
void net_configure(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns);

/* Resolve a hostname (or dotted quad) to an IPv4 address. 0 / -1. */
int  dns_resolve(const char *name, uint32_t *ip_be);

/* ICMP echo; returns round-trip time in ms (10ms granularity) or -1. */
long icmp_ping(uint32_t ip_be, int timeout_ms);

int  udp_send(uint32_t ip_be, uint16_t sport, uint16_t dport,
              const void *buf, int len);
/* Returns payload length or -1 on timeout/error. Binds port if needed. */
int  udp_recv(uint16_t port, void *buf, int maxlen, int timeout_ms);

/* Drain NIC RX ring; safe to call anywhere, no-op when no NIC. */
void net_poll(void);

/* ---- stack internals (net.c / udp.c / dns.c / rtl8139.c) ---- */

#define ETH_MTU          1500
#define NET_UDP_MAX      1472   /* ETH_MTU - 20 (ip) - 8 (udp) */

/* Frame handoff from the NIC driver; runs in IRQ context — must not sleep. */
void net_rx(const uint8_t *frame, int len);

/* Build+send one IPv4 packet (resolves the MAC via ARP or the cache). */
int  net_ip_send(uint32_t dst_ip_be, uint8_t proto,
                 const void *payload, int len);

/* Internet checksum, returned ready to store in a header field. */
uint16_t net_checksum(const void *data, int len);

uint32_t net_ip_addr(void);
uint32_t net_dns_addr(void);

/* Poll + give up the CPU for ~1 tick (task context wait loops). */
void net_wait_tick(void);

/* udp.c internals used by net.c and dns.c */
void udp_init(void);
void udp_input(uint32_t src_ip_be, const uint8_t *seg, int len);
void udp_unbind(uint16_t port);
