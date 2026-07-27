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
 * Deliberate limits: bodies are capped at 1 MiB; this wave does not fetch
 * external stylesheets or images and does not execute JavaScript.  HTTPS is
 * always TLS 1.3 with certificate verification required.
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
#include "../libweb/layout.h"
#include "../libweb/paint.h"
#include "../libweb/url.h"

TLS_ASSERT_TRANSPORT_LAYOUT();

#define BROWSER_HOME "/doc/home.html"
#define FETCH_MAX    (1024UL * 1024UL)
#define HIST_MAX     64
#define TEXT_COLS    78
#define MAX_COLS     200
#define LINK_MAX     4096

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
    int store_live;
};

struct browser_page {
    char *url;                     /* final URL, including redirects */
    struct dom_document *doc;
    struct css_stylesheet *author;
    struct style_engine *styles;
    struct lay_document *layout;

    unsigned long bytes;
    int status;                    /* HTTP status, 0 for local */
    int is_local;
    int is_html;
    int failed;                    /* transport/local error or non-2xx */
    char error[320];
};

/* ------------------------------------------------------------------ *
 * Small utilities
 * ------------------------------------------------------------------ */

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

static void runtime_destroy(struct browser_runtime *rt)
{
    if (!rt)
        return;
    if (rt->http) {
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
 * Page construction and lifetime
 * ------------------------------------------------------------------ */

static void page_content_destroy(struct browser_page *p)
{
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
    dom_document_free(p->doc);
    p->doc = 0;
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
build_pipeline(struct browser_runtime *rt, struct browser_page *p,
               const char *src, unsigned long len, int viewport_w,
               int viewport_h, char *err, unsigned long errsz)
{
    struct css_media media;
    struct css_stylesheet *sheets[2];
    struct lay_opts lo;
    const struct lay_paint_item *paint_items;
    int styled;

    p->doc = html_parse_document(src, len);
    if (!p->doc) {
        snprintf(err, errsz, "out of memory parsing %lu bytes", len);
        return -1;
    }
    if (p->doc->oom) {
        snprintf(err, errsz, "out of memory while building the DOM");
        return -1;
    }

    memset(&media, 0, sizeof(media));
    media.width = viewport_w;
    media.height = viewport_h;
    media.dpi = 96;
    media.screen = 1;
    css_set_media(rt->ua, &media);

    /* Parse even an empty collected sheet: it is the author-origin member
     * of this page's fixed [UA, author] cascade. */
    p->author = css_parse(p->doc->style_text, p->doc->style_len,
                          CSS_ORIGIN_AUTHOR, &media);
    if (!p->author) {
        snprintf(err, errsz, "out of memory parsing page styles");
        return -1;
    }
    sheets[0] = rt->ua;
    sheets[1] = p->author;
    p->styles = style_engine_new(sheets, 2, css_dom_ops());
    if (!p->styles) {
        snprintf(err, errsz, "out of memory creating the style engine");
        return -1;
    }
    style_engine_set_viewport(p->styles, viewport_w, viewport_h);
    styled = style_compute_tree(p->styles, p->doc->root, 0,
                                css_style_dom_sink, 0);
    if (styled <= 0) {
        snprintf(err, errsz, "out of memory computing page styles");
        return -1;
    }

    lay_opts_init(&lo, viewport_w, viewport_h);
    p->layout = lay_layout(p->doc, &lo);
    if (!p->layout) {
        snprintf(err, errsz, "out of memory starting page layout");
        return -1;
    }
    /* The current layout API reports paint-order allocation failure as an
     * empty order.  An actually empty document has only its ICB; anything
     * more substantial must have at least one background/content item. */
    if (lay_box_count(p->layout) > 1 &&
        lay_paint_order(p->layout, &paint_items) <= 0) {
        snprintf(err, errsz, "out of memory building page paint order");
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
page_load(struct browser_runtime *rt, const char *target, int viewport_w,
          int viewport_h, char *fatal, unsigned long fatalsz)
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

    if (!url_supported(target)) {
        snprintf(w->primary_error, sizeof(w->primary_error),
                 "unsupported URL scheme (use file, http, or https)");
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
        if (build_error_page(rt, p, viewport_w, viewport_h,
                             w->primary_error, fatal, fatalsz) != 0) {
            page_load_work_destroy(w);
            page_destroy(p);
            return 0;
        }
        page_load_work_destroy(w);
        return p;
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
        page_content_destroy(p);
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
            int64_t available = (int64_t)cols - (int64_t)col - 2;
            int start = clamp_i64((int64_t)col + 1);

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
    char page_info[128];
    int status_err;
    int sb_drag;
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
load_gui(struct bstate *b, const char *url, int push)
{
    struct browser_page *next, *old;
    char *history_copy;

    set_status(b, 0, "loading", url);
    draw(b);
    next = page_load(b->rt, url, b->view_w, b->view_h,
                     b->load_error, sizeof(b->load_error));
    if (!next) {
        set_status(b, 1, "cannot replace this page:", b->load_error);
        return -1;
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
    history_commit(b, history_copy, push);
    snprintf(b->addr, sizeof(b->addr), "%s", next->url);
    b->addr_len = (int)strlen(b->addr);
    b->addr_cur = b->addr_len;
    set_fragment_scroll(b);

    if (next->failed) {
        set_status(b, 1, "error:", next->error);
    } else {
        snprintf(b->page_info, sizeof(b->page_info), "%s%lu bytes%s%s",
                 next->is_local ? "local file, " : "",
                 next->bytes,
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
        int doc_x = x;                    /* x - origin_x + scroll_x */
        int doc_y = y - BAR_H + b->scroll;

        if (lay_link_at(b->page->layout, doc_x, doc_y, &href) &&
            href && *href)
            navigate_relative(b, href);
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
        if (r == 0)
            continue;
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
