#pragma once

#include <stdint.h>

/* KestrelOS Avionics Flight Recorder.
 *
 * Fixed-size static BSS ring buffer of kernel transitions (zero heap
 * allocation, protected by a leaf irqsave spinlock). Exposed to userspace
 * via /dev/flight as a @krec typed record table.
 */

#define FLIGHT_RING_SIZE   256
#define FLIGHT_EVENT_MAX   16
#define FLIGHT_DETAIL_MAX  24

enum flight_cat {
    FLIGHT_CAT_PROC  = 0,
    FLIGHT_CAT_FS    = 1,
    FLIGHT_CAT_VM    = 2,
    FLIGHT_CAT_PULSE = 3,
    FLIGHT_CAT_TALON = 4,
    FLIGHT_CAT_NET   = 5,
    FLIGHT_CAT_COUNT = 6,
};

struct flight_entry {
    uint64_t tick;
    uint64_t arg0;
    uint64_t arg1;
    int16_t  pid;
    uint8_t  cpu;
    uint8_t  cat;
    char     event[FLIGHT_EVENT_MAX];
    char     detail[FLIGHT_DETAIL_MAX];
};

void flight_init(void);
void flight_record(uint8_t cat, const char *event, int16_t pid,
                   uint64_t arg0, uint64_t arg1, const char *detail);
const char *flight_cat_name(uint8_t cat);
uint32_t flight_render_krec(char *buf, uint32_t cap);
uint64_t flight_total(void);
