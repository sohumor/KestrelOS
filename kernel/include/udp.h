#pragma once

#include <stdint.h>

void udp_init(void);
int  udp_bind(uint16_t port);
void udp_unbind(uint16_t port);
void udp_input(uint32_t src_ip_be, uint32_t dst_ip_be,
               const uint8_t *seg, int len);
int  udp_send(uint32_t ip_be, uint16_t sport, uint16_t dport,
              const void *buf, int len);
int  udp_recv(uint16_t port, void *buf, int maxlen, int timeout_ms);

/* UDP ports bound by `pid`; Hangar refuses those. */
int udp_count_owned(int pid);
