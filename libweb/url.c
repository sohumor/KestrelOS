/* KestrelOS libweb: RFC 3986 URI handling.
 *
 * No allocation and no recursion anywhere in this file; every routine
 * works in caller storage or in fixed automatic buffers whose size is a
 * URL_*_MAX constant, so the worst-case stack cost is bounded and small
 * (url_resolve() is the deepest at roughly 2.5 KiB of path buffers).
 */

#include "url.h"

#include <string.h>

/* ---- character classes ---------------------------------------------- */

static int u_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int is_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int is_unreserved(int c)
{
    return is_alpha(c) || is_digit(c) ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

static int is_subdelim(int c)
{
    return c == '!' || c == '$' || c == '&' || c == '\'' || c == '(' ||
           c == ')' || c == '*' || c == '+' || c == ',' || c == ';' ||
           c == '=';
}

/* Is `c` allowed to stand for itself in component `comp`? */
static int comp_allows(int c, int comp)
{
    if (is_unreserved(c))
        return 1;
    switch (comp) {
    case URL_COMP_PATH:
        return is_subdelim(c) || c == ':' || c == '@' || c == '/';
    case URL_COMP_SEGMENT:
        return is_subdelim(c) || c == ':' || c == '@';
    case URL_COMP_QUERY:
    case URL_COMP_FRAGMENT:
        return is_subdelim(c) || c == ':' || c == '@' || c == '/' || c == '?';
    case URL_COMP_USERINFO:
        return is_subdelim(c) || c == ':';
    case URL_COMP_HOST:
        return is_subdelim(c);
    case URL_COMP_FORM:
    default:
        return 0;
    }
}

static const char hex_upper[] = "0123456789ABCDEF";

/* Copy `in` into `out`, percent-encoding whatever `comp` does not allow.
 * When keep_triplets is set an existing "%XY" is passed through with its
 * hex digits upper-cased instead of being escaped again. */
static long pct_copy(const char *in, unsigned long n, int comp,
                     int keep_triplets, char *out, unsigned long outsz)
{
    unsigned long i = 0, o = 0;

    if (outsz == 0)
        return -1;
    while (i < n) {
        unsigned char c = (unsigned char)in[i];

        if (c == '%' && keep_triplets && i + 2 < n &&
            hexval((unsigned char)in[i + 1]) >= 0 &&
            hexval((unsigned char)in[i + 2]) >= 0) {
            if (o + 3 >= outsz)
                return -1;
            out[o++] = '%';
            out[o++] = hex_upper[hexval((unsigned char)in[i + 1])];
            out[o++] = hex_upper[hexval((unsigned char)in[i + 2])];
            i += 3;
            continue;
        }
        if (comp == URL_COMP_FORM && c == ' ') {
            if (o + 1 >= outsz)
                return -1;
            out[o++] = '+';
            i++;
            continue;
        }
        if (comp_allows(c, comp)) {
            if (o + 1 >= outsz)
                return -1;
            out[o++] = (char)c;
            i++;
            continue;
        }
        if (o + 3 >= outsz)
            return -1;
        out[o++] = '%';
        out[o++] = hex_upper[(c >> 4) & 0xf];
        out[o++] = hex_upper[c & 0xf];
        i++;
    }
    out[o] = '\0';
    return (long)o;
}

long url_pct_encode(const char *in, unsigned long inlen, int comp,
                    char *out, unsigned long outsz)
{
    if (in == 0 || out == 0)
        return -1;
    if (inlen == ~0UL)
        inlen = strlen(in);
    return pct_copy(in, inlen, comp, comp != URL_COMP_FORM, out, outsz);
}

long url_pct_decode(const char *in, unsigned long inlen, int plus,
                    char *out, unsigned long outsz)
{
    unsigned long i = 0, o = 0;

    if (in == 0 || out == 0 || outsz == 0)
        return -1;
    if (inlen == ~0UL)
        inlen = strlen(in);
    while (i < inlen) {
        unsigned char c = (unsigned char)in[i];
        int h, l;

        if (c == '%' && i + 2 < inlen &&
            (h = hexval((unsigned char)in[i + 1])) >= 0 &&
            (l = hexval((unsigned char)in[i + 2])) >= 0) {
            c = (unsigned char)((h << 4) | l);
            i += 3;
        } else {
            if (plus && c == '+')
                c = ' ';
            i++;
        }
        if (o + 1 >= outsz)
            return -1;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    return (long)o;
}

/* ---- small helpers --------------------------------------------------- */

static int copy_n(char *dst, unsigned long dstsz, const char *src,
                  unsigned long n)
{
    if (n + 1 > dstsz)
        return URL_ETOOLONG;
    if (n)
        memcpy(dst, src, n);
    dst[n] = '\0';
    return URL_OK;
}

static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (u_lower((unsigned char)*a) != u_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int str_append(char *out, unsigned long outsz, unsigned long *o,
                      const char *s)
{
    unsigned long n = strlen(s);

    if (*o + n + 1 > outsz)
        return URL_ETOOLONG;
    memcpy(out + *o, s, n);
    *o += n;
    out[*o] = '\0';
    return URL_OK;
}

static int chr_append(char *out, unsigned long outsz, unsigned long *o, char c)
{
    if (*o + 2 > outsz)
        return URL_ETOOLONG;
    out[(*o)++] = c;
    out[*o] = '\0';
    return URL_OK;
}

static int uint_append(char *out, unsigned long outsz, unsigned long *o,
                       unsigned int v)
{
    char tmp[12];
    int n = 0;

    if (v == 0)
        tmp[n++] = '0';
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    if (*o + (unsigned long)n + 1 > outsz)
        return URL_ETOOLONG;
    while (n > 0)
        out[(*o)++] = tmp[--n];
    out[*o] = '\0';
    return URL_OK;
}

/* ---- authority ------------------------------------------------------- */

static int parse_authority(const char *a, const char *ae, struct url *u)
{
    const char *at = 0, *p, *hs, *he, *colon;
    int rc;

    for (p = a; p < ae; p++)
        if (*p == '@')
            at = p;                      /* last '@' wins */

    if (at) {
        rc = pct_copy(a, (unsigned long)(at - a), URL_COMP_USERINFO, 1,
                      u->userinfo, sizeof(u->userinfo)) < 0
                 ? URL_ETOOLONG : URL_OK;
        if (rc != URL_OK)
            return rc;
        u->has_userinfo = 1;
        hs = at + 1;
    } else {
        hs = a;
    }

    if (hs < ae && *hs == '[') {
        he = hs + 1;
        while (he < ae && *he != ']')
            he++;
        if (he >= ae)
            return URL_EPARSE;           /* unterminated literal */
        /* Only hex digits, ':' and '.' (IPv4-mapped tails); at least one
         * ':' so it cannot be a mis-bracketed name. */
        {
            const char *q;
            int colons = 0;
            if (he == hs + 1)
                return URL_EPARSE;
            for (q = hs + 1; q < he; q++) {
                if (*q == ':')
                    colons++;
                else if (hexval((unsigned char)*q) < 0 && *q != '.')
                    return URL_EPARSE;
            }
            if (colons == 0)
                return URL_EPARSE;
        }
        rc = copy_n(u->host, sizeof(u->host), hs + 1,
                    (unsigned long)(he - hs - 1));
        if (rc != URL_OK)
            return rc;
        u->is_ipv6 = 1;
        colon = (he + 1 < ae && he[1] == ':') ? he + 1 : 0;
        if (he + 1 < ae && he[1] != ':')
            return URL_EPARSE;           /* junk after the literal */
    } else {
        he = hs;
        while (he < ae && *he != ':')
            he++;
        {
            const char *q;
            for (q = hs; q < he; q++)
                if ((unsigned char)*q <= ' ' || *q == '[' || *q == ']' ||
                    *q == '\\' || (unsigned char)*q == 0x7f)
                    return URL_EPARSE;
        }
        if (pct_copy(hs, (unsigned long)(he - hs), URL_COMP_HOST, 1,
                     u->host, sizeof(u->host)) < 0)
            return URL_ETOOLONG;
        colon = (he < ae) ? he : 0;
    }

    {
        char *q;
        for (q = u->host; *q; q++)
            *q = (char)u_lower((unsigned char)*q);
    }

    if (colon) {
        const char *pp = colon + 1;
        long v = 0;
        if (pp == ae) {
            u->port = -1;                /* "host:" — default port */
        } else {
            for (; pp < ae; pp++) {
                if (!is_digit((unsigned char)*pp))
                    return URL_EPARSE;
                v = v * 10 + (*pp - '0');
                if (v > 65535)
                    return URL_EPARSE;
            }
            u->port = (int)v;
        }
    }
    return URL_OK;
}

/* ---- parse ----------------------------------------------------------- */

int url_parse(const char *s, struct url *u)
{
    const char *p, *q, *end;
    int rc;

    if (u == 0)
        return URL_EPARSE;
    memset(u, 0, sizeof(*u));
    u->port = -1;
    if (s == 0)
        return URL_EPARSE;

    /* Browsers strip surrounding whitespace before parsing; so do we. */
    while (*s && (unsigned char)*s <= ' ')
        s++;
    end = s + strlen(s);
    while (end > s && (unsigned char)end[-1] <= ' ')
        end--;
    if ((unsigned long)(end - s) >= URL_MAX)
        return URL_ETOOLONG;

    p = s;
    if (p < end && is_alpha((unsigned char)*p)) {
        q = p;
        while (q < end && (is_alpha((unsigned char)*q) ||
                           is_digit((unsigned char)*q) ||
                           *q == '+' || *q == '-' || *q == '.'))
            q++;
        if (q < end && *q == ':') {
            rc = copy_n(u->scheme, sizeof(u->scheme), p,
                        (unsigned long)(q - p));
            if (rc != URL_OK)
                return rc;
            {
                char *w;
                for (w = u->scheme; *w; w++)
                    *w = (char)u_lower((unsigned char)*w);
            }
            u->has_scheme = 1;
            p = q + 1;
        }
    }

    if (end - p >= 2 && p[0] == '/' && p[1] == '/') {
        const char *a = p + 2, *ae = a;
        while (ae < end && *ae != '/' && *ae != '?' && *ae != '#')
            ae++;
        rc = parse_authority(a, ae, u);
        if (rc != URL_OK)
            return rc;
        u->has_authority = 1;
        p = ae;
    }

    q = p;
    while (q < end && *q != '?' && *q != '#')
        q++;
    if (pct_copy(p, (unsigned long)(q - p), URL_COMP_PATH, 1,
                 u->path, sizeof(u->path)) < 0)
        return URL_ETOOLONG;

    if (q < end && *q == '?') {
        const char *qs = ++q;
        while (q < end && *q != '#')
            q++;
        if (pct_copy(qs, (unsigned long)(q - qs), URL_COMP_QUERY, 1,
                     u->query, sizeof(u->query)) < 0)
            return URL_ETOOLONG;
        u->has_query = 1;
    }
    if (q < end && *q == '#') {
        if (pct_copy(q + 1, (unsigned long)(end - q - 1), URL_COMP_FRAGMENT, 1,
                     u->fragment, sizeof(u->fragment)) < 0)
            return URL_ETOOLONG;
        u->has_fragment = 1;
    }

    /* A relative reference whose first segment contains ':' would be
     * re-parsed as a scheme; RFC 3986 4.2 forbids it. We accept it (the
     * damage is only in re-serialisation) but a path-only reference with
     * no authority must not start with "//" or it becomes an authority. */
    return URL_OK;
}

/* ---- dot-segment removal (RFC 3986 5.2.4) ---------------------------- */

static void remove_dot_segments(const char *in, char *buf, unsigned long bufsz,
                                char *out, unsigned long outsz)
{
    unsigned long i = 0, o = 0, n = strlen(in);

    out[0] = '\0';
    if (n + 1 > bufsz) {                 /* cannot happen: caller sizes it */
        copy_n(out, outsz, in, n < outsz - 1 ? n : outsz - 1);
        return;
    }
    memcpy(buf, in, n + 1);

    while (buf[i]) {
        if (buf[i] == '.' && buf[i + 1] == '.' && buf[i + 2] == '/') {
            i += 3;
        } else if (buf[i] == '.' && buf[i + 1] == '/') {
            i += 2;
        } else if (buf[i] == '/' && buf[i + 1] == '.' && buf[i + 2] == '/') {
            i += 2;
        } else if (buf[i] == '/' && buf[i + 1] == '.' && buf[i + 2] == '\0') {
            buf[i + 1] = '\0';           /* leaves "/" to be moved out */
        } else if (buf[i] == '/' && buf[i + 1] == '.' && buf[i + 2] == '.' &&
                   buf[i + 3] == '/') {
            i += 3;
            while (o > 0 && out[o - 1] != '/')
                o--;
            if (o > 0)
                o--;
            out[o] = '\0';
        } else if (buf[i] == '/' && buf[i + 1] == '.' && buf[i + 2] == '.' &&
                   buf[i + 3] == '\0') {
            buf[i + 1] = '\0';
            while (o > 0 && out[o - 1] != '/')
                o--;
            if (o > 0)
                o--;
            out[o] = '\0';
        } else if ((buf[i] == '.' && buf[i + 1] == '\0') ||
                   (buf[i] == '.' && buf[i + 1] == '.' && buf[i + 2] == '\0')) {
            break;
        } else {
            unsigned long j = i;
            if (buf[j] == '/')
                j++;
            while (buf[j] && buf[j] != '/')
                j++;
            if (o + (j - i) + 1 > outsz)
                break;                   /* truncate rather than overrun */
            memcpy(out + o, buf + i, j - i);
            o += j - i;
            out[o] = '\0';
            i = j;
        }
    }
    out[o] = '\0';
}

/* ---- normalisation --------------------------------------------------- */

int url_default_port(const char *scheme)
{
    if (scheme == 0)
        return -1;
    if (ci_eq(scheme, "http"))
        return 80;
    if (ci_eq(scheme, "https"))
        return 443;
    if (ci_eq(scheme, "ftp"))
        return 21;
    if (ci_eq(scheme, "ws"))
        return 80;
    if (ci_eq(scheme, "wss"))
        return 443;
    return -1;
}

int url_effective_port(const struct url *u)
{
    if (u->port >= 0)
        return u->port;
    return url_default_port(u->scheme);
}

/* Decode %XX triplets that stand for unreserved bytes, in place. */
static void decode_unreserved(char *s)
{
    unsigned long r = 0, w = 0;

    while (s[r]) {
        int h, l;
        if (s[r] == '%' && (h = hexval((unsigned char)s[r + 1])) >= 0 &&
            (l = hexval((unsigned char)s[r + 2])) >= 0) {
            int c = (h << 4) | l;
            if (is_unreserved(c)) {
                s[w++] = (char)c;
                r += 3;
                continue;
            }
            s[w++] = '%';
            s[w++] = hex_upper[h];
            s[w++] = hex_upper[l];
            r += 3;
            continue;
        }
        s[w++] = s[r++];
    }
    s[w] = '\0';
}

void url_normalize(struct url *u, int flags)
{
    if (u == 0)
        return;
    if ((flags & URL_N_PORT) && u->port >= 0 && u->has_scheme &&
        u->port == url_default_port(u->scheme))
        u->port = -1;

    if (flags & URL_N_DOTS) {
        char buf[URL_PATH_MAX];
        char out[URL_PATH_MAX];
        remove_dot_segments(u->path, buf, sizeof(buf), out, sizeof(out));
        memcpy(u->path, out, strlen(out) + 1);
    }
    if (flags & URL_N_PCT) {
        decode_unreserved(u->path);
        decode_unreserved(u->query);
        decode_unreserved(u->fragment);
    }
    if ((flags & URL_N_EMPTY_PATH) && u->has_authority && u->path[0] == '\0') {
        u->path[0] = '/';
        u->path[1] = '\0';
    }
}

/* ---- resolution (RFC 3986 5.2.2) ------------------------------------- */

/* RFC 3986 5.2.3 merge. */
static int merge_paths(const struct url *base, const char *ref,
                       char *out, unsigned long outsz)
{
    unsigned long o = 0;

    if (base->has_authority && base->path[0] == '\0') {
        if (chr_append(out, outsz, &o, '/') != URL_OK)
            return URL_ETOOLONG;
        return str_append(out, outsz, &o, ref);
    }
    {
        const char *slash = strrchr(base->path, '/');
        unsigned long keep = slash ? (unsigned long)(slash - base->path) + 1 : 0;
        if (keep + 1 > outsz)
            return URL_ETOOLONG;
        memcpy(out, base->path, keep);
        o = keep;
        out[o] = '\0';
    }
    return str_append(out, outsz, &o, ref);
}

int url_resolve(const struct url *base, const struct url *ref,
                struct url *out, int strict)
{
    char merged[URL_PATH_MAX];
    char scratch[URL_PATH_MAX];
    int use_ref_scheme;

    if (base == 0 || ref == 0 || out == 0)
        return URL_EPARSE;
    if (!base->has_scheme)
        return URL_EPARSE;               /* a base must be absolute */

    memset(out, 0, sizeof(*out));
    out->port = -1;

    use_ref_scheme = ref->has_scheme &&
                     (strict || !ci_eq(ref->scheme, base->scheme));

    if (use_ref_scheme) {
        memcpy(out, ref, sizeof(*out));
        remove_dot_segments(ref->path, merged, sizeof(merged),
                            scratch, sizeof(scratch));
        if (copy_n(out->path, sizeof(out->path), scratch,
                   strlen(scratch)) != URL_OK)
            return URL_ETOOLONG;
        return URL_OK;
    }

    /* Scheme always comes from the base from here down. */
    memcpy(out->scheme, base->scheme, sizeof(out->scheme));
    out->has_scheme = 1;

    if (ref->has_authority) {
        memcpy(out->userinfo, ref->userinfo, sizeof(out->userinfo));
        memcpy(out->host, ref->host, sizeof(out->host));
        out->port = ref->port;
        out->has_authority = 1;
        out->has_userinfo = ref->has_userinfo;
        out->is_ipv6 = ref->is_ipv6;
        remove_dot_segments(ref->path, merged, sizeof(merged),
                            scratch, sizeof(scratch));
        if (copy_n(out->path, sizeof(out->path), scratch,
                   strlen(scratch)) != URL_OK)
            return URL_ETOOLONG;
        out->has_query = ref->has_query;
        memcpy(out->query, ref->query, sizeof(out->query));
    } else {
        memcpy(out->userinfo, base->userinfo, sizeof(out->userinfo));
        memcpy(out->host, base->host, sizeof(out->host));
        out->port = base->port;
        out->has_authority = base->has_authority;
        out->has_userinfo = base->has_userinfo;
        out->is_ipv6 = base->is_ipv6;

        if (ref->path[0] == '\0') {
            memcpy(out->path, base->path, sizeof(out->path));
            if (ref->has_query) {
                out->has_query = 1;
                memcpy(out->query, ref->query, sizeof(out->query));
            } else {
                out->has_query = base->has_query;
                memcpy(out->query, base->query, sizeof(out->query));
            }
        } else {
            if (ref->path[0] == '/') {
                remove_dot_segments(ref->path, merged, sizeof(merged),
                                    scratch, sizeof(scratch));
            } else {
                int rc = merge_paths(base, ref->path, merged, sizeof(merged));
                if (rc != URL_OK)
                    return rc;
                {
                    char tmp[URL_PATH_MAX];
                    remove_dot_segments(merged, tmp, sizeof(tmp),
                                        scratch, sizeof(scratch));
                }
            }
            if (copy_n(out->path, sizeof(out->path), scratch,
                       strlen(scratch)) != URL_OK)
                return URL_ETOOLONG;
            out->has_query = ref->has_query;
            memcpy(out->query, ref->query, sizeof(out->query));
        }
    }

    out->has_fragment = ref->has_fragment;
    memcpy(out->fragment, ref->fragment, sizeof(out->fragment));
    return URL_OK;
}

int url_resolve_str(const char *base, const char *ref,
                    char *out, unsigned long outsz)
{
    struct url b, r, t;
    int rc;

    rc = url_parse(base, &b);
    if (rc != URL_OK)
        return rc;
    rc = url_parse(ref, &r);
    if (rc != URL_OK)
        return rc;
    rc = url_resolve(&b, &r, &t, 1);
    if (rc != URL_OK)
        return rc;
    return url_serialize(&t, out, outsz);
}

/* ---- serialisation --------------------------------------------------- */

int url_serialize(const struct url *u, char *out, unsigned long outsz)
{
    unsigned long o = 0;
    int rc;

    if (u == 0 || out == 0 || outsz == 0)
        return URL_ETOOLONG;
    out[0] = '\0';

    if (u->has_scheme) {
        if ((rc = str_append(out, outsz, &o, u->scheme)) != URL_OK)
            return rc;
        if ((rc = chr_append(out, outsz, &o, ':')) != URL_OK)
            return rc;
    }
    if (u->has_authority) {
        if ((rc = str_append(out, outsz, &o, "//")) != URL_OK)
            return rc;
        if (u->has_userinfo) {
            if ((rc = str_append(out, outsz, &o, u->userinfo)) != URL_OK)
                return rc;
            if ((rc = chr_append(out, outsz, &o, '@')) != URL_OK)
                return rc;
        }
        if (u->is_ipv6) {
            if ((rc = chr_append(out, outsz, &o, '[')) != URL_OK)
                return rc;
            if ((rc = str_append(out, outsz, &o, u->host)) != URL_OK)
                return rc;
            if ((rc = chr_append(out, outsz, &o, ']')) != URL_OK)
                return rc;
        } else if ((rc = str_append(out, outsz, &o, u->host)) != URL_OK) {
            return rc;
        }
        if (u->port >= 0) {
            if ((rc = chr_append(out, outsz, &o, ':')) != URL_OK)
                return rc;
            if ((rc = uint_append(out, outsz, &o,
                                  (unsigned int)u->port)) != URL_OK)
                return rc;
        }
    }
    if ((rc = str_append(out, outsz, &o, u->path)) != URL_OK)
        return rc;
    if (u->has_query) {
        if ((rc = chr_append(out, outsz, &o, '?')) != URL_OK)
            return rc;
        if ((rc = str_append(out, outsz, &o, u->query)) != URL_OK)
            return rc;
    }
    if (u->has_fragment) {
        if ((rc = chr_append(out, outsz, &o, '#')) != URL_OK)
            return rc;
        if ((rc = str_append(out, outsz, &o, u->fragment)) != URL_OK)
            return rc;
    }
    return URL_OK;
}

int url_origin(const struct url *u, char *out, unsigned long outsz)
{
    unsigned long o = 0;
    int rc, port;

    if (u == 0 || out == 0 || outsz == 0)
        return URL_ETOOLONG;
    out[0] = '\0';
    if ((rc = str_append(out, outsz, &o, u->scheme)) != URL_OK)
        return rc;
    if ((rc = str_append(out, outsz, &o, "://")) != URL_OK)
        return rc;
    if (u->is_ipv6 && (rc = chr_append(out, outsz, &o, '[')) != URL_OK)
        return rc;
    if ((rc = str_append(out, outsz, &o, u->host)) != URL_OK)
        return rc;
    if (u->is_ipv6 && (rc = chr_append(out, outsz, &o, ']')) != URL_OK)
        return rc;
    port = url_effective_port(u);
    if (port >= 0 && port != url_default_port(u->scheme)) {
        if ((rc = chr_append(out, outsz, &o, ':')) != URL_OK)
            return rc;
        if ((rc = uint_append(out, outsz, &o, (unsigned int)port)) != URL_OK)
            return rc;
    }
    return URL_OK;
}

int url_request_target(const struct url *u, char *out, unsigned long outsz)
{
    unsigned long o = 0;
    int rc;

    if (u == 0 || out == 0 || outsz == 0)
        return URL_ETOOLONG;
    out[0] = '\0';
    if (u->path[0] != '/') {
        if ((rc = chr_append(out, outsz, &o, '/')) != URL_OK)
            return rc;
    }
    if ((rc = str_append(out, outsz, &o, u->path)) != URL_OK)
        return rc;
    if (u->has_query) {
        if ((rc = chr_append(out, outsz, &o, '?')) != URL_OK)
            return rc;
        if ((rc = str_append(out, outsz, &o, u->query)) != URL_OK)
            return rc;
    }
    return URL_OK;
}

int url_host_header(const struct url *u, char *out, unsigned long outsz)
{
    unsigned long o = 0;
    int rc;

    if (u == 0 || out == 0 || outsz == 0)
        return URL_ETOOLONG;
    out[0] = '\0';
    if (u->is_ipv6 && (rc = chr_append(out, outsz, &o, '[')) != URL_OK)
        return rc;
    if ((rc = str_append(out, outsz, &o, u->host)) != URL_OK)
        return rc;
    if (u->is_ipv6 && (rc = chr_append(out, outsz, &o, ']')) != URL_OK)
        return rc;
    if (u->port >= 0 && u->port != url_default_port(u->scheme)) {
        if ((rc = chr_append(out, outsz, &o, ':')) != URL_OK)
            return rc;
        if ((rc = uint_append(out, outsz, &o,
                              (unsigned int)u->port)) != URL_OK)
            return rc;
    }
    return URL_OK;
}

int url_same_origin(const struct url *a, const struct url *b)
{
    if (a == 0 || b == 0)
        return 0;
    return ci_eq(a->scheme, b->scheme) && strcmp(a->host, b->host) == 0 &&
           url_effective_port(a) == url_effective_port(b);
}

const char *url_strerror(int err)
{
    switch (err) {
    case URL_OK:       return "ok";
    case URL_EPARSE:   return "malformed URL";
    case URL_ETOOLONG: return "URL component too long";
    default:           return "unknown URL error";
    }
}
