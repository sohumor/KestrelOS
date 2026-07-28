#pragma once

/* KestrelOS libc: a minimal HTTP/1.1 client over plain TCP or libtls.
 * HTTPS uses TLS 1.3 with hostname, chain, validity and signature
 * verification enabled by default.
 */

#include <stdint.h>

/* Longest URL accepted, including the scheme and NUL. */
#define HTTP_URL_MAX    1024
/* Largest response body http_get() will accumulate (8 MiB). */
#define HTTP_MAX_BODY   (8UL * 1024UL * 1024UL)
/* 3xx responses followed before giving up. */
#define HTTP_MAX_REDIRECTS 3

/* Result codes. http_get() returns HTTP_OK whenever a complete response
 * was received and parsed - including 404 and other error statuses - so
 * callers must always look at *status as well. Negative values mean the
 * exchange never completed and *body is left untouched. */
#define HTTP_OK          0
#define HTTP_EURL       -1   /* malformed or over-long URL */
#define HTTP_EHTTPS     -2   /* legacy value; no longer returned */
#define HTTP_ESCHEME    -3   /* some other scheme, or no scheme at all */
#define HTTP_EDNS       -4   /* the host name did not resolve */
#define HTTP_ECONNECT   -5   /* TCP connect failed or timed out */
#define HTTP_ESEND      -6   /* the request could not be sent */
#define HTTP_ERECV      -7   /* the connection died mid-response */
#define HTTP_EPROTO     -8   /* the response was not valid HTTP/1.x */
#define HTTP_ETOOBIG    -9   /* body larger than HTTP_MAX_BODY */
#define HTTP_EREDIR    -10   /* too many redirects, or a bad Location */
#define HTTP_ENOMEM    -11   /* out of memory */
#define HTTP_ETLS      -12   /* TLS handshake, trust, or record failure */

/* Fetch `url` with GET.
 *
 * On HTTP_OK: *body points at a malloc()ed buffer of *len bytes with an
 * extra NUL past the end (so text bodies can be used as C strings) and
 * the caller must free() it; *status holds the HTTP status code.
 * `status` may be NULL; `body` and `len` may not.
 *
 * Redirects (301/302/303/307/308 with a Location:) are followed up to
 * HTTP_MAX_REDIRECTS times. Both Content-Length and chunked
 * transfer-encoding are understood, as is a body that simply runs to
 * connection close. */
int http_get(const char *url, char **body, unsigned long *len, int *status);

/* Human-readable text for a negative http_get() result. */
const char *http_strerror(int err);

/* Reason phrase for a status code, "Unknown" if it is not one we know. */
const char *http_status_text(int status);
