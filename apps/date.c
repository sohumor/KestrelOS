/* date.c - print the current CMOS/RTC wall clock time.
 *
 * Format: "Mon 2026-07-26 13:45:02 UTC".
 * Prints "no clock" if the kernel has no readable RTC.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

/* Provided by abi/kestrel_abi.h once the RTC syscall lands; the guard
 * keeps this app compilable against either revision of the ABI. */
#ifndef SYS_RTC
#define SYS_RTC 25   /* (struct k_rtc*) -> 0 / -1 */
struct k_rtc {
    uint16_t year;
    uint8_t mon, day, hour, min, sec, wday, pad;
};
#endif

static const char *const wdays[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

/* Pull the shell-injected "--cwd=<path>" argument, wherever it sits. */
static int strip_cwd_arg(int argc, char **argv)
{
    int i, out = 1;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) != 0)
            argv[out++] = argv[i];
    }
    return out;
}

int main(int argc, char **argv)
{
    struct k_rtc tm;
    const char *wd;

    argc = strip_cwd_arg(argc, argv);
    if (argc > 1) {
        printf("usage: date\n");
        return 1;
    }

    memset(&tm, 0, sizeof(tm));
    if (syscall(SYS_RTC, (long)&tm, 0, 0, 0) < 0) {
        printf("no clock\n");
        return 1;
    }

    wd = tm.wday < 7 ? wdays[tm.wday] : "???";
    printf("%s %04u-%02u-%02u %02u:%02u:%02u UTC\n", wd,
           (unsigned)tm.year, (unsigned)tm.mon, (unsigned)tm.day,
           (unsigned)tm.hour, (unsigned)tm.min, (unsigned)tm.sec);
    return 0;
}
