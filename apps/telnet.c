/* telnet.c - a raw TCP client for poking at the stack by hand.
 *
 *   telnet <host> <port>
 *
 * Not the telnet protocol: no option negotiation, no IAC handling. It
 * opens a TCP connection, sends whatever you type a line at a time, and
 * prints whatever comes back. That is what makes it useful for testing
 * kernel/tcp.c against a real server:
 *
 *   telnet example.com 80
 *   GET / HTTP/1.0
 *   <blank line>
 *
 * ctrl-D on an empty line, or the word "quit" on its own, closes the
 * connection. There is no libc wrapper for the TCP syscalls yet, so the
 * four calls below go through syscall() directly.
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONNECT_MS   5000
#define RECV_SLICE_MS 60      /* short, so typing stays responsive */
#define LINE_MAX     1024
#define RX_MAX       2048

/* --- raw TCP syscalls (see abi/kestrel_abi.h) --- */

static int t_connect(uint32_t ip_be, int port, int timeout_ms)
{
    return (int)syscall(SYS_TCP_CONNECT, (long)ip_be, port, timeout_ms, 0);
}

static long t_send(int h, const void *buf, unsigned long len)
{
    return syscall(SYS_TCP_SEND, h, (long)buf, (long)len, 0);
}

static long t_recv(int h, void *buf, unsigned long max, int timeout_ms)
{
    return syscall(SYS_TCP_RECV, h, (long)buf, (long)max, timeout_ms);
}

static int t_close(int h)
{
    return (int)syscall(SYS_TCP_CLOSE, h, 0, 0, 0);
}

/* Print a received chunk, turning control bytes into something the
 * console can render so a binary protocol cannot scramble the screen. */
static void dump(const char *buf, long n)
{
    long i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\n' || c == '\r' || c == '\t')
            putchar(c);
        else if (c < 32 || c >= 127)
            putchar('.');
        else
            putchar(c);
    }
}

static int send_all(int h, const char *buf, unsigned long len)
{
    unsigned long done = 0;
    while (done < len) {
        long n = t_send(h, buf + done, len - done);
        if (n <= 0)
            return -1;
        done += (unsigned long)n;
    }
    return 0;
}

static void usage(void)
{
    printf("usage: telnet [-n] <host> <port>\n");
    printf("  -n  end lines with LF only (default is CRLF)\n");
    printf("ctrl-D on an empty line or \"quit\" closes the connection.\n");
}

int main(int argc, char **argv)
{
    const char *host = 0;
    int port = 0, crlf = 1;
    int i, h;
    uint32_t ip;
    struct k_netinfo ni;
    char line[LINE_MAX];
    int llen = 0;
    char rx[RX_MAX];
    char ipbuf[16];
    long got;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) == 0)
            continue;
        if (strcmp(argv[i], "-n") == 0) {
            crlf = 0;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (!host) {
            host = argv[i];
        } else if (!port) {
            port = atoi(argv[i]);
        }
    }

    if (!host || port <= 0 || port > 65535) {
        usage();
        return 2;
    }

    if (netinfo(&ni) != 0 || !ni.up) {
        printf("telnet: network unavailable\n");
        return 1;
    }

    ip = ip_aton(host);
    if (ip == 0 && dns_resolve(host, &ip) != 0) {
        printf("telnet: cannot resolve %s\n", host);
        return 1;
    }
    if (ip == 0) {
        printf("telnet: invalid address %s\n", host);
        return 1;
    }

    printf("connecting to %s (%s) port %d ...\n", host, ip_ntoa(ip, ipbuf),
           port);
    h = t_connect(ip, port, CONNECT_MS);
    if (h < 0) {
        printf("telnet: connection to %s:%d failed (refused, filtered or "
               "timed out after %d ms)\n", ip_ntoa(ip, ipbuf), port,
               CONNECT_MS);
        return 1;
    }
    printf("connected. ctrl-D or \"quit\" to close.\n");

    for (;;) {
        unsigned char c;

        /* 1. drain anything the peer sent */
        got = t_recv(h, rx, sizeof rx, RECV_SLICE_MS);
        if (got > 0) {
            dump(rx, got);
        } else if (got == 0) {
            printf("\ntelnet: connection closed by peer\n");
            break;
        }

        /* 2. take whatever the user typed since last time. read_nb keeps
         * this loop non-blocking so incoming data still arrives while a
         * line is half-typed. */
        while (read_nb(0, &c, 1) > 0) {
            if (c == 4) {                       /* ctrl-D */
                if (llen == 0) {
                    printf("\ntelnet: closing\n");
                    t_close(h);
                    return 0;
                }
                continue;
            }
            if (c == '\n' || c == '\r') {
                line[llen] = 0;
                putchar('\n');
                if (strcmp(line, "quit") == 0) {
                    printf("telnet: closing\n");
                    t_close(h);
                    return 0;
                }
                if (crlf) {
                    if (llen + 2 <= LINE_MAX) {
                        line[llen] = '\r';
                        line[llen + 1] = '\n';
                    }
                    if (send_all(h, line, (unsigned long)llen + 2) != 0)
                        goto send_failed;
                } else {
                    line[llen] = '\n';
                    if (send_all(h, line, (unsigned long)llen + 1) != 0)
                        goto send_failed;
                }
                llen = 0;
                continue;
            }
            if (c == 8 || c == 127) {           /* backspace */
                if (llen > 0) {
                    llen--;
                    printf("\b \b");
                }
                continue;
            }
            if (c == 21) {                      /* ctrl-U: kill line */
                while (llen > 0) {
                    llen--;
                    printf("\b \b");
                }
                continue;
            }
            if (c < 32 || c >= 127)
                continue;                       /* ignore arrows etc. */
            if (llen < LINE_MAX - 3) {
                line[llen++] = (char)c;
                putchar(c);
            }
        }
    }

    t_close(h);
    return 0;

send_failed:
    printf("\ntelnet: send failed, closing\n");
    t_close(h);
    return 1;
}
