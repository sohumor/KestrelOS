#include "kernel.h"
#include "rtc.h"
#include "proc.h"
#include "string.h"
#include "io.h"
#include "spinlock.h"

/* CMOS/RTC (MC146818). Index register 0x70, data register 0x71.
 *
 * Bit 7 of the index port is the NMI-disable line; we always write it as 0
 * so reading the clock never leaves NMIs masked behind us.
 *
 * The index/data pair is global mutable hardware state and syscalls run
 * preemptible, so a task switch between the index write and the data read
 * would hand back a different register's contents. Every access sequence
 * below therefore runs under an IRQ-safe spin lock. The critical sections
 * are a handful of port reads; the unbounded UIP spin deliberately sits
 * outside them so we never hold the lock for long. */

#define CMOS_INDEX      0x70
#define CMOS_DATA       0x71

#define RTC_SEC         0x00
#define RTC_MIN         0x02
#define RTC_HOUR        0x04
#define RTC_WDAY        0x06
#define RTC_DAY         0x07
#define RTC_MON         0x08
#define RTC_YEAR        0x09
#define RTC_STATUS_A    0x0A
#define RTC_STATUS_B    0x0B
#define RTC_CENTURY     0x32

#define STATUS_A_UIP    0x80    /* update in progress */
#define STATUS_B_24H    0x02    /* 0 = 12-hour mode */
#define STATUS_B_BIN    0x04    /* 0 = BCD encoded */

#define HOUR_PM         0x80    /* 12-hour mode: set for 12:00-23:59 */

/* Bounded so a dead or emulated-away chip can never wedge the kernel. */
#define UIP_SPINS       1000000
#define SETTLE_TRIES    16
static spinlock_t rtc_lock = SPINLOCK_INIT;

/* Caller must hold interrupts off (see cmos_read_locked). */
static uint8_t cmos_read_raw(uint8_t reg)
{
    outb(CMOS_INDEX, reg);
    io_wait();
    return inb(CMOS_DATA);
}

static uint8_t cmos_read_locked(uint8_t reg)
{
    uint64_t f = spin_lock_irqsave(&rtc_lock);
    uint8_t v = cmos_read_raw(reg);
    spin_unlock_irqrestore(&rtc_lock, f);
    return v;
}

static uint8_t bcd_to_bin(uint8_t v)
{
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

/* Sakamoto's method. Returns 0 = Sunday .. 6 = Saturday. Valid for any
 * Gregorian date, so we do not have to trust CMOS register 0x06 (which is
 * wrong or simply unmaintained on plenty of boards and emulators). */
static uint8_t day_of_week(uint16_t year, uint8_t mon, uint8_t day)
{
    static const uint8_t off[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    uint32_t y = year;

    if (mon < 3)
        y--;
    return (uint8_t)((y + y / 4 - y / 100 + y / 400 + off[mon - 1] + day) % 7);
}

static int days_in_month(uint16_t year, uint8_t mon)
{
    static const uint8_t dim[12] = { 31, 28, 31, 30, 31, 30,
                                     31, 31, 30, 31, 30, 31 };
    if (mon == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
        return 29;
    return dim[mon - 1];
}

static int plausible(const struct k_rtc *t)
{
    if (t->mon < 1 || t->mon > 12)
        return 0;
    if (t->day < 1 || t->day > days_in_month(t->year, t->mon))
        return 0;
    if (t->hour > 23 || t->min > 59 || t->sec > 59)
        return 0;
    /* The RTC is battery-backed and may be unset; anything outside this
     * window means the chip is missing or the encoding guess was wrong. */
    if (t->year < 1980 || t->year > 2199)
        return 0;
    return 1;
}

/* One raw sweep of registers 0-9 (plus the century byte), taken while the
 * update flag is clear. Fields are left exactly as the chip reported them. */
struct raw_time {
    uint8_t sec, min, hour, day, mon, year, century;
};

static void wait_uip_clear(void)
{
    for (uint32_t i = 0; i < UIP_SPINS; i++)
        if (!(cmos_read_locked(RTC_STATUS_A) & STATUS_A_UIP))
            return;
}

static void read_raw(struct raw_time *r)
{
    uint64_t f;

    wait_uip_clear();

    /* One indivisible sweep: an update cannot start mid-read, and no other
     * task can steal the index register out from under us. */
    f = spin_lock_irqsave(&rtc_lock);
    r->sec     = cmos_read_raw(RTC_SEC);
    r->min     = cmos_read_raw(RTC_MIN);
    r->hour    = cmos_read_raw(RTC_HOUR);
    r->day     = cmos_read_raw(RTC_DAY);
    r->mon     = cmos_read_raw(RTC_MON);
    r->year    = cmos_read_raw(RTC_YEAR);
    r->century = cmos_read_raw(RTC_CENTURY);
    spin_unlock_irqrestore(&rtc_lock, f);
}

int rtc_read(struct k_rtc *out)
{
    struct raw_time a, b;
    uint8_t status_b, century;
    int pm;
    int settled = 0;

    if (!out)
        return -1;

    /* Re-read until two consecutive sweeps agree. Combined with the UIP
     * check this rules out tearing across a seconds/minute rollover. */
    read_raw(&a);
    for (int i = 0; i < SETTLE_TRIES; i++) {
        read_raw(&b);
        if (memcmp(&a, &b, sizeof(a)) == 0) {
            settled = 1;
            break;
        }
        a = b;
    }
    if (!settled)
        return -1;

    /* Status B describes the encoding, so it must be sampled too. */
    status_b = cmos_read_locked(RTC_STATUS_B);

    /* In 12-hour mode the PM flag rides in bit 7 of the hour byte and is
     * not part of the BCD digits, so strip it before any conversion. */
    pm = !(status_b & STATUS_B_24H) && (a.hour & HOUR_PM);
    a.hour &= (uint8_t)~HOUR_PM;

    if (!(status_b & STATUS_B_BIN)) {
        a.sec     = bcd_to_bin(a.sec);
        a.min     = bcd_to_bin(a.min);
        a.hour    = bcd_to_bin(a.hour);
        a.day     = bcd_to_bin(a.day);
        a.mon     = bcd_to_bin(a.mon);
        a.year    = bcd_to_bin(a.year);
        a.century = bcd_to_bin(a.century);
    }

    if (pm)
        a.hour = (uint8_t)((a.hour % 12) + 12);   /* 12 PM stays 12 */
    else if (!(status_b & STATUS_B_24H))
        a.hour = (uint8_t)(a.hour % 12);          /* 12 AM is hour 0 */

    /* Register 0x32 only holds a century on boards whose ACPI FADT says so;
     * elsewhere it is scratch space. Trust it only when it reads sanely. */
    century = (a.century >= 19 && a.century <= 21) ? a.century : 20;

    memset(out, 0, sizeof(*out));
    out->year = (uint16_t)(century * 100 + a.year);
    out->mon  = a.mon;
    out->day  = a.day;
    out->hour = a.hour;
    out->min  = a.min;
    out->sec  = a.sec;

    if (!plausible(out))
        return -1;

    out->wday = day_of_week(out->year, out->mon, out->day);
    return 0;
}

const char *rtc_wday_name(uint8_t wday)
{
    static const char *const names[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    return wday < 7 ? names[wday] : "???";
}

/* Two zero-padded decimal digits, no snprintf in the kernel. */
static void put2(char *dst, unsigned v)
{
    dst[0] = (char)('0' + (v / 10) % 10);
    dst[1] = (char)('0' + v % 10);
}

int rtc_format(char *buf, int max)
{
    /* "YYYY-MM-DD HH:MM:SS Www\n" plus NUL */
    char tmp[25];
    struct k_rtc t;
    const char *w;
    int len = 24;

    if (!buf || max <= 0)
        return -1;
    if (rtc_read(&t) < 0)
        return -1;

    put2(tmp + 0, t.year / 100);
    put2(tmp + 2, t.year % 100);
    tmp[4] = '-';
    put2(tmp + 5, t.mon);
    tmp[7] = '-';
    put2(tmp + 8, t.day);
    tmp[10] = ' ';
    put2(tmp + 11, t.hour);
    tmp[13] = ':';
    put2(tmp + 14, t.min);
    tmp[16] = ':';
    put2(tmp + 17, t.sec);
    tmp[19] = ' ';
    w = rtc_wday_name(t.wday);
    tmp[20] = w[0];
    tmp[21] = w[1];
    tmp[22] = w[2];
    tmp[23] = '\n';
    tmp[24] = '\0';

    if (len > max)
        len = max;
    memcpy(buf, tmp, (size_t)len);
    return len;
}
