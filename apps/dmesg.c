/* dmesg.c - print the kernel log ring.
 *
 *   dmesg            print everything the ring still holds
 *   dmesg -n N       print only the last N entries
 *   dmesg -f         keep printing new entries as they arrive (ctrl-C)
 */

#include <kestrel.h>
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

static void print_entry(struct k_logent *e)
{
    e->tag[sizeof(e->tag) - 1] = '\0';
    e->msg[sizeof(e->msg) - 1] = '\0';
    printf("[%10u] %-5s %-8s (%u) %s\n", e->time, level_name(e->level),
           e->tag[0] ? e->tag : "-", e->pid, e->msg);
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
    struct k_logent e;
    uint32_t next = 0;
    int follow = 0, limit = -1;
    int i, count, start, printed = 0;

    argc = strip_cwd_arg(argc, argv);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) {
            follow = 1;
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            limit = parse_count(argv[++i]);
            if (limit < 0) {
                printf("dmesg: -n needs a non-negative count\n");
                return 1;
            }
        } else {
            printf("usage: dmesg [-f] [-n count]\n");
            return 1;
        }
    }

    count = ring_count();
    start = (limit >= 0 && count > limit) ? count - limit : 0;
    for (i = start; i < count; i++) {
        if (log_read(i, &e) < 0)
            break;             /* the ring wrapped under us */
        print_entry(&e);
        next = e.seq + 1;
        printed++;
    }
    if (printed == 0 && count > 0 && log_read(count - 1, &e) == 0)
        next = e.seq + 1;              /* -n 0: follow from here on */
    if (printed == 0 && !follow && count == 0)
        printf("dmesg: the kernel log is empty\n");

    if (!follow)
        return 0;

    for (;;) {
        sleep_ms(FOLLOW_MS);
        for (i = 0; log_read(i, &e) == 0; i++) {
            if (e.seq < next)
                continue;
            print_entry(&e);
            next = e.seq + 1;
        }
    }
}
