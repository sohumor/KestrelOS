#pragma once

#include <stdint.h>

/* KestrelOS TCP: client-side connections over the IPv4 layer in net.c.
 *
 * Handles are small integers (slot indices). All blocking calls run in
 * task context and spin on net_wait_tick(); tcp_input() is the IRQ-side
 * entry point and never transmits, allocates or sleeps. */

#define TCP_CONNS   8       /* simultaneous connections */
#define TCP_MSS     1460    /* announced + maximum segment payload */
#define TCP_RXBUF   8192    /* per-connection receive buffer */
#define TCP_TXBUF   8192    /* per-connection send buffer */

void tcp_init(void);

/* Open a connection to ip_be:port (port host order). Handle >= 0 or -1. */
int  tcp_connect(uint32_t ip_be, uint16_t port, int timeout_ms);

/* Queue len bytes for transmission. Returns bytes queued or -1. */
int  tcp_send(int handle, const void *buf, int len);

/* >0 bytes read, 0 = peer closed cleanly, -1 = error or timeout. */
int  tcp_recv(int handle, void *buf, int max, int timeout_ms);

/* Graceful close (FIN handshake). Returns 0 / -1. */
int  tcp_close(int handle);

/* IRQ context: one TCP segment (IPv4 payload) arrived from src_ip_be. */
void tcp_input(uint32_t src_ip_be, const uint8_t *seg, int len);

/* Task context: retransmit / timeout / delayed-ACK pump. Call ~10x/s. */
void tcp_tick(void);
