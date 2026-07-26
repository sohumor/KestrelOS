/* KestrelOS libweb: RFC 6265 cookie jar.
 *
 * Storage is a singly linked list; jars are small by construction
 * (COOKIE_JAR_MAX entries, COOKIE_PER_DOMAIN_MAX per domain, a byte
 * ceiling on top of both) so linear scans are the right shape and there
 * is nothing here that a hostile Set-Cookie can make grow.
 *
 * No recursion. The deepest automatic buffer is cookie_set()'s working
 * copy of one Set-Cookie field, 8 KiB.
 */

#include "cookie.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HTTP_HOST
#include <fcntl.h>
#include <unistd.h>
static int ck_open_w(const char *p) { return open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600); }
#else
#include <kestrel.h>
static int ck_open_w(const char *p) { return open(p, O_WRONLY | O_CREAT | O_TRUNC); }
#endif

static int ck_open_r(const char *p) { return open(p, O_RDONLY); }

/* Longest Set-Cookie field value we will even look at. */
#define SC_MAX 8192
/* Total string bytes the jar may hold. */
#define JAR_BYTES_MAX (512UL * 1024UL)
/* Cookies considered for one Cookie: header. */
#define SEND_MAX 128
/* Browsers clamp cookie lifetimes; 400 days is the current consensus. */
#define COOKIE_AGE_CAP (400L * 86400L)

struct cookie_jar {
    struct cookie *head;
    int count;
    unsigned long bytes;
};

/* ---- tiny helpers ---------------------------------------------------- */

static int c_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int ci_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        int x = c_lower((unsigned char)*a), y = c_lower((unsigned char)*b);
        if (x != y)
            return x - y;
        a++;
        b++;
    }
    return c_lower((unsigned char)*a) - c_lower((unsigned char)*b);
}

static int is_space(int c)
{
    return c == ' ' || c == '\t';
}

static int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static char *ck_dup(const char *s)
{
    unsigned long n = strlen(s);
    char *p = malloc(n + 1);

    if (p)
        memcpy(p, s, n + 1);
    return p;
}

/* Trim ASCII blanks at both ends, in place. */
static char *trim(char *s)
{
    unsigned long n;

    while (*s && is_space((unsigned char)*s))
        s++;
    n = strlen(s);
    while (n > 0 && is_space((unsigned char)s[n - 1]))
        s[--n] = '\0';
    return s;
}

/* Drop control characters; a cookie octet may not contain them and they
 * would corrupt both the Cookie header and the persistence file. */
static void strip_ctl(char *s)
{
    unsigned long r = 0, w = 0;

    while (s[r]) {
        unsigned char c = (unsigned char)s[r++];
        if (c >= 0x20 && c != 0x7f)
            s[w++] = (char)c;
    }
    s[w] = '\0';
}

static void lower_str(char *s)
{
    for (; *s; s++)
        *s = (char)c_lower((unsigned char)*s);
}

static int host_is_ip(const char *h)
{
    int dots = 0;

    if (*h == '\0')
        return 0;
    for (; *h; h++) {
        if (*h == '.')
            dots++;
        else if (!is_digit((unsigned char)*h))
            return 0;
    }
    return dots == 3;
}

/* ---- dates (RFC 6265 5.1.1) ------------------------------------------ */

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

static int month_from_name(const char *s)
{
    static const char names[12][4] = {
        "jan", "feb", "mar", "apr", "may", "jun",
        "jul", "aug", "sep", "oct", "nov", "dec"
    };
    int i, k;

    for (i = 0; i < 12; i++) {
        for (k = 0; k < 3; k++)
            if (c_lower((unsigned char)s[k]) != names[i][k])
                break;
        if (k == 3)
            return i + 1;
    }
    return 0;
}

/* RFC 6265 delimiters: everything that is not a digit, a letter, ':' or
 * one of the few bytes the grammar keeps. */
static int is_delim(int c)
{
    return c == 0x09 || (c >= 0x20 && c <= 0x2f) ||
           (c >= 0x3b && c <= 0x40) || (c >= 0x5b && c <= 0x60) ||
           (c >= 0x7b && c <= 0x7e);
}

static int digits_prefix(const char *s, int max, long *out)
{
    int n = 0;
    long v = 0;

    while (n < max && is_digit((unsigned char)s[n])) {
        v = v * 10 + (s[n] - '0');
        n++;
    }
    *out = v;
    return n;
}

long cookie_parse_date(const char *s)
{
    int have_time = 0, have_day = 0, have_mon = 0, have_year = 0;
    int hh = 0, mm = 0, ss = 0, mon = 0;
    long day = 0, year = 0;
    unsigned long i = 0, n;

    if (s == 0)
        return -1;
    n = strlen(s);
    if (n > 128)
        return -1;

    while (i < n) {
        unsigned long start;
        const char *tok;

        while (i < n && is_delim((unsigned char)s[i]))
            i++;
        start = i;
        while (i < n && !is_delim((unsigned char)s[i]))
            i++;
        if (i == start)
            break;
        tok = s + start;

        if (!have_time) {
            long a, b, c;
            int p = digits_prefix(tok, 2, &a);
            if (p >= 1 && tok[p] == ':') {
                int q = digits_prefix(tok + p + 1, 2, &b);
                if (q >= 1 && tok[p + 1 + q] == ':') {
                    int r = digits_prefix(tok + p + 1 + q + 1, 2, &c);
                    if (r >= 1) {
                        hh = (int)a;
                        mm = (int)b;
                        ss = (int)c;
                        have_time = 1;
                        continue;
                    }
                }
            }
        }
        if (!have_day) {
            long v;
            int p = digits_prefix(tok, 2, &v);
            if (p >= 1 && (i - start) <= 2) {
                day = v;
                have_day = 1;
                continue;
            }
        }
        if (!have_mon && (i - start) >= 3) {
            int m = month_from_name(tok);
            if (m) {
                mon = m;
                have_mon = 1;
                continue;
            }
        }
        if (!have_year) {
            long v;
            int p = digits_prefix(tok, 4, &v);
            if (p >= 2 && (unsigned long)p == i - start) {
                year = v;
                have_year = 1;
                continue;
            }
        }
    }

    if (!have_time || !have_day || !have_mon || !have_year)
        return -1;
    if (year >= 70 && year <= 99)
        year += 1900;
    else if (year >= 0 && year <= 69)
        year += 2000;
    if (day < 1 || day > 31 || year < 1601 || hh > 23 || mm > 59 || ss > 59)
        return -1;
    return days_from_civil(year, mon, (int)day) * 86400L +
           hh * 3600L + mm * 60L + ss;
}

/* ---- matching -------------------------------------------------------- */

int cookie_domain_match(const char *host, const char *domain)
{
    unsigned long hl, dl;

    if (host == 0 || domain == 0 || *host == 0 || *domain == 0)
        return 0;
    if (ci_cmp(host, domain) == 0)
        return 1;
    hl = strlen(host);
    dl = strlen(domain);
    if (dl >= hl)
        return 0;
    if (host[hl - dl - 1] != '.')
        return 0;
    if (ci_cmp(host + hl - dl, domain) != 0)
        return 0;
    if (host_is_ip(host))
        return 0;
    return 1;
}

int cookie_path_match(const char *req_path, const char *cookie_path)
{
    unsigned long rl, cl;

    if (req_path == 0 || cookie_path == 0 || *cookie_path == 0)
        return 0;
    rl = strlen(req_path);
    cl = strlen(cookie_path);
    if (strcmp(req_path, cookie_path) == 0)
        return 1;
    if (cl > rl)
        return 0;
    if (memcmp(req_path, cookie_path, cl) != 0)
        return 0;
    if (cookie_path[cl - 1] == '/')
        return 1;
    return req_path[cl] == '/';
}

int cookie_default_path(const char *req_path, char *out, unsigned long outsz)
{
    const char *slash;
    unsigned long n;

    if (out == 0 || outsz < 2)
        return -1;
    if (req_path == 0 || req_path[0] != '/') {
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }
    /* Ignore any query string: the caller passes a path, but be safe. */
    slash = strrchr(req_path, '/');
    if (slash == req_path) {
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }
    n = (unsigned long)(slash - req_path);
    if (n + 1 > outsz)
        return -1;
    memcpy(out, req_path, n);
    out[n] = '\0';
    return 0;
}

/* A short list of second-level registries. Not a real PSL — the point is
 * to stop "Domain=co.uk" style supercookies, not to be exhaustive. */
static const char *const psl2[] = {
    "co.uk", "org.uk", "me.uk", "ltd.uk", "plc.uk", "net.uk", "sch.uk",
    "ac.uk", "gov.uk", "nhs.uk", "com.au", "net.au", "org.au", "edu.au",
    "gov.au", "id.au", "co.nz", "net.nz", "org.nz", "govt.nz", "ac.nz",
    "co.za", "org.za", "co.jp", "ne.jp", "or.jp", "ac.jp", "go.jp",
    "com.br", "net.br", "org.br", "gov.br", "com.cn", "net.cn", "org.cn",
    "gov.cn", "edu.cn", "co.in", "net.in", "org.in", "gen.in", "firm.in",
    "com.mx", "org.mx", "co.kr", "or.kr", "ne.kr", "com.tr", "gen.tr",
    "co.il", "org.il", "ac.il", "com.sg", "com.hk", "org.hk", "com.tw",
    "com.ar", "com.pl", "com.ua", "com.ru", "com.co", "co.id", "co.th",
    "github.io", "blogspot.com", "appspot.com", "s3.amazonaws.com", 0
};

/* Second-level labels that are registry-operated under a two-letter
 * ccTLD even when the exact pair is not in the list above. */
static const char *const psl2_generic[] = {
    "co", "com", "net", "org", "edu", "gov", "mil", "ac", "or", "ne",
    "go", "gr", "ltd", "plc", "sch", "nom", "info", "biz", "govt", 0
};

int cookie_public_suffix(const char *domain)
{
    const char *last, *p;
    int labels = 1;
    int i;

    if (domain == 0 || *domain == '\0')
        return 1;
    if (host_is_ip(domain))
        return 1;                       /* only host-only cookies for IPs */
    for (p = domain; *p; p++)
        if (*p == '.')
            labels++;
    if (labels < 2)
        return 1;                       /* bare TLD or single label */
    if (labels > 2)
        return 0;

    for (i = 0; psl2[i]; i++)
        if (ci_cmp(domain, psl2[i]) == 0)
            return 1;

    last = strrchr(domain, '.');
    if (last && strlen(last + 1) == 2) {
        char first[32];
        unsigned long n = (unsigned long)(last - domain);
        if (n < sizeof(first)) {
            memcpy(first, domain, n);
            first[n] = '\0';
            for (i = 0; psl2_generic[i]; i++)
                if (ci_cmp(first, psl2_generic[i]) == 0)
                    return 1;
        }
    }
    return 0;
}

/* ---- jar ------------------------------------------------------------- */

struct cookie_jar *cookie_jar_new(void)
{
    struct cookie_jar *j = malloc(sizeof(*j));

    if (j) {
        j->head = 0;
        j->count = 0;
        j->bytes = 0;
    }
    return j;
}

static unsigned long cookie_bytes(const struct cookie *c)
{
    return strlen(c->name) + strlen(c->value) + strlen(c->domain) +
           strlen(c->path) + 4;
}

static void cookie_destroy(struct cookie *c)
{
    free(c->name);
    free(c->value);
    free(c->domain);
    free(c->path);
    free(c);
}

static void jar_unlink(struct cookie_jar *j, struct cookie *victim)
{
    struct cookie **pp = &j->head;

    while (*pp) {
        if (*pp == victim) {
            *pp = victim->next;
            j->count--;
            j->bytes -= cookie_bytes(victim);
            cookie_destroy(victim);
            return;
        }
        pp = &(*pp)->next;
    }
}

void cookie_jar_clear(struct cookie_jar *j)
{
    struct cookie *c;

    if (j == 0)
        return;
    c = j->head;
    while (c) {
        struct cookie *n = c->next;
        cookie_destroy(c);
        c = n;
    }
    j->head = 0;
    j->count = 0;
    j->bytes = 0;
}

void cookie_jar_free(struct cookie_jar *j)
{
    if (j == 0)
        return;
    cookie_jar_clear(j);
    free(j);
}

int cookie_jar_count(const struct cookie_jar *j)
{
    return j ? j->count : 0;
}

const struct cookie *cookie_jar_first(const struct cookie_jar *j)
{
    return j ? j->head : 0;
}

int cookie_expire(struct cookie_jar *j, long now)
{
    struct cookie **pp;
    int dropped = 0;

    if (j == 0)
        return 0;
    pp = &j->head;
    while (*pp) {
        struct cookie *c = *pp;
        if (c->persistent && c->expires <= now) {
            *pp = c->next;
            j->count--;
            j->bytes -= cookie_bytes(c);
            cookie_destroy(c);
            dropped++;
            continue;
        }
        pp = &c->next;
    }
    return dropped;
}

/* Evict the least recently used cookie, preferring `domain` when given. */
static void jar_evict_one(struct cookie_jar *j, const char *domain)
{
    struct cookie *c, *victim = 0;

    for (c = j->head; c; c = c->next) {
        if (domain && ci_cmp(c->domain, domain) != 0)
            continue;
        if (victim == 0 || c->accessed < victim->accessed ||
            (c->accessed == victim->accessed && c->created < victim->created))
            victim = c;
    }
    if (victim)
        jar_unlink(j, victim);
}

static int jar_domain_count(const struct cookie_jar *j, const char *domain)
{
    const struct cookie *c;
    int n = 0;

    for (c = j->head; c; c = c->next)
        if (ci_cmp(c->domain, domain) == 0)
            n++;
    return n;
}

static struct cookie *jar_find(struct cookie_jar *j, const char *name,
                               const char *domain, const char *path)
{
    struct cookie *c;

    for (c = j->head; c; c = c->next)
        if (strcmp(c->name, name) == 0 && ci_cmp(c->domain, domain) == 0 &&
            strcmp(c->path, path) == 0)
            return c;
    return 0;
}

/* Insert or replace. Takes ownership of nothing; duplicates the strings. */
static int jar_store(struct cookie_jar *j, const char *name, const char *value,
                     const char *domain, const char *path, long expires,
                     int persistent, int host_only, int secure, int http_only,
                     int samesite, long now)
{
    struct cookie *c = jar_find(j, name, domain, path);
    long created = now;

    if (c) {
        created = c->created;
        jar_unlink(j, c);
    }
    while (jar_domain_count(j, domain) >= COOKIE_PER_DOMAIN_MAX)
        jar_evict_one(j, domain);
    while (j->count >= COOKIE_JAR_MAX ||
           j->bytes + strlen(name) + strlen(value) + strlen(domain) +
               strlen(path) + 4 > JAR_BYTES_MAX) {
        if (j->head == 0)
            return -1;
        jar_evict_one(j, 0);
    }

    c = malloc(sizeof(*c));
    if (c == 0)
        return -1;
    memset(c, 0, sizeof(*c));
    c->name = ck_dup(name);
    c->value = ck_dup(value);
    c->domain = ck_dup(domain);
    c->path = ck_dup(path);
    if (c->name == 0 || c->value == 0 || c->domain == 0 || c->path == 0) {
        cookie_destroy(c);
        return -1;
    }
    c->expires = expires;
    c->persistent = (unsigned char)(persistent != 0);
    c->host_only = (unsigned char)(host_only != 0);
    c->secure = (unsigned char)(secure != 0);
    c->http_only = (unsigned char)(http_only != 0);
    c->samesite = (unsigned char)samesite;
    c->created = created;
    c->accessed = now;
    c->next = j->head;
    j->head = c;
    j->count++;
    j->bytes += cookie_bytes(c);
    return 1;
}

/* ---- Set-Cookie ------------------------------------------------------ */

int cookie_set(struct cookie_jar *j, const char *set_cookie, const char *host,
               const char *path, int secure_channel, long now)
{
    char buf[SC_MAX];
    char domain[COOKIE_ATTR_MAX];
    char cpath[COOKIE_ATTR_MAX];
    char lhost[256];
    char *nv, *attrs, *eq, *name, *value;
    long expires = 0, maxage = 0;
    int have_expires = 0, have_maxage = 0, have_domain = 0, have_path = 0;
    int secure = 0, http_only = 0, samesite = COOKIE_SS_LAX;
    int host_only, persistent;
    unsigned long n;

    (void)secure_channel;               /* RFC 6265 allows Secure over http */

    if (j == 0 || set_cookie == 0 || host == 0)
        return 0;
    n = strlen(set_cookie);
    if (n == 0 || n >= sizeof(buf))
        return 0;
    memcpy(buf, set_cookie, n + 1);
    if (strlen(host) >= sizeof(lhost))
        return 0;
    strcpy(lhost, host);
    lower_str(lhost);

    attrs = strchr(buf, ';');
    if (attrs) {
        *attrs = '\0';
        attrs++;
    }
    nv = buf;
    eq = strchr(nv, '=');
    if (eq == 0)
        return 0;                       /* RFC 6265 5.2 step 2: ignore */
    *eq = '\0';
    name = trim(nv);
    value = trim(eq + 1);
    strip_ctl(name);
    strip_ctl(value);
    if (name[0] == '\0')
        return 0;
    if (strlen(name) + strlen(value) > COOKIE_PAIR_MAX)
        return 0;

    domain[0] = '\0';
    cpath[0] = '\0';

    while (attrs && *attrs) {
        char *semi = strchr(attrs, ';');
        char noval[1];
        char *an, *av;

        noval[0] = '\0';
        if (semi) {
            *semi = '\0';
            semi++;
        }
        an = attrs;
        av = strchr(an, '=');
        if (av) {
            *av = '\0';
            av++;
        }
        an = trim(an);
        av = av ? trim(av) : noval;
        strip_ctl(av);

        if (ci_cmp(an, "expires") == 0) {
            long t = cookie_parse_date(av);
            if (t >= 0) {
                expires = t;
                have_expires = 1;
            }
        } else if (ci_cmp(an, "max-age") == 0) {
            if (av[0] == '-' || is_digit((unsigned char)av[0])) {
                int neg = av[0] == '-';
                const char *p = av + (neg || av[0] == '+' ? 1 : 0);
                long v = 0;
                int ok = 0;
                while (is_digit((unsigned char)*p)) {
                    if (v > (COOKIE_AGE_CAP + 1))
                        v = COOKIE_AGE_CAP + 1;
                    else
                        v = v * 10 + (*p - '0');
                    p++;
                    ok = 1;
                }
                if (ok) {
                    maxage = neg ? -v : v;
                    have_maxage = 1;
                }
            }
        } else if (ci_cmp(an, "domain") == 0) {
            char *d = av;
            if (*d == '.')
                d++;
            if (*d && strlen(d) < sizeof(domain)) {
                strcpy(domain, d);
                lower_str(domain);
                have_domain = 1;
            }
        } else if (ci_cmp(an, "path") == 0) {
            if (av[0] == '/' && strlen(av) < sizeof(cpath)) {
                strcpy(cpath, av);
                have_path = 1;
            }
        } else if (ci_cmp(an, "secure") == 0) {
            secure = 1;
        } else if (ci_cmp(an, "httponly") == 0) {
            http_only = 1;
        } else if (ci_cmp(an, "samesite") == 0) {
            if (ci_cmp(av, "strict") == 0)
                samesite = COOKIE_SS_STRICT;
            else if (ci_cmp(av, "none") == 0)
                samesite = COOKIE_SS_NONE;
            else
                samesite = COOKIE_SS_LAX;
        }
        attrs = semi;
    }

    /* RFC 6265 5.3 step 4: Max-Age wins over Expires. */
    persistent = 0;
    if (have_maxage) {
        persistent = 1;
        if (maxage <= 0)
            expires = 0;                /* already expired: delete */
        else
            expires = now + (maxage > COOKIE_AGE_CAP ? COOKIE_AGE_CAP : maxage);
    } else if (have_expires) {
        persistent = 1;
        if (expires - now > COOKIE_AGE_CAP)
            expires = now + COOKIE_AGE_CAP;
    }

    if (have_domain) {
        if (cookie_public_suffix(domain)) {
            if (ci_cmp(domain, lhost) != 0)
                return 0;               /* supercookie attempt */
            host_only = 1;
            strcpy(domain, lhost);
        } else if (!cookie_domain_match(lhost, domain)) {
            return 0;
        } else {
            host_only = 0;
        }
    } else {
        if (strlen(lhost) >= sizeof(domain))
            return 0;
        strcpy(domain, lhost);
        host_only = 1;
    }

    if (!have_path) {
        if (cookie_default_path(path, cpath, sizeof(cpath)) != 0)
            return 0;
    }

    if (persistent && expires <= now) {
        struct cookie *old = jar_find(j, name, domain, cpath);
        if (old)
            jar_unlink(j, old);
        return 1;
    }

    return jar_store(j, name, value, domain, cpath, expires, persistent,
                     host_only, secure, http_only, samesite, now);
}

/* ---- Cookie: header -------------------------------------------------- */

static int send_allowed(const struct cookie *c, int ctx)
{
    if (ctx == COOKIE_CTX_SAME_SITE)
        return 1;
    if (ctx == COOKIE_CTX_CROSS_TOP)
        return c->samesite != COOKIE_SS_STRICT;
    return c->samesite == COOKIE_SS_NONE;
}

long cookie_header(struct cookie_jar *j, const char *host, const char *path,
                   int secure_channel, int http_api, int ctx, long now,
                   char *out, unsigned long outsz)
{
    struct cookie *pick[SEND_MAX];
    struct cookie *c;
    int n = 0, i, k;
    unsigned long o = 0;
    char lhost[256];

    if (out == 0 || outsz == 0)
        return -1;
    out[0] = '\0';
    if (j == 0 || host == 0 || path == 0)
        return 0;
    if (strlen(host) >= sizeof(lhost))
        return 0;
    strcpy(lhost, host);
    lower_str(lhost);

    for (c = j->head; c && n < SEND_MAX; c = c->next) {
        if (c->persistent && c->expires <= now)
            continue;
        if (c->host_only) {
            if (ci_cmp(lhost, c->domain) != 0)
                continue;
        } else if (!cookie_domain_match(lhost, c->domain)) {
            continue;
        }
        if (!cookie_path_match(path, c->path))
            continue;
        if (c->secure && !secure_channel)
            continue;
        if (c->http_only && !http_api)
            continue;
        if (!send_allowed(c, ctx))
            continue;
        pick[n++] = c;
    }

    /* RFC 6265 5.4: longer paths first, then earlier creation time.
     * Selection sort: n is at most SEND_MAX and usually tiny. */
    for (i = 0; i < n; i++) {
        int best = i;
        for (k = i + 1; k < n; k++) {
            unsigned long lb = strlen(pick[best]->path);
            unsigned long lk = strlen(pick[k]->path);
            if (lk > lb || (lk == lb && pick[k]->created < pick[best]->created))
                best = k;
        }
        if (best != i) {
            struct cookie *t = pick[i];
            pick[i] = pick[best];
            pick[best] = t;
        }
    }

    for (i = 0; i < n; i++) {
        unsigned long need = strlen(pick[i]->name) + 1 +
                             strlen(pick[i]->value) + (o ? 2 : 0);
        if (o + need + 1 > outsz || o + need + 1 > COOKIE_HEADER_MAX)
            break;
        if (o) {
            out[o++] = ';';
            out[o++] = ' ';
        }
        memcpy(out + o, pick[i]->name, strlen(pick[i]->name));
        o += strlen(pick[i]->name);
        out[o++] = '=';
        memcpy(out + o, pick[i]->value, strlen(pick[i]->value));
        o += strlen(pick[i]->value);
        pick[i]->accessed = now;
    }
    out[o] = '\0';
    return (long)o;
}

/* ---- persistence ----------------------------------------------------- */

int cookie_jar_save(struct cookie_jar *j, const char *file, long now)
{
    struct cookie *c;
    int fd;
    char line[COOKIE_PAIR_MAX + 1024];

    if (j == 0 || file == 0)
        return -1;
    fd = ck_open_w(file);
    if (fd < 0)
        return -1;
    {
        const char *hdr = "# KestrelOS cookie jar v1\n";
        if (write(fd, hdr, strlen(hdr)) < 0) {
            close(fd);
            return -1;
        }
    }
    for (c = j->head; c; c = c->next) {
        int len;
        if (!c->persistent || c->expires <= now)
            continue;
        len = snprintf(line, sizeof(line),
                       "%s\t%d\t%s\t%d\t%d\t%d\t%ld\t%ld\t%s\t%s\n",
                       c->domain, (int)c->host_only, c->path, (int)c->secure,
                       (int)c->http_only, (int)c->samesite, c->expires,
                       c->created, c->name, c->value);
        if (len <= 0 || (unsigned long)len >= sizeof(line))
            continue;
        if (write(fd, line, (unsigned long)len) < 0) {
            close(fd);
            return -1;
        }
    }
    close(fd);
    return 0;
}

static char *next_field(char **p)
{
    char *s = *p, *tab;

    if (s == 0)
        return 0;
    tab = strchr(s, '\t');
    if (tab) {
        *tab = '\0';
        *p = tab + 1;
    } else {
        *p = 0;
    }
    return s;
}

int cookie_jar_load(struct cookie_jar *j, const char *file, long now)
{
    char *buf, *p;
    int fd;
    long total = 0, got;

    if (j == 0 || file == 0)
        return -1;
    fd = ck_open_r(file);
    if (fd < 0)
        return -1;
    buf = malloc(COOKIE_FILE_MAX + 1);
    if (buf == 0) {
        close(fd);
        return -1;
    }
    while (total < (long)COOKIE_FILE_MAX) {
        got = read(fd, buf + total, (unsigned long)(COOKIE_FILE_MAX - total));
        if (got <= 0)
            break;
        total += got;
    }
    close(fd);
    buf[total] = '\0';

    p = buf;
    while (*p) {
        char *line = p;
        char *nl = strchr(p, '\n');
        char *f[10];
        int i;

        if (nl) {
            *nl = '\0';
            p = nl + 1;
        } else {
            p = line + strlen(line);
        }
        if (line[0] == '#' || line[0] == '\0')
            continue;
        {
            char *cur = line;
            for (i = 0; i < 10; i++) {
                f[i] = next_field(&cur);
                if (f[i] == 0)
                    break;
            }
            if (i < 10)
                continue;               /* malformed line: skip */
        }
        {
            long exp = atol(f[6]);
            long created = atol(f[7]);
            if (exp <= now)
                continue;
            if (f[8][0] == '\0')
                continue;
            if (jar_store(j, f[8], f[9], f[0], f[2], exp, 1, atoi(f[1]),
                          atoi(f[3]), atoi(f[4]), atoi(f[5]), now) < 0)
                break;
            /* Preserve the recorded creation order. */
            {
                struct cookie *c = jar_find(j, f[8], f[0], f[2]);
                if (c)
                    c->created = created;
            }
        }
    }
    free(buf);
    return 0;
}
