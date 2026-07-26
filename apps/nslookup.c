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

    uint32_t ip;
    if (dns_resolve(name, &ip) != 0) {
        printf("nslookup: cannot resolve %s\n", name);
        return 1;
    }

    char buf[16];
    printf("%s -> %s\n", name, ip_ntoa(ip, buf));
    return 0;
}
