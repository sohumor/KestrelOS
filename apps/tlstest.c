/* KestrelOS tlstest: open a TLS 1.3 connection and say exactly what
 * happened.
 *
 * This is the on-target counterpart of tools/test_tls.c. The library is
 * tested exhaustively on the host, where openssl is available to be an
 * independent second implementation; what this program is for is proving
 * the same code works over the kernel's own TCP stack, and giving anyone
 * debugging a connection a way to see the negotiated parameters, the
 * certificate chain and the exact reason a handshake failed.
 *
 *   tlstest <host> [port] [path]   fetch https://host:port/path
 *   tlstest --roots                list the trust store
 *   tlstest --insecure ...         connect even if the chain is bad, and
 *                                  print why it was bad
 *   tlstest --head ...             print only the response headers
 *   tlstest --group x25519|p256    force one key exchange group
 *   tlstest --retry                offer both groups but send a key share
 *                                  only for X25519's partner, forcing a
 *                                  HelloRetryRequest from most servers
 *   tlstest --suite aes128|aes256|chacha20
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kestrel.h>

#include "tls.h"

static void print_date(const char *what, uint32_t t)
{
    char buf[16];

    x509_format_date(t, buf, sizeof(buf));
    printf("  %-10s %s\n", what, buf);
}

static int list_roots(void)
{
    int i, n, total = 0, from_file = 0;

    if (!tls_default_store()) {
        printf("the trust store could not be built\n");
        return 1;
    }
    n = tls_builtin_root_count();
    tls_default_store_stat(&total, &from_file);
    printf("%d roots compiled in, %d loaded from %s, %d usable in total\n\n",
           n, from_file, TLS_ROOTS_FILE, total);
    for (i = 0; i < n; i++)
        printf("  %s\n", tls_builtin_root_name(i));
    if (from_file)
        printf("\n  ...and %d more from %s\n", from_file, TLS_ROOTS_FILE);
    return 0;
}

int main(int argc, char *argv[])
{
    struct tls_options o;
    struct tls_error err;
    struct tls_info info;
    struct tls_conn *c;
    const char *host = 0, *path = "/";
    char req[512];
    char buf[2048];
    int port = 443;
    int i, n, headers_only = 0, positional = 0;
    unsigned long total = 0;
    unsigned long start;

    tls_options_default(&o);

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strncmp(a, "--cwd=", 6) == 0)
            continue;
        if (strcmp(a, "--roots") == 0)
            return list_roots();
        if (strcmp(a, "--insecure") == 0) {
            o.verify = TLS_VERIFY_NONE;
            continue;
        }
        if (strcmp(a, "--head") == 0) {
            headers_only = 1;
            continue;
        }
        if (strcmp(a, "--retry") == 0) {
            o.groups = TLS_G_ALL;
            o.key_shares = TLS_G_P256;
            continue;
        }
        if (strcmp(a, "--group") == 0 && i + 1 < argc) {
            const char *g = argv[++i];
            if (strcmp(g, "x25519") == 0)
                o.groups = o.key_shares = TLS_G_X25519;
            else if (strcmp(g, "p256") == 0)
                o.groups = o.key_shares = TLS_G_P256;
            else {
                printf("tlstest: unknown group '%s'\n", g);
                return 2;
            }
            continue;
        }
        if (strcmp(a, "--suite") == 0 && i + 1 < argc) {
            const char *s = argv[++i];
            if (strcmp(s, "aes128") == 0)
                o.suites = TLS_S_AES128GCM;
            else if (strcmp(s, "aes256") == 0)
                o.suites = TLS_S_AES256GCM;
            else if (strcmp(s, "chacha20") == 0)
                o.suites = TLS_S_CHACHA20;
            else {
                printf("tlstest: unknown suite '%s'\n", s);
                return 2;
            }
            continue;
        }
        if (a[0] == '-' && a[1] == '-') {
            printf("tlstest: unknown option '%s'\n", a);
            return 2;
        }
        if (positional == 0)
            host = a;
        else if (positional == 1)
            port = atoi(a);
        else if (positional == 2)
            path = a;
        positional++;
    }

    if (!host) {
        printf("usage: tlstest <host> [port] [path]\n"
               "       tlstest --roots\n"
               "options: --insecure --head --retry --group <g> "
               "--suite <s>\n");
        return 2;
    }
    if (port <= 0 || port > 65535)
        port = 443;

    {
        struct k_netinfo ni;
        if (netinfo(&ni) != 0 || !ni.up) {
            printf("tlstest: the network is not up\n");
            return 1;
        }
    }

    printf("connecting to %s:%d ...\n", host, port);
    start = uptime_ms();
    c = tls_connect(host, port, &o, &err);
    if (!c) {
        printf("\nHANDSHAKE FAILED\n  %s\n", err.msg);
        if (err.alert >= 0)
            printf("  (TLS alert %d, %s)\n", err.alert,
                   err.alert_received ? "sent by the server"
                                      : "sent by this client");
        if (err.cert_failure)
            printf("  Run again with --insecure to connect anyway. Do that "
                   "only if you\n  understand why the check failed.\n");
        return 1;
    }
    tls_info(c, &info);

    printf("\nhandshake done in %lu ms\n", uptime_ms() - start);
    printf("  cipher     %s\n", info.cipher);
    printf("  group      %s%s\n", info.group,
           info.hello_retry ? " (after a HelloRetryRequest)" : "");
    printf("  alpn       %s\n", info.alpn[0] ? info.alpn : "(none)");
    printf("  chain      %d certificate%s\n", info.chain_len,
           info.chain_len == 1 ? "" : "s");
    printf("  subject    %s\n", info.subject);
    printf("  issuer     %s\n", info.issuer);
    print_date("valid from", info.not_before);
    print_date("until", info.not_after);
    if (info.verified) {
        printf("  verified   yes, against the trust store\n");
    } else {
        printf("  verified   NO -- %s\n", info.cert_error);
        printf("             the connection is encrypted but NOT "
               "authenticated\n");
    }
    if (info.weak_entropy)
        printf("  entropy    WEAK: no hardware random source was found, so "
               "the keys\n             are only as unpredictable as the "
               "system timers\n");

    snprintf(req, sizeof(req),
             "%s %s HTTP/1.1\r\nHost: %s\r\n"
             "User-Agent: KestrelOS/1.0 (tlstest)\r\n"
             "Accept: */*\r\nConnection: close\r\n\r\n",
             headers_only ? "HEAD" : "GET", path, host);
    n = tls_write(c, req, (int)strlen(req));
    if (n < 0) {
        printf("\nsend failed: %s\n", tls_conn_error(c));
        tls_close(c);
        return 1;
    }

    printf("\n---- response ----\n");
    for (;;) {
        n = tls_read(c, buf, (int)sizeof(buf) - 1);
        if (n == 0)
            break;
        if (n < 0) {
            if (n == TLS_E_TRUNCATED)
                printf("\n[the server cut the connection without a "
                       "close_notify]\n");
            else
                printf("\n[read failed: %s]\n", tls_conn_error(c));
            break;
        }
        total += (unsigned long)n;
        if (headers_only) {
            buf[n] = 0;
            printf("%s", buf);
            if (strstr(buf, "\r\n\r\n"))
                break;
        } else {
            /* Write the bytes through unchanged; this is a page, not a
             * format string. */
            write(1, buf, (unsigned long)n);
        }
    }
    printf("\n---- %lu bytes ----\n", total);
    tls_close(c);
    return 0;
}
