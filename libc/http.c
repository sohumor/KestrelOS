/* KestrelOS libc: a minimal HTTP/1.1 client over TCP or verified TLS 1.3.
 *
 * Deliberately small: GET only, one request per connection
 * (Connection: close), an in-memory body with a hard cap. It speaks
 * enough of HTTP/1.1 to talk to ordinary servers: absolute and relative
 * redirects, case-insensitive headers, Content-Length, chunked
 * transfer-encoding, and bodies delimited by connection close.
 *
 * Plain transport uses the TCP syscalls; https:// uses libtls.
 */

#include <http.h>
#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tls.h>

#define HTTP_HOST_MAX     256
#define HTTP_PATH_MAX     768
#define HTTP_LINE_MAX     1024
#define HTTP_CONNECT_MS   10000
#define HTTP_RECV_MS      15000
/* The kernel bounces TCP payloads through a 1400-byte staging buffer,
 * so there is no point asking for more than that in one call. */
#define HTTP_SEGMENT      1400
#define HTTP_BODY_START   8192

static char g_http_tls_error[160];

/* Internal: fetch_once() wants to be redirected. Positive so it can
 * never be confused with a public HTTP_E* code. */
#define HTTP_REDIRECT     1

/* ---- small helpers ------------------------------------------------- */

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* Case-insensitive "does s start with pfx". */
static int ci_prefix(const char *s, const char *pfx)
{
    while (*pfx) {
        if (lower((unsigned char)*s) != lower((unsigned char)*pfx))
            return 0;
        s++;
        pfx++;
    }
    return 1;
}

static int is_space(int c)
{
    return c == ' ' || c == '\t';
}

/* Value of a header line whose name we already matched: skip the name,
 * the colon and any leading blanks, then trim trailing blanks in place. */
static char *hdr_value(char *line, unsigned long namelen)
{
    char *v = line + namelen;
    unsigned long n;

    while (*v && is_space((unsigned char)*v))
        v++;
    n = strlen(v);
    while (n > 0 && is_space((unsigned char)v[n - 1]))
        v[--n] = '\0';
    return v;
}

static unsigned long parse_ulong(const char *s, int *ok)
{
    unsigned long v = 0;
    int digits = 0;

    while (*s >= '0' && *s <= '9') {
        if (v > (~0UL - 9) / 10) {       /* overflow: clamp and stop */
            v = ~0UL;
            digits = 1;
            break;
        }
        v = v * 10 + (unsigned long)(*s++ - '0');
        digits++;
    }
    if (ok)
        *ok = digits > 0;
    return v;
}

/* Leading hex digits of a chunk-size line ("1a3" or "1a3;ext=x"). */
static unsigned long parse_hex(const char *s, int *ok)
{
    unsigned long v = 0;
    int digits = 0;

    for (;;) {
        int c = lower((unsigned char)*s);
        int d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else
            break;
        if (v > (~0UL >> 4)) {
            if (ok)
                *ok = 0;
            return 0;
        }
        v = (v << 4) | (unsigned long)d;
        digits++;
        s++;
    }
    if (ok)
        *ok = digits > 0;
    return v;
}

/* ---- URL splitting -------------------------------------------------- */

/* Split "http[s]://host[:port][/path]" into its parts. */
static int url_split(const char *url, char *host, unsigned long hostsz,
                     uint16_t *port, int *secure,
                     char *path, unsigned long pathsz)
{
    const char *p, *h;
    unsigned long n;

    if (url == 0)
        return HTTP_EURL;
    if (ci_prefix(url, "https://")) {
        *secure = 1;
        h = url + 8;
    } else if (ci_prefix(url, "http://")) {
        *secure = 0;
        h = url + 7;
    } else {
        /* Anything with a "scheme://" is a scheme we do not speak; a URL
         * with no scheme at all is simply malformed. */
        return strstr(url, "://") ? HTTP_ESCHEME : HTTP_EURL;
    }

    p = h;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#')
        p++;
    n = (unsigned long)(p - h);
    if (n == 0 || n >= hostsz)
        return HTTP_EURL;
    memcpy(host, h, n);
    host[n] = '\0';

    *port = *secure ? 443 : 80;
    if (*p == ':') {
        int ok;
        unsigned long v = parse_ulong(p + 1, &ok);
        if (!ok || v == 0 || v > 65535)
            return HTTP_EURL;
        *port = (uint16_t)v;
        p++;
        while (*p >= '0' && *p <= '9')
            p++;
    }

    if (*p == '\0' || *p == '#') {
        if (pathsz < 2)
            return HTTP_EURL;
        path[0] = '/';
        path[1] = '\0';
        return HTTP_OK;
    }
    if (*p != '/' && *p != '?')
        return HTTP_EURL;

    /* Drop any fragment; it is never sent to the server. */
    h = p;
    while (*p && *p != '#')
        p++;
    n = (unsigned long)(p - h);
    if (n + 2 >= pathsz)
        return HTTP_EURL;
    if (*h == '?') {
        path[0] = '/';
        memcpy(path + 1, h, n);
        path[n + 1] = '\0';
    } else {
        memcpy(path, h, n);
        path[n] = '\0';
    }
    return HTTP_OK;
}

/* ---- growable body buffer ------------------------------------------- */

struct hbuf {
    char *p;
    unsigned long len;
    unsigned long cap;
};

static int hb_reserve(struct hbuf *b, unsigned long want)
{
    unsigned long cap;
    char *np;

    if (want <= b->cap)
        return HTTP_OK;
    if (want > HTTP_MAX_BODY)
        return HTTP_ETOOBIG;
    cap = b->cap ? b->cap : HTTP_BODY_START;
    while (cap < want) {
        if (cap > HTTP_MAX_BODY / 2) {
            cap = HTTP_MAX_BODY;
            break;
        }
        cap *= 2;
    }
    /* One spare byte so the body is always NUL-terminated. */
    np = realloc(b->p, cap + 1);
    if (np == 0)
        return HTTP_ENOMEM;
    b->p = np;
    b->cap = cap;
    return HTTP_OK;
}

static int hb_add(struct hbuf *b, const void *src, unsigned long n)
{
    int rc;

    if (n == 0)
        return HTTP_OK;
    if (b->len + n > HTTP_MAX_BODY)
        return HTTP_ETOOBIG;
    rc = hb_reserve(b, b->len + n);
    if (rc != HTTP_OK)
        return rc;
    memcpy(b->p + b->len, src, n);
    b->len += n;
    b->p[b->len] = '\0';
    return HTTP_OK;
}

static void hb_free(struct hbuf *b)
{
    free(b->p);
    b->p = 0;
    b->len = 0;
    b->cap = 0;
}

/* ---- buffered connection -------------------------------------------- */

struct hconn {
    int handle;
    struct tls_conn *tls;
    int secure;
    int len;
    int pos;
    int eof;                       /* peer closed cleanly */
    int err;                       /* timeout or reset */
    unsigned char buf[HTTP_SEGMENT];
};

/* Make at least one byte available. Returns 1 if there is data. */
static int hc_fill(struct hconn *c)
{
    long n;

    if (c->pos < c->len)
        return 1;
    if (c->eof || c->err)
        return 0;
    if (c->secure) {
        tls_set_timeout(c->tls, HTTP_RECV_MS);
        n = tls_read(c->tls, c->buf, (int)sizeof(c->buf));
    } else {
        n = syscall(SYS_TCP_RECV, c->handle, (long)c->buf,
                    (long)sizeof(c->buf), HTTP_RECV_MS);
    }
    if (n == 0) {
        c->eof = 1;
        return 0;
    }
    if (n < 0) {
        c->err = 1;
        return 0;
    }
    c->pos = 0;
    c->len = (int)n;
    return 1;
}

/* Read one CRLF- or LF-terminated line into out (NUL-terminated, the
 * terminator stripped). Returns the length, or -1 at end of stream, or
 * -2 if the line did not fit. */
static long hc_line(struct hconn *c, char *out, unsigned long outsz)
{
    unsigned long n = 0;
    int got = 0;

    for (;;) {
        int ch;
        if (!hc_fill(c))
            break;
        ch = c->buf[c->pos++];
        got = 1;
        if (ch == '\n') {
            if (n > 0 && out[n - 1] == '\r')
                n--;
            out[n] = '\0';
            return (long)n;
        }
        if (n + 1 >= outsz)
            return -2;
        out[n++] = (char)ch;
    }
    if (!got)
        return -1;
    out[n] = '\0';                 /* last line without a terminator */
    return (long)n;
}

/* Copy up to max buffered/received bytes. Returns 0 at end of stream. */
static long hc_read(struct hconn *c, void *dst, unsigned long max)
{
    unsigned long avail;

    if (max == 0)
        return 0;
    if (!hc_fill(c))
        return 0;
    avail = (unsigned long)(c->len - c->pos);
    if (avail > max)
        avail = max;
    memcpy(dst, c->buf + c->pos, avail);
    c->pos += (int)avail;
    return (long)avail;
}

/* ---- request/response ------------------------------------------------ */

static void hc_close(struct hconn *c)
{
    if (c->secure && c->tls) {
        tls_close(c->tls);
        c->tls = 0;
    } else if (c->handle >= 0) {
        syscall(SYS_TCP_CLOSE, c->handle, 0, 0, 0);
        c->handle = -1;
    }
}

static int send_all(struct hconn *c, const char *buf, unsigned long len)
{
    unsigned long done = 0;

    while (done < len) {
        unsigned long chunk = len - done;
        long n;
        if (chunk > HTTP_SEGMENT)
            chunk = HTTP_SEGMENT;
        if (c->secure)
            n = tls_write(c->tls, buf + done, (int)chunk);
        else
            n = syscall(SYS_TCP_SEND, c->handle, (long)(buf + done),
                        (long)chunk, 0);
        if (n <= 0)
            return HTTP_ESEND;
        done += (unsigned long)n;
    }
    return HTTP_OK;
}

static int read_body_length(struct hconn *c, struct hbuf *b, unsigned long want)
{
    char tmp[HTTP_SEGMENT];

    if (want > HTTP_MAX_BODY)
        return HTTP_ETOOBIG;
    while (b->len < want) {
        unsigned long need = want - b->len;
        long n;
        if (need > sizeof(tmp))
            need = sizeof(tmp);
        n = hc_read(c, tmp, need);
        if (n <= 0)
            return HTTP_ERECV;         /* short body: the server lied */
        {
            int rc = hb_add(b, tmp, (unsigned long)n);
            if (rc != HTTP_OK)
                return rc;
        }
    }
    return HTTP_OK;
}

static int read_body_close(struct hconn *c, struct hbuf *b)
{
    char tmp[HTTP_SEGMENT];

    for (;;) {
        long n = hc_read(c, tmp, sizeof(tmp));
        int rc;
        if (n <= 0)
            break;
        rc = hb_add(b, tmp, (unsigned long)n);
        if (rc != HTTP_OK)
            return rc;
    }
    /* A body delimited by close ends at the FIN; a timeout after we have
     * data is treated the same way rather than throwing the body out. */
    if (c->err && b->len == 0)
        return HTTP_ERECV;
    return HTTP_OK;
}

static int read_body_chunked(struct hconn *c, struct hbuf *b)
{
    char line[HTTP_LINE_MAX];
    char tmp[HTTP_SEGMENT];

    for (;;) {
        unsigned long size;
        int ok;
        long ln = hc_line(c, line, sizeof(line));
        if (ln < 0)
            return HTTP_ERECV;
        size = parse_hex(line, &ok);
        if (!ok)
            return HTTP_EPROTO;
        if (size == 0)
            break;
        while (size > 0) {
            unsigned long need = size > sizeof(tmp) ? sizeof(tmp) : size;
            long n = hc_read(c, tmp, need);
            int rc;
            if (n <= 0)
                return HTTP_ERECV;
            rc = hb_add(b, tmp, (unsigned long)n);
            if (rc != HTTP_OK)
                return rc;
            size -= (unsigned long)n;
        }
        if (hc_line(c, line, sizeof(line)) < 0)  /* CRLF after the chunk */
            return HTTP_ERECV;
    }
    /* Trailer: header lines up to a blank one. Absent trailers still end
     * with one empty line, and a truncated stream here is harmless. */
    for (;;) {
        long ln = hc_line(c, line, sizeof(line));
        if (ln <= 0)
            break;
    }
    return HTTP_OK;
}

/* Turn a Location: value into an absolute URL in `out`, using
 * the request it came from as the base. */
static int resolve_location(const char *loc, const char *host, uint16_t port,
                            int secure, const char *path,
                            char *out, unsigned long outsz)
{
    char base[HTTP_HOST_MAX + 16];
    int n;

    if (loc[0] == '\0')
        return HTTP_EREDIR;
    if (ci_prefix(loc, "https://") || ci_prefix(loc, "http://")) {
        if (strlen(loc) + 1 > outsz)
            return HTTP_EURL;
        strcpy(out, loc);
        return HTTP_OK;
    }
    if (strstr(loc, "://"))
        return HTTP_ESCHEME;

    if ((!secure && port == 80) || (secure && port == 443))
        n = snprintf(base, sizeof(base), "%s://%s",
                     secure ? "https" : "http", host);
    else
        n = snprintf(base, sizeof(base), "%s://%s:%u",
                     secure ? "https" : "http", host, (unsigned)port);
    if (n < 0 || (unsigned long)n >= sizeof(base))
        return HTTP_EURL;

    if (loc[0] == '/')
        n = snprintf(out, outsz, "%s%s", base, loc);
    else {
        /* Relative to the directory part of the current path. */
        const char *slash = strrchr(path, '/');
        unsigned long dirlen = slash ? (unsigned long)(slash - path) + 1 : 1;
        char dir[HTTP_PATH_MAX];
        if (dirlen >= sizeof(dir))
            return HTTP_EURL;
        memcpy(dir, path, dirlen);
        dir[dirlen] = '\0';
        if (dir[0] != '/') {
            dir[0] = '/';
            dir[1] = '\0';
        }
        n = snprintf(out, outsz, "%s%s%s", base, dir, loc);
    }
    if (n < 0 || (unsigned long)n >= outsz)
        return HTTP_EURL;
    return HTTP_OK;
}

/* One request/response exchange. Returns HTTP_OK (body filled),
 * HTTP_REDIRECT (next filled, body untouched), or a negative error. */
static int fetch_once(const char *url, struct hbuf *body, int *status,
                      char *next, unsigned long nextsz)
{
    char host[HTTP_HOST_MAX];
    char path[HTTP_PATH_MAX];
    char req[HTTP_HOST_MAX + HTTP_PATH_MAX + 128];
    char line[HTTP_LINE_MAX];
    char location[HTTP_LINE_MAX];
    struct hconn c;
    uint16_t port = 80;
    int secure = 0;
    uint32_t ip = 0;
    unsigned long clen = 0;
    int have_clen = 0, chunked = 0, have_loc = 0;
    int code = 0, rc, n;

    memset(&c, 0, sizeof(c));
    c.handle = -1;

    rc = url_split(url, host, sizeof(host), &port, &secure,
                   path, sizeof(path));
    if (rc != HTTP_OK)
        return rc;

    if (secure) {
        struct tls_options options;
        struct tls_error error;
        tls_options_default(&options);
        options.alpn = "http/1.1";
        options.timeout_ms = HTTP_CONNECT_MS;
        c.tls = tls_connect(host, port, &options, &error);
        if (!c.tls) {
            snprintf(g_http_tls_error, sizeof(g_http_tls_error), "%s",
                     error.msg[0] ? error.msg : "TLS handshake failed");
            return HTTP_ETLS;
        }
        g_http_tls_error[0] = '\0';
        c.secure = 1;
    } else {
        ip = ip_aton(host);
        if (ip == 0 && dns_resolve(host, &ip) < 0)
            return HTTP_EDNS;
        if (ip == 0)
            return HTTP_EDNS;
        c.handle = (int)syscall(SYS_TCP_CONNECT, (long)ip, port,
                                HTTP_CONNECT_MS, 0);
        if (c.handle < 0)
            return HTTP_ECONNECT;
    }

    if ((!secure && port == 80) || (secure && port == 443))
        n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: KestrelOS-http/1.0\r\n"
                     "Accept: */*\r\n"
                     "Connection: close\r\n\r\n",
                     path, host);
    else
        n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s:%u\r\n"
                     "User-Agent: KestrelOS-http/1.0\r\n"
                     "Accept: */*\r\n"
                     "Connection: close\r\n\r\n",
                     path, host, (unsigned)port);
    if (n < 0 || (unsigned long)n >= sizeof(req)) {
        hc_close(&c);
        return HTTP_EURL;
    }

    rc = send_all(&c, req, (unsigned long)n);
    if (rc != HTTP_OK) {
        hc_close(&c);
        return rc;
    }

    location[0] = '\0';

    /* Status line: "HTTP/1.x <code> <reason>". */
    if (hc_line(&c, line, sizeof(line)) < 0) {
        hc_close(&c);
        return c.err ? HTTP_ERECV : HTTP_EPROTO;
    }
    if (!ci_prefix(line, "HTTP/1.")) {
        hc_close(&c);
        return HTTP_EPROTO;
    }
    {
        const char *sp = strchr(line, ' ');
        int ok;
        if (sp == 0) {
            hc_close(&c);
            return HTTP_EPROTO;
        }
        while (*sp == ' ')
            sp++;
        code = (int)parse_ulong(sp, &ok);
        if (!ok || code < 100 || code > 599) {
            hc_close(&c);
            return HTTP_EPROTO;
        }
    }

    /* Headers up to the blank line. */
    for (;;) {
        long ln = hc_line(&c, line, sizeof(line));
        if (ln == -2) {                 /* absurd header: give up cleanly */
            hc_close(&c);
            return HTTP_EPROTO;
        }
        if (ln < 0) {
            hc_close(&c);
            return HTTP_ERECV;
        }
        if (ln == 0)
            break;
        if (ci_prefix(line, "content-length:")) {
            int ok;
            clen = parse_ulong(hdr_value(line, 15), &ok);
            have_clen = ok;
        } else if (ci_prefix(line, "transfer-encoding:")) {
            char *v = hdr_value(line, 18);
            char *q;
            for (q = v; *q; q++)
                *q = (char)lower((unsigned char)*q);
            if (strstr(v, "chunked"))
                chunked = 1;
        } else if (ci_prefix(line, "location:")) {
            char *v = hdr_value(line, 9);
            snprintf(location, sizeof(location), "%s", v);
            have_loc = location[0] != '\0';
        }
    }

    if ((code == 301 || code == 302 || code == 303 ||
         code == 307 || code == 308) && have_loc) {
        rc = resolve_location(location, host, port, secure, path,
                              next, nextsz);
        hc_close(&c);
        return rc == HTTP_OK ? HTTP_REDIRECT : rc;
    }

    /* 204 and 304 never carry a body, whatever the headers claim. */
    if (code == 204 || code == 304)
        rc = HTTP_OK;
    else if (chunked)
        rc = read_body_chunked(&c, body);
    else if (have_clen)
        rc = read_body_length(&c, body, clen);
    else
        rc = read_body_close(&c, body);

    hc_close(&c);
    if (rc != HTTP_OK)
        return rc;
    if (status)
        *status = code;
    return HTTP_OK;
}

int http_get(const char *url, char **body, unsigned long *len, int *status)
{
    char cur[HTTP_URL_MAX];
    char next[HTTP_URL_MAX];
    struct hbuf b;
    int hops = 0, rc, code = 0;

    if (url == 0 || body == 0 || len == 0)
        return HTTP_EURL;
    if (strlen(url) + 1 > sizeof(cur))
        return HTTP_EURL;
    strcpy(cur, url);

    memset(&b, 0, sizeof(b));
    for (;;) {
        rc = fetch_once(cur, &b, &code, next, sizeof(next));
        if (rc == HTTP_REDIRECT) {
            if (++hops > HTTP_MAX_REDIRECTS) {
                hb_free(&b);
                return HTTP_EREDIR;
            }
            /* Redirect bodies are discarded; only the final one counts. */
            b.len = 0;
            if (b.p)
                b.p[0] = '\0';
            strcpy(cur, next);
            continue;
        }
        break;
    }
    if (rc != HTTP_OK) {
        hb_free(&b);
        return rc;
    }

    /* Always hand back a non-0, NUL-terminated buffer. */
    if (b.p == 0) {
        b.p = malloc(1);
        if (b.p == 0)
            return HTTP_ENOMEM;
        b.p[0] = '\0';
    }
    *body = b.p;
    *len = b.len;
    if (status)
        *status = code;
    return HTTP_OK;
}

const char *http_strerror(int err)
{
    switch (err) {
    case HTTP_OK:       return "ok";
    case HTTP_EURL:     return "malformed URL";
    case HTTP_EHTTPS:   return "legacy HTTPS capability error";
    case HTTP_ESCHEME:  return "unsupported URL scheme (use http:// or https://)";
    case HTTP_EDNS:     return "host name did not resolve";
    case HTTP_ECONNECT: return "could not connect";
    case HTTP_ESEND:    return "could not send the request";
    case HTTP_ERECV:    return "connection closed before the response ended";
    case HTTP_EPROTO:   return "malformed HTTP response";
    case HTTP_ETOOBIG:  return "response body larger than 8 MiB";
    case HTTP_EREDIR:   return "too many redirects";
    case HTTP_ENOMEM:   return "out of memory";
    case HTTP_ETLS:     return g_http_tls_error[0]
                              ? g_http_tls_error : "TLS handshake failed";
    default:            return "unknown error";
    }
}

const char *http_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 408: return "Request Timeout";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default:  return "Unknown";
    }
}
