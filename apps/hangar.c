/* hangar.c - park running apps to disk and bring them back.
 *
 *   hangar               list parked apps
 *   hangar park PID      park a running app (its windows disappear)
 *   hangar resume BAY    resume a parked app exactly where it was
 *   hangar discard BAY   drop a parked app for good
 *
 * Parked apps survive reboots. Apps holding state the kernel cannot
 * capture (network connections, pipes, devices, child processes) are
 * refused with the reason. */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void)
{
    printf("usage: hangar [list | park PID | resume BAY | discard BAY]\n");
    return 2;
}

static int parse_num(const char *s, int *out)
{
    int v = 0;

    if (!s || !*s)
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000000)
            return -1;
        v = v * 10 + (*s - '0');
    }
    *out = v;
    return 0;
}

static void fmt_size(uint64_t bytes, char *out, int n)
{
    if (bytes >= 1024 * 1024)
        snprintf(out, n, "%lu.%lu MiB", (unsigned long)(bytes >> 20),
                 (unsigned long)((bytes & 0xFFFFF) * 10 >> 20));
    else
        snprintf(out, n, "%lu KiB", (unsigned long)((bytes + 1023) >> 10));
}

static int cmd_list(void)
{
    struct k_hangar_info info;
    int n = 0;

    for (int i = 0; hangar_list(i, &info) == 0; i++) {
        char size[24];

        if (n++ == 0)
            printf("BAY  APP              WINDOW                    SIZE\n");
        fmt_size(info.bytes, size, sizeof(size));
        printf("%02u   %-16s %-25.25s %s\n", info.bay, info.name,
               info.nwins ? info.title : "(no window)", size);
    }
    if (n == 0)
        printf("hangar: no parked apps\n");
    return 0;
}

int main(int argc, char **argv)
{
    char reason[K_HANGAR_REASON_MAX];
    int v, rc;

    if (argc < 2 || !strcmp(argv[1], "list"))
        return argc > 2 ? usage() : cmd_list();
    if (argc != 3 || parse_num(argv[2], &v) < 0)
        return usage();
    reason[0] = '\0';

    if (!strcmp(argv[1], "park")) {
        rc = hangar_park(v, reason);
        if (rc < 0) {
            printf("hangar: cannot park pid %d: %s\n", v, reason);
            return 1;
        }
        printf("hangar: pid %d parked in bay %02d\n", v, rc);
        return 0;
    }
    if (!strcmp(argv[1], "resume")) {
        rc = hangar_resume(v, reason);
        if (rc < 0) {
            printf("hangar: cannot resume bay %02d: %s\n", v, reason);
            return 1;
        }
        printf("hangar: bay %02d resumed as pid %d\n", v, rc);
        return 0;
    }
    if (!strcmp(argv[1], "discard")) {
        if (hangar_discard(v) < 0) {
            printf("hangar: no parked app in bay %02d\n", v);
            return 1;
        }
        printf("hangar: bay %02d discarded\n", v);
        return 0;
    }
    return usage();
}
