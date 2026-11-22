#pragma once

#include <stdint.h>

/* KestrelOS TCP: client-side connections over the IPv4 layer in net.c.
 *
 * Handles are small integers (slot indices). All blocking calls run in
 * task context and spin on net_wait_tick(); tcp_input() is the receive
 * consumer's entry point and never transmits, allocates or sleeps. */

#define TCP_CONNS   8       /* simultaneous connections */
#define TCP_MSS     1460    /* announced + maximum segment payload */
#define TCP_RXBUF   8192    /* per-connection receive buffer */
#define TCP_TXBUF   8192    /* per-connection send buffer */

void tcp_init(void);

/* Open a connection to ip_be:port (port host order). Handle >= 0 or -1. */
int  tcp_connect(uint32_t ip_be, uint16_t port, int timeout_ms);

/* Queue len bytes for transmission. Returns bytes queued or -1. */
int  tcp_connect6(const uint8_t ip6[16], uint16_t port, int timeout_ms);
int  tcp_send(int handle, const void *buf, int len);

/* >0 bytes read, 0 = peer closed cleanly, -1 = error, -2 = timeout. */
int  tcp_recv(int handle, void *buf, int max, int timeout_ms);

/* Graceful close (FIN handshake). Returns 0 / -1. */
int  tcp_close(int handle);

/* One TCP segment (IPv4 payload) arrived from src_ip_be. */
void tcp_input(uint32_t src_ip_be, const uint8_t *seg, int len);

/* Task context: retransmit / timeout / delayed-ACK pump. Call ~10x/s. */
void tcp_input6(const uint8_t src[16], const uint8_t dst[16],
                const uint8_t *seg, int len);
void tcp_tick(void);

/* Render active TCP socket table as a @krec stream (buf may be NULL to measure). */
uint32_t tcp_snapshot_krec(char *buf, uint32_t cap);

/* Live (non-detached) connections owned by `pid`; Hangar refuses those. */
int tcp_count_owned(int pid);
