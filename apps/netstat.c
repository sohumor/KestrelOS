/* netstat.c - show the network configuration and what can be probed.
 *
 *   netstat        interface, addresses and routes
 *   netstat -r     routes only
 *   netstat -i     interface only
 *   netstat -p     also probe the gateway and the DNS server with ICMP
 *   netstat -a     everything, including the not-yet-exported sections
 *
 * The kernel keeps an ARP cache (kernel/net.c) and a table of TCP
 * connections (kernel/tcp.c, TCP_CONNS slots), but neither is reachable
 * from userspace: SYS_NETINFO is the only network-state syscall in
 * abi/kestrel_abi.h. Rather than pretend, this prints exactly what is
 * missing and what syscall would expose it; see docs/browser.md.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#define PROBE_MS 1500

static void show_ip(const char *label, uint32_t ip_be)
{
    char buf[16];
    if (ip_be == 0)
        printf("  %-12s (unset)\n", label);
    else
        printf("  %-12s %s\n", label, ip_ntoa(ip_be, buf));
}

/* Contiguous netmasks turn into a prefix length; anything else is
 * reported as-is rather than silently rounded. */
static int mask_prefix(uint32_t mask_be)
{
    uint32_t m = ((mask_be & 0xFF) << 24) | ((mask_be & 0xFF00) << 8) |
                 ((mask_be & 0xFF0000) >> 8) | ((mask_be & 0xFF000000u) >> 24);
    int bits = 0;
    while (m & 0x80000000u) {
        bits++;
        m <<= 1;
    }
    return m ? -1 : bits;
}

static void show_iface(const struct k_netinfo *ni)
{
    int pfx = mask_prefix(ni->netmask);

    printf("interface eth0\n");
    printf("  %-12s %02x:%02x:%02x:%02x:%02x:%02x\n", "hwaddr",
           ni->mac[0], ni->mac[1], ni->mac[2], ni->mac[3], ni->mac[4],
           ni->mac[5]);
    printf("  %-12s %s\n", "state", ni->up ? "up" : "down");
    show_ip("inet", ni->ip);
    show_ip("netmask", ni->netmask);
    if (pfx >= 0)
        printf("  %-12s /%d\n", "prefix", pfx);
    else
        printf("  %-12s non-contiguous\n", "prefix");
    show_ip("gateway", ni->gateway);
    show_ip("nameserver", ni->dns);
    {
        uint32_t net = ni->ip & ni->netmask;
        uint32_t bcast = net | ~ni->netmask;
        show_ip("network", net);
        show_ip("broadcast", bcast);
    }
}

static void show_routes(const struct k_netinfo *ni)
{
    char a[16], b[16];
    int pfx = mask_prefix(ni->netmask);

    printf("routes\n");
    printf("  %-20s %-16s %s\n", "destination", "gateway", "iface");
    if (ni->netmask) {
        char dst[24];
        snprintf(dst, sizeof dst, "%s/%d",
                 ip_ntoa(ni->ip & ni->netmask, a), pfx >= 0 ? pfx : 0);
        printf("  %-20s %-16s %s\n", dst, "on-link", "eth0");
    }
    if (ni->gateway)
        printf("  %-20s %-16s %s\n", "0.0.0.0/0", ip_ntoa(ni->gateway, b),
               "eth0");
    else
        printf("  (no default route)\n");
}

static void probe(const char *label, uint32_t ip_be)
{
    char buf[16];
    long rtt;

    if (!ip_be) {
        printf("  %-12s (unset)\n", label);
        return;
    }
    rtt = ping(ip_be, PROBE_MS);
    if (rtt < 0)
        printf("  %-12s %-16s no reply in %d ms\n", label,
               ip_ntoa(ip_be, buf), PROBE_MS);
    else
        printf("  %-12s %-16s %ld ms\n", label, ip_ntoa(ip_be, buf), rtt);
}

static void show_probes(const struct k_netinfo *ni)
{
    printf("reachability (ICMP echo, %d ms timeout)\n", PROBE_MS);
    probe("gateway", ni->gateway);
    probe("nameserver", ni->dns);
}

static void show_missing(void)
{
    printf("not available from userspace\n");
    printf("  ARP cache        kernel/net.c keeps it; no syscall reads it.\n");
    printf("                   Needs e.g. SYS_ARPINFO(index, {ip, mac,\n");
    printf("                   age}) -> 0 / -1 at end, like SYS_PSINFO.\n");
    printf("  TCP connections  kernel/tcp.c owns a fixed slot table\n");
    printf("                   (TCP_CONNS). Needs e.g. SYS_TCPINFO(index,\n");
    printf("                   {state, local_port, remote_ip,\n");
    printf("                   remote_port, rx_queued, tx_queued}).\n");
    printf("  UDP bindings     kernel/udp.c tracks bound ports the same\n");
    printf("                   way; the same syscall shape would do.\n");
    printf("  counters         no packet/byte/error counters are kept by\n");
    printf("                   the drivers yet.\n");
}

static void usage(void)
{
    printf("usage: netstat [-i] [-r] [-p] [-a]\n");
    printf("  -i  interface and addresses\n");
    printf("  -r  routing table\n");
    printf("  -p  probe the gateway and nameserver with ICMP\n");
    printf("  -a  all of the above plus the unexported-state notes\n");
    printf("with no options: -i and -r\n");
}

int main(int argc, char **argv)
{
    struct k_netinfo ni;
    int want_i = 0, want_r = 0, want_p = 0, want_x = 0, any = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) == 0)
            continue;
        if (strcmp(argv[i], "-i") == 0) {
            want_i = any = 1;
        } else if (strcmp(argv[i], "-r") == 0) {
            want_r = any = 1;
        } else if (strcmp(argv[i], "-p") == 0) {
            want_p = any = 1;
        } else if (strcmp(argv[i], "-a") == 0) {
            want_i = want_r = want_p = want_x = any = 1;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else {
            printf("netstat: unknown option %s\n", argv[i]);
            usage();
            return 2;
        }
    }
    if (!any) {
        want_i = 1;
        want_r = 1;
    }

    if (netinfo(&ni) != 0) {
        printf("netstat: no network interface was found\n");
        printf("  The kernel did not initialise a NIC. Only the rtl8139\n");
        printf("  and e1000 drivers exist; check the QEMU/VirtualBox\n");
        printf("  adapter type.\n");
        return 1;
    }

    if (want_i)
        show_iface(&ni);
    if (want_r) {
        if (want_i)
            printf("\n");
        show_routes(&ni);
    }
    if (want_p) {
        printf("\n");
        if (!ni.up)
            printf("reachability: skipped, the interface is down\n");
        else
            show_probes(&ni);
    }
    if (want_x) {
        printf("\n");
        show_missing();
    }

    return ni.up ? 0 : 1;
}
