#pragma once

/* KestrelOS libweb: an HTTP/1.1 client a modern server is happy to talk to.
 *
 * What it does: persistent connections with a small pool, GET/HEAD/POST,
 * Content-Length / chunked / close framing, folded and duplicated headers,
 * gzip and deflate content coding, redirects with method rewriting and
 * loop detection, per-phase timeouts, cookies, and a response cache with
 * conditional revalidation.
 *
 * What it deliberately does not do: it never opens a socket itself. All
 * I/O goes through a struct http_transport obtained from a factory that
 * was registered for the URL's scheme, so TLS can be plugged in for
 * https:// without this file knowing anything about it.
 *
 * Memory: every buffer has a documented ceiling and the client fails with
 * HTTP_E_TOOBIG rather than growing without bound. Nothing here recurses.
 */

#include "url.h"

/* ---- result codes ---------------------------------------------------
 * Names are distinct from libc/include/http.h's HTTP_E* set on purpose,
 * so a translation unit can include both headers during the migration. */
#define HTTP_OK          0
#define HTTP_E_INVAL    -1   /* bad argument */
#define HTTP_E_URL      -2   /* malformed or over-long URL */
#define HTTP_E_SCHEME   -3   /* no transport registered for this scheme */
#define HTTP_E_DNS      -4   /* the host name did not resolve */
#define HTTP_E_CONNECT  -5   /* connect failed or timed out */
#define HTTP_E_SEND     -6   /* the request could not be sent */
#define HTTP_E_RECV     -7   /* the connection died mid-response */
#define HTTP_E_PROTO    -8   /* not valid HTTP/1.x */
#define HTTP_E_TOOBIG   -9   /* headers or body past the configured cap */
#define HTTP_E_REDIR   -10   /* too many redirects, or a bad Location */
#define HTTP_E_LOOP    -11   /* the redirect chain revisited a URL */
#define HTTP_E_NOMEM   -12   /* out of memory */
#define HTTP_E_TIMEOUT -13   /* a phase deadline expired */
#define HTTP_E_ABORT   -14   /* the body sink asked to stop */
#define HTTP_E_DECODE  -15   /* Content-Encoding could not be undone */

/* ---- limits (all overridable per request where it makes sense) ------- */
#define HTTP_BODY_MAX       (16UL * 1024UL * 1024UL)
#define HTTP_HEADERS_MAX    (32UL * 1024UL)  /* whole header block */
#define HTTP_LINE_MAX       8192             /* one header line, unfolded */
#define HTTP_HEADER_COUNT   128
#define HTTP_REDIRECT_MAX   10
#define HTTP_POOL_MAX       6                /* live pooled connections */
#define HTTP_RXBUF          4096             /* per-connection read buffer */
#define HTTP_IDLE_MS        30000            /* pooled idle connection life */
#define HTTP_CACHE_BYTES    (4UL * 1024UL * 1024UL)
#define HTTP_CACHE_ENTRY_MAX (1UL * 1024UL * 1024UL)

/* Default per-phase deadlines, milliseconds. */
#define HTTP_T_CONNECT  10000
#define HTTP_T_HEADERS  20000
#define HTTP_T_BODY     60000

/* ---- transport ------------------------------------------------------
 * read()  returns >0 bytes, 0 for a clean end of stream, <0 for an error
 *         (use HTTP_E_TIMEOUT for a deadline, anything else negative for
 *         a hard failure).
 * write() returns the number of bytes accepted (>0) or <0 on failure; a
 *         short write is fine, the caller loops.
 * close() releases everything, including ctx itself.
 * set_timeout() is optional (may be NULL). When present the client calls
 *         it before each phase with the remaining budget in milliseconds
 *         so a stalled server cannot hang a blocking read. A transport
 *         that leaves it NULL must enforce some timeout of its own. */
struct http_transport {
    void *ctx;
    int (*read)(void *ctx, void *buf, int len);
    int (*write)(void *ctx, const void *buf, int len);
    void (*close)(void *ctx);
    int (*set_timeout)(void *ctx, int ms);
};

/* Open a transport to host:port. Returns HTTP_OK with *out filled, or a
 * negative HTTP_E* code. `user` is the pointer given to
 * http_register_scheme() — a TLS factory would use it for its trust
 * store. `host` is the bare host (an IPv6 literal without brackets) and
 * doubles as the SNI name. */
typedef int (*http_transport_fn)(const char *host, int port, int timeout_ms,
                                 void *user, struct http_transport *out);

/* Bind a factory to a scheme ("http", "https", ...). Registering a
 * scheme twice replaces the earlier factory. Up to 8 schemes. Returns
 * HTTP_OK or HTTP_E_INVAL. Scheme names are compared case-insensitively.
 *
 * "http" is bound to the built-in TCP transport the first time a client
 * is created, unless something has already claimed it. */
int http_register_scheme(const char *scheme, http_transport_fn fn, void *user);

/* The built-in plain-TCP transport, exported so a TLS factory can layer
 * on top of it instead of reimplementing the socket handling. */
int http_tcp_transport(const char *host, int port, int timeout_ms,
                       void *user, struct http_transport *out);

/* ---- content decoding ------------------------------------------------
 * Wrapper constants match libz/inflate.h's. The client holds a function
 * pointer rather than calling inflate_buf() directly so libweb links
 * without libz until the integrator wires them together. */
#define HTTP_INFLATE_RAW  0
#define HTTP_INFLATE_ZLIB 1
#define HTTP_INFLATE_GZIP 2

typedef int (*http_inflate_fn)(const void *src, unsigned long slen,
                               void **out, unsigned long *olen, int wrapper);

/* Install the decompressor. Until this is called (or the file is built
 * with -DHTTP_HAVE_INFLATE, which makes inflate_buf the default) the
 * client does not advertise Accept-Encoding and rejects an encoded
 * response with HTTP_E_DECODE. */
void http_set_inflate(http_inflate_fn fn);

/* ---- responses ------------------------------------------------------ */

struct http_header {
    char *name;                 /* as sent, look-ups are case-insensitive */
    char *value;
};

struct http_response {
    int status;                 /* 100..599 */
    int http_minor;             /* 0 or 1 */
    char *reason;
    struct http_header *headers;
    int nheaders;
    char *body;                 /* NUL-terminated; NULL when streaming */
    unsigned long body_len;
    char *final_url;            /* after redirects, always set on HTTP_OK */
    int redirects;              /* hops taken */
    int from_cache;             /* 1 = served from cache (fresh or 304) */
    /* Private: the block the header strings point into. */
    char *_hdrblock;
};

void http_response_free(struct http_response *r);

/* First value for `name`, or NULL. */
const char *http_header_get(const struct http_response *r, const char *name);
/* Nth value (0-based) for `name`, or NULL — for Set-Cookie and friends. */
const char *http_header_nth(const struct http_response *r, const char *name,
                            int n);

/* ---- requests ------------------------------------------------------- */

/* Body sink. Returns 0 to keep going, non-zero to abort the transfer
 * (http_fetch then returns HTTP_E_ABORT). Called with decoded bytes. */
typedef int (*http_sink_fn)(void *user, const void *data, unsigned long len);

/* Called once, after the final response's headers are parsed and before
 * any body arrives. Returns 0 to continue, non-zero to abort. */
typedef int (*http_headers_fn)(void *user, const struct http_response *r);

/* Request flags. */
#define HTTP_F_NO_COOKIES   0x01
#define HTTP_F_NO_CACHE     0x02   /* neither read nor write the cache */
#define HTTP_F_NO_REDIRECT  0x04   /* return the 3xx as-is */
#define HTTP_F_NO_DECODE    0x08   /* hand back the coded bytes untouched */
#define HTTP_F_NO_KEEPALIVE 0x10   /* Connection: close, never pool */
#define HTTP_F_REFRESH      0x20   /* revalidate even if the entry is fresh */

struct http_request {
    const char *method;         /* NULL = "GET" */
    const char *url;
    const void *body;
    unsigned long body_len;
    const char *content_type;   /* required when body is set */
    const char *extra_headers;  /* raw "Name: value\r\n" lines, may be NULL */
    const char *accept;         /* NULL = a browser-ish default Accept */
    int flags;
    int max_redirects;          /* 0 = HTTP_REDIRECT_MAX */
    int connect_timeout_ms;     /* 0 = HTTP_T_CONNECT */
    int header_timeout_ms;      /* 0 = HTTP_T_HEADERS */
    int body_timeout_ms;        /* 0 = HTTP_T_BODY */
    unsigned long max_body;     /* 0 = HTTP_BODY_MAX */
    http_sink_fn sink;          /* NULL = accumulate into res->body */
    void *sink_user;
    http_headers_fn on_headers;
    void *headers_user;
};

/* ---- client --------------------------------------------------------- */

struct http_client;
struct cookie_jar;
struct http_cache;

struct http_client *http_client_new(void);
void http_client_free(struct http_client *c);

/* Cookie jar and cache are created with the client; these expose them so
 * the browser can list, clear or persist them. Either may be replaced
 * with NULL to disable the feature entirely. */
struct cookie_jar *http_client_jar(struct http_client *c);
struct http_cache *http_client_cache(struct http_client *c);
void http_client_set_cache(struct http_client *c, struct http_cache *cache);

/* Override the User-Agent (default "KestrelOS/1.0 (libweb)"). */
void http_client_set_agent(struct http_client *c, const char *ua);

/* Close every pooled connection. */
void http_client_drop_connections(struct http_client *c);

/* Counters, for tests and for the browser's network panel. */
struct http_stats {
    unsigned long requests;
    unsigned long connections;   /* transports actually opened */
    unsigned long reused;        /* requests served on a pooled connection */
    unsigned long cache_hits;    /* fresh, no network at all */
    unsigned long revalidations; /* conditional request sent */
    unsigned long not_modified;  /* 304 received */
    unsigned long bytes_in;      /* wire bytes read */
    unsigned long bytes_out;
};
void http_client_stats(const struct http_client *c, struct http_stats *out);

/* Perform a request, following redirects. Returns HTTP_OK when a complete
 * response was received — including 404 and 500, so always look at
 * res->status — or a negative HTTP_E*. On HTTP_OK the caller owns *res
 * and must http_response_free() it. */
int http_fetch(struct http_client *c, const struct http_request *req,
               struct http_response *res);

/* One-shot GET into memory. */
int http_get_url(struct http_client *c, const char *url,
                 struct http_response *res);

/* One-shot form POST. `body` is sent verbatim with the given type. */
int http_post_url(struct http_client *c, const char *url, const void *body,
                  unsigned long len, const char *content_type,
                  struct http_response *res);

const char *http_error_text(int err);
const char *http_reason_phrase(int status);

/* Parse an HTTP-date (IMF-fixdate, RFC 850 or asctime) to unix seconds,
 * or -1. Exported because the cache and the browser both need it. */
long http_parse_date(const char *s);

/* ---- response cache -------------------------------------------------
 * Implemented in cache.c. Declared here because libweb has no cache.h. */

struct http_cache *http_cache_new(unsigned long max_bytes);
void http_cache_free(struct http_cache *cache);
void http_cache_clear(struct http_cache *cache);
void http_cache_stat(const struct http_cache *cache, unsigned long *bytes,
                     int *entries);

/* What a lookup found. */
#define HTTP_CACHE_MISS    0
#define HTTP_CACHE_FRESH   1   /* usable without touching the network */
#define HTTP_CACHE_STALE   2   /* revalidate with the returned validators */

/* Look `key` up. On FRESH or STALE, *etag / *last_mod point at the stored
 * validators (NULL when absent) and stay valid until the next store or
 * clear on the same cache. */
int http_cache_lookup(struct http_cache *cache, const char *key, long now,
                      const char **etag, const char **last_mod);

/* Build a response from the stored entry. Returns HTTP_OK or a negative
 * code. Does not consume the entry. */
int http_cache_fill(struct http_cache *cache, const char *key,
                    struct http_response *out);

/* Store a response if it is cacheable. `raw_headers` is the unfolded
 * header block ("Name: value\n" lines). Returns 1 if stored. */
int http_cache_store(struct http_cache *cache, const char *key, int status,
                     const char *raw_headers, const void *body,
                     unsigned long len, long now, const char *method);

/* Apply a 304's headers to the stored entry (refreshes its freshness and
 * merges updated headers). Returns 1 if the entry was refreshed. */
int http_cache_refresh(struct http_cache *cache, const char *key,
                       const char *raw_headers, long now);

/* Drop one entry (used when a non-cacheable response supersedes it). */
void http_cache_invalidate(struct http_cache *cache, const char *key);
