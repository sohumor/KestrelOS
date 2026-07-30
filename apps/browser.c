/* browser.c - the KestrelOS web browser.
 *
 * Both front ends use the same browser pipeline:
 *
 *   bytes -> DOM -> UA + author CSS -> computed styles -> layout
 *
 * Graphical mode paints that layout directly into the window.  Text mode
 * projects the same laid-out text to stdout, which keeps it useful over the
 * serial console and makes the complete stack testable without a framebuffer.
 *
 * Page bodies, subresources and scripts are bounded independently.  External
 * stylesheets and images share the HTTP cache/cookie jar with navigation, and
 * classic scripts run against a live DOM before layout.  HTTPS is always
 * TLS 1.3 with certificate verification required.
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gui.h>

/* libc also has a legacy http.h.  Relative includes make it impossible to
 * accidentally compile this application against that old interface. */
#include "../libgui/font.h"
#include "../libz/inflate.h"
#include "../libtls/tls.h"
#include "../libweb/dom.h"
#include "../libweb/css.h"
#include "../libweb/http.h"
#include "../libweb/cookie.h"
#include "../libweb/layout.h"
#include "../libweb/paint.h"
#include "../libweb/url.h"
#include "../libweb/jsdom.h"
#include "../libweb/storage.h"
#include "../libimg/img.h"

TLS_ASSERT_TRANSPORT_LAYOUT();

#define BROWSER_HOME "/doc/home.html"
#define FETCH_MAX    DOM_MAX_INPUT
#define RESOURCE_MAX (16UL * 1024UL * 1024UL)
#define CSS_TOTAL_MAX (4UL * 1024UL * 1024UL)
#define RESOURCE_TOTAL_MAX (48UL * 1024UL * 1024UL)
#define CSS_RESOURCE_MAX (1024UL * 1024UL)
#define SCRIPT_RESOURCE_MAX (2UL * 1024UL * 1024UL)
#define JS_FETCH_MAX (8UL * 1024UL * 1024UL)
#define RESOURCE_COUNT_MAX 192
#define CSS_DEPTH_MAX 4
#define HIST_MAX     64
#define TEXT_COLS    78
#define MAX_COLS     200
#define LINK_MAX     4096
#define STORAGE_QUOTA (1024UL * 1024UL)
#define STORAGE_FILE_MAX (8UL * 1024UL * 1024UL)

#define WIN_W   900
#define WIN_H   620
#define BAR_H   30
#define STAT_H  20
#define SB_W    12
#define PAD     6
#define GLYPH_W GUI_FONT_W
#define GLYPH_H GUI_FONT_H

#define C_BG      0x00FFFFFFu
#define C_TEXT    0x00101010u
#define C_LINK    0x000B3FBFu
#define C_CHROME  0x00D8D8D8u
#define C_FIELD   0x00FFFFFFu
#define C_FRAME   0x00707070u
#define C_BTN     0x00F0F0F0u
#define C_BTNDN   0x00A8A8A8u
#define C_DIM     0x00909090u
#define C_STATBG  0x00E8E8E8u
#define C_CARET   0x00202020u

#if defined(__GNUC__)
#define BROWSER_NOINLINE __attribute__((noinline))
#else
#define BROWSER_NOINLINE
#endif

static const char *g_cwd = "/";

struct browser_runtime {
    struct tls_options tls;
    struct http_client *http;
    struct css_stylesheet *ua;
    struct web_storage *local_storage;
    struct web_storage *session_storage;
    char storage_path[96];
    int store_live;
};

struct browser_image {
    char *url;
    struct image decoded;
    int state;                     /* 1 decoded, -1 failed */
    struct browser_image *next;
};

enum browser_load_state {
    PAGE_CREATED = 0,
    PAGE_NAVIGATING,
    PAGE_FETCHING,
    PAGE_RESPONSE,
    PAGE_PARSING,
    PAGE_STYLING,
    PAGE_LAYOUT,
    PAGE_SCRIPTING,
    PAGE_COMPLETE,
    PAGE_FAILED
};

struct browser_page {
    char *url;                     /* final URL, including redirects */
    char *base_url;                /* first valid <base href>, or url */
    struct dom_document *doc;
    struct css_stylesheet *author;
    struct style_engine *styles;
    struct lay_document *layout;
    struct jsdom *js;
    struct browser_runtime *runtime;
    struct browser_image *images;

    unsigned long bytes;
    unsigned long resource_bytes;
    unsigned int resources;
    unsigned int stylesheets;
    unsigned int scripts;
    unsigned int script_errors;
    unsigned int resource_errors;
    char cookie_view[COOKIE_HEADER_MAX];
    int status;                    /* HTTP status, 0 for local */
    int is_local;
    int is_html;
    int failed;                    /* transport/local error or non-2xx */
    enum browser_load_state load_state;
    char error[320];
};

/* ------------------------------------------------------------------ *
 * Small utilities
 * ------------------------------------------------------------------ */

static const char *page_state_name(enum browser_load_state state)
{
    static const char *const names[] = {
        "created", "navigating", "fetching", "response", "parsing",
        "styling", "layout", "scripting", "complete", "failed"
    };

    if ((unsigned int)state >= sizeof(names) / sizeof(names[0]))
        return "invalid";
    return names[state];
}

static int page_state_allows(enum browser_load_state from,
                             enum browser_load_state to)
{
    if (to == PAGE_FAILED)
        return 1;
    switch (from) {
    case PAGE_CREATED:
        return to == PAGE_NAVIGATING;
    case PAGE_NAVIGATING:
        return to == PAGE_FETCHING;
    case PAGE_FETCHING:
        return to == PAGE_RESPONSE;
    case PAGE_RESPONSE:
    case PAGE_FAILED:
        return to == PAGE_PARSING;
    case PAGE_PARSING:
        return to == PAGE_STYLING;
    case PAGE_STYLING:
        return to == PAGE_LAYOUT;
    case PAGE_LAYOUT:
        return to == PAGE_SCRIPTING || to == PAGE_COMPLETE;
    case PAGE_SCRIPTING:
        return to == PAGE_STYLING || to == PAGE_COMPLETE;
    case PAGE_COMPLETE:
        return to == PAGE_STYLING || to == PAGE_NAVIGATING;
    default:
        return 0;
    }
}

static int page_transition(struct browser_page *p,
                           enum browser_load_state next)
{
    if (!p || !page_state_allows(p->load_state, next)) {
        if (p)
            p->load_state = PAGE_FAILED;
        return -1;
    }
    p->load_state = next;
    return 0;
}

static char *str_dup(const char *s)
{
    unsigned long n;
    char *p;

    if (!s)
        return 0;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static int ascii_lower(int c)
{
    if (c >= 'A' && c <= 'Z')
        c += 'a' - 'A';
    return c;
}

static int checked_mul_add(unsigned long n, unsigned long mul,
                           unsigned long add, unsigned long *out)
{
    if (!out || (mul && n > (~0UL - add) / mul))
        return -1;
    *out = n * mul + add;
    return 0;
}

static int clamp_i64(int64_t v)
{
    if (v > 2147483647LL)
        return 2147483647;
    if (v < (-2147483647LL - 1LL))
        return -2147483647 - 1;
    return (int)v;
}

static int mul_div_i(int64_t a, int64_t b, int64_t divisor)
{
    if (!divisor)
        return 0;
    return clamp_i64((a * b) / divisor);
}

static int str_ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (ascii_lower((unsigned char)*a) !=
            ascii_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int str_ci_contains(const char *s, const char *needle)
{
    unsigned long n = strlen(needle);
    unsigned long i, j;

    if (!n)
        return 1;
    for (i = 0; s[i]; i++) {
        for (j = 0; j < n && s[i + j]; j++)
            if (ascii_lower((unsigned char)s[i + j]) !=
                ascii_lower((unsigned char)needle[j]))
                break;
        if (j == n)
            return 1;
    }
    return 0;
}

static int trim_copy(const char *src, char *dst, unsigned long cap)
{
    const char *end;
    unsigned long n;

    while (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n')
        src++;
    end = src + strlen(src);
    while (end > src && (end[-1] == ' ' || end[-1] == '\t' ||
                         end[-1] == '\r' || end[-1] == '\n'))
        end--;
    n = (unsigned long)(end - src);
    if (!n || n + 1 > cap)
        return -1;
    memcpy(dst, src, n);
    dst[n] = 0;
    return 0;
}

static int path_join(const char *tok, char *out, unsigned long outsz)
{
    int n;

    if (tok[0] == '/')
        n = snprintf(out, outsz, "%s", tok);
    else if (g_cwd[0] == '/' && g_cwd[1] == '\0')
        n = snprintf(out, outsz, "/%s", tok);
    else
        n = snprintf(out, outsz, "%s/%s", g_cwd, tok);
    return n >= 0 && (unsigned long)n < outsz ? 0 : -1;
}

static BROWSER_NOINLINE int url_supported(const char *s)
{
    struct url u;

    if (url_parse(s, &u) != URL_OK || !u.has_scheme)
        return 0;
    return str_ci_eq(u.scheme, "file") || str_ci_eq(u.scheme, "http") ||
           str_ci_eq(u.scheme, "https");
}

static BROWSER_NOINLINE int url_is_scheme(const char *s, const char *scheme)
{
    struct url u;

    return url_parse(s, &u) == URL_OK && u.has_scheme &&
           str_ci_eq(u.scheme, scheme);
}

/* Turn a target typed at the shell/address bar into an absolute URL.  Local
 * files use file: URLs too, so RFC 3986 resolution works for every page. */
static BROWSER_NOINLINE int canonicalize_input(const char *input, char *out,
                                               unsigned long outsz, char *err,
                                               unsigned long errsz)
{
    char in[URL_MAX];
    char path[URL_PATH_MAX];
    char tmp[URL_MAX];
    struct url u;
    struct k_stat st;
    int rc;

    if (trim_copy(input, in, sizeof(in)) != 0) {
        snprintf(err, errsz, "the address is empty or too long");
        return -1;
    }

    rc = url_parse(in, &u);
    if (rc == URL_OK && u.has_scheme) {
        /* file:foo is interpreted relative to the shell's cwd. */
        if (str_ci_eq(u.scheme, "file") && u.path[0] != '/') {
            char decoded[URL_PATH_MAX];

            if (url_pct_decode(u.path, ~0UL, 0, decoded,
                               sizeof(decoded)) < 0 ||
                path_join(decoded, path, sizeof(path)) != 0) {
                snprintf(err, errsz, "the local path is too long");
                return -1;
            }
            if (snprintf(tmp, sizeof(tmp), "file://%s", path) < 0) {
                snprintf(err, errsz, "cannot form a file URL");
                return -1;
            }
            rc = url_parse(tmp, &u);
            if (rc != URL_OK) {
                snprintf(err, errsz, "bad file URL: %s", url_strerror(rc));
                return -1;
            }
        }
        url_normalize(&u, URL_N_ALL);
        rc = url_serialize(&u, out, outsz);
        if (rc != URL_OK) {
            snprintf(err, errsz, "address is too long: %s",
                     url_strerror(rc));
            return -1;
        }
        return 0;
    }

    /* An existing path, or an explicitly absolute path, wins over the
     * hostname shorthand. */
    if (path_join(in, path, sizeof(path)) == 0 &&
        (in[0] == '/' || stat_(path, &st) == 0)) {
        int n = snprintf(tmp, sizeof(tmp), "file://%s", path);

        if (n < 0 || (unsigned long)n >= sizeof(tmp) ||
            url_parse(tmp, &u) != URL_OK) {
            snprintf(err, errsz, "the local path is too long");
            return -1;
        }
        url_normalize(&u, URL_N_ALL);
        if (url_serialize(&u, out, outsz) != URL_OK) {
            snprintf(err, errsz, "the local file URL is too long");
            return -1;
        }
        return 0;
    }

    if (snprintf(tmp, sizeof(tmp), "http://%s", in) < 0 ||
        url_parse(tmp, &u) != URL_OK) {
        snprintf(err, errsz, "malformed address");
        return -1;
    }
    url_normalize(&u, URL_N_ALL);
    if (url_serialize(&u, out, outsz) != URL_OK) {
        snprintf(err, errsz, "address is too long");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * Process-lifetime browser services
 * ------------------------------------------------------------------ */

static void runtime_storage_load(struct browser_runtime *rt)
{
    struct k_stat st;
    char *data;
    unsigned long got = 0;
    long n;
    int fd;

    if (!rt || !rt->local_storage ||
        stat_(rt->storage_path, &st) != 0 || st.is_dir ||
        st.size <= 0 || (unsigned long)st.size > STORAGE_FILE_MAX)
        return;
    fd = open(rt->storage_path, O_RDONLY);
    if (fd < 0)
        return;
    data = (char *)malloc((unsigned long)st.size);
    if (!data) {
        close(fd);
        return;
    }
    while (got < (unsigned long)st.size) {
        n = read(fd, data + got, (unsigned long)st.size - got);
        if (n <= 0)
            break;
        got += (unsigned long)n;
    }
    close(fd);
    if (got == (unsigned long)st.size)
        web_storage_import(rt->local_storage, data, got);
    free(data);
}

static void runtime_storage_save(struct browser_runtime *rt)
{
    char *data;
    unsigned long len = 0, wrote = 0;
    long n;
    int fd;

    if (!rt || !rt->local_storage ||
        !web_storage_dirty(rt->local_storage))
        return;
    data = web_storage_export(rt->local_storage, &len);
    if (!data)
        return;
#ifdef JS_HOST
    fd = open(rt->storage_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
#else
    fd = open(rt->storage_path, O_WRONLY | O_CREAT | O_TRUNC);
#endif
    if (fd >= 0) {
        while (wrote < len) {
            n = write(fd, data + wrote, len - wrote);
            if (n <= 0)
                break;
            wrote += (unsigned long)n;
        }
        close(fd);
        if (wrote == len)
            web_storage_clear_dirty(rt->local_storage);
    }
    free(data);
}

static void runtime_destroy(struct browser_runtime *rt)
{
    if (!rt)
        return;
    runtime_storage_save(rt);
    web_storage_free(rt->session_storage);
    rt->session_storage = 0;
    web_storage_free(rt->local_storage);
    rt->local_storage = 0;
    if (rt->http) {
        struct cookie_jar *jar = http_client_jar(rt->http);
        long now = syscall(SYS_TIME, 0, 0, 0, 0);

        if (jar)
            cookie_jar_save(jar, COOKIE_DEFAULT_FILE, now);
        http_client_drop_connections(rt->http);
        http_client_free(rt->http);
        rt->http = 0;
    }
    css_free(rt->ua);
    rt->ua = 0;
    if (rt->store_live) {
        tls_default_store_free();
        rt->store_live = 0;
    }
}

static BROWSER_NOINLINE int runtime_init(struct browser_runtime *rt, char *err,
                                         unsigned long errsz)
{
    struct css_media media;
    struct x509_store *roots;
    int rc;

    memset(rt, 0, sizeof(*rt));
    snprintf(rt->storage_path, sizeof(rt->storage_path),
             "/tmp/.kestrel-browser-%ld.storage",
             syscall(SYS_GETUID, 0, 0, 0, 0));
    rt->local_storage = web_storage_new(STORAGE_QUOTA, 2048);
    rt->session_storage = web_storage_new(STORAGE_QUOTA, 1024);
    if (!rt->local_storage || !rt->session_storage) {
        snprintf(err, errsz, "out of memory creating browser storage");
        runtime_destroy(rt);
        return -1;
    }
    runtime_storage_load(rt);
    tls_options_default(&rt->tls);
    rt->tls.verify = TLS_VERIFY_REQUIRED;
    rt->tls.alpn = "http/1.1";

    roots = tls_default_store();
    if (!roots) {
        snprintf(err, errsz, "cannot build the TLS trust store");
        return -1;
    }
    rt->store_live = 1;
    rt->tls.roots = roots;

    rc = TLS_REGISTER_HTTPS(&rt->tls);
    if (rc != HTTP_OK) {
        snprintf(err, errsz, "cannot register verified HTTPS: %s",
                 http_error_text(rc));
        runtime_destroy(rt);
        return -1;
    }
    http_set_inflate(inflate_buf);

    rt->http = http_client_new();
    if (!rt->http) {
        snprintf(err, errsz, "out of memory creating the HTTP client");
        runtime_destroy(rt);
        return -1;
    }
    http_client_set_agent(rt->http, "KestrelOS/1.0 (browser)");
    cookie_jar_load(http_client_jar(rt->http), COOKIE_DEFAULT_FILE,
                    syscall(SYS_TIME, 0, 0, 0, 0));

    memset(&media, 0, sizeof(media));
    media.width = WIN_W - SB_W;
    media.height = WIN_H - BAR_H - STAT_H;
    media.dpi = 96;
    media.screen = 1;
    rt->ua = css_parse(css_ua_stylesheet(), css_ua_stylesheet_len(),
                       CSS_ORIGIN_UA, &media);
    if (!rt->ua) {
        snprintf(err, errsz, "out of memory parsing the browser stylesheet");
        runtime_destroy(rt);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * Input and generated local documents
 * ------------------------------------------------------------------ */

static int sniff_html(const char *b, unsigned long n)
{
    static const char *const marks[] = {
        "<html", "<!doctype", "<head", "<body", "<div", "<p", "<title",
        "<meta", "<span", "<h1", "<table", "<ul", "<ol", "<a", "<br", 0
    };
    unsigned long i, j, lim = n < 1024 ? n : 1024;
    int k;

    for (i = 0; i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' ||
                          b[i] == '\n'); i++)
        ;
    if (i < n && b[i] == '<')
        return 1;
    for (k = 0; marks[k]; k++) {
        unsigned long ml = strlen(marks[k]);

        for (i = 0; i + ml <= lim; i++) {
            for (j = 0; j < ml; j++)
                if (ascii_lower((unsigned char)b[i + j]) != marks[k][j])
                    break;
            if (j == ml)
                return 1;
        }
    }
    return 0;
}

static void html_append(char *dst, unsigned long cap, unsigned long *used,
                        const char *s)
{
    while (*s && *used + 1 < cap)
        dst[(*used)++] = *s++;
}

static void html_append_escaped(char *dst, unsigned long cap,
                                unsigned long *used, const char *s,
                                unsigned long n)
{
    unsigned long i;

    for (i = 0; i < n && *used + 1 < cap; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == '&')
            html_append(dst, cap, used, "&amp;");
        else if (c == '<')
            html_append(dst, cap, used, "&lt;");
        else if (c == '>')
            html_append(dst, cap, used, "&gt;");
        else if (c == '"')
            html_append(dst, cap, used, "&quot;");
        else if (c == '\n' || c == '\t' || c == '\r')
            dst[(*used)++] = (char)c;
        else if (c < 32 || c >= 127)
            dst[(*used)++] = '.';
        else
            dst[(*used)++] = (char)c;
    }
}

static BROWSER_NOINLINE char *
make_plain_source(const char *body, unsigned long len, int status,
                  unsigned long *out_len)
{
    unsigned long cap;
    unsigned long used = 0;
    char head[192];
    char *out;

    /* &quot; is the longest escape emitted above: six bytes per input
     * byte, plus the fixed document/status wrapper. */
    if (checked_mul_add(len, 6, 768, &cap) != 0 ||
        cap > DOM_MAX_INPUT)
        return 0;
    out = (char *)malloc(cap);
    if (!out)
        return 0;
    html_append(out, cap, &used,
                "<html><head><title>Text document</title></head><body>");
    if (status && (status < 200 || status >= 300)) {
        snprintf(head, sizeof(head),
                 "<div style=\"padding:8px;border:2px solid #a00000;"
                 "background:#fff0f0\"><h2>HTTP %d</h2>"
                 "<p>The server returned status %d.</p></div>",
                 status, status);
        html_append(out, cap, &used, head);
    }
    html_append(out, cap, &used, "<pre>");
    html_append_escaped(out, cap, &used, body, len);
    html_append(out, cap, &used, "</pre></body></html>");
    out[used] = 0;
    *out_len = used;
    return out;
}

static BROWSER_NOINLINE char *
make_status_source(const char *body, unsigned long len, int status,
                   unsigned long *out_len)
{
    char head[256];
    unsigned long hn;
    char *out;
    int n;

    n = snprintf(head, sizeof(head),
                 "<div style=\"padding:8px;border:2px solid #a00000;"
                 "background:#fff0f0\"><h2>HTTP %d</h2>"
                 "<p>The server returned status %d for this address.</p>"
                 "</div>", status, status);
    if (n < 0)
        return 0;
    hn = (unsigned long)n < sizeof(head) ? (unsigned long)n
                                         : sizeof(head) - 1;
    if (len > DOM_MAX_INPUT - hn)
        return 0;
    out = (char *)malloc(hn + len + 1);
    if (!out)
        return 0;
    memcpy(out, head, hn);
    memcpy(out + hn, body, len);
    out[hn + len] = 0;
    *out_len = hn + len;
    return out;
}

static BROWSER_NOINLINE char *
make_error_source(const char *url, const char *message, unsigned long *out_len)
{
    unsigned long un = strlen(url), mn = strlen(message);
    unsigned long total, cap;
    unsigned long used = 0;
    char *out;

    if (un > ~0UL - mn)
        return 0;
    total = un + mn;
    if (checked_mul_add(total, 6, 512, &cap) != 0 ||
        cap > DOM_MAX_INPUT)
        return 0;
    out = (char *)malloc(cap);
    if (!out)
        return 0;
    html_append(out, cap, &used,
                "<html><head><title>Cannot load page</title></head><body>"
                "<h1>Cannot load page</h1><p><b>");
    html_append_escaped(out, cap, &used, message, strlen(message));
    html_append(out, cap, &used, "</b></p><p>while fetching:</p><pre>");
    html_append_escaped(out, cap, &used, url, strlen(url));
    html_append(out, cap, &used,
                "</pre><hr><p>Check the address and network, then try "
                "again.</p></body></html>");
    out[used] = 0;
    *out_len = used;
    return out;
}

static BROWSER_NOINLINE int
read_local_url(const char *target, char **body, unsigned long *len, char *err,
               unsigned long errsz)
{
    struct url u;
    struct k_stat st;
    char path[URL_PATH_MAX];
    int fd;
    long n;
    unsigned long got = 0;

    *body = 0;
    *len = 0;
    if (url_parse(target, &u) != URL_OK || !str_ci_eq(u.scheme, "file")) {
        snprintf(err, errsz, "malformed local file URL");
        return -1;
    }
    if (url_pct_decode(u.path, ~0UL, 0, path, sizeof(path)) < 0 ||
        path[0] != '/') {
        snprintf(err, errsz, "local file path is invalid or too long");
        return -1;
    }
    if (stat_(path, &st) != 0) {
        snprintf(err, errsz, "no such file: %s", path);
        return -1;
    }
    if (st.is_dir) {
        snprintf(err, errsz, "%s is a directory", path);
        return -1;
    }
    if ((unsigned long)st.size > FETCH_MAX) {
        snprintf(err, errsz, "%s is too large (%lu bytes; limit %lu)",
                 path, (unsigned long)st.size, FETCH_MAX);
        return -1;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(err, errsz, "cannot open %s (permission denied?)", path);
        return -1;
    }
    *body = (char *)malloc((unsigned long)st.size + 1);
    if (!*body) {
        close(fd);
        snprintf(err, errsz, "out of memory reading %s", path);
        return -1;
    }
    while (got < (unsigned long)st.size) {
        n = read(fd, *body + got, (unsigned long)st.size - got);
        if (n < 0) {
            close(fd);
            free(*body);
            *body = 0;
            snprintf(err, errsz, "read failed for %s", path);
            return -1;
        }
        if (n == 0)
            break;
        got += (unsigned long)n;
    }
    close(fd);
    (*body)[got] = 0;
    *len = got;
    return 0;
}

/* ------------------------------------------------------------------ *
 * Page subresources
 * ------------------------------------------------------------------ */

struct resource_body {
    char *data;
    unsigned long len;
    char *url;
};

struct bytebuf {
    char *p;
    unsigned long n, cap, limit;
};

static void resource_body_free(struct resource_body *r)
{
    if (!r)
        return;
    free(r->data);
    free(r->url);
    memset(r, 0, sizeof(*r));
}

static int buf_reserve(struct bytebuf *b, unsigned long extra)
{
    unsigned long need, cap;
    char *p;

    if (extra > b->limit - b->n)
        return -1;
    need = b->n + extra + 1;
    if (need <= b->cap)
        return 0;
    cap = b->cap ? b->cap : 4096;
    while (cap < need) {
        if (cap > b->limit / 2) {
            cap = b->limit + 1;
            break;
        }
        cap *= 2;
    }
    if (cap > b->limit + 1)
        cap = b->limit + 1;
    p = (char *)realloc(b->p, cap);
    if (!p)
        return -1;
    b->p = p;
    b->cap = cap;
    return 0;
}

static int buf_add(struct bytebuf *b, const char *s, unsigned long n)
{
    if (buf_reserve(b, n) != 0)
        return -1;
    if (n)
        memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
    return 0;
}

static int rel_has_token(const char *rel, const char *token)
{
    unsigned long tn = strlen(token);

    if (!rel)
        return 0;
    while (*rel) {
        const char *s;
        unsigned long n;

        while (*rel == ' ' || *rel == '\t' || *rel == '\r' ||
               *rel == '\n')
            rel++;
        s = rel;
        while (*rel && *rel != ' ' && *rel != '\t' && *rel != '\r' &&
               *rel != '\n')
            rel++;
        n = (unsigned long)(rel - s);
        if (n == tn) {
            unsigned long i;

            for (i = 0; i < n; i++)
                if (ascii_lower((unsigned char)s[i]) !=
                    ascii_lower((unsigned char)token[i]))
                    break;
            if (i == n)
                return 1;
        }
    }
    return 0;
}

static int resource_url(struct browser_page *p, const char *base,
                        const char *ref, char *out, unsigned long outsz,
                        char *err, unsigned long errsz)
{
    struct url page, u;

    if (!ref || !*ref ||
        url_resolve_str(base ? base : p->base_url, ref, out, outsz) !=
            URL_OK ||
        url_parse(out, &u) != URL_OK || !u.has_scheme ||
        (!str_ci_eq(u.scheme, "file") && !str_ci_eq(u.scheme, "http") &&
         !str_ci_eq(u.scheme, "https"))) {
        snprintf(err, errsz, "unsupported or malformed resource URL");
        return -1;
    }
    if (url_parse(p->url, &page) == URL_OK) {
        if (str_ci_eq(page.scheme, "https") && !str_ci_eq(u.scheme, "https")) {
            snprintf(err, errsz, "blocked insecure mixed content");
            return -1;
        }
        if ((str_ci_eq(page.scheme, "http") ||
             str_ci_eq(page.scheme, "https")) &&
            str_ci_eq(u.scheme, "file")) {
            snprintf(err, errsz, "blocked network access to a local file");
            return -1;
        }
    }
    return 0;
}

static BROWSER_NOINLINE int
resource_fetch(struct browser_page *p, const char *base, const char *ref,
               unsigned long max, const char *accept,
               struct resource_body *out, char *err, unsigned long errsz)
{
    char absolute[URL_MAX];

    memset(out, 0, sizeof(*out));
    if (p->resources >= RESOURCE_COUNT_MAX) {
        snprintf(err, errsz, "page resource-count limit reached");
        return -1;
    }
    if (resource_url(p, base, ref, absolute, sizeof(absolute), err, errsz) != 0)
        return -1;
    if (url_is_scheme(absolute, "file")) {
        if (read_local_url(absolute, &out->data, &out->len, err, errsz) != 0)
            return -1;
        if (out->len > max) {
            snprintf(err, errsz, "resource is larger than %lu bytes", max);
            resource_body_free(out);
            return -1;
        }
        out->url = str_dup(absolute);
    } else {
        struct http_request req;
        struct http_response res;
        int rc;

        memset(&req, 0, sizeof(req));
        memset(&res, 0, sizeof(res));
        req.url = absolute;
        req.accept = accept;
        req.max_body = max;
        rc = http_fetch(p->runtime->http, &req, &res);
        if (rc != HTTP_OK) {
            snprintf(err, errsz, "%s", http_error_text(rc));
            return -1;
        }
        if (res.status < 200 || res.status >= 300) {
            snprintf(err, errsz, "HTTP %d for subresource", res.status);
            http_response_free(&res);
            return -1;
        }
        out->data = (char *)malloc(res.body_len + 1);
        out->url = str_dup(res.final_url ? res.final_url : absolute);
        if (!out->data || !out->url) {
            snprintf(err, errsz, "out of memory loading subresource");
            http_response_free(&res);
            resource_body_free(out);
            return -1;
        }
        if (res.body_len)
            memcpy(out->data, res.body, res.body_len);
        out->data[res.body_len] = 0;
        out->len = res.body_len;
        http_response_free(&res);
    }
    if (!out->url) {
        snprintf(err, errsz, "out of memory storing resource URL");
        resource_body_free(out);
        return -1;
    }
    if (out->len > RESOURCE_TOTAL_MAX - p->resource_bytes) {
        snprintf(err, errsz, "page subresource byte limit reached");
        resource_body_free(out);
        return -1;
    }
    p->resource_bytes += out->len;
    p->resources++;
    return 0;
}

static void choose_image_sources(struct browser_page *p)
{
    struct dom_node *n;

    for (n = p->doc->root; n; n = dom_next(n)) {
        const char *src, *candidate, *end;
        char selected[URL_MAX];
        unsigned long len;

        if (n->type != DOM_ELEMENT || n->tag_id != HTAG_IMG)
            continue;
        src = dom_get_attr(n, "src");
        if ((!src || !*src) && (candidate = dom_get_attr(n, "data-src")) &&
            *candidate) {
            dom_set_attr(n, "src", candidate);
            continue;
        }
        if (src && *src)
            continue;
        candidate = dom_get_attr(n, "srcset");
        if (!candidate || !*candidate)
            continue;
        while (*candidate == ' ' || *candidate == '\t') candidate++;
        end = candidate;
        while (*end && *end != ',' && *end != ' ' && *end != '\t') end++;
        len = (unsigned long)(end - candidate);
        if (len && len < sizeof(selected)) {
            memcpy(selected, candidate, len);
            selected[len] = 0;
            dom_set_attr(n, "src", selected);
        }
    }
}

static int b64_value(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int decode_data_image(const char *uri, unsigned char **out,
                             unsigned long *out_len)
{
    const char *comma, *p;
    unsigned long n, cap, used = 0;
    unsigned char *buf;
    int base64 = 0, bits = -8;
    unsigned val = 0;

    *out = 0;
    *out_len = 0;
    if (strncmp(uri, "data:image/", 11) != 0 ||
        !(comma = strchr(uri, ',')))
        return -1;
    for (p = uri; p < comma; p++)
        if ((unsigned long)(comma - p) >= 7 && p[0] == ';' &&
            ascii_lower((unsigned char)p[1]) == 'b' &&
            ascii_lower((unsigned char)p[2]) == 'a' &&
            ascii_lower((unsigned char)p[3]) == 's' &&
            ascii_lower((unsigned char)p[4]) == 'e' &&
            p[5] == '6' && p[6] == '4') {
            base64 = 1;
            break;
        }
    n = strlen(comma + 1);
    if (n > 4UL * 1024UL * 1024UL)
        return -1;
    cap = base64 ? (n / 4 + 1) * 3 : n + 1;
    buf = (unsigned char *)malloc(cap ? cap : 1);
    if (!buf)
        return -1;
    if (base64) {
        for (p = comma + 1; *p; p++) {
            int d;
            if (*p == '=') break;
            if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                continue;
            d = b64_value((unsigned char)*p);
            if (d < 0) { free(buf); return -1; }
            val = (val << 6) | (unsigned)d;
            bits += 6;
            if (bits >= 0) {
                if (used >= cap) { free(buf); return -1; }
                buf[used++] = (unsigned char)((val >> bits) & 0xff);
                bits -= 8;
            }
        }
    } else {
        for (p = comma + 1; *p; p++) {
            if (*p == '%' && p[1] && p[2]) {
                int a = p[1], b = p[2];
                a = a >= '0' && a <= '9' ? a - '0' :
                    a >= 'a' && a <= 'f' ? a - 'a' + 10 :
                    a >= 'A' && a <= 'F' ? a - 'A' + 10 : -1;
                b = b >= '0' && b <= '9' ? b - '0' :
                    b >= 'a' && b <= 'f' ? b - 'a' + 10 :
                    b >= 'A' && b <= 'F' ? b - 'A' + 10 : -1;
                if (a < 0 || b < 0) { free(buf); return -1; }
                buf[used++] = (unsigned char)((a << 4) | b);
                p += 2;
            } else {
                buf[used++] = (unsigned char)*p;
            }
        }
    }
    *out = buf;
    *out_len = used;
    return 0;
}

static struct browser_image *
page_image_load(struct browser_page *p, const char *ref)
{
    struct browser_image *it, *im;
    struct resource_body body;
    char absolute[URL_MAX], err[160];
    int rc;

    if (!ref || !*ref)
        return 0;
    if (!strncmp(ref, "data:image/", 11)) {
        unsigned char *encoded = 0;
        unsigned long encoded_len = 0;

        for (it = p->images; it; it = it->next)
            if (!strcmp(it->url, ref))
                return it;
        im = (struct browser_image *)calloc(1, sizeof(*im));
        if (!im)
            return 0;
        im->url = str_dup(ref);
        if (!im->url) { free(im); return 0; }
        im->next = p->images;
        p->images = im;
        if (p->resources >= RESOURCE_COUNT_MAX ||
            decode_data_image(ref, &encoded, &encoded_len) != 0 ||
            encoded_len > RESOURCE_MAX ||
            encoded_len > RESOURCE_TOTAL_MAX - p->resource_bytes ||
            img_decode(encoded, encoded_len, &im->decoded) != IMG_OK) {
            free(encoded);
            im->state = -1;
            p->resource_errors++;
            return im;
        }
        free(encoded);
        im->state = 1;
        p->resources++;
        p->resource_bytes += encoded_len;
        return im;
    }
    if (resource_url(p, p->base_url, ref, absolute, sizeof(absolute),
                     err, sizeof(err)) != 0)
        return 0;
    for (it = p->images; it; it = it->next)
        if (!strcmp(it->url, absolute))
            return it;
    im = (struct browser_image *)calloc(1, sizeof(*im));
    if (!im)
        return 0;
    im->url = str_dup(absolute);
    if (!im->url) {
        free(im);
        return 0;
    }
    im->next = p->images;
    p->images = im;
    if (resource_fetch(p, p->base_url, ref, RESOURCE_MAX,
                       "image/avif,image/webp,image/png,image/jpeg,"
                       "image/gif,image/bmp,*/*;q=0.5",
                       &body, err, sizeof(err)) != 0) {
        p->resource_errors++;
        im->state = -1;
        return im;
    }
    rc = img_decode(body.data, body.len, &im->decoded);
    resource_body_free(&body);
    if (rc != IMG_OK) {
        p->resource_errors++;
        im->state = -1;
        return im;
    }
    im->state = 1;
    return im;
}

static int page_image_size(void *ctx, const char *url, int *w, int *h)
{
    struct browser_image *im = page_image_load((struct browser_page *)ctx,
                                                url);
    if (!im || im->state != 1)
        return 0;
    *w = im->decoded.w;
    *h = im->decoded.h;
    return 1;
}

static const struct image *page_paint_image(void *ctx, const char *url)
{
    struct browser_image *im = page_image_load((struct browser_page *)ctx,
                                                url);
    return im && im->state == 1 ? &im->decoded : 0;
}

static int css_word_at(const char *s, unsigned long n, unsigned long at,
                       const char *word)
{
    unsigned long i, wn = strlen(word);

    if (at > n || wn > n - at)
        return 0;
    for (i = 0; i < wn; i++)
        if (ascii_lower((unsigned char)s[at + i]) !=
            ascii_lower((unsigned char)word[i]))
            return 0;
    return 1;
}

/* CSS url() values are relative to the stylesheet, not the document.  The
 * style engine intentionally stores only the URL token, so absolutize it
 * while the response URL is still available. */
static int css_add_rebased(struct browser_page *p, struct bytebuf *out,
                           const char *source, unsigned long len,
                           const char *source_url)
{
    unsigned long i = 0, literal = 0;

    while (i < len) {
        if (i + 1 < len && source[i] == '/' && source[i + 1] == '*') {
            i += 2;
            while (i + 1 < len &&
                   !(source[i] == '*' && source[i + 1] == '/'))
                i++;
            if (i + 1 < len) i += 2;
            continue;
        }
        if (source[i] == '\'' || source[i] == '"') {
            int quote = source[i++];
            while (i < len) {
                if (source[i] == '\\' && i + 1 < len) {
                    i += 2;
                    continue;
                }
                if (source[i++] == quote)
                    break;
            }
            continue;
        }
        if (css_word_at(source, len, i, "url")) {
            unsigned long p0 = i + 3, start, end, close;
            int quote = 0;
            char ref[URL_MAX], absolute[URL_MAX], error[96];

            while (p0 < len && (source[p0] == ' ' || source[p0] == '\t'))
                p0++;
            if (p0 >= len || source[p0] != '(') {
                i++;
                continue;
            }
            p0++;
            while (p0 < len && (source[p0] == ' ' || source[p0] == '\t' ||
                                source[p0] == '\r' || source[p0] == '\n'))
                p0++;
            if (p0 < len && (source[p0] == '\'' || source[p0] == '"'))
                quote = source[p0++];
            start = p0;
            if (quote) {
                while (p0 < len && source[p0] != quote) p0++;
                end = p0;
                if (p0 < len) p0++;
            } else {
                while (p0 < len && source[p0] != ')') p0++;
                end = p0;
                while (end > start &&
                       (source[end - 1] == ' ' || source[end - 1] == '\t' ||
                        source[end - 1] == '\r' || source[end - 1] == '\n'))
                    end--;
            }
            while (p0 < len && source[p0] != ')') p0++;
            if (p0 >= len) {
                i++;
                continue;
            }
            close = p0 + 1;
            if (end - start >= sizeof(ref)) {
                i = close;
                continue;
            }
            memcpy(ref, source + start, end - start);
            ref[end - start] = 0;
            if (strncmp(ref, "data:", 5) != 0 && ref[0] != '#' &&
                resource_url(p, source_url, ref, absolute, sizeof(absolute),
                             error, sizeof(error)) == 0) {
                if (buf_add(out, source + literal, i - literal) != 0 ||
                    buf_add(out, "url(\"", 5) != 0 ||
                    buf_add(out, absolute, strlen(absolute)) != 0 ||
                    buf_add(out, "\")", 2) != 0)
                    return -1;
                literal = close;
            }
            i = close;
            continue;
        }
        i++;
    }
    return buf_add(out, source + literal, len - literal);
}

static int collect_css_source(struct browser_page *p, struct bytebuf *css,
                              const char *source, unsigned long len,
                              const char *source_url, const char *media,
                              int depth)
{
    struct css_media env;
    struct css_stylesheet *probe;
    int i;

    if (depth > CSS_DEPTH_MAX) {
        p->resource_errors++;
        return 0;
    }
    memset(&env, 0, sizeof(env));
    env.width = WIN_W - SB_W;
    env.height = WIN_H - BAR_H - STAT_H;
    env.dpi = 96;
    env.screen = 1;
    probe = css_parse(source, len, CSS_ORIGIN_AUTHOR, &env);
    if (!probe)
        return -1;
    for (i = 0; i < css_import_count(probe); i++) {
        const char *import_url = css_import_url(probe, i);
        struct resource_body imported;
        char err[160];

        if (!import_url || !*import_url)
            continue;
        if (resource_fetch(p, source_url, import_url, CSS_RESOURCE_MAX,
                           "text/css,*/*;q=0.1", &imported,
                           err, sizeof(err)) == 0) {
            p->stylesheets++;
            if (collect_css_source(p, css, imported.data, imported.len,
                                   imported.url, 0, depth + 1) != 0) {
                resource_body_free(&imported);
                css_free(probe);
                return -1;
            }
            resource_body_free(&imported);
        } else {
            p->resource_errors++;
        }
    }
    css_free(probe);

    if (media && *media && !str_ci_eq(media, "all")) {
        if (buf_add(css, "@media ", 7) != 0 ||
            buf_add(css, media, strlen(media)) != 0 ||
            buf_add(css, "{\n", 2) != 0)
            return -1;
    }
    if (css_add_rebased(p, css, source, len, source_url) != 0 ||
        buf_add(css, "\n", 1) != 0)
        return -1;
    if (media && *media && !str_ci_eq(media, "all") &&
        buf_add(css, "}\n", 2) != 0)
        return -1;
    return 0;
}

static int collect_author_css(struct browser_page *p, struct bytebuf *css)
{
    struct dom_node *n;

    memset(css, 0, sizeof(*css));
    css->limit = CSS_TOTAL_MAX;
    for (n = p->doc->root; n; n = dom_next(n)) {
        if (n->type != DOM_ELEMENT)
            continue;
        if (n->tag_id == HTAG_STYLE) {
            char *text;
            unsigned long len;
            const char *media = dom_get_attr(n, "media");

            text = dom_text_content(n, &len);
            if (!text)
                return -1;
            if (collect_css_source(p, css, text, len, p->base_url,
                                   media, 0) != 0) {
                free(text);
                return -1;
            }
            free(text);
            p->stylesheets++;
        } else if (n->tag_id == HTAG_LINK &&
                   rel_has_token(dom_get_attr(n, "rel"), "stylesheet") &&
                   !rel_has_token(dom_get_attr(n, "rel"), "alternate") &&
                   !dom_has_attr(n, "disabled")) {
            const char *href = dom_get_attr(n, "href");
            const char *media = dom_get_attr(n, "media");
            struct resource_body sheet;
            char err[160];

            if (!href || !*href)
                continue;
            if (resource_fetch(p, p->base_url, href, CSS_RESOURCE_MAX,
                               "text/css,*/*;q=0.1", &sheet,
                               err, sizeof(err)) != 0) {
                p->resource_errors++;
                continue;
            }
            if (collect_css_source(p, css, sheet.data, sheet.len, sheet.url,
                                   media, 0) != 0) {
                resource_body_free(&sheet);
                return -1;
            }
            resource_body_free(&sheet);
            p->stylesheets++;
        }
    }
    if (!css->p) {
        css->p = str_dup("");
        css->cap = 1;
    }
    return css->p ? 0 : -1;
}

static void script_print(void *user, const char *text)
{
    struct browser_page *p = (struct browser_page *)user;
    (void)p;
    printf("[browser console] %s\n", text ? text : "");
}

static const char *script_cookie_get(void *user)
{
    struct browser_page *p = (struct browser_page *)user;
    struct cookie_jar *jar = http_client_jar(p->runtime->http);
    struct url u;
    long n;

    p->cookie_view[0] = 0;
    if (!jar || url_parse(p->url, &u) != URL_OK || !u.has_authority)
        return p->cookie_view;
    n = cookie_header(jar, u.host, u.path[0] ? u.path : "/",
                      str_ci_eq(u.scheme, "https"), 0,
                      COOKIE_CTX_SAME_SITE,
                      syscall(SYS_TIME, 0, 0, 0, 0),
                      p->cookie_view, sizeof(p->cookie_view));
    if (n < 0)
        p->cookie_view[0] = 0;
    return p->cookie_view;
}

static int script_cookie_set(void *user, const char *value)
{
    struct browser_page *p = (struct browser_page *)user;
    struct cookie_jar *jar = http_client_jar(p->runtime->http);
    struct url u;

    /* document.cookie cannot manufacture an HttpOnly cookie. */
    if (str_ci_contains(value, "httponly"))
        return 0;
    if (!jar || url_parse(p->url, &u) != URL_OK || !u.has_authority)
        return 0;
    return cookie_set(jar, value, u.host, u.path[0] ? u.path : "/",
                      str_ci_eq(u.scheme, "https"),
                      syscall(SYS_TIME, 0, 0, 0, 0));
}

static void script_fetch_result_free(struct jsdom_fetch_response *out)
{
    free(out->status_text);
    free(out->url);
    free(out->content_type);
    free(out->body);
    memset(out, 0, sizeof(*out));
}

static int script_fetch_method_allowed(const char *method)
{
    return !strcmp(method, "GET") || !strcmp(method, "HEAD") ||
           !strcmp(method, "POST") || !strcmp(method, "PUT") ||
           !strcmp(method, "PATCH") || !strcmp(method, "DELETE") ||
           !strcmp(method, "OPTIONS");
}

static int script_fetch_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

static int script_fetch_cors_allows(const struct http_response *res,
                                    const char *origin)
{
    const char *allow = http_header_get(res, "Access-Control-Allow-Origin");

    return allow && (!strcmp(allow, "*") || str_ci_eq(allow, origin));
}

static int script_fetch_simple_content_type(const char *value)
{
    static const char *const allowed[] = {
        "application/x-www-form-urlencoded",
        "multipart/form-data",
        "text/plain"
    };
    unsigned long i;

    if (!value || !*value)
        return 1;
    while (*value == ' ' || *value == '\t')
        value++;
    for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        unsigned long n = strlen(allowed[i]);
        unsigned long j;

        for (j = 0; j < n; j++)
            if (ascii_lower((unsigned char)value[j]) !=
                ascii_lower((unsigned char)allowed[i][j]))
                break;
        if (j == n &&
            (!value[n] || value[n] == ';' ||
             value[n] == ' ' || value[n] == '\t'))
            return 1;
    }
    return 0;
}

static int script_fetch(void *user, const char *url, const char *method,
                        const void *body, unsigned long body_len,
                        const char *request_content_type,
                        const char *request_accept,
                        const char *request_headers,
                        const char *request_mode,
                        const char *request_credentials,
                        const char *request_redirect,
                        struct jsdom_fetch_response *out,
                        char *err, unsigned long errsz)
{
    struct browser_page *p = (struct browser_page *)user;
    char absolute[URL_MAX];

    memset(out, 0, sizeof(*out));
    if (p->resources >= RESOURCE_COUNT_MAX) {
        snprintf(err, errsz, "page resource-count limit reached");
        return -1;
    }
    if (body_len > JS_FETCH_MAX ||
        resource_url(p, p->base_url, url, absolute, sizeof(absolute),
                     err, errsz) != 0)
        return -1;
    if (url_is_scheme(absolute, "file")) {
        if (strcmp(method, "GET") && strcmp(method, "HEAD")) {
            snprintf(err, errsz, "local fetch supports GET and HEAD only");
            return -1;
        }
        if (read_local_url(absolute, &out->body, &out->body_len,
                           err, errsz) != 0)
            return -1;
        if (!strcmp(method, "HEAD")) {
            free(out->body);
            out->body = str_dup("");
            out->body_len = 0;
        }
        out->status = 200;
        out->status_text = str_dup("OK");
        out->url = str_dup(absolute);
        out->content_type = str_dup("application/octet-stream");
    } else {
        struct url page_url;
        char page_origin[URL_MAX];
        char current[URL_MAX];
        const char *send_method = method;
        const void *send_body = body;
        unsigned long send_body_len = body_len;
        const char *content_type;
        int page_has_origin, redirects;

        if (!script_fetch_method_allowed(method)) {
            snprintf(err, errsz,
                     "fetch request method is not supported");
            return -1;
        }
        page_has_origin =
            url_parse(p->url, &page_url) == URL_OK &&
            page_url.has_authority &&
            (str_ci_eq(page_url.scheme, "http") ||
             str_ci_eq(page_url.scheme, "https")) &&
            url_origin(&page_url, page_origin, sizeof(page_origin)) == URL_OK;
        if (!page_has_origin)
            strcpy(page_origin, "null");
        strcpy(current, absolute);

        for (redirects = 0; ; redirects++) {
            struct http_request req;
            struct http_response res;
            struct url request_url;
            char origin_header[URL_MAX + 16];
            int cross_origin, rc;

            if (url_parse(current, &request_url) != URL_OK ||
                !request_url.has_authority) {
                snprintf(err, errsz, "invalid fetch URL");
                return -1;
            }
            cross_origin = !page_has_origin ||
                           !url_same_origin(&page_url, &request_url);
            if (cross_origin &&
                !strcmp(request_mode, "same-origin")) {
                snprintf(err, errsz,
                         "cross-origin request blocked by same-origin mode");
                return -1;
            }
            if (cross_origin && !strcmp(request_mode, "no-cors")) {
                snprintf(err, errsz,
                         "opaque no-cors responses are not implemented");
                return -1;
            }
            if (cross_origin &&
                ((request_headers && *request_headers) ||
                 !script_fetch_simple_content_type(
                     request_content_type))) {
                snprintf(err, errsz,
                         "cross-origin request requires CORS preflight");
                return -1;
            }
            memset(&req, 0, sizeof(req));
            memset(&res, 0, sizeof(res));
            req.method = send_method;
            req.url = current;
            req.body = send_body_len ? send_body : 0;
            req.body_len = send_body_len;
            req.content_type = send_body_len
                ? (request_content_type
                   ? request_content_type : "text/plain;charset=UTF-8")
                : 0;
            req.accept = request_accept ? request_accept : "*/*";
            req.extra_headers = request_headers;
            req.flags = HTTP_F_NO_REDIRECT;
            if (!strcmp(request_credentials, "omit") ||
                (cross_origin &&
                 !strcmp(request_credentials, "same-origin")))
                req.flags |= HTTP_F_NO_COOKIES;
            if (cross_origin) {
                snprintf(origin_header, sizeof(origin_header),
                         "Origin: %s\r\n", page_origin);
                req.extra_headers = origin_header;
            }
            req.max_body = JS_FETCH_MAX;
            rc = http_fetch(p->runtime->http, &req, &res);
            if (rc != HTTP_OK) {
                snprintf(err, errsz, "%s", http_error_text(rc));
                return -1;
            }
            if (cross_origin) {
                const char *allow_origin =
                    http_header_get(&res,
                                    "Access-Control-Allow-Origin");
                const char *allow_credentials =
                    http_header_get(&res,
                                    "Access-Control-Allow-Credentials");
                int cors_allowed =
                    script_fetch_cors_allows(&res, page_origin);

                if (!strcmp(request_credentials, "include"))
                    cors_allowed = allow_origin &&
                        !strcmp(allow_origin, page_origin) &&
                        allow_credentials &&
                        str_ci_eq(allow_credentials, "true");
                if (!cors_allowed) {
                    snprintf(err, errsz,
                             "cross-origin response did not allow %s",
                             page_origin);
                    http_response_free(&res);
                    return -1;
                }
            }
            if (script_fetch_redirect(res.status)) {
                const char *location = http_header_get(&res, "Location");
                char next[URL_MAX];

                if (!location || !*location) {
                    /* A redirect status without Location is an ordinary
                     * Response, as required by the Fetch redirect rules. */
                } else if (!strcmp(request_redirect, "error")) {
                    snprintf(err, errsz,
                             "redirect blocked by Request policy");
                    http_response_free(&res);
                    return -1;
                } else if (!strcmp(request_redirect, "manual")) {
                    /* Return the redirect response without following it.
                     * The current response surface does not yet expose the
                     * filtered Location header of an opaqueredirect. */
                } else if (redirects >= 8) {
                    snprintf(err, errsz, "too many fetch redirects");
                    http_response_free(&res);
                    return -1;
                } else if (resource_url(p, current, location, next,
                                        sizeof(next), err, errsz) != 0) {
                    http_response_free(&res);
                    return -1;
                } else {
                    if (res.status == 303 ||
                        ((res.status == 301 || res.status == 302) &&
                         !strcmp(send_method, "POST"))) {
                        send_method = "GET";
                        send_body = 0;
                        send_body_len = 0;
                    }
                    http_response_free(&res);
                    strcpy(current, next);
                    continue;
                }
            }
            if (res.body_len > RESOURCE_TOTAL_MAX - p->resource_bytes) {
                snprintf(err, errsz,
                         "page subresource byte limit reached");
                http_response_free(&res);
                return -1;
            }
            content_type = http_header_get(&res, "Content-Type");
            out->status = res.status;
            out->status_text = str_dup(res.reason ? res.reason :
                                       http_reason_phrase(res.status));
            out->url = str_dup(current);
            out->content_type = str_dup(content_type ? content_type : "");
            out->redirected = redirects > 0;
            out->body = (char *)malloc(res.body_len + 1);
            if (out->body) {
                if (res.body_len)
                    memcpy(out->body, res.body, res.body_len);
                out->body[res.body_len] = 0;
                out->body_len = res.body_len;
            }
            http_response_free(&res);
            break;
        }
    }
    if (!out->status_text || !out->url || !out->content_type ||
        !out->body) {
        snprintf(err, errsz, "out of memory creating fetch response");
        script_fetch_result_free(out);
        return -1;
    }
    if (out->body_len > RESOURCE_TOTAL_MAX - p->resource_bytes) {
        snprintf(err, errsz, "page subresource byte limit reached");
        script_fetch_result_free(out);
        return -1;
    }
    p->resource_bytes += out->body_len;
    p->resources++;
    return 0;
}

/* ------------------------------------------------------------------ *
 * Bounded ES module loader
 *
 * libjs remains a compact interpreter, so module syntax is lowered to an
 * isolated function scope.  Static one-line import/export declarations are
 * supported, dependencies are resolved relative to the importing module,
 * and every URL is evaluated once.  Bindings are snapshots rather than the
 * specification's live bindings; cycles are rejected with a clear error.
 * ------------------------------------------------------------------ */

#define MODULE_MAX 64
#define MODULE_DEPTH_MAX 16

struct browser_module {
    char *url;
    const char *meta_url;
    char *base_url;
    char *source;
    unsigned long source_len;
    unsigned int index;
    int state;                     /* 0 new, 1 visiting, 2 done, -1 failed */
    struct browser_module *next;
};

struct module_loader {
    struct browser_page *page;
    struct browser_module *head;
    unsigned int count;
};

static int module_ident_start(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '_' || c == '$';
}

static int module_ident_part(int c)
{
    return module_ident_start(c) || (c >= '0' && c <= '9');
}

static const char *module_skip(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r'))
        p++;
    return p;
}

static const char *module_trim_end(const char *p, const char *end)
{
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == ';'))
        end--;
    return end;
}

static const char *module_statement_end(const char *p, const char *end)
{
    int quote = 0;

    while (p < end) {
        if (quote) {
            if (*p == '\\' && p + 1 < end) {
                p += 2;
                continue;
            }
            if (*p == quote)
                quote = 0;
        } else if (*p == '\'' || *p == '"') {
            quote = *p;
        } else if (*p == ';') {
            return p;
        }
        p++;
    }
    return end;
}

static int module_word(const char *p, const char *end, const char *word)
{
    unsigned long n = strlen(word);

    return (unsigned long)(end - p) >= n &&
           !memcmp(p, word, n) &&
           (p == end || p + n == end ||
            !module_ident_part((unsigned char)p[n]));
}

static int module_read_ident(const char **pp, const char *end,
                             char *out, unsigned long outsz)
{
    const char *p = module_skip(*pp, end);
    unsigned long n = 0;

    if (p >= end || !module_ident_start((unsigned char)*p))
        return -1;
    while (p < end && module_ident_part((unsigned char)*p)) {
        if (n + 1 >= outsz)
            return -1;
        out[n++] = *p++;
    }
    out[n] = 0;
    *pp = p;
    return 0;
}

static const char *module_find_word(const char *p, const char *end,
                                    const char *word)
{
    const char *start = p;
    unsigned long n = strlen(word);

    while (p + n <= end) {
        int left = p == start ||
            !module_ident_part((unsigned char)p[-1]);
        int right = p + n == end ||
            !module_ident_part((unsigned char)p[n]);

        if (left && right && !memcmp(p, word, n))
            return p;
        p++;
    }
    return 0;
}

static struct browser_module *module_find(struct module_loader *l,
                                           const char *url)
{
    struct browser_module *m;

    for (m = l->head; m; m = m->next)
        if (!strcmp(m->url, url))
            return m;
    return 0;
}

static struct browser_module *module_add(struct module_loader *l,
                                          char *url, char *base_url,
                                          char *source,
                                          unsigned long source_len)
{
    struct browser_module *m;

    if (l->count >= MODULE_MAX)
        return 0;
    m = (struct browser_module *)calloc(1, sizeof(*m));
    if (!m)
        return 0;
    m->url = url;
    m->meta_url = url;
    m->base_url = base_url;
    m->source = source;
    m->source_len = source_len;
    m->index = l->count++;
    m->next = l->head;
    l->head = m;
    return m;
}

static struct browser_module *
module_external(struct module_loader *l, const char *base, const char *ref,
                char *err, unsigned long errsz)
{
    char absolute[URL_MAX];
    struct browser_module *m;
    struct resource_body body;
    struct jsdom_fetch_response fetched;
    const char *content_type;
    int mime_ok = 0;
    unsigned long i;
    static const char *const js_mimes[] = {
        "text/javascript",
        "application/javascript",
        "text/ecmascript",
        "application/ecmascript"
    };

    if (resource_url(l->page, base, ref, absolute, sizeof(absolute),
                     err, errsz) != 0)
        return 0;
    m = module_find(l, absolute);
    if (m)
        return m;
    memset(&fetched, 0, sizeof(fetched));
    if (script_fetch(l->page, absolute, "GET", 0, 0, 0, 0, 0,
                     "cors", "same-origin", "follow", &fetched,
                     err, errsz) != 0)
        return 0;
    if (fetched.status < 200 || fetched.status >= 300) {
        snprintf(err, errsz, "HTTP %d loading module", fetched.status);
        script_fetch_result_free(&fetched);
        return 0;
    }
    if (fetched.body_len > SCRIPT_RESOURCE_MAX) {
        snprintf(err, errsz, "module is larger than %lu bytes",
                 SCRIPT_RESOURCE_MAX);
        script_fetch_result_free(&fetched);
        return 0;
    }
    content_type = fetched.content_type;
    while (content_type &&
           (*content_type == ' ' || *content_type == '\t'))
        content_type++;
    if (url_is_scheme(fetched.url, "file")) {
        mime_ok = 1;
    } else {
        for (i = 0; i < sizeof(js_mimes) / sizeof(js_mimes[0]); i++) {
            unsigned long n = strlen(js_mimes[i]), j;

            for (j = 0; content_type && j < n; j++)
                if (ascii_lower((unsigned char)content_type[j]) !=
                    ascii_lower((unsigned char)js_mimes[i][j]))
                    break;
            if (content_type && j == n &&
                (!content_type[n] || content_type[n] == ';' ||
                 content_type[n] == ' ' || content_type[n] == '\t')) {
                mime_ok = 1;
                break;
            }
        }
    }
    if (!mime_ok) {
        snprintf(err, errsz, "module response has a non-JavaScript MIME type");
        script_fetch_result_free(&fetched);
        return 0;
    }
    memset(&body, 0, sizeof(body));
    body.data = fetched.body;
    body.len = fetched.body_len;
    body.url = fetched.url;
    fetched.body = 0;
    fetched.url = 0;
    script_fetch_result_free(&fetched);
    m = module_find(l, body.url);
    if (m) {
        resource_body_free(&body);
        return m;
    }
    {
        char *module_url = body.url;
        char *module_source = body.data;
        char *module_base;

        body.url = 0;
        body.data = 0;
        module_base = str_dup(module_url);
        m = module_base
            ? module_add(l, module_url, module_base,
                         module_source, body.len)
            : 0;
        if (!m) {
            free(module_url);
            free(module_base);
            free(module_source);
            snprintf(err, errsz, "module-count limit or out of memory");
        }
    }
    resource_body_free(&body);
    return m;
}

static struct browser_module *
module_inline(struct module_loader *l, const char *source, unsigned long len,
              unsigned int serial, char *err, unsigned long errsz)
{
    char name[URL_MAX];
    char *url, *base, *copy;

    snprintf(name, sizeof(name), "%s#inline-module-%u",
             l->page->url, serial);
    url = str_dup(name);
    base = str_dup(l->page->base_url);
    copy = (char *)malloc(len + 1);
    if (copy) {
        if (len) memcpy(copy, source, len);
        copy[len] = 0;
    }
    if (!url || !base || !copy) {
        free(url); free(base); free(copy);
        snprintf(err, errsz, "out of memory storing inline module");
        return 0;
    }
    {
        struct browser_module *m = module_add(l, url, base, copy, len);
        if (!m) {
            free(url); free(base); free(copy);
            snprintf(err, errsz, "module-count limit reached");
        } else {
            m->meta_url = l->page->url;
        }
        return m;
    }
}

static int module_eval(struct module_loader *l, struct browser_module *m,
                       int depth, char *err, unsigned long errsz);

static int module_emit_binding(struct bytebuf *out, const char *local,
                               unsigned int dep, const char *exported)
{
    char line[512];

    snprintf(line, sizeof(line),
             "var %s=window.__kmod_%u[\"%s\"];\n",
             local, dep, exported);
    return buf_add(out, line, strlen(line));
}

static int module_emit_namespace(struct bytebuf *out, const char *local,
                                 unsigned int dep)
{
    char line[256];

    snprintf(line, sizeof(line), "var %s=window.__kmod_%u;\n", local, dep);
    return buf_add(out, line, strlen(line));
}

static int module_emit_export(struct bytebuf *out, const char *local,
                              const char *exported)
{
    char line[512];

    snprintf(line, sizeof(line), "__exports[\"%s\"]=%s;\n",
             exported, local);
    return buf_add(out, line, strlen(line));
}

static int module_specifier(const char *p, const char *end,
                            char *out, unsigned long outsz,
                            const char **quote_at)
{
    const char *q, *close;
    int quote;
    unsigned long n;

    for (q = p; q < end && *q != '\'' && *q != '"'; q++)
        ;
    if (q >= end)
        return -1;
    quote = *q;
    for (close = q + 1; close < end && *close != quote; close++) {
        if (*close == '\\')
            return -1;                  /* escaped specs are not lowered */
    }
    if (close >= end)
        return -1;
    n = (unsigned long)(close - q - 1);
    if (!n || n + 1 > outsz)
        return -1;
    memcpy(out, q + 1, n);
    out[n] = 0;
    if (quote_at) *quote_at = q;
    return 0;
}

static int module_named_imports(struct bytebuf *out, const char *p,
                                const char *end, unsigned int dep,
                                char *err, unsigned long errsz)
{
    p = module_skip(p, end);
    if (p >= end || *p != '{')
        return -1;
    p++;
    while (p < end) {
        char imported[128], local[128];

        p = module_skip(p, end);
        if (p < end && *p == '}')
            return 0;
        if (module_read_ident(&p, end, imported, sizeof(imported)) != 0)
            break;
        snprintf(local, sizeof(local), "%s", imported);
        p = module_skip(p, end);
        if (module_word(p, end, "as")) {
            p += 2;
            if (module_read_ident(&p, end, local, sizeof(local)) != 0)
                break;
        }
        if (module_emit_binding(out, local, dep, imported) != 0)
            return -1;
        p = module_skip(p, end);
        if (p < end && *p == ',') {
            p++;
            continue;
        }
        if (p < end && *p == '}')
            return 0;
        break;
    }
    snprintf(err, errsz, "unsupported named import syntax");
    return -1;
}

static int module_import_line(struct module_loader *l,
                              struct browser_module *owner,
                              const char *p, const char *end,
                              struct bytebuf *out, int depth,
                              char *err, unsigned long errsz)
{
    char spec[URL_MAX], local[128];
    const char *quote_at, *from, *clause, *clause_end, *q;
    struct browser_module *dep;

    if (module_specifier(p, end, spec, sizeof(spec), &quote_at) != 0) {
        snprintf(err, errsz, "module import lacks a simple string specifier");
        return -1;
    }
    dep = module_external(l, owner->base_url, spec, err, errsz);
    if (!dep || module_eval(l, dep, depth + 1, err, errsz) != 0)
        return -1;
    clause = module_skip(p + 6, quote_at);
    from = module_find_word(clause, quote_at, "from");
    if (!from)
        return 0;                       /* side-effect import */
    clause_end = module_trim_end(clause, from);
    q = module_skip(clause, clause_end);
    if (q >= clause_end)
        return 0;
    if (*q != '{' && *q != '*') {
        if (module_read_ident(&q, clause_end, local, sizeof(local)) != 0 ||
            module_emit_binding(out, local, dep->index, "default") != 0)
            return -1;
        q = module_skip(q, clause_end);
        if (q < clause_end && *q == ',')
            q = module_skip(q + 1, clause_end);
        else
            return q == clause_end ? 0 : -1;
    }
    if (q < clause_end && *q == '*') {
        q = module_skip(q + 1, clause_end);
        if (!module_word(q, clause_end, "as"))
            return -1;
        q += 2;
        if (module_read_ident(&q, clause_end, local, sizeof(local)) != 0)
            return -1;
        return module_emit_namespace(out, local, dep->index);
    }
    return module_named_imports(out, q, clause_end, dep->index,
                                err, errsz);
}

static int module_export_list(struct module_loader *l,
                              struct browser_module *owner,
                              const char *p, const char *end,
                              struct bytebuf *out, int depth,
                              char *err, unsigned long errsz)
{
    const char *close, *after;
    struct browser_module *dep = 0;

    for (close = p; close < end && *close != '}'; close++)
        ;
    if (close >= end)
        return -1;
    after = module_skip(close + 1, end);
    if (module_word(after, end, "from")) {
        char spec[URL_MAX];

        if (module_specifier(after + 4, end, spec, sizeof(spec), 0) != 0)
            return -1;
        dep = module_external(l, owner->base_url, spec, err, errsz);
        if (!dep || module_eval(l, dep, depth + 1, err, errsz) != 0)
            return -1;
    }
    p++;
    while (p < close) {
        char local[128], exported[128];

        p = module_skip(p, close);
        if (p >= close)
            break;
        if (module_read_ident(&p, close, local, sizeof(local)) != 0)
            return -1;
        snprintf(exported, sizeof(exported), "%s", local);
        p = module_skip(p, close);
        if (module_word(p, close, "as")) {
            p += 2;
            if (module_read_ident(&p, close, exported, sizeof(exported)) != 0)
                return -1;
        }
        if (dep) {
            char line[512];
            snprintf(line, sizeof(line),
                     "__exports[\"%s\"]=window.__kmod_%u[\"%s\"];\n",
                     exported, dep->index, local);
            if (buf_add(out, line, strlen(line)) != 0)
                return -1;
        } else if (module_emit_export(out, local, exported) != 0) {
            return -1;
        }
        p = module_skip(p, close);
        if (p < close && *p == ',') p++;
    }
    return 0;
}

static int module_emit_string(struct bytebuf *out, const char *s)
{
    const char *literal = s;

    if (buf_add(out, "\"", 1) != 0)
        return -1;
    while (*s) {
        if (*s == '"' || *s == '\\') {
            if (buf_add(out, literal, (unsigned long)(s - literal)) != 0 ||
                buf_add(out, "\\", 1) != 0 ||
                buf_add(out, s, 1) != 0)
                return -1;
            s++;
            literal = s;
            continue;
        }
        s++;
    }
    return buf_add(out, literal, (unsigned long)(s - literal)) != 0 ||
           buf_add(out, "\"", 1) != 0 ? -1 : 0;
}

static int module_import_left(const char *line, const char *p)
{
    while (p > line &&
           (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\r'))
        p--;
    return p == line ||
           (!module_ident_part((unsigned char)p[-1]) && p[-1] != '.');
}

/* Lower literal dynamic imports eagerly and preserve their asynchronous
 * observable result with Promise.resolve(namespace). This is intentionally
 * narrower than the specification: computed specifiers remain an error, and
 * dependencies still live inside the same bounded graph. */
static int module_dynamic_line(struct module_loader *l,
                               struct browser_module *owner,
                               const char *line, const char *end,
                               struct bytebuf *out, int depth,
                               int *block_comment,
                               char *err, unsigned long errsz)
{
    const char *p = line, *literal = line;
    int quote = 0;

    while (p < end) {
        if (*block_comment) {
            const char *close = p;

            while (close + 1 < end &&
                   !(close[0] == '*' && close[1] == '/'))
                close++;
            if (close + 1 >= end) {
                p = end;
                break;
            }
            *block_comment = 0;
            p = close + 2;
            continue;
        }
        if (quote) {
            if (*p == '\\' && p + 1 < end) {
                p += 2;
                continue;
            }
            if (*p == quote)
                quote = 0;
            p++;
            continue;
        }
        if (*p == '\'' || *p == '"') {
            quote = *p++;
            continue;
        }
        if (*p == '/' && p + 1 < end && p[1] == '/')
            break;
        if (*p == '/' && p + 1 < end && p[1] == '*') {
            const char *close = p + 2;

            while (close + 1 < end &&
                   !(close[0] == '*' && close[1] == '/'))
                close++;
            if (close + 1 >= end) {
                *block_comment = 1;
                p = end;
                break;
            }
            p = close + 2;
            continue;
        }
        if (module_import_left(line, p) &&
            module_word(p, end, "import")) {
            const char *q = module_skip(p + 6, end);

            if (q + 9 <= end && !memcmp(q, ".meta.url", 9) &&
                (q + 9 == end ||
                 !module_ident_part((unsigned char)q[9]))) {
                if (buf_add(out, literal, (unsigned long)(p - literal)) != 0 ||
                    module_emit_string(out, owner->meta_url) != 0)
                    return -2;
                p = q + 9;
                literal = p;
                continue;
            }
            if (q < end && *q == '(') {
                const char *spec_at, *close, *after;
                char spec[URL_MAX];
                unsigned long n;
                struct browser_module *dep;
                char replacement[128];
                int delimiter;

                q = module_skip(q + 1, end);
                if (q >= end || (*q != '\'' && *q != '"')) {
                    snprintf(err, errsz,
                             "dynamic import requires a literal specifier");
                    return -1;
                }
                delimiter = *q;
                spec_at = q + 1;
                close = spec_at;
                while (close < end && *close != delimiter) {
                    if (*close == '\\') {
                        snprintf(err, errsz,
                                 "escaped dynamic import specifier unsupported");
                        return -1;
                    }
                    close++;
                }
                if (close >= end) {
                    snprintf(err, errsz, "unterminated dynamic import");
                    return -1;
                }
                n = (unsigned long)(close - spec_at);
                if (!n || n >= sizeof(spec)) {
                    snprintf(err, errsz, "dynamic import specifier is invalid");
                    return -1;
                }
                memcpy(spec, spec_at, n);
                spec[n] = 0;
                after = module_skip(close + 1, end);
                if (after >= end || *after != ')') {
                    snprintf(err, errsz,
                             "dynamic import has unsupported arguments");
                    return -1;
                }
                dep = module_external(l, owner->base_url, spec, err, errsz);
                if (!dep ||
                    module_eval(l, dep, depth + 1, err, errsz) != 0)
                    return -1;
                snprintf(replacement, sizeof(replacement),
                         "Promise.resolve(window.__kmod_%u)", dep->index);
                if (buf_add(out, literal, (unsigned long)(p - literal)) != 0 ||
                    buf_add(out, replacement, strlen(replacement)) != 0)
                    return -2;
                p = after + 1;
                literal = p;
                continue;
            }
        }
        p++;
    }
    return buf_add(out, literal, (unsigned long)(end - literal)) != 0
        ? -2 : 0;
}

static int module_transform(struct module_loader *l,
                            struct browser_module *m, int depth,
                            struct bytebuf *out,
                            char *err, unsigned long errsz)
{
    struct bytebuf body, trailer;
    unsigned long pos = 0;
    char prefix[128], suffix[128];
    int block_comment = 0;

    memset(&body, 0, sizeof(body));
    memset(&trailer, 0, sizeof(trailer));
    body.limit = SCRIPT_RESOURCE_MAX * 2;
    trailer.limit = SCRIPT_RESOURCE_MAX;
    while (pos < m->source_len) {
        unsigned long start = pos, stop;
        const char *line, *end, *p;
        int started_in_comment;

        while (pos < m->source_len && m->source[pos] != '\n')
            pos++;
        stop = pos;
        if (pos < m->source_len) pos++;
        line = m->source + start;
        end = m->source + stop;
        p = module_skip(line, end);
        started_in_comment = block_comment;
        if (!started_in_comment && module_word(p, end, "import") &&
            module_skip(p + 6, end) < end &&
            *module_skip(p + 6, end) != '(' &&
            *module_skip(p + 6, end) != '.') {
            const char *statement = module_statement_end(p, end);

            if (module_import_line(l, m, p, statement, &body, depth,
                                   err, errsz) != 0)
                goto fail;
            if (statement < end) {
                int dynamic = module_dynamic_line(
                    l, m, statement + 1, end, &body, depth,
                    &block_comment, err, errsz);

                if (dynamic == -1)
                    goto fail;
                if (dynamic == -2)
                    goto oom;
            }
            if (buf_add(&body, "\n", 1) != 0)
                goto oom;
            continue;
        }
        if (!started_in_comment && module_word(p, end, "export")) {
            const char *q = module_skip(p + 6, end);

            if (module_word(q, end, "default")) {
                const char *value = module_skip(q + 7, end);
                if (module_word(value, end, "function")) {
                    const char *name_at = module_skip(value + 8, end);
                    char name[128];

                    if (name_at < end &&
                        module_ident_start((unsigned char)*name_at)) {
                        const char *scan = name_at;
                        int dynamic;

                        if (module_read_ident(&scan, end, name,
                                              sizeof(name)) != 0)
                            goto fail;
                        dynamic = module_dynamic_line(
                            l, m, value, end, &body, depth, &block_comment,
                            err, errsz);
                        if (dynamic == -1)
                            goto fail;
                        if (dynamic == -2 ||
                            buf_add(&body, "\n", 1) != 0 ||
                            module_emit_export(&trailer, name, "default") != 0)
                            goto oom;
                    } else {
                        int dynamic;

                        if (buf_add(&body, "__exports[\"default\"]=", 21) != 0)
                            goto oom;
                        dynamic = module_dynamic_line(
                            l, m, value, end, &body, depth, &block_comment,
                            err, errsz);
                        if (dynamic == -1)
                            goto fail;
                        if (dynamic == -2 ||
                            buf_add(&body, "\n", 1) != 0)
                            goto oom;
                    }
                } else {
                    int dynamic;

                    if (buf_add(&body, "__exports[\"default\"]=", 21) != 0)
                        goto oom;
                    dynamic = module_dynamic_line(
                        l, m, value, end, &body, depth, &block_comment,
                        err, errsz);
                    if (dynamic == -1)
                        goto fail;
                    if (dynamic == -2 ||
                        buf_add(&body, "\n", 1) != 0)
                        goto oom;
                }
                continue;
            }
            if (q < end && *q == '{') {
                if (module_export_list(l, m, q, end, &body, depth,
                                       err, errsz) != 0)
                    goto fail;
                if (buf_add(&body, "\n", 1) != 0)
                    goto oom;
                continue;
            }
            if (module_word(q, end, "var") ||
                module_word(q, end, "let") ||
                module_word(q, end, "const") ||
                module_word(q, end, "function")) {
                const char *name_at;
                char name[128];
                unsigned long keyword = module_word(q, end, "function")
                    ? 8 : (module_word(q, end, "const") ? 5 : 3);
                int dynamic;

                name_at = q + keyword;
                if (module_read_ident(&name_at, end, name,
                                      sizeof(name)) != 0) {
                    snprintf(err, errsz, "unsupported export declaration");
                    goto fail;
                }
                dynamic = module_dynamic_line(
                    l, m, q, end, &body, depth, &block_comment,
                    err, errsz);
                if (dynamic == -1)
                    goto fail;
                if (dynamic == -2 ||
                    buf_add(&body, "\n", 1) != 0 ||
                    module_emit_export(&trailer, name, name) != 0)
                    goto oom;
                continue;
            }
            snprintf(err, errsz, "unsupported export syntax in %s", m->url);
            goto fail;
        }
        {
            int dynamic = module_dynamic_line(l, m, line, end, &body, depth,
                                              &block_comment,
                                              err, errsz);
            if (dynamic == -1)
                goto fail;
            if (dynamic == -2 ||
                (pos > stop && buf_add(&body, "\n", 1) != 0))
                goto oom;
        }
    }
    out->limit = SCRIPT_RESOURCE_MAX * 3;
    snprintf(prefix, sizeof(prefix), "(function(__exports){\n");
    snprintf(suffix, sizeof(suffix),
             "\n})(window.__kmod_%u={});\n", m->index);
    if (buf_add(out, prefix, strlen(prefix)) != 0 ||
        buf_add(out, body.p ? body.p : "", body.n) != 0 ||
        buf_add(out, trailer.p ? trailer.p : "", trailer.n) != 0 ||
        buf_add(out, suffix, strlen(suffix)) != 0)
        goto oom;
    free(body.p);
    free(trailer.p);
    return 0;

oom:
    snprintf(err, errsz, "module transform exceeded its memory limit");
fail:
    free(body.p);
    free(trailer.p);
    return -1;
}

static int module_eval(struct module_loader *l, struct browser_module *m,
                       int depth, char *err, unsigned long errsz)
{
    struct bytebuf transformed;

    if (m->state == 2)
        return 0;
    if (m->state == 1) {
        snprintf(err, errsz, "circular module dependency at %s", m->url);
        return -1;
    }
    if (m->state < 0) {
        snprintf(err, errsz, "module previously failed: %s", m->url);
        return -1;
    }
    if (depth > MODULE_DEPTH_MAX) {
        snprintf(err, errsz, "module dependency depth limit reached");
        return -1;
    }
    m->state = 1;
    memset(&transformed, 0, sizeof(transformed));
    if (module_transform(l, m, depth, &transformed, err, errsz) != 0) {
        m->state = -1;
        return -1;
    }
    l->page->scripts++;
    if (jsdom_eval(l->page->js, transformed.p, m->url,
                   err, errsz) != 0) {
        free(transformed.p);
        m->state = -1;
        return -1;
    }
    free(transformed.p);
    m->state = 2;
    return 0;
}

static void module_loader_free(struct module_loader *l)
{
    struct browser_module *m = l->head;

    while (m) {
        struct browser_module *next = m->next;
        free(m->url);
        free(m->base_url);
        free(m->source);
        free(m);
        m = next;
    }
    memset(l, 0, sizeof(*l));
}

static int script_is_module(const char *type)
{
    return type && str_ci_eq(type, "module");
}

static int script_type_supported(const char *type)
{
    if (!type || !*type)
        return 1;
    if (script_is_module(type))
        return 0;
    return str_ci_contains(type, "javascript") ||
           str_ci_contains(type, "ecmascript");
}

static int page_sync_document_url(struct browser_page *p)
{
    const char *current;
    char *url_copy, *base_copy = 0;
    int base_follows;

    if (!p || !p->js)
        return 0;
    current = jsdom_document_url(p->js);
    if (!current || !strcmp(current, p->url))
        return 0;
    base_follows = p->base_url && !strcmp(p->base_url, p->url);
    url_copy = str_dup(current);
    if (base_follows)
        base_copy = str_dup(current);
    if (!url_copy || (base_follows && !base_copy)) {
        free(base_copy);
        free(url_copy);
        return -1;
    }
    free(p->url);
    p->url = url_copy;
    if (base_follows) {
        free(p->base_url);
        p->base_url = base_copy;
    }
    return 1;
}

static int execute_page_scripts(struct browser_page *p,
                                int viewport_width, int viewport_height)
{
    struct jsdom_config cfg;
    struct k_cpuinfo cpu;
    struct dom_node *n;
    struct module_loader modules;
    unsigned int inline_module = 0;

    memset(&cfg, 0, sizeof(cfg));
    memset(&cpu, 0, sizeof(cpu));
    cfg.url = p->url;
    cfg.base_url = p->base_url;
    cfg.print = script_print;
    cfg.cookie_get = script_cookie_get;
    cfg.cookie_set = script_cookie_set;
    cfg.fetch = script_fetch;
    cfg.local_storage = p->runtime->local_storage;
    cfg.session_storage = p->runtime->session_storage;
    cfg.user = p;
    cfg.max_heap = 16UL * 1024UL * 1024UL;
    cfg.max_steps = 4000000UL;
    cfg.viewport_width = viewport_width > 0
        ? (unsigned int)viewport_width : 1;
    cfg.viewport_height = viewport_height > 0
        ? (unsigned int)viewport_height : 1;
    if (cpuinfo(&cpu) == 0 && cpu.online)
        cfg.hardware_concurrency = cpu.online;
    p->js = jsdom_new(p->doc, &cfg);
    if (!p->js)
        return -1;
    memset(&modules, 0, sizeof(modules));
    modules.page = p;

    /* Classic scripts run in document order. Modules are deferred until the
     * complete document has been visited, matching browser scheduling. */
    for (n = p->doc->root; n; n = dom_next(n)) {
        const char *type, *src;
        char *code = 0;
        const char *name = "inline script";
        struct resource_body external;
        unsigned long len = 0;
        char err[256];

        err[0] = 0;
        if (n->type != DOM_ELEMENT || n->tag_id != HTAG_SCRIPT)
            continue;
        type = dom_get_attr(n, "type");
        if (!script_type_supported(type))
            continue;
        src = dom_get_attr(n, "src");
        memset(&external, 0, sizeof(external));
        if (src && *src) {
            if (resource_fetch(p, p->base_url, src, SCRIPT_RESOURCE_MAX,
                               "text/javascript,application/javascript,"
                               "*/*;q=0.1", &external,
                               err, sizeof(err)) != 0) {
                p->resource_errors++;
                p->script_errors++;
                continue;
            }
            code = external.data;
            len = external.len;
            name = external.url;
        } else {
            code = dom_text_content(n, &len);
            if (!code) {
                p->script_errors++;
                continue;
            }
        }
        if (len && jsdom_eval(p->js, code, name, err, sizeof(err)) != 0) {
            printf("[browser script error] %s: %s\n", name, err);
            p->script_errors++;
        }
        p->scripts++;
        if (src && *src)
            resource_body_free(&external);
        else
            free(code);
    }
    for (n = p->doc->root; n; n = dom_next(n)) {
        const char *type, *src;
        struct browser_module *m = 0;
        char err[256];

        err[0] = 0;
        if (n->type != DOM_ELEMENT || n->tag_id != HTAG_SCRIPT)
            continue;
        type = dom_get_attr(n, "type");
        if (!script_is_module(type))
            continue;
        src = dom_get_attr(n, "src");
        if (src && *src) {
            m = module_external(&modules, p->base_url, src,
                                err, sizeof(err));
            if (!m)
                p->resource_errors++;
        } else {
            unsigned long len = 0;
            char *code = dom_text_content(n, &len);

            if (code) {
                m = module_inline(&modules, code, len, inline_module++,
                                  err, sizeof(err));
                free(code);
            } else {
                snprintf(err, sizeof(err),
                         "out of memory reading inline module");
            }
        }
        if (!m || module_eval(&modules, m, 0, err, sizeof(err)) != 0) {
            if (!err[0])
                snprintf(err, sizeof(err), "unsupported module syntax");
            printf("[browser module error] %s: %s\n",
                   src && *src ? src : "inline module", err);
            p->script_errors++;
        }
    }
    module_loader_free(&modules);
    {
        char err[256];
        int ran;

        jsdom_dispatch_document(p->js, "DOMContentLoaded",
                                err, sizeof(err));
        jsdom_dispatch_document(p->js, "load", err, sizeof(err));
        ran = jsdom_pump(p->js, err, sizeof(err));
        if (ran < 0) {
            printf("[browser timer error] %s\n", err);
            p->script_errors++;
        }
    }
    if (page_sync_document_url(p) < 0)
        return -1;
    return 0;
}

static int establish_page_base(struct browser_page *p)
{
    struct dom_node *n;
    char resolved[URL_MAX];
    char err[80];

    p->base_url = str_dup(p->url);
    if (!p->base_url)
        return -1;
    for (n = p->doc->root; n; n = dom_next(n)) {
        const char *href;

        if (n->type != DOM_ELEMENT || n->tag_id != HTAG_BASE)
            continue;
        href = dom_get_attr(n, "href");
        if (!href || !*href)
            continue;
        if (resource_url(p, p->url, href, resolved, sizeof(resolved),
                         err, sizeof(err)) == 0) {
            char *copy = str_dup(resolved);

            if (!copy)
                return -1;
            free(p->base_url);
            p->base_url = copy;
        }
        break;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * Page construction and lifetime
 * ------------------------------------------------------------------ */

static void page_content_destroy(struct browser_page *p)
{
    struct browser_image *im, *next;

    if (!p)
        return;
    lay_free(p->layout);
    p->layout = 0;
    if (p->doc)
        css_style_dom_free(p->doc->root);
    style_engine_free(p->styles);
    p->styles = 0;
    css_free(p->author);
    p->author = 0;
    jsdom_free(p->js);
    p->js = 0;
    for (im = p->images; im; im = next) {
        next = im->next;
        img_free(&im->decoded);
        free(im->url);
        free(im);
    }
    p->images = 0;
    dom_document_free(p->doc);
    p->doc = 0;
    free(p->base_url);
    p->base_url = 0;
}

static void page_destroy(struct browser_page *p)
{
    if (!p)
        return;
    page_content_destroy(p);
    free(p->url);
    free(p);
}

static BROWSER_NOINLINE int
page_reflow(struct browser_runtime *rt, struct browser_page *p,
            int viewport_w, int viewport_h, char *err, unsigned long errsz)
{
    struct css_media media;
    struct css_stylesheet *sheets[2];
    struct lay_opts lo;
    const struct lay_paint_item *paint_items;
    enum browser_load_state entry_state;
    int styled;

    entry_state = p->load_state;
    if (page_transition(p, PAGE_STYLING) != 0) {
        snprintf(err, errsz, "invalid page lifecycle transition to styling");
        return -1;
    }
    lay_free(p->layout);
    p->layout = 0;
    if (p->doc)
        css_style_dom_free(p->doc->root);
    style_engine_free(p->styles);
    p->styles = 0;

    memset(&media, 0, sizeof(media));
    media.width = viewport_w;
    media.height = viewport_h;
    media.dpi = 96;
    media.screen = 1;
    css_set_media(rt->ua, &media);
    css_set_media(p->author, &media);
    sheets[0] = rt->ua;
    sheets[1] = p->author;
    p->styles = style_engine_new(sheets, 2, css_dom_ops());
    if (!p->styles) {
        page_transition(p, PAGE_FAILED);
        snprintf(err, errsz, "out of memory creating the style engine");
        return -1;
    }
    style_engine_set_viewport(p->styles, viewport_w, viewport_h);
    styled = style_compute_tree(p->styles, p->doc->root, 0,
                                css_style_dom_sink, 0);
    if (styled <= 0) {
        page_transition(p, PAGE_FAILED);
        snprintf(err, errsz, "out of memory computing page styles");
        return -1;
    }

    if (page_transition(p, PAGE_LAYOUT) != 0) {
        snprintf(err, errsz, "invalid page lifecycle transition to layout");
        return -1;
    }
    lay_opts_init(&lo, viewport_w, viewport_h);
    lo.image_size = page_image_size;
    lo.image_ctx = p;
    p->layout = lay_layout(p->doc, &lo);
    if (!p->layout) {
        page_transition(p, PAGE_FAILED);
        snprintf(err, errsz, "out of memory starting page layout");
        return -1;
    }
    /* The current layout API reports paint-order allocation failure as an
     * empty order.  An actually empty document has only its ICB; anything
     * more substantial must have at least one background/content item. */
    if (lay_box_count(p->layout) > 1 &&
        lay_paint_order(p->layout, &paint_items) <= 0) {
        page_transition(p, PAGE_FAILED);
        snprintf(err, errsz, "out of memory building page paint order");
        return -1;
    }
    if (entry_state == PAGE_COMPLETE &&
        page_transition(p, PAGE_COMPLETE) != 0) {
        snprintf(err, errsz, "invalid page lifecycle transition after reflow");
        return -1;
    }
    return 0;
}

static BROWSER_NOINLINE int
build_pipeline(struct browser_runtime *rt, struct browser_page *p,
               const char *src, unsigned long len, int viewport_w,
               int viewport_h, char *err, unsigned long errsz)
{
    struct css_media media;
    struct bytebuf css;

    p->runtime = rt;
    if (page_transition(p, PAGE_PARSING) != 0) {
        snprintf(err, errsz, "invalid page lifecycle transition to parsing");
        return -1;
    }
    p->doc = html_parse_document(src, len);
    if (!p->doc) {
        snprintf(err, errsz, "out of memory parsing %lu bytes", len);
        return -1;
    }
    if (p->doc->oom) {
        snprintf(err, errsz, "out of memory while building the DOM");
        return -1;
    }
    if (establish_page_base(p) != 0) {
        snprintf(err, errsz, "out of memory establishing the document base");
        return -1;
    }
    choose_image_sources(p);
    if (collect_author_css(p, &css) != 0) {
        snprintf(err, errsz, "out of memory collecting page stylesheets");
        return -1;
    }
    memset(&media, 0, sizeof(media));
    media.width = viewport_w;
    media.height = viewport_h;
    media.dpi = 96;
    media.screen = 1;
    p->author = css_parse(css.p, css.n, CSS_ORIGIN_AUTHOR, &media);
    free(css.p);
    if (!p->author) {
        snprintf(err, errsz, "out of memory parsing page styles");
        return -1;
    }
    if (page_reflow(rt, p, viewport_w, viewport_h, err, errsz) != 0)
        return -1;
    if (page_transition(p, PAGE_SCRIPTING) != 0) {
        snprintf(err, errsz, "invalid page lifecycle transition to scripting");
        return -1;
    }
    if (execute_page_scripts(p, viewport_w, viewport_h) != 0) {
        page_transition(p, PAGE_FAILED);
        snprintf(err, errsz, "out of memory creating the JavaScript runtime");
        return -1;
    }
    if (jsdom_dirty(p->js) &&
        page_reflow(rt, p, viewport_w, viewport_h, err, errsz) != 0)
        return -1;
    jsdom_clear_dirty(p->js);
    if (page_transition(p, PAGE_COMPLETE) != 0) {
        snprintf(err, errsz, "invalid page lifecycle transition to complete");
        return -1;
    }
    return 0;
}

static BROWSER_NOINLINE int
build_error_page(struct browser_runtime *rt, struct browser_page *p,
                 int viewport_w, int viewport_h, const char *message,
                 char *err, unsigned long errsz)
{
    char *source;
    unsigned long source_len;

    page_content_destroy(p);
    snprintf(p->error, sizeof(p->error), "%s", message);
    p->failed = 1;
    p->is_html = 1;
    source = make_error_source(p->url ? p->url : "(unknown address)",
                               message, &source_len);
    if (!source) {
        snprintf(err, errsz, "out of memory creating an error page");
        return -1;
    }
    if (build_pipeline(rt, p, source, source_len, viewport_w, viewport_h,
                       err, errsz) != 0) {
        free(source);
        return -1;
    }
    free(source);
    return 0;
}

struct page_load_work {
    struct http_response response;
    struct http_request request;
    struct k_netinfo net;
    const char *body;
    char *local_body;
    char *owned_source;
    unsigned long body_len;
    unsigned long source_len;
    int response_live;
    int is_html;
    char primary_error[320];
};

static void page_load_work_destroy(struct page_load_work *w)
{
    if (!w)
        return;
    if (w->response_live)
        http_response_free(&w->response);
    free(w->owned_source);
    free(w->local_body);
    free(w);
}

static BROWSER_NOINLINE struct browser_page *
page_load_request(struct browser_runtime *rt, const char *target,
                  const char *method, const void *request_body,
                  unsigned long request_len, const char *content_type,
                  int viewport_w, int viewport_h,
                  char *fatal, unsigned long fatalsz)
{
    struct browser_page *p;
    struct page_load_work *w;
    int rc;

    p = (struct browser_page *)calloc(1, sizeof(*p));
    if (!p) {
        snprintf(fatal, fatalsz, "out of memory creating a page");
        return 0;
    }
    w = (struct page_load_work *)calloc(1, sizeof(*w));
    if (!w) {
        snprintf(fatal, fatalsz, "out of memory creating load state");
        page_destroy(p);
        return 0;
    }
    p->url = str_dup(target);
    if (!p->url) {
        snprintf(fatal, fatalsz, "out of memory copying the address");
        page_load_work_destroy(w);
        page_destroy(p);
        return 0;
    }

    if (page_transition(p, PAGE_NAVIGATING) != 0) {
        snprintf(fatal, fatalsz, "invalid initial navigation state");
        page_load_work_destroy(w);
        page_destroy(p);
        return 0;
    }
    if (!url_supported(target)) {
        snprintf(w->primary_error, sizeof(w->primary_error),
                 "unsupported URL scheme (use file, http, or https)");
    } else if (page_transition(p, PAGE_FETCHING) != 0) {
        snprintf(w->primary_error, sizeof(w->primary_error),
                 "invalid page lifecycle transition to fetching");
    } else if (url_is_scheme(target, "file")) {
        p->is_local = 1;
        if (read_local_url(target, &w->local_body, &w->body_len,
                           w->primary_error,
                           sizeof(w->primary_error)) == 0) {
            w->body = w->local_body;
            w->is_html = sniff_html(w->body, w->body_len);
        }
    } else if (netinfo(&w->net) != 0 || !w->net.up) {
        snprintf(w->primary_error, sizeof(w->primary_error),
                 "network unavailable: no NIC is configured");
    } else {
        w->request.url = target;
        w->request.method = method;
        w->request.body = request_body;
        w->request.body_len = request_len;
        w->request.content_type = content_type;
        w->request.max_body = FETCH_MAX;
        rc = http_fetch(rt->http, &w->request, &w->response);
        if (rc != HTTP_OK) {
            const char *tls_detail =
                url_is_scheme(target, "https") &&
                (rc == HTTP_E_CONNECT || rc == HTTP_E_DNS)
                    ? tls_last_transport_error() : 0;

            if (tls_detail && *tls_detail &&
                strcmp(tls_detail, "no error") != 0)
                snprintf(w->primary_error, sizeof(w->primary_error),
                         "%s: %s", http_error_text(rc), tls_detail);
            else
                snprintf(w->primary_error, sizeof(w->primary_error), "%s",
                         http_error_text(rc));
        } else {
            const char *ct;
            char *final_copy;

            w->response_live = 1;
            final_copy = w->response.final_url
                ? str_dup(w->response.final_url) : 0;
            if (!w->response.final_url) {
                snprintf(w->primary_error, sizeof(w->primary_error),
                         "HTTP client returned no final URL");
            } else if (!final_copy) {
                snprintf(w->primary_error, sizeof(w->primary_error),
                         "out of memory copying the final URL");
            } else if (!w->response.body && w->response.body_len) {
                free(final_copy);
                snprintf(w->primary_error, sizeof(w->primary_error),
                         "HTTP client returned a missing response body");
            } else {
                free(p->url);
                p->url = final_copy;
                p->status = w->response.status;
                p->bytes = w->response.body_len;
                w->body = w->response.body ? w->response.body : "";
                w->body_len = w->response.body_len;
                ct = http_header_get(&w->response, "Content-Type");
                w->is_html =
                    (ct && (str_ci_contains(ct, "text/html") ||
                            str_ci_contains(ct, "xhtml"))) ||
                    sniff_html(w->body, w->body_len);
                if (p->status < 200 || p->status >= 300) {
                    p->failed = 1;
                    snprintf(p->error, sizeof(p->error), "HTTP %d %s",
                             p->status,
                             w->response.reason ? w->response.reason : "");
                }
            }
        }
    }

    if (w->primary_error[0]) {
        page_transition(p, PAGE_FAILED);
        if (build_error_page(rt, p, viewport_w, viewport_h,
                             w->primary_error, fatal, fatalsz) != 0) {
            page_load_work_destroy(w);
            page_destroy(p);
            return 0;
        }
        page_load_work_destroy(w);
        return p;
    }

    if (page_transition(p, PAGE_RESPONSE) != 0) {
        snprintf(w->primary_error, sizeof(w->primary_error),
                 "invalid page lifecycle transition after response");
    }
    p->bytes = w->body_len;
    p->is_html = w->is_html;
    if (!w->is_html) {
        w->owned_source =
            make_plain_source(w->body, w->body_len, p->status,
                              &w->source_len);
        if (!w->owned_source)
            snprintf(w->primary_error, sizeof(w->primary_error),
                     "out of memory preparing plain text");
    } else if (p->status && (p->status < 200 || p->status >= 300)) {
        w->owned_source =
            make_status_source(w->body, w->body_len, p->status,
                               &w->source_len);
        if (!w->owned_source)
            snprintf(w->primary_error, sizeof(w->primary_error),
                     "out of memory preparing the HTTP error body");
    } else {
        w->source_len = w->body_len;
    }

    if (!w->primary_error[0] &&
        build_pipeline(rt, p,
                       w->owned_source ? w->owned_source : w->body,
                       w->source_len, viewport_w, viewport_h,
                       w->primary_error,
                       sizeof(w->primary_error)) != 0) {
        page_transition(p, PAGE_FAILED);
        page_content_destroy(p);
    }

    if (w->primary_error[0]) {
        page_transition(p, PAGE_FAILED);
    }
    if (w->primary_error[0] &&
        build_error_page(rt, p, viewport_w, viewport_h, w->primary_error,
                         fatal, fatalsz) != 0) {
        page_load_work_destroy(w);
        page_destroy(p);
        return 0;
    }
    page_load_work_destroy(w);
    return p;
}

static struct browser_page *
page_load(struct browser_runtime *rt, const char *target, int viewport_w,
          int viewport_h, char *fatal, unsigned long fatalsz)
{
    return page_load_request(rt, target, 0, 0, 0, 0, viewport_w, viewport_h,
                             fatal, fatalsz);
}

/* ------------------------------------------------------------------ *
 * Text mode: project the actual layout to terminal rows
 * ------------------------------------------------------------------ */

static int box_line_baseline(const struct lay_box *b)
{
    const struct lay_box *p = b;
    int guard = 0;

    while (p && guard++ < LAY_MAX_DEPTH) {
        if (p->kind == LAY_BOX_LINE)
            return clamp_i64((int64_t)p->y + p->baseline);
        p = p->parent;
    }
    return clamp_i64((int64_t)b->y + b->baseline);
}

static const struct lay_box *next_projected_box(const struct lay_box *b)
{
    const struct lay_box *n = b;

    for (;;) {
        if (n->first_child) {
            n = n->first_child;
        } else {
            while (n && !n->next)
                n = n->parent;
            if (!n)
                return 0;
            n = n->next;
        }
        if (!(n->flags & LAYF_HIDDEN) &&
            (n->kind == LAY_BOX_TEXT || n->kind == LAY_BOX_MARKER ||
             n->kind == LAY_BOX_REPLACED))
            return n;
    }
}

static void text_row_put(char *row, int cols, int *used, int col,
                         const char *text, unsigned long len)
{
    unsigned long i;

    if (col < 0) {
        unsigned long skip = (unsigned long)(-(int64_t)col);

        if (skip >= len)
            return;
        text += skip;
        len -= skip;
        col = 0;
    }
    if (col >= cols)
        return;
    if (len > (unsigned long)(cols - col))
        len = (unsigned long)(cols - col);
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];

        if (c >= 32 && c < 127)
            row[col + (int)i] = (char)c;
    }
    if (col + (int)len > *used)
        *used = col + (int)len;
}

static void text_row_flush(char *row, int *used)
{
    while (*used > 0 && row[*used - 1] == ' ')
        (*used)--;
    row[*used] = 0;
    printf("%s\n", row);
}

static int render_layout_text(const struct lay_document *layout, int cols,
                              int viewport_w)
{
    struct lay_box *root, *b, *child;
    struct lay_box **stack;
    unsigned long cap, sp = 0;
    char row[MAX_COLS + 1];
    int line = -1, used = 0;

    root = lay_root(layout);
    cap = lay_box_count(layout);
    if (!root || !cap || cap > ~0UL / sizeof(*stack) - 1)
        return -1;
    stack = (struct lay_box **)malloc((cap + 1) * sizeof(*stack));
    if (!stack)
        return -1;
    stack[sp++] = root;
    memset(row, ' ', (unsigned long)cols);

    while (sp) {
        b = stack[--sp];
        /* Reverse sibling push makes the first child the next pop.  That
         * preserves final tree/document order even when the first child is
         * positioned and paint order would move it to another stack. */
        for (child = b->last_child; child; child = child->prev) {
            if (sp >= cap) {
                free(stack);
                return -1;
            }
            stack[sp++] = child;
        }
        if ((b->flags & LAYF_HIDDEN) ||
            (b->kind != LAY_BOX_TEXT && b->kind != LAY_BOX_MARKER &&
             b->kind != LAY_BOX_REPLACED))
            continue;
        {
        int baseline = box_line_baseline(b);
        int col = viewport_w > 0
            ? mul_div_i((int64_t)b->x, cols, viewport_w) : 0;
        const char *s;
        unsigned long n;

        if (line != baseline) {
            if (line >= 0) {
                text_row_flush(row, &used);
                if ((int64_t)baseline > (int64_t)line + 30)
                    putchar('\n');
            }
            memset(row, ' ', (unsigned long)cols);
            used = 0;
            line = baseline;
        }
        if (b->kind == LAY_BOX_REPLACED) {
            const char *alt = b->node ? dom_get_attr(b->node, "alt") : 0;
            unsigned long an = alt ? strlen(alt) : 0;
            unsigned long shown = an;
            int projected = viewport_w > 0
                ? mul_div_i((int64_t)b->w, cols, viewport_w) : 0;
            int64_t available;
            int start = clamp_i64((int64_t)col + 1);

            /* Never let an alt label overwrite the following inline box.
             * Tiny decoded icons have no printable terminal cell.  A
             * missing image, however, was sized in pixels from its alt
             * text; use the real gap to the next projected run so the
             * artificial square brackets do not clip its last character. */
            if (b->intrinsic_w <= 0 && b->intrinsic_h <= 0) {
                const struct lay_box *next = next_projected_box(b);

                projected = cols - col;
                if (next && box_line_baseline(next) == baseline) {
                    int next_col = viewport_w > 0
                        ? mul_div_i((int64_t)next->x, cols, viewport_w) : 0;

                    if (next_col > col && next_col - col < projected)
                        projected = next_col - col;
                }
            } else if (projected < 3) {
                continue;
            }
            available = (int64_t)projected - 2;
            if (available > (int64_t)cols - (int64_t)col - 2)
                available = (int64_t)cols - (int64_t)col - 2;
            text_row_put(row, cols, &used, col, "[", 1);
            if (available <= 0)
                shown = 0;
            else if (shown > (unsigned long)available)
                shown = (unsigned long)available;
            if (shown)
                text_row_put(row, cols, &used, start, alt, shown);
            text_row_put(row, cols, &used,
                         clamp_i64((int64_t)start + (int64_t)shown),
                         "]", 1);
            continue;
        } else if (b->kind == LAY_BOX_MARKER) {
            s = b->marker ? b->marker : "*";
            n = strlen(s);
        } else {
            s = b->text;
            n = b->text_len;
        }
        if (s && n)
            text_row_put(row, cols, &used, col, s, n);
        }
    }
    if (line >= 0)
        text_row_flush(row, &used);
    free(stack);
    return 0;
}

static unsigned long link_hash(const char *s)
{
    unsigned long h = 2166136261u;

    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

/* Keep the historical text-mode contract for local links: resolution still
 * happens as an absolute file: URL, but shell users see a path they can pass
 * directly to cat/edit/browser. */
static const char *link_display(const char *absolute, char *out,
                                unsigned long outsz)
{
    struct url u;
    unsigned long used;
    int n;

    if (url_parse(absolute, &u) != URL_OK || !u.has_scheme ||
        !str_ci_eq(u.scheme, "file") ||
        url_pct_decode(u.path, ~0UL, 0, out, outsz) < 0)
        return absolute;
    used = strlen(out);
    if (u.has_query) {
        n = snprintf(out + used, outsz - used, "?%s", u.query);
        if (n < 0 || (unsigned long)n >= outsz - used)
            return absolute;
        used += (unsigned long)n;
    }
    if (u.has_fragment) {
        n = snprintf(out + used, outsz - used, "#%s", u.fragment);
        if (n < 0 || (unsigned long)n >= outsz - used)
            return absolute;
    }
    return out;
}

static void render_links(const struct browser_page *p)
{
    char **seen;
    struct dom_node *n;
    int count = 0, full = 0;

    seen = (char **)calloc(LINK_MAX, sizeof(*seen));
    if (!seen) {
        printf("\nLinks: [out of memory listing links]\n");
        return;
    }
    for (n = p->doc->root; n; n = dom_next(n)) {
        const char *href;
        char absolute[URL_MAX];
        char display[URL_MAX];
        const char *shown;
        unsigned long slot, start;

        if (n->type != DOM_ELEMENT || n->tag_id != HTAG_A)
            continue;
        href = dom_get_attr(n, "href");
        if (!href || !*href)
            continue;
        if (url_resolve_str(p->url, href, absolute, sizeof(absolute)) !=
            URL_OK)
            continue;

        slot = link_hash(absolute) % LINK_MAX;
        start = slot;
        while (seen[slot] && strcmp(seen[slot], absolute) != 0) {
            slot = (slot + 1) % LINK_MAX;
            if (slot == start) {
                full = 1;
                break;
            }
        }
        if (full)
            break;
        if (seen[slot])
            continue;
        seen[slot] = str_dup(absolute);
        if (!seen[slot]) {
            full = 1;
            break;
        }
        if (count == 0)
            printf("\nLinks:\n");
        shown = link_display(absolute, display, sizeof(display));
        printf("  [%d] %s\n", ++count, shown);
    }
    if (full)
        printf("  [link list stopped at %d distinct targets]\n", count);
    for (count = 0; count < LINK_MAX; count++)
        free(seen[count]);
    free(seen);
}

/* Presentation runs only after page_load() has returned, so its printf and
 * projection spills cannot contribute to the deep layout call chain. */
static BROWSER_NOINLINE int
text_present(struct browser_page *p, int cols, int viewport_w,
             int show_links, int verbose)
{
    int rc;

    rc = p->failed ? 1 : 0;
    if (p->failed)
        printf("browser: %s\n\n", p->error);
    if (verbose) {
        printf("url: %s\n", p->url);
        if (p->status)
            printf("status: %d\n", p->status);
        printf("bytes: %lu  type: %s\n", p->bytes,
               p->is_html ? "html" : "text");
        printf("subresources: %u  resource-bytes: %lu  "
               "resource-errors: %u\n",
               p->resources, p->resource_bytes, p->resource_errors);
        printf("stylesheets: %u  scripts: %u  script-errors: %u\n",
               p->stylesheets, p->scripts, p->script_errors);
        printf("lifecycle: %s\n", page_state_name(p->load_state));
        if (p->doc->title && p->doc->title[0])
            printf("title: %s\n", p->doc->title);
        printf("\n");
    }
    if (render_layout_text(p->layout, cols, viewport_w) != 0) {
        printf("browser: out of memory projecting the page as text\n");
        rc = 1;
    }
    if (p->doc->truncated)
        printf("\n[DOM truncated: 0x%x]\n", p->doc->truncated);
    if (lay_truncated(p->layout))
        printf("\n[layout truncated: 0x%x]\n", lay_truncated(p->layout));
    if (show_links)
        render_links(p);
    page_destroy(p);
    return rc;
}

static BROWSER_NOINLINE int
text_mode(struct browser_runtime *rt, const char *url, int cols,
          int show_links, int verbose)
{
    struct browser_page *p;
    char *fatal;
    int viewport_w = cols * GLYPH_W;

    fatal = (char *)malloc(320);
    if (!fatal) {
        printf("browser: out of memory creating load diagnostics\n");
        return 1;
    }
    p = page_load(rt, url, viewport_w, 768, fatal, 320);
    if (!p) {
        printf("browser: %s\n", fatal);
        free(fatal);
        return 1;
    }
    free(fatal);
    return text_present(p, cols, viewport_w, show_links, verbose);
}

/* ------------------------------------------------------------------ *
 * Graphical browser
 * ------------------------------------------------------------------ */

struct bstate {
    struct browser_runtime *rt;
    gui_window *win;
    struct browser_page *page;

    char *hist[HIST_MAX];
    int hist_n, hist_i;

    char addr[URL_MAX];
    int addr_len, addr_cur, addr_focus;

    int scroll;
    int view_w, view_h;
    char status[320];
    char load_error[320];
    char page_info[192];
    int status_err;
    int sb_drag;
    struct dom_node *focus_node;
    int focus_cursor;
    int quit;
};

static int ui_text_w(const char *s)
{
    return gui_text_w(s);
}

static void ui_text(gui_window *w, int x, int y, const char *s, unsigned c,
                    int bold)
{
    gui_text(w, x, y, s, c, GUI_TRANSPARENT);
    if (bold)
        gui_text(w, x + 1, y, s, c, GUI_TRANSPARENT);
}

static void ui_box(gui_window *w, int x, int y, int cw, int ch, unsigned fill,
                   unsigned edge)
{
    gui_rect(w, x, y, cw, ch, fill);
    gui_frame(w, x, y, cw, ch, edge);
}

static void ui_button(gui_window *w, int x, int y, int cw, int ch,
                      const char *label, int enabled, int down)
{
    int tw = ui_text_w(label);

    ui_box(w, x, y, cw, ch, down ? C_BTNDN : C_BTN, C_FRAME);
    ui_text(w, x + (cw - tw) / 2, y + (ch - GLYPH_H) / 2, label,
            enabled ? C_TEXT : C_DIM, 0);
}

static void set_status(struct bstate *b, int error, const char *head,
                       const char *extra)
{
    snprintf(b->status, sizeof(b->status), "%s%s%s", head,
             extra && *extra ? "  -  " : "", extra ? extra : "");
    b->status_err = error;
}

static int content_h(const struct bstate *b)
{
    return b->page && b->page->layout ? lay_height(b->page->layout) : 0;
}

static void clamp_scroll(struct bstate *b)
{
    int max = content_h(b) - b->view_h;

    if (max < 0)
        max = 0;
    if (b->scroll > max)
        b->scroll = max;
    if (b->scroll < 0)
        b->scroll = 0;
}

static BROWSER_NOINLINE void draw(struct bstate *b)
{
    int bx, cw;

    gui_clear(b->win, C_BG);

    if (b->page && b->page->layout) {
        struct paint_target target;
        struct paint_opts opts;

        target.px = b->win->px;
        target.w = b->win->w;
        target.h = b->win->h;
        target.stride = b->win->w;
        paint_opts_init(&opts);
        opts.origin_x = 0;
        opts.origin_y = BAR_H;
        opts.scroll_x = 0;
        opts.scroll_y = b->scroll;
        opts.canvas = lay_canvas_color(b->page->layout);
        opts.image = page_paint_image;
        opts.image_ctx = b->page;
        if (b->focus_node)
            opts.highlight_box =
                lay_box_for_node(b->page->layout, b->focus_node);
        lay_paint(b->page->layout, &target,
                  lay_mkrect(0, BAR_H, b->view_w, b->view_h), &opts, 0);
    }

    /* Toolbar. */
    gui_rect(b->win, 0, 0, b->win->w, BAR_H, C_CHROME);
    ui_button(b->win, PAD, 4, 46, BAR_H - 8, "Back", b->hist_i > 0, 0);
    ui_button(b->win, PAD + 50, 4, 62, BAR_H - 8, "Fwd",
              b->hist_i + 1 < b->hist_n, 0);
    ui_button(b->win, PAD + 116, 4, 62, BAR_H - 8, "Reload",
              b->page != 0, 0);

    bx = PAD + 184;
    cw = b->win->w - bx - PAD;
    if (cw < 40)
        cw = 40;
    ui_box(b->win, bx, 4, cw, BAR_H - 8, C_FIELD,
           b->addr_focus ? C_LINK : C_FRAME);
    {
        int maxch = (cw - 8) / GLYPH_W;
        int from = b->addr_cur > maxch ? b->addr_cur - maxch : 0;
        char tmp[URL_MAX];

        snprintf(tmp, sizeof(tmp), "%s", b->addr + from);
        if (maxch > 0 && (int)strlen(tmp) > maxch)
            tmp[maxch] = 0;
        ui_text(b->win, bx + 4, 4 + (BAR_H - 8 - GLYPH_H) / 2,
                tmp, C_TEXT, 0);
        if (b->addr_focus) {
            int cx = bx + 4 + (b->addr_cur - from) * GLYPH_W;

            gui_rect(b->win, cx, 8, 1, BAR_H - 16, C_CARET);
        }
    }

    /* Scrollbar. */
    {
        int sx = b->view_w;
        int ch = content_h(b);
        int th, ty;

        gui_rect(b->win, sx, BAR_H, SB_W, b->view_h, C_CHROME);
        gui_frame(b->win, sx, BAR_H, SB_W, b->view_h, C_FRAME);
        if (ch > b->view_h) {
            th = mul_div_i((int64_t)b->view_h, b->view_h, ch);
            if (th < 18)
                th = 18;
            ty = clamp_i64((int64_t)BAR_H +
                 mul_div_i((int64_t)b->view_h - th, b->scroll,
                           (int64_t)ch - b->view_h));
            ui_box(b->win, sx + 1, ty, SB_W - 2, th, C_BTN, C_FRAME);
        }
    }

    /* Status line. */
    {
        int sy = b->win->h - STAT_H;
        int maxch = (b->win->w - 2 * PAD) / GLYPH_W;
        char tmp[320];

        gui_rect(b->win, 0, sy, b->win->w, STAT_H, C_STATBG);
        gui_line(b->win, 0, sy, b->win->w - 1, sy, C_FRAME);
        snprintf(tmp, sizeof(tmp), "%s", b->status);
        if (maxch > 0 && (int)strlen(tmp) > maxch)
            tmp[maxch] = 0;
        ui_text(b->win, PAD, sy + 2, tmp,
                b->status_err ? 0x00A00000u : C_TEXT, 0);
    }
    gui_flush(b->win);
}

static void history_discard_from(struct bstate *b, int first)
{
    int i;

    for (i = first; i < b->hist_n; i++) {
        free(b->hist[i]);
        b->hist[i] = 0;
    }
    b->hist_n = first;
}

/* Commit an already allocated URL.  Nothing below this point can fail, so
 * page/history replacement can be one transaction. */
static BROWSER_NOINLINE void
history_commit(struct bstate *b, char *copy, int push)
{
    if (!push && b->hist_i >= 0 && b->hist_i < b->hist_n) {
        free(b->hist[b->hist_i]);
        b->hist[b->hist_i] = copy;
        return;
    }

    if (b->hist_n > 0)
        history_discard_from(b, b->hist_i + 1);
    if (b->hist_n == HIST_MAX) {
        free(b->hist[0]);
        memmove(&b->hist[0], &b->hist[1],
                (HIST_MAX - 1) * sizeof(b->hist[0]));
        b->hist_n--;
        b->hist_i--;
    }
    b->hist[b->hist_n] = copy;
    b->hist_i = b->hist_n;
    b->hist_n++;
}

static BROWSER_NOINLINE void set_fragment_scroll(struct bstate *b)
{
    struct url u;
    char id[URL_FRAG_MAX];
    int32_t y;

    b->scroll = 0;
    if (!b->page || url_parse(b->page->url, &u) != URL_OK ||
        !u.has_fragment || !u.fragment[0])
        return;
    if (url_pct_decode(u.fragment, ~0UL, 0, id, sizeof(id)) < 0)
        return;
    if (lay_scroll_to_id(b->page->layout, id, &y))
        b->scroll = y;
    clamp_scroll(b);
}

static BROWSER_NOINLINE int
load_gui_request(struct bstate *b, const char *url, int push,
                 const char *method, const void *body, unsigned long body_len,
                 const char *content_type)
{
    struct browser_page *next, *old;
    char *history_copy;
    int script_hops = 0;

    set_status(b, 0, "loading", url);
    draw(b);
    next = page_load_request(b->rt, url, method, body, body_len, content_type,
                             b->view_w, b->view_h,
                             b->load_error, sizeof(b->load_error));
    if (!next) {
        set_status(b, 1, "cannot replace this page:", b->load_error);
        return -1;
    }
    while (next->js && jsdom_pending_navigation(next->js) &&
           script_hops++ < 8) {
        char *target = str_dup(jsdom_pending_navigation(next->js));
        struct browser_page *redirected;

        if (!target || !url_supported(target)) {
            free(target);
            break;
        }
        redirected = page_load(b->rt, target, b->view_w, b->view_h,
                               b->load_error, sizeof(b->load_error));
        free(target);
        if (!redirected)
            break;
        page_destroy(next);
        next = redirected;
    }
    if (!push && (b->hist_i < 0 || b->hist_i >= b->hist_n)) {
        page_destroy(next);
        set_status(b, 1, "cannot replace invalid history entry", "");
        return -1;
    }
    history_copy = str_dup(next->url);
    if (!history_copy) {
        page_destroy(next);
        set_status(b, 1, "out of memory updating history", "");
        return -1;
    }

    old = b->page;
    b->page = next;
    b->focus_node = 0;
    b->focus_cursor = 0;
    history_commit(b, history_copy, push);
    snprintf(b->addr, sizeof(b->addr), "%s", next->url);
    b->addr_len = (int)strlen(b->addr);
    b->addr_cur = b->addr_len;
    set_fragment_scroll(b);

    if (next->failed) {
        set_status(b, 1, "error:", next->error);
    } else {
        snprintf(b->page_info, sizeof(b->page_info),
                 "%s%lu bytes, %u resources, %u CSS, %u scripts%s%s%s%s",
                 next->is_local ? "local file, " : "",
                 next->bytes,
                 next->resources, next->stylesheets, next->scripts,
                 next->resource_errors ? ", resource errors" : "",
                 next->script_errors ? ", script errors" : "",
                 next->doc->truncated ? ", DOM truncated" : "",
                 lay_truncated(next->layout) ? ", layout truncated" : "");
        set_status(b, 0,
                   next->doc->title && next->doc->title[0]
                       ? next->doc->title : next->url,
                   b->page_info);
    }
    page_destroy(old);
    return 0;
}

static int load_gui(struct bstate *b, const char *url, int push)
{
    return load_gui_request(b, url, push, 0, 0, 0, 0);
}

static BROWSER_NOINLINE void
navigate_relative(struct bstate *b, const char *href)
{
    char *full;
    int rc;

    full = (char *)malloc(URL_MAX);
    if (!full) {
        set_status(b, 1, "out of memory resolving link", "");
        return;
    }
    rc = url_resolve_str(b->page->url, href, full, URL_MAX);
    if (rc != URL_OK) {
        set_status(b, 1, "cannot resolve link:", url_strerror(rc));
        free(full);
        return;
    }
    if (!url_supported(full)) {
        set_status(b, 1, "cannot follow unsupported link:", full);
        free(full);
        return;
    }
    load_gui(b, full, 1);
    free(full);
}

static BROWSER_NOINLINE void go_back(struct bstate *b)
{
    char *target;
    int previous;

    if (b->hist_i <= 0) {
        set_status(b, 1, "no page to go back to", "");
        return;
    }
    target = str_dup(b->hist[b->hist_i - 1]);
    if (!target) {
        set_status(b, 1, "out of memory opening history", "");
        return;
    }
    previous = b->hist_i;
    b->hist_i--;
    if (load_gui(b, target, 0) != 0)
        b->hist_i = previous;
    free(target);
}

static BROWSER_NOINLINE void go_fwd(struct bstate *b)
{
    char *target;
    int previous;

    if (b->hist_i + 1 >= b->hist_n) {
        set_status(b, 1, "no page to go forward to", "");
        return;
    }
    target = str_dup(b->hist[b->hist_i + 1]);
    if (!target) {
        set_status(b, 1, "out of memory opening history", "");
        return;
    }
    previous = b->hist_i;
    b->hist_i++;
    if (load_gui(b, target, 0) != 0)
        b->hist_i = previous;
    free(target);
}

static void addr_insert(struct bstate *b, char c)
{
    int i;

    if (b->addr_len + 1 >= (int)sizeof(b->addr))
        return;
    for (i = b->addr_len; i > b->addr_cur; i--)
        b->addr[i] = b->addr[i - 1];
    b->addr[b->addr_cur++] = c;
    b->addr_len++;
    b->addr[b->addr_len] = 0;
}

static void addr_delete(struct bstate *b, int before)
{
    int at = before ? b->addr_cur - 1 : b->addr_cur;
    int i;

    if (at < 0 || at >= b->addr_len)
        return;
    for (i = at; i < b->addr_len - 1; i++)
        b->addr[i] = b->addr[i + 1];
    b->addr_len--;
    b->addr[b->addr_len] = 0;
    if (before)
        b->addr_cur--;
}

static void restore_address(struct bstate *b)
{
    const char *url = b->page ? b->page->url : "";

    snprintf(b->addr, sizeof(b->addr), "%s", url);
    b->addr_len = (int)strlen(b->addr);
    b->addr_cur = b->addr_len;
}

struct address_work {
    char url[URL_MAX];
    char error[192];
};

static BROWSER_NOINLINE void open_address_bar(struct bstate *b)
{
    struct address_work *w;

    w = (struct address_work *)malloc(sizeof(*w));
    if (!w) {
        set_status(b, 1, "out of memory opening address", "");
        return;
    }
    if (canonicalize_input(b->addr, w->url, sizeof(w->url), w->error,
                           sizeof(w->error)) != 0) {
        set_status(b, 1, "bad address:", w->error);
    } else if (!url_supported(w->url)) {
        set_status(b, 1, "unsupported address:", w->url);
    } else {
        load_gui(b, w->url, 1);
    }
    free(w);
}

static struct dom_node *element_ancestor(struct dom_node *n, int tag)
{
    while (n) {
        if (n->type == DOM_ELEMENT && n->tag_id == tag)
            return n;
        n = n->parent;
    }
    return 0;
}

static const char *control_type(struct dom_node *n)
{
    const char *t = n && n->tag_id == HTAG_INPUT
        ? dom_get_attr(n, "type") : 0;
    return t && *t ? t : "text";
}

static char *control_value(struct dom_node *n)
{
    const char *v;

    if (!n)
        return str_dup("");
    v = dom_get_attr(n, "value");
    if (v)
        return str_dup(v);
    if (n->tag_id == HTAG_TEXTAREA || n->tag_id == HTAG_OPTION)
        return dom_text_content(n, 0);
    return str_dup("");
}

static int form_encode_byte(struct bytebuf *out, unsigned char c)
{
    static const char hex[] = "0123456789ABCDEF";
    char enc[3];

    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '*')
        return buf_add(out, (const char *)&c, 1);
    if (c == ' ')
        return buf_add(out, "+", 1);
    enc[0] = '%';
    enc[1] = hex[c >> 4];
    enc[2] = hex[c & 15];
    return buf_add(out, enc, 3);
}

static int form_add_pair(struct bytebuf *out, const char *name,
                         const char *value)
{
    const unsigned char *p;

    if (out->n && buf_add(out, "&", 1) != 0)
        return -1;
    for (p = (const unsigned char *)name; *p; p++)
        if (form_encode_byte(out, *p) != 0)
            return -1;
    if (buf_add(out, "=", 1) != 0)
        return -1;
    for (p = (const unsigned char *)value; *p; p++)
        if (form_encode_byte(out, *p) != 0)
            return -1;
    return 0;
}

static int option_add(struct bytebuf *out, const char *name,
                      struct dom_node *option)
{
    char *owned = 0;
    const char *v = dom_get_attr(option, "value");
    int rc;

    if (!v) {
        owned = dom_text_content(option, 0);
        v = owned ? owned : "";
    }
    rc = form_add_pair(out, name, v);
    free(owned);
    return rc;
}

static int build_form_data(struct dom_node *form, struct dom_node *submitter,
                           struct bytebuf *out)
{
    struct dom_node *n;

    memset(out, 0, sizeof(*out));
    out->limit = 64UL * 1024UL;
    for (n = form->first_child; n; n = dom_next_within(n, form)) {
        const char *name, *type;
        char *value;

        if (n->type != DOM_ELEMENT ||
            (n->tag_id != HTAG_INPUT && n->tag_id != HTAG_TEXTAREA &&
             n->tag_id != HTAG_SELECT && n->tag_id != HTAG_BUTTON) ||
            dom_has_attr(n, "disabled"))
            continue;
        name = dom_get_attr(n, "name");
        if (!name || !*name)
            continue;
        type = control_type(n);
        if (n->tag_id == HTAG_INPUT &&
            (str_ci_eq(type, "checkbox") || str_ci_eq(type, "radio")) &&
            !dom_has_attr(n, "checked"))
            continue;
        if ((n->tag_id == HTAG_BUTTON ||
             (n->tag_id == HTAG_INPUT &&
              (str_ci_eq(type, "submit") || str_ci_eq(type, "image")))) &&
            n != submitter)
            continue;
        if (n->tag_id == HTAG_INPUT &&
            (str_ci_eq(type, "button") || str_ci_eq(type, "reset") ||
             str_ci_eq(type, "file")))
            continue;
        if (n->tag_id == HTAG_SELECT) {
            struct dom_node *o;
            int selected = 0;

            for (o = n->first_child; o; o = dom_next_within(o, n))
                if (o->type == DOM_ELEMENT && o->tag_id == HTAG_OPTION &&
                    dom_has_attr(o, "selected")) {
                    if (option_add(out, name, o) != 0)
                        goto fail;
                    selected = 1;
                    if (!dom_has_attr(n, "multiple"))
                        break;
                }
            if (!selected)
                for (o = n->first_child; o; o = dom_next_within(o, n))
                    if (o->type == DOM_ELEMENT && o->tag_id == HTAG_OPTION) {
                        if (option_add(out, name, o) != 0)
                            goto fail;
                        break;
                    }
            continue;
        }
        value = control_value(n);
        if (!value)
            goto fail;
        if ((str_ci_eq(type, "checkbox") || str_ci_eq(type, "radio")) &&
            !*value) {
            free(value);
            value = str_dup("on");
            if (!value)
                goto fail;
        }
        if (form_add_pair(out, name, value) != 0) {
            free(value);
            goto fail;
        }
        free(value);
    }
    if (!out->p) {
        out->p = str_dup("");
        out->cap = 1;
    }
    return out->p ? 0 : -1;

fail:
    free(out->p);
    memset(out, 0, sizeof(*out));
    return -1;
}

static int sync_script_page(struct bstate *b)
{
    char err[256];
    const char *pending;
    char *target;
    int url_sync;

    if (!b->page || !b->page->js)
        return 0;
    if (jsdom_pump(b->page->js, err, sizeof(err)) < 0)
        set_status(b, 1, "page timer failed:", err);
    url_sync = page_sync_document_url(b->page);
    if (url_sync < 0) {
        set_status(b, 1, "cannot update script history URL", "");
        return -1;
    }
    if (url_sync > 0 && !b->addr_focus)
        restore_address(b);
    if (jsdom_dirty(b->page->js)) {
        if (page_reflow(b->rt, b->page, b->view_w, b->view_h,
                        err, sizeof(err)) != 0) {
            set_status(b, 1, "script changed the page, but reflow failed:", err);
            return -1;
        }
        jsdom_clear_dirty(b->page->js);
        clamp_scroll(b);
    }
    pending = jsdom_pending_navigation(b->page->js);
    if (!pending)
        return 0;
    target = str_dup(pending);
    jsdom_clear_navigation(b->page->js);
    if (!target) {
        set_status(b, 1, "out of memory following script navigation", "");
        return -1;
    }
    if (!url_supported(target)) {
        set_status(b, 1, "script requested an unsupported address:", target);
        free(target);
        return -1;
    }
    load_gui(b, target, 1);
    free(target);
    return 1;
}

static void submit_form(struct bstate *b, struct dom_node *form,
                        struct dom_node *submitter)
{
    struct bytebuf data;
    const char *action, *method;
    char target[URL_MAX];
    char err[192];
    int allow;

    if (!form || form->tag_id != HTAG_FORM)
        return;
    allow = !b->page->js ||
        jsdom_dispatch(b->page->js, form, "submit", err, sizeof(err));
    if (!allow) {
        sync_script_page(b);
        set_status(b, 0, "form submission cancelled by page script", "");
        return;
    }
    if (build_form_data(form, submitter, &data) != 0) {
        set_status(b, 1, "cannot encode form data", "");
        return;
    }
    action = dom_get_attr(form, "action");
    if (!action || !*action)
        action = b->page->url;
    if (url_resolve_str(b->page->base_url, action, target, sizeof(target)) !=
        URL_OK || !url_supported(target)) {
        free(data.p);
        set_status(b, 1, "invalid form action", action);
        return;
    }
    method = dom_get_attr(form, "method");
    if (method && str_ci_eq(method, "post")) {
        load_gui_request(b, target, 1, "POST", data.p, data.n,
                         "application/x-www-form-urlencoded");
    } else {
        char *full;
        const char *frag = strchr(target, '#');
        unsigned long base_len = frag ? (unsigned long)(frag - target)
                                      : strlen(target);
        unsigned long need = base_len + data.n + 3;
        char sep = strchr(target, '?') ? '&' : '?';

        full = (char *)malloc(need);
        if (!full) {
            free(data.p);
            set_status(b, 1, "out of memory opening form result", "");
            return;
        }
        memcpy(full, target, base_len);
        full[base_len] = 0;
        if (data.n)
            snprintf(full + base_len, need - base_len, "%c%s", sep, data.p);
        load_gui(b, full, 1);
        free(full);
    }
    free(data.p);
}

static int edit_focused_control(struct bstate *b, unsigned int key)
{
    struct dom_node *n = b->focus_node;
    char *old, *value;
    unsigned long len, pos;
    int changed = 0;
    char err[160];

    if (!n || (n->tag_id != HTAG_INPUT && n->tag_id != HTAG_TEXTAREA))
        return 0;
    if (n->tag_id == HTAG_INPUT &&
        !(str_ci_eq(control_type(n), "text") ||
          str_ci_eq(control_type(n), "search") ||
          str_ci_eq(control_type(n), "password") ||
          str_ci_eq(control_type(n), "email") ||
          str_ci_eq(control_type(n), "url") ||
          str_ci_eq(control_type(n), "tel") ||
          str_ci_eq(control_type(n), "number")))
        return 0;
    old = control_value(n);
    if (!old)
        return 1;
    len = strlen(old);
    if (b->focus_cursor < 0 || (unsigned long)b->focus_cursor > len)
        b->focus_cursor = (int)len;
    pos = (unsigned long)b->focus_cursor;
    value = (char *)malloc(len + 2);
    if (!value) {
        free(old);
        return 1;
    }
    memcpy(value, old, len + 1);
    if (key == KEY_LEFT) {
        if (b->focus_cursor > 0) b->focus_cursor--;
    } else if (key == KEY_RIGHT) {
        if ((unsigned long)b->focus_cursor < len) b->focus_cursor++;
    } else if (key == KEY_HOME) {
        b->focus_cursor = 0;
    } else if (key == KEY_END) {
        b->focus_cursor = (int)len;
    } else if (key == 8 && pos > 0) {
        memmove(value + pos - 1, value + pos, len - pos + 1);
        b->focus_cursor--;
        changed = 1;
    } else if (key == KEY_DELETE && pos < len) {
        memmove(value + pos, value + pos + 1, len - pos);
        changed = 1;
    } else if ((key == '\n' || key == '\r') && n->tag_id == HTAG_TEXTAREA &&
               len < 4096) {
        memmove(value + pos + 1, value + pos, len - pos + 1);
        value[pos] = '\n';
        b->focus_cursor++;
        changed = 1;
    } else if ((key == '\n' || key == '\r') && n->tag_id == HTAG_INPUT) {
        struct dom_node *form = element_ancestor(n, HTAG_FORM);
        free(value);
        free(old);
        if (form) submit_form(b, form, 0);
        return 1;
    } else if (key >= 32 && key < 127 && len < 4096) {
        memmove(value + pos + 1, value + pos, len - pos + 1);
        value[pos] = (char)key;
        b->focus_cursor++;
        changed = 1;
    }
    if (changed) {
        dom_set_attr(n, "value", value);
        if (b->page->js)
            jsdom_dispatch(b->page->js, n, "input", err, sizeof(err));
        if (b->page->js)
            sync_script_page(b);
        else
            page_reflow(b->rt, b->page, b->view_w, b->view_h,
                        err, sizeof(err));
    }
    free(value);
    free(old);
    return 1;
}

static BROWSER_NOINLINE void on_key(struct bstate *b, unsigned int key)
{
    int page_step = b->view_h - GLYPH_H * 2;

    if (page_step < GLYPH_H)
        page_step = GLYPH_H;
    switch (key) {
    case 17: b->quit = 1; return;             /* ctrl-Q */
    case 12:                                  /* ctrl-L */
        b->addr_focus = 1;
        b->addr_cur = b->addr_len;
        return;
    case 18:                                  /* ctrl-R */
        if (b->page)
            load_gui(b, b->page->url, 0);
        return;
    case 2: go_back(b); return;               /* ctrl-B */
    case 6: go_fwd(b); return;                /* ctrl-F */
    case 27:
        b->addr_focus = 0;
        restore_address(b);
        return;
    default:
        break;
    }

    if (b->addr_focus) {
        switch (key) {
        case '\n':
        case '\r':
            b->addr_focus = 0;
            open_address_bar(b);
            return;
        case 8: addr_delete(b, 1); return;
        case KEY_DELETE: addr_delete(b, 0); return;
        case KEY_LEFT:
            if (b->addr_cur > 0) b->addr_cur--;
            return;
        case KEY_RIGHT:
            if (b->addr_cur < b->addr_len) b->addr_cur++;
            return;
        case KEY_HOME: b->addr_cur = 0; return;
        case KEY_END: b->addr_cur = b->addr_len; return;
        case 21:
            b->addr[0] = 0;
            b->addr_len = b->addr_cur = 0;
            return;
        default:
            if (key >= 32 && key < 127)
                addr_insert(b, (char)key);
            return;
        }
    }

    if (edit_focused_control(b, key))
        return;

    switch (key) {
    case KEY_UP:   b->scroll -= GLYPH_H * 2; break;
    case KEY_DOWN: b->scroll += GLYPH_H * 2; break;
    case KEY_PGUP: b->scroll -= page_step;   break;
    case KEY_PGDN: b->scroll += page_step;   break;
    case ' ':      b->scroll += page_step;   break;
    case KEY_HOME: b->scroll = 0;            break;
    case KEY_END:  b->scroll = content_h(b); break;
    default: break;
    }
    clamp_scroll(b);
}

static void radio_select(struct browser_page *p, struct dom_node *chosen)
{
    const char *name = dom_get_attr(chosen, "name");
    struct dom_node *n;

    if (name && *name)
        for (n = p->doc->root; n; n = dom_next(n))
            if (n != chosen && n->type == DOM_ELEMENT &&
                n->tag_id == HTAG_INPUT &&
                str_ci_eq(control_type(n), "radio") &&
                dom_get_attr(n, "name") &&
                !strcmp(dom_get_attr(n, "name"), name))
                dom_remove_attr(n, "checked");
    dom_set_attr(chosen, "checked", "");
}

static void select_next_option(struct dom_node *select)
{
    struct dom_node *n, *first = 0, *selected = 0, *next = 0;

    for (n = select->first_child; n; n = dom_next_within(n, select)) {
        if (n->type != DOM_ELEMENT || n->tag_id != HTAG_OPTION ||
            dom_has_attr(n, "disabled"))
            continue;
        if (!first) first = n;
        if (selected && !next) next = n;
        if (dom_has_attr(n, "selected")) selected = n;
    }
    if (selected)
        dom_remove_attr(selected, "selected");
    if (!next)
        next = first;
    if (next)
        dom_set_attr(next, "selected", "");
}

static BROWSER_NOINLINE void on_click(struct bstate *b, int x, int y)
{
    int bx = PAD + 184;

    if (y < BAR_H) {
        b->addr_focus = 0;
        if (x >= PAD && x < PAD + 46) {
            go_back(b);
        } else if (x >= PAD + 50 && x < PAD + 112) {
            go_fwd(b);
        } else if (x >= PAD + 116 && x < PAD + 178) {
            if (b->page)
                load_gui(b, b->page->url, 0);
        } else if (x >= bx) {
            int cw = b->win->w - bx - PAD;
            int maxch = (cw - 8) / GLYPH_W;
            int from;

            b->addr_focus = 1;
            from = b->addr_cur > maxch ? b->addr_cur - maxch : 0;
            b->addr_cur = from + (x - bx - 4) / GLYPH_W;
            if (b->addr_cur > b->addr_len)
                b->addr_cur = b->addr_len;
            if (b->addr_cur < 0)
                b->addr_cur = 0;
        }
        return;
    }
    if (y >= b->win->h - STAT_H)
        return;
    b->addr_focus = 0;

    if (x >= b->view_w) {
        int ch = content_h(b);

        if (ch > b->view_h) {
            int rel = y - BAR_H;

            b->scroll = mul_div_i(rel, (int64_t)ch - b->view_h,
                                  b->view_h ? b->view_h : 1);
            clamp_scroll(b);
            b->sb_drag = 1;
        }
        return;
    }

    if (b->page && b->page->layout) {
        const char *href = 0;
        struct dom_node *node, *action = 0;
        struct dom_node *form = 0;
        char event_error[192];
        int allow = 1;
        int doc_x = x;                    /* x - origin_x + scroll_x */
        int doc_y = y - BAR_H + b->scroll;

        node = lay_node_at(b->page->layout, doc_x, doc_y);
        for (action = node; action; action = action->parent) {
            if (action->type != DOM_ELEMENT)
                continue;
            if (action->tag_id == HTAG_A || action->tag_id == HTAG_INPUT ||
                action->tag_id == HTAG_BUTTON ||
                action->tag_id == HTAG_SELECT ||
                action->tag_id == HTAG_TEXTAREA)
                break;
        }
        if (!action)
            action = node;
        if (action && action->type == DOM_ELEMENT &&
            (action->tag_id == HTAG_INPUT ||
             action->tag_id == HTAG_TEXTAREA ||
             action->tag_id == HTAG_SELECT ||
             action->tag_id == HTAG_BUTTON)) {
            b->focus_node = action;
            {
                char *v = control_value(action);
                b->focus_cursor = v ? (int)strlen(v) : 0;
                free(v);
            }
        } else {
            b->focus_node = 0;
        }
        if (b->page->js && action)
            allow = jsdom_dispatch(b->page->js, action, "click",
                                   event_error, sizeof(event_error));
        if (allow && action && action->type == DOM_ELEMENT) {
            if (action->tag_id == HTAG_INPUT &&
                str_ci_eq(control_type(action), "checkbox")) {
                if (dom_has_attr(action, "checked"))
                    dom_remove_attr(action, "checked");
                else
                    dom_set_attr(action, "checked", "");
                if (b->page->js)
                    jsdom_dispatch(b->page->js, action, "change",
                                   event_error, sizeof(event_error));
            } else if (action->tag_id == HTAG_INPUT &&
                       str_ci_eq(control_type(action), "radio")) {
                radio_select(b->page, action);
                if (b->page->js)
                    jsdom_dispatch(b->page->js, action, "change",
                                   event_error, sizeof(event_error));
            } else if (action->tag_id == HTAG_SELECT) {
                select_next_option(action);
                if (b->page->js)
                    jsdom_dispatch(b->page->js, action, "change",
                                   event_error, sizeof(event_error));
            }
        }
        if (b->page->js && sync_script_page(b) != 0)
            return;
        if (allow && action && action->type == DOM_ELEMENT &&
            (action->tag_id == HTAG_BUTTON ||
             (action->tag_id == HTAG_INPUT &&
              str_ci_eq(control_type(action), "submit")))) {
            form = element_ancestor(action, HTAG_FORM);
            if (form) {
                submit_form(b, form, action);
                return;
            }
        }
        if (allow &&
            lay_link_at(b->page->layout, doc_x, doc_y, &href) &&
            href && *href)
            navigate_relative(b, href);
        else if (allow && action && action->type == DOM_ELEMENT &&
                 (action->tag_id == HTAG_INPUT ||
                  action->tag_id == HTAG_SELECT))
            page_reflow(b->rt, b->page, b->view_w, b->view_h,
                        event_error, sizeof(event_error));
    }
}

static BROWSER_NOINLINE int
gui_mode(struct browser_runtime *rt, const char *start_url)
{
    struct bstate *b;
    struct k_event ev;
    struct k_fbinfo fb;
    int result = 0;

    if (syscall(SYS_FBINFO, (long)&fb, 0, 0, 0) != 0 || !fb.present) {
        printf("browser: no framebuffer; use 'browser -t <url>'\n");
        return 1;
    }
    b = (struct bstate *)calloc(1, sizeof(*b));
    if (!b) {
        printf("browser: out of memory creating the window state\n");
        return 1;
    }
    b->hist_i = -1;
    b->rt = rt;
    b->win = gui_open("Kestrel Browser", 40, 40, WIN_W, WIN_H, 0);
    if (!b->win) {
        printf("browser: cannot open a window (is the desktop running?)\n");
        printf("browser: use 'browser -t <url>' for text mode\n");
        free(b);
        return 1;
    }
    b->view_w = b->win->w - SB_W;
    b->view_h = b->win->h - BAR_H - STAT_H;
    if (b->view_w < 1) b->view_w = 1;
    if (b->view_h < 1) b->view_h = 1;

    if (load_gui(b, start_url, 1) != 0)
        result = 1;
    draw(b);

    while (!b->quit) {
        int r = gui_next_event(b->win, &ev, 200);

        if (r < 0)
            break;
        if (r == 0) {
            if (sync_script_page(b) != 0)
                result = 1;
            draw(b);
            continue;
        }
        /* Event timing is useful extra input on machines without a hardware
         * random instruction; it never replaces the DRBG's other sources. */
        tls_add_entropy(&ev, sizeof(ev));
        switch (ev.type) {
        case KEV_KEY:
            on_key(b, ev.key);
            break;
        case KEV_MOUSE_DOWN:
            on_click(b, ev.x, ev.y);
            break;
        case KEV_MOUSE_UP:
            b->sb_drag = 0;
            break;
        case KEV_MOUSE_MOVE:
            if (b->sb_drag && (ev.buttons & K_MOUSE_LEFT)) {
                int ch = content_h(b);

                if (ch > b->view_h) {
                    int rel = ev.y - BAR_H;

                    b->scroll = mul_div_i(rel,
                                          (int64_t)ch - b->view_h,
                                          b->view_h ? b->view_h : 1);
                    clamp_scroll(b);
                }
            } else {
                continue;
            }
            break;
        case KEV_CLOSE:
            b->quit = 1;
            break;
        default:
            continue;
        }
        draw(b);
    }

    page_destroy(b->page);
    history_discard_from(b, 0);
    gui_close(b->win);
    free(b);
    return result;
}

/* ------------------------------------------------------------------ *
 * CLI
 * ------------------------------------------------------------------ */

static void usage(void)
{
    printf("usage: browser [-t] [-w cols] [-l] [-v] <url|file>\n");
    printf("  -t        text mode: render to stdout and exit\n");
    printf("  -w cols   text width (default %d, max %d)\n",
           TEXT_COLS, MAX_COLS);
    printf("  -l        list resolved page links after the text\n");
    printf("  -v        print final URL, status, size and title first\n");
    printf("\n");
    printf("Verified https://, http:// and local files are supported.\n");
    printf("In the window: click links, Back/Fwd/Reload, arrows and\n");
    printf("PgUp/PgDn scroll, ctrl-L address bar, ctrl-R reload,\n");
    printf("ctrl-B back, ctrl-F forward, ctrl-Q quit.\n");
}

struct startup_work {
    struct browser_runtime runtime;
    char url[URL_MAX];
    char error[320];
};

int main(int argc, char **argv)
{
    struct startup_work *w;
    const char *target = 0;
    int text = 0, cols = TEXT_COLS, links = 0, verbose = 0;
    int i, rc;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) == 0) {
            g_cwd = argv[i] + 6;
        } else if (strcmp(argv[i], "-t") == 0 ||
                   strcmp(argv[i], "--text") == 0) {
            text = 1;
        } else if (strcmp(argv[i], "-l") == 0) {
            links = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            cols = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1]) {
            printf("browser: unknown option %s\n", argv[i]);
            usage();
            return 2;
        } else if (!target) {
            target = argv[i];
        }
    }

    if (!target) {
        if (!text)
            target = BROWSER_HOME;
        else {
            usage();
            return 2;
        }
    }
    if (cols < 20)
        cols = 20;
    if (cols > MAX_COLS)
        cols = MAX_COLS;
    w = (struct startup_work *)calloc(1, sizeof(*w));
    if (!w) {
        printf("browser: out of memory creating startup state\n");
        return 1;
    }
    if (canonicalize_input(target, w->url, sizeof(w->url), w->error,
                           sizeof(w->error)) != 0) {
        printf("browser: %s\n", w->error);
        free(w);
        return 2;
    }
    if (runtime_init(&w->runtime, w->error, sizeof(w->error)) != 0) {
        printf("browser: %s\n", w->error);
        free(w);
        return 1;
    }

    if (text)
        rc = text_mode(&w->runtime, w->url, cols, links, verbose);
    else
        rc = gui_mode(&w->runtime, w->url);

    runtime_destroy(&w->runtime);
    free(w);
    return rc;
}
