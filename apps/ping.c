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
#define PING_MAX_COUNT  10000
#define PING_GAP_MS     1000
#define PING_SLICE_MS   100

/* Sleep `ms` in short slices, returning 1 as soon as the user asks to stop.
 * The OS has no interrupt key, so an app that wants to be abortable has to
 * poll the keyboard itself (same pattern as apps/udp.c). */
static int nap_or_abort(unsigned long ms)
{
    unsigned long slept;
    unsigned char c;

    for (slept = 0; slept < ms; slept += PING_SLICE_MS) {
        while (read_nb(0, &c, 1) > 0)
            if (c == 'q' || c == 'Q' || c == 0x03)
                return 1;
        sleep_ms(PING_SLICE_MS);
    }
    while (read_nb(0, &c, 1) > 0)
        if (c == 'q' || c == 'Q' || c == 0x03)
            return 1;
    return 0;
}

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
    if (count > PING_MAX_COUNT)
        count = PING_MAX_COUNT;

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
    /* ip_aton() returns 0 for both "0.0.0.0" and a parse failure, and
     * dns_resolve() happily parses "0.0.0.0" too; the unspecified address
     * is never a valid destination. */
    if (ip == 0) {
        printf("ping: invalid destination %s\n", host);
        return 1;
    }

    char ipbuf[16];
    ip_ntoa(ip, ipbuf);
    const char *unit = (count == 1) ? "probe" : "probes";
    if (strcmp(host, ipbuf) == 0)
        printf("PING %s: %d %s\n", ipbuf, count, unit);
    else
        printf("PING %s (%s): %d %s\n", host, ipbuf, count, unit);

    int received = 0;
    int sent = 0;
    long min = 0, max = 0, sum = 0;
    for (int seq = 0; seq < count; seq++) {
        long rtt = ping(ip, PING_TIMEOUT_MS);
        sent++;
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
        /* Spend the inter-probe gap polling for q/Q/Ctrl-C instead of
         * sleeping straight through it. Statistics below cover the probes
         * actually sent, so an aborted run still reports useful numbers. */
        if (seq < count - 1 && nap_or_abort(PING_GAP_MS))
            break;
    }

    int loss = (sent - received) * 100 / sent;
    printf("\n--- %s ping statistics ---\n", host);
    printf("%d sent, %d received, %d%% loss\n", sent, received, loss);
    if (received > 0)
        printf("rtt min/avg/max = %ld/%ld/%ld ms\n",
               min, sum / received, max);

    return received > 0 ? 0 : 1;
}
