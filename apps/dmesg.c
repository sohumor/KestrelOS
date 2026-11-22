/* dmesg.c - print the kernel log ring.
 *
 *   dmesg            print everything the ring still holds
 *   dmesg -n N       print only the last N entries
 *   dmesg -f         keep printing new entries as they arrive (ctrl-C)
 *   dmesg -r         emit @krec records
 */

#include <kestrel.h>
#include <krec.h>
#include <stdio.h>
#include <string.h>

#define FOLLOW_MS 300

static const char *level_name(unsigned int level)
{
    switch (level) {
    case K_LOG_DEBUG: return "debug";
    case K_LOG_INFO:  return "info";
    case K_LOG_WARN:  return "warn";
    case K_LOG_ERR:   return "error";
    default:          return "?";
    }
}

static int log_read(int index, struct k_logent *e)
{
    return (int)syscall(SYS_LOGREAD, index, (long)e, 0, 0);
}

static void print_entry(struct k_logent *e, int rflag)
{
    e->tag[sizeof(e->tag) - 1] = '\0';
    e->msg[sizeof(e->msg) - 1] = '\0';
    if (rflag) {
        char tick_s[24], pid_s[16];
        snprintf(tick_s, sizeof(tick_s), "%u", e->time);
        snprintf(pid_s, sizeof(pid_s), "%u", e->pid);
        krec_row(1, 5, tick_s, level_name(e->level),
                 e->tag[0] ? e->tag : "-", pid_s, e->msg);
    } else {
        printf("[%10u] %-5s %-8s (%u) %s\n", e->time, level_name(e->level),
               e->tag[0] ? e->tag : "-", e->pid, e->msg);
    }
}

/* Number of entries the ring currently holds. */
static int ring_count(void)
{
    struct k_logent e;
    int i = 0;

    while (log_read(i, &e) == 0)
        i++;
    return i;
}

/* Strict non-negative decimal parse. Returns -1 on any junk. */
static int parse_count(const char *s)
{
    long v = 0;
    int i;

    if (!s || !s[0])
        return -1;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
        if (v > 0x7fffffffL)
            return -1;
    }
    return (int)v;
}

int main(int argc, char **argv)
{
    static const struct krec_col cols[] = {
        { "tick",  KREC_U64 },
        { "level", KREC_STR },
        { "tag",   KREC_STR },
        { "pid",   KREC_INT },
        { "msg",   KREC_STR },
    };
    struct k_logent e;
    uint32_t next = 0;
    int follow = 0, limit = -1, rflag = 0;
    int i, count, start, printed = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) {
            follow = 1;
        } else if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--krec")) {
            rflag = 1;
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            limit = parse_count(argv[++i]);
            if (limit < 0) {
                printf("dmesg: -n needs a non-negative count\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("usage: dmesg [-f] [-n count] [-r]\n");
            return 0;
        } else {
            printf("usage: dmesg [-f] [-n count] [-r]\n");
            return 1;
        }
    }

    if (rflag && krec_begin(1, cols, 5) < 0)
        return 1;

    count = ring_count();
    start = (limit >= 0 && count > limit) ? count - limit : 0;
    for (i = start; i < count; i++) {
        if (log_read(i, &e) < 0)
            break;             /* the ring wrapped under us */
        print_entry(&e, rflag);
        next = e.seq + 1;
        printed++;
    }
    if (printed == 0 && count > 0 && log_read(count - 1, &e) == 0)
        next = e.seq + 1;              /* -n 0: follow from here on */
    if (printed == 0 && !follow && count == 0 && !rflag)
        printf("dmesg: the kernel log is empty\n");

    if (!follow)
        return 0;

    for (;;) {
        sleep_ms(FOLLOW_MS);
        for (i = 0; log_read(i, &e) == 0; i++) {
            if (e.seq < next)
                continue;
            print_entry(&e, rflag);
            next = e.seq + 1;
        }
    }
}
