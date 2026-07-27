#pragma once

#include <stdint.h>
#include <stdbool.h>

/* DHCP client: performs DHCP DISCOVER / REQUEST to configure IP settings.
 * Returns 0 on success (network configured via DHCP), -1 on failure/timeout. */
int dhcp_discover(void);
