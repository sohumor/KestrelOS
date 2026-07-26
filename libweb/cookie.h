#pragma once

/* KestrelOS libweb: an RFC 6265 cookie jar.
 *
 * Parses Set-Cookie (Expires, Max-Age, Domain, Path, Secure, HttpOnly,
 * SameSite), applies the domain- and path-matching rules, expires
 * cookies, emits the Cookie request header in the order the RFC asks
 * for, and persists the jar to a file so a session survives a restart.
 *
 * "now" is passed in everywhere rather than read from the clock, so the
 * whole thing is a pure function of its inputs and expiry is testable.
 */

/* Where the browser keeps its jar. */
#define COOKIE_DEFAULT_FILE "/var/cookies.txt"

/* Ceilings. A hostile server cannot push the jar past these. */
#define COOKIE_PAIR_MAX       4096   /* name + "=" + value, bytes */
#define COOKIE_ATTR_MAX        512   /* one attribute value */
#define COOKIE_JAR_MAX        1000   /* cookies in the whole jar */
#define COOKIE_PER_DOMAIN_MAX   50
#define COOKIE_HEADER_MAX     8192   /* bytes of Cookie: header emitted */
#define COOKIE_FILE_MAX  (512UL * 1024UL)

/* SameSite values. */
#define COOKIE_SS_NONE   0
#define COOKIE_SS_LAX    1
#define COOKIE_SS_STRICT 2

/* Request context for SameSite decisions, passed to cookie_header(). */
#define COOKIE_CTX_SAME_SITE   0   /* same-site navigation or subresource */
#define COOKIE_CTX_CROSS_TOP   1   /* cross-site top-level navigation */
#define COOKIE_CTX_CROSS_SUB   2   /* cross-site subresource */

struct cookie {
    char *name;
    char *value;
    char *domain;              /* lower-case, no leading dot */
    char *path;
    long expires;              /* unix seconds; 0 for a session cookie */
    long created;
    long accessed;
    unsigned char host_only;
    unsigned char secure;
    unsigned char http_only;
    unsigned char samesite;    /* COOKIE_SS_* */
    unsigned char persistent;
    struct cookie *next;
};

struct cookie_jar;

struct cookie_jar *cookie_jar_new(void);
void cookie_jar_free(struct cookie_jar *j);
void cookie_jar_clear(struct cookie_jar *j);
int cookie_jar_count(const struct cookie_jar *j);
/* Walk the jar: cookie_jar_first() then c->next. Order is unspecified. */
const struct cookie *cookie_jar_first(const struct cookie_jar *j);

/* Apply one Set-Cookie field value. `host` is the request host (bare, no
 * brackets or port), `path` the request path. Returns 1 if a cookie was
 * stored or replaced, 0 if it was rejected, negative on allocation
 * failure. A cookie whose expiry is in the past deletes any match, which
 * also counts as 1. */
int cookie_set(struct cookie_jar *j, const char *set_cookie,
               const char *host, const char *path, int secure_channel,
               long now);

/* Build the Cookie: header value for a request. `http_api` non-zero means
 * this is a real HTTP request (HttpOnly cookies included); zero is the
 * document.cookie view. Returns the length written (0 = no cookies apply,
 * out is still NUL-terminated) or -1 if outsz was too small. */
long cookie_header(struct cookie_jar *j, const char *host, const char *path,
                   int secure_channel, int http_api, int ctx, long now,
                   char *out, unsigned long outsz);

/* Drop everything that expired at or before `now`. Returns the count. */
int cookie_expire(struct cookie_jar *j, long now);

/* Persistence. load merges into the jar, dropping expired entries; save
 * writes only persistent, unexpired ones. Return 0 or -1. */
int cookie_jar_load(struct cookie_jar *j, const char *file, long now);
int cookie_jar_save(struct cookie_jar *j, const char *file, long now);

/* ---- pieces of the algorithm, exposed because they are worth testing -- */

/* RFC 6265 5.1.3. Non-zero when `host` matches cookie-domain `domain`. */
int cookie_domain_match(const char *host, const char *domain);
/* RFC 6265 5.1.4. */
int cookie_path_match(const char *req_path, const char *cookie_path);
/* RFC 6265 5.1.4 default-path of a request path. */
int cookie_default_path(const char *req_path, char *out, unsigned long outsz);
/* RFC 6265 5.1.1 cookie-date. Returns unix seconds, or -1 if unparsable. */
long cookie_parse_date(const char *s);
/* Crude public-suffix guard: non-zero when no cookie may name `domain`. */
int cookie_public_suffix(const char *domain);
