/* KestrelOS ping.
 *
 * Usage: ping <host> [count]   (default count 4)
 * Resolves dotted quad first, then DNS. Exit 0 if any reply arrived.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kestrel.h>

#define PING_TIMEOUT_MS 2000

int main(int argc, char *argv[])
{
    const char *host = 0;
    int count = 4;
    int narg = 0;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) == 0)
            continue;
        if (narg == 0)
            host = argv[i];
        else if (narg == 1)
            count = atoi(argv[i]);
        narg++;
    }

    if (!host || count <= 0) {
        printf("usage: ping <host> [count]\n");
        return 1;
    }

    struct k_netinfo ni;
    if (netinfo(&ni) != 0 || !ni.up) {
        printf("network unavailable\n");
        return 1;
    }

    uint32_t ip = ip_aton(host);
    if (ip == 0) {
        if (dns_resolve(host, &ip) != 0) {
            printf("ping: cannot resolve %s\n", host);
            return 1;
        }
    }

    char ipbuf[16];
    ip_ntoa(ip, ipbuf);
    printf("PING %s (%s): %d probes\n", host, ipbuf, count);

    int received = 0;
    long min = 0, max = 0, sum = 0;
    for (int seq = 0; seq < count; seq++) {
        long rtt = ping(ip, PING_TIMEOUT_MS);
        if (rtt >= 0) {
            printf("reply from %s: seq=%d time=%ldms\n", ipbuf, seq, rtt);
            if (received == 0 || rtt < min)
                min = rtt;
            if (received == 0 || rtt > max)
                max = rtt;
            sum += rtt;
            received++;
        } else {
            printf("timeout\n");
        }
        if (seq < count - 1)
            sleep_ms(1000);
    }

    int loss = (count - received) * 100 / count;
    printf("\n--- %s ping statistics ---\n", host);
    printf("%d sent, %d received, %d%% loss\n", count, received, loss);
    if (received > 0)
        printf("rtt min/avg/max = %ld/%ld/%ld ms\n",
               min, sum / received, max);

    return received > 0 ? 0 : 1;
}
