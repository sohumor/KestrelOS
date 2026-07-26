/* KestrelOS libweb: the HTTP response cache.
 *
 * In memory only, bounded by total bytes with LRU eviction. Honours
 * Cache-Control (no-store, no-cache, max-age, must-revalidate), Expires,
 * Age and Date for freshness, and keeps ETag / Last-Modified so the
 * client can revalidate with If-None-Match / If-Modified-Since and
 * handle a 304 without refetching the body.
 *
 * Bodies are stored *decoded*: the client undoes Content-Encoding before
 * handing them over, and the stored header block has Content-Encoding,
 * Content-Length and the hop-by-hop headers stripped and a correct
 * Content-Length appended. That keeps a replayed response usable without
 * a decompressor.
 *
 * Entries are a singly linked list. The cache holds a few hundred items
 * at most (4 MiB default, 1 MiB per entry), so linear scans are fine and
 * there is no hash table to get wrong.
 */

#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Heuristic freshness is capped at a day, as RFC 7234 suggests. */
#define HEURISTIC_CAP 86400L
/* A stored header block never exceeds this. */
#define CACHE_HDR_MAX 16384

struct cache_entry {
    char *key;
    char *headers;             /* "Name: value\n" lines, sanitised */
    char *etag;
    char *last_mod;
    unsigned char *body;
    unsigned long len;
    int status;
    long stored;               /* unix seconds when we received it */
    long expires;              /* absolute; only valid if have_expires */
    int have_expires;
    int always_revalidate;     /* no-cache / must-revalidate */
    unsigned long seq;         /* LRU: higher is more recent */
    struct cache_entry *next;
};

struct http_cache {
    struct cache_entry *head;
    unsigned long bytes;
    unsigned long max;
    unsigned long tick;
    int count;
};

/* ---- helpers --------------------------------------------------------- */

static int c_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int ci_eq_n(const char *a, const char *b, unsigned long n)
{
    unsigned long i;

    for (i = 0; i < n; i++)
        if (c_lower((unsigned char)a[i]) != c_lower((unsigned char)b[i]))
            return 0;
    return 1;
}

static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (c_lower((unsigned char)*a) != c_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int is_blank(int c)
{
    return c == ' ' || c == '\t';
}

/* KestrelOS libc has no memchr(); find ':' within the first n bytes. */
static const char *find_colon(const char *p, unsigned long n)
{
    unsigned long i;

    for (i = 0; i < n; i++)
        if (p[i] == ':')
            return p + i;
    return 0;
}

static char *dup_str(const char *s)
{
    unsigned long n;
    char *p;

    if (s == 0)
        return 0;
    n = strlen(s);
    p = malloc(n + 1);
    if (p)
        memcpy(p, s, n + 1);
    return p;
}

/* Copy the value of `name` out of a raw "Name: value\n" block. Returns
 * `buf` or NULL. */
static char *raw_get(const char *raw, const char *name, char *buf,
                     unsigned long bufsz)
{
    unsigned long nl = strlen(name);
    const char *p = raw;

    if (raw == 0 || bufsz == 0)
        return 0;
    while (*p) {
        const char *eol = strchr(p, '\n');
        unsigned long linelen = eol ? (unsigned long)(eol - p) : strlen(p);

        if (linelen > nl && p[nl] == ':' && ci_eq_n(p, name, nl)) {
            const char *v = p + nl + 1;
            unsigned long vl;
            while (v < p + linelen && is_blank((unsigned char)*v))
                v++;
            vl = (unsigned long)(p + linelen - v);
            while (vl > 0 && is_blank((unsigned char)v[vl - 1]))
                vl--;
            if (vl + 1 > bufsz)
                vl = bufsz - 1;
            memcpy(buf, v, vl);
            buf[vl] = '\0';
            return buf;
        }
        if (eol == 0)
            break;
        p = eol + 1;
    }
    return 0;
}

/* Case-insensitive token search inside a comma list. */
static const char *cc_find(const char *list, const char *tok)
{
    unsigned long tl = strlen(tok);
    const char *p = list;

    if (list == 0)
        return 0;
    while (*p) {
        while (*p && (is_blank((unsigned char)*p) || *p == ','))
            p++;
        if (*p == '\0')
            break;
        if (ci_eq_n(p, tok, tl)) {
            char after = p[tl];
            if (after == '\0' || after == ',' || after == '=' ||
                is_blank((unsigned char)after))
                return p;
        }
        while (*p && *p != ',')
            p++;
    }
    return 0;
}

static int cc_delta(const char *list, const char *tok, long *out)
{
    const char *p = cc_find(list, tok);

    if (p == 0)
        return 0;
    p += strlen(tok);
    while (is_blank((unsigned char)*p))
        p++;
    if (*p != '=')
        return 0;
    p++;
    while (is_blank((unsigned char)*p) || *p == '"')
        p++;
    if (*p < '0' || *p > '9')
        return 0;
    {
        long v = 0;
        int n = 0;
        while (*p >= '0' && *p <= '9' && n < 12) {
            v = v * 10 + (*p - '0');
            p++;
            n++;
        }
        *out = v;
    }
    return 1;
}

/* ---- lifecycle -------------------------------------------------------- */

struct http_cache *http_cache_new(unsigned long max_bytes)
{
    struct http_cache *c = malloc(sizeof(*c));

    if (c == 0)
        return 0;
    c->head = 0;
    c->bytes = 0;
    c->max = max_bytes ? max_bytes : HTTP_CACHE_BYTES;
    c->tick = 0;
    c->count = 0;
    return c;
}

static unsigned long entry_bytes(const struct cache_entry *e)
{
    return e->len + strlen(e->key) + (e->headers ? strlen(e->headers) : 0) +
           sizeof(*e);
}

static void entry_destroy(struct cache_entry *e)
{
    free(e->key);
    free(e->headers);
    free(e->etag);
    free(e->last_mod);
    free(e->body);
    free(e);
}

void http_cache_clear(struct http_cache *c)
{
    struct cache_entry *e;

    if (c == 0)
        return;
    e = c->head;
    while (e) {
        struct cache_entry *n = e->next;
        entry_destroy(e);
        e = n;
    }
    c->head = 0;
    c->bytes = 0;
    c->count = 0;
}

void http_cache_free(struct http_cache *c)
{
    if (c == 0)
        return;
    http_cache_clear(c);
    free(c);
}

void http_cache_stat(const struct http_cache *c, unsigned long *bytes,
                     int *entries)
{
    if (bytes)
        *bytes = c ? c->bytes : 0;
    if (entries)
        *entries = c ? c->count : 0;
}

static void cache_unlink(struct http_cache *c, struct cache_entry *victim)
{
    struct cache_entry **pp = &c->head;

    while (*pp) {
        if (*pp == victim) {
            *pp = victim->next;
            c->bytes -= entry_bytes(victim);
            c->count--;
            entry_destroy(victim);
            return;
        }
        pp = &(*pp)->next;
    }
}

static struct cache_entry *cache_find(struct http_cache *c, const char *key)
{
    struct cache_entry *e;

    for (e = c->head; e; e = e->next)
        if (strcmp(e->key, key) == 0)
            return e;
    return 0;
}

void http_cache_invalidate(struct http_cache *c, const char *key)
{
    struct cache_entry *e;

    if (c == 0 || key == 0)
        return;
    e = cache_find(c, key);
    if (e)
        cache_unlink(c, e);
}

static void evict_lru(struct http_cache *c)
{
    struct cache_entry *e, *victim = 0;

    for (e = c->head; e; e = e->next)
        if (victim == 0 || e->seq < victim->seq)
            victim = e;
    if (victim)
        cache_unlink(c, victim);
}

/* ---- freshness -------------------------------------------------------- */

/* Fill in expires/have_expires/always_revalidate from a header block.
 * Returns 0 if the response must not be stored at all. */
static int compute_freshness(struct cache_entry *e, const char *raw, long now,
                             int status)
{
    char buf[512];
    long lifetime = -1, age = 0, date = -1, initial_age = 0;
    const char *cc = 0;
    char ccbuf[512];

    e->have_expires = 0;
    e->always_revalidate = 0;
    e->expires = 0;

    if (raw_get(raw, "Cache-Control", ccbuf, sizeof(ccbuf)))
        cc = ccbuf;
    if (cc) {
        if (cc_find(cc, "no-store"))
            return 0;
        if (cc_find(cc, "no-cache") || cc_find(cc, "must-revalidate"))
            e->always_revalidate = 1;
        {
            long v;
            if (cc_delta(cc, "max-age", &v))
                lifetime = v;
        }
    }
    if (raw_get(raw, "Pragma", buf, sizeof(buf)) && cc_find(buf, "no-cache"))
        e->always_revalidate = 1;

    if (raw_get(raw, "Age", buf, sizeof(buf))) {
        long v = 0;
        const char *p = buf;
        int n = 0;
        while (*p >= '0' && *p <= '9' && n < 12) {
            v = v * 10 + (*p - '0');
            p++;
            n++;
        }
        age = v;
    }
    if (raw_get(raw, "Date", buf, sizeof(buf)))
        date = http_parse_date(buf);

    if (lifetime < 0 && raw_get(raw, "Expires", buf, sizeof(buf))) {
        long exp = http_parse_date(buf);
        long base = date >= 0 ? date : now;
        if (exp >= 0)
            lifetime = exp - base;
        else
            lifetime = 0;               /* unparsable Expires = stale */
    }

    if (lifetime < 0) {
        /* Heuristic: a tenth of the time since Last-Modified. */
        if (raw_get(raw, "Last-Modified", buf, sizeof(buf))) {
            long lm = http_parse_date(buf);
            long base = date >= 0 ? date : now;
            if (lm >= 0 && base > lm) {
                lifetime = (base - lm) / 10;
                if (lifetime > HEURISTIC_CAP)
                    lifetime = HEURISTIC_CAP;
            }
        }
    }
    if (lifetime < 0) {
        if (status == 301 || status == 308 || status == 410)
            lifetime = HEURISTIC_CAP;   /* permanent by definition */
        else
            lifetime = 0;
    }

    initial_age = age;
    if (date >= 0 && now > date)
        initial_age += now - date;
    if (initial_age < 0)
        initial_age = 0;

    e->have_expires = 1;
    e->expires = now + lifetime - initial_age;
    return 1;
}

static int status_cacheable(int status)
{
    switch (status) {
    case 200: case 203: case 204: case 300: case 301: case 308:
    case 404: case 405: case 410: case 414: case 501:
        return 1;
    default:
        return 0;
    }
}

/* ---- storing ---------------------------------------------------------- */

/* Headers we never replay. */
static int drop_header(const char *name)
{
    static const char *const drop[] = {
        "connection", "keep-alive", "proxy-authenticate",
        "proxy-authorization", "te", "trailer", "transfer-encoding",
        "upgrade", "content-length", "content-encoding", 0
    };
    int i;

    for (i = 0; drop[i]; i++)
        if (ci_eq(name, drop[i]))
            return 1;
    return 0;
}

/* Rewrite a raw header block: drop hop-by-hop and coding headers, then
 * append an accurate Content-Length. Returns a malloc'd string. */
static char *sanitise_headers(const char *raw, unsigned long body_len)
{
    unsigned long cap = (raw ? strlen(raw) : 0) + 64;
    char *out;
    unsigned long o = 0;
    const char *p = raw;

    if (cap > CACHE_HDR_MAX)
        return 0;
    out = malloc(cap + 1);
    if (out == 0)
        return 0;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        unsigned long linelen = eol ? (unsigned long)(eol - p) : strlen(p);
        const char *colon = find_colon(p, linelen);

        if (colon) {
            char name[64];
            unsigned long nl = (unsigned long)(colon - p);
            if (nl < sizeof(name)) {
                memcpy(name, p, nl);
                name[nl] = '\0';
                if (!drop_header(name)) {
                    memcpy(out + o, p, linelen);
                    o += linelen;
                    out[o++] = '\n';
                }
            }
        }
        if (eol == 0)
            break;
        p = eol + 1;
    }
    o += (unsigned long)snprintf(out + o, cap + 1 - o, "Content-Length: %lu\n",
                                 body_len);
    out[o] = '\0';
    return out;
}

int http_cache_store(struct http_cache *c, const char *key, int status,
                     const char *raw_headers, const void *body,
                     unsigned long len, long now, const char *method)
{
    struct cache_entry *e;
    char buf[512];
    unsigned long need;

    if (c == 0 || key == 0 || raw_headers == 0)
        return 0;
    if (method && !(ci_eq(method, "GET") || ci_eq(method, "HEAD")))
        return 0;
    if (!status_cacheable(status))
        return 0;
    if (len > HTTP_CACHE_ENTRY_MAX || len > c->max)
        return 0;
    /* We do not key on request headers, so anything that varies is unsafe
     * to replay. Accept-Encoding is fine: we store the decoded body. */
    if (raw_get(raw_headers, "Vary", buf, sizeof(buf))) {
        if (!ci_eq(buf, "Accept-Encoding"))
            return 0;
    }

    e = cache_find(c, key);
    if (e)
        cache_unlink(c, e);

    e = malloc(sizeof(*e));
    if (e == 0)
        return 0;
    memset(e, 0, sizeof(*e));
    e->key = dup_str(key);
    e->headers = sanitise_headers(raw_headers, len);
    e->status = status;
    e->stored = now;
    if (e->key == 0 || e->headers == 0) {
        entry_destroy(e);
        return 0;
    }
    if (!compute_freshness(e, raw_headers, now, status)) {
        entry_destroy(e);
        return 0;
    }
    if (raw_get(raw_headers, "ETag", buf, sizeof(buf)))
        e->etag = dup_str(buf);
    if (raw_get(raw_headers, "Last-Modified", buf, sizeof(buf)))
        e->last_mod = dup_str(buf);
    /* Nothing to revalidate with and already stale: pointless to keep. */
    if (e->etag == 0 && e->last_mod == 0 && e->expires <= now) {
        entry_destroy(e);
        return 0;
    }
    if (len > 0) {
        e->body = malloc(len);
        if (e->body == 0) {
            entry_destroy(e);
            return 0;
        }
        memcpy(e->body, body, len);
        e->len = len;
    }

    need = entry_bytes(e);
    while (c->bytes + need > c->max && c->head)
        evict_lru(c);
    if (c->bytes + need > c->max) {
        entry_destroy(e);
        return 0;
    }
    e->seq = ++c->tick;
    e->next = c->head;
    c->head = e;
    c->bytes += need;
    c->count++;
    return 1;
}

/* ---- lookup ----------------------------------------------------------- */

int http_cache_lookup(struct http_cache *c, const char *key, long now,
                      const char **etag, const char **last_mod)
{
    struct cache_entry *e;

    if (etag)
        *etag = 0;
    if (last_mod)
        *last_mod = 0;
    if (c == 0 || key == 0)
        return HTTP_CACHE_MISS;
    e = cache_find(c, key);
    if (e == 0)
        return HTTP_CACHE_MISS;
    e->seq = ++c->tick;

    if (etag)
        *etag = e->etag;
    if (last_mod)
        *last_mod = e->last_mod;

    if (!e->always_revalidate && e->have_expires && now < e->expires)
        return HTTP_CACHE_FRESH;
    if (e->etag || e->last_mod)
        return HTTP_CACHE_STALE;
    /* Stale and unverifiable: drop it and make the caller refetch. */
    cache_unlink(c, e);
    if (etag)
        *etag = 0;
    if (last_mod)
        *last_mod = 0;
    return HTTP_CACHE_MISS;
}

/* Split a raw block into a response's header array, same layout the
 * client builds so http_header_get() works on a replayed response. */
static int fill_headers(struct http_response *r, const char *raw)
{
    char *block, *p;
    int n = 0, i = 0;

    if (raw == 0)
        raw = "";
    block = dup_str(raw);
    if (block == 0)
        return HTTP_E_NOMEM;
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
            while (*v && is_blank((unsigned char)*v))
                v++;
            vl = strlen(v);
            while (vl > 0 && is_blank((unsigned char)v[vl - 1]))
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

int http_cache_fill(struct http_cache *c, const char *key,
                    struct http_response *out)
{
    struct cache_entry *e;
    int rc;

    if (c == 0 || key == 0 || out == 0)
        return HTTP_E_INVAL;
    e = cache_find(c, key);
    if (e == 0)
        return HTTP_E_INVAL;

    memset(out, 0, sizeof(*out));
    out->status = e->status;
    out->http_minor = 1;
    out->reason = dup_str(http_reason_phrase(e->status));
    rc = fill_headers(out, e->headers);
    if (rc != HTTP_OK) {
        http_response_free(out);
        return rc;
    }
    out->body = malloc(e->len + 1);
    if (out->body == 0) {
        http_response_free(out);
        return HTTP_E_NOMEM;
    }
    if (e->len)
        memcpy(out->body, e->body, e->len);
    out->body[e->len] = '\0';
    out->body_len = e->len;
    out->from_cache = 1;
    e->seq = ++c->tick;
    return HTTP_OK;
}

/* ---- 304 --------------------------------------------------------------- */

/* Replace or append each header from the 304 into the stored block. */
static char *merge_headers(const char *base, const char *fresh)
{
    unsigned long cap = strlen(base) + strlen(fresh) + 2;
    char *out;
    unsigned long o = 0;
    const char *p;

    if (cap > CACHE_HDR_MAX)
        return 0;
    out = malloc(cap + 1);
    if (out == 0)
        return 0;
    out[0] = '\0';

    for (p = base; *p;) {
        const char *eol = strchr(p, '\n');
        unsigned long linelen = eol ? (unsigned long)(eol - p) : strlen(p);
        const char *colon = find_colon(p, linelen);
        int replaced = 0;

        if (colon) {
            char name[64];
            unsigned long nl = (unsigned long)(colon - p);
            if (nl < sizeof(name)) {
                char vbuf[512];
                memcpy(name, p, nl);
                name[nl] = '\0';
                if (!drop_header(name) &&
                    raw_get(fresh, name, vbuf, sizeof(vbuf))) {
                    unsigned long need = nl + strlen(vbuf) + 3;
                    if (o + need <= cap) {
                        memcpy(out + o, name, nl);
                        o += nl;
                        out[o++] = ':';
                        out[o++] = ' ';
                        memcpy(out + o, vbuf, strlen(vbuf));
                        o += strlen(vbuf);
                        out[o++] = '\n';
                    }
                    replaced = 1;
                }
            }
        }
        if (!replaced && colon && o + linelen + 2 <= cap) {
            memcpy(out + o, p, linelen);
            o += linelen;
            out[o++] = '\n';
        }
        out[o] = '\0';
        if (eol == 0)
            break;
        p = eol + 1;
    }
    out[o] = '\0';
    /* Anything in the 304 we did not already have. */
    for (p = fresh; *p;) {
        const char *eol = strchr(p, '\n');
        unsigned long linelen = eol ? (unsigned long)(eol - p) : strlen(p);
        const char *colon = find_colon(p, linelen);

        if (colon) {
            char name[64];
            unsigned long nl = (unsigned long)(colon - p);
            if (nl < sizeof(name)) {
                char vbuf[512];
                memcpy(name, p, nl);
                name[nl] = '\0';
                if (!drop_header(name) &&
                    raw_get(out, name, vbuf, sizeof(vbuf)) == 0 &&
                    o + linelen + 2 <= cap) {
                    memcpy(out + o, p, linelen);
                    o += linelen;
                    out[o++] = '\n';
                    out[o] = '\0';
                }
            }
        }
        if (eol == 0)
            break;
        p = eol + 1;
    }
    out[o] = '\0';
    return out;
}

int http_cache_refresh(struct http_cache *c, const char *key,
                       const char *raw_headers, long now)
{
    struct cache_entry *e;
    char buf[512];
    char *merged;
    unsigned long before, after;

    if (c == 0 || key == 0 || raw_headers == 0)
        return 0;
    e = cache_find(c, key);
    if (e == 0)
        return 0;

    before = entry_bytes(e);
    merged = merge_headers(e->headers, raw_headers);
    if (merged) {
        free(e->headers);
        e->headers = merged;
    }
    e->stored = now;
    compute_freshness(e, e->headers, now, e->status);
    if (raw_get(raw_headers, "ETag", buf, sizeof(buf))) {
        free(e->etag);
        e->etag = dup_str(buf);
    }
    if (raw_get(raw_headers, "Last-Modified", buf, sizeof(buf))) {
        free(e->last_mod);
        e->last_mod = dup_str(buf);
    }
    e->seq = ++c->tick;
    after = entry_bytes(e);
    c->bytes = c->bytes - before + after;
    return 1;
}
