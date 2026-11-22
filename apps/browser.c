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
#ifndef JS_HOST
#include <kestrel_cwd.h>
#endif
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
#define TAB_H   26
#define BAR_H   30
#define BMARK_H 24
#define FIND_H  26
#define DEV_H   200
#define DEV_HDR_H 24
#define REPL_H  24
#define STAT_H  20
#define SB_W    12
#define SELECT_SB_W 8
#define PAD     6
#define GLYPH_W GUI_FONT_W
#define GLYPH_H GUI_FONT_H

#define MAX_TABS        8
#define CONSOLE_LOG_MAX 64
#define NET_LOG_MAX     64
#define BOOKMARK_MAX    16
#define BOOKMARK_FILE   "/home/.browser_bookmarks"
#define SESSION_FILE    "/home/.browser_session"
#define SESSION_MAX_BYTES 32768UL
#define FAVICON_CACHE_MAX 16
#define FAVICON_SIZE    16
#define FAVICON_MAX_BYTES (1024UL * 1024UL)
#define DOWNLOAD_DIR    "/home/Downloads"
#define DOWNLOAD_MAX    (16UL * 1024UL * 1024UL)

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

struct browser_tab;

struct browser_runtime {
    struct tls_options tls;
    struct http_client *http;
    struct css_stylesheet *ua;
    struct web_storage *local_storage;
    struct web_storage *session_storage;
    char storage_path[96];
    int store_live;
    int is_gui;
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
    struct browser_tab *tab;
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
    int scroll_x, scroll_y;
    int viewport_w, viewport_h;
    int has_hover_css;
    enum browser_load_state load_state;
    char error[320];
};

struct net_log_entry {
    int status;
    unsigned long bytes;
    char kind[16];
    char url[160];
};

struct favicon_entry {
    char *url;
    struct image icon16;
    int state;                     /* 0 empty, 1 decoded, -1 failed */
    unsigned long last_used;
};

struct browser_tab {
    struct browser_page *page;
    char *hist[HIST_MAX];
    int hist_n, hist_i;
    char addr[URL_MAX];
    int addr_len, addr_cur;
    int scroll;
    int zoom_pct;
    char *favicon_url;
    struct dom_node *focus_node;
    int focus_cursor;
    struct dom_node *hover_node;
    struct dom_node *active_node;
    struct dom_node *select_open_node;
    int select_hover;
    int select_top;
    int reader_mode;
    char *raw_source;
    unsigned long raw_len;
    int raw_is_md;
    int last_vw, last_vh, last_prefers_dark;
    char page_status[320];
    int page_status_err;

    char *console_lines[CONSOLE_LOG_MAX];
    int console_head;
    int console_count;

    struct net_log_entry net_entries[NET_LOG_MAX];
    int net_head;
    int net_count;

    int elem_scroll;
    int con_scroll;
    int net_scroll;
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

static int url_is_view_source(const char *s, const char **inner_out)
{
    static const char prefix[] = "view-source:";
    int i;

    if (!s)
        return 0;
    for (i = 0; i < 12; i++) {
        if (ascii_lower((unsigned char)s[i]) != prefix[i])
            return 0;
    }
    if (inner_out)
        *inner_out = s + 12;
    return 1;
}

static BROWSER_NOINLINE int url_supported(const char *s)
{
    const char *inner = 0;
    struct url *u;
    int ok;

    if (url_is_view_source(s, &inner)) {
        if (!*inner || url_is_view_source(inner, 0))
            return 0;
        s = inner;
    }
    u = (struct url *)malloc(sizeof(*u));
    if (!u)
        return 0;
    ok = (url_parse(s, u) == URL_OK && u->has_scheme &&
          (str_ci_eq(u->scheme, "file") || str_ci_eq(u->scheme, "http") ||
           str_ci_eq(u->scheme, "https")));
    free(u);
    return ok;
}

static BROWSER_NOINLINE int url_is_scheme(const char *s, const char *scheme)
{
    struct url *u;
    int ok;

    u = (struct url *)malloc(sizeof(*u));
    if (!u)
        return 0;
    ok = (url_parse(s, u) == URL_OK && u->has_scheme &&
          str_ci_eq(u->scheme, scheme));
    free(u);
    return ok;
}

struct canon_work {
    char in[URL_MAX];
    char path[URL_PATH_MAX];
    char tmp[URL_MAX];
    char decoded[URL_PATH_MAX];
    char inner_out[URL_MAX];
    struct url u;
};

static int canonicalize_inner(struct canon_work *w, const char *raw, char *out,
                              unsigned long outsz, char *err,
                              unsigned long errsz)
{
    struct k_stat st;
    int rc;

    rc = url_parse(raw, &w->u);
    if (rc == URL_OK && w->u.has_scheme) {
        /* file:foo is interpreted relative to the shell's cwd. */
        if (str_ci_eq(w->u.scheme, "file") && w->u.path[0] != '/') {
            if (url_pct_decode(w->u.path, ~0UL, 0, w->decoded,
                               sizeof(w->decoded)) < 0 ||
                path_join(w->decoded, w->path, sizeof(w->path)) != 0) {
                snprintf(err, errsz, "the local path is too long");
                return -1;
            }
            if (snprintf(w->tmp, sizeof(w->tmp), "file://%s", w->path) < 0) {
                snprintf(err, errsz, "cannot form a file URL");
                return -1;
            }
            rc = url_parse(w->tmp, &w->u);
            if (rc != URL_OK) {
                snprintf(err, errsz, "bad file URL: %s", url_strerror(rc));
                return -1;
            }
        }
        url_normalize(&w->u, URL_N_ALL);
        rc = url_serialize(&w->u, out, outsz);
        if (rc != URL_OK) {
            snprintf(err, errsz, "address is too long: %s",
                     url_strerror(rc));
            return -1;
        }
        return 0;
    }

    /* An existing path, or an explicitly absolute path, wins over the
     * hostname shorthand. */
    if (path_join(raw, w->path, sizeof(w->path)) == 0 &&
        (raw[0] == '/' || stat_(w->path, &st) == 0)) {
        int n = snprintf(w->tmp, sizeof(w->tmp), "file://%s", w->path);

        if (n < 0 || (unsigned long)n >= sizeof(w->tmp) ||
            url_parse(w->tmp, &w->u) != URL_OK) {
            snprintf(err, errsz, "the local path is too long");
            return -1;
        }
        url_normalize(&w->u, URL_N_ALL);
        if (url_serialize(&w->u, out, outsz) != URL_OK) {
            snprintf(err, errsz, "the local file URL is too long");
            return -1;
        }
        return 0;
    }

    if (snprintf(w->tmp, sizeof(w->tmp), "http://%s", raw) < 0 ||
        url_parse(w->tmp, &w->u) != URL_OK) {
        snprintf(err, errsz, "malformed address");
        return -1;
    }
    url_normalize(&w->u, URL_N_ALL);
    if (url_serialize(&w->u, out, outsz) != URL_OK) {
        snprintf(err, errsz, "address is too long");
        return -1;
    }
    return 0;
}

/* Turn a target typed at the shell/address bar into an absolute URL.  Local
 * files use file: URLs too, so RFC 3986 resolution works for every page. */
static BROWSER_NOINLINE int canonicalize_input(const char *input, char *out,
                                               unsigned long outsz, char *err,
                                               unsigned long errsz)
{
    struct canon_work *w;
    const char *inner = 0;
    int rc;

    w = (struct canon_work *)malloc(sizeof(*w));
    if (!w) {
        snprintf(err, errsz, "out of memory parsing address");
        return -1;
    }
    if (trim_copy(input, w->in, sizeof(w->in)) != 0) {
        snprintf(err, errsz, "the address is empty or too long");
        free(w);
        return -1;
    }
    if (url_is_view_source(w->in, &inner)) {
        while (*inner == ' ' || *inner == '\t')
            inner++;
        if (!*inner) {
            snprintf(err, errsz, "missing URL after view-source:");
            free(w);
            return -1;
        }
        if (url_is_view_source(inner, 0)) {
            snprintf(err, errsz, "nested view-source: URLs are not supported");
            free(w);
            return -1;
        }
        rc = canonicalize_inner(w, inner, w->inner_out, sizeof(w->inner_out),
                                err, errsz);
        if (rc == 0) {
            int n = snprintf(out, outsz, "view-source:%s", w->inner_out);
            if (n < 0 || (unsigned long)n >= outsz) {
                snprintf(err, errsz, "view-source address is too long");
                rc = -1;
            }
        }
        free(w);
        return rc;
    }
    rc = canonicalize_inner(w, w->in, out, outsz, err, errsz);
    free(w);
    return rc;
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

static void page_fill_media(const struct browser_runtime *rt,
                            int viewport_w, int viewport_h,
                            struct css_media *m)
{
    memset(m, 0, sizeof(*m));
    m->width = viewport_w > 0 ? viewport_w : 1;
    m->height = viewport_h > 0 ? viewport_h : 1;
    m->dpi = 96;
    m->screen = 1;
    m->scripting = 1;
    if (rt && rt->is_gui) {
        gui_settings gs;

        gui_settings_get(&gs);
        m->prefers_dark = gs.light ? 0 : 1;
    }
}

static BROWSER_NOINLINE int runtime_init(struct browser_runtime *rt, int is_gui,
                                         char *err, unsigned long errsz)
{
    struct css_media media;
    struct x509_store *roots;
    int rc;

    memset(rt, 0, sizeof(*rt));
    rt->is_gui = is_gui;
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

    page_fill_media(rt, WIN_W - SB_W, WIN_H - BAR_H - STAT_H, &media);
    rt->ua = css_parse(css_ua_stylesheet(), css_ua_stylesheet_len(),
                       CSS_ORIGIN_UA, &media);
    if (!rt->ua) {
        snprintf(err, errsz, "out of memory parsing the browser stylesheet");
        runtime_destroy(rt);
        return -1;
    }
    return 0;
}

static void tab_console_log(struct browser_tab *tab, const char *text)
{
    int idx;
    unsigned long len;
    char *copy;

    if (!tab)
        return;
    if (!text)
        text = "";
    len = strlen(text);
    if (len > 511)
        len = 511;
    copy = (char *)malloc(len + 1);
    if (!copy)
        return;
    memcpy(copy, text, len);
    copy[len] = 0;
    if (tab->console_count < CONSOLE_LOG_MAX) {
        idx = (tab->console_head + tab->console_count) % CONSOLE_LOG_MAX;
        tab->console_count++;
    } else {
        idx = tab->console_head;
        free(tab->console_lines[idx]);
        tab->console_head = (tab->console_head + 1) % CONSOLE_LOG_MAX;
    }
    tab->console_lines[idx] = copy;
}

static void tab_net_log(struct browser_tab *tab, int status, const char *kind,
                        unsigned long bytes, const char *url)
{
    int idx;
    struct net_log_entry *e;

    if (!tab)
        return;
    if (tab->net_count < NET_LOG_MAX) {
        idx = (tab->net_head + tab->net_count) % NET_LOG_MAX;
        tab->net_count++;
    } else {
        idx = tab->net_head;
        tab->net_head = (tab->net_head + 1) % NET_LOG_MAX;
    }
    e = &tab->net_entries[idx];
    e->status = status;
    e->bytes = bytes;
    snprintf(e->kind, sizeof(e->kind), "%s", kind ? kind : "res");
    snprintf(e->url, sizeof(e->url), "%.*s",
             (int)(sizeof(e->url) - 1), url ? url : "");
}

/* ------------------------------------------------------------------ *
 * Input and generated local documents
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

static int buf_add_str(struct bytebuf *b, const char *s)
{
    return buf_add(b, s, strlen(s));
}

static int buf_add_escaped_html(struct bytebuf *b, const char *s,
                                unsigned long n)
{
    unsigned long i, lit = 0;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *rep = 0;
        char dot = '.';

        if (c == '&')
            rep = "&amp;";
        else if (c == '<')
            rep = "&lt;";
        else if (c == '>')
            rep = "&gt;";
        else if (c == '"')
            rep = "&quot;";
        else if (c < 32 && c != '\n' && c != '\t' && c != '\r')
            rep = &dot;
        if (rep) {
            if (i > lit && buf_add(b, s + lit, i - lit) != 0)
                return -1;
            if (buf_add(b, rep, rep == &dot ? 1 : strlen(rep)) != 0)
                return -1;
            lit = i + 1;
        }
    }
    return n > lit ? buf_add(b, s + lit, n - lit) : 0;
}

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

static int url_is_markdown(const char *url)
{
    const char *end;
    unsigned long len;

    if (!url)
        return 0;
    end = url;
    while (*end && *end != '?' && *end != '#')
        end++;
    len = (unsigned long)(end - url);
    return len >= 3 && url[len - 3] == '.' &&
           ascii_lower((unsigned char)url[len - 2]) == 'm' &&
           ascii_lower((unsigned char)url[len - 1]) == 'd';
}

static int md_link_safe(const char *url, unsigned long len)
{
    unsigned long i = 0;

    while (i < len && (url[i] == ' ' || url[i] == '\t'))
        i++;
    if (len - i >= 11 &&
        ascii_lower((unsigned char)url[i]) == 'j' &&
        ascii_lower((unsigned char)url[i + 1]) == 'a' &&
        ascii_lower((unsigned char)url[i + 2]) == 'v' &&
        ascii_lower((unsigned char)url[i + 3]) == 'a' &&
        ascii_lower((unsigned char)url[i + 4]) == 's' &&
        ascii_lower((unsigned char)url[i + 5]) == 'c' &&
        ascii_lower((unsigned char)url[i + 6]) == 'r' &&
        ascii_lower((unsigned char)url[i + 7]) == 'i' &&
        ascii_lower((unsigned char)url[i + 8]) == 'p' &&
        ascii_lower((unsigned char)url[i + 9]) == 't' &&
        url[i + 10] == ':')
        return 0;
    return 1;
}

static int md_emit_inline(struct bytebuf *out, const char *s, unsigned long n)
{
    unsigned long i = 0;

    while (i < n) {
        if (s[i] == '`') {
            unsigned long close = i + 1;
            while (close < n && s[close] != '`')
                close++;
            if (close < n) {
                if (buf_add_str(out, "<code>") != 0 ||
                    buf_add_escaped_html(out, s + i + 1, close - i - 1) != 0 ||
                    buf_add_str(out, "</code>") != 0)
                    return -1;
                i = close + 1;
                continue;
            }
        }
        if (s[i] == '[') {
            unsigned long rb = i + 1;
            while (rb < n && s[rb] != ']' && s[rb] != '[')
                rb++;
            if (rb < n && s[rb] == ']' && rb + 1 < n && s[rb + 1] == '(') {
                unsigned long rp = rb + 2;
                while (rp < n && s[rp] != ')')
                    rp++;
                if (rp < n && rp > rb + 2 &&
                    md_link_safe(s + rb + 2, rp - rb - 2)) {
                    if (buf_add_str(out, "<a href=\"") != 0 ||
                        buf_add_escaped_html(out, s + rb + 2, rp - rb - 2) != 0 ||
                        buf_add_str(out, "\">") != 0 ||
                        buf_add_escaped_html(out, s + i + 1, rb - i - 1) != 0 ||
                        buf_add_str(out, "</a>") != 0)
                        return -1;
                    i = rp + 1;
                    continue;
                }
            }
        }
        if (i + 1 < n && s[i] == '*' && s[i + 1] == '*') {
            unsigned long close = i + 2;
            while (close + 1 < n && !(s[close] == '*' && s[close + 1] == '*'))
                close++;
            if (close + 1 < n && close > i + 2) {
                if (buf_add_str(out, "<b>") != 0 ||
                    buf_add_escaped_html(out, s + i + 2, close - i - 2) != 0 ||
                    buf_add_str(out, "</b>") != 0)
                    return -1;
                i = close + 2;
                continue;
            }
        }
        if (s[i] == '*' && i + 1 < n && s[i + 1] != ' ' && s[i + 1] != '*') {
            unsigned long close = i + 1;
            while (close < n && s[close] != '*')
                close++;
            if (close < n && close > i + 1) {
                if (buf_add_str(out, "<i>") != 0 ||
                    buf_add_escaped_html(out, s + i + 1, close - i - 1) != 0 ||
                    buf_add_str(out, "</i>") != 0)
                    return -1;
                i = close + 1;
                continue;
            }
        }
        if (buf_add_escaped_html(out, s + i, 1) != 0)
            return -1;
        i++;
    }
    return 0;
}

static int md_is_rule(const char *s, unsigned long n)
{
    char ch;
    unsigned long i = 0, count = 0;

    while (i < n && (s[i] == ' ' || s[i] == '\t'))
        i++;
    if (i >= n || (s[i] != '-' && s[i] != '*' && s[i] != '_'))
        return 0;
    ch = s[i];
    for (; i < n; i++) {
        if (s[i] == ch)
            count++;
        else if (s[i] != ' ' && s[i] != '\t')
            return 0;
    }
    return count >= 3;
}

static BROWSER_NOINLINE char *
make_markdown_source(const char *md, unsigned long len, int is_dark,
                     unsigned long *out_len)
{
    struct bytebuf out;
    unsigned long pos = 0;
    int in_code = 0, in_list = 0, in_para = 0;
    const char *title = "Markdown document";
    unsigned long title_len = 17;

    memset(&out, 0, sizeof(out));
    out.limit = DOM_MAX_INPUT;
    /* Scan first heading for <title>. */
    while (pos < len) {
        unsigned long ls = pos, le;
        while (pos < len && md[pos] != '\n')
            pos++;
        le = pos;
        if (pos < len)
            pos++;
        if (le > ls && md[le - 1] == '\r')
            le--;
        if (le > ls + 2 && md[ls] == '#' &&
            (md[ls + 1] == ' ' || md[ls + 1] == '\t')) {
            title = md + ls + 2;
            while (title < md + le && (*title == ' ' || *title == '\t'))
                title++;
            title_len = (unsigned long)(md + le - title);
            if (title_len > 120)
                title_len = 120;
            break;
        }
    }

    if (buf_add_str(&out, "<!doctype html><html><head><title>") != 0 ||
        buf_add_escaped_html(&out, title, title_len) != 0 ||
        buf_add_str(&out, "</title><style>") != 0 ||
        buf_add_str(&out, is_dark
            ? "body{font-family:sans-serif;line-height:1.5;max-width:740px;"
              "margin:16px auto;padding:0 16px;color:#e2e8f0;background:#111a28}"
              "h1,h2,h3{color:#f8fafc;margin:1em 0 0.4em}"
              "pre{background:#0b111b;padding:10px;border:1px solid #334155}"
              "code{font-family:monospace;background:#1e293b;padding:1px 3px}"
              "pre code{background:transparent;padding:0}"
              "blockquote{border-left:3px solid #60a5fa;margin:0.8em 0;"
              "padding:2px 12px;color:#94a3b8}a{color:#60a5fa}"
            : "body{font-family:sans-serif;line-height:1.5;max-width:740px;"
              "margin:16px auto;padding:0 16px;color:#1b2432;background:#ffffff}"
              "h1,h2,h3{color:#0f172a;margin:1em 0 0.4em}"
              "pre{background:#f1f5f9;padding:10px;border:1px solid #cbd5e1}"
              "code{font-family:monospace;background:#f1f5f9;padding:1px 3px}"
              "pre code{background:transparent;padding:0}"
              "blockquote{border-left:3px solid #64748b;margin:0.8em 0;"
              "padding:2px 12px;color:#475569}a{color:#1d4ed8}") != 0 ||
        buf_add_str(&out, "</style></head><body>\n") != 0) {
        free(out.p);
        return 0;
    }

    pos = 0;
    while (pos < len) {
        unsigned long ls = pos, le, indent = 0;
        const char *line;
        unsigned long llen;

        while (pos < len && md[pos] != '\n')
            pos++;
        le = pos;
        if (pos < len)
            pos++;
        if (le > ls && md[le - 1] == '\r')
            le--;
        line = md + ls;
        llen = le - ls;

        while (indent < llen && indent < 3 && line[indent] == ' ')
            indent++;
        if (llen - indent >= 3 && line[indent] == '`' &&
            line[indent + 1] == '`' && line[indent + 2] == '`') {
            if (in_list) { buf_add_str(&out, "</ul>\n"); in_list = 0; }
            if (in_para) { buf_add_str(&out, "</p>\n"); in_para = 0; }
            if (!in_code) {
                if (buf_add_str(&out, "<pre><code>") != 0) goto fail;
                in_code = 1;
            } else {
                if (buf_add_str(&out, "</code></pre>\n") != 0) goto fail;
                in_code = 0;
            }
            continue;
        }
        if (in_code) {
            if (buf_add_escaped_html(&out, line, llen) != 0 ||
                buf_add_str(&out, "\n") != 0)
                goto fail;
            continue;
        }

        {
            unsigned long ws = 0;
            while (ws < llen && (line[ws] == ' ' || line[ws] == '\t'))
                ws++;
            if (ws == llen) {
                if (in_list) { buf_add_str(&out, "</ul>\n"); in_list = 0; }
                if (in_para) { buf_add_str(&out, "</p>\n"); in_para = 0; }
                continue;
            }
        }

        if (llen > 1 && line[0] == '#') {
            unsigned long h = 0;
            char tag[8];
            while (h < llen && h < 6 && line[h] == '#')
                h++;
            if (h < llen && (line[h] == ' ' || line[h] == '\t')) {
                unsigned long ts = h + 1;
                while (ts < llen && (line[ts] == ' ' || line[ts] == '\t'))
                    ts++;
                if (in_list) { buf_add_str(&out, "</ul>\n"); in_list = 0; }
                if (in_para) { buf_add_str(&out, "</p>\n"); in_para = 0; }
                snprintf(tag, sizeof(tag), "<h%lu>", h);
                if (buf_add_str(&out, tag) != 0 ||
                    md_emit_inline(&out, line + ts, llen - ts) != 0)
                    goto fail;
                snprintf(tag, sizeof(tag), "</h%lu>\n", h);
                if (buf_add_str(&out, tag) != 0)
                    goto fail;
                continue;
            }
        }

        if (md_is_rule(line, llen)) {
            if (in_list) { buf_add_str(&out, "</ul>\n"); in_list = 0; }
            if (in_para) { buf_add_str(&out, "</p>\n"); in_para = 0; }
            if (buf_add_str(&out, "<hr>\n") != 0)
                goto fail;
            continue;
        }

        {
            unsigned long li = 0;
            while (li < llen && (line[li] == ' ' || line[li] == '\t'))
                li++;
            if (li + 1 < llen && (line[li] == '-' || line[li] == '*') &&
                (line[li + 1] == ' ' || line[li + 1] == '\t')) {
                unsigned long ts = li + 2;
                while (ts < llen && (line[ts] == ' ' || line[ts] == '\t'))
                    ts++;
                if (in_para) { buf_add_str(&out, "</p>\n"); in_para = 0; }
                if (!in_list) {
                    if (buf_add_str(&out, "<ul>\n") != 0) goto fail;
                    in_list = 1;
                }
                if (buf_add_str(&out, "<li>") != 0 ||
                    md_emit_inline(&out, line + ts, llen - ts) != 0 ||
                    buf_add_str(&out, "</li>\n") != 0)
                    goto fail;
                continue;
            }
        }

        if (line[0] == '>') {
            unsigned long qs = 1;
            if (qs < llen && line[qs] == ' ')
                qs++;
            if (in_list) { buf_add_str(&out, "</ul>\n"); in_list = 0; }
            if (in_para) { buf_add_str(&out, "</p>\n"); in_para = 0; }
            if (buf_add_str(&out, "<blockquote>") != 0 ||
                md_emit_inline(&out, line + qs, llen - qs) != 0 ||
                buf_add_str(&out, "</blockquote>\n") != 0)
                goto fail;
            continue;
        }

        if (in_list) {
            if (buf_add_str(&out, "</ul>\n") != 0) goto fail;
            in_list = 0;
        }
        if (!in_para) {
            if (buf_add_str(&out, "<p>") != 0) goto fail;
            in_para = 1;
        } else {
            if (buf_add_str(&out, " ") != 0) goto fail;
        }
        if (md_emit_inline(&out, line, llen) != 0)
            goto fail;
    }
    if (in_code && buf_add_str(&out, "</code></pre>\n") != 0) goto fail;
    if (in_list && buf_add_str(&out, "</ul>\n") != 0) goto fail;
    if (in_para && buf_add_str(&out, "</p>\n") != 0) goto fail;
    if (buf_add_str(&out, "</body></html>") != 0) goto fail;
    *out_len = out.n;
    return out.p;

fail:
    free(out.p);
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
make_view_source_source(const char *url, const char *body, unsigned long len,
                        int status, int is_dark, unsigned long *out_len)
{
    struct bytebuf out;
    unsigned long pos = 0, lines = 0, ln = 0;
    char meta[96], lnum[48];

    if (out_len)
        *out_len = 0;
    if (len == 0) {
        lines = 1;
    } else {
        while (pos < len) {
            lines++;
            while (pos < len && body[pos] != '\n')
                pos++;
            if (pos < len)
                pos++;
        }
    }

    memset(&out, 0, sizeof(out));
    out.limit = DOM_MAX_INPUT;
    if (buf_add_str(&out, "<!doctype html><html><head><title>view-source: ") != 0 ||
        buf_add_escaped_html(&out, url ? url : "", url ? strlen(url) : 0) != 0 ||
        buf_add_str(&out, "</title><style>") != 0 ||
        buf_add_str(&out, is_dark
            ? "body{margin:12px;background:#0b1220;color:#e2e8f0;font-family:sans-serif;}"
              ".vs-banner{padding:10px 14px;margin-bottom:10px;background:#132238;"
              "border:1px solid #2b4c76;border-left:4px solid #38bdf8;color:#e2e8f0;}"
              ".vs-badge{display:inline-block;padding:2px 8px;margin-right:8px;"
              "background:#1d4ed8;color:#ffffff;font-family:monospace;font-weight:bold;}"
              ".vs-url{font-family:monospace;font-weight:bold;color:#93c5fd;}"
              ".vs-meta{margin-top:4px;font-size:12px;color:#94a3b8;}"
              "pre.vs-code{margin:0;padding:10px 12px;background:#0f172a;color:#e2e8f0;"
              "border:1px solid #273854;font-family:monospace;white-space:pre-wrap;"
              "line-height:1.45;}"
              ".vs-ln{color:#64748b;}"
            : "body{margin:12px;background:#f8fafc;color:#0f172a;font-family:sans-serif;}"
              ".vs-banner{padding:10px 14px;margin-bottom:10px;background:#0f2744;"
              "border:1px solid #1e3a5f;border-left:4px solid #38bdf8;color:#f1f5f9;}"
              ".vs-badge{display:inline-block;padding:2px 8px;margin-right:8px;"
              "background:#0284c7;color:#ffffff;font-family:monospace;font-weight:bold;}"
              ".vs-url{font-family:monospace;font-weight:bold;color:#bae6fd;}"
              ".vs-meta{margin-top:4px;font-size:12px;color:#cbd5e1;}"
              "pre.vs-code{margin:0;padding:10px 12px;background:#ffffff;color:#0f172a;"
              "border:1px solid #cbd5e1;font-family:monospace;white-space:pre-wrap;"
              "line-height:1.45;}"
              ".vs-ln{color:#64748b;}") != 0 ||
        buf_add_str(&out, "</style></head><body>"
                          "<div class=\"vs-banner\">"
                          "<span class=\"vs-badge\">VIEW SOURCE</span>"
                          "<span class=\"vs-url\">") != 0 ||
        buf_add_escaped_html(&out, url ? url : "", url ? strlen(url) : 0) != 0)
        goto fail;

    if (status)
        snprintf(meta, sizeof(meta), "%lu bytes | %lu lines | HTTP %d",
                 len, lines, status);
    else
        snprintf(meta, sizeof(meta), "%lu bytes | %lu lines | local",
                 len, lines);

    if (buf_add_str(&out, "</span><div class=\"vs-meta\">") != 0 ||
        buf_add_escaped_html(&out, meta, strlen(meta)) != 0 ||
        buf_add_str(&out, "</div></div><pre class=\"vs-code\">") != 0)
        goto fail;

    if (len == 0) {
        if (buf_add_str(&out, "<span class=\"vs-ln\">   1 | </span>\n") != 0)
            goto fail;
    } else {
        pos = 0;
        while (pos < len) {
            unsigned long ls = pos, le;
            while (pos < len && body[pos] != '\n')
                pos++;
            le = pos;
            if (pos < len)
                pos++;
            if (le > ls && body[le - 1] == '\r')
                le--;
            ln++;
            snprintf(lnum, sizeof(lnum), "<span class=\"vs-ln\">%4lu | </span>", ln);
            if (buf_add_str(&out, lnum) != 0 ||
                buf_add_escaped_html(&out, body + ls, le - ls) != 0 ||
                buf_add_str(&out, "\n") != 0)
                goto fail;
        }
    }

    if (buf_add_str(&out, "</pre></body></html>") != 0)
        goto fail;
    if (out_len)
        *out_len = out.n;
    return out.p;

fail:
    free(out.p);
    return 0;
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

static const char *accept_to_net_kind(const char *accept)
{
    if (accept && str_ci_contains(accept, "text/css"))
        return "css";
    if (accept && str_ci_contains(accept, "image/"))
        return "img";
    if (accept && str_ci_contains(accept, "javascript"))
        return "script";
    return "res";
}

static BROWSER_NOINLINE int
resource_fetch(struct browser_page *p, const char *base, const char *ref,
               unsigned long max, const char *accept,
               struct resource_body *out, char *err, unsigned long errsz)
{
    char absolute[URL_MAX];
    const char *kind = accept_to_net_kind(accept);
    int net_status = 200;

    memset(out, 0, sizeof(*out));
    if (p->resources >= RESOURCE_COUNT_MAX) {
        snprintf(err, errsz, "page resource-count limit reached");
        return -1;
    }
    if (resource_url(p, base, ref, absolute, sizeof(absolute), err, errsz) != 0)
        return -1;
    if (url_is_scheme(absolute, "file")) {
        if (read_local_url(absolute, &out->data, &out->len, err, errsz) != 0) {
            tab_net_log(p->tab, -1, kind, 0, absolute);
            return -1;
        }
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
            tab_net_log(p->tab, rc, kind, 0, absolute);
            return -1;
        }
        net_status = res.status;
        if (res.status < 200 || res.status >= 300) {
            tab_net_log(p->tab, res.status, kind, res.body_len,
                        res.final_url ? res.final_url : absolute);
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
    tab_net_log(p->tab, net_status, kind, out->len, out->url);
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
                       "image/png,image/jpeg,image/gif,image/bmp,"
                       "image/svg+xml,image/x-icon,*/*;q=0.5",
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
    page_fill_media(p->runtime,
                    p->viewport_w ? p->viewport_w : WIN_W - SB_W,
                    p->viewport_h ? p->viewport_h : WIN_H - BAR_H - STAT_H,
                    &env);
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

    if (p && p->tab)
        tab_console_log(p->tab, text ? text : "");
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

struct script_fetch_work {
    char absolute[URL_MAX];
    struct url page_url;
    struct url request_url;
    char page_origin[URL_MAX];
    char current[URL_MAX];
    char origin_header[URL_MAX + 16];
    char next[URL_MAX];
    struct http_request req;
    struct http_response res;
};

static BROWSER_NOINLINE int
script_fetch(void *user, const char *url, const char *method,
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
    struct script_fetch_work *fw;

    memset(out, 0, sizeof(*out));
    if (p->resources >= RESOURCE_COUNT_MAX) {
        snprintf(err, errsz, "page resource-count limit reached");
        return -1;
    }
    fw = (struct script_fetch_work *)calloc(1, sizeof(*fw));
    if (!fw) {
        snprintf(err, errsz, "out of memory starting fetch");
        return -1;
    }
    if (body_len > JS_FETCH_MAX ||
        resource_url(p, p->base_url, url, fw->absolute, sizeof(fw->absolute),
                     err, errsz) != 0)
        goto fail;
    if (url_is_scheme(fw->absolute, "file")) {
        if (strcmp(method, "GET") && strcmp(method, "HEAD")) {
            snprintf(err, errsz, "local fetch supports GET and HEAD only");
            goto fail;
        }
        if (read_local_url(fw->absolute, &out->body, &out->body_len,
                           err, errsz) != 0) {
            tab_net_log(p->tab, -1, "fetch", 0, fw->absolute);
            goto fail;
        }
        if (!strcmp(method, "HEAD")) {
            free(out->body);
            out->body = str_dup("");
            out->body_len = 0;
        }
        out->status = 200;
        out->status_text = str_dup("OK");
        out->url = str_dup(fw->absolute);
        out->content_type = str_dup("application/octet-stream");
    } else {
        const char *send_method = method;
        const void *send_body = body;
        unsigned long send_body_len = body_len;
        const char *content_type;
        int page_has_origin, redirects;

        if (!script_fetch_method_allowed(method)) {
            snprintf(err, errsz,
                     "fetch request method is not supported");
            goto fail;
        }
        page_has_origin =
            url_parse(p->url, &fw->page_url) == URL_OK &&
            fw->page_url.has_authority &&
            (str_ci_eq(fw->page_url.scheme, "http") ||
             str_ci_eq(fw->page_url.scheme, "https")) &&
            url_origin(&fw->page_url, fw->page_origin,
                       sizeof(fw->page_origin)) == URL_OK;
        if (!page_has_origin)
            strcpy(fw->page_origin, "null");
        strcpy(fw->current, fw->absolute);

        for (redirects = 0; ; redirects++) {
            int cross_origin, rc;

            if (url_parse(fw->current, &fw->request_url) != URL_OK ||
                !fw->request_url.has_authority) {
                snprintf(err, errsz, "invalid fetch URL");
                goto fail;
            }
            cross_origin = !page_has_origin ||
                           !url_same_origin(&fw->page_url, &fw->request_url);
            if (cross_origin &&
                !strcmp(request_mode, "same-origin")) {
                snprintf(err, errsz,
                         "cross-origin request blocked by same-origin mode");
                goto fail;
            }
            if (cross_origin && !strcmp(request_mode, "no-cors")) {
                snprintf(err, errsz,
                         "opaque no-cors responses are not implemented");
                goto fail;
            }
            if (cross_origin &&
                ((request_headers && *request_headers) ||
                 !script_fetch_simple_content_type(
                     request_content_type))) {
                snprintf(err, errsz,
                         "cross-origin request requires CORS preflight");
                goto fail;
            }
            memset(&fw->req, 0, sizeof(fw->req));
            memset(&fw->res, 0, sizeof(fw->res));
            fw->req.method = send_method;
            fw->req.url = fw->current;
            fw->req.body = send_body_len ? send_body : 0;
            fw->req.body_len = send_body_len;
            fw->req.content_type = send_body_len
                ? (request_content_type
                   ? request_content_type : "text/plain;charset=UTF-8")
                : 0;
            fw->req.accept = request_accept ? request_accept : "*/*";
            fw->req.extra_headers = request_headers;
            fw->req.flags = HTTP_F_NO_REDIRECT;
            if (!strcmp(request_credentials, "omit") ||
                (cross_origin &&
                 !strcmp(request_credentials, "same-origin")))
                fw->req.flags |= HTTP_F_NO_COOKIES;
            if (cross_origin) {
                snprintf(fw->origin_header, sizeof(fw->origin_header),
                         "Origin: %s\r\n", fw->page_origin);
                fw->req.extra_headers = fw->origin_header;
            }
            fw->req.max_body = JS_FETCH_MAX;
            rc = http_fetch(p->runtime->http, &fw->req, &fw->res);
            if (rc != HTTP_OK) {
                snprintf(err, errsz, "%s", http_error_text(rc));
                tab_net_log(p->tab, rc, "fetch", 0, fw->current);
                goto fail;
            }
            if (cross_origin) {
                const char *allow_origin =
                    http_header_get(&fw->res,
                                    "Access-Control-Allow-Origin");
                const char *allow_credentials =
                    http_header_get(&fw->res,
                                    "Access-Control-Allow-Credentials");
                int cors_allowed =
                    script_fetch_cors_allows(&fw->res, fw->page_origin);

                if (!strcmp(request_credentials, "include"))
                    cors_allowed = allow_origin &&
                        !strcmp(allow_origin, fw->page_origin) &&
                        allow_credentials &&
                        str_ci_eq(allow_credentials, "true");
                if (!cors_allowed) {
                    tab_net_log(p->tab, fw->res.status, "fetch",
                                fw->res.body_len, fw->current);
                    snprintf(err, errsz,
                             "cross-origin response did not allow %s",
                             fw->page_origin);
                    http_response_free(&fw->res);
                    goto fail;
                }
            }
            if (script_fetch_redirect(fw->res.status)) {
                const char *location = http_header_get(&fw->res, "Location");

                if (!location || !*location) {
                    /* A redirect status without Location is an ordinary
                     * Response, as required by the Fetch redirect rules. */
                } else if (!strcmp(request_redirect, "error")) {
                    snprintf(err, errsz,
                             "redirect blocked by Request policy");
                    http_response_free(&fw->res);
                    goto fail;
                } else if (!strcmp(request_redirect, "manual")) {
                    /* Return the redirect response without following it.
                     * The current response surface does not yet expose the
                     * filtered Location header of an opaqueredirect. */
                } else if (redirects >= 8) {
                    snprintf(err, errsz, "too many fetch redirects");
                    http_response_free(&fw->res);
                    goto fail;
                } else if (resource_url(p, fw->current, location, fw->next,
                                        sizeof(fw->next), err, errsz) != 0) {
                    http_response_free(&fw->res);
                    goto fail;
                } else {
                    if (fw->res.status == 303 ||
                        ((fw->res.status == 301 || fw->res.status == 302) &&
                         !strcmp(send_method, "POST"))) {
                        send_method = "GET";
                        send_body = 0;
                        send_body_len = 0;
                    }
                    tab_net_log(p->tab, fw->res.status, "fetch", 0, fw->current);
                    http_response_free(&fw->res);
                    strcpy(fw->current, fw->next);
                    continue;
                }
            }
            if (fw->res.body_len > RESOURCE_TOTAL_MAX - p->resource_bytes) {
                snprintf(err, errsz,
                         "page subresource byte limit reached");
                http_response_free(&fw->res);
                goto fail;
            }
            content_type = http_header_get(&fw->res, "Content-Type");
            out->status = fw->res.status;
            out->status_text = str_dup(fw->res.reason ? fw->res.reason :
                                       http_reason_phrase(fw->res.status));
            out->url = str_dup(fw->current);
            out->content_type = str_dup(content_type ? content_type : "");
            out->redirected = redirects > 0;
            out->body = (char *)malloc(fw->res.body_len + 1);
            if (out->body) {
                if (fw->res.body_len)
                    memcpy(out->body, fw->res.body, fw->res.body_len);
                out->body[fw->res.body_len] = 0;
                out->body_len = fw->res.body_len;
            }
            http_response_free(&fw->res);
            break;
        }
    }
    if (!out->status_text || !out->url || !out->content_type ||
        !out->body) {
        snprintf(err, errsz, "out of memory creating fetch response");
        script_fetch_result_free(out);
        goto fail;
    }
    if (out->body_len > RESOURCE_TOTAL_MAX - p->resource_bytes) {
        snprintf(err, errsz, "page subresource byte limit reached");
        script_fetch_result_free(out);
        goto fail;
    }
    p->resource_bytes += out->body_len;
    p->resources++;
    tab_net_log(p->tab, out->status, "fetch", out->body_len, out->url);
    free(fw);
    return 0;

fail:
    free(fw);
    return -1;
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
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        p++;
    return p;
}

static const char *module_trim_end(const char *p, const char *end)
{
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n' ||
                       end[-1] == ';'))
        end--;
    return end;
}

static const char *module_skip_decl(const char *p, const char *end)
{
    for (;;) {
        p = module_skip(p, end);
        if (p + 1 < end && p[0] == '/' && p[1] == '/') {
            p += 2;
            while (p < end && *p != '\n')
                p++;
            continue;
        }
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
                p++;
            p = (p + 1 < end) ? p + 2 : end;
            continue;
        }
        return p;
    }
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

static int module_read_ident_decl(const char **pp, const char *end,
                                  char *out, unsigned long outsz)
{
    const char *p = module_skip_decl(*pp, end);

    *pp = p;
    return module_read_ident(pp, end, out, outsz);
}

static const char *module_find_from(const char *p, const char *end)
{
    const char *start = p;
    int depth = 0, quote = 0;

    while (p < end) {
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
        if (p + 1 < end && p[0] == '/' && p[1] == '/') {
            while (p < end && *p != '\n')
                p++;
            continue;
        }
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
                p++;
            p = (p + 1 < end) ? p + 2 : end;
            continue;
        }
        if (*p == '{') {
            depth++;
            p++;
            continue;
        }
        if (*p == '}' && depth > 0) {
            depth--;
            p++;
            continue;
        }
        if (depth == 0 && p + 4 <= end && !memcmp(p, "from", 4) &&
            (p == start || !module_ident_part((unsigned char)p[-1])) &&
            (p + 4 == end || !module_ident_part((unsigned char)p[4])))
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
    const char *q = p, *close;
    int quote;
    unsigned long n;

    while (q < end && *q != '\'' && *q != '"') {
        if (q + 1 < end && q[0] == '/' && q[1] == '/') {
            while (q < end && *q != '\n')
                q++;
            continue;
        }
        if (q + 1 < end && q[0] == '/' && q[1] == '*') {
            q += 2;
            while (q + 1 < end && !(q[0] == '*' && q[1] == '/'))
                q++;
            q = (q + 1 < end) ? q + 2 : end;
            continue;
        }
        q++;
    }
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
    p = module_skip_decl(p, end);
    if (p >= end || *p != '{')
        return -1;
    p++;
    while (p < end) {
        char imported[128], local[128];

        p = module_skip_decl(p, end);
        if (p < end && *p == '}')
            return 0;
        if (module_read_ident_decl(&p, end, imported, sizeof(imported)) != 0)
            break;
        snprintf(local, sizeof(local), "%s", imported);
        p = module_skip_decl(p, end);
        if (module_word(p, end, "as")) {
            p += 2;
            if (module_read_ident_decl(&p, end, local, sizeof(local)) != 0)
                break;
        }
        if (module_emit_binding(out, local, dep, imported) != 0)
            return -1;
        p = module_skip_decl(p, end);
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

    from = module_find_from(p + 6, end);
    if (module_specifier(from ? from + 4 : p + 6, end,
                         spec, sizeof(spec), &quote_at) != 0) {
        snprintf(err, errsz, "module import lacks a simple string specifier");
        return -1;
    }
    dep = module_external(l, owner->base_url, spec, err, errsz);
    if (!dep || module_eval(l, dep, depth + 1, err, errsz) != 0)
        return -1;
    clause = module_skip_decl(p + 6, quote_at);
    if (!from)
        return 0;                       /* side-effect import */
    clause_end = module_trim_end(clause, from);
    q = module_skip_decl(clause, clause_end);
    if (q >= clause_end)
        return 0;
    if (*q != '{' && *q != '*') {
        if (module_read_ident_decl(&q, clause_end, local, sizeof(local)) != 0 ||
            module_emit_binding(out, local, dep->index, "default") != 0)
            return -1;
        q = module_skip_decl(q, clause_end);
        if (q < clause_end && *q == ',')
            q = module_skip_decl(q + 1, clause_end);
        else
            return q == clause_end ? 0 : -1;
    }
    if (q < clause_end && *q == '*') {
        q = module_skip_decl(q + 1, clause_end);
        if (!module_word(q, clause_end, "as"))
            return -1;
        q += 2;
        if (module_read_ident_decl(&q, clause_end, local, sizeof(local)) != 0)
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

    for (close = p + 1; close < end && *close != '}'; ) {
        if (close + 1 < end && close[0] == '/' && close[1] == '/') {
            while (close < end && *close != '\n')
                close++;
            continue;
        }
        if (close + 1 < end && close[0] == '/' && close[1] == '*') {
            close += 2;
            while (close + 1 < end && !(close[0] == '*' && close[1] == '/'))
                close++;
            close = (close + 1 < end) ? close + 2 : end;
            continue;
        }
        close++;
    }
    if (close >= end)
        return -1;
    after = module_skip_decl(close + 1, end);
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

        p = module_skip_decl(p, close);
        if (p >= close)
            break;
        if (module_read_ident_decl(&p, close, local, sizeof(local)) != 0)
            return -1;
        snprintf(exported, sizeof(exported), "%s", local);
        p = module_skip_decl(p, close);
        if (module_word(p, close, "as")) {
            p += 2;
            if (module_read_ident_decl(&p, close, exported,
                                       sizeof(exported)) != 0)
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
        p = module_skip_decl(p, close);
        if (p < close && *p == ',')
            p++;
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
           (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\r' || p[-1] == '\n'))
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
        if (*p == '/' && p + 1 < end && p[1] == '/') {
            while (p < end && *p != '\n')
                p++;
            continue;
        }
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

static void module_scan_stmt(const char *p, const char *src_end, int is_export,
                             const char **out_stmt_end, const char **out_resume)
{
    const char *s = p;
    int depth = 0, quote = 0, saw_close = 0, saw_spec = 0;
    int need_spec = !is_export;

    while (s < src_end) {
        if (quote) {
            if (*s == '\\' && s + 1 < src_end) {
                s += 2;
                continue;
            }
            if (*s == quote) {
                quote = 0;
                if (depth == 0)
                    saw_spec = 1;
            }
            s++;
            continue;
        }
        if (*s == '\'' || *s == '"') {
            quote = *s++;
            continue;
        }
        if (s + 1 < src_end && s[0] == '/' && s[1] == '/') {
            while (s < src_end && *s != '\n')
                s++;
            continue;
        }
        if (s + 1 < src_end && s[0] == '/' && s[1] == '*') {
            s += 2;
            while (s + 1 < src_end && !(s[0] == '*' && s[1] == '/'))
                s++;
            s = (s + 1 < src_end) ? s + 2 : src_end;
            continue;
        }
        if (*s == '{') {
            depth++;
            s++;
            continue;
        }
        if (*s == '}') {
            if (depth > 0)
                depth--;
            if (depth == 0) {
                saw_close = 1;
                if (is_export) {
                    const char *after = module_skip_decl(s + 1, src_end);
                    if (module_word(after, src_end, "from"))
                        need_spec = 1;
                }
            }
            s++;
            continue;
        }
        if (depth == 0) {
            if (*s == ';') {
                *out_stmt_end = s;
                *out_resume = s + 1;
                return;
            }
            if (*s == '\n' &&
                ((!need_spec && saw_close) || (need_spec && saw_spec))) {
                *out_stmt_end = s;
                *out_resume = s;
                return;
            }
        }
        s++;
    }
    *out_stmt_end = src_end;
    *out_resume = src_end;
}

static int module_decl_keyword(const char *q, const char *end, int default_only,
                               const char **name_at, int *is_class)
{
    *is_class = 0;
    if (module_word(q, end, "function")) {
        *name_at = q + 8;
        return 1;
    }
    if (module_word(q, end, "class")) {
        *name_at = q + 5;
        *is_class = 1;
        return 1;
    }
    if (module_word(q, end, "async")) {
        const char *fn_at = module_skip(q + 5, end);
        if (module_word(fn_at, end, "function")) {
            *name_at = fn_at + 8;
            return 1;
        }
    }
    if (default_only)
        return 0;
    if (module_word(q, end, "const")) {
        *name_at = q + 5;
        return 1;
    }
    if (module_word(q, end, "var") || module_word(q, end, "let")) {
        *name_at = q + 3;
        return 1;
    }
    return 0;
}

static BROWSER_NOINLINE int
module_transform(struct module_loader *l, struct browser_module *m, int depth,
                 struct bytebuf *out, char *err, unsigned long errsz)
{
    struct bytebuf body, trailer;
    const char *src_end = m->source + m->source_len;
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
        if (!started_in_comment && module_word(p, end, "import")) {
            const char *after_imp = module_skip(p + 6, src_end);

            if (after_imp < src_end && *after_imp != '(' &&
                *after_imp != '.') {
                const char *stmt_end, *resume;

                module_scan_stmt(p, src_end, 0, &stmt_end, &resume);
                if (module_import_line(l, m, p, stmt_end, &body, depth,
                                       err, errsz) != 0)
                    goto fail;
                if (buf_add(&body, "\n", 1) != 0)
                    goto oom;
                pos = (unsigned long)(resume - m->source);
                continue;
            }
        }
        if (!started_in_comment && module_word(p, end, "export")) {
            const char *q_multi = module_skip(p + 6, src_end);
            const char *q = module_skip(p + 6, end);

            if (q_multi < src_end && *q_multi == '{') {
                const char *stmt_end, *resume;

                module_scan_stmt(q_multi, src_end, 1, &stmt_end, &resume);
                if (module_export_list(l, m, q_multi, stmt_end, &body, depth,
                                       err, errsz) != 0)
                    goto fail;
                if (buf_add(&body, "\n", 1) != 0)
                    goto oom;
                pos = (unsigned long)(resume - m->source);
                continue;
            }
            if (module_word(q, end, "default")) {
                const char *value = module_skip(q + 7, end);
                const char *name_at = 0;
                int is_class = 0, dynamic, named = 0;
                char name[128];

                if (module_decl_keyword(value, end, 1, &name_at, &is_class)) {
                    const char *scan = module_skip(name_at, end);

                    if (scan < end &&
                        module_ident_start((unsigned char)*scan) &&
                        !(is_class && module_word(scan, end, "extends"))) {
                        if (module_read_ident(&scan, end, name,
                                              sizeof(name)) != 0)
                            goto fail;
                        named = 1;
                    }
                }
                if (!named &&
                    buf_add(&body, "__exports[\"default\"]=", 21) != 0)
                    goto oom;
                dynamic = module_dynamic_line(
                    l, m, value, end, &body, depth, &block_comment,
                    err, errsz);
                if (dynamic == -1)
                    goto fail;
                if (dynamic == -2 || buf_add(&body, "\n", 1) != 0 ||
                    (named && module_emit_export(&trailer, name,
                                                 "default") != 0))
                    goto oom;
                continue;
            }
            {
                const char *name_at = 0;
                int is_class = 0;

                if (module_decl_keyword(q, end, 0, &name_at, &is_class)) {
                    char name[128];
                    int dynamic;

                    if (module_read_ident(&name_at, end, name,
                                          sizeof(name)) != 0 ||
                        (is_class && !strcmp(name, "extends"))) {
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

static BROWSER_NOINLINE int
module_eval(struct module_loader *l, struct browser_module *m,
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

static int script_get_box_rect(void *user, struct dom_node *node,
                               float *x, float *y, float *w, float *h)
{
    struct browser_page *p = (struct browser_page *)user;
    struct lay_box *b;
    lay_rect r;
    int sx = 0, sy = 0;

    if (!p || !p->layout || !node)
        return 0;
    b = lay_box_for_node(p->layout, node);
    if (!b)
        return 0;
    r = lay_border_rect(b);
    if (!(b->flags & (LAYF_FIXED | LAYF_FIXED_CTX))) {
        sx = p->scroll_x;
        sy = p->scroll_y;
        if (b->flags & (LAYF_STICKY | LAYF_STICKY_CTX)) {
            int32_t sdx = 0, sdy = 0;
            lay_sticky_offset(p->layout, b, sx, sy, &sdx, &sdy);
            r.x += sdx;
            r.y += sdy;
        }
    }
    if (x) *x = (float)(r.x - sx);
    if (y) *y = (float)(r.y - sy);
    if (w) *w = (float)r.w;
    if (h) *h = (float)r.h;
    return 1;
}

static void script_scroll_cb(void *user, double x, double y)
{
    struct browser_page *p = (struct browser_page *)user;
    int sy = (int)y;
    int max_y = 0;

    (void)x;
    if (!p)
        return;
    if (p->layout) {
        max_y = lay_height(p->layout) - p->viewport_h;
        if (max_y < 0)
            max_y = 0;
    }
    if (sy < 0)
        sy = 0;
    if (p->layout && sy > max_y)
        sy = max_y;
    p->scroll_x = 0;
    p->scroll_y = sy;
    if (p->tab)
        p->tab->scroll = sy;
}

static int script_clipboard_write(void *user, const char *text,
                                  unsigned long len)
{
    int n;

    (void)user;
    if (!text)
        return -1;
    if (len == 0)
        return 0;
    n = len > K_PARCEL_MAX_BYTES ? K_PARCEL_MAX_BYTES : (int)len;
    return gui_clipboard_copy(text, n, "Browser") >= 0 ? 0 : -1;
}

static char *script_clipboard_read(void *user, unsigned long *len_out)
{
    char *buf;
    int n;

    (void)user;
    if (len_out)
        *len_out = 0;
    buf = (char *)malloc(K_PARCEL_MAX_BYTES + 1);
    if (!buf)
        return 0;
    n = gui_clipboard_paste(buf, K_PARCEL_MAX_BYTES + 1);
    if (n <= 0) {
        free(buf);
        return 0;
    }
    if (len_out)
        *len_out = (unsigned long)n;
    return buf;
}

static void log_script_err(struct browser_page *p, const char *kind,
                           const char *name, const char *err)
{
    char line[384];

    if (!p || !p->tab)
        return;
    snprintf(line, sizeof(line), "[err] %s %.120s: %.180s",
             kind, name ? name : "script", err ? err : "error");
    tab_console_log(p->tab, line);
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
    cfg.get_box_rect = script_get_box_rect;
    cfg.scroll_cb = script_scroll_cb;
    cfg.clipboard_write = script_clipboard_write;
    cfg.clipboard_read = script_clipboard_read;
    cfg.stylesheet = p->author;
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
                log_script_err(p, "fetch", src, err);
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
            log_script_err(p, "script", name, err);
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
            log_script_err(p, "module", src && *src ? src : "inline", err);
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
            log_script_err(p, "timer", "pump", err);
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

static struct dom_node *g_hover_node;
static struct dom_node *g_active_node;
static struct dom_node *g_focus_node;
static unsigned (*g_base_dom_state)(void *e);

static unsigned browser_dom_state(void *e)
{
    struct dom_node *n = (struct dom_node *)e;
    struct dom_node *a;
    unsigned st = g_base_dom_state ? g_base_dom_state(e) : 0;

    if (!n)
        return st;
    if (n == g_focus_node)
        st |= CSS_STATE_FOCUS;
    for (a = g_hover_node; a; a = a->parent) {
        if (a == n) {
            st |= CSS_STATE_HOVER;
            break;
        }
    }
    for (a = g_active_node; a; a = a->parent) {
        if (a == n) {
            st |= CSS_STATE_ACTIVE;
            break;
        }
    }
    return st;
}

static int css_compound_has_dynamic(const struct css_compound *cp)
{
    int i;

    if (!cp)
        return 0;
    for (i = 0; i < cp->n; i++) {
        const struct css_simple *s = &cp->simples[i];

        if (s->kind != CSS_SIMPLE_PSEUDO)
            continue;
        if (s->pseudo == CSS_PSEUDO_HOVER ||
            s->pseudo == CSS_PSEUDO_ACTIVE)
            return 1;
        if (s->pseudo == CSS_PSEUDO_NOT && s->sub &&
            css_compound_has_dynamic(s->sub))
            return 1;
    }
    return 0;
}

static int css_sheet_has_dynamic_pseudo(const struct css_stylesheet *ss)
{
    int i, n;

    if (!ss)
        return 0;
    n = css_rule_count(ss);
    for (i = 0; i < n; i++) {
        const struct css_rule *r = css_rule_at(ss, i);
        int k;

        if (!r)
            continue;
        for (k = 0; k < r->sel.nparts; k++)
            if (css_compound_has_dynamic(&r->sel.parts[k]))
                return 1;
    }
    return 0;
}

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
    jsdom_free(p->js);
    p->js = 0;
    css_free(p->author);
    p->author = 0;
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

static int32_t zoom_scale_px(int32_t v, int zoom_pct)
{
    int64_t scaled;

    if (v == 0 || zoom_pct == 100 || zoom_pct <= 0)
        return v;
    if (v > 0) {
        scaled = ((int64_t)v * zoom_pct + 50) / 100;
        if (scaled < 1)
            scaled = 1;
    } else {
        scaled = -((-(int64_t)v * zoom_pct + 50) / 100);
        if (scaled > -1)
            scaled = -1;
    }
    return (int32_t)clamp_i64(scaled);
}

static void zoom_scale_len(struct css_len *l, int zoom_pct)
{
    if (!l)
        return;
    if (l->type == CSS_LEN_PX || l->type == CSS_LEN_CALC)
        l->v = zoom_scale_px(l->v, zoom_pct);
}

static BROWSER_NOINLINE void zoom_scale_dom_styles(struct dom_node *root,
                                                   int zoom_pct)
{
    struct dom_node *n;

    if (!root || zoom_pct == 100 || zoom_pct <= 0)
        return;
    for (n = root; n; n = dom_next(n)) {
        struct computed_style *cs;
        int i;

        if (n->type != DOM_ELEMENT || !n->style)
            continue;
        cs = (struct computed_style *)n->style;
        cs->font_size = zoom_scale_px(cs->font_size, zoom_pct);
        if (cs->font_size < 1)
            cs->font_size = 1;
        if (cs->font_size > 400)
            cs->font_size = 400;
        for (i = 0; i < 4; i++) {
            cs->border_width[i] = zoom_scale_px(cs->border_width[i], zoom_pct);
            cs->border_radius[i] = zoom_scale_px(cs->border_radius[i], zoom_pct);
            zoom_scale_len(&cs->margin[i], zoom_pct);
            zoom_scale_len(&cs->padding[i], zoom_pct);
            zoom_scale_len(&cs->offset[i], zoom_pct);
        }
        cs->border_spacing = zoom_scale_px(cs->border_spacing, zoom_pct);
        cs->outline_width = zoom_scale_px(cs->outline_width, zoom_pct);
        cs->outline_offset = zoom_scale_px(cs->outline_offset, zoom_pct);
        cs->vertical_align_px = zoom_scale_px(cs->vertical_align_px, zoom_pct);
        cs->gap = zoom_scale_px(cs->gap, zoom_pct);
        cs->row_gap = zoom_scale_px(cs->row_gap, zoom_pct);
        cs->column_gap = zoom_scale_px(cs->column_gap, zoom_pct);
        cs->letter_spacing = zoom_scale_px(cs->letter_spacing, zoom_pct);
        zoom_scale_len(&cs->width, zoom_pct);
        zoom_scale_len(&cs->height, zoom_pct);
        zoom_scale_len(&cs->min_width, zoom_pct);
        zoom_scale_len(&cs->max_width, zoom_pct);
        zoom_scale_len(&cs->min_height, zoom_pct);
        zoom_scale_len(&cs->max_height, zoom_pct);
        zoom_scale_len(&cs->flex_basis, zoom_pct);
        zoom_scale_len(&cs->line_height, zoom_pct);
        zoom_scale_len(&cs->text_indent, zoom_pct);
        zoom_scale_len(&cs->translate_x, zoom_pct);
        zoom_scale_len(&cs->translate_y, zoom_pct);
        if (cs->has_shadow) {
            cs->shadow_x = zoom_scale_px(cs->shadow_x, zoom_pct);
            cs->shadow_y = zoom_scale_px(cs->shadow_y, zoom_pct);
            cs->shadow_blur = zoom_scale_px(cs->shadow_blur, zoom_pct);
            cs->shadow_spread = zoom_scale_px(cs->shadow_spread, zoom_pct);
        }
        for (i = 0; i < (int)cs->grid_ncols && i < CSS_GRID_MAX_COLS; i++) {
            if (cs->grid_cols[i].kind == CSS_GRID_PX)
                cs->grid_cols[i].val =
                    zoom_scale_px(cs->grid_cols[i].val, zoom_pct);
        }
        for (i = 0; i < (int)cs->grid_nrows && i < CSS_GRID_MAX_ROWS; i++) {
            if (cs->grid_rows[i].kind == CSS_GRID_PX)
                cs->grid_rows[i].val =
                    zoom_scale_px(cs->grid_rows[i].val, zoom_pct);
        }
    }
}

static int page_zoom_pct(const struct browser_page *p)
{
    return (p && p->tab && p->tab->zoom_pct > 0) ? p->tab->zoom_pct : 100;
}

static void page_zoom_viewport(int viewport_w, int viewport_h, int zoom_pct,
                               int *eff_w, int *eff_h)
{
    int vw = viewport_w > 0 ? viewport_w : 1;
    int vh = viewport_h > 0 ? viewport_h : 1;

    if (zoom_pct != 100 && zoom_pct > 0) {
        vw = mul_div_i(vw, 100, zoom_pct);
        vh = mul_div_i(vh, 100, zoom_pct);
        if (vw < 1) vw = 1;
        if (vh < 1) vh = 1;
    }
    *eff_w = vw;
    *eff_h = vh;
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
    int styled, zoom_pct, eff_vw, eff_vh;

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

    p->viewport_w = viewport_w;
    p->viewport_h = viewport_h;
    zoom_pct = page_zoom_pct(p);
    page_zoom_viewport(viewport_w, viewport_h, zoom_pct, &eff_vw, &eff_vh);
    page_fill_media(rt, eff_vw, eff_vh, &media);
    css_set_media(rt->ua, &media);
    css_set_media(p->author, &media);
    sheets[0] = rt->ua;
    sheets[1] = p->author;
    if (rt && rt->is_gui) {
        struct css_elem_ops ops = *css_dom_ops();

        g_base_dom_state = ops.state;
        ops.state = browser_dom_state;
        p->styles = style_engine_new(sheets, 2, &ops);
    } else {
        p->styles = style_engine_new(sheets, 2, css_dom_ops());
    }
    if (!p->styles) {
        page_transition(p, PAGE_FAILED);
        snprintf(err, errsz, "out of memory creating the style engine");
        return -1;
    }
    style_engine_set_viewport(p->styles, eff_vw, eff_vh);
    styled = style_compute_tree(p->styles, p->doc->root, 0,
                                css_style_dom_sink, 0);
    if (styled <= 0) {
        page_transition(p, PAGE_FAILED);
        snprintf(err, errsz, "out of memory computing page styles");
        return -1;
    }
    if (zoom_pct != 100)
        zoom_scale_dom_styles(p->doc->root, zoom_pct);

    if (page_transition(p, PAGE_LAYOUT) != 0) {
        snprintf(err, errsz, "invalid page lifecycle transition to layout");
        return -1;
    }
    lay_opts_init(&lo, viewport_w, viewport_h);
    if (zoom_pct != 100) {
        lo.default_font_size = zoom_scale_px(16, zoom_pct);
        if (lo.default_font_size < 1)
            lo.default_font_size = 1;
    }
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
    int eff_vw, eff_vh;

    p->runtime = rt;
    p->viewport_w = viewport_w;
    p->viewport_h = viewport_h;
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
    page_zoom_viewport(viewport_w, viewport_h, page_zoom_pct(p),
                       &eff_vw, &eff_vh);
    page_fill_media(rt, eff_vw, eff_vh, &media);
    p->author = css_parse(css.p, css.n, CSS_ORIGIN_AUTHOR, &media);
    free(css.p);
    if (!p->author) {
        snprintf(err, errsz, "out of memory parsing page styles");
        return -1;
    }
    p->has_hover_css = css_sheet_has_dynamic_pseudo(p->author);
    if (page_reflow(rt, p, viewport_w, viewport_h, err, errsz) != 0)
        return -1;
    if (page_transition(p, PAGE_SCRIPTING) != 0) {
        snprintf(err, errsz, "invalid page lifecycle transition to scripting");
        return -1;
    }
    if (execute_page_scripts(p, eff_vw, eff_vh) != 0) {
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

struct download_info {
    int triggered;
    int ok;
    char path[256];
    unsigned long bytes;
};

static int is_renderable_mime(const char *ct)
{
    if (!ct || !*ct)
        return 1;
    return str_ci_contains(ct, "html") ||
           str_ci_contains(ct, "text/") ||
           str_ci_contains(ct, "image/") ||
           str_ci_contains(ct, "json") ||
           str_ci_contains(ct, "javascript") ||
           str_ci_contains(ct, "ecmascript") ||
           str_ci_contains(ct, "css") ||
           str_ci_contains(ct, "xml") ||
           str_ci_contains(ct, "markdown");
}

static int has_attachment_disposition(const char *cd)
{
    return cd && str_ci_contains(cd, "attachment");
}

static void sanitize_download_name(char *name, unsigned long cap)
{
    unsigned long i;

    for (i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '/' || c == '\\' || c < 32 || c >= 127)
            name[i] = '_';
    }
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, ".."))
        snprintf(name, cap, "download.bin");
}

static void extract_download_filename(const char *cd, const char *url,
                                      char *out, unsigned long outsz)
{
    out[0] = 0;
    if (cd) {
        const char *p = cd;
        while (*p) {
            if ((p == cd || p[-1] == ';' || p[-1] == ' ' || p[-1] == '\t') &&
                css_word_at(p, strlen(p), 0, "filename")) {
                const char *eq = p + 8;
                while (*eq == ' ' || *eq == '\t')
                    eq++;
                if (*eq == '=') {
                    const char *v = eq + 1;
                    unsigned long n = 0;
                    int quote = 0;

                    while (*v == ' ' || *v == '\t')
                        v++;
                    if (*v == '\'' || *v == '"')
                        quote = *v++;
                    while (v[n] && (quote ? v[n] != quote
                                          : (v[n] != ';' && v[n] != ' ' &&
                                             v[n] != '\t' && v[n] != '\r' &&
                                             v[n] != '\n')))
                        n++;
                    if (n > 0) {
                        if (n >= outsz)
                            n = outsz - 1;
                        memcpy(out, v, n);
                        out[n] = 0;
                        sanitize_download_name(out, outsz);
                        return;
                    }
                }
            }
            p++;
        }
    }
    if (url && *url) {
        const char *end = url;
        const char *slash;

        while (*end && *end != '?' && *end != '#')
            end++;
        slash = end;
        while (slash > url && slash[-1] != '/')
            slash--;
        if (end > slash && !(slash == url || slash[-1] == ':')) {
            unsigned long n = (unsigned long)(end - slash);
            if (n >= outsz)
                n = outsz - 1;
            memcpy(out, slash, n);
            out[n] = 0;
            sanitize_download_name(out, outsz);
            return;
        }
    }
    snprintf(out, outsz, "download.bin");
}

static BROWSER_NOINLINE int
save_download_file(const char *filename, const void *data, unsigned long len,
                   char *out_path, unsigned long out_pathsz)
{
    char clean[128];
    const char *src = (const char *)data;
    unsigned long wrote = 0;
    int fd;

    snprintf(clean, sizeof(clean), "%s", filename && *filename ? filename
                                                             : "download.bin");
    sanitize_download_name(clean, sizeof(clean));
    syscall(SYS_MKDIR, (long)"/home", 0, 0, 0);
    syscall(SYS_MKDIR, (long)DOWNLOAD_DIR, 0, 0, 0);
    snprintf(out_path, out_pathsz, "%s/%s", DOWNLOAD_DIR, clean);
#ifdef JS_HOST
    fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
#else
    fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC);
#endif
    if (fd < 0)
        return -1;
    while (wrote < len) {
        long n = write(fd, src + wrote, len - wrote);
        if (n <= 0)
            break;
        wrote += (unsigned long)n;
    }
    close(fd);
    return wrote == len ? 0 : -1;
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
    int is_md;
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
page_load_request_tab(struct browser_runtime *rt, struct browser_tab *tab,
                      const char *target, const char *method,
                      const void *request_body, unsigned long request_len,
                      const char *content_type, int viewport_w,
                      int viewport_h, struct download_info *dl_out,
                      char *fatal, unsigned long fatalsz)
{
    struct browser_page *p;
    struct page_load_work *w;
    const char *fetch_target = target;
    int is_vs;
    int rc, is_dark = 0;

    if (dl_out)
        memset(dl_out, 0, sizeof(*dl_out));
    if (rt && rt->is_gui) {
        gui_settings gs;
        gui_settings_get(&gs);
        is_dark = !gs.light;
    }
    is_vs = url_is_view_source(target, &fetch_target);
    if (!is_vs)
        fetch_target = target;
    p = (struct browser_page *)calloc(1, sizeof(*p));
    if (!p) {
        snprintf(fatal, fatalsz, "out of memory creating a page");
        return 0;
    }
    p->tab = tab;
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
    } else if (url_is_scheme(fetch_target, "file")) {
        p->is_local = 1;
        if (read_local_url(fetch_target, &w->local_body, &w->body_len,
                           w->primary_error,
                           sizeof(w->primary_error)) == 0) {
            w->body = w->local_body;
            if (!is_vs) {
                w->is_md = url_is_markdown(fetch_target);
                w->is_html = w->is_md ? 0 : sniff_html(w->body, w->body_len);
            }
            tab_net_log(tab, 200, "doc", w->body_len, fetch_target);
        } else {
            tab_net_log(tab, -1, "doc", 0, fetch_target);
        }
    } else if (netinfo(&w->net) != 0 || !w->net.up) {
        snprintf(w->primary_error, sizeof(w->primary_error),
                 "network unavailable: no NIC is configured");
    } else {
        w->request.url = fetch_target;
        w->request.method = method;
        w->request.body = request_body;
        w->request.body_len = request_len;
        w->request.content_type = content_type;
        w->request.max_body = FETCH_MAX;
        rc = http_fetch(rt->http, &w->request, &w->response);
        if (rc != HTTP_OK) {
            const char *tls_detail =
                url_is_scheme(fetch_target, "https") &&
                (rc == HTTP_E_CONNECT || rc == HTTP_E_DNS)
                    ? tls_last_transport_error() : 0;

            tab_net_log(tab, rc, "doc", 0, fetch_target);
            if (tls_detail && *tls_detail &&
                strcmp(tls_detail, "no error") != 0)
                snprintf(w->primary_error, sizeof(w->primary_error),
                         "%s: %s", http_error_text(rc), tls_detail);
            else
                snprintf(w->primary_error, sizeof(w->primary_error), "%s",
                         http_error_text(rc));
        } else {
            const char *ct, *cd;
            char *final_copy = 0;

            w->response_live = 1;
            if (w->response.final_url) {
                if (is_vs) {
                    unsigned long fl = strlen(w->response.final_url);
                    final_copy = (char *)malloc(13 + fl);
                    if (final_copy)
                        snprintf(final_copy, 13 + fl, "view-source:%s",
                                 w->response.final_url);
                } else {
                    final_copy = str_dup(w->response.final_url);
                }
            }
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
                cd = http_header_get(&w->response, "Content-Disposition");
                tab_net_log(tab, p->status, "doc", w->body_len, p->url);
                if (!is_vs && rt && rt->is_gui && dl_out &&
                    p->status >= 200 && p->status < 300 &&
                    (has_attachment_disposition(cd) ||
                     (ct && *ct && !is_renderable_mime(ct)))) {
                    char fname[128];

                    extract_download_filename(cd, p->url, fname, sizeof(fname));
                    dl_out->triggered = 1;
                    dl_out->bytes = w->body_len;
                    dl_out->ok = save_download_file(fname, w->body, w->body_len,
                                                    dl_out->path,
                                                    sizeof(dl_out->path)) == 0;
                    page_load_work_destroy(w);
                    page_destroy(p);
                    return 0;
                }
                if (!is_vs) {
                    w->is_html =
                        (ct && (str_ci_contains(ct, "text/html") ||
                                str_ci_contains(ct, "xhtml"))) ||
                        sniff_html(w->body, w->body_len);
                    w->is_md =
                        (ct && str_ci_contains(ct, "text/markdown")) ||
                        (!w->is_html && url_is_markdown(p->url));
                }
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
    p->is_html = is_vs ? 1 : (w->is_html || w->is_md);
    if (is_vs) {
        const char *disp_url = p->url;

        if (tab) {
            free(tab->raw_source);
            tab->raw_source = 0;
            tab->raw_len = 0;
            tab->raw_is_md = 0;
        }
        url_is_view_source(p->url, &disp_url);
        w->owned_source = make_view_source_source(disp_url, w->body,
                                                  w->body_len, p->status,
                                                  is_dark, &w->source_len);
        if (!w->owned_source)
            snprintf(w->primary_error, sizeof(w->primary_error),
                     "out of memory preparing view-source");
    } else {
        if (tab && w->body && !p->failed) {
            char *raw_copy = (char *)malloc(w->body_len + 1);
            if (raw_copy) {
                if (w->body_len)
                    memcpy(raw_copy, w->body, w->body_len);
                raw_copy[w->body_len] = 0;
                free(tab->raw_source);
                tab->raw_source = raw_copy;
                tab->raw_len = w->body_len;
                tab->raw_is_md = w->is_md;
            }
        }
        if (w->is_md) {
            w->owned_source = make_markdown_source(w->body, w->body_len, is_dark,
                                                   &w->source_len);
            if (!w->owned_source)
                snprintf(w->primary_error, sizeof(w->primary_error),
                         "out of memory formatting markdown");
        } else if (!w->is_html) {
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
page_load_request(struct browser_runtime *rt, const char *target,
                  const char *method, const void *request_body,
                  unsigned long request_len, const char *content_type,
                  int viewport_w, int viewport_h,
                  char *fatal, unsigned long fatalsz)
{
    return page_load_request_tab(rt, 0, target, method, request_body,
                                 request_len, content_type, viewport_w,
                                 viewport_h, 0, fatal, fatalsz);
}

static struct browser_page *
page_load(struct browser_runtime *rt, const char *target, int viewport_w,
          int viewport_h, char *fatal, unsigned long fatalsz)
{
    return page_load_request(rt, target, 0, 0, 0, 0, viewport_w, viewport_h,
                             fatal, fatalsz);
}

static BROWSER_NOINLINE struct browser_page *
page_from_memory(struct browser_runtime *rt, struct browser_tab *tab,
                 const char *url, const char *src, unsigned long len,
                 int is_html, int is_local, int status,
                 int viewport_w, int viewport_h,
                 char *fatal, unsigned long fatalsz)
{
    struct browser_page *p;
    char err[320];

    p = (struct browser_page *)calloc(1, sizeof(*p));
    if (!p) {
        snprintf(fatal, fatalsz, "out of memory creating page");
        return 0;
    }
    p->tab = tab;
    p->url = str_dup(url);
    if (!p->url) {
        snprintf(fatal, fatalsz, "out of memory copying URL");
        page_destroy(p);
        return 0;
    }
    p->is_local = is_local;
    p->is_html = is_html;
    p->status = status;
    p->bytes = len;
    if (page_transition(p, PAGE_NAVIGATING) != 0 ||
        page_transition(p, PAGE_FETCHING) != 0 ||
        page_transition(p, PAGE_RESPONSE) != 0) {
        snprintf(fatal, fatalsz, "invalid page lifecycle state");
        page_destroy(p);
        return 0;
    }
    if (build_pipeline(rt, p, src, len, viewport_w, viewport_h,
                       err, sizeof(err)) != 0) {
        page_transition(p, PAGE_FAILED);
        if (build_error_page(rt, p, viewport_w, viewport_h, err,
                             fatal, fatalsz) != 0) {
            page_destroy(p);
            return 0;
        }
    }
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

struct browser_bookmark {
    char title[48];
    char url[URL_MAX];
};

enum ui_btn_id {
    BTN_BACK = 0,
    BTN_FWD,
    BTN_RELOAD,
    BTN_ADDR,
    BTN_STAR,
    BTN_PIN,
    BTN_READER,
    BTN_ZOOM,
    BTN_FIND,
    BTN_DEV,
    BTN_COUNT
};

struct ui_btn_rect {
    int x, y, w, h;
};

struct bstate {
    struct browser_runtime *rt;
    gui_window *win;
    struct browser_tab *tabs[MAX_TABS];
    int tab_count;
    int active_tab;
    int addr_focus;

    int view_w, view_h;
    int top_chrome_h;
    char status[320];
    char load_error[320];
    char page_info[192];
    char hover_url[URL_MAX];
    int status_err;
    int sb_drag;
    int quit;
    int restoring_session;

    struct browser_bookmark bookmarks[BOOKMARK_MAX];
    int bookmark_count;

    struct favicon_entry favicons[FAVICON_CACHE_MAX];
    unsigned long favicon_clock;

    int find_open;
    char find_query[128];
    int find_len, find_cur;
    struct dom_node *find_nodes[256];
    int find_count, find_index;
    const struct lay_box *find_box;

    int dev_open;
    int dev_tab;              /* 0 = Elements, 1 = Console, 2 = Network */
    char repl[512];
    int repl_len, repl_cur, repl_focus;

    char draw_tmp[URL_MAX];
};

static struct browser_tab *active_tab(const struct bstate *b)
{
    if (!b || b->active_tab < 0 || b->active_tab >= b->tab_count)
        return 0;
    return b->tabs[b->active_tab];
}

static struct browser_page *active_page(const struct bstate *b)
{
    struct browser_tab *t = active_tab(b);

    return t ? t->page : 0;
}

static void sync_dom_pseudo_globals(struct bstate *b)
{
    struct browser_tab *t = active_tab(b);

    g_hover_node = t ? t->hover_node : 0;
    g_active_node = t ? t->active_node : 0;
    g_focus_node = t ? t->focus_node : 0;
}

static void recompute_layout_dims(struct bstate *b, int *out_dev_y,
                                  int *out_dev_h, int *out_find_y,
                                  int *out_stat_y)
{
    int top_h = TAB_H + BAR_H + BMARK_H;
    int stat_y = b->win->h - STAT_H;
    int bot_top = stat_y;
    int find_y = stat_y;
    int dev_y, dev_h = 0;

    if (b->find_open) {
        find_y = bot_top - FIND_H;
        if (find_y < top_h + 60)
            find_y = top_h + 60;
        bot_top = find_y;
    }
    dev_y = bot_top;
    if (b->dev_open) {
        int avail = bot_top - top_h - 80;

        if (avail >= 60) {
            dev_h = avail > DEV_H ? DEV_H : avail;
            dev_y = bot_top - dev_h;
            bot_top = dev_y;
        }
    }
    b->top_chrome_h = top_h;
    b->view_w = b->win->w - SB_W;
    if (b->view_w < 64)
        b->view_w = 64;
    b->view_h = bot_top - top_h;
    if (b->view_h < 40)
        b->view_h = 40;
    if (out_dev_y)
        *out_dev_y = dev_y;
    if (out_dev_h)
        *out_dev_h = dev_h;
    if (out_find_y)
        *out_find_y = find_y;
    if (out_stat_y)
        *out_stat_y = stat_y;
}

static int tab_pill_width(int win_w, int tab_count)
{
    int avail = win_w - 2 * PAD - 32;
    int tw;

    if (tab_count < 1)
        tab_count = 1;
    tw = (avail - (tab_count - 1) * 3) / tab_count;
    if (tw > 180)
        tw = 180;
    if (tw < 28)
        tw = 28;
    return tw;
}

static void tab_pill_rect(const struct bstate *b, int idx,
                          int *ox, int *oy, int *ow, int *oh)
{
    int tw = tab_pill_width(b->win->w, b->tab_count);

    *ox = PAD + idx * (tw + 3);
    *oy = 3;
    *ow = tw;
    *oh = TAB_H - 4;
}

static void tab_close_rect(const struct bstate *b, int idx,
                           int *ox, int *oy, int *ow, int *oh)
{
    int px, py, pw, ph;

    tab_pill_rect(b, idx, &px, &py, &pw, &ph);
    *ox = px + pw - 16;
    *oy = py + 2;
    *ow = 14;
    *oh = ph - 4;
}

static void tab_new_btn_rect(const struct bstate *b,
                             int *ox, int *oy, int *ow, int *oh)
{
    int tw = tab_pill_width(b->win->w, b->tab_count);
    int x = PAD + b->tab_count * (tw + 3) + 2;

    if (x + 24 > b->win->w - PAD)
        x = b->win->w - PAD - 24;
    *ox = x;
    *oy = 3;
    *ow = 24;
    *oh = TAB_H - 5;
}

static void toolbar_layout(const struct bstate *b, struct ui_btn_rect out[BTN_COUNT])
{
    static const int rwidths[6] = { 38, 40, 44, 54, 34, 42 }; /* Dev, Find, Zoom, Reader, Pin, Star */
    static const int rids[6] = { BTN_DEV, BTN_FIND, BTN_ZOOM, BTN_READER, BTN_PIN, BTN_STAR };
    int y0 = TAB_H + 3;
    int bh = BAR_H - 6;
    int rx = b->win->w - PAD;
    int i, addr_x, addr_w;

    out[BTN_BACK]   = (struct ui_btn_rect){ PAD, y0, 42, bh };
    out[BTN_FWD]    = (struct ui_btn_rect){ PAD + 46, y0, 38, bh };
    out[BTN_RELOAD] = (struct ui_btn_rect){ PAD + 88, y0, 54, bh };

    for (i = 0; i < 6; i++) {
        rx -= rwidths[i];
        out[rids[i]] = (struct ui_btn_rect){ rx, y0, rwidths[i], bh };
        rx -= 4;
    }
    addr_x = PAD + 146;
    addr_w = rx - addr_x;
    if (addr_w < 40)
        addr_w = 40;
    out[BTN_ADDR] = (struct ui_btn_rect){ addr_x, y0, addr_w, bh };
}

static int bmark_chip_rect(const struct bstate *b, int idx,
                           int *ox, int *oy, int *ow, int *oh)
{
    int x = PAD;
    int y = TAB_H + BAR_H + 2;
    int h = BMARK_H - 4;
    int i;

    for (i = 0; i <= idx && i < b->bookmark_count; i++) {
        int tw = gui_text_w(b->bookmarks[i].title);
        int cw = tw + 14;

        if (cw > 156)
            cw = 156;
        if (cw < 36)
            cw = 36;
        if (x + cw > b->win->w - PAD)
            return 0;
        if (i == idx) {
            *ox = x;
            *oy = y;
            *ow = cw;
            *oh = h;
            return 1;
        }
        x += cw + 4;
    }
    return 0;
}

static int pt_in_rect(int px, int py, int rx, int ry, int rw, int rh)
{
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

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

static void ui_text_maxw(gui_window *w, int x, int y, const char *s,
                         int max_w, unsigned c, int bold)
{
    int maxch;
    int len;

    if (!s || max_w <= 0)
        return;
    maxch = max_w / GLYPH_W;
    if (maxch <= 0)
        return;
    len = (int)strlen(s);
    if (len > maxch)
        len = maxch;
    gui_text_n(w, x, y, s, len, c, GUI_TRANSPARENT);
    if (bold)
        gui_text_n(w, x + 1, y, s, len, c, GUI_TRANSPARENT);
}

static void ui_box(gui_window *w, int x, int y, int cw, int ch, unsigned fill,
                   unsigned edge)
{
    gui_rect(w, x, y, cw, ch, fill);
    gui_frame(w, x, y, cw, ch, edge);
}

static void ui_button_th(gui_window *w, const gui_theme *th, int x, int y,
                         int cw, int ch, const char *label,
                         int enabled, int active)
{
    int tw = ui_text_w(label);
    unsigned fill = active ? th->accent_dark : th->surface_alt;
    unsigned edge = active ? th->accent : th->border;
    unsigned fg = !enabled ? th->text_dim : (active ? th->text_inv : th->text);

    ui_box(w, x, y, cw, ch, fill, edge);
    ui_text(w, x + (cw - tw) / 2, y + (ch - GLYPH_H) / 2, label, fg, active);
}

static void set_status(struct bstate *b, int error, const char *head,
                       const char *extra)
{
    struct browser_tab *t = active_tab(b);

    snprintf(b->status, sizeof(b->status), "%s%s%s", head ? head : "",
             extra && *extra ? "  -  " : "", extra ? extra : "");
    b->status_err = error;
    if (t) {
        snprintf(t->page_status, sizeof(t->page_status), "%s", b->status);
        t->page_status_err = error;
    }
}

static int content_h(const struct bstate *b)
{
    struct browser_page *p = active_page(b);

    return p && p->layout ? lay_height(p->layout) : 0;
}

static void clamp_scroll(struct bstate *b)
{
    struct browser_tab *t = active_tab(b);
    int max;

    if (!t)
        return;
    max = content_h(b) - b->view_h;
    if (max < 0)
        max = 0;
    if (t->scroll > max)
        t->scroll = max;
    if (t->scroll < 0)
        t->scroll = 0;
    if (t->page)
        t->page->scroll_y = t->scroll;
}

static void scroll_box_into_view(struct bstate *b, const struct lay_box *box)
{
    struct browser_tab *t = active_tab(b);
    lay_rect r;

    if (!t || !box)
        return;
    r = lay_border_rect(box);
    if (r.y < t->scroll)
        t->scroll = r.y - 16;
    else if (r.y + r.h > t->scroll + b->view_h)
        t->scroll = r.y + r.h - b->view_h + 16;
    clamp_scroll(b);
    if (t->page && t->page->js)
        jsdom_set_scroll(t->page->js, 0, t->scroll);
}

static void canonicalize_match_url(const char *in, char *out,
                                   unsigned long outsz)
{
    char err[64];

    if (!in || !*in) {
        out[0] = 0;
        return;
    }
    if (canonicalize_input(in, out, outsz, err, sizeof(err)) != 0)
        snprintf(out, outsz, "%.*s", (int)(outsz - 1), in);
}

static void bookmarks_save(const struct bstate *b)
{
    struct bytebuf out;
    int i, fd;
    unsigned long wrote = 0;

    memset(&out, 0, sizeof(out));
    out.limit = 65536;
    for (i = 0; i < b->bookmark_count; i++) {
        if (buf_add_str(&out, b->bookmarks[i].title) != 0 ||
            buf_add(&out, "|", 1) != 0 ||
            buf_add_str(&out, b->bookmarks[i].url) != 0 ||
            buf_add(&out, "\n", 1) != 0) {
            free(out.p);
            return;
        }
    }
    syscall(SYS_MKDIR, (long)"/home", 0, 0, 0);
#ifdef JS_HOST
    fd = open(BOOKMARK_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
#else
    fd = open(BOOKMARK_FILE, O_WRONLY | O_CREAT | O_TRUNC);
#endif
    if (fd >= 0) {
        while (wrote < out.n) {
            long n = write(fd, out.p + wrote, out.n - wrote);
            if (n <= 0)
                break;
            wrote += (unsigned long)n;
        }
        close(fd);
    }
    free(out.p);
}

static void bookmarks_seed_defaults(struct bstate *b)
{
    b->bookmark_count = 2;
    snprintf(b->bookmarks[0].title, sizeof(b->bookmarks[0].title), "Home");
    snprintf(b->bookmarks[0].url, sizeof(b->bookmarks[0].url),
             "/doc/home.html");
    snprintf(b->bookmarks[1].title, sizeof(b->bookmarks[1].title),
             "Playground");
    snprintf(b->bookmarks[1].url, sizeof(b->bookmarks[1].url),
             "/doc/playground.html");
    bookmarks_save(b);
}

static BROWSER_NOINLINE void bookmarks_load(struct bstate *b)
{
    struct k_stat st;
    char *data, *p, *end;
    unsigned long got = 0;
    int fd;

    b->bookmark_count = 0;
    if (stat_(BOOKMARK_FILE, &st) != 0 || st.is_dir ||
        st.size <= 0 || (unsigned long)st.size > 65536UL) {
        bookmarks_seed_defaults(b);
        return;
    }
    fd = open(BOOKMARK_FILE, O_RDONLY);
    if (fd < 0) {
        bookmarks_seed_defaults(b);
        return;
    }
    data = (char *)malloc((unsigned long)st.size + 1);
    if (!data) {
        close(fd);
        bookmarks_seed_defaults(b);
        return;
    }
    while (got < (unsigned long)st.size) {
        long n = read(fd, data + got, (unsigned long)st.size - got);
        if (n <= 0)
            break;
        got += (unsigned long)n;
    }
    close(fd);
    data[got] = 0;

    p = data;
    end = data + got;
    while (p < end && b->bookmark_count < BOOKMARK_MAX) {
        char *nl = p, *bar = 0;
        unsigned long tl, ul;
        struct browser_bookmark *bm;

        while (nl < end && *nl != '\n' && *nl != '\r') {
            if (!bar && *nl == '|')
                bar = nl;
            nl++;
        }
        if (bar && bar > p && nl > bar + 1) {
            bm = &b->bookmarks[b->bookmark_count++];
            tl = (unsigned long)(bar - p);
            if (tl >= sizeof(bm->title))
                tl = sizeof(bm->title) - 1;
            memcpy(bm->title, p, tl);
            bm->title[tl] = 0;
            ul = (unsigned long)(nl - (bar + 1));
            if (ul >= sizeof(bm->url))
                ul = sizeof(bm->url) - 1;
            memcpy(bm->url, bar + 1, ul);
            bm->url[ul] = 0;
        }
        p = nl;
        while (p < end && (*p == '\n' || *p == '\r'))
            p++;
    }
    free(data);
    if (b->bookmark_count == 0)
        bookmarks_seed_defaults(b);
}

static int bookmark_find_index(const struct bstate *b, const char *url)
{
    char target[URL_MAX], cand[URL_MAX];
    int i;

    if (!url || !*url)
        return -1;
    canonicalize_match_url(url, target, sizeof(target));
    for (i = 0; i < b->bookmark_count; i++) {
        canonicalize_match_url(b->bookmarks[i].url, cand, sizeof(cand));
        if (strcmp(target, cand) == 0)
            return i;
    }
    return -1;
}

static void sanitize_bookmark_title(char *s)
{
    unsigned long i;

    for (i = 0; s[i]; i++) {
        if (s[i] == '|' || s[i] == '\r' || s[i] == '\n')
            s[i] = ' ';
    }
}

static void bookmark_toggle(struct bstate *b)
{
    struct browser_page *p = active_page(b);
    int idx;
    const char *title;

    if (!p || !p->url || !*p->url)
        return;
    idx = bookmark_find_index(b, p->url);
    if (idx >= 0) {
        int i;

        for (i = idx; i + 1 < b->bookmark_count; i++)
            b->bookmarks[i] = b->bookmarks[i + 1];
        b->bookmark_count--;
        bookmarks_save(b);
        set_status(b, 0, "Bookmark removed", p->url);
        return;
    }
    if (b->bookmark_count >= BOOKMARK_MAX) {
        set_status(b, 1, "Bookmark limit reached", p->url);
        return;
    }
    title = (p->doc && p->doc->title && *p->doc->title) ? p->doc->title : p->url;
    idx = b->bookmark_count++;
    snprintf(b->bookmarks[idx].title, sizeof(b->bookmarks[idx].title),
             "%.*s", (int)(sizeof(b->bookmarks[idx].title) - 1), title);
    sanitize_bookmark_title(b->bookmarks[idx].title);
    if (!b->bookmarks[idx].title[0])
        snprintf(b->bookmarks[idx].title, sizeof(b->bookmarks[idx].title), "Page");
    snprintf(b->bookmarks[idx].url, sizeof(b->bookmarks[idx].url),
             "%.*s", (int)(sizeof(b->bookmarks[idx].url) - 1), p->url);
    bookmarks_save(b);
    set_status(b, 0, "Bookmarked", b->bookmarks[idx].title);
}

static int reader_skip_tag(const char *tag)
{
    if (!tag)
        return 0;
    return !strcmp(tag, "script") || !strcmp(tag, "style") ||
           !strcmp(tag, "noscript") || !strcmp(tag, "nav") ||
           !strcmp(tag, "aside") || !strcmp(tag, "footer") ||
           !strcmp(tag, "form") || !strcmp(tag, "button") ||
           !strcmp(tag, "input") || !strcmp(tag, "select") ||
           !strcmp(tag, "textarea") || !strcmp(tag, "svg");
}

static int reader_block_tag(const char *tag)
{
    if (!tag)
        return 0;
    return !strcmp(tag, "h1") || !strcmp(tag, "h2") || !strcmp(tag, "h3") ||
           !strcmp(tag, "h4") || !strcmp(tag, "h5") || !strcmp(tag, "h6") ||
           !strcmp(tag, "p") || !strcmp(tag, "blockquote") ||
           !strcmp(tag, "pre") || !strcmp(tag, "li");
}

static int reader_emit_node_inline(struct bytebuf *out, struct dom_node *block)
{
    struct dom_node *c = block->first_child;

    while (c) {
        if (c->type == DOM_ELEMENT && reader_skip_tag(c->tag)) {
            c = dom_next_skip(c, block);
            continue;
        }
        if (c->type == DOM_ELEMENT && c->tag) {
            if (!strcmp(c->tag, "br")) {
                if (buf_add_str(out, "<br>") != 0)
                    return -1;
            } else if (!strcmp(c->tag, "a")) {
                const char *href = dom_get_attr(c, "href");

                if (href && md_link_safe(href, strlen(href))) {
                    if (buf_add_str(out, "<a href=\"") != 0 ||
                        buf_add_escaped_html(out, href, strlen(href)) != 0 ||
                        buf_add_str(out, "\">") != 0)
                        return -1;
                }
            } else if (!strcmp(c->tag, "code")) {
                if (buf_add_str(out, "<code>") != 0)
                    return -1;
            }
        } else if (c->type == DOM_TEXT && c->text && c->text_len > 0) {
            if (buf_add_escaped_html(out, c->text, c->text_len) != 0)
                return -1;
        }
        if (c->first_child) {
            c = c->first_child;
            continue;
        }
        for (;;) {
            if (c->type == DOM_ELEMENT && c->tag) {
                if (!strcmp(c->tag, "a")) {
                    const char *href = dom_get_attr(c, "href");
                    if (href && md_link_safe(href, strlen(href)) &&
                        buf_add_str(out, "</a>") != 0)
                        return -1;
                } else if (!strcmp(c->tag, "code")) {
                    if (buf_add_str(out, "</code>") != 0)
                        return -1;
                }
            }
            if (c->next_sibling) {
                c = c->next_sibling;
                break;
            }
            c = c->parent;
            if (!c || c == block) {
                c = 0;
                break;
            }
        }
    }
    return 0;
}

static unsigned long reader_text_weight(struct dom_node *root)
{
    struct dom_node *n = root;
    unsigned long total = 0;

    while (n) {
        if (n->type == DOM_ELEMENT && reader_skip_tag(n->tag)) {
            n = dom_next_skip(n, root);
            continue;
        }
        if (n->type == DOM_TEXT && n->text)
            total += n->text_len;
        n = dom_next_within(n, root);
    }
    return total;
}

static BROWSER_NOINLINE char *
make_reader_source(const struct dom_document *doc, const char *url,
                   int is_dark, unsigned long *out_len)
{
    struct bytebuf out;
    struct dom_node *root = 0, *best = 0, *n;
    const char *title;
    int emitted = 0, saw_h1 = 0;

    if (out_len)
        *out_len = 0;
    if (!doc || !doc->root)
        return 0;
    root = doc->body ? doc->body : doc->root;
    for (n = root; n; n = dom_next_within(n, root)) {
        if (n->type == DOM_ELEMENT && n->tag &&
            (!strcmp(n->tag, "article") || !strcmp(n->tag, "main"))) {
            if (reader_text_weight(n) >= 40) {
                best = n;
                break;
            }
        }
    }
    if (best)
        root = best;
    title = (doc->title && *doc->title) ? doc->title : (url ? url : "Reader");

    memset(&out, 0, sizeof(out));
    out.limit = 4UL * 1024UL * 1024UL;
    if (buf_add_str(&out, "<!doctype html><html><head><title>") != 0 ||
        buf_add_escaped_html(&out, title, strlen(title)) != 0 ||
        buf_add_str(&out, "</title><style>") != 0 ||
        buf_add_str(&out, is_dark
            ? "body{font-family:sans-serif;background:#141a22;color:#e4ebf5;"
              "max-width:68ch;margin:20px auto;padding:10px 18px;line-height:1.55;}"
              "a{color:#66b3ff;}h1,h2,h3{color:#f4f7fc;margin:18px 0 8px;}"
              "pre,code{background:#0d1117;color:#d6e2f0;font-family:monospace;}"
              "pre{padding:10px 12px;border:1px solid #2b3648;overflow:auto;}"
              "blockquote{border-left:3px solid #4880c8;margin:10px 0;padding:4px 12px;color:#b8c6db;}"
              ".reader-bar{font-size:12px;color:#8ea2be;border-bottom:1px solid #2b3648;"
              "padding-bottom:6px;margin-bottom:14px;}"
            : "body{font-family:sans-serif;background:#faf8f2;color:#1e2228;"
              "max-width:68ch;margin:20px auto;padding:10px 18px;line-height:1.55;}"
              "a{color:#0b4ec2;}h1,h2,h3{color:#11151b;margin:18px 0 8px;}"
              "pre,code{background:#efece4;color:#1e2228;font-family:monospace;}"
              "pre{padding:10px 12px;border:1px solid #d4cfc4;overflow:auto;}"
              "blockquote{border-left:3px solid #356bb3;margin:10px 0;padding:4px 12px;color:#444b54;}"
              ".reader-bar{font-size:12px;color:#5a6370;border-bottom:1px solid #d4cfc4;"
              "padding-bottom:6px;margin-bottom:14px;}") != 0 ||
        buf_add_str(&out, "</style></head><body><div class=\"reader-bar\">"
                          "Reader View</div>") != 0)
        goto fail;

    if (doc->title && *doc->title) {
        if (buf_add_str(&out, "<h1>") != 0 ||
            buf_add_escaped_html(&out, doc->title, strlen(doc->title)) != 0 ||
            buf_add_str(&out, "</h1>\n") != 0)
            goto fail;
        saw_h1 = 1;
    }

    n = root->first_child;
    while (n) {
        if (n->type == DOM_ELEMENT && reader_skip_tag(n->tag)) {
            n = dom_next_skip(n, root);
            continue;
        }
        if (n->type == DOM_ELEMENT && n->tag) {
            if (!strcmp(n->tag, "hr")) {
                if (buf_add_str(&out, "<hr>\n") != 0)
                    goto fail;
                emitted++;
                n = dom_next_skip(n, root);
                continue;
            }
            if (reader_block_tag(n->tag)) {
                struct bytebuf seg;

                memset(&seg, 0, sizeof(seg));
                seg.limit = 512UL * 1024UL;
                if (reader_emit_node_inline(&seg, n) != 0) {
                    free(seg.p);
                    goto fail;
                }
                if (seg.n > 0) {
                    const char *t = n->tag;
                    if (!strcmp(t, "h1") && saw_h1)
                        t = "h2";
                    if (buf_add(&out, "<", 1) != 0 ||
                        buf_add_str(&out, t) != 0 ||
                        buf_add(&out, ">", 1) != 0 ||
                        buf_add(&out, seg.p, seg.n) != 0 ||
                        buf_add(&out, "</", 2) != 0 ||
                        buf_add_str(&out, t) != 0 ||
                        buf_add(&out, ">\n", 2) != 0) {
                        free(seg.p);
                        goto fail;
                    }
                    emitted++;
                }
                free(seg.p);
                n = dom_next_skip(n, root);
                continue;
            }
        }
        n = dom_next_within(n, root);
    }

    if (emitted == 0) {
        for (n = root; n; ) {
            if (n->type == DOM_ELEMENT && reader_skip_tag(n->tag)) {
                n = dom_next_skip(n, root);
                continue;
            }
            if (n->type == DOM_TEXT && n->text && n->text_len > 0) {
                unsigned long i = 0;
                while (i < n->text_len &&
                       (unsigned char)n->text[i] <= ' ')
                    i++;
                if (i < n->text_len) {
                    if (buf_add_str(&out, "<p>") != 0 ||
                        buf_add_escaped_html(&out, n->text, n->text_len) != 0 ||
                        buf_add_str(&out, "</p>\n") != 0)
                        goto fail;
                }
            }
            n = dom_next_within(n, root);
        }
    }

    if (buf_add_str(&out, "</body></html>") != 0)
        goto fail;
    if (out_len)
        *out_len = out.n;
    return out.p;

fail:
    free(out.p);
    return 0;
}

static int ci_contains_n(const char *hay, unsigned long hay_len,
                         const char *needle, unsigned long needle_len)
{
    unsigned long i, j;

    if (!needle || needle_len == 0)
        return 0;
    if (!hay || hay_len < needle_len)
        return 0;
    for (i = 0; i <= hay_len - needle_len; i++) {
        for (j = 0; j < needle_len; j++) {
            unsigned char a = (unsigned char)hay[i + j];
            unsigned char c = (unsigned char)needle[j];
            if (a >= 'A' && a <= 'Z')
                a += 32;
            if (c >= 'A' && c <= 'Z')
                c += 32;
            if (a != c)
                break;
        }
        if (j == needle_len)
            return 1;
    }
    return 0;
}

static void find_clear(struct bstate *b)
{
    b->find_count = 0;
    b->find_index = 0;
    b->find_box = 0;
}

static const struct lay_box *find_box_for_node(const struct lay_document *L,
                                               struct dom_node *n)
{
    struct dom_node *a;

    if (!L || !n)
        return 0;
    for (a = n; a; a = a->parent) {
        const struct lay_box *box = lay_box_for_node(L, a);
        if (box && !(box->flags & LAYF_HIDDEN))
            return box;
    }
    return 0;
}

static void find_select_idx(struct bstate *b, int idx, int do_scroll)
{
    struct browser_page *p = active_page(b);

    if (b->find_count <= 0 || !p || !p->layout) {
        b->find_index = 0;
        b->find_box = 0;
        return;
    }
    if (idx < 0)
        idx = b->find_count - 1;
    if (idx >= b->find_count)
        idx = 0;
    b->find_index = idx;
    b->find_box = find_box_for_node(p->layout, b->find_nodes[idx]);
    if (do_scroll && b->find_box)
        scroll_box_into_view(b, b->find_box);
}

static void find_refresh(struct bstate *b, int jump)
{
    struct browser_page *p = active_page(b);
    struct dom_node *n;
    unsigned long qlen = (unsigned long)b->find_len;

    find_clear(b);
    if (!p || !p->doc || !p->doc->root || !p->layout || qlen == 0)
        return;
    n = p->doc->root;
    while (n && b->find_count < (int)(sizeof(b->find_nodes) / sizeof(b->find_nodes[0]))) {
        if (n->type == DOM_ELEMENT && n->tag &&
            (!strcmp(n->tag, "script") || !strcmp(n->tag, "style") ||
             !strcmp(n->tag, "noscript"))) {
            n = dom_next_skip(n, p->doc->root);
            continue;
        }
        if (n->type == DOM_TEXT && n->text && n->text_len > 0) {
            if (ci_contains_n(n->text, n->text_len, b->find_query, qlen) &&
                find_box_for_node(p->layout, n)) {
                b->find_nodes[b->find_count++] = n;
            }
        }
        n = dom_next_within(n, p->doc->root);
    }
    if (b->find_count > 0)
        find_select_idx(b, 0, jump);
}

static void find_step(struct bstate *b, int delta)
{
    if (b->find_count <= 0) {
        find_refresh(b, 1);
        return;
    }
    find_select_idx(b, b->find_index + delta, 1);
}

static int select_option_count(struct dom_node *sel)
{
    struct dom_node *c;
    int n = 0;

    if (!sel)
        return 0;
    for (c = sel->first_child; c; c = dom_next_within(c, sel)) {
        if (c->type == DOM_ELEMENT && c->tag && !strcmp(c->tag, "option"))
            n++;
    }
    return n;
}

static struct dom_node *select_option_nth(struct dom_node *sel, int target)
{
    struct dom_node *c;
    int idx = 0;

    if (!sel || target < 0)
        return 0;
    for (c = sel->first_child; c; c = dom_next_within(c, sel)) {
        if (c->type == DOM_ELEMENT && c->tag && !strcmp(c->tag, "option")) {
            if (idx == target)
                return c;
            idx++;
        }
    }
    return 0;
}

static int select_dropdown_rect(const struct bstate *b, int *ox, int *oy,
                                int *ow, int *oh, int *o_vis)
{
    struct browser_tab *t = active_tab(b);
    struct browser_page *p = t ? t->page : 0;
    const struct lay_box *box;
    lay_rect r;
    int n, vis, row_h = GLYPH_H + 6;
    int x, y, w, h;

    if (!t || !t->select_open_node || !p || !p->layout)
        return 0;
    box = lay_box_for_node(p->layout, t->select_open_node);
    if (!box)
        return 0;
    n = select_option_count(t->select_open_node);
    if (n <= 0)
        return 0;
    r = lay_border_rect(box);
    vis = n > 8 ? 8 : n;
    w = r.w < 140 ? 140 : r.w;
    if (w > b->view_w - 8)
        w = b->view_w - 8;
    h = vis * row_h + 4;
    x = r.x;
    if (x + w > b->view_w)
        x = b->view_w - w;
    if (x < 0)
        x = 0;
    y = b->top_chrome_h + (r.y - t->scroll) + r.h;
    if (y + h > b->top_chrome_h + b->view_h) {
        int above = b->top_chrome_h + (r.y - t->scroll) - h;
        if (above >= b->top_chrome_h)
            y = above;
    }
    *ox = x;
    *oy = y;
    *ow = w;
    *oh = h;
    if (o_vis)
        *o_vis = vis;
    return 1;
}

static int select_selected_index(struct dom_node *sel)
{
    struct dom_node *c;
    int idx = 0;

    for (c = sel->first_child; c; c = dom_next_within(c, sel)) {
        if (c->type == DOM_ELEMENT && c->tag && !strcmp(c->tag, "option")) {
            if (dom_has_attr(c, "selected"))
                return idx;
            idx++;
        }
    }
    return 0;
}

/* Clamp the first visible option so a full page of rows stays in view. */
static void select_set_top(struct browser_tab *t, int top, int vis)
{
    int max_top = select_option_count(t->select_open_node) - vis;

    if (top > max_top)
        top = max_top;
    if (top < 0)
        top = 0;
    t->select_top = top;
}

static void select_reveal(struct browser_tab *t, int idx, int vis)
{
    if (idx < t->select_top)
        select_set_top(t, idx, vis);
    else if (idx >= t->select_top + vis)
        select_set_top(t, idx - vis + 1, vis);
}

static void select_open(struct bstate *b, struct dom_node *sel)
{
    struct browser_tab *t = active_tab(b);
    int dx, dy, dw, dh, vis;

    t->select_open_node = sel;
    t->select_hover = -1;
    t->select_top = 0;
    if (select_dropdown_rect(b, &dx, &dy, &dw, &dh, &vis))
        select_reveal(t, select_selected_index(sel), vis);
}

static void select_close(struct browser_tab *t)
{
    t->select_open_node = 0;
    t->select_hover = -1;
}

/* Row under (x, y) as an option index, or -1 outside the option rows. */
static int select_option_at(const struct bstate *b, int x, int y)
{
    struct browser_tab *t = active_tab(b);
    int dx, dy, dw, dh, vis, row;

    if (!select_dropdown_rect(b, &dx, &dy, &dw, &dh, &vis) ||
        !pt_in_rect(x, y, dx, dy, dw, dh))
        return -1;
    row = (y - dy - 2) / (GLYPH_H + 6);
    if (row < 0 || row >= vis)
        return -1;
    return t->select_top + row;
}

static int dev_collect_elem_line(struct browser_page *p, int target_idx,
                                 char *out, unsigned long outsz,
                                 int *total_out)
{
    struct dom_node *n;
    int idx = 0;

    if (!p || !p->doc || !p->doc->root) {
        if (total_out)
            *total_out = 0;
        return 0;
    }
    for (n = p->doc->root; n; n = dom_next_within(n, p->doc->root)) {
        if (n->type != DOM_ELEMENT || !n->tag)
            continue;
        if (idx == target_idx && out && outsz > 0) {
            const char *id = dom_get_attr(n, "id");
            const char *cls = dom_get_attr(n, "class");
            const struct lay_box *box =
                p->layout ? lay_box_for_node(p->layout, n) : 0;
            unsigned indent = n->depth > 1 ? (n->depth - 1) * 2 : 0;
            unsigned long pos = 0;
            char sizebuf[32];

            if (indent > 24)
                indent = 24;
            while (pos < indent && pos + 1 < outsz)
                out[pos++] = ' ';
            sizebuf[0] = 0;
            if (box) {
                lay_rect r = lay_border_rect(box);
                snprintf(sizebuf, sizeof(sizebuf), " [%dx%d]", r.w, r.h);
            }
            snprintf(out + pos, outsz - pos, "<%s%s%s%s%s>%s",
                     n->tag,
                     id && *id ? "#" : "", id ? id : "",
                     cls && *cls ? "." : "", cls ? cls : "",
                     sizebuf);
            if (!total_out)
                return 1;
        }
        idx++;
    }
    if (total_out)
        *total_out = idx;
    return 0;
}

static void dev_exec_repl(struct bstate *b)
{
    struct browser_tab *t = active_tab(b);
    struct browser_page *p = t ? t->page : 0;
    char *expr;
    char echo[520];
    char wrapped_buf[640];
    char err[256];
    int rc;

    if (!t || b->repl_len <= 0)
        return;
    b->repl[b->repl_len] = 0;
    expr = b->repl;
    snprintf(echo, sizeof(echo), "> %.*s", (int)(sizeof(echo) - 4), expr);
    tab_console_log(t, echo);
    if (!p || !p->js) {
        tab_console_log(t, "error: no active JS context");
        return;
    }
    err[0] = 0;
    snprintf(wrapped_buf, sizeof(wrapped_buf),
             "console.log(%.*s)", (int)(sizeof(wrapped_buf) - 16), expr);
    rc = jsdom_eval(p->js, wrapped_buf, "repl", err, sizeof(err));
    if (rc != 0) {
        err[0] = 0;
        rc = jsdom_eval(p->js, expr, "repl", err, sizeof(err));
        if (rc != 0) {
            snprintf(echo, sizeof(echo), "error: %s",
                     err[0] ? err : "evaluation failed");
            tab_console_log(t, echo);
        }
    }
    b->repl_len = 0;
    b->repl_cur = 0;
    b->repl[0] = 0;
    if (t->console_count > 6)
        t->con_scroll = t->console_count - 6;
    if (jsdom_dirty(p->js)) {
        char rerr[320];

        sync_dom_pseudo_globals(b);
        if (page_reflow(b->rt, p, b->view_w, b->view_h,
                        rerr, sizeof(rerr)) == 0) {
            find_refresh(b, 0);
            clamp_scroll(b);
        }
        jsdom_clear_dirty(p->js);
    }
}

static struct favicon_entry *
favicon_cache_find(struct bstate *b, const char *url, int touch)
{
    int i;

    if (!b || !url || !*url)
        return 0;
    for (i = 0; i < FAVICON_CACHE_MAX; i++) {
        struct favicon_entry *e = &b->favicons[i];

        if (e->state != 0 && e->url && strcmp(e->url, url) == 0) {
            if (touch)
                e->last_used = ++b->favicon_clock;
            return e;
        }
    }
    return 0;
}

static struct favicon_entry *favicon_cache_alloc(struct bstate *b)
{
    int i, best = 0;

    for (i = 0; i < FAVICON_CACHE_MAX; i++) {
        if (b->favicons[i].state == 0 && !b->favicons[i].url)
            return &b->favicons[i];
        if (b->favicons[i].last_used < b->favicons[best].last_used)
            best = i;
    }
    free(b->favicons[best].url);
    img_free(&b->favicons[best].icon16);
    memset(&b->favicons[best], 0, sizeof(b->favicons[best]));
    return &b->favicons[best];
}

static void favicon_cache_free_all(struct bstate *b)
{
    int i;

    if (!b)
        return;
    for (i = 0; i < FAVICON_CACHE_MAX; i++) {
        free(b->favicons[i].url);
        img_free(&b->favicons[i].icon16);
        memset(&b->favicons[i], 0, sizeof(b->favicons[i]));
    }
}

struct favicon_work {
    char candidate[URL_MAX];
    char err[160];
    struct http_request req;
    struct http_response res;
    struct k_netinfo net;
    struct image decoded;
};

static BROWSER_NOINLINE void
tab_load_favicon(struct bstate *b, struct browser_tab *t)
{
    struct browser_page *p;
    struct favicon_work *fw;
    struct favicon_entry *ce;
    struct dom_node *n;
    int found = 0, is_data = 0, decoded_ok = 0;

    if (!t)
        return;
    free(t->favicon_url);
    t->favicon_url = 0;
    p = t->page;
    if (!b || !b->rt || !b->rt->is_gui || !p || p->failed || !p->is_html ||
        !p->doc || !p->doc->root || t->reader_mode ||
        url_is_view_source(p->url, 0))
        return;

    fw = (struct favicon_work *)calloc(1, sizeof(*fw));
    if (!fw)
        return;

    for (n = p->doc->root; n; n = dom_next(n)) {
        const char *rel, *href;

        if (n->type != DOM_ELEMENT || n->tag_id != HTAG_LINK)
            continue;
        rel = dom_get_attr(n, "rel");
        href = dom_get_attr(n, "href");
        if (!rel_has_token(rel, "icon") || !href || !*href)
            continue;
        if (strncmp(href, "data:image/", 11) == 0) {
            if (strlen(href) < sizeof(fw->candidate)) {
                snprintf(fw->candidate, sizeof(fw->candidate), "%s", href);
                is_data = 1;
                found = 1;
            }
        } else if (resource_url(p, p->base_url ? p->base_url : p->url, href,
                                fw->candidate, sizeof(fw->candidate),
                                fw->err, sizeof(fw->err)) == 0) {
            found = 1;
        }
        break;
    }

    if (!found &&
        (url_is_scheme(p->url, "http") || url_is_scheme(p->url, "https"))) {
        if (resource_url(p, p->url, "/favicon.ico", fw->candidate,
                         sizeof(fw->candidate), fw->err,
                         sizeof(fw->err)) == 0)
            found = 1;
    }

    if (!found) {
        free(fw);
        return;
    }

    ce = favicon_cache_find(b, fw->candidate, 1);
    if (ce) {
        t->favicon_url = str_dup(fw->candidate);
        free(fw);
        return;
    }

    ce = favicon_cache_alloc(b);
    if (!ce) {
        free(fw);
        return;
    }
    ce->url = str_dup(fw->candidate);
    if (!ce->url) {
        free(fw);
        return;
    }
    ce->last_used = ++b->favicon_clock;
    ce->state = -1;

    if (is_data) {
        unsigned char *raw = 0;
        unsigned long raw_len = 0;

        if (decode_data_image(fw->candidate, &raw, &raw_len) == 0 &&
            raw_len > 0 && raw_len <= FAVICON_MAX_BYTES) {
            if (img_decode(raw, raw_len, &fw->decoded) == IMG_OK)
                decoded_ok = 1;
        }
        free(raw);
    } else if (url_is_scheme(fw->candidate, "file")) {
        char *local_buf = 0;
        unsigned long local_len = 0;

        if (read_local_url(fw->candidate, &local_buf, &local_len,
                           fw->err, sizeof(fw->err)) == 0) {
            tab_net_log(t, 200, "icon", local_len, fw->candidate);
            if (local_len > 0 && local_len <= FAVICON_MAX_BYTES &&
                img_decode(local_buf, local_len, &fw->decoded) == IMG_OK)
                decoded_ok = 1;
        } else {
            tab_net_log(t, -1, "icon", 0, fw->candidate);
        }
        free(local_buf);
    } else if (b->rt->http && netinfo(&fw->net) == 0 && fw->net.up) {
        int rc;

        fw->req.url = fw->candidate;
        fw->req.accept = "image/png,image/jpeg,image/gif,image/bmp,"
                         "image/svg+xml,image/x-icon,*/*;q=0.5";
        fw->req.max_body = FAVICON_MAX_BYTES;
        rc = http_fetch(b->rt->http, &fw->req, &fw->res);
        if (rc != HTTP_OK) {
            tab_net_log(t, rc, "icon", 0, fw->candidate);
        } else {
            tab_net_log(t, fw->res.status, "icon", fw->res.body_len,
                        fw->res.final_url ? fw->res.final_url : fw->candidate);
            if (fw->res.status >= 200 && fw->res.status < 300 &&
                fw->res.body && fw->res.body_len > 0 &&
                img_decode(fw->res.body, fw->res.body_len,
                           &fw->decoded) == IMG_OK)
                decoded_ok = 1;
            http_response_free(&fw->res);
        }
    }

    if (decoded_ok) {
        if (img_scale(&fw->decoded, FAVICON_SIZE, FAVICON_SIZE,
                      &ce->icon16) == IMG_OK)
            ce->state = 1;
        img_free(&fw->decoded);
    }
    t->favicon_url = str_dup(fw->candidate);
    free(fw);
}

static void draw_icon16(gui_window *win, int x, int y, const struct image *im)
{
    int row, col;

    if (!win || !im || !im->px || im->w <= 0 || im->h <= 0)
        return;
    for (row = 0; row < im->h; row++) {
        for (col = 0; col < im->w; col++) {
            long si = (long)row * im->w + col;
            uint8_t a = (im->has_alpha && im->alpha) ? im->alpha[si] : 255u;

            if (a == 255u)
                gui_pixel(win, x + col, y + row, im->px[si] & 0x00FFFFFFu);
            else if (a > 0)
                gui_blend_pixel(win, x + col, y + row, im->px[si], a);
        }
    }
}

static void draw_tabs(struct bstate *b, const gui_theme *th)
{
    int i, nx, ny, nw, nh;

    gui_rect(b->win, 0, 0, b->win->w, TAB_H, th->panel);
    gui_line(b->win, 0, TAB_H - 1, b->win->w - 1, TAB_H - 1, th->border);
    for (i = 0; i < b->tab_count; i++) {
        struct browser_tab *t = b->tabs[i];
        struct browser_page *p = t ? t->page : 0;
        const char *title = (p && p->doc && p->doc->title && *p->doc->title)
                                ? p->doc->title
                                : (t && t->addr[0] ? t->addr : "New Tab");
        struct favicon_entry *fe =
            (t && t->favicon_url) ? favicon_cache_find(b, t->favicon_url, 0) : 0;
        int px, py, pw, ph, cx, cy, cw, ch;
        int is_cur = (i == b->active_tab);
        int has_icon = 0;

        tab_pill_rect(b, i, &px, &py, &pw, &ph);
        has_icon = fe && fe->state == 1 && fe->icon16.px && pw >= 44;
        ui_box(b->win, px, py, pw, ph,
               is_cur ? th->surface : th->surface_alt,
               is_cur ? th->accent : th->border);
        if (is_cur)
            gui_rect(b->win, px + 1, py + 1, pw - 2, 2, th->accent);
        if (has_icon) {
            draw_icon16(b->win, px + 5, py + (ph - FAVICON_SIZE) / 2,
                        &fe->icon16);
            ui_text_maxw(b->win, px + 25, py + (ph - GLYPH_H) / 2, title,
                         pw - 43, is_cur ? th->text : th->text_dim, is_cur);
        } else {
            ui_text_maxw(b->win, px + 6, py + (ph - GLYPH_H) / 2, title,
                         pw - 24, is_cur ? th->text : th->text_dim, is_cur);
        }
        tab_close_rect(b, i, &cx, &cy, &cw, &ch);
        ui_text(b->win, cx + 3, cy + (ch - GLYPH_H) / 2, "x",
                th->text_dim, 0);
    }
    tab_new_btn_rect(b, &nx, &ny, &nw, &nh);
    ui_button_th(b->win, th, nx, ny, nw, nh, "+",
                 b->tab_count < MAX_TABS, 0);
}

static void draw_toolbar(struct bstate *b, const gui_theme *th)
{
    struct browser_tab *t = active_tab(b);
    struct ui_btn_rect br[BTN_COUNT];
    char zoom_lbl[16];
    int starred, zoom_pct;

    toolbar_layout(b, br);
    gui_rect(b->win, 0, TAB_H, b->win->w, BAR_H, th->panel_hi);
    gui_line(b->win, 0, TAB_H + BAR_H - 1, b->win->w - 1,
             TAB_H + BAR_H - 1, th->border);

    ui_button_th(b->win, th, br[BTN_BACK].x, br[BTN_BACK].y,
                 br[BTN_BACK].w, br[BTN_BACK].h,
                 "Back", t && t->hist_i > 0, 0);
    ui_button_th(b->win, th, br[BTN_FWD].x, br[BTN_FWD].y,
                 br[BTN_FWD].w, br[BTN_FWD].h,
                 "Fwd", t && t->hist_i + 1 < t->hist_n, 0);
    ui_button_th(b->win, th, br[BTN_RELOAD].x, br[BTN_RELOAD].y,
                 br[BTN_RELOAD].w, br[BTN_RELOAD].h,
                 "Reload", t && t->page != 0, 0);

    ui_box(b->win, br[BTN_ADDR].x, br[BTN_ADDR].y,
           br[BTN_ADDR].w, br[BTN_ADDR].h, th->sunken,
           b->addr_focus ? th->accent : th->border);
    if (t) {
        int maxch = (br[BTN_ADDR].w - 8) / GLYPH_W;
        int from = t->addr_cur > maxch ? t->addr_cur - maxch : 0;
        int len = t->addr_len - from;

        if (len > maxch)
            len = maxch;
        if (len > 0)
            gui_text_n(b->win, br[BTN_ADDR].x + 4,
                       br[BTN_ADDR].y + (br[BTN_ADDR].h - GLYPH_H) / 2,
                       t->addr + from, len, th->text, GUI_TRANSPARENT);
        if (b->addr_focus) {
            int cx = br[BTN_ADDR].x + 4 + (t->addr_cur - from) * GLYPH_W;

            gui_rect(b->win, cx, br[BTN_ADDR].y + 4, 2,
                     br[BTN_ADDR].h - 8, th->accent);
        }
    }

    starred = (t && t->page && t->page->url)
                  ? bookmark_find_index(b, t->page->url) >= 0
                  : 0;
    zoom_pct = (t && t->zoom_pct > 0) ? t->zoom_pct : 100;
    snprintf(zoom_lbl, sizeof(zoom_lbl), "%d%%", zoom_pct);
    ui_button_th(b->win, th, br[BTN_STAR].x, br[BTN_STAR].y,
                 br[BTN_STAR].w, br[BTN_STAR].h,
                 "Star", t && t->page != 0, starred);
    ui_button_th(b->win, th, br[BTN_PIN].x, br[BTN_PIN].y,
                 br[BTN_PIN].w, br[BTN_PIN].h,
                 "Pin", t && t->page != 0, 0);
    ui_button_th(b->win, th, br[BTN_READER].x, br[BTN_READER].y,
                 br[BTN_READER].w, br[BTN_READER].h,
                 "Reader", t && t->page != 0, t && t->reader_mode);
    ui_button_th(b->win, th, br[BTN_ZOOM].x, br[BTN_ZOOM].y,
                 br[BTN_ZOOM].w, br[BTN_ZOOM].h,
                 zoom_lbl, t && t->page != 0, zoom_pct != 100);
    ui_button_th(b->win, th, br[BTN_FIND].x, br[BTN_FIND].y,
                 br[BTN_FIND].w, br[BTN_FIND].h,
                 "Find", t && t->page != 0, b->find_open);
    ui_button_th(b->win, th, br[BTN_DEV].x, br[BTN_DEV].y,
                 br[BTN_DEV].w, br[BTN_DEV].h,
                 "Dev", 1, b->dev_open);
}

static void draw_bookmarks(struct bstate *b, const gui_theme *th)
{
    int y0 = TAB_H + BAR_H;
    int i, rx, ry, rw, rh;

    gui_rect(b->win, 0, y0, b->win->w, BMARK_H, th->panel);
    gui_line(b->win, 0, y0 + BMARK_H - 1, b->win->w - 1,
             y0 + BMARK_H - 1, th->border);
    for (i = 0; i < b->bookmark_count; i++) {
        if (!bmark_chip_rect(b, i, &rx, &ry, &rw, &rh))
            break;
        ui_box(b->win, rx, ry, rw, rh, th->surface, th->border);
        ui_text_maxw(b->win, rx + 7, ry + (rh - GLYPH_H) / 2,
                     b->bookmarks[i].title, rw - 14, th->text, 0);
    }
}

static void draw_form_caret(struct bstate *b, const gui_theme *th)
{
    struct browser_tab *t = active_tab(b);
    struct browser_page *p = t ? t->page : 0;
    struct dom_node *fn = t ? t->focus_node : 0;
    const struct lay_box *fb;
    const char *type;
    const char *val;
    lay_rect cr;
    int cursor, cx, cy, ch;

    if (!fn || !fn->tag || !p || !p->layout || b->addr_focus || b->repl_focus)
        return;
    if (strcmp(fn->tag, "input") != 0 && strcmp(fn->tag, "textarea") != 0)
        return;
    type = dom_get_attr(fn, "type");
    if (!strcmp(fn->tag, "input") && type &&
        (!strcmp(type, "button") || !strcmp(type, "submit") ||
         !strcmp(type, "reset") || !strcmp(type, "checkbox") ||
         !strcmp(type, "radio") || !strcmp(type, "hidden") ||
         !strcmp(type, "range") || !strcmp(type, "color")))
        return;
    fb = lay_box_for_node(p->layout, fn);
    if (!fb)
        return;
    cr = lay_content_rect(fb);
    val = dom_get_attr(fn, "value");
    cursor = t->focus_cursor;
    if (cursor < 0)
        cursor = 0;
    if (!strcmp(fn->tag, "textarea")) {
        int line = 0, col = 0, i;
        for (i = 0; val && val[i] && i < cursor; i++) {
            if (val[i] == '\n') {
                line++;
                col = 0;
            } else {
                col++;
            }
        }
        cx = cr.x + 4 + col * GLYPH_W;
        cy = b->top_chrome_h + (cr.y - t->scroll) + 4 + line * GLYPH_H;
        ch = GLYPH_H;
    } else {
        cx = cr.x + 4 + cursor * GLYPH_W;
        cy = b->top_chrome_h + (cr.y - t->scroll) + (cr.h - GLYPH_H) / 2;
        ch = GLYPH_H;
    }
    if (cx < cr.x + 2)
        cx = cr.x + 2;
    if (cx > cr.x + cr.w - 3)
        cx = cr.x + cr.w - 3;
    if (cy < b->top_chrome_h || cy + ch > b->top_chrome_h + b->view_h)
        return;
    gui_rect(b->win, cx, cy, 2, ch, th->accent);
}

static void draw_select_dropdown(struct bstate *b, const gui_theme *th)
{
    struct browser_tab *t = active_tab(b);
    int dx, dy, dw, dh, vis, n, i, row_h = GLYPH_H + 6;
    int row_w, sb_w = 0;

    if (!select_dropdown_rect(b, &dx, &dy, &dw, &dh, &vis))
        return;
    n = select_option_count(t->select_open_node);
    ui_box(b->win, dx, dy, dw, dh, th->surface, th->accent);
    if (n > vis) {
        int track_h = dh - 2;
        int thumb_h = mul_div_i(track_h, vis, n);
        int thumb_y = dy + 1 + mul_div_i(track_h - thumb_h, t->select_top,
                                         n - vis);

        sb_w = SELECT_SB_W;
        gui_rect(b->win, dx + dw - 1 - sb_w, dy + 1, sb_w, track_h, th->panel);
        ui_box(b->win, dx + dw - 1 - sb_w, thumb_y, sb_w, thumb_h,
               th->surface_alt, th->border);
    }
    row_w = dw - 2 - sb_w;
    for (i = 0; i < vis; i++) {
        int idx = t->select_top + i;
        struct dom_node *opt = select_option_nth(t->select_open_node, idx);
        int ry = dy + 2 + i * row_h;
        int selected = opt && dom_has_attr(opt, "selected");
        int hover = (idx == t->select_hover);
        unsigned bg = hover ? th->accent : (selected ? th->panel_hi : th->surface);
        unsigned fg = hover ? th->text_inv : th->text;
        char *label;

        if (!opt)
            break;
        gui_rect(b->win, dx + 1, ry, row_w, row_h, bg);
        label = dom_text_content(opt, 0);
        if (label) {
            if (*label)
                ui_text_maxw(b->win, dx + 6, ry + 3, label, row_w - 10, fg,
                             selected);
            free(label);
        }
    }
}

static void draw_find_bar(struct bstate *b, const gui_theme *th, int find_y)
{
    char count_buf[48];

    gui_rect(b->win, 0, find_y, b->win->w, FIND_H, th->panel);
    gui_line(b->win, 0, find_y, b->win->w - 1, find_y, th->border);
    ui_text(b->win, PAD, find_y + (FIND_H - GLYPH_H) / 2, "Find:", th->text, 1);

    ui_box(b->win, PAD + 44, find_y + 3, 240, FIND_H - 6, th->sunken, th->accent);
    if (b->find_len > 0)
        gui_text_n(b->win, PAD + 48, find_y + (FIND_H - GLYPH_H) / 2,
                   b->find_query, b->find_len, th->text, GUI_TRANSPARENT);
    gui_rect(b->win, PAD + 48 + b->find_cur * GLYPH_W, find_y + 6,
             2, FIND_H - 12, th->accent);

    if (b->find_len == 0)
        snprintf(count_buf, sizeof(count_buf), "type query");
    else if (b->find_count == 0)
        snprintf(count_buf, sizeof(count_buf), "0/0");
    else
        snprintf(count_buf, sizeof(count_buf), "match %d/%d",
                 b->find_index + 1, b->find_count);
    ui_text(b->win, PAD + 292, find_y + (FIND_H - GLYPH_H) / 2,
            count_buf, th->text_dim, 0);
    ui_button_th(b->win, th, PAD + 392, find_y + 3, 44, FIND_H - 6, "Prev",
                 b->find_count > 0, 0);
    ui_button_th(b->win, th, PAD + 440, find_y + 3, 44, FIND_H - 6, "Next",
                 b->find_count > 0, 0);
    ui_button_th(b->win, th, b->win->w - PAD - 24, find_y + 3, 24, FIND_H - 6,
                 "x", 1, 0);
}

static void draw_dev_drawer(struct bstate *b, const gui_theme *th,
                            int dev_y, int dev_h)
{
    static const char *const names[3] = { "Elements", "Console", "Network" };
    struct browser_tab *t = active_tab(b);
    struct browser_page *p = t ? t->page : 0;
    int i, body_y, body_h, line_h = GLYPH_H + 2;

    if (!b->dev_open || dev_h <= DEV_HDR_H)
        return;
    gui_rect(b->win, 0, dev_y, b->win->w, dev_h, th->surface);
    gui_rect(b->win, 0, dev_y, b->win->w, DEV_HDR_H, th->panel);
    gui_line(b->win, 0, dev_y, b->win->w - 1, dev_y, th->accent);
    gui_line(b->win, 0, dev_y + DEV_HDR_H - 1, b->win->w - 1,
             dev_y + DEV_HDR_H - 1, th->border);
    for (i = 0; i < 3; i++)
        ui_button_th(b->win, th, PAD + i * 82, dev_y + 2, 78, DEV_HDR_H - 4,
                     names[i], 1, b->dev_tab == i);
    ui_button_th(b->win, th, b->win->w - PAD - 24, dev_y + 2, 24,
                 DEV_HDR_H - 4, "x", 1, 0);

    body_y = dev_y + DEV_HDR_H + 2;
    body_h = dev_h - DEV_HDR_H - 4;
    if (b->dev_tab == 1)
        body_h -= REPL_H;
    if (body_h < line_h)
        body_h = line_h;

    if (b->dev_tab == 0) {
        int total = 0, vis_rows = body_h / line_h;

        dev_collect_elem_line(p, -1, 0, 0, &total);
        if (t && t->elem_scroll > total - vis_rows)
            t->elem_scroll = total > vis_rows ? total - vis_rows : 0;
        if (t && t->elem_scroll < 0)
            t->elem_scroll = 0;
        for (i = 0; i < vis_rows && t && t->elem_scroll + i < total; i++) {
            dev_collect_elem_line(p, t->elem_scroll + i,
                                  b->draw_tmp, sizeof(b->draw_tmp), 0);
            ui_text_maxw(b->win, PAD, body_y + i * line_h,
                         b->draw_tmp, b->win->w - 2 * PAD, th->text, 0);
        }
    } else if (b->dev_tab == 1) {
        int vis_rows = body_h / line_h;
        int rx = PAD, ry = dev_y + dev_h - REPL_H + 2, rw = b->win->w - 2 * PAD;

        if (t) {
            if (t->con_scroll > t->console_count - vis_rows)
                t->con_scroll = t->console_count > vis_rows
                                    ? t->console_count - vis_rows : 0;
            if (t->con_scroll < 0)
                t->con_scroll = 0;
            for (i = 0; i < vis_rows && t->con_scroll + i < t->console_count; i++) {
                int slot = (t->console_head + t->con_scroll + i) % CONSOLE_LOG_MAX;
                const char *line = t->console_lines[slot];
                if (line)
                    ui_text_maxw(b->win, PAD, body_y + i * line_h,
                                 line, b->win->w - 2 * PAD, th->text, 0);
            }
        }
        ui_box(b->win, rx, ry, rw, REPL_H - 4, th->sunken,
               b->repl_focus ? th->accent : th->border);
        ui_text(b->win, rx + 4, ry + 2, ">", th->accent, 1);
        {
            int maxch = (rw - 24) / GLYPH_W;
            int from = b->repl_cur > maxch ? b->repl_cur - maxch : 0;
            int len = b->repl_len - from;
            if (len > maxch)
                len = maxch;
            if (len > 0)
                gui_text_n(b->win, rx + 16, ry + 2, b->repl + from, len,
                           th->text, GUI_TRANSPARENT);
            if (b->repl_focus) {
                int cx = rx + 16 + (b->repl_cur - from) * GLYPH_W;
                gui_rect(b->win, cx, ry + 2, 2, REPL_H - 8, th->accent);
            }
        }
    } else {
        int vis_rows = body_h / line_h;

        if (t) {
            if (t->net_scroll > t->net_count - vis_rows)
                t->net_scroll = t->net_count > vis_rows
                                    ? t->net_count - vis_rows : 0;
            if (t->net_scroll < 0)
                t->net_scroll = 0;
            for (i = 0; i < vis_rows && t->net_scroll + i < t->net_count; i++) {
                int slot = (t->net_head + t->net_scroll + i) % NET_LOG_MAX;
                const struct net_log_entry *e = &t->net_entries[slot];

                snprintf(b->draw_tmp, sizeof(b->draw_tmp),
                         "%4d  %-6s  %7luB  %s",
                         e->status, e->kind, e->bytes, e->url);
                ui_text_maxw(b->win, PAD, body_y + i * line_h, b->draw_tmp,
                             b->win->w - 2 * PAD,
                             e->status >= 400 || e->status < 0 ? th->error : th->text,
                             0);
            }
        }
    }
}

static BROWSER_NOINLINE void draw(struct bstate *b)
{
    const gui_theme *th = gui_theme_current();
    struct browser_tab *t = active_tab(b);
    struct browser_page *p = t ? t->page : 0;
    int dev_y, dev_h, find_y, stat_y;

    recompute_layout_dims(b, &dev_y, &dev_h, &find_y, &stat_y);
    gui_clear(b->win, C_BG);

    if (p && p->layout) {
        struct paint_target target;
        struct paint_opts opts;

        target.px = b->win->px;
        target.w = b->win->w;
        target.h = b->win->h;
        target.stride = b->win->w;
        paint_opts_init(&opts);
        opts.origin_x = 0;
        opts.origin_y = b->top_chrome_h;
        opts.scroll_x = 0;
        opts.scroll_y = t->scroll;
        opts.canvas = lay_canvas_color(p->layout);
        opts.image = page_paint_image;
        opts.image_ctx = p;
        if (b->find_open && b->find_box) {
            opts.highlight_box = b->find_box;
            opts.highlight_color = th->warn;
        } else if (t->focus_node) {
            opts.highlight_box = lay_box_for_node(p->layout, t->focus_node);
            opts.highlight_color = th->accent;
        }
        lay_paint(p->layout, &target,
                  lay_mkrect(0, b->top_chrome_h, b->view_w, b->view_h),
                  &opts, 0);
        draw_form_caret(b, th);
    }

    draw_tabs(b, th);
    draw_toolbar(b, th);
    draw_bookmarks(b, th);

    /* Scrollbar. */
    {
        int sx = b->view_w;
        int ch = content_h(b);
        int th_h, ty;

        gui_rect(b->win, sx, b->top_chrome_h, SB_W, b->view_h, th->panel);
        gui_frame(b->win, sx, b->top_chrome_h, SB_W, b->view_h, th->border);
        if (ch > b->view_h) {
            th_h = mul_div_i((int64_t)b->view_h, b->view_h, ch);
            if (th_h < 18)
                th_h = 18;
            ty = clamp_i64((int64_t)b->top_chrome_h +
                 mul_div_i((int64_t)b->view_h - th_h, t ? t->scroll : 0,
                           (int64_t)ch - b->view_h));
            ui_box(b->win, sx + 1, ty, SB_W - 2, th_h, th->surface_alt, th->border);
        }
    }

    draw_select_dropdown(b, th);
    if (b->dev_open)
        draw_dev_drawer(b, th, dev_y, dev_h);
    if (b->find_open)
        draw_find_bar(b, th, find_y);

    /* Status line. */
    gui_rect(b->win, 0, stat_y, b->win->w, STAT_H, th->panel);
    gui_line(b->win, 0, stat_y, b->win->w - 1, stat_y, th->border);
    ui_text_maxw(b->win, PAD, stat_y + 2,
                 b->hover_url[0] ? b->hover_url : b->status,
                 b->win->w - 2 * PAD,
                 (!b->hover_url[0] && b->status_err) ? th->error : th->text, 0);
    gui_flush(b->win);
}

static void history_discard_from(struct browser_tab *t, int first)
{
    int i;

    if (!t)
        return;
    for (i = first; i < t->hist_n; i++) {
        free(t->hist[i]);
        t->hist[i] = 0;
    }
    t->hist_n = first;
}

static void tab_destroy(struct browser_tab *t)
{
    int i;

    if (!t)
        return;
    page_destroy(t->page);
    t->page = 0;
    history_discard_from(t, 0);
    for (i = 0; i < CONSOLE_LOG_MAX; i++) {
        free(t->console_lines[i]);
        t->console_lines[i] = 0;
    }
    free(t->raw_source);
    t->raw_source = 0;
    free(t->favicon_url);
    t->favicon_url = 0;
    free(t);
}

/* Commit an already allocated URL.  Nothing below this point can fail, so
 * page/history replacement can be one transaction. */
static BROWSER_NOINLINE void
history_commit(struct browser_tab *t, char *copy, int push)
{
    if (!push && t->hist_i >= 0 && t->hist_i < t->hist_n) {
        free(t->hist[t->hist_i]);
        t->hist[t->hist_i] = copy;
        return;
    }

    if (t->hist_n > 0)
        history_discard_from(t, t->hist_i + 1);
    if (t->hist_n == HIST_MAX) {
        free(t->hist[0]);
        memmove(&t->hist[0], &t->hist[1],
                (HIST_MAX - 1) * sizeof(t->hist[0]));
        t->hist_n--;
        t->hist_i--;
    }
    t->hist[t->hist_n] = copy;
    t->hist_i = t->hist_n;
    t->hist_n++;
}

static void runtime_persist_state(struct browser_runtime *rt, int save_cookies)
{
    if (!rt)
        return;
    runtime_storage_save(rt);
    if (save_cookies && rt->http) {
        struct cookie_jar *jar = http_client_jar(rt->http);
        long now = syscall(SYS_TIME, 0, 0, 0, 0);

        if (jar)
            cookie_jar_save(jar, COOKIE_DEFAULT_FILE, now);
    }
}

static BROWSER_NOINLINE void session_clear_file(void)
{
    syscall(SYS_UNLINK, (long)SESSION_FILE, 0, 0, 0);
}

static BROWSER_NOINLINE void session_save(const struct bstate *b)
{
    struct bytebuf out;
    char line[64];
    int i, fd, active, saved = 0;
    unsigned long wrote = 0;

    if (!b || !b->rt || !b->rt->is_gui || b->restoring_session)
        return;
    if (b->tab_count <= 0) {
        session_clear_file();
        return;
    }
    memset(&out, 0, sizeof(out));
    out.limit = SESSION_MAX_BYTES;
    active = b->active_tab;
    if (active < 0)
        active = 0;
    if (active >= b->tab_count)
        active = b->tab_count - 1;
    snprintf(line, sizeof(line), "active=%d\n", active);
    if (buf_add_str(&out, line) != 0) {
        free(out.p);
        return;
    }
    for (i = 0; i < b->tab_count; i++) {
        const struct browser_tab *t = b->tabs[i];
        const char *url;
        int z;

        if (!t)
            continue;
        url = (t->page && t->page->url[0]) ? t->page->url : t->addr;
        if (!url || !url[0] || strchr(url, '\n') || strchr(url, '\r'))
            continue;
        z = t->zoom_pct > 0 ? t->zoom_pct : 100;
        snprintf(line, sizeof(line), "zoom=%d\n", z);
        if (buf_add_str(&out, line) != 0 ||
            buf_add_str(&out, "tab=") != 0 ||
            buf_add_str(&out, url) != 0 ||
            buf_add(&out, "\n", 1) != 0) {
            free(out.p);
            return;
        }
        saved++;
    }
    if (saved == 0) {
        free(out.p);
        session_clear_file();
        return;
    }
    syscall(SYS_MKDIR, (long)"/home", 0, 0, 0);
#ifdef JS_HOST
    fd = open(SESSION_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
#else
    fd = open(SESSION_FILE, O_WRONLY | O_CREAT | O_TRUNC);
#endif
    if (fd >= 0) {
        while (wrote < out.n) {
            long n = write(fd, out.p + wrote, out.n - wrote);
            if (n <= 0)
                break;
            wrote += (unsigned long)n;
        }
        close(fd);
    }
    free(out.p);
}

static BROWSER_NOINLINE void set_fragment_scroll(struct bstate *b)
{
    struct browser_tab *t = active_tab(b);
    struct url u;
    char id[URL_FRAG_MAX];
    int32_t y;

    if (!t)
        return;
    t->scroll = 0;
    if (!t->page || url_parse(t->page->url, &u) != URL_OK ||
        !u.has_fragment || !u.fragment[0])
        return;
    if (url_pct_decode(u.fragment, ~0UL, 0, id, sizeof(id)) < 0)
        return;
    if (lay_scroll_to_id(t->page->layout, id, &y))
        t->scroll = y;
    clamp_scroll(b);
}

static BROWSER_NOINLINE void
page_apply_to_tab(struct bstate *b, struct browser_tab *t,
                  struct browser_page *next, char *history_copy, int push)
{
    struct browser_page *old = t->page;
    gui_settings gs;

    gui_settings_get(&gs);
    t->page = next;
    t->focus_node = 0;
    t->focus_cursor = 0;
    t->hover_node = 0;
    t->active_node = 0;
    t->select_open_node = 0;
    t->last_vw = b->view_w;
    t->last_vh = b->view_h;
    t->last_prefers_dark = !gs.light;
    b->hover_url[0] = 0;

    if (history_copy)
        history_commit(t, history_copy, push);
    snprintf(t->addr, sizeof(t->addr), "%s", next->url);
    t->addr_len = (int)strlen(t->addr);
    t->addr_cur = t->addr_len;
    set_fragment_scroll(b);
    find_refresh(b, 0);

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
}

static BROWSER_NOINLINE int
load_gui_request(struct bstate *b, const char *url, int push,
                 const char *method, const void *body, unsigned long body_len,
                 const char *content_type)
{
    struct browser_tab *t = active_tab(b);
    struct browser_page *next;
    struct download_info dl;
    char *history_copy;
    int script_hops = 0;

    if (!t)
        return -1;
    recompute_layout_dims(b, 0, 0, 0, 0);
    t->reader_mode = 0;
    t->hover_node = 0;
    t->active_node = 0;
    t->focus_node = 0;
    sync_dom_pseudo_globals(b);
    set_status(b, 0, "loading", url);
    draw(b);
    next = page_load_request_tab(b->rt, t, url, method, body, body_len,
                                 content_type, b->view_w, b->view_h, &dl,
                                 b->load_error, sizeof(b->load_error));
    if (!next && dl.triggered) {
        snprintf(t->addr, sizeof(t->addr), "%s",
                 t->page ? t->page->url : "");
        t->addr_len = (int)strlen(t->addr);
        t->addr_cur = t->addr_len;
        if (dl.ok) {
            snprintf(b->load_error, sizeof(b->load_error),
                     "Saved %s (%lu bytes)", dl.path, dl.bytes);
            set_status(b, 0, b->load_error, "");
        } else {
            set_status(b, 1, "download failed:", dl.path);
        }
        return 0;
    }
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
        redirected = page_load_request_tab(b->rt, t, target, 0, 0, 0, 0,
                                           b->view_w, b->view_h, &dl,
                                           b->load_error,
                                           sizeof(b->load_error));
        free(target);
        if (!redirected)
            break;
        page_destroy(next);
        next = redirected;
    }
    if (!push && (t->hist_i < 0 || t->hist_i >= t->hist_n)) {
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

    page_apply_to_tab(b, t, next, history_copy, push);
    tab_load_favicon(b, t);
    runtime_persist_state(b->rt, 1);
    session_save(b);
    return 0;
}

static int load_gui(struct bstate *b, const char *url, int push)
{
    return load_gui_request(b, url, push, 0, 0, 0, 0);
}

struct download_link_work {
    char abs[URL_MAX];
    char fname[128];
    char path[256];
    char err[320];
    struct http_request req;
    struct http_response res;
};

static BROWSER_NOINLINE void
download_link(struct bstate *b, const char *raw_href, const char *suggested)
{
    struct browser_tab *t = active_tab(b);
    struct browser_page *p = t ? t->page : 0;
    struct download_link_work *w;
    char *local_data = 0;
    unsigned long local_len = 0;
    const void *data = 0;
    unsigned long dlen = 0;

    if (!t || !p || !raw_href || !*raw_href)
        return;
    w = (struct download_link_work *)calloc(1, sizeof(*w));
    if (!w) {
        set_status(b, 1, "out of memory downloading file", "");
        return;
    }
    if (resource_url(p, p->base_url ? p->base_url : p->url,
                     raw_href, w->abs, sizeof(w->abs),
                     w->err, sizeof(w->err)) != 0) {
        set_status(b, 1, "download blocked:", w->err);
        free(w);
        return;
    }
    set_status(b, 0, "downloading", w->abs);
    draw(b);
    if (url_is_scheme(w->abs, "file")) {
        if (read_local_url(w->abs, &local_data, &local_len,
                           w->err, sizeof(w->err)) != 0) {
            tab_net_log(t, -1, "dl", 0, w->abs);
            set_status(b, 1, "download failed:", w->err);
            free(w);
            return;
        }
        tab_net_log(t, 200, "dl", local_len, w->abs);
        if (suggested && *suggested)
            snprintf(w->fname, sizeof(w->fname), "%.*s",
                     (int)(sizeof(w->fname) - 1), suggested);
        else
            extract_download_filename(0, w->abs, w->fname, sizeof(w->fname));
        data = local_data;
        dlen = local_len;
    } else {
        int rc;
        const char *cd;

        w->req.url = w->abs;
        w->req.accept = "*/*";
        w->req.max_body = DOWNLOAD_MAX;
        rc = http_fetch(b->rt->http, &w->req, &w->res);
        if (rc != HTTP_OK) {
            tab_net_log(t, rc, "dl", 0, w->abs);
            set_status(b, 1, "download failed:", http_error_text(rc));
            http_response_free(&w->res);
            free(w);
            return;
        }
        tab_net_log(t, w->res.status, "dl", w->res.body_len,
                    w->res.final_url ? w->res.final_url : w->abs);
        if (w->res.status < 200 || w->res.status >= 300 ||
            w->res.body_len > DOWNLOAD_MAX) {
            snprintf(w->err, sizeof(w->err), "HTTP %d", w->res.status);
            set_status(b, 1, "download failed:", w->err);
            http_response_free(&w->res);
            free(w);
            return;
        }
        cd = http_header_get(&w->res, "content-disposition");
        if (suggested && *suggested)
            snprintf(w->fname, sizeof(w->fname), "%.*s",
                     (int)(sizeof(w->fname) - 1), suggested);
        else
            extract_download_filename(cd,
                                      w->res.final_url ? w->res.final_url
                                                       : w->abs,
                                      w->fname, sizeof(w->fname));
        data = w->res.body;
        dlen = w->res.body_len;
    }

    if (save_download_file(w->fname, data, dlen, w->path, sizeof(w->path)) == 0) {
        snprintf(w->err, sizeof(w->err), "Saved %s (%lu bytes)",
                 w->path, dlen);
        set_status(b, 0, w->err, "");
    } else {
        set_status(b, 1, "cannot write download:", w->path);
    }
    free(local_data);
    http_response_free(&w->res);
    free(w);
}

static BROWSER_NOINLINE void
navigate_relative(struct bstate *b, const char *href)
{
    struct browser_page *p = active_page(b);
    char *full;
    int rc;

    if (!p)
        return;
    full = (char *)malloc(URL_MAX);
    if (!full) {
        set_status(b, 1, "out of memory resolving link", "");
        return;
    }
    rc = url_resolve_str(p->base_url ? p->base_url : p->url, href,
                         full, URL_MAX);
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
    struct browser_tab *t = active_tab(b);
    char *target;
    int previous;

    if (!t || t->hist_i <= 0) {
        set_status(b, 1, "no page to go back to", "");
        return;
    }
    target = str_dup(t->hist[t->hist_i - 1]);
    if (!target) {
        set_status(b, 1, "out of memory opening history", "");
        return;
    }
    previous = t->hist_i;
    t->hist_i--;
    if (load_gui(b, target, 0) != 0)
        t->hist_i = previous;
    free(target);
}

static BROWSER_NOINLINE void go_fwd(struct bstate *b)
{
    struct browser_tab *t = active_tab(b);
    char *target;
    int previous;

    if (!t || t->hist_i + 1 >= t->hist_n) {
        set_status(b, 1, "no page to go forward to", "");
        return;
    }
    target = str_dup(t->hist[t->hist_i + 1]);
    if (!target) {
        set_status(b, 1, "out of memory opening history", "");
        return;
    }
    previous = t->hist_i;
    t->hist_i++;
    if (load_gui(b, target, 0) != 0)
        t->hist_i = previous;
    free(target);
}

static void line_buf_insert(char *buf, int cap, int *len, int *cur, char c)
{
    int i;

    if (*len + 1 >= cap)
        return;
    if (*cur < 0)
        *cur = 0;
    if (*cur > *len)
        *cur = *len;
    for (i = *len; i > *cur; i--)
        buf[i] = buf[i - 1];
    buf[(*cur)++] = c;
    (*len)++;
    buf[*len] = 0;
}

static void line_buf_delete(char *buf, int *len, int *cur, int before)
{
    int at = before ? *cur - 1 : *cur;
    int i;

    if (at < 0 || at >= *len)
        return;
    for (i = at; i < *len - 1; i++)
        buf[i] = buf[i + 1];
    (*len)--;
    buf[*len] = 0;
    if (before)
        (*cur)--;
}

static void line_buf_paste(char *buf, int cap, int *len, int *cur)
{
    char clip[512];
    int got = gui_clipboard_paste(clip, sizeof(clip) - 1);
    int i;

    if (got <= 0)
        return;
    for (i = 0; i < got; i++) {
        unsigned char c = (unsigned char)clip[i];
        if (c >= 32 && c < 127)
            line_buf_insert(buf, cap, len, cur, (char)c);
    }
}

static void restore_address(struct bstate *b)
{
    struct browser_tab *t = active_tab(b);
    const char *url = (t && t->page) ? t->page->url : "";

    if (!t)
        return;
    snprintf(t->addr, sizeof(t->addr), "%s", url);
    t->addr_len = (int)strlen(t->addr);
    t->addr_cur = t->addr_len;
}

struct address_work {
    char url[URL_MAX];
    char error[192];
};

static BROWSER_NOINLINE void open_address_bar(struct bstate *b)
{
    struct browser_tab *t = active_tab(b);
    struct address_work *w;

    if (!t)
        return;
    w = (struct address_work *)malloc(sizeof(*w));
    if (!w) {
        set_status(b, 1, "out of memory opening address", "");
        return;
    }
    if (canonicalize_input(t->addr, w->url, sizeof(w->url), w->error,
                           sizeof(w->error)) != 0) {
        set_status(b, 1, "bad address:", w->error);
    } else if (!url_supported(w->url)) {
        set_status(b, 1, "unsupported address:", w->url);
    } else {
        load_gui(b, w->url, 1);
    }
    free(w);
}

static BROWSER_NOINLINE void toggle_reader_mode(struct bstate *b)
{
    struct browser_tab *t = active_tab(b);
    struct browser_page *p = t ? t->page : 0;
    struct browser_page *next = 0;
    gui_settings gs;
    char *html = 0;
    unsigned long hlen = 0;
    int is_dark;

    if (!t || !p)
        return;
    recompute_layout_dims(b, 0, 0, 0, 0);
    gui_settings_get(&gs);
    is_dark = !gs.light;

    if (!t->reader_mode) {
        if (!p->doc)
            return;
        html = make_reader_source(p->doc, p->url, is_dark, &hlen);
        if (!html) {
            set_status(b, 1, "cannot format reader view", "");
            return;
        }
        next = page_from_memory(b->rt, t, p->url, html, hlen, 1, p->is_local,
                                p->status, b->view_w, b->view_h,
                                b->load_error, sizeof(b->load_error));
        free(html);
        if (!next) {
            set_status(b, 1, "reader view failed:", b->load_error);
            return;
        }
        t->reader_mode = 1;
        page_apply_to_tab(b, t, next, 0, 0);
        set_status(b, 0, "Reader View",
                   p->doc->title && p->doc->title[0] ? p->doc->title
                                                     : next->url);
    } else {
        if (!t->raw_source) {
            load_gui(b, p->url, 0);
            return;
        }
        if (t->raw_is_md) {
            html = make_markdown_source(t->raw_source, t->raw_len, is_dark,
                                        &hlen);
            if (!html) {
                load_gui(b, p->url, 0);
                return;
            }
            next = page_from_memory(b->rt, t, p->url, html, hlen, 1,
                                    p->is_local, p->status,
                                    b->view_w, b->view_h,
                                    b->load_error, sizeof(b->load_error));
            free(html);
        } else {
            next = page_from_memory(b->rt, t, p->url, t->raw_source,
                                    t->raw_len, p->is_html, p->is_local,
                                    p->status, b->view_w, b->view_h,
                                    b->load_error, sizeof(b->load_error));
        }
        if (!next) {
            set_status(b, 1, "cannot exit reader view:", b->load_error);
            return;
        }
        t->reader_mode = 0;
        page_apply_to_tab(b, t, next, 0, 0);
    }
}

static BROWSER_NOINLINE void tab_sync_active_view(struct bstate *b)
{
    struct browser_tab *t = active_tab(b);
    gui_settings gs;
    int is_dark;

    if (!t)
        return;
    recompute_layout_dims(b, 0, 0, 0, 0);
    sync_dom_pseudo_globals(b);
    b->hover_url[0] = 0;
    snprintf(b->status, sizeof(b->status), "%s", t->page_status);
    b->status_err = t->page_status_err;

    gui_settings_get(&gs);
    is_dark = !gs.light;
    if (t->page &&
        (t->last_vw != b->view_w || t->last_vh != b->view_h ||
         t->last_prefers_dark != is_dark)) {
        char err[256];

        if (t->last_vw != b->view_w || t->last_vh != b->view_h) {
            if (t->page->js)
                jsdom_notify_resize(t->page->js, b->view_w, b->view_h,
                                    err, sizeof(err));
        }
        if (t->raw_is_md && t->last_prefers_dark != is_dark &&
            !t->reader_mode && t->raw_source) {
            unsigned long hlen = 0;
            char *html = make_markdown_source(t->raw_source, t->raw_len,
                                              is_dark, &hlen);
            if (html) {
                struct browser_page *next = page_from_memory(
                    b->rt, t, t->page->url, html, hlen, 1,
                    t->page->is_local, t->page->status,
                    b->view_w, b->view_h, b->load_error,
                    sizeof(b->load_error));
                free(html);
                if (next) {
                    page_apply_to_tab(b, t, next, 0, 0);
                    return;
                }
            }
        }
        if (t->reader_mode && t->last_prefers_dark != is_dark && t->page->doc) {
            unsigned long hlen = 0;
            char *html = make_reader_source(t->page->doc, t->page->url,
                                            is_dark, &hlen);
            if (html) {
                struct browser_page *next = page_from_memory(
                    b->rt, t, t->page->url, html, hlen, 1,
                    t->page->is_local, t->page->status,
                    b->view_w, b->view_h, b->load_error,
                    sizeof(b->load_error));
                free(html);
                if (next) {
                    page_apply_to_tab(b, t, next, 0, 0);
                    return;
                }
            }
        }
        if (page_reflow(b->rt, t->page, b->view_w, b->view_h,
                        err, sizeof(err)) == 0) {
            t->last_vw = b->view_w;
            t->last_vh = b->view_h;
            t->last_prefers_dark = is_dark;
            clamp_scroll(b);
        }
    }
    find_refresh(b, 0);
}

static BROWSER_NOINLINE int tab_new_zoom(struct bstate *b, const char *url,
                                         int zoom_pct)
{
    struct browser_tab *t;
    int prev = b->active_tab;

    if (b->tab_count >= MAX_TABS) {
        set_status(b, 1, "tab limit reached (max 8)", "");
        return -1;
    }
    t = (struct browser_tab *)calloc(1, sizeof(*t));
    if (!t) {
        set_status(b, 1, "out of memory creating tab", "");
        return -1;
    }
    t->hist_i = -1;
    t->zoom_pct = (zoom_pct > 0) ? zoom_pct : 100;
    b->tabs[b->tab_count] = t;
    b->active_tab = b->tab_count++;
    b->addr_focus = 0;
    b->repl_focus = 0;
    b->hover_url[0] = 0;
    find_clear(b);
    if (load_gui(b, url ? url : BROWSER_HOME, 1) != 0) {
        if (!t->page && b->tab_count > 1) {
            tab_destroy(t);
            b->tabs[--b->tab_count] = 0;
            b->active_tab = prev;
            tab_sync_active_view(b);
        }
        return -1;
    }
    return 0;
}

static BROWSER_NOINLINE int tab_new(struct bstate *b, const char *url)
{
    return tab_new_zoom(b, url, 100);
}

static void tab_switch(struct bstate *b, int idx)
{
    if (idx < 0 || idx >= b->tab_count || idx == b->active_tab)
        return;
    b->active_tab = idx;
    b->addr_focus = 0;
    b->repl_focus = 0;
    tab_sync_active_view(b);
    session_save(b);
}

static BROWSER_NOINLINE void tab_close(struct bstate *b, int idx)
{
    int i;

    if (idx < 0 || idx >= b->tab_count)
        return;
    tab_destroy(b->tabs[idx]);
    for (i = idx; i + 1 < b->tab_count; i++)
        b->tabs[i] = b->tabs[i + 1];
    b->tabs[--b->tab_count] = 0;
    if (b->tab_count == 0) {
        b->active_tab = -1;
        b->quit = 1;
        session_clear_file();
        return;
    }
    if (b->active_tab >= b->tab_count)
        b->active_tab = b->tab_count - 1;
    else if (idx < b->active_tab)
        b->active_tab--;
    b->addr_focus = 0;
    b->repl_focus = 0;
    tab_sync_active_view(b);
    session_save(b);
}

struct session_work {
    char urls[MAX_TABS][URL_MAX];
    int zooms[MAX_TABS];
    int count;
    int active;
};

static int valid_zoom_pct(int z)
{
    return z == 90 || z == 100 || z == 125 || z == 150;
}

static BROWSER_NOINLINE int session_restore(struct bstate *b)
{
    struct session_work *sw;
    struct k_stat st;
    char *data, *p, *end;
    unsigned long got = 0;
    int fd, i, pending_zoom = 100;

    if (!b || !b->rt || !b->rt->is_gui)
        return 0;
    if (stat_(SESSION_FILE, &st) != 0 || st.is_dir ||
        st.size <= 0 || (unsigned long)st.size > SESSION_MAX_BYTES)
        return 0;
    fd = open(SESSION_FILE, O_RDONLY);
    if (fd < 0)
        return 0;
    data = (char *)malloc((unsigned long)st.size + 1);
    sw = (struct session_work *)calloc(1, sizeof(*sw));
    if (!data || !sw) {
        close(fd);
        free(data);
        free(sw);
        return 0;
    }
    while (got < (unsigned long)st.size) {
        long n = read(fd, data + got, (unsigned long)st.size - got);
        if (n <= 0)
            break;
        got += (unsigned long)n;
    }
    close(fd);
    data[got] = 0;

    p = data;
    end = data + got;
    while (p < end) {
        char *nl = p, *tail;

        while (nl < end && *nl != '\n' && *nl != '\r')
            nl++;
        tail = nl;
        *nl = 0;
        while (*p == ' ' || *p == '\t')
            p++;
        while (tail > p && (tail[-1] == ' ' || tail[-1] == '\t'))
            *--tail = 0;
        if (*p && *p != '#') {
            if (strncmp(p, "active=", 7) == 0) {
                sw->active = atoi(p + 7);
            } else if (strncmp(p, "zoom=", 5) == 0) {
                int z = atoi(p + 5);
                if (valid_zoom_pct(z))
                    pending_zoom = z;
            } else {
                const char *u = (strncmp(p, "tab=", 4) == 0) ? (p + 4) : p;
                if (*u && sw->count < MAX_TABS &&
                    canonicalize_input(u, sw->urls[sw->count], URL_MAX,
                                       b->load_error,
                                       sizeof(b->load_error)) == 0 &&
                    url_supported(sw->urls[sw->count])) {
                    sw->zooms[sw->count] = pending_zoom;
                    sw->count++;
                }
                pending_zoom = 100;
            }
        }
        p = nl + 1;
        while (p < end && (*p == '\n' || *p == '\r'))
            p++;
    }
    free(data);
    if (sw->count == 0) {
        free(sw);
        return 0;
    }

    b->restoring_session = 1;
    b->tabs[0] = (struct browser_tab *)calloc(1, sizeof(*b->tabs[0]));
    if (!b->tabs[0]) {
        b->restoring_session = 0;
        free(sw);
        return 0;
    }
    b->tabs[0]->hist_i = -1;
    b->tabs[0]->zoom_pct = valid_zoom_pct(sw->zooms[0]) ? sw->zooms[0] : 100;
    b->tab_count = 1;
    b->active_tab = 0;
    load_gui(b, sw->urls[0], 1);
    for (i = 1; i < sw->count; i++)
        tab_new_zoom(b, sw->urls[i],
                     valid_zoom_pct(sw->zooms[i]) ? sw->zooms[i] : 100);
    if (sw->active < 0)
        sw->active = 0;
    if (sw->active >= b->tab_count)
        sw->active = b->tab_count - 1;
    tab_switch(b, sw->active);
    b->restoring_session = 0;
    session_save(b);
    free(sw);
    return b->tab_count > 0;
}

static int tab_reflow_active(struct bstate *b, char *err, unsigned long errsz);

static BROWSER_NOINLINE void set_tab_zoom(struct bstate *b, int zoom_pct)
{
    struct browser_tab *t = active_tab(b);
    struct browser_page *p = t ? t->page : 0;
    char zbuf[24];
    char err[192];

    if (!t)
        return;
    if (!valid_zoom_pct(zoom_pct))
        zoom_pct = 100;
    if (t->zoom_pct == zoom_pct)
        return;
    t->zoom_pct = zoom_pct;
    if (p)
        tab_reflow_active(b, err, sizeof(err));
    snprintf(zbuf, sizeof(zbuf), "%d%%", t->zoom_pct);
    set_status(b, 0, "Zoom", zbuf);
    session_save(b);
}

static void cycle_tab_zoom(struct bstate *b)
{
    struct browser_tab *t = active_tab(b);
    int cur = (t && t->zoom_pct > 0) ? t->zoom_pct : 100;
    int next;

    if (cur == 100)
        next = 125;
    else if (cur == 125)
        next = 150;
    else if (cur == 150)
        next = 90;
    else
        next = 100;
    set_tab_zoom(b, next);
}

static void step_tab_zoom(struct bstate *b, int dir)
{
    static const int levels[4] = { 90, 100, 125, 150 };
    struct browser_tab *t = active_tab(b);
    int cur = (t && t->zoom_pct > 0) ? t->zoom_pct : 100;
    int i, idx = 1;

    for (i = 0; i < 4; i++) {
        if (levels[i] == cur) {
            idx = i;
            break;
        }
    }
    idx += dir;
    if (idx < 0)
        idx = 0;
    if (idx > 3)
        idx = 3;
    set_tab_zoom(b, levels[idx]);
}

static BROWSER_NOINLINE void open_view_source_tab(struct bstate *b)
{
    struct browser_tab *t = active_tab(b);
    struct browser_page *p = t ? t->page : 0;
    char *vs_url;
    unsigned long nlen;

    if (!p || !p->url[0])
        return;
    if (url_is_view_source(p->url, 0)) {
        set_status(b, 0, "Already viewing source", p->url);
        return;
    }
    nlen = 12 + strlen(p->url) + 1;
    vs_url = (char *)malloc(nlen);
    if (!vs_url) {
        set_status(b, 1, "out of memory opening view-source", "");
        return;
    }
    snprintf(vs_url, nlen, "view-source:%s", p->url);
    if (b->tab_count < MAX_TABS)
        tab_new(b, vs_url);
    else
        load_gui(b, vs_url, 1);
    free(vs_url);
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
    const char *t = n && (n->tag_id == HTAG_INPUT || n->tag_id == HTAG_BUTTON)
        ? dom_get_attr(n, "type") : 0;
    if (n && n->tag_id == HTAG_BUTTON)
        return t && (str_ci_eq(t, "button") || str_ci_eq(t, "reset"))
            ? t : "submit";
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

static int tab_reflow_active(struct bstate *b, char *err, unsigned long errsz)
{
    struct browser_tab *t = active_tab(b);
    struct browser_page *p = t ? t->page : 0;
    int rc;

    if (!p)
        return 0;
    b->find_box = 0;
    sync_dom_pseudo_globals(b);
    rc = page_reflow(b->rt, p, b->view_w, b->view_h, err, errsz);
    if (rc == 0) {
        if (b->find_open)
            find_refresh(b, 0);
        clamp_scroll(b);
    }
    return rc;
}

static int sync_script_page(struct bstate *b)
{
    struct browser_tab *t = active_tab(b);
    struct browser_page *p = t ? t->page : 0;
    char err[256];
    const char *pending;
    char *target;
    int url_sync;

    if (!p || !p->js)
        return 0;
    if (jsdom_pump(p->js, err, sizeof(err)) < 0)
        set_status(b, 1, "page timer failed:", err);
    url_sync = page_sync_document_url(p);
    if (url_sync < 0) {
        set_status(b, 1, "cannot update script history URL", "");
        return -1;
    }
    if (url_sync > 0 && !b->addr_focus)
        restore_address(b);
    if (jsdom_dirty(p->js)) {
        if (tab_reflow_active(b, err, sizeof(err)) != 0) {
            set_status(b, 1, "script changed the page, but reflow failed:", err);
            return -1;
        }
        jsdom_clear_dirty(p->js);
    }
    pending = jsdom_pending_navigation(p->js);
    if (!pending)
        return 0;
    target = str_dup(pending);
    jsdom_clear_navigation(p->js);
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
    struct browser_page *p = active_page(b);
    struct bytebuf data;
    const char *action, *method;
    char target[URL_MAX];
    char err[192];
    int allow;

    if (!p || !form || form->tag_id != HTAG_FORM)
        return;
    allow = !p->js ||
        jsdom_dispatch(p->js, form, "submit", err, sizeof(err));
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
        action = p->url;
    if (url_resolve_str(p->base_url ? p->base_url : p->url,
                        action, target, sizeof(target)) !=
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
    struct browser_tab *t = active_tab(b);
    struct browser_page *p = t ? t->page : 0;
    struct dom_node *n = t ? t->focus_node : 0;
    char *old, *value;
    unsigned long len, pos, extra = 2;
    int changed = 0;
    char err[160];
    char clip[512];
    int clip_n = 0;

    if (!p || !n || (n->tag_id != HTAG_INPUT && n->tag_id != HTAG_TEXTAREA))
        return 0;
    if (dom_has_attr(n, "disabled"))
        return 1;
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
    if (key == 3) {
        if (len > 0)
            gui_clipboard_copy(old, (int)len, "Browser input");
        free(old);
        return 1;
    }
    if (dom_has_attr(n, "readonly") && key != KEY_LEFT && key != KEY_RIGHT &&
        key != KEY_HOME && key != KEY_END &&
        !(n->tag_id == HTAG_INPUT && (key == '\n' || key == '\r'))) {
        free(old);
        return 1;
    }
    if (t->focus_cursor < 0 || (unsigned long)t->focus_cursor > len)
        t->focus_cursor = (int)len;
    pos = (unsigned long)t->focus_cursor;
    if (key == 22) {
        clip_n = gui_clipboard_paste(clip, sizeof(clip) - 1);
        if (clip_n > 0)
            extra = (unsigned long)clip_n + 2;
    }
    value = (char *)malloc(len + extra);
    if (!value) {
        free(old);
        return 1;
    }
    memcpy(value, old, len + 1);
    if (key == KEY_LEFT) {
        if (t->focus_cursor > 0)
            t->focus_cursor--;
    } else if (key == KEY_RIGHT) {
        if ((unsigned long)t->focus_cursor < len)
            t->focus_cursor++;
    } else if (key == KEY_HOME) {
        t->focus_cursor = 0;
    } else if (key == KEY_END) {
        t->focus_cursor = (int)len;
    } else if (key == 8 && pos > 0) {
        memmove(value + pos - 1, value + pos, len - pos + 1);
        t->focus_cursor--;
        changed = 1;
    } else if (key == KEY_DELETE && pos < len) {
        memmove(value + pos, value + pos + 1, len - pos);
        changed = 1;
    } else if (key == 22 && clip_n > 0) {
        int i;
        unsigned long ins = 0;

        for (i = 0; i < clip_n && len + ins < 4096; i++) {
            unsigned char c = (unsigned char)clip[i];
            if ((c >= 32 && c < 127) ||
                (c == '\n' && n->tag_id == HTAG_TEXTAREA))
                ins++;
        }
        if (ins > 0) {
            unsigned long dst = pos;
            memmove(value + pos + ins, value + pos, len - pos + 1);
            for (i = 0; i < clip_n && dst < pos + ins; i++) {
                unsigned char c = (unsigned char)clip[i];
                if ((c >= 32 && c < 127) ||
                    (c == '\n' && n->tag_id == HTAG_TEXTAREA))
                    value[dst++] = (char)c;
            }
            t->focus_cursor = (int)dst;
            changed = 1;
        }
    } else if ((key == '\n' || key == '\r') && n->tag_id == HTAG_TEXTAREA &&
               len < 4096) {
        memmove(value + pos + 1, value + pos, len - pos + 1);
        value[pos] = '\n';
        t->focus_cursor++;
        changed = 1;
    } else if ((key == '\n' || key == '\r') && n->tag_id == HTAG_INPUT) {
        struct dom_node *form = element_ancestor(n, HTAG_FORM);
        free(value);
        free(old);
        if (form)
            submit_form(b, form, 0);
        return 1;
    } else if (key >= 32 && key < 127 && len < 4096) {
        memmove(value + pos + 1, value + pos, len - pos + 1);
        value[pos] = (char)key;
        t->focus_cursor++;
        changed = 1;
    }
    if (changed) {
        dom_set_attr(n, "value", value);
        if (p->js)
            jsdom_dispatch(p->js, n, "input", err, sizeof(err));
        if (p->js)
            sync_script_page(b);
        else
            tab_reflow_active(b, err, sizeof(err));
    }
    free(value);
    free(old);
    return 1;
}

static int handle_line_edit_key(char *buf, int cap, int *len, int *cur,
                                unsigned int key)
{
    switch (key) {
    case 8:
        line_buf_delete(buf, len, cur, 1);
        return 1;
    case KEY_DELETE:
        line_buf_delete(buf, len, cur, 0);
        return 1;
    case KEY_LEFT:
        if (*cur > 0)
            (*cur)--;
        return 1;
    case KEY_RIGHT:
        if (*cur < *len)
            (*cur)++;
        return 1;
    case KEY_HOME:
        *cur = 0;
        return 1;
    case KEY_END:
        *cur = *len;
        return 1;
    case 21:                                  /* ctrl-U */
        buf[0] = 0;
        *len = *cur = 0;
        return 1;
    case 22:                                  /* ctrl-V */
        line_buf_paste(buf, cap, len, cur);
        return 1;
    default:
        if (key >= 32 && key < 127) {
            line_buf_insert(buf, cap, len, cur, (char)key);
            return 1;
        }
        return 0;
    }
}

static void select_choose_option(struct bstate *b, struct dom_node *select,
                                 struct dom_node *opt);

/* Keyboard navigation for an open <select> dropdown.  Returns 1 when the
 * key was consumed. */
static int select_dropdown_key(struct bstate *b, unsigned int key)
{
    struct browser_tab *t = active_tab(b);
    struct dom_node *sel = t ? t->select_open_node : 0;
    int dx, dy, dw, dh, vis, n, idx;

    if (!sel || !select_dropdown_rect(b, &dx, &dy, &dw, &dh, &vis))
        return 0;
    n = select_option_count(sel);
    idx = t->select_hover >= 0 ? t->select_hover : select_selected_index(sel);
    switch (key) {
    case KEY_UP:   idx--;        break;
    case KEY_DOWN: idx++;        break;
    case KEY_PGUP: idx -= vis;   break;
    case KEY_PGDN: idx += vis;   break;
    case KEY_HOME: idx = 0;      break;
    case KEY_END:  idx = n - 1;  break;
    case '\n':
    case '\r':
    case ' ': {
        struct dom_node *opt = select_option_nth(sel, t->select_hover);

        if (opt)
            select_choose_option(b, sel, opt);
        else
            select_close(t);
        return 1;
    }
    default:
        return 0;
    }
    if (idx < 0)
        idx = 0;
    if (idx > n - 1)
        idx = n - 1;
    t->select_hover = idx;
    select_reveal(t, idx, vis);
    return 1;
}

static BROWSER_NOINLINE void on_key(struct bstate *b, unsigned int key)
{
    struct browser_tab *t = active_tab(b);
    struct browser_page *p = t ? t->page : 0;
    int page_step = b->view_h - GLYPH_H * 2;

    if (page_step < GLYPH_H)
        page_step = GLYPH_H;
    if (select_dropdown_key(b, key))
        return;
    switch (key) {
    case 17:                                  /* ctrl-Q */
        b->quit = 1;
        return;
    case 20:                                  /* ctrl-T */
        tab_new(b, BROWSER_HOME);
        return;
    case 23:                                  /* ctrl-W */
        tab_close(b, b->active_tab);
        return;
    case 14:                                  /* ctrl-N */
        if (b->tab_count > 1)
            tab_switch(b, (b->active_tab + 1) % b->tab_count);
        return;
    case 16:                                  /* ctrl-P */
        if (b->tab_count > 1)
            tab_switch(b, (b->active_tab + b->tab_count - 1) % b->tab_count);
        return;
    case 12:                                  /* ctrl-L */
        b->addr_focus = 1;
        b->repl_focus = 0;
        if (t) {
            t->focus_node = 0;
            t->addr_cur = t->addr_len;
            select_close(t);
        }
        return;
    case 18:                                  /* ctrl-R */
        if (p)
            load_gui(b, p->url, 0);
        return;
    case 2:                                   /* ctrl-B */
        go_back(b);
        return;
    case 5:                                   /* ctrl-E */
        go_fwd(b);
        return;
    case 4:                                   /* ctrl-D */
        bookmark_toggle(b);
        return;
    case 15:                                  /* ctrl-O */
        toggle_reader_mode(b);
        return;
    case 6:                                   /* ctrl-F */
        b->find_open = !b->find_open;
        b->addr_focus = 0;
        b->repl_focus = 0;
        if (t) {
            t->focus_node = 0;
            select_close(t);
        }
        if (!b->find_open)
            find_clear(b);
        else
            b->find_cur = b->find_len;
        tab_sync_active_view(b);
        return;
    case 7:                                   /* ctrl-G */
        if (b->find_open)
            find_step(b, 1);
        return;
    case 11:                                  /* ctrl-K */
        b->dev_open = !b->dev_open;
        b->addr_focus = 0;
        b->repl_focus = (b->dev_open && b->dev_tab == 1);
        if (b->repl_focus && t) {
            t->focus_node = 0;
            select_close(t);
        }
        tab_sync_active_view(b);
        return;
    case 27:                                  /* Esc */
        if (t && t->select_open_node) {
            t->select_open_node = 0;
            return;
        }
        if (b->addr_focus) {
            b->addr_focus = 0;
            restore_address(b);
            return;
        }
        if (b->repl_focus) {
            b->repl_focus = 0;
            return;
        }
        if (b->find_open) {
            b->find_open = 0;
            find_clear(b);
            tab_sync_active_view(b);
            return;
        }
        return;
    default:
        break;
    }

    if (b->addr_focus && t) {
        if (key == '\n' || key == '\r') {
            b->addr_focus = 0;
            open_address_bar(b);
            return;
        }
        if (key == 3) {
            if (t->addr_len > 0) {
                gui_clipboard_copy(t->addr, t->addr_len, "Browser URL");
                set_status(b, 0, "Copied URL to clipboard", t->addr);
            }
            return;
        }
        handle_line_edit_key(t->addr, (int)sizeof(t->addr),
                             &t->addr_len, &t->addr_cur, key);
        return;
    }

    if (b->dev_open && b->dev_tab == 1 && b->repl_focus) {
        if (key == '\n' || key == '\r') {
            dev_exec_repl(b);
            return;
        }
        if (key == 3) {
            if (b->repl_len > 0)
                gui_clipboard_copy(b->repl, b->repl_len, "Browser REPL");
            return;
        }
        if (handle_line_edit_key(b->repl, (int)sizeof(b->repl),
                                 &b->repl_len, &b->repl_cur, key))
            return;
    }

    if (b->find_open && (!t || !t->focus_node) && !b->repl_focus) {
        if (key == '\n' || key == '\r') {
            find_step(b, 1);
            return;
        }
        if (handle_line_edit_key(b->find_query, (int)sizeof(b->find_query),
                                 &b->find_len, &b->find_cur, key)) {
            find_refresh(b, 1);
            return;
        }
    }

    if (edit_focused_control(b, key))
        return;

    if (!t)
        return;
    switch (key) {
    case 21:       open_view_source_tab(b); return;  /* ctrl-U */
    case '0':      set_tab_zoom(b, 100);    return;
    case '+':
    case '=':      step_tab_zoom(b, 1);     return;
    case '-':
    case '_':      step_tab_zoom(b, -1);    return;
    case KEY_UP:   t->scroll -= GLYPH_H * 2; break;
    case KEY_DOWN: t->scroll += GLYPH_H * 2; break;
    case KEY_PGUP: t->scroll -= page_step;   break;
    case KEY_PGDN: t->scroll += page_step;   break;
    case ' ':      t->scroll += page_step;   break;
    case KEY_HOME: t->scroll = 0;            break;
    case KEY_END:  t->scroll = content_h(b); break;
    default: return;
    }
    clamp_scroll(b);
    if (p && p->js)
        jsdom_set_scroll(p->js, 0, t->scroll);
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

static void select_choose_option(struct bstate *b, struct dom_node *select,
                                 struct dom_node *opt)
{
    struct browser_tab *t = active_tab(b);
    struct browser_page *p = t ? t->page : 0;
    struct dom_node *n;
    char err[192];

    if (!t || !select || !opt || dom_has_attr(opt, "disabled"))
        return;
    if (!dom_has_attr(select, "multiple")) {
        for (n = select->first_child; n; n = dom_next_within(n, select)) {
            if (n->type == DOM_ELEMENT && n->tag_id == HTAG_OPTION)
                dom_remove_attr(n, "selected");
        }
        dom_set_attr(opt, "selected", "");
    } else {
        if (dom_has_attr(opt, "selected"))
            dom_remove_attr(opt, "selected");
        else
            dom_set_attr(opt, "selected", "");
    }
    t->select_open_node = 0;
    t->select_hover = -1;
    if (p && p->js)
        jsdom_dispatch(p->js, select, "change", err, sizeof(err));
    if (p && p->js)
        sync_script_page(b);
    else
        tab_reflow_active(b, err, sizeof(err));
}

static BROWSER_NOINLINE void on_click(struct bstate *b, int x, int y)
{
    struct browser_tab *t = active_tab(b);
    struct browser_page *p = t ? t->page : 0;
    int dev_y, dev_h, find_y, stat_y;

    if (t && t->select_open_node) {
        int dx, dy, dw, dh;

        if (select_dropdown_rect(b, &dx, &dy, &dw, &dh, 0) &&
            pt_in_rect(x, y, dx, dy, dw, dh)) {
            struct dom_node *opt =
                select_option_nth(t->select_open_node,
                                  select_option_at(b, x, y));

            if (opt)
                select_choose_option(b, t->select_open_node, opt);
            return;
        }
        select_close(t);
    }

    if (y < TAB_H) {
        int i, nx, ny, nw, nh;

        b->addr_focus = 0;
        b->repl_focus = 0;
        tab_new_btn_rect(b, &nx, &ny, &nw, &nh);
        if (pt_in_rect(x, y, nx, ny, nw, nh)) {
            tab_new(b, BROWSER_HOME);
            return;
        }
        for (i = 0; i < b->tab_count; i++) {
            int px, py, pw, ph, cx, cy, cw, ch;

            tab_close_rect(b, i, &cx, &cy, &cw, &ch);
            if (pt_in_rect(x, y, cx, cy, cw, ch)) {
                tab_close(b, i);
                return;
            }
            tab_pill_rect(b, i, &px, &py, &pw, &ph);
            if (pt_in_rect(x, y, px, py, pw, ph)) {
                tab_switch(b, i);
                return;
            }
        }
        return;
    }

    if (y < TAB_H + BAR_H) {
        struct ui_btn_rect br[BTN_COUNT];

        toolbar_layout(b, br);
        b->addr_focus = 0;
        b->repl_focus = 0;
        if (pt_in_rect(x, y, br[BTN_BACK].x, br[BTN_BACK].y,
                       br[BTN_BACK].w, br[BTN_BACK].h)) {
            go_back(b);
        } else if (pt_in_rect(x, y, br[BTN_FWD].x, br[BTN_FWD].y,
                              br[BTN_FWD].w, br[BTN_FWD].h)) {
            go_fwd(b);
        } else if (pt_in_rect(x, y, br[BTN_RELOAD].x, br[BTN_RELOAD].y,
                              br[BTN_RELOAD].w, br[BTN_RELOAD].h)) {
            if (p)
                load_gui(b, p->url, 0);
        } else if (pt_in_rect(x, y, br[BTN_STAR].x, br[BTN_STAR].y,
                              br[BTN_STAR].w, br[BTN_STAR].h)) {
            bookmark_toggle(b);
        } else if (pt_in_rect(x, y, br[BTN_PIN].x, br[BTN_PIN].y,
                              br[BTN_PIN].w, br[BTN_PIN].h)) {
            if (p && p->url && *p->url) {
                gui_clipboard_copy(p->url, (int)strlen(p->url),
                                   "Browser Pin");
                set_status(b, 0, "Pinned URL to clipboard", p->url);
            }
        } else if (pt_in_rect(x, y, br[BTN_READER].x, br[BTN_READER].y,
                              br[BTN_READER].w, br[BTN_READER].h)) {
            toggle_reader_mode(b);
        } else if (pt_in_rect(x, y, br[BTN_ZOOM].x, br[BTN_ZOOM].y,
                              br[BTN_ZOOM].w, br[BTN_ZOOM].h)) {
            cycle_tab_zoom(b);
        } else if (pt_in_rect(x, y, br[BTN_FIND].x, br[BTN_FIND].y,
                              br[BTN_FIND].w, br[BTN_FIND].h)) {
            b->find_open = !b->find_open;
            if (!b->find_open)
                find_clear(b);
            else
                b->find_cur = b->find_len;
            tab_sync_active_view(b);
        } else if (pt_in_rect(x, y, br[BTN_DEV].x, br[BTN_DEV].y,
                              br[BTN_DEV].w, br[BTN_DEV].h)) {
            b->dev_open = !b->dev_open;
            b->repl_focus = (b->dev_open && b->dev_tab == 1);
            tab_sync_active_view(b);
        } else if (t && pt_in_rect(x, y, br[BTN_ADDR].x, br[BTN_ADDR].y,
                                   br[BTN_ADDR].w, br[BTN_ADDR].h)) {
            int maxch = (br[BTN_ADDR].w - 8) / GLYPH_W;
            int from;

            b->addr_focus = 1;
            t->focus_node = 0;
            from = t->addr_cur > maxch ? t->addr_cur - maxch : 0;
            t->addr_cur = from + (x - br[BTN_ADDR].x - 4) / GLYPH_W;
            if (t->addr_cur > t->addr_len)
                t->addr_cur = t->addr_len;
            if (t->addr_cur < 0)
                t->addr_cur = 0;
        }
        return;
    }

    if (y < b->top_chrome_h) {
        int i, rx, ry, rw, rh;

        b->addr_focus = 0;
        b->repl_focus = 0;
        for (i = 0; i < b->bookmark_count; i++) {
            if (!bmark_chip_rect(b, i, &rx, &ry, &rw, &rh))
                break;
            if (pt_in_rect(x, y, rx, ry, rw, rh)) {
                canonicalize_match_url(b->bookmarks[i].url,
                                       b->draw_tmp, sizeof(b->draw_tmp));
                if (url_supported(b->draw_tmp))
                    load_gui(b, b->draw_tmp, 1);
                return;
            }
        }
        return;
    }

    recompute_layout_dims(b, &dev_y, &dev_h, &find_y, &stat_y);
    if (y >= stat_y)
        return;
    b->addr_focus = 0;

    if (b->find_open && y >= find_y) {
        b->repl_focus = 0;
        if (t)
            t->focus_node = 0;
        if (x >= b->win->w - PAD - 24) {
            b->find_open = 0;
            find_clear(b);
            tab_sync_active_view(b);
        } else if (pt_in_rect(x, y, PAD + 392, find_y + 3, 44, FIND_H - 6)) {
            find_step(b, -1);
        } else if (pt_in_rect(x, y, PAD + 440, find_y + 3, 44, FIND_H - 6)) {
            find_step(b, 1);
        } else {
            b->find_cur = (x - (PAD + 48)) / GLYPH_W;
            if (b->find_cur < 0)
                b->find_cur = 0;
            if (b->find_cur > b->find_len)
                b->find_cur = b->find_len;
        }
        return;
    }

    if (b->dev_open && y >= dev_y) {
        if (y < dev_y + DEV_HDR_H) {
            int i;

            if (x >= b->win->w - PAD - 24) {
                b->dev_open = 0;
                b->repl_focus = 0;
                tab_sync_active_view(b);
                return;
            }
            for (i = 0; i < 3; i++) {
                if (pt_in_rect(x, y, PAD + i * 82, dev_y + 2, 78,
                               DEV_HDR_H - 4)) {
                    b->dev_tab = i;
                    b->repl_focus = (i == 1);
                    if (b->repl_focus && t)
                        t->focus_node = 0;
                    return;
                }
            }
            return;
        }
        if (b->dev_tab == 1 && y >= dev_y + dev_h - REPL_H) {
            b->repl_focus = 1;
            if (t)
                t->focus_node = 0;
        }
        return;
    }

    b->repl_focus = 0;
    if (!t)
        return;

    if (x >= b->view_w) {
        int ch = content_h(b);

        if (ch > b->view_h) {
            int rel = y - b->top_chrome_h;

            t->scroll = mul_div_i(rel, (int64_t)ch - b->view_h,
                                  b->view_h ? b->view_h : 1);
            clamp_scroll(b);
            if (p && p->js)
                jsdom_set_scroll(p->js, 0, t->scroll);
            b->sb_drag = 1;
        }
        return;
    }

    if (p && p->layout) {
        const char *href = 0;
        struct lay_box *hbox;
        struct dom_node *node, *action = 0, *link = 0, *form = 0;
        char event_error[192];
        int allow = 1;
        int vx = x;
        int vy = y - b->top_chrome_h;
        int doc_x = vx;
        int doc_y = vy + t->scroll;

        hbox = lay_hit_test_view(p->layout, vx, vy, 0, t->scroll);
        node = hbox ? hbox->node : lay_node_at(p->layout, doc_x, doc_y);
        link = element_ancestor(node, HTAG_A);
        for (action = node; action; action = action->parent) {
            if (action->type != DOM_ELEMENT)
                continue;
            if (action->tag_id == HTAG_A || action->tag_id == HTAG_INPUT ||
                action->tag_id == HTAG_BUTTON ||
                action->tag_id == HTAG_SELECT ||
                action->tag_id == HTAG_TEXTAREA ||
                (action->tag_id == HTAG_SUMMARY && action->parent &&
                 action->parent->tag_id == HTAG_DETAILS))
                break;
        }
        if (!action)
            action = node;
        t->active_node = action;
        if (action && action->type == DOM_ELEMENT &&
            !dom_has_attr(action, "disabled") &&
            (action->tag_id == HTAG_INPUT ||
             action->tag_id == HTAG_TEXTAREA ||
             action->tag_id == HTAG_SELECT ||
             action->tag_id == HTAG_BUTTON)) {
            t->focus_node = action;
            {
                char *v = control_value(action);
                t->focus_cursor = v ? (int)strlen(v) : 0;
                free(v);
            }
        } else {
            t->focus_node = 0;
        }
        if (p->js && action)
            allow = jsdom_dispatch(p->js, action, "click",
                                   event_error, sizeof(event_error));
        if (allow && action && action->type == DOM_ELEMENT) {
            if (action->tag_id == HTAG_INPUT &&
                str_ci_eq(control_type(action), "checkbox")) {
                if (dom_has_attr(action, "checked"))
                    dom_remove_attr(action, "checked");
                else
                    dom_set_attr(action, "checked", "");
                if (p->js)
                    jsdom_dispatch(p->js, action, "change",
                                   event_error, sizeof(event_error));
            } else if (action->tag_id == HTAG_INPUT &&
                       str_ci_eq(control_type(action), "radio")) {
                radio_select(p, action);
                if (p->js)
                    jsdom_dispatch(p->js, action, "change",
                                   event_error, sizeof(event_error));
            } else if (action->tag_id == HTAG_SELECT) {
                if (t->select_open_node == action)
                    select_close(t);
                else
                    select_open(b, action);
            } else if (action->tag_id == HTAG_SUMMARY && action->parent &&
                       action->parent->tag_id == HTAG_DETAILS) {
                struct dom_node *det = action->parent;
                if (dom_has_attr(det, "open"))
                    dom_remove_attr(det, "open");
                else
                    dom_set_attr(det, "open", "");
                if (p->js)
                    jsdom_dispatch(p->js, det, "toggle",
                                   event_error, sizeof(event_error));
            }
        }
        if (p->js) {
            int sres = sync_script_page(b);
            if (sres != 0)
                return;
        }
        if (allow && action && action->type == DOM_ELEMENT &&
            (action->tag_id == HTAG_BUTTON || action->tag_id == HTAG_INPUT) &&
            str_ci_eq(control_type(action), "submit")) {
            form = element_ancestor(action, HTAG_FORM);
            if (form) {
                submit_form(b, form, action);
                return;
            }
        }
        href = link ? dom_get_attr(link, "href") : 0;
        if (!href && lay_link_at(p->layout, doc_x, doc_y, &href))
            link = 0;
        if (allow && href && *href) {
            if (link && dom_has_attr(link, "download")) {
                download_link(b, href, dom_get_attr(link, "download"));
                return;
            }
            navigate_relative(b, href);
            return;
        }
        if (allow && ((action && action->type == DOM_ELEMENT &&
                       (action->tag_id == HTAG_INPUT ||
                        action->tag_id == HTAG_SELECT ||
                        action->tag_id == HTAG_BUTTON ||
                        action->tag_id == HTAG_SUMMARY)) ||
                      p->has_hover_css)) {
            tab_reflow_active(b, event_error, sizeof(event_error));
        }
    }
}

static BROWSER_NOINLINE int gui_update_hover(struct bstate *b, int x, int y)
{
    struct browser_tab *t = active_tab(b);
    struct browser_page *p = t ? t->page : 0;
    struct dom_node *next_hover = 0, *old_link, *new_link = 0;
    const char *href = 0;
    int need_draw = 0;
    char err[192];

    if (!t)
        return 0;
    if (t->select_open_node) {
        int hover_idx = select_option_at(b, x, y);

        if (hover_idx != t->select_hover) {
            t->select_hover = hover_idx;
            return 1;
        }
        return 0;
    }

    b->draw_tmp[0] = 0;
    if (p && p->layout &&
        x >= 0 && x < b->view_w &&
        y >= b->top_chrome_h && y < b->top_chrome_h + b->view_h) {
        struct lay_box *hbox =
            lay_hit_test_view(p->layout, x, y - b->top_chrome_h, 0, t->scroll);

        next_hover = hbox ? hbox->node : 0;
        if (next_hover && next_hover->type == DOM_TEXT && next_hover->parent)
            next_hover = next_hover->parent;
        new_link = element_ancestor(next_hover, HTAG_A);
        href = new_link ? dom_get_attr(new_link, "href") : 0;
        if (href && *href) {
            if (url_resolve_str(p->base_url ? p->base_url : p->url,
                                href, b->draw_tmp, sizeof(b->draw_tmp)) != URL_OK)
                snprintf(b->draw_tmp, sizeof(b->draw_tmp), "%.*s",
                         (int)(sizeof(b->draw_tmp) - 1), href);
        }
    }

    if (strcmp(b->hover_url, b->draw_tmp) != 0) {
        snprintf(b->hover_url, sizeof(b->hover_url), "%s", b->draw_tmp);
        need_draw = 1;
    }
    if (next_hover != t->hover_node) {
        old_link = element_ancestor(t->hover_node, HTAG_A);
        t->hover_node = next_hover;
        if (p && (p->has_hover_css || old_link != new_link)) {
            tab_reflow_active(b, err, sizeof(err));
            need_draw = 1;
        }
    }
    return need_draw;
}

static BROWSER_NOINLINE int
gui_mode(struct browser_runtime *rt, const char *start_url, int allow_restore)
{
    struct bstate *b;
    struct k_event ev;
    struct k_fbinfo fb;
    int i, idle_ticks = 0, result = 0;

    if (syscall(SYS_FBINFO, (long)&fb, 0, 0, 0) != 0 || !fb.present) {
        printf("browser: no framebuffer; use 'browser -t <url>'\n");
        return 1;
    }
    b = (struct bstate *)calloc(1, sizeof(*b));
    if (!b) {
        printf("browser: out of memory creating the window state\n");
        return 1;
    }
    b->rt = rt;
    b->win = gui_open("Kestrel Browser", 40, 40, WIN_W, WIN_H, 0);
    if (!b->win) {
        printf("browser: cannot open a window (is the desktop running?)\n");
        printf("browser: use 'browser -t <url>' for text mode\n");
        free(b);
        return 1;
    }
    bookmarks_load(b);
    recompute_layout_dims(b, 0, 0, 0, 0);

    if (!allow_restore || !session_restore(b)) {
        b->tabs[0] = (struct browser_tab *)calloc(1, sizeof(*b->tabs[0]));
        if (!b->tabs[0]) {
            favicon_cache_free_all(b);
            gui_close(b->win);
            free(b);
            return 1;
        }
        b->tabs[0]->hist_i = -1;
        b->tabs[0]->zoom_pct = 100;
        b->tab_count = 1;
        b->active_tab = 0;

        if (load_gui(b, start_url, 1) != 0)
            result = 1;
    }
    draw(b);

    while (!b->quit) {
        int r = gui_next_event(b->win, &ev, 200);
        struct browser_tab *t;

        if (r < 0)
            break;
        if (r == 0) {
            if (sync_script_page(b) != 0)
                result = 1;
            if (++idle_ticks >= 25) {
                idle_ticks = 0;
                runtime_persist_state(b->rt, 1);
            }
            draw(b);
            continue;
        }
        /* Event timing is useful extra input on machines without a hardware
         * random instruction; it never replaces the DRBG's other sources. */
        tls_add_entropy(&ev, sizeof(ev));
        t = active_tab(b);
        switch (ev.type) {
        case KEV_KEY:
            on_key(b, ev.key);
            break;
        case KEV_MOUSE_DOWN:
            on_click(b, ev.x, ev.y);
            break;
        case KEV_MOUSE_UP:
            b->sb_drag = 0;
            if (t && t->active_node) {
                char err[128];
                t->active_node = 0;
                if (t->page && t->page->has_hover_css)
                    tab_reflow_active(b, err, sizeof(err));
            }
            break;
        case KEV_MOUSE_MOVE:
            if (b->sb_drag && (ev.buttons & K_MOUSE_LEFT) && t) {
                int ch = content_h(b);

                if (ch > b->view_h) {
                    int rel = ev.y - b->top_chrome_h;

                    t->scroll = mul_div_i(rel,
                                          (int64_t)ch - b->view_h,
                                          b->view_h ? b->view_h : 1);
                    clamp_scroll(b);
                    if (t->page && t->page->js)
                        jsdom_set_scroll(t->page->js, 0, t->scroll);
                }
            } else if (!gui_update_hover(b, ev.x, ev.y)) {
                continue;
            }
            break;
        case KEV_MOUSE_WHEEL:
            if (t) {
                int dev_y, dev_h, find_y, stat_y;
                int delta = (int32_t)ev.key;
                int dx, dy, dw, dh, vis;

                recompute_layout_dims(b, &dev_y, &dev_h, &find_y, &stat_y);
                if (select_dropdown_rect(b, &dx, &dy, &dw, &dh, &vis)) {
                    select_set_top(t, t->select_top - delta * 3, vis);
                    t->select_hover = select_option_at(b, ev.x, ev.y);
                } else if (b->dev_open && ev.y >= dev_y + DEV_HDR_H &&
                    ev.y < dev_y + dev_h) {
                    if (b->dev_tab == 0)
                        t->elem_scroll -= delta * 2;
                    else if (b->dev_tab == 1)
                        t->con_scroll -= delta * 2;
                    else
                        t->net_scroll -= delta * 2;
                    if (t->elem_scroll < 0) t->elem_scroll = 0;
                    if (t->con_scroll < 0) t->con_scroll = 0;
                    if (t->net_scroll < 0) t->net_scroll = 0;
                } else {
                    t->scroll -= delta * GLYPH_H * 3;
                    clamp_scroll(b);
                    if (t->page && t->page->js)
                        jsdom_set_scroll(t->page->js, 0, t->scroll);
                }
            }
            break;
        case KEV_RESIZE:
        case KEV_THEME:
            tab_sync_active_view(b);
            break;
        case KEV_CLOSE:
            b->quit = 1;
            break;
        default:
            continue;
        }
        draw(b);
    }

    runtime_persist_state(b->rt, 1);
    if (b->tab_count > 0)
        session_save(b);
    for (i = 0; i < b->tab_count; i++) {
        tab_destroy(b->tabs[i]);
        b->tabs[i] = 0;
    }
    favicon_cache_free_all(b);
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
    printf("Verified https://, http://, local files and Markdown are supported.\n");
    printf("In the window: click links, Back/Fwd/Reload, arrows/wheel,\n");
    printf("ctrl-T/W/N/P tabs, ctrl-L address, ctrl-R reload, ctrl-B back,\n");
    printf("ctrl-E forward, ctrl-F find, ctrl-D bookmark, ctrl-O reader,\n");
    printf("ctrl-U view-source, +/-/0 zoom, ctrl-K inspector, ctrl-Q quit.\n");
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
    int i, rc, allow_restore = 0;

#ifndef JS_HOST
    g_cwd = kestrel_startup_cwd();
#endif
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 ||
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
        if (!text) {
            target = BROWSER_HOME;
            allow_restore = 1;
        } else {
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
    if (runtime_init(&w->runtime, !text, w->error, sizeof(w->error)) != 0) {
        printf("browser: %s\n", w->error);
        free(w);
        return 1;
    }

    if (text)
        rc = text_mode(&w->runtime, w->url, cols, links, verbose);
    else
        rc = gui_mode(&w->runtime, w->url, allow_restore);

    runtime_destroy(&w->runtime);
    free(w);
    return rc;
}
