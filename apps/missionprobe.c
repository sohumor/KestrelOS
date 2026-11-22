#include <kestrel.h>
#include <stdio.h>
#include <string.h>

/* Repeated opens make live grant changes observable without relaunching. */
int main(int argc, char **argv)
{
    if (argc != 4) { puts("usage: missionprobe <read-path> <deny-path|-> <deny-ip|->"); return 1; }
    int last = -1;
    uint32_t host = !strcmp(argv[3], "-") ? 0 : ip_aton(argv[3]);
    for (;;) {
        int fd = open(argv[1], O_RDONLY);
        int allowed = fd >= 0;
        if (fd >= 0) close(fd);
        int file_denied = 1, net_denied = 1;
        if (strcmp(argv[2], "-")) {
            fd = open(argv[2], O_RDONLY);
            file_denied = fd < 0;
            if (fd >= 0) close(fd);
        }
        if (host) net_denied = ping(host, 10) < 0;
        int state = allowed | (file_denied << 1) | (net_denied << 2);
        if (state != last) {
            printf("missionprobe pid=%d read=%s file=%s net=%s\n", getpid(),
                   allowed ? "allow" : "deny", file_denied ? "deny" : "allow",
                   net_denied ? "deny" : "allow");
            last = state;
        }
        sleep_ms(100);
    }
}
