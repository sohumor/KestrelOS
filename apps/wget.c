/* wget.c - download an http:// or https:// URL to a file or stdout.
 *
 *   wget <url>              save as the last path component
 *   wget -O <file> <url>    save under a given name
 *   wget -O - <url>         write the body to stdout
 *   wget -q <url>           only errors
 *
 * HTTPS uses libtls with certificate and hostname verification.
 */

#include <kestrel.h>
#include <http.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH_MAX_  256
#define IO_CHUNK   4096

static const char *g_cwd = "/";

/* Pull the shell-injected "--cwd=<path>" argument, wherever it sits. */
static int strip_cwd_arg(int argc, char **argv)
{
    int i, out = 1;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) == 0)
            g_cwd = argv[i] + 6;
        else
            argv[out++] = argv[i];
    }
    return out;
}

static void resolve(const char *tok, char *out, unsigned long outsz)
{
    if (tok[0] == '/')
        snprintf(out, outsz, "%s", tok);
    else if (g_cwd[0] == '/' && g_cwd[1] == '\0')
        snprintf(out, outsz, "/%s", tok);
    else
        snprintf(out, outsz, "%s/%s", g_cwd, tok);
}

/* Last path component of the URL, minus any query string. "index.html"
 * when the URL names a directory. */
static void name_from_url(const char *url, char *out, unsigned long outsz)
{
    const char *p = url;
    const char *start;
    unsigned long n = 0;

    if (strncmp(p, "http://", 7) == 0)
        p += 7;
    start = p;
    while (*p && *p != '?' && *p != '#') {
        if (*p == '/')
            start = p + 1;
        p++;
    }
    while (start < p && n + 1 < outsz)
        out[n++] = *start++;
    out[n] = '\0';
    if (out[0] == '\0')
        snprintf(out, outsz, "index.html");
}

static int write_out(const char *path, const char *body, unsigned long len)
{
    unsigned long done = 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);

    if (fd < 0) {
        printf("wget: cannot create %s\n", path);
        return -1;
    }
    while (done < len) {
        unsigned long chunk = len - done;
        long n;
        if (chunk > IO_CHUNK)
            chunk = IO_CHUNK;
        n = write(fd, body + done, chunk);
        if (n <= 0) {
            printf("wget: write failed after %lu bytes\n", done);
            close(fd);
            return -1;
        }
        done += (unsigned long)n;
    }
    close(fd);
    return 0;
}

static void usage(void)
{
    printf("usage: wget [-q] [-O <file>|-] <url>\n");
}

int main(int argc, char **argv)
{
    char path[PATH_MAX_];
    char name[128];
    const char *url = 0;
    const char *outarg = 0;
    char *body = 0;
    unsigned long len = 0;
    unsigned long start, elapsed;
    int status = 0, quiet = 0, rc, i;

    argc = strip_cwd_arg(argc, argv);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-O") == 0) {
            if (++i >= argc) {
                usage();
                return 1;
            }
            outarg = argv[i];
        } else if (strcmp(argv[i], "-q") == 0) {
            quiet = 1;
        } else if (strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            printf("wget: unknown option '%s'\n", argv[i]);
            return 1;
        } else if (url == 0) {
            url = argv[i];
        } else {
            usage();
            return 1;
        }
    }
    if (url == 0) {
        usage();
        return 1;
    }

    if (!quiet)
        printf("wget: GET %s\n", url);

    start = uptime_ms();
    rc = http_get(url, &body, &len, &status);
    elapsed = uptime_ms() - start;
    if (rc != HTTP_OK) {
        printf("wget: %s: %s\n", url, http_strerror(rc));
        return 1;
    }

    if (!quiet)
        printf("wget: HTTP %d %s, %lu bytes in %lu ms\n", status,
               http_status_text(status), len, elapsed);

    if (status < 200 || status > 299) {
        printf("wget: server returned %d %s\n", status,
               http_status_text(status));
        free(body);
        return 1;
    }

    if (outarg && strcmp(outarg, "-") == 0) {
        unsigned long done = 0;
        while (done < len) {
            unsigned long chunk = len - done;
            long n;
            if (chunk > IO_CHUNK)
                chunk = IO_CHUNK;
            n = write(1, body + done, chunk);
            if (n <= 0)
                break;
            done += (unsigned long)n;
        }
        free(body);
        return 0;
    }

    if (outarg)
        resolve(outarg, path, sizeof(path));
    else {
        name_from_url(url, name, sizeof(name));
        resolve(name, path, sizeof(path));
    }

    if (write_out(path, body, len) < 0) {
        free(body);
        return 1;
    }
    free(body);
    if (!quiet)
        printf("wget: saved %lu bytes to %s\n", len, path);
    return 0;
}
