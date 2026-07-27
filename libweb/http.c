/* KestrelOS libweb: the HTTP/1.1 client.
 *
 * Layering, from the bottom:
 *
 *   struct http_transport   byte pipe; plain TCP here, TLS elsewhere
 *   struct hconn            buffered reader + the pool entry
 *   exchange()              one request/response on one connection
 *   http_fetch()            redirects, cookies, cache, retries
 *
 * Nothing in this file recurses and every buffer has a ceiling, so the
 * worst case is bounded even for a server that is actively hostile: the
 * deepest stack frame is http_fetch() at roughly 8 KiB (two struct urls
 * and a URL string), and the header block, the body and the compressed
 * staging buffer are all heap with explicit caps.
 */

#include "http.h"
#include "cookie.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HTTP_HOST
#include <time.h>
static unsigned long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000UL +
           (unsigned long)(ts.tv_nsec / 1000000L);
}
static long now_sec(void) { return (long)time(0); }
#else
#include <kestrel.h>
static unsigned long now_ms(void) { return uptime_ms(); }
static long now_sec(void) { return syscall(SYS_TIME, 0, 0, 0, 0); }
#endif

#ifdef HTTP_HAVE_INFLATE
extern int inflate_buf(const void *src, unsigned long slen, void **out,
                       unsigned long *olen, int wrapper);
#endif

/* Largest request head we will build. */
#define REQ_HEAD_MAX 16384
/* Largest compressed body we will stage before inflating. */
#define CODED_MAX (8UL * 1024UL * 1024UL)
/* Chunk handed to the sink when replaying a decoded body. */
#define EMIT_CHUNK 16384

/* ---- ASCII helpers --------------------------------------------------- */

static int h_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int h_space(int c)
{
    return c == ' ' || c == '\t';
}

static int h_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (h_lower((unsigned char)*a) != h_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int ci_prefix(const char *s, const char *pfx)
{
    while (*pfx) {
        if (h_lower((unsigned char)*s) != h_lower((unsigned char)*pfx))
            return 0;
        s++;
        pfx++;
    }
    return 1;
}

/* Case-insensitive substring, for comma lists like Cache-Control. */
static const char *ci_str(const char *hay, const char *needle)
{
    unsigned long n = strlen(needle);

    if (n == 0)
        return hay;
    for (; *hay; hay++)
        if (ci_prefix(hay, needle))
            return hay;
    (void)n;
    return 0;
}

static unsigned long parse_ul(const char *s, int *ok)
{
    unsigned long v = 0;
    int digits = 0;

    while (h_space((unsigned char)*s))
        s++;
    while (h_digit((unsigned char)*s)) {
        if (v > (~0UL - 9) / 10) {
            if (ok)
                *ok = 0;
            return ~0UL;
        }
        v = v * 10 + (unsigned long)(*s++ - '0');
        digits++;
    }
    if (ok)
        *ok = digits > 0;
    return v;
}

static unsigned long parse_hex(const char *s, int *ok)
{
    unsigned long v = 0;
    int digits = 0;

    for (;;) {
        int c = h_lower((unsigned char)*s), d;
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

static unsigned long fnv1a(const char *s)
{
    unsigned long h = 1469598103934665603UL;

    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211UL;
    }
    return h;
}

/* ---- growable byte buffer -------------------------------------------- */

struct hbuf {
    char *p;
    unsigned long len;
    unsigned long cap;
    unsigned long max;
};

static void hb_init(struct hbuf *b, unsigned long max)
{
    b->p = 0;
    b->len = 0;
    b->cap = 0;
    b->max = max;
}

static int hb_reserve(struct hbuf *b, unsigned long want)
{
    unsigned long cap;
    char *np;

    if (want <= b->cap)
        return HTTP_OK;
    if (want > b->max)
        return HTTP_E_TOOBIG;
    cap = b->cap ? b->cap : 1024;
    while (cap < want) {
        if (cap > b->max / 2) {
            cap = b->max;
            break;
        }
        cap *= 2;
    }
    np = realloc(b->p, cap + 1);
    if (np == 0)
        return HTTP_E_NOMEM;
    b->p = np;
    b->cap = cap;
    return HTTP_OK;
}

static int hb_add(struct hbuf *b, const void *src, unsigned long n)
{
    int rc;

    if (n == 0)
        return HTTP_OK;
    if (b->len + n > b->max || b->len + n < b->len)
        return HTTP_E_TOOBIG;
    rc = hb_reserve(b, b->len + n);
    if (rc != HTTP_OK)
        return rc;
    memcpy(b->p + b->len, src, n);
    b->len += n;
    b->p[b->len] = '\0';
    return HTTP_OK;
}

static int hb_str(struct hbuf *b, const char *s)
{
    return hb_add(b, s, strlen(s));
}

static void hb_free(struct hbuf *b)
{
    free(b->p);
    b->p = 0;
    b->len = 0;
    b->cap = 0;
}

/* ---- scheme registry ------------------------------------------------- */

#define SCHEME_MAX 8

struct scheme_ent {
    char name[16];
    http_transport_fn fn;
    void *user;
};

static struct scheme_ent g_schemes[SCHEME_MAX];
static int g_nschemes;
static http_inflate_fn g_inflate =
#ifdef HTTP_HAVE_INFLATE
    inflate_buf;
#else
    0;
#endif

int http_register_scheme(const char *scheme, http_transport_fn fn, void *user)
{
    int i;

    if (scheme == 0 || *scheme == '\0' || strlen(scheme) >= 16)
        return HTTP_E_INVAL;
    for (i = 0; i < g_nschemes; i++) {
        if (ci_eq(g_schemes[i].name, scheme)) {
            g_schemes[i].fn = fn;
            g_schemes[i].user = user;
            return HTTP_OK;
        }
    }
    if (fn == 0)
        return HTTP_OK;
    if (g_nschemes >= SCHEME_MAX)
        return HTTP_E_INVAL;
    strcpy(g_schemes[g_nschemes].name, scheme);
    g_schemes[g_nschemes].fn = fn;
    g_schemes[g_nschemes].user = user;
    g_nschemes++;
    return HTTP_OK;
}

static struct scheme_ent *scheme_find(const char *scheme)
{
    int i;

    for (i = 0; i < g_nschemes; i++)
        if (ci_eq(g_schemes[i].name, scheme) && g_schemes[i].fn)
            return &g_schemes[i];
    return 0;
}

void http_set_inflate(http_inflate_fn fn)
{
    g_inflate = fn;
}

/* ---- built-in TCP transport ------------------------------------------ */

#ifdef HTTP_HOST

int http_tcp_transport(const char *host, int port, int timeout_ms,
                       void *user, struct http_transport *out)
{
    (void)host;
    (void)port;
    (void)timeout_ms;
    (void)user;
    (void)out;
    return HTTP_E_SCHEME;   /* the host test harness registers its own */
}

#else

struct tcp_ctx {
    int handle;
    int timeout_ms;
};

static int tcp_read(void *ctx, void *buf, int len)
{
    struct tcp_ctx *t = ctx;
    long n = syscall(SYS_TCP_RECV, t->handle, (long)buf, (long)len,
                     t->timeout_ms);

    if (n < 0)
        return HTTP_E_TIMEOUT;
    return (int)n;
}

static int tcp_write(void *ctx, const void *buf, int len)
{
    struct tcp_ctx *t = ctx;
    long n;

    /* The kernel stages TCP payloads through a 1400-byte buffer. */
    if (len > 1400)
        len = 1400;
    n = syscall(SYS_TCP_SEND, t->handle, (long)buf, (long)len, 0);
    if (n <= 0)
        return HTTP_E_SEND;
    return (int)n;
}

static void tcp_close(void *ctx)
{
    struct tcp_ctx *t = ctx;

    syscall(SYS_TCP_CLOSE, t->handle, 0, 0, 0);
    free(t);
}

static int tcp_set_timeout(void *ctx, int ms)
{
    struct tcp_ctx *t = ctx;

    t->timeout_ms = ms > 0 ? ms : 1;
    return 0;
}

int http_tcp_transport(const char *host, int port, int timeout_ms,
                       void *user, struct http_transport *out)
{
    struct tcp_ctx *t;
    uint32_t ip;
    int handle;

    (void)user;
    if (host == 0 || port <= 0 || out == 0)
        return HTTP_E_INVAL;
    ip = ip_aton(host);
    if (ip == 0 && dns_resolve(host, &ip) < 0)
        return HTTP_E_DNS;
    if (ip == 0)
        return HTTP_E_DNS;

    handle = (int)syscall(SYS_TCP_CONNECT, (long)ip, port,
                          timeout_ms > 0 ? timeout_ms : HTTP_T_CONNECT, 0);
    if (handle < 0)
        return HTTP_E_CONNECT;

    t = malloc(sizeof(*t));
    if (t == 0) {
        syscall(SYS_TCP_CLOSE, handle, 0, 0, 0);
        return HTTP_E_NOMEM;
    }
    t->handle = handle;
    t->timeout_ms = HTTP_T_BODY;
    out->ctx = t;
    out->read = tcp_read;
    out->write = tcp_write;
    out->close = tcp_close;
    out->set_timeout = tcp_set_timeout;
    return HTTP_OK;
}

#endif /* HTTP_HOST */

/* ---- buffered connection --------------------------------------------- */

struct hconn {
    char origin[URL_HOST_MAX + 32];
    struct http_transport t;
    unsigned char buf[HTTP_RXBUF];
    int pos, len;
    int eof;
    int broken;
    int open;
    int pooled;
    unsigned long idle_at;
};

struct http_client {
    struct hconn pool[HTTP_POOL_MAX];
    struct cookie_jar *jar;
    struct http_cache *cache;
    char agent[96];
    struct http_stats st;
};

static void hc_shutdown(struct hconn *c)
{
    if (c->open && c->t.close)
        c->t.close(c->t.ctx);
    c->open = 0;
    c->pooled = 0;
    c->pos = c->len = 0;
    c->eof = 0;
    c->broken = 0;
    c->origin[0] = '\0';
}

/* Arm the transport's read deadline. */
static int hc_arm(struct hconn *c, unsigned long deadline)
{
    long remain;

    if (deadline == 0)
        return HTTP_OK;
    remain = (long)(deadline - now_ms());
    if (remain <= 0)
        return HTTP_E_TIMEOUT;
    if (remain > 0x7ffffff0L)
        remain = 0x7ffffff0L;
    if (c->t.set_timeout)
        c->t.set_timeout(c->t.ctx, (int)remain);
    return HTTP_OK;
}

/* Make at least one byte available. 1 = data, 0 = clean EOF, <0 = error. */
static int hc_fill(struct hconn *c, unsigned long deadline,
                   struct http_client *cl)
{
    int n, rc;

    if (c->pos < c->len)
        return 1;
    if (c->eof)
        return 0;
    if (c->broken)
        return HTTP_E_RECV;
    rc = hc_arm(c, deadline);
    if (rc != HTTP_OK) {
        c->broken = 1;
        return rc;
    }
    n = c->t.read(c->t.ctx, c->buf, (int)sizeof(c->buf));
    if (n == 0) {
        c->eof = 1;
        return 0;
    }
    if (n < 0) {
        c->broken = 1;
        return n == HTTP_E_TIMEOUT ? HTTP_E_TIMEOUT : HTTP_E_RECV;
    }
    c->pos = 0;
    c->len = n;
    if (cl)
        cl->st.bytes_in += (unsigned long)n;
    return 1;
}

/* One CRLF/LF-terminated line, terminator stripped. >= 0 length, else
 * HTTP_E_RECV (stream ended), HTTP_E_TOOBIG or HTTP_E_TIMEOUT. */
static long hc_line(struct hconn *c, char *out, unsigned long outsz,
                    unsigned long deadline, struct http_client *cl)
{
    unsigned long n = 0;

    for (;;) {
        int r = hc_fill(c, deadline, cl);
        int ch;

        if (r < 0)
            return r;
        if (r == 0)
            return n > 0 ? HTTP_E_PROTO : HTTP_E_RECV;
        ch = c->buf[c->pos++];
        if (ch == '\n') {
            if (n > 0 && out[n - 1] == '\r')
                n--;
            out[n] = '\0';
            return (long)n;
        }
        if (n + 1 >= outsz)
            return HTTP_E_TOOBIG;
        out[n++] = (char)ch;
    }
}

/* Up to `max` bytes. 0 = end of stream, <0 = error. */
static long hc_read(struct hconn *c, void *dst, unsigned long max,
                    unsigned long deadline, struct http_client *cl)
{
    unsigned long avail;
    int r;

    if (max == 0)
        return 0;
    r = hc_fill(c, deadline, cl);
    if (r <= 0)
        return r;
    avail = (unsigned long)(c->len - c->pos);
    if (avail > max)
        avail = max;
    memcpy(dst, c->buf + c->pos, avail);
    c->pos += (int)avail;
    return (long)avail;
}

static int hc_send(struct hconn *c, const void *buf, unsigned long len,
                   struct http_client *cl)
{
    const char *p = buf;
    unsigned long done = 0;

    while (done < len) {
        unsigned long chunk = len - done;
        int n;
        if (chunk > 0x40000000UL)
            chunk = 0x40000000UL;
        n = c->t.write(c->t.ctx, p + done, (int)chunk);
        if (n <= 0) {
            c->broken = 1;
            return HTTP_E_SEND;
        }
        done += (unsigned long)n;
    }
    if (cl)
        cl->st.bytes_out += len;
    return HTTP_OK;
}

/* ---- connection pool -------------------------------------------------- */

static struct hconn *pool_take(struct http_client *c, const char *origin)
{
    int i;
    unsigned long now = now_ms();

    /* Retire anything that sat too long first. */
    for (i = 0; i < HTTP_POOL_MAX; i++) {
        struct hconn *e = &c->pool[i];
        if (e->open && e->pooled && now - e->idle_at > HTTP_IDLE_MS)
            hc_shutdown(e);
    }
    for (i = 0; i < HTTP_POOL_MAX; i++) {
        struct hconn *e = &c->pool[i];
        if (e->open && e->pooled && !e->broken && !e->eof &&
            strcmp(e->origin, origin) == 0) {
            e->pooled = 0;
            return e;
        }
    }
    return 0;
}

static struct hconn *pool_slot(struct http_client *c)
{
    int i, oldest = -1;

    for (i = 0; i < HTTP_POOL_MAX; i++)
        if (!c->pool[i].open)
            return &c->pool[i];
    for (i = 0; i < HTTP_POOL_MAX; i++)
        if (c->pool[i].pooled &&
            (oldest < 0 || c->pool[i].idle_at < c->pool[oldest].idle_at))
            oldest = i;
    if (oldest < 0)
        return 0;
    hc_shutdown(&c->pool[oldest]);
    return &c->pool[oldest];
}

void http_client_drop_connections(struct http_client *c)
{
    int i;

    if (c == 0)
        return;
    for (i = 0; i < HTTP_POOL_MAX; i++)
        hc_shutdown(&c->pool[i]);
}

/* ---- header block ----------------------------------------------------- */

/* `raw` holds unfolded "Name: value\n" lines. Split it into the response's
 * name/value array; the array points into a private copy so the caller
 * may keep using `raw` (the cache wants it verbatim). */
static int headers_build(struct http_response *r, const struct hbuf *raw)
{
    char *block, *p;
    int n = 0, i;

    free(r->headers);
    free(r->_hdrblock);
    r->headers = 0;
    r->_hdrblock = 0;
    r->nheaders = 0;
    if (raw->len == 0)
        return HTTP_OK;

    block = malloc(raw->len + 1);
    if (block == 0)
        return HTTP_E_NOMEM;
    memcpy(block, raw->p, raw->len);
    block[raw->len] = '\0';

    for (p = block; *p; p++)
        if (*p == '\n')
            n++;
    if (n == 0)
        n = 1;
    if (n > HTTP_HEADER_COUNT)
        n = HTTP_HEADER_COUNT;
    r->headers = malloc((unsigned long)n * sizeof(*r->headers));
    if (r->headers == 0) {
        free(block);
        return HTTP_E_NOMEM;
    }
    r->_hdrblock = block;

    p = block;
    i = 0;
    while (*p && i < n) {
        char *nl = strchr(p, '\n');
        char *colon;
        if (nl)
            *nl = '\0';
        colon = strchr(p, ':');
        if (colon) {
            char *v = colon + 1;
            unsigned long vl;
            *colon = '\0';
            while (*v && h_space((unsigned char)*v))
                v++;
            vl = strlen(v);
            while (vl > 0 && h_space((unsigned char)v[vl - 1]))
                v[--vl] = '\0';
            r->headers[i].name = p;
            r->headers[i].value = v;
            i++;
        }
        if (nl == 0)
            break;
        p = nl + 1;
    }
    r->nheaders = i;
    return HTTP_OK;
}

const char *http_header_get(const struct http_response *r, const char *name)
{
    return http_header_nth(r, name, 0);
}

const char *http_header_nth(const struct http_response *r, const char *name,
                            int n)
{
    int i;

    if (r == 0 || name == 0)
        return 0;
    for (i = 0; i < r->nheaders; i++) {
        if (ci_eq(r->headers[i].name, name)) {
            if (n == 0)
                return r->headers[i].value;
            n--;
        }
    }
    return 0;
}

void http_response_free(struct http_response *r)
{
    if (r == 0)
        return;
    free(r->reason);
    free(r->headers);
    free(r->_hdrblock);
    free(r->body);
    free(r->final_url);
    memset(r, 0, sizeof(*r));
}

/* ---- body plumbing ---------------------------------------------------- */

struct body_out {
    http_sink_fn sink;
    void *sink_user;
    struct hbuf mem;         /* accumulation when there is no sink */
    struct hbuf tee;         /* copy for the cache */
    int tee_on;
    unsigned long total;
    unsigned long max;
};

static int out_write(struct body_out *o, const void *d, unsigned long n)
{
    int rc;

    if (n == 0)
        return HTTP_OK;
    if (o->total + n > o->max)
        return HTTP_E_TOOBIG;
    o->total += n;
    if (o->tee_on) {
        if (hb_add(&o->tee, d, n) != HTTP_OK) {
            o->tee_on = 0;               /* too big to cache: give up on it */
            hb_free(&o->tee);
        }
    }
    if (o->sink) {
        if (o->sink(o->sink_user, d, n) != 0)
            return HTTP_E_ABORT;
        return HTTP_OK;
    }
    rc = hb_add(&o->mem, d, n);
    return rc;
}

/* Raw sink used while reading the wire body. */
typedef int (*wire_sink)(void *u, const void *d, unsigned long n);

static int sink_to_out(void *u, const void *d, unsigned long n)
{
    return out_write((struct body_out *)u, d, n);
}

static int sink_to_buf(void *u, const void *d, unsigned long n)
{
    return hb_add((struct hbuf *)u, d, n);
}

static int read_body_length(struct hconn *c, unsigned long want,
                            wire_sink fn, void *u, unsigned long deadline,
                            struct http_client *cl)
{
    char tmp[HTTP_RXBUF];
    unsigned long got = 0;

    while (got < want) {
        unsigned long need = want - got;
        long n;
        int rc;
        if (need > sizeof(tmp))
            need = sizeof(tmp);
        n = hc_read(c, tmp, need, deadline, cl);
        if (n < 0)
            return (int)n;
        if (n == 0)
            return HTTP_E_RECV;          /* short body: the server lied */
        rc = fn(u, tmp, (unsigned long)n);
        if (rc != HTTP_OK)
            return rc;
        got += (unsigned long)n;
    }
    return HTTP_OK;
}

static int read_body_close(struct hconn *c, wire_sink fn, void *u,
                           unsigned long deadline, struct http_client *cl)
{
    char tmp[HTTP_RXBUF];

    for (;;) {
        long n = hc_read(c, tmp, sizeof(tmp), deadline, cl);
        int rc;
        if (n < 0)
            return (int)n;
        if (n == 0)
            break;
        rc = fn(u, tmp, (unsigned long)n);
        if (rc != HTTP_OK)
            return rc;
    }
    return HTTP_OK;
}

/* Chunked, with chunk extensions and trailers. Trailer lines are appended
 * to `raw` so they join the response's headers. */
static int read_body_chunked(struct hconn *c, wire_sink fn, void *u,
                             unsigned long deadline, struct http_client *cl,
                             struct hbuf *raw, char *line, unsigned long linesz)
{
    char tmp[HTTP_RXBUF];
    unsigned long chunks = 0;

    for (;;) {
        unsigned long size;
        int ok;
        long ln = hc_line(c, line, linesz, deadline, cl);

        if (ln < 0)
            return (int)ln;
        size = parse_hex(line, &ok);
        if (!ok)
            return HTTP_E_PROTO;
        if (++chunks > 1000000UL)
            return HTTP_E_TOOBIG;
        if (size == 0)
            break;
        while (size > 0) {
            unsigned long need = size > sizeof(tmp) ? sizeof(tmp) : size;
            long n = hc_read(c, tmp, need, deadline, cl);
            int rc;
            if (n < 0)
                return (int)n;
            if (n == 0)
                return HTTP_E_RECV;
            rc = fn(u, tmp, (unsigned long)n);
            if (rc != HTTP_OK)
                return rc;
            size -= (unsigned long)n;
        }
        ln = hc_line(c, line, linesz, deadline, cl);   /* CRLF after data */
        if (ln < 0)
            return (int)ln;
        if (ln != 0)
            return HTTP_E_PROTO;
    }
    /* Trailer section: header lines up to a blank one. */
    for (;;) {
        long ln = hc_line(c, line, linesz, deadline, cl);
        if (ln < 0)
            return (int)ln;
        if (ln == 0)
            break;
        if (raw && raw->len + (unsigned long)ln + 1 < raw->max) {
            hb_add(raw, line, (unsigned long)ln);
            hb_add(raw, "\n", 1);
        }
    }
    return HTTP_OK;
}

/* ---- content decoding ------------------------------------------------- */

static int coding_id(const char *tok)
{
    if (ci_eq(tok, "gzip") || ci_eq(tok, "x-gzip"))
        return HTTP_INFLATE_GZIP;
    if (ci_eq(tok, "deflate"))
        return HTTP_INFLATE_ZLIB;
    if (ci_eq(tok, "identity") || *tok == '\0')
        return -1;                       /* nothing to do */
    return -2;                           /* unknown */
}

/* Split a Content-Encoding value into up to 3 codings, outermost first. */
static int coding_list(const char *v, int *out, int max)
{
    int n = 0;

    while (*v && n < max) {
        char tok[32];
        unsigned long k = 0;
        while (*v && (h_space((unsigned char)*v) || *v == ','))
            v++;
        while (*v && *v != ',' && !h_space((unsigned char)*v)) {
            if (k + 1 < sizeof(tok))
                tok[k++] = *v;
            v++;
        }
        tok[k] = '\0';
        if (k == 0)
            break;
        out[n] = coding_id(tok);
        if (out[n] == -2)
            return -1;
        if (out[n] >= 0)
            n++;
    }
    if (*v)
        return -1;                       /* more codings than we handle */
    return n;
}

/* Inflate `in` through the coding list (applied last-to-first). */
static int decode_body(struct hbuf *in, const int *codings, int ncod,
                       struct body_out *out)
{
    void *cur = in->p;
    unsigned long curlen = in->len;
    void *owned = 0;
    int i, rc = HTTP_OK;

    if (g_inflate == 0)
        return HTTP_E_DECODE;
    if (in->p == 0 || in->len == 0)
        return HTTP_OK;                  /* nothing was sent */
    for (i = ncod - 1; i >= 0; i--) {
        void *o = 0;
        unsigned long ol = 0;
        int wrapper = codings[i];
        int r = g_inflate(cur, curlen, &o, &ol, wrapper);

        if (r != 0 && wrapper == HTTP_INFLATE_ZLIB) {
            /* Servers labelled "deflate" often send a raw stream. */
            o = 0;
            ol = 0;
            r = g_inflate(cur, curlen, &o, &ol, HTTP_INFLATE_RAW);
        }
        if (r != 0 || o == 0) {
            free(owned);
            return HTTP_E_DECODE;
        }
        free(owned);
        owned = o;
        cur = o;
        curlen = ol;
        if (curlen > out->max) {
            free(owned);
            return HTTP_E_TOOBIG;
        }
    }
    {
        unsigned long off = 0;
        while (off < curlen) {
            unsigned long n = curlen - off;
            if (n > EMIT_CHUNK)
                n = EMIT_CHUNK;
            rc = out_write(out, (const char *)cur + off, n);
            if (rc != HTTP_OK)
                break;
            off += n;
        }
        if (curlen == 0)
            rc = HTTP_OK;
    }
    free(owned);
    return rc;
}

/* ---- HTTP dates (RFC 7231 IMF-fixdate and friends) -------------------- */

static long days_from_civil(long y, int m, int d)
{
    long era, doe, yoe, doy;

    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int month_id(const char *s)
{
    static const char n[12][4] = { "jan", "feb", "mar", "apr", "may", "jun",
                                   "jul", "aug", "sep", "oct", "nov", "dec" };
    int i, k;

    for (i = 0; i < 12; i++) {
        for (k = 0; k < 3; k++)
            if (h_lower((unsigned char)s[k]) != n[i][k])
                break;
        if (k == 3)
            return i + 1;
    }
    return 0;
}

/* Accepts IMF-fixdate, RFC 850 and asctime. -1 if unparsable. */
long http_parse_date(const char *s)
{
    const char *p;
    int day = 0, mon = 0, hh = 0, mm = 0, ss = 0;
    long year = 0;
    int got_time = 0, got_day = 0, got_mon = 0, got_year = 0;

    if (s == 0)
        return -1;
    if (strlen(s) > 64)
        return -1;
    p = s;
    while (*p) {
        while (*p && !((*p >= '0' && *p <= '9') ||
                       (h_lower((unsigned char)*p) >= 'a' &&
                        h_lower((unsigned char)*p) <= 'z')))
            p++;
        if (*p == '\0')
            break;
        if (h_digit((unsigned char)*p)) {
            const char *q = p;
            long v = 0;
            int nd = 0;
            while (h_digit((unsigned char)*q)) {
                v = v * 10 + (*q - '0');
                q++;
                nd++;
            }
            if (*q == ':' && !got_time) {
                int a = (int)v, b = 0, cc = 0;
                q++;
                while (h_digit((unsigned char)*q))
                    b = b * 10 + (*q++ - '0');
                if (*q == ':') {
                    q++;
                    while (h_digit((unsigned char)*q))
                        cc = cc * 10 + (*q++ - '0');
                }
                hh = a;
                mm = b;
                ss = cc;
                got_time = 1;
            } else if (nd <= 2 && !got_day) {
                day = (int)v;
                got_day = 1;
            } else if (!got_year) {
                year = v;
                got_year = 1;
            }
            p = q;
        } else {
            const char *q = p;
            while (h_lower((unsigned char)*q) >= 'a' &&
                   h_lower((unsigned char)*q) <= 'z')
                q++;
            if (!got_mon && q - p >= 3) {
                int m = month_id(p);
                if (m) {
                    mon = m;
                    got_mon = 1;
                }
            }
            p = q;
        }
    }
    if (!got_day || !got_mon || !got_year || !got_time)
        return -1;
    if (year >= 70 && year <= 99)
        year += 1900;
    else if (year >= 0 && year <= 69)
        year += 2000;
    if (day < 1 || day > 31 || year < 1601)
        return -1;
    return days_from_civil(year, mon, day) * 86400L + hh * 3600L +
           mm * 60L + ss;
}

/* ---- one request/response exchange ------------------------------------ */

static int is_redirect(int s);

struct exch {
    const char *method;
    const struct url *u;
    const struct http_request *req;
    const char *cond_etag;
    const char *cond_lm;
    int want_body;                 /* 0 for HEAD */
    struct hbuf raw;               /* unfolded header block */
    struct body_out *out;
};

static int build_request(struct http_client *c, struct exch *e, struct hbuf *hb)
{
    char target[URL_MAX];
    char hostbuf[URL_HOST_MAX + 16];
    char num[32];
    int rc;

    rc = url_request_target(e->u, target, sizeof(target));
    if (rc != URL_OK)
        return HTTP_E_URL;
    rc = url_host_header(e->u, hostbuf, sizeof(hostbuf));
    if (rc != URL_OK)
        return HTTP_E_URL;

    if (hb_str(hb, e->method) || hb_str(hb, " ") || hb_str(hb, target) ||
        hb_str(hb, " HTTP/1.1\r\nHost: ") || hb_str(hb, hostbuf) ||
        hb_str(hb, "\r\nUser-Agent: ") || hb_str(hb, c->agent) ||
        hb_str(hb, "\r\nAccept: "))
        return HTTP_E_TOOBIG;
    if (hb_str(hb, e->req->accept ? e->req->accept
                                  : "text/html,application/xhtml+xml,"
                                    "text/plain;q=0.9,image/png,image/jpeg,"
                                    "image/gif,*/*;q=0.5"))
        return HTTP_E_TOOBIG;
    if (g_inflate && !(e->req->flags & HTTP_F_NO_DECODE)) {
        if (hb_str(hb, "\r\nAccept-Encoding: gzip, deflate"))
            return HTTP_E_TOOBIG;
    } else if (hb_str(hb, "\r\nAccept-Encoding: identity")) {
        return HTTP_E_TOOBIG;
    }
    if (hb_str(hb, (e->req->flags & HTTP_F_NO_KEEPALIVE)
                       ? "\r\nConnection: close"
                       : "\r\nConnection: keep-alive"))
        return HTTP_E_TOOBIG;

    if (c->jar && !(e->req->flags & HTTP_F_NO_COOKIES)) {
        /* On the heap: COOKIE_HEADER_MAX is 8 KiB and the user stack is
         * only 64 KiB. */
        char *cookies = malloc(COOKIE_HEADER_MAX);
        long n;
        if (cookies == 0)
            return HTTP_E_NOMEM;
        n = cookie_header(c->jar, e->u->host, e->u->path,
                          ci_eq(e->u->scheme, "https"), 1,
                          COOKIE_CTX_SAME_SITE, now_sec(),
                          cookies, COOKIE_HEADER_MAX);
        if (n > 0) {
            if (hb_str(hb, "\r\nCookie: ") || hb_str(hb, cookies)) {
                free(cookies);
                return HTTP_E_TOOBIG;
            }
        }
        free(cookies);
    }
    if (e->cond_etag && *e->cond_etag) {
        if (hb_str(hb, "\r\nIf-None-Match: ") || hb_str(hb, e->cond_etag))
            return HTTP_E_TOOBIG;
    }
    if (e->cond_lm && *e->cond_lm) {
        if (hb_str(hb, "\r\nIf-Modified-Since: ") || hb_str(hb, e->cond_lm))
            return HTTP_E_TOOBIG;
    }
    if (e->req->body && e->req->body_len > 0) {
        if (e->req->content_type) {
            if (hb_str(hb, "\r\nContent-Type: ") ||
                hb_str(hb, e->req->content_type))
                return HTTP_E_TOOBIG;
        }
        snprintf(num, sizeof(num), "%lu", e->req->body_len);
        if (hb_str(hb, "\r\nContent-Length: ") || hb_str(hb, num))
            return HTTP_E_TOOBIG;
    } else if (ci_eq(e->method, "POST")) {
        if (hb_str(hb, "\r\nContent-Length: 0"))
            return HTTP_E_TOOBIG;
    }
    if (e->req->extra_headers && *e->req->extra_headers) {
        if (hb_str(hb, "\r\n") || hb_str(hb, e->req->extra_headers))
            return HTTP_E_TOOBIG;
        /* Tolerate a caller that already ended with CRLF. */
        while (hb->len >= 2 && hb->p[hb->len - 1] == '\n' &&
               hb->p[hb->len - 2] == '\r')
            hb->len -= 2;
        hb->p[hb->len] = '\0';
    }
    if (hb_str(hb, "\r\n\r\n"))
        return HTTP_E_TOOBIG;
    return HTTP_OK;
}

/* Read the status line and header block. */
static int read_head(struct hconn *conn, struct exch *e,
                     struct http_response *res, char *line,
                     unsigned long linesz, unsigned long deadline,
                     struct http_client *cl)
{
    long ln;
    int rc, rounds = 0;

    for (;;) {
        const char *sp;
        int ok, code;

        if (++rounds > 16)
            return HTTP_E_PROTO;         /* endless blank or 1xx responses */
        ln = hc_line(conn, line, linesz, deadline, cl);
        if (ln < 0)
            return (int)ln;
        if (ln == 0)
            continue;                    /* tolerate a stray leading CRLF */
        if (!ci_prefix(line, "HTTP/1."))
            return HTTP_E_PROTO;
        res->http_minor = line[7] == '0' ? 0 : 1;
        sp = strchr(line, ' ');
        if (sp == 0)
            return HTTP_E_PROTO;
        while (*sp == ' ')
            sp++;
        code = (int)parse_ul(sp, &ok);
        if (!ok || code < 100 || code > 599)
            return HTTP_E_PROTO;
        res->status = code;
        {
            const char *r = sp;
            while (*r && *r != ' ')
                r++;
            while (*r == ' ')
                r++;
            free(res->reason);
            res->reason = malloc(strlen(r) + 1);
            if (res->reason)
                strcpy(res->reason, r);
        }

        hb_free(&e->raw);
        hb_init(&e->raw, HTTP_HEADERS_MAX);
        for (;;) {
            ln = hc_line(conn, line, linesz, deadline, cl);
            if (ln < 0)
                return (int)ln;
            if (ln == 0)
                break;
            if (h_space((unsigned char)line[0])) {
                /* obs-fold: continuation of the previous header */
                char *v = line;
                while (*v && h_space((unsigned char)*v))
                    v++;
                if (e->raw.len == 0)
                    return HTTP_E_PROTO;
                e->raw.len--;            /* drop the '\n' */
                e->raw.p[e->raw.len] = '\0';
                if (hb_str(&e->raw, " ") || hb_str(&e->raw, v) ||
                    hb_str(&e->raw, "\n"))
                    return HTTP_E_TOOBIG;
                continue;
            }
            if (strchr(line, ':') == 0)
                return HTTP_E_PROTO;
            if (hb_add(&e->raw, line, (unsigned long)ln) ||
                hb_str(&e->raw, "\n"))
                return HTTP_E_TOOBIG;
        }

        /* 1xx other than 101 is informational: read the real one. */
        if (code >= 100 && code < 200 && code != 101)
            continue;
        break;
    }
    rc = headers_build(res, &e->raw);
    return rc;
}

/* Work out how the body is framed. */
struct framing {
    int chunked;
    int have_len;
    unsigned long len;
    int to_close;
    int determinate;
};

static int decide_framing(struct http_response *res, struct exch *e,
                          struct framing *f)
{
    const char *te;
    int i;

    memset(f, 0, sizeof(*f));
    f->determinate = 1;

    te = http_header_get(res, "Transfer-Encoding");
    if (te) {
        const char *last = te;
        const char *comma = te;
        while ((comma = strchr(comma, ',')) != 0) {
            last = comma + 1;
            comma++;
        }
        while (*last && h_space((unsigned char)*last))
            last++;
        if (ci_prefix(last, "chunked"))
            f->chunked = 1;
        else
            f->determinate = 0;          /* unknown coding: read to close */
    }

    if (!f->chunked) {
        int ok = 0;
        unsigned long v = 0, first = 0;
        int seen = 0;
        for (i = 0;; i++) {
            const char *s = http_header_nth(res, "Content-Length", i);
            if (s == 0)
                break;
            v = parse_ul(s, &ok);
            if (!ok)
                return HTTP_E_PROTO;
            if (seen && v != first)
                return HTTP_E_PROTO;     /* request smuggling attempt */
            first = v;
            seen = 1;
        }
        if (seen) {
            f->have_len = 1;
            f->len = first;
        }
    }

    if (!e->want_body || res->status == 204 || res->status == 304 ||
        (res->status >= 100 && res->status < 200)) {
        f->chunked = 0;
        f->have_len = 1;
        f->len = 0;
        return HTTP_OK;
    }
    if (!f->chunked && !f->have_len) {
        f->to_close = 1;
        f->determinate = 0;
    }
    return HTTP_OK;
}

static int should_keep_alive(struct http_response *res, struct framing *f,
                             const struct http_request *req)
{
    const char *conn = http_header_get(res, "Connection");

    if (req->flags & HTTP_F_NO_KEEPALIVE)
        return 0;
    if (!f->determinate)
        return 0;
    if (conn) {
        if (ci_str(conn, "close"))
            return 0;
        if (res->http_minor == 0 && ci_str(conn, "keep-alive"))
            return 1;
    }
    return res->http_minor >= 1;
}

/* ---- the exchange ----------------------------------------------------- */

static int exchange(struct http_client *c, struct exch *e,
                    struct http_response *res)
{
    char origin[URL_HOST_MAX + 32];
    struct hconn *conn = 0;
    struct hbuf head;
    struct hbuf coded;
    struct framing f;
    char *line = 0;
    int rc = HTTP_OK, attempt, ncod = 0;
    int codings[3] = { 0, 0, 0 };
    unsigned long deadline;
    int connect_ms = e->req->connect_timeout_ms > 0
                         ? e->req->connect_timeout_ms : HTTP_T_CONNECT;
    int head_ms = e->req->header_timeout_ms > 0
                      ? e->req->header_timeout_ms : HTTP_T_HEADERS;
    int body_ms = e->req->body_timeout_ms > 0
                      ? e->req->body_timeout_ms : HTTP_T_BODY;

    if (url_origin(e->u, origin, sizeof(origin)) != URL_OK)
        return HTTP_E_URL;

    hb_init(&head, REQ_HEAD_MAX);
    rc = build_request(c, e, &head);
    if (rc != HTTP_OK) {
        hb_free(&head);
        return rc;
    }
    line = malloc(HTTP_LINE_MAX);
    if (line == 0) {
        hb_free(&head);
        return HTTP_E_NOMEM;
    }

    /* Two attempts: a pooled connection the server closed while idle is a
     * normal race, so a request that fails on a reused socket is retried
     * once on a fresh one. A fresh connection is never retried. */
    for (attempt = 0; attempt < 2; attempt++) {
        int reused = 0;

        conn = pool_take(c, origin);
        if (conn) {
            reused = 1;
        } else {
            struct scheme_ent *se = scheme_find(e->u->scheme);
            if (se == 0) {
                rc = HTTP_E_SCHEME;
                goto done;
            }
            conn = pool_slot(c);
            if (conn == 0) {
                rc = HTTP_E_CONNECT;
                goto done;
            }
            memset(conn, 0, sizeof(*conn));
            rc = se->fn(e->u->host, url_effective_port(e->u), connect_ms,
                        se->user, &conn->t);
            if (rc != HTTP_OK)
                goto done;
            conn->open = 1;
            snprintf(conn->origin, sizeof(conn->origin), "%s", origin);
            c->st.connections++;
        }
        if (reused)
            c->st.reused++;

        deadline = now_ms() + (unsigned long)head_ms;
        rc = hc_send(conn, head.p, head.len, c);
        if (rc == HTTP_OK && e->req->body && e->req->body_len > 0)
            rc = hc_send(conn, e->req->body, e->req->body_len, c);
        if (rc == HTTP_OK)
            rc = read_head(conn, e, res, line, HTTP_LINE_MAX, deadline, c);

        if (rc == HTTP_OK)
            break;
        hc_shutdown(conn);
        if (!reused)
            goto done;
        if (reused)
            c->st.reused--;
        /* retry once on a fresh connection */
    }
    if (rc != HTTP_OK)
        goto done;

    rc = decide_framing(res, e, &f);
    if (rc != HTTP_OK)
        goto done;

    /* Tell the caller what is coming before any of it arrives, so a page
     * can start laying out. Skipped on a hop we are about to follow. */
    if (e->req->on_headers &&
        !(is_redirect(res->status) && !(e->req->flags & HTTP_F_NO_REDIRECT) &&
          http_header_get(res, "Location"))) {
        if (e->req->on_headers(e->req->headers_user, res) != 0) {
            rc = HTTP_E_ABORT;
            goto done;
        }
    }

    /* Content-Encoding? */
    hb_init(&coded, CODED_MAX);
    if (!(e->req->flags & HTTP_F_NO_DECODE)) {
        const char *ce = http_header_get(res, "Content-Encoding");
        if (ce && *ce) {
            ncod = coding_list(ce, codings, 3);
            if (ncod < 0) {
                rc = HTTP_E_DECODE;
                goto done;
            }
            if (ncod > 0 && g_inflate == 0) {
                rc = HTTP_E_DECODE;
                goto done;
            }
        }
    }

    deadline = now_ms() + (unsigned long)body_ms;
    {
        wire_sink fn = ncod > 0 ? sink_to_buf : sink_to_out;
        void *u = ncod > 0 ? (void *)&coded : (void *)e->out;

        if (f.chunked)
            rc = read_body_chunked(conn, fn, u, deadline, c, &e->raw, line,
                                   HTTP_LINE_MAX);
        else if (f.have_len)
            rc = read_body_length(conn, f.len, fn, u, deadline, c);
        else
            rc = read_body_close(conn, fn, u, deadline, c);
    }

    if (rc == HTTP_OK && ncod > 0)
        rc = decode_body(&coded, codings, ncod, e->out);
    hb_free(&coded);

    /* Trailers may have been appended to the header block. */
    if (rc == HTTP_OK && f.chunked)
        headers_build(res, &e->raw);

    if (rc == HTTP_OK && should_keep_alive(res, &f, e->req)) {
        conn->pooled = 1;
        conn->idle_at = now_ms();
    } else {
        hc_shutdown(conn);
    }

done:
    free(line);
    hb_free(&head);
    return rc;
}

/* ---- cookies from a response ------------------------------------------ */

static void absorb_cookies(struct http_client *c, const struct url *u,
                           struct http_response *res, int flags)
{
    int i;
    long now;

    if (c->jar == 0 || (flags & HTTP_F_NO_COOKIES))
        return;
    now = now_sec();
    for (i = 0; i < res->nheaders; i++)
        if (ci_eq(res->headers[i].name, "Set-Cookie"))
            cookie_set(c->jar, res->headers[i].value, u->host, u->path,
                       ci_eq(u->scheme, "https"), now);
}

/* ---- redirects --------------------------------------------------------- */

static int is_redirect(int s)
{
    return s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
}

/* RFC 7231: 301/302 historically rewrite POST to GET, 303 always does,
 * 307/308 never do. HEAD is never rewritten. */
static const char *rewrite_method(int status, const char *method,
                                  int *drop_body)
{
    *drop_body = 0;
    if (status == 307 || status == 308)
        return method;
    if (ci_eq(method, "HEAD"))
        return "HEAD";
    if (status == 303) {
        *drop_body = 1;
        return "GET";
    }
    if (ci_eq(method, "GET"))
        return "GET";
    *drop_body = 1;
    return "GET";
}

/* ---- cacheability ------------------------------------------------------ */

static int method_cacheable(const char *m)
{
    return ci_eq(m, "GET") || ci_eq(m, "HEAD");
}

/* ---- public API -------------------------------------------------------- */

struct http_client *http_client_new(void)
{
    struct http_client *c = malloc(sizeof(*c));

    if (c == 0)
        return 0;
    memset(c, 0, sizeof(*c));
    snprintf(c->agent, sizeof(c->agent), "KestrelOS/1.0 (libweb)");
    c->jar = cookie_jar_new();
    c->cache = http_cache_new(HTTP_CACHE_BYTES);
    if (scheme_find("http") == 0)
        http_register_scheme("http", http_tcp_transport, 0);
    return c;
}

void http_client_free(struct http_client *c)
{
    if (c == 0)
        return;
    http_client_drop_connections(c);
    cookie_jar_free(c->jar);
    http_cache_free(c->cache);
    free(c);
}

struct cookie_jar *http_client_jar(struct http_client *c)
{
    return c ? c->jar : 0;
}

struct http_cache *http_client_cache(struct http_client *c)
{
    return c ? c->cache : 0;
}

void http_client_set_cache(struct http_client *c, struct http_cache *cache)
{
    if (c == 0)
        return;
    if (c->cache != cache)
        http_cache_free(c->cache);
    c->cache = cache;
}

void http_client_set_agent(struct http_client *c, const char *ua)
{
    if (c && ua)
        snprintf(c->agent, sizeof(c->agent), "%s", ua);
}

void http_client_stats(const struct http_client *c, struct http_stats *out)
{
    if (c && out)
        *out = c->st;
}

int http_fetch(struct http_client *c, const struct http_request *req,
               struct http_response *res)
{
    struct url cur, next;
    struct http_request r;
    struct body_out out;
    struct exch e;
    char urlbuf[URL_MAX];
    char keybuf[URL_MAX + 16];
    char etagbuf[256], lmbuf[128];
    unsigned long seen[HTTP_REDIRECT_MAX + 2];
    const char *method;
    const char *cond_etag = 0, *cond_lm = 0;
    char *final;
    int hops = 0, nseen = 0, rc, i;
    int maxhops;
    int cacheable_req, had_cache_entry = 0;
    long now;

    if (c == 0 || req == 0 || res == 0 || req->url == 0)
        return HTTP_E_INVAL;
    memset(res, 0, sizeof(*res));
    r = *req;
    method = r.method ? r.method : "GET";
    maxhops = r.max_redirects > 0 ? r.max_redirects : HTTP_REDIRECT_MAX;
    if (maxhops > HTTP_REDIRECT_MAX)
        maxhops = HTTP_REDIRECT_MAX;

    if (url_parse(r.url, &cur) != URL_OK)
        return HTTP_E_URL;
    if (!cur.has_scheme || !cur.has_authority || cur.host[0] == '\0')
        return HTTP_E_URL;
    url_normalize(&cur, URL_N_PORT | URL_N_DOTS | URL_N_EMPTY_PATH);

    memset(&out, 0, sizeof(out));
    out.sink = r.sink;
    out.sink_user = r.sink_user;
    out.max = r.max_body ? r.max_body : HTTP_BODY_MAX;
    hb_init(&out.mem, out.max);
    hb_init(&out.tee, HTTP_CACHE_ENTRY_MAX);

    now = now_sec();
    cacheable_req = c->cache && method_cacheable(method) &&
                    !(r.flags & HTTP_F_NO_CACHE);

    etagbuf[0] = lmbuf[0] = '\0';
    if (url_serialize(&cur, urlbuf, sizeof(urlbuf)) != URL_OK) {
        hb_free(&out.mem);
        hb_free(&out.tee);
        return HTTP_E_URL;
    }
    snprintf(keybuf, sizeof(keybuf), "%s %s", method, urlbuf);
    if (cacheable_req) {
        const char *et = 0, *lm = 0;
        int st;
        st = http_cache_lookup(c->cache, keybuf, now, &et, &lm);
        if (st == HTTP_CACHE_FRESH && !(r.flags & HTTP_F_REFRESH)) {
            rc = http_cache_fill(c->cache, keybuf, res);
            if (rc == HTTP_OK) {
                res->from_cache = 1;
                c->st.requests++;
                c->st.cache_hits++;
                final = malloc(strlen(urlbuf) + 1);
                if (final == 0) {
                    rc = HTTP_E_NOMEM;
                    goto fail;
                }
                strcpy(final, urlbuf);
                free(res->final_url);
                res->final_url = final;
                if (out.sink && res->body && res->body_len)
                    out.sink(out.sink_user, res->body, res->body_len);
                hb_free(&out.mem);
                hb_free(&out.tee);
                return HTTP_OK;
            }
        }
        if (st == HTTP_CACHE_STALE || (st == HTTP_CACHE_FRESH &&
                                       (r.flags & HTTP_F_REFRESH))) {
            had_cache_entry = 1;
            if (et) {
                snprintf(etagbuf, sizeof(etagbuf), "%s", et);
                cond_etag = etagbuf;
            }
            if (lm) {
                snprintf(lmbuf, sizeof(lmbuf), "%s", lm);
                cond_lm = lmbuf;
            }
            c->st.revalidations++;
        }
    }

    c->st.requests++;
    out.tee_on = cacheable_req;

    for (;;) {
        unsigned long h;

        if (url_serialize(&cur, urlbuf, sizeof(urlbuf)) != URL_OK) {
            rc = HTTP_E_URL;
            goto fail;
        }
        h = fnv1a(urlbuf);
        for (i = 0; i < nseen; i++) {
            if (seen[i] == h) {
                rc = HTTP_E_LOOP;
                goto fail;
            }
        }
        if (nseen < (int)(sizeof(seen) / sizeof(seen[0])))
            seen[nseen++] = h;

        memset(&e, 0, sizeof(e));
        e.method = method;
        e.u = &cur;
        e.req = &r;
        e.cond_etag = cond_etag;
        e.cond_lm = cond_lm;
        e.want_body = !ci_eq(method, "HEAD");
        e.out = &out;
        hb_init(&e.raw, HTTP_HEADERS_MAX);

        rc = exchange(c, &e, res);
        if (rc != HTTP_OK) {
            hb_free(&e.raw);
            goto fail;
        }
        absorb_cookies(c, &cur, res, r.flags);

        if (is_redirect(res->status) && !(r.flags & HTTP_F_NO_REDIRECT)) {
            const char *loc = http_header_get(res, "Location");
            struct url ref;
            int drop;

            if (loc == 0 || *loc == '\0') {
                hb_free(&e.raw);
                break;                   /* a 3xx with no Location is final */
            }
            if (++hops > maxhops) {
                hb_free(&e.raw);
                rc = HTTP_E_REDIR;
                goto fail;
            }
            if (url_parse(loc, &ref) != URL_OK ||
                url_resolve(&cur, &ref, &next, 1) != URL_OK) {
                hb_free(&e.raw);
                rc = HTTP_E_REDIR;
                goto fail;
            }
            url_normalize(&next, URL_N_PORT | URL_N_DOTS | URL_N_EMPTY_PATH);
            if (!next.has_scheme || !next.has_authority ||
                next.host[0] == '\0') {
                hb_free(&e.raw);
                rc = HTTP_E_REDIR;
                goto fail;
            }
            method = rewrite_method(res->status, method, &drop);
            if (drop) {
                r.body = 0;
                r.body_len = 0;
                r.content_type = 0;
            }
            /* Only the final response is cached or revalidated. */
            cond_etag = cond_lm = 0;
            had_cache_entry = 0;
            out.tee_on = 0;
            out.total = 0;
            out.mem.len = 0;
            if (out.mem.p)
                out.mem.p[0] = '\0';
            hb_free(&out.tee);
            hb_init(&out.tee, HTTP_CACHE_ENTRY_MAX);
            cur = next;
            hb_free(&e.raw);
            http_response_free(res);
            res->redirects = hops;
            continue;
        }

        /* 304 to our conditional request: reuse the stored entry. */
        if (res->status == 304 && had_cache_entry) {
            c->st.not_modified++;
            http_cache_refresh(c->cache, keybuf, e.raw.p ? e.raw.p : "", now);
            {
                int saved = res->redirects;
                http_response_free(res);
                rc = http_cache_fill(c->cache, keybuf, res);
                res->redirects = saved;
            }
            if (rc != HTTP_OK) {
                hb_free(&e.raw);
                goto fail;
            }
            res->from_cache = 1;
            if (out.sink && res->body && res->body_len)
                out.sink(out.sink_user, res->body, res->body_len);
            hb_free(&e.raw);
            goto finish;
        }

        /* Store, if the response says we may. */
        if (cacheable_req && out.tee_on && res->status != 304) {
            http_cache_store(c->cache, keybuf, res->status,
                             e.raw.p ? e.raw.p : "", out.tee.p, out.tee.len,
                             now, method);
        } else if (c->cache && !method_cacheable(method)) {
            /* An unsafe method invalidates the cached GET for that URL. */
            snprintf(keybuf, sizeof(keybuf), "GET %s", urlbuf);
            http_cache_invalidate(c->cache, keybuf);
        }
        hb_free(&e.raw);
        break;
    }

    if (out.sink == 0) {
        res->body = out.mem.p;
        res->body_len = out.mem.len;
        out.mem.p = 0;
        if (res->body == 0) {
            res->body = malloc(1);
            if (res->body)
                res->body[0] = '\0';
            res->body_len = 0;
        }
    }

finish:
    res->redirects = hops;
    if (url_serialize(&cur, urlbuf, sizeof(urlbuf)) != URL_OK) {
        rc = HTTP_E_URL;
        goto fail;
    }
    final = malloc(strlen(urlbuf) + 1);
    if (final == 0) {
        rc = HTTP_E_NOMEM;
        goto fail;
    }
    strcpy(final, urlbuf);
    free(res->final_url);
    res->final_url = final;
    hb_free(&out.mem);
    hb_free(&out.tee);
    return HTTP_OK;

fail:
    hb_free(&out.mem);
    hb_free(&out.tee);
    http_response_free(res);
    return rc;
}

int http_get_url(struct http_client *c, const char *url,
                 struct http_response *res)
{
    struct http_request r;

    memset(&r, 0, sizeof(r));
    r.method = "GET";
    r.url = url;
    return http_fetch(c, &r, res);
}

int http_post_url(struct http_client *c, const char *url, const void *body,
                  unsigned long len, const char *content_type,
                  struct http_response *res)
{
    struct http_request r;

    memset(&r, 0, sizeof(r));
    r.method = "POST";
    r.url = url;
    r.body = body;
    r.body_len = len;
    r.content_type = content_type ? content_type
                                  : "application/x-www-form-urlencoded";
    return http_fetch(c, &r, res);
}

const char *http_error_text(int err)
{
    switch (err) {
    case HTTP_OK:        return "ok";
    case HTTP_E_INVAL:   return "invalid argument";
    case HTTP_E_URL:     return "malformed URL";
    case HTTP_E_SCHEME:  return "no transport for that URL scheme";
    case HTTP_E_DNS:     return "host name did not resolve";
    case HTTP_E_CONNECT: return "could not connect";
    case HTTP_E_SEND:    return "could not send the request";
    case HTTP_E_RECV:    return "connection closed before the response ended";
    case HTTP_E_PROTO:   return "malformed HTTP response";
    case HTTP_E_TOOBIG:  return "response larger than the configured limit";
    case HTTP_E_REDIR:   return "too many redirects, or a bad Location";
    case HTTP_E_LOOP:    return "redirect loop";
    case HTTP_E_NOMEM:   return "out of memory";
    case HTTP_E_TIMEOUT: return "the server stopped responding";
    case HTTP_E_ABORT:   return "transfer aborted by the caller";
    case HTTP_E_DECODE:  return "could not decode the content encoding";
    default:             return "unknown error";
    }
}

const char *http_reason_phrase(int status)
{
    switch (status) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
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
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 410: return "Gone";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default:  return "Unknown";
    }
}
