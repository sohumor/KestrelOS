/* KestrelOS nslookup: resolve a hostname via SYS_DNS. */

#include <stdio.h>
#include <string.h>
#include <kestrel.h>

int main(int argc, char *argv[])
{
    const char *name = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) == 0)
            continue;
        if (!name)
            name = argv[i];
    }

    if (!name) {
        printf("usage: nslookup <name>\n");
        return 1;
    }

    struct k_netinfo ni;
    if (netinfo(&ni) != 0 || !ni.up) {
        printf("network unavailable\n");
        return 1;
    }

    /* A first (unchanged output), then AAAA on its own line. */
    uint32_t ip;
    uint8_t ip6[16];
    char buf[48];
    int found = 0;
    if (dns_resolve(name, &ip) == 0) {
        printf("%s -> %s\n", name, ip_ntoa(ip, buf));
        found = 1;
    }
    if (dns_resolve6(name, ip6) == 0) {
        printf("%s -> %s (AAAA)\n", name, ip6_ntoa(ip6, buf));
        found = 1;
    }
    if (!found) {
        printf("nslookup: cannot resolve %s\n", name);
        return 1;
    }
    return 0;
}
