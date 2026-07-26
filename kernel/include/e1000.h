#pragma once

#include <stdbool.h>

/* Intel 8254x-family "e1000" gigabit driver (MMIO register file).
 * Covers the 82540EM emulated by QEMU, VirtualBox and VMware. */

/* Probe/initialize; registers a netdev and returns true on success. */
bool e1000_init(void);
