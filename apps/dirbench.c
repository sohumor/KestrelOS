#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 4 || (strcmp(argv[1], "stat") != 0 &&
                     strcmp(argv[1], "create") != 0)) {
        printf("usage: dirbench stat|create <absolute-path> <count>\n");
        return 1;
    }
    int n = atoi(argv[3]);
    if (n < 0 || n > 100000 || argv[2][0] != '/')
        return 1;
    int create = strcmp(argv[1], "create") == 0;
    if (create && mkdir_(argv[2]) < 0) {
        printf("dirbench: use a new directory\n");
        return 1;
    }
    int ok = 0, errors = 0;
    unsigned long start = uptime_ms();
    for (int i = 0; i < n; i++) {
        if (create) {
            char path[256];
            if (snprintf(path, sizeof(path), "%s/f%d", argv[2], i) >= (int)sizeof(path))
                return 1;
            int fd = open(path, O_WRONLY | O_CREAT);
            if (fd < 0)
                errors++;
            else {
                close(fd);
                ok++;
            }
        } else {
            struct k_stat st;
            if (stat_(argv[2], &st) < 0)
                errors++;
            else
                ok++;
        }
    }
    printf("dirbench: %s %s n=%d ok=%d errors=%d ms=%lu\n",
           argv[1], argv[2], n, ok, errors, uptime_ms() - start);
    return create && errors ? 1 : 0;
}
