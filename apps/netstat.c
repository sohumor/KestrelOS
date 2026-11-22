/* netstat.c - show network configuration, live kernel tables, and probes.
 *
 *   netstat          interface, addresses and routes
 *   netstat -R       routes only
 *   netstat -i       interface only
 *   netstat -p       probe the gateway and the DNS server with ICMP
 *   netstat -s       active TCP sockets (/dev/sockets)
 *   netstat -c       ARP neighbor table and DNS TTL cache (/dev/arp, /dev/dns)
 *   netstat --stats  RX/TX packet, byte, and protocol counters (/dev/net)
 *   netstat -a       all of the above
 *   netstat -r       emit @krec structured records
 */

#include <kestrel.h>
#include <krec.h>
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
    {
        struct k_netinfo6 n6;
        char a[48];
        if (netinfo6(&n6) == 0) {
            if (n6.flags & K_NET6_LINKLOCAL)
                printf("  %-12s %s/64 (link)\n", "inet6",
                       ip6_ntoa(n6.link_local, a));
            if (n6.flags & K_NET6_GLOBAL)
                printf("  %-12s %s/%u (slaac)\n", "inet6",
                       ip6_ntoa(n6.global, a), n6.prefix_len);
            if (n6.flags & K_NET6_ROUTER)
                printf("  %-12s %s\n", "gateway6", ip6_ntoa(n6.router, a));
            if (n6.flags & K_NET6_DNS)
                printf("  %-12s %s\n", "nameserver6", ip6_ntoa(n6.dns, a));
        }
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

static int open_krec_dev(const char *path, struct krec_reader *r,
                         struct krec_schema *schema)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    krec_reader_init(r, fd);
    if (krec_read_header(r, schema) <= 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int stream_raw_dev(const char *path)
{
    char buf[512];
    int fd = open(path, O_RDONLY);
    long n;

    if (fd < 0) {
        krec_err("netstat: cannot open %s\n", path);
        return 1;
    }
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        long off = 0;
        while (off < n) {
            long w = write(1, buf + off, (unsigned long)(n - off));
            if (w <= 0) {
                close(fd);
                return 1;
            }
            off += w;
        }
    }
    close(fd);
    return n < 0 ? 1 : 0;
}

static void show_stats(void)
{
    struct krec_reader r;
    struct krec_schema schema;
    int fd = open_krec_dev("/dev/net", &r, &schema);

    printf("counters (/dev/net)\n");
    if (fd < 0) {
        printf("  (unavailable)\n");
        return;
    }

    int c_metric = krec_find_col(&schema, "metric");
    int c_count = krec_find_col(&schema, "count");
    if (c_metric < 0 || c_count < 0) {
        close(fd);
        printf("  (unexpected /dev/net schema)\n");
        return;
    }

    char nic_name[32];
    snprintf(nic_name, sizeof(nic_name), "none");
    uint64_t up = 0, rx_pkts = 0, tx_pkts = 0, rx_bytes = 0, tx_bytes = 0;
    uint64_t rx_drops = 0, arp_hits = 0, arp_misses = 0;
    uint64_t icmp_rx = 0, icmp_tx = 0, udp_rx = 0, udp_tx = 0;
    uint64_t tcp_rx = 0, tcp_tx = 0, dns_hits = 0, dns_misses = 0;

    char line[512];
    char *fields[KREC_MAX_COLS];
    while (krec_next(&r, line, sizeof(line), fields, KREC_MAX_COLS) == 1) {
        const char *m = fields[c_metric];
        uint64_t v = 0;
        if (krec_parse_u64(fields[c_count], &v) < 0)
            continue;
        if (strncmp(m, "nic_", 4) == 0)
            snprintf(nic_name, sizeof(nic_name), "%s", m + 4);
        else if (strcmp(m, "up") == 0)         up = v;
        else if (strcmp(m, "rx_pkts") == 0)    rx_pkts = v;
        else if (strcmp(m, "tx_pkts") == 0)    tx_pkts = v;
        else if (strcmp(m, "rx_bytes") == 0)   rx_bytes = v;
        else if (strcmp(m, "tx_bytes") == 0)   tx_bytes = v;
        else if (strcmp(m, "rx_drops") == 0)   rx_drops = v;
        else if (strcmp(m, "arp_hits") == 0)   arp_hits = v;
        else if (strcmp(m, "arp_misses") == 0) arp_misses = v;
        else if (strcmp(m, "icmp_rx") == 0)    icmp_rx = v;
        else if (strcmp(m, "icmp_tx") == 0)    icmp_tx = v;
        else if (strcmp(m, "udp_rx") == 0)     udp_rx = v;
        else if (strcmp(m, "udp_tx") == 0)     udp_tx = v;
        else if (strcmp(m, "tcp_rx") == 0)     tcp_rx = v;
        else if (strcmp(m, "tcp_tx") == 0)     tcp_tx = v;
        else if (strcmp(m, "dns_hits") == 0)   dns_hits = v;
        else if (strcmp(m, "dns_misses") == 0) dns_misses = v;
    }
    close(fd);

    printf("  %-12s %s (%s)\n", "nic", nic_name, up ? "up" : "down");
    printf("  %-12s %llu pkts, %llu bytes, %llu drops\n", "rx",
           (unsigned long long)rx_pkts,
           (unsigned long long)rx_bytes,
           (unsigned long long)rx_drops);
    printf("  %-12s %llu pkts, %llu bytes\n", "tx",
           (unsigned long long)tx_pkts,
           (unsigned long long)tx_bytes);
    printf("  %-12s %llu rx / %llu tx\n", "udp",
           (unsigned long long)udp_rx, (unsigned long long)udp_tx);
    printf("  %-12s %llu rx / %llu tx\n", "tcp",
           (unsigned long long)tcp_rx, (unsigned long long)tcp_tx);
    printf("  %-12s %llu rx / %llu tx\n", "icmp",
           (unsigned long long)icmp_rx, (unsigned long long)icmp_tx);
    printf("  %-12s %llu hits / %llu misses\n", "arp cache",
           (unsigned long long)arp_hits, (unsigned long long)arp_misses);
    printf("  %-12s %llu hits / %llu misses\n", "dns cache",
           (unsigned long long)dns_hits, (unsigned long long)dns_misses);
}

static void show_sockets(void)
{
    struct krec_reader r;
    struct krec_schema schema;
    int fd = open_krec_dev("/dev/sockets", &r, &schema);

    printf("tcp sockets (/dev/sockets)\n");
    if (fd < 0) {
        printf("  (unavailable)\n");
        return;
    }

    int c_slot  = krec_find_col(&schema, "slot");
    int c_pid   = krec_find_col(&schema, "pid");
    int c_state = krec_find_col(&schema, "state");
    int c_lport = krec_find_col(&schema, "local_port");
    int c_pip   = krec_find_col(&schema, "peer_ip");
    int c_pport = krec_find_col(&schema, "peer_port");
    int c_rx    = krec_find_col(&schema, "rx_bytes");
    int c_tx    = krec_find_col(&schema, "tx_bytes");
    if (c_slot < 0 || c_pid < 0 || c_state < 0 || c_lport < 0 ||
        c_pip < 0 || c_pport < 0 || c_rx < 0 || c_tx < 0) {
        close(fd);
        printf("  (unexpected /dev/sockets schema)\n");
        return;
    }

    char line[512];
    char *fields[KREC_MAX_COLS];
    int count = 0;
    while (krec_next(&r, line, sizeof(line), fields, KREC_MAX_COLS) == 1) {
        char peer[32];
        if (count == 0) {
            printf("  %-4s %-5s %-12s %-6s %-22s %-6s %s\n",
                   "SLOT", "PID", "STATE", "LPORT", "PEER", "RX_Q", "TX_Q");
        }
        snprintf(peer, sizeof(peer), "%s:%s", fields[c_pip], fields[c_pport]);
        printf("  %-4s %-5s %-12s %-6s %-22s %-6s %s\n",
               fields[c_slot], fields[c_pid], fields[c_state],
               fields[c_lport], peer, fields[c_rx], fields[c_tx]);
        count++;
    }
    close(fd);
    if (count == 0)
        printf("  (none)\n");
}

static void show_arp(void)
{
    struct krec_reader r;
    struct krec_schema schema;
    int fd = open_krec_dev("/dev/arp", &r, &schema);

    printf("arp cache (/dev/arp)\n");
    if (fd < 0) {
        printf("  (unavailable)\n");
        return;
    }

    int c_ip  = krec_find_col(&schema, "ip");
    int c_mac = krec_find_col(&schema, "mac");
    int c_age = krec_find_col(&schema, "age_s");
    if (c_ip < 0 || c_mac < 0 || c_age < 0) {
        close(fd);
        printf("  (unexpected /dev/arp schema)\n");
        return;
    }

    char line[512];
    char *fields[KREC_MAX_COLS];
    int count = 0;
    while (krec_next(&r, line, sizeof(line), fields, KREC_MAX_COLS) == 1) {
        char age[24];
        if (count == 0)
            printf("  %-16s %-18s %s\n", "IP", "MAC", "AGE");
        snprintf(age, sizeof(age), "%ss", fields[c_age]);
        printf("  %-16s %-18s %s\n", fields[c_ip], fields[c_mac], age);
        count++;
    }
    close(fd);
    if (count == 0)
        printf("  (empty)\n");
}

static void show_dns(void)
{
    struct krec_reader r;
    struct krec_schema schema;
    int fd = open_krec_dev("/dev/dns", &r, &schema);

    printf("dns cache (/dev/dns)\n");
    if (fd < 0) {
        printf("  (unavailable)\n");
        return;
    }

    int c_name = krec_find_col(&schema, "name");
    int c_ip   = krec_find_col(&schema, "ip");
    int c_ttl  = krec_find_col(&schema, "ttl_s");
    int c_hits = krec_find_col(&schema, "hits");
    if (c_name < 0 || c_ip < 0 || c_ttl < 0 || c_hits < 0) {
        close(fd);
        printf("  (unexpected /dev/dns schema)\n");
        return;
    }

    char line[512];
    char *fields[KREC_MAX_COLS];
    int count = 0;
    while (krec_next(&r, line, sizeof(line), fields, KREC_MAX_COLS) == 1) {
        char ttl[24];
        if (count == 0)
            printf("  %-28s %-16s %-7s %s\n", "NAME", "IP", "TTL", "HITS");
        snprintf(ttl, sizeof(ttl), "%ss", fields[c_ttl]);
        printf("  %-28s %-16s %-7s %s\n",
               fields[c_name], fields[c_ip], ttl, fields[c_hits]);
        count++;
    }
    close(fd);
    if (count == 0)
        printf("  (empty)\n");
}

static void usage(void)
{
    printf("usage: netstat [-i] [-R] [-p] [-s] [-c] [--stats] [-a] [-r]\n");
    printf("  -i         interface and addresses\n");
    printf("  -R         routing table\n");
    printf("  -p         probe the gateway and nameserver with ICMP\n");
    printf("  -s         active TCP sockets (/dev/sockets)\n");
    printf("  -c         ARP and DNS caches (/dev/arp, /dev/dns)\n");
    printf("  --stats    RX/TX packet, byte, and protocol counters (/dev/net)\n");
    printf("  -a         all of the above\n");
    printf("  -r         emit @krec structured records\n");
    printf("with no options: -i and -R\n");
}

static void emit_netinfo_krec(const struct k_netinfo *ni)
{
    static const struct krec_col cols[] = {
        { "iface",   KREC_STR },
        { "state",   KREC_STR },
        { "mac",     KREC_STR },
        { "ip",      KREC_IP  },
        { "netmask", KREC_IP  },
        { "prefix",  KREC_INT },
        { "gateway", KREC_IP  },
        { "dns",     KREC_IP  },
    };
    char mac[24], ip[16], mask[16], gw[16], dns[16], pfx_s[16];

    krec_begin(1, cols, 8);
    if (!ni)
        return;

    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             ni->mac[0], ni->mac[1], ni->mac[2],
             ni->mac[3], ni->mac[4], ni->mac[5]);
    ip_ntoa(ni->ip, ip);
    ip_ntoa(ni->netmask, mask);
    ip_ntoa(ni->gateway, gw);
    ip_ntoa(ni->dns, dns);
    snprintf(pfx_s, sizeof(pfx_s), "%d", mask_prefix(ni->netmask));
    krec_row(1, 8, "eth0", ni->up ? "up" : "down", mac,
             ip, mask, pfx_s, gw, dns);
}

static void emit_routes_krec(const struct k_netinfo *ni)
{
    static const struct krec_col cols[] = {
        { "dst",     KREC_STR },
        { "gateway", KREC_STR },
        { "iface",   KREC_STR },
    };
    char a[16], b[16];

    krec_begin(1, cols, 3);
    if (!ni)
        return;

    if (ni->netmask) {
        char dst[24];
        int pfx = mask_prefix(ni->netmask);
        snprintf(dst, sizeof(dst), "%s/%d",
                 ip_ntoa(ni->ip & ni->netmask, a), pfx >= 0 ? pfx : 0);
        krec_row(1, 3, dst, "on-link", "eth0");
    }
    if (ni->gateway)
        krec_row(1, 3, "0.0.0.0/0", ip_ntoa(ni->gateway, b), "eth0");
}

static int emit_cache_krec(void)
{
    static const struct krec_col cols[] = {
        { "cache",     KREC_STR },
        { "key",       KREC_STR },
        { "value",     KREC_STR },
        { "ttl_age_s", KREC_INT },
        { "hits",      KREC_INT },
    };
    struct krec_reader r;
    struct krec_schema schema;
    char line[512];
    char *fields[KREC_MAX_COLS];

    if (krec_begin(1, cols, 5) < 0)
        return 1;

    int fd = open_krec_dev("/dev/arp", &r, &schema);
    if (fd >= 0) {
        int c_ip  = krec_find_col(&schema, "ip");
        int c_mac = krec_find_col(&schema, "mac");
        int c_age = krec_find_col(&schema, "age_s");
        if (c_ip >= 0 && c_mac >= 0 && c_age >= 0) {
            while (krec_next(&r, line, sizeof(line), fields, KREC_MAX_COLS) == 1) {
                krec_row(1, 5, "arp", fields[c_ip], fields[c_mac],
                         fields[c_age], "0");
            }
        }
        close(fd);
    }

    fd = open_krec_dev("/dev/dns", &r, &schema);
    if (fd >= 0) {
        int c_name = krec_find_col(&schema, "name");
        int c_ip   = krec_find_col(&schema, "ip");
        int c_ttl  = krec_find_col(&schema, "ttl_s");
        int c_hits = krec_find_col(&schema, "hits");
        if (c_name >= 0 && c_ip >= 0 && c_ttl >= 0 && c_hits >= 0) {
            while (krec_next(&r, line, sizeof(line), fields, KREC_MAX_COLS) == 1) {
                krec_row(1, 5, "dns", fields[c_name], fields[c_ip],
                         fields[c_ttl], fields[c_hits]);
            }
        }
        close(fd);
    }

    return 0;
}

int main(int argc, char **argv)
{
    struct k_netinfo ni;
    int want_i = 0, want_r = 0, want_p = 0;
    int want_s = 0, want_c = 0, want_stats = 0;
    int any = 0, want_krec = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0) {
            want_i = any = 1;
        } else if (strcmp(argv[i], "-R") == 0 ||
                   strcmp(argv[i], "--routes") == 0) {
            want_r = any = 1;
        } else if (strcmp(argv[i], "-r") == 0 ||
                   strcmp(argv[i], "--krec") == 0) {
            want_krec = 1;
        } else if (strcmp(argv[i], "-p") == 0) {
            want_p = any = 1;
        } else if (strcmp(argv[i], "-s") == 0 ||
                   strcmp(argv[i], "--sockets") == 0) {
            want_s = any = 1;
        } else if (strcmp(argv[i], "-c") == 0 ||
                   strcmp(argv[i], "--cache") == 0) {
            want_c = any = 1;
        } else if (strcmp(argv[i], "--stats") == 0) {
            want_stats = any = 1;
        } else if (strcmp(argv[i], "-a") == 0) {
            want_i = want_r = want_p = want_s = want_c = want_stats = any = 1;
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

    int need_netinfo = want_i || want_r || want_p;
    int have_ni = (netinfo(&ni) == 0);
    if (need_netinfo && !have_ni) {
        if (want_krec) {
            if (want_r && !want_i)
                emit_routes_krec(0);
            else
                emit_netinfo_krec(0);
            krec_err("netstat: no network interface was found\n");
            return 1;
        }
        printf("netstat: no network interface was found\n");
        printf("  The kernel did not initialise a NIC. Only the rtl8139\n");
        printf("  and e1000 drivers exist; check the QEMU/VirtualBox\n");
        printf("  adapter type.\n");
        return 1;
    }

    if (want_krec) {
        if (want_i) {
            emit_netinfo_krec(&ni);
            return 0;
        }
        if (want_r) {
            emit_routes_krec(&ni);
            return 0;
        }
        if (want_s && !want_c && !want_stats)
            return stream_raw_dev("/dev/sockets");
        if (want_stats && !want_s && !want_c)
            return stream_raw_dev("/dev/net");
        if (want_c && !want_s && !want_stats)
            return emit_cache_krec();
        if (have_ni) {
            emit_netinfo_krec(&ni);
            return 0;
        }
        return stream_raw_dev("/dev/net");
    }

    int printed = 0;
    if (want_i) {
        show_iface(&ni);
        printed = 1;
    }
    if (want_r) {
        if (printed)
            printf("\n");
        show_routes(&ni);
        printed = 1;
    }
    if (want_p) {
        if (printed)
            printf("\n");
        if (!ni.up)
            printf("reachability: skipped, the interface is down\n");
        else
            show_probes(&ni);
        printed = 1;
    }
    if (want_stats) {
        if (printed)
            printf("\n");
        show_stats();
        printed = 1;
    }
    if (want_s) {
        if (printed)
            printf("\n");
        show_sockets();
        printed = 1;
    }
    if (want_c) {
        if (printed)
            printf("\n");
        show_arp();
        printf("\n");
        show_dns();
    }

    if (need_netinfo)
        return ni.up ? 0 : 1;
    return 0;
}
