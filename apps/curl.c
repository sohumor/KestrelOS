/* curl.c - fetch a URL and write it to stdout or a file.
 *
 *   curl <url>              body to stdout
 *   curl -i <url>           status line and headers first
 *   curl -o <file> <url>    body to a file
 *   curl -s <url>           no progress or error chatter, just the body
 *
 * The transfer itself is http_get() from libc/http.c. That call hands
 * back only the body, the length and the status, so -i prints a
 * RECONSTRUCTED header block rather than the bytes the server sent; the
 * output says so. See docs/browser.md for what it would take to print
 * the real headers.
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#  if __has_include(<http.h>)
#    include <http.h>
#    define HAVE_HTTP 1
#  endif
#endif

#define PATH_LIM 256

static const char *g_cwd = "/";

static void resolve(const char *tok, char *out, unsigned long outsz)
{
    if (tok[0] == '/')
        snprintf(out, outsz, "%s", tok);
    else if (g_cwd[0] == '/' && g_cwd[1] == '\0')
        snprintf(out, outsz, "/%s", tok);
    else
        snprintf(out, outsz, "%s/%s", g_cwd, tok);
}

static void usage(void)
{
    printf("usage: curl [-i] [-s] [-o <file>] <url>\n");
    printf("  -i         print a status line and headers before the body\n");
    printf("  -o <file>  write the body to <file> instead of stdout\n");
    printf("  -s         silent: suppress messages, print only the body\n");
    printf("\n");
    printf("Only http:// is supported. Exit status is 0 for an HTTP 2xx\n");
    printf("response and 1 for anything else.\n");
}

/* Reconstruct the host part of a URL for the DNS pre-check. */
static int url_host(const char *url, char *host, unsigned long n, int *port)
{
    const char *p = strstr(url, "://");
    const char *q;
    unsigned long len;
    char *colon;

    *port = 80;
    if (!p)
        return -1;
    p += 3;
    q = p;
    while (*q && *q != '/' && *q != '?' && *q != '#')
        q++;
    len = (unsigned long)(q - p);
    if (len == 0 || len >= n)
        return -1;
    memcpy(host, p, len);
    host[len] = 0;
    colon = strrchr(host, ':');
    if (colon) {
        *colon = 0;
        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535)
            *port = 80;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *url = 0, *outfile = 0;
    int inc_hdr = 0, silent = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) == 0) {
            g_cwd = argv[i] + 6;
        } else if (strcmp(argv[i], "-i") == 0) {
            inc_hdr = 1;
        } else if (strcmp(argv[i], "-s") == 0) {
            silent = 1;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outfile = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1]) {
            printf("curl: unknown option %s\n", argv[i]);
            return 2;
        } else if (!url) {
            url = argv[i];
        }
    }

    if (!url) {
        usage();
        return 2;
    }

#ifndef HAVE_HTTP
    (void)inc_hdr;
    (void)silent;
    (void)outfile;
    printf("curl: this build has no HTTP client (libc/http.c not linked)\n");
    return 1;
#else
    {
        char full[512];
        char host[160];
        struct k_netinfo ni;
        uint32_t ip;
        char *body = 0;
        unsigned long len = 0;
        int status = 0, port = 80, rc;

        if (!strstr(url, "://"))
            snprintf(full, sizeof full, "http://%s", url);
        else
            snprintf(full, sizeof full, "%s", url);

        if (strncmp(full, "http://", 7) != 0) {
            if (!silent)
                printf("curl: only http:// URLs are supported\n");
            return 1;
        }
        if (url_host(full, host, sizeof host, &port) != 0) {
            if (!silent)
                printf("curl: malformed URL: %s\n", full);
            return 1;
        }
        if (netinfo(&ni) != 0 || !ni.up) {
            if (!silent)
                printf("curl: network unavailable (no NIC)\n");
            return 1;
        }
        ip = ip_aton(host);
        if (ip == 0 && dns_resolve(host, &ip) != 0) {
            if (!silent)
                printf("curl: cannot resolve %s\n", host);
            return 1;
        }

        rc = http_get(full, &body, &len, &status);
        if (rc != 0 || !body) {
            char ipbuf[16];
            free(body);
            if (!silent)
                printf("curl: cannot connect to %s (%s) port %d\n", host,
                       ip_ntoa(ip, ipbuf), port);
            return 1;
        }

        if (inc_hdr) {
            /* Reconstructed, not received - http_get() does not expose
             * the response headers. Flagged so nobody parses it as the
             * real thing. */
            printf("HTTP/1.1 %d\n", status);
            printf("Content-Length: %lu\n", len);
            printf("X-Kestrel-Note: headers reconstructed by curl; "
                   "http_get() returns only body/length/status\n");
            printf("\n");
        }

        if (outfile) {
            char path[PATH_LIM];
            int fd;
            unsigned long done = 0;

            resolve(outfile, path, sizeof path);
            fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
            if (fd < 0) {
                free(body);
                if (!silent)
                    printf("curl: cannot write %s\n", path);
                return 1;
            }
            while (done < len) {
                long n = write(fd, body + done, len - done);
                if (n <= 0)
                    break;
                done += (unsigned long)n;
            }
            close(fd);
            if (done != len) {
                free(body);
                if (!silent)
                    printf("curl: short write to %s (%lu of %lu bytes)\n",
                           path, done, len);
                return 1;
            }
            if (!silent)
                printf("curl: wrote %lu bytes to %s (HTTP %d)\n", len, path,
                       status);
        } else {
            unsigned long done = 0;
            while (done < len) {
                long n = write(1, body + done, len - done);
                if (n <= 0)
                    break;
                done += (unsigned long)n;
            }
        }

        free(body);
        if (status < 200 || status >= 300) {
            if (!silent)
                printf("curl: HTTP %d\n", status);
            return 1;
        }
        return 0;
    }
#endif
}
