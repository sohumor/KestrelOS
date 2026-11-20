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

/* Telemetry counters and @krec snapshots for /dev/{net,arp,dns}. */
enum net_stat_kind {
    NET_STAT_RX_PKT = 0,
    NET_STAT_TX_PKT,
    NET_STAT_RX_DROP,
    NET_STAT_ARP_HIT,
    NET_STAT_ARP_MISS,
    NET_STAT_ICMP_RX,
    NET_STAT_ICMP_TX,
    NET_STAT_UDP_RX,
    NET_STAT_UDP_TX,
    NET_STAT_TCP_RX,
    NET_STAT_TCP_TX,
    NET_STAT_DNS_HIT,
    NET_STAT_DNS_MISS,
};

void net_stat_inc(int kind, uint32_t bytes);
uint32_t net_snapshot_krec(char *buf, uint32_t cap);
uint32_t arp_snapshot_krec(char *buf, uint32_t cap);
uint32_t dns_snapshot_krec(char *buf, uint32_t cap);

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
/* IPv4 pseudo-header + transport segment checksum (TCP/UDP). */
uint16_t net_transport_checksum(uint32_t src_be, uint32_t dst_be,
                                uint8_t protocol,
                                const void *segment, int len);

uint32_t net_ip_addr(void);
uint32_t net_dns_addr(void);

/* Raw Ethernet transmit through the active NIC. */
int  net_eth_send(const uint8_t *dst_mac, uint16_t ethertype,
                  const void *payload, int len);

/* ---- IPv6 (net6.c); addresses are 16 network-order bytes ---- */

#define NET6_UDP_MAX     1452   /* ETH_MTU - 40 (ipv6) - 8 (udp) */

void net6_init(void);                 /* link-local + SLAAC, after net_init */
void ipv6_input(const uint8_t *eth_src, const uint8_t *pkt, int len);
int  net6_send(const uint8_t dst[16], uint8_t next_header,
               const void *payload, int len);
bool net6_source_for(const uint8_t dst[16], uint8_t src_out[16]);
bool net6_dns_addr(uint8_t out[16]);
void net6_get_info(struct k_netinfo6 *out);
long icmp6_ping(const uint8_t dst[16], int timeout_ms);
int  dns_resolve6(const char *name, uint8_t out[16]);
int  udp_send6(const uint8_t dst[16], uint16_t sport, uint16_t dport,
               const void *buf, int len);
void udp_input6(const uint8_t src[16], const uint8_t dst[16],
                const uint8_t *seg, int len);
/* IPv6 pseudo-header + transport segment checksum (TCP/UDP/ICMPv6). */
uint16_t net_transport_checksum6(const uint8_t src[16], const uint8_t dst[16],
                                 uint8_t next_header,
                                 const void *segment, int len);

/* Poll + give up the CPU for ~1 tick (task context wait loops). */
void net_wait_tick(void);

/* udp.c internals used by net.c and dns.c */
void udp_init(void);
void udp_input(uint32_t src_ip_be, uint32_t dst_ip_be,
               const uint8_t *seg, int len);
void udp_unbind(uint16_t port);
