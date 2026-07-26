#pragma once

/* KestrelOS libweb: RFC 3986 URI parsing, normalisation, reference
 * resolution and serialisation.
 *
 * Everything here is pure computation over caller-supplied buffers: no
 * allocation, no I/O, no recursion. A struct url is about 2.7 KiB, so
 * hold at most a handful of them live on the 64 KiB user stack (the
 * resolver itself uses two internal path buffers, not another url).
 *
 * Parsing is strict about structure and lenient about bytes: a byte that
 * is not legal in the component it appears in is percent-encoded rather
 * than rejected, which is what browsers do and what keeps real-world
 * links working. Percent-triplets that are already well formed are kept
 * (with their hex digits upper-cased); a stray '%' becomes "%25".
 */

/* Component capacities, all including the terminating NUL. */
#define URL_SCHEME_MAX   24
#define URL_USERINFO_MAX 128
#define URL_HOST_MAX     256
#define URL_PATH_MAX     1024
#define URL_QUERY_MAX    1024
#define URL_FRAG_MAX     256
/* Longest URL string url_parse() accepts and url_serialize() produces. */
#define URL_MAX          2048

/* Result codes. */
#define URL_OK        0
#define URL_EPARSE   -1    /* structurally malformed */
#define URL_ETOOLONG -2    /* a component did not fit */

struct url {
    char scheme[URL_SCHEME_MAX];      /* lower-cased, no ':' */
    char userinfo[URL_USERINFO_MAX];  /* no '@' */
    char host[URL_HOST_MAX];          /* lower-cased, IPv6 without [] */
    char path[URL_PATH_MAX];
    char query[URL_QUERY_MAX];        /* no '?' */
    char fragment[URL_FRAG_MAX];      /* no '#' */
    int  port;                        /* -1 when absent or empty */
    unsigned char has_scheme;
    unsigned char has_authority;
    unsigned char has_userinfo;
    unsigned char has_query;
    unsigned char has_fragment;
    unsigned char is_ipv6;            /* host was a bracketed literal */
};

/* Components, for url_pct_encode(). Each keeps the byte set that RFC 3986
 * allows unescaped in that position. */
#define URL_COMP_PATH     0   /* pchar / "/" */
#define URL_COMP_SEGMENT  1   /* pchar only: "/" is escaped too */
#define URL_COMP_QUERY    2   /* pchar / "/" / "?" */
#define URL_COMP_FRAGMENT 3   /* pchar / "/" / "?" */
#define URL_COMP_USERINFO 4   /* unreserved / sub-delims / ":" */
#define URL_COMP_HOST     5   /* unreserved / sub-delims */
#define URL_COMP_FORM     6   /* x-www-form-urlencoded: unreserved, ' '->'+' */

/* Normalisation flags for url_normalize(). */
#define URL_N_PORT       0x01  /* drop a port equal to the scheme default */
#define URL_N_DOTS       0x02  /* remove "." and ".." segments */
#define URL_N_PCT        0x04  /* decode percent-triplets of unreserved bytes */
#define URL_N_EMPTY_PATH 0x08  /* empty path + authority -> "/" */
#define URL_N_ALL        0x0f

/* Parse an absolute URI or a relative reference. Returns URL_OK, or
 * URL_EPARSE / URL_ETOOLONG with *u left zeroed-but-valid. */
int url_parse(const char *s, struct url *u);

/* Apply the selected normalisations in place. Never fails; a component
 * that would grow past its capacity is left alone. */
void url_normalize(struct url *u, int flags);

/* RFC 3986 section 5.2 reference resolution. `strict` follows 5.2.2
 * exactly; strict == 0 uses the backwards-compatible rule that ignores a
 * scheme on the reference when it matches the base's ("http:g" against
 * "http://a/b/c/d;p?q" gives "http://a/b/c/g" instead of "http:g").
 * out may alias neither base nor ref. */
int url_resolve(const struct url *base, const struct url *ref,
                struct url *out, int strict);

/* Convenience: parse both, resolve, serialise. Uses strict resolution. */
int url_resolve_str(const char *base, const char *ref,
                    char *out, unsigned long outsz);

/* Write the URL back out. Returns URL_OK or URL_ETOOLONG. */
int url_serialize(const struct url *u, char *out, unsigned long outsz);

/* "scheme://host[:port]" with the default port omitted — the origin, used
 * as a connection-pool and cache key. */
int url_origin(const struct url *u, char *out, unsigned long outsz);

/* "path[?query]", always starting with '/': the HTTP request target. */
int url_request_target(const struct url *u, char *out, unsigned long outsz);

/* Host as it belongs in a Host: header — bracketed when it is an IPv6
 * literal, with ":port" appended when the port is not the default. */
int url_host_header(const struct url *u, char *out, unsigned long outsz);

/* Default port for a scheme, or -1 if unknown. */
int url_default_port(const char *scheme);

/* The port to actually connect to: explicit port, else scheme default. */
int url_effective_port(const struct url *u);

/* 1 when scheme, host and effective port all match. */
int url_same_origin(const struct url *a, const struct url *b);

/* Percent-encode `in` (inlen bytes, or ~0UL for strlen) into out for the
 * given URL_COMP_*. Bytes already part of a valid triplet are passed
 * through with upper-cased hex. Returns the length written, or -1 if it
 * did not fit. */
long url_pct_encode(const char *in, unsigned long inlen, int comp,
                    char *out, unsigned long outsz);

/* Percent-decode. `plus` non-zero also turns '+' into ' ' (form data).
 * An invalid triplet is copied through literally. Returns the length
 * written (out is NUL-terminated), or -1 if it did not fit. */
long url_pct_decode(const char *in, unsigned long inlen, int plus,
                    char *out, unsigned long outsz);

/* Human-readable text for a URL_E* code. */
const char *url_strerror(int err);
