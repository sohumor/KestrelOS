#pragma once

#include <stdbool.h>

/* Realtek RTL8139 fast-ethernet driver (I/O-port mode, PIO + DMA rings). */

/* Probe/initialize; registers a netdev and returns true on success,
 * false if no RTL8139 is present. */
bool rtl8139_init(void);
