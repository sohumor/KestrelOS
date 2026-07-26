/* KestrelOS udp: tiny datagram tool.
 *
 * udp send <ip> <port> <message...>   one datagram, source port 40000
 * udp listen <port>                   print incoming payloads, q quits
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kestrel.h>

#define UDP_SPORT   40000
#define RECV_MAX    1472
#define RECV_WAIT_MS 5000

static void usage(void)
{
    printf("usage: udp send <ip> <port> <message...>\n");
    printf("       udp listen <port>\n");
}

static int do_send(int nargs, char *args[])
{
    if (nargs < 3) {
        usage();
        return 1;
    }

    uint32_t ip = ip_aton(args[0]);
    if (ip == 0 && dns_resolve(args[0], &ip) != 0) {
        printf("udp: cannot resolve %s\n", args[0]);
        return 1;
    }
    int port = atoi(args[1]);
    if (port <= 0 || port > 65535) {
        printf("udp: bad port %s\n", args[1]);
        return 1;
    }

    /* join remaining args with single spaces */
    char msg[1024];
    msg[0] = 0;
    unsigned long len = 0;
    for (int i = 2; i < nargs; i++) {
        unsigned long alen = strlen(args[i]);
        if (len + alen + 2 >= sizeof(msg))
            break;
        if (i > 2)
            msg[len++] = ' ';
        memcpy(msg + len, args[i], alen);
        len += alen;
    }
    msg[len] = 0;

    if (udp_send(ip, (uint16_t)port, UDP_SPORT, msg, len) < 0) {
        printf("udp: send failed\n");
        return 1;
    }
    char buf[16];
    printf("sent %lu bytes to %s:%d\n", len, ip_ntoa(ip, buf), port);
    return 0;
}

static int do_listen(int nargs, char *args[])
{
    if (nargs < 1) {
        usage();
        return 1;
    }
    int port = atoi(args[0]);
    if (port <= 0 || port > 65535) {
        printf("udp: bad port %s\n", args[0]);
        return 1;
    }

    printf("listening on udp port %d (q to quit)\n", port);
    char buf[RECV_MAX + 1];
    for (;;) {
        long n = udp_recv((uint16_t)port, buf, RECV_MAX, RECV_WAIT_MS);
        if (n > 0) {
            buf[n] = 0;
            printf("[%ld bytes] %s\n", n, buf);
        }
        /* between (or after) timeouts, check the keyboard for q */
        unsigned char c;
        while (read_nb(0, &c, 1) > 0) {
            if (c == 'q' || c == 'Q') {
                printf("udp: bye\n");
                return 0;
            }
        }
    }
}

int main(int argc, char *argv[])
{
    /* strip the shell-injected --cwd= argument */
    char *args[32];
    int nargs = 0;
    for (int i = 1; i < argc && nargs < 32; i++) {
        if (strncmp(argv[i], "--cwd=", 6) == 0)
            continue;
        args[nargs++] = argv[i];
    }

    if (nargs < 1) {
        usage();
        return 1;
    }

    struct k_netinfo ni;
    if (netinfo(&ni) != 0 || !ni.up) {
        printf("network unavailable\n");
        return 1;
    }

    if (strcmp(args[0], "send") == 0)
        return do_send(nargs - 1, args + 1);
    if (strcmp(args[0], "listen") == 0)
        return do_listen(nargs - 1, args + 1);

    usage();
    return 1;
}
