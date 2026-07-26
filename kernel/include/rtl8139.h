#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Realtek RTL8139 fast-ethernet driver (I/O-port mode, PIO + DMA rings). */

/* Probe/initialize; returns false if no RTL8139 is present. */
bool rtl8139_init(void);

bool rtl8139_present(void);

/* Station MAC address (valid after successful rtl8139_init). */
const uint8_t *rtl8139_mac(void);

/* Transmit one ethernet frame (dst/src/ethertype/payload, no CRC).
 * Synchronous; safe from task and IRQ context. Returns 0 or -1. */
int rtl8139_send(const void *frame, int len);

/* Drain the RX ring without relying on the IRQ (polling paths). */
void rtl8139_poll(void);
