#pragma once

#include <stdint.h>
#include "kestrel_abi.h"

/* MC146818-compatible CMOS real-time clock (ports 0x70/0x71). */

/* Fills *out with the current wall-clock time. Returns 0 on success, -1 if
 * the chip never settled or handed back an impossible date. */
int rtc_read(struct k_rtc *out);

/* Renders the current time as "YYYY-MM-DD HH:MM:SS Www\n" into buf (needs
 * 25 bytes). Returns the number of bytes written, or -1 if the clock is
 * unreadable. Backs a /dev-style pseudo-file. */
int rtc_format(char *buf, int max);

/* "Sun".."Sat" for wday 0..6, "???" otherwise. */
const char *rtc_wday_name(uint8_t wday);
