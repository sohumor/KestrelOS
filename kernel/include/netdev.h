#pragma once

#include <stdint.h>

/* Minimal NIC abstraction. A driver probes its hardware at init time and,
 * on success, registers itself here; the network stack transmits and polls
 * through the registered device. One active NIC at a time. */

struct netdev {
    const char *name;
    uint8_t mac[6];
    int (*send)(const void *frame, int len);   /* whole frame, no CRC */
    void (*poll)(void);        /* drain RX; safe to call anywhere */
};

void netdev_register(const struct netdev *dev);
const struct netdev *netdev_current(void);   /* NULL if no NIC */
