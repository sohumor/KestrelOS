/* browser.c - the KestrelOS web browser.
 *
 * Two front ends over one engine (apps/html.c):
 *
 *   browser -t <url>     render the page as text on stdout. Works over
 *                        serial, needs no framebuffer, and is what the
 *                        automated tests drive.
 *   browser <url>        graphical: address bar, Back/Forward/Reload,
 *                        a scrollable content area, clickable links.
 *
 * Sources: http://host[:port]/path via http_get() from libc/http.c, and
 * local files (file:///path, or any path that exists) so the browser is
 * useful and testable with no network at all. Anything that is not HTML
 * is rendered as plain text; every failure - bad DNS, refused
 * connection, non-200 status, oversized page - becomes a readable page
 * rather than a crash.
 *
 * The two optional dependencies are behind __has_include so this app
 * always builds: without libc/http.c only local files work, without
 * libgui only -t mode works. Both say so instead of failing silently.
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "html.h"

#if defined(__has_include)
#  if __has_include(<http.h>)
#    include <http.h>
#    define HAVE_HTTP 1
#  endif
#  if __has_include(<gui.h>)
#    include <gui.h>
#    define HAVE_GUI 1
#  endif
#endif

#define URL_MAX      512

/* Where the window opens when no page is named (the desktop launcher). */
#define BROWSER_HOME "/doc/home.html"
#define HOST_MAX     160
#define PATH_MAX_B   384
#define HIST_MAX     64
#define TEXT_COLS    78
#define MAX_COLS     200
#define FETCH_MAX    (1024UL * 1024UL)   /* matches HTML_MAX_INPUT */

/* ------------------------------------------------------------------ *
 * argv / cwd plumbing (see apps/cat.c for the --cwd= convention)
 * ------------------------------------------------------------------ */

static const char *g_cwd = "/";

static void path_join(const char *tok, char *out, unsigned long outsz)
{
    if (tok[0] == '/')
        snprintf(out, outsz, "%s", tok);
    else if (g_cwd[0] == '/' && g_cwd[1] == '\0')
        snprintf(out, outsz, "/%s", tok);
    else
        snprintf(out, outsz, "%s/%s", g_cwd, tok);
}

/* ------------------------------------------------------------------ *
 * URLs
 * ------------------------------------------------------------------ */

struct url {
    char scheme[12];
    char host[HOST_MAX];
    int  port;
    char path[PATH_MAX_B];
};

static int is_url_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

/* Does s start with "scheme://"? */
static int has_scheme(const char *s)
{
    int i;
    for (i = 0; s[i] && i < 10; i++) {
        if (s[i] == ':')
            return i > 0 && s[i + 1] == '/' && s[i + 2] == '/';
        if (!is_url_char((unsigned char)s[i]))
            return 0;
    }
    return 0;
}

static void str_lower(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s + 32);
}

/* Split an absolute URL. Returns 0 on success. Fragments are dropped:
 * there is no in-page anchor navigation. */
static int url_split(const char *s, struct url *u)
{
    const char *p = s, *q;
    unsigned long n;

    memset(u, 0, sizeof *u);
    u->port = 80;

    q = strstr(p, "://");
    if (!q)
        return -1;
    n = (unsigned long)(q - p);
    if (n >= sizeof u->scheme)
        return -1;
    memcpy(u->scheme, p, n);
    u->scheme[n] = 0;
    str_lower(u->scheme);
    p = q + 3;

    /* authority runs to '/', '?' or end */
    q = p;
    while (*q && *q != '/' && *q != '?' && *q != '#')
        q++;
    n = (unsigned long)(q - p);
    if (n >= sizeof u->host)
        n = sizeof u->host - 1;
    memcpy(u->host, p, n);
    u->host[n] = 0;

    {
        char *colon = strrchr(u->host, ':');
        if (colon) {
            *colon = 0;
            u->port = atoi(colon + 1);
            if (u->port <= 0 || u->port > 65535)
                u->port = 80;
        }
    }
    str_lower(u->host);

    if (*q == '#' || !*q) {
        u->path[0] = '/';
        u->path[1] = 0;
        return 0;
    }
    p = q;
    q = strchr(p, '#');
    n = q ? (unsigned long)(q - p) : strlen(p);
    if (n >= sizeof u->path)
        n = sizeof u->path - 1;
    memcpy(u->path, p, n);
    u->path[n] = 0;
    if (u->path[0] != '/') {
        /* "http://host?x" - synthesise the root path */
        memmove(u->path + 1, u->path, strlen(u->path) + 1);
        u->path[0] = '/';
    }
    return 0;
}

/* Collapse "." and ".." segments in place. */
static void path_normalize(char *p)
{
    char out[PATH_MAX_B];
    int oi = 0;
    const char *s = p;

    if (*s != '/') {
        out[oi++] = '/';
    }
    while (*s) {
        const char *seg;
        int len;

        while (*s == '/')
            s++;
        seg = s;
        while (*s && *s != '/')
            s++;
        len = (int)(s - seg);
        if (len == 0)
            break;
        if (len == 1 && seg[0] == '.')
            continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            /* Drop the last segment: back over its name, then over the
             * '/' in front of it. Never past the leading '/'. */
            while (oi > 1 && out[oi - 1] != '/')
                oi--;
            if (oi > 1)
                oi--;
            if (oi == 0)
                out[oi++] = '/';
            continue;
        }
        if (oi == 0 || out[oi - 1] != '/')
            out[oi++] = '/';
        if (oi + len >= (int)sizeof out - 2)
            break;
        memcpy(out + oi, seg, (unsigned long)len);
        oi += len;
    }
    if (oi == 0)
        out[oi++] = '/';
    out[oi] = 0;
    memcpy(p, out, (unsigned long)oi + 1);
}

/* Resolve `rel` against `base` (which may be an absolute URL or a local
 * path) into `out`. */
static void url_resolve(const char *base, const char *rel, char *out,
                        unsigned long outsz)
{
    struct url u;
    char tmp[PATH_MAX_B * 2];
    char relbuf[URL_MAX];
    int rl;

    /* href="  /page  " is common in hand-written HTML; whitespace is not
     * part of the URL. */
    while (*rel == ' ' || *rel == '\t' || *rel == '\n' || *rel == '\r')
        rel++;
    snprintf(relbuf, sizeof relbuf, "%s", rel);
    rl = (int)strlen(relbuf);
    while (rl > 0 && (relbuf[rl - 1] == ' ' || relbuf[rl - 1] == '\t' ||
                      relbuf[rl - 1] == '\n' || relbuf[rl - 1] == '\r'))
        relbuf[--rl] = 0;
    rel = relbuf;

    if (has_scheme(rel)) {
        snprintf(out, outsz, "%s", rel);
        return;
    }
    if (rel[0] == '#' || rel[0] == 0) {
        snprintf(out, outsz, "%s", base);
        return;
    }
    /* Non-navigable schemes: keep them intact so the caller can complain
     * with the actual text the page contained. */
    if (strncmp(rel, "mailto:", 7) == 0 || strncmp(rel, "javascript:", 11) == 0 ||
        strncmp(rel, "tel:", 4) == 0 || strncmp(rel, "data:", 5) == 0) {
        snprintf(out, outsz, "%s", rel);
        return;
    }

    if (has_scheme(base) && url_split(base, &u) == 0) {
        if (rel[0] == '/' && rel[1] == '/') {
            snprintf(out, outsz, "%s:%s", u.scheme, rel);
            return;
        }
        if (rel[0] == '/') {
            snprintf(tmp, sizeof tmp, "%s", rel);
        } else {
            char *slash;
            snprintf(tmp, sizeof tmp, "%s", u.path);
            slash = strrchr(tmp, '/');
            if (slash)
                slash[1] = 0;
            else
                snprintf(tmp, sizeof tmp, "/");
            {
                unsigned long l = strlen(tmp);
                snprintf(tmp + l, sizeof tmp - l, "%s", rel);
            }
        }
        path_normalize(tmp);
        if (u.port == 80)
            snprintf(out, outsz, "%s://%s%s", u.scheme, u.host, tmp);
        else
            snprintf(out, outsz, "%s://%s:%d%s", u.scheme, u.host, u.port,
                     tmp);
        return;
    }

    /* base is a local path */
    if (rel[0] == '/') {
        snprintf(tmp, sizeof tmp, "%s", rel);
    } else {
        char *slash;
        snprintf(tmp, sizeof tmp, "%s", base);
        slash = strrchr(tmp, '/');
        if (slash)
            slash[1] = 0;
        else
            snprintf(tmp, sizeof tmp, "/");
        {
            unsigned long l = strlen(tmp);
            snprintf(tmp + l, sizeof tmp - l, "%s", rel);
        }
    }
    path_normalize(tmp);
    snprintf(out, outsz, "%s", tmp);
}

/* Turn what the user typed into something fetchable. */
static void url_from_input(const char *in, char *out, unsigned long outsz)
{
    char path[PATH_MAX_B];
    struct k_stat st;

    while (*in == ' ')
        in++;
    if (has_scheme(in) || strncmp(in, "file:", 5) == 0) {
        snprintf(out, outsz, "%s", in);
        return;
    }
    /* An existing file wins over a hostname guess: "browser -t doc.html"
     * has to work in the shell's cwd. */
    path_join(in, path, sizeof path);
    if (stat_(path, &st) == 0) {
        snprintf(out, outsz, "%s", path);
        return;
    }
    if (in[0] == '/') {
        snprintf(out, outsz, "%s", in);
        return;
    }
    snprintf(out, outsz, "http://%s", in);
}

/* ------------------------------------------------------------------ *
 * Fetching
 * ------------------------------------------------------------------ */

struct page {
    char url[URL_MAX];
    char *body;
    unsigned long len;
    int status;         /* HTTP status; 0 for a local file */
    int is_html;
    int truncated;
    char err[192];      /* empty when the fetch worked */
};

static void page_reset(struct page *pg)
{
    free(pg->body);
    pg->body = 0;
    pg->len = 0;
    pg->status = 0;
    pg->is_html = 0;
    pg->truncated = 0;
    pg->err[0] = 0;
}

/* Content sniffing. There is no Content-Type to consult (http_get()
 * hands back only the body), so decide from the bytes. */
static int sniff_html(const char *b, unsigned long n)
{
    unsigned long i, lim = n < 1024 ? n : 1024;
    static const char *marks[] = {
        "<html", "<!doctype", "<head", "<body", "<div", "<p>", "<p ",
        "<title", "<meta", "<span", "<h1", "<table", "<ul", "<a ", "<br", 0
    };
    int k;

    for (i = 0; i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\r' ||
                          b[i] == '\n'); i++)
        ;
    if (i < n && b[i] == '<' && i + 1 < n) {
        char c = b[i + 1];
        if (c == '!' || c == '/' || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z'))
            return 1;
    }
    for (k = 0; marks[k]; k++) {
        unsigned long ml = strlen(marks[k]);
        for (i = 0; i + ml <= lim; i++) {
            unsigned long j;
            for (j = 0; j < ml; j++) {
                char c = b[i + j];
                if (c >= 'A' && c <= 'Z')
                    c = (char)(c + 32);
                if (c != marks[k][j])
                    break;
            }
            if (j == ml)
                return 1;
        }
    }
    return 0;
}

static int read_local(const char *path, struct page *pg)
{
    struct k_stat st;
    int fd;
    long n;
    unsigned long got = 0, cap;

    if (stat_(path, &st) != 0) {
        snprintf(pg->err, sizeof pg->err, "no such file: %s", path);
        return -1;
    }
    if (st.is_dir) {
        snprintf(pg->err, sizeof pg->err, "%s is a directory", path);
        return -1;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(pg->err, sizeof pg->err, "cannot open %s (permission?)",
                 path);
        return -1;
    }
    cap = st.size;
    if (cap > FETCH_MAX) {
        cap = FETCH_MAX;
        pg->truncated = 1;
    }
    pg->body = (char *)malloc(cap + 1);
    if (!pg->body) {
        close(fd);
        snprintf(pg->err, sizeof pg->err, "out of memory for %lu bytes",
                 cap);
        return -1;
    }
    while (got < cap && (n = read(fd, pg->body + got, cap - got)) > 0)
        got += (unsigned long)n;
    close(fd);
    pg->body[got] = 0;
    pg->len = got;
    pg->status = 0;
    pg->is_html = sniff_html(pg->body, got);
    return 0;
}

static int fetch(const char *url, struct page *pg)
{
    struct url u;

    page_reset(pg);
    snprintf(pg->url, sizeof pg->url, "%s", url);

    if (!has_scheme(url)) {
        return read_local(url, pg);
    }
    if (url_split(url, &u) != 0) {
        snprintf(pg->err, sizeof pg->err, "malformed URL");
        return -1;
    }
    if (strcmp(u.scheme, "file") == 0) {
        /* file://host/path - the host part is ignored, as everywhere. */
        return read_local(u.path, pg);
    }
    if (strcmp(u.scheme, "http") != 0) {
        snprintf(pg->err, sizeof pg->err,
                 "unsupported scheme \"%s\" (only http and file)", u.scheme);
        return -1;
    }

#ifndef HAVE_HTTP
    snprintf(pg->err, sizeof pg->err,
             "this build has no HTTP client (libc/http.c was not linked)");
    return -1;
#else
    {
        struct k_netinfo ni;
        uint32_t ip;
        char *body = 0;
        unsigned long len = 0;
        int status = 0, rc;

        if (netinfo(&ni) != 0 || !ni.up) {
            snprintf(pg->err, sizeof pg->err,
                     "network unavailable: no NIC is configured");
            return -1;
        }
        /* Resolve first so a DNS failure can be reported as a DNS
         * failure instead of a generic "could not connect". */
        ip = ip_aton(u.host);
        if (ip == 0 && dns_resolve(u.host, &ip) != 0) {
            snprintf(pg->err, sizeof pg->err,
                     "DNS lookup failed for \"%s\"", u.host);
            return -1;
        }
        if (ip == 0) {
            snprintf(pg->err, sizeof pg->err, "invalid address for \"%s\"",
                     u.host);
            return -1;
        }

        rc = http_get(url, &body, &len, &status);
        if (rc != 0 || !body) {
            char ipbuf[16];
            free(body);
            snprintf(pg->err, sizeof pg->err,
                     "cannot connect to %s (%s) port %d - refused, timed "
                     "out or reset", u.host, ip_ntoa(ip, ipbuf), u.port);
            return -1;
        }
        if (len > FETCH_MAX) {
            len = FETCH_MAX;
            pg->truncated = 1;
        }
        pg->body = body;
        pg->len = len;
        pg->status = status;
        pg->is_html = sniff_html(body, len);
        return 0;
    }
#endif
}

/* ------------------------------------------------------------------ *
 * Documents
 * ------------------------------------------------------------------ */

static void esc_append(char *dst, unsigned long *di, unsigned long cap,
                       const char *s)
{
    while (*s && *di + 1 < cap)
        dst[(*di)++] = *s++;
}

/* Wrap plain text in <pre> so the one layout engine handles both. An
 * optional HTML prefix carries a banner (an HTTP status, say). */
static struct html_doc *doc_from_text(const char *prefix, const char *src,
                                      unsigned long len)
{
    unsigned long cap, di = 0, i;
    char *buf;
    struct html_doc *d;

    if (len > HTML_MAX_INPUT)
        len = HTML_MAX_INPUT;
    /* "&amp;" is the worst case: five output bytes for one input byte. */
    cap = len * 5 + (prefix ? strlen(prefix) : 0) + 64;
    buf = (char *)malloc(cap);
    if (!buf)
        return 0;
    if (prefix)
        esc_append(buf, &di, cap, prefix);
    esc_append(buf, &di, cap, "<pre>");
    for (i = 0; i < len; i++) {
        char c = src[i];
        if (c == '&')
            esc_append(buf, &di, cap, "&amp;");
        else if (c == '<')
            esc_append(buf, &di, cap, "&lt;");
        else if (c == '>')
            esc_append(buf, &di, cap, "&gt;");
        else if (c == '\n' || c == '\t') {
            if (di + 1 < cap)
                buf[di++] = c;
        } else if ((unsigned char)c < 32 || (unsigned char)c >= 127) {
            /* Never let a binary file spray control bytes at the serial
             * console or the framebuffer font. */
            if (di + 1 < cap)
                buf[di++] = '.';
        } else if (di + 1 < cap) {
            buf[di++] = c;
        }
    }
    esc_append(buf, &di, cap, "</pre>");
    d = html_parse(buf, di);
    free(buf);
    return d;
}

static struct html_doc *doc_from_error(const char *url, const char *msg)
{
    char buf[URL_MAX + 512];
    int n = snprintf(buf, sizeof buf,
                     "<h1>Cannot load page</h1><p><b>%s</b></p>"
                     "<p>while fetching:</p><pre>%s</pre>"
                     "<hr><p>Check the address, or try "
                     "<i>ping</i> and <i>nslookup</i> from the shell.</p>",
                     msg, url);
    if (n < 0)
        n = 0;
    if ((unsigned long)n > sizeof buf - 1)
        n = (int)sizeof buf - 1;
    return html_parse(buf, (unsigned long)n);
}

/* Build the document for a fetched page, including any status banner.
 * A non-2xx body is usually still worth reading, so it is shown under a
 * banner rather than discarded. */
static struct html_doc *doc_from_page(struct page *pg)
{
    char head[256];
    const char *banner = 0;
    unsigned long hn = 0;

    if (pg->err[0])
        return doc_from_error(pg->url, pg->err);

    if (pg->status && (pg->status < 200 || pg->status >= 300)) {
        int n = snprintf(head, sizeof head,
                         "<h2>HTTP %d</h2><p>The server returned status "
                         "%d for this address.</p><hr>",
                         pg->status, pg->status);
        hn = (n > 0 && (unsigned long)n < sizeof head) ? (unsigned long)n
                                                       : sizeof head - 1;
        head[hn] = 0;
        banner = head;
    }

    if (!pg->is_html)
        return doc_from_text(banner, pg->body, pg->len);

    if (!banner)
        return html_parse(pg->body, pg->len);
    {
        unsigned long total = hn + pg->len;
        char *combined = (char *)malloc(total + 1);
        struct html_doc *d;
        if (!combined)
            return html_parse(head, hn);
        memcpy(combined, head, hn);
        memcpy(combined + hn, pg->body, pg->len);
        d = html_parse(combined, total);
        free(combined);
        return d;
    }
}

/* ------------------------------------------------------------------ *
 * Text mode
 * ------------------------------------------------------------------ */

static void render_text(const struct html_layout *l, int cols)
{
    char row[MAX_COLS + 2];
    int i, j, y = 0;

    if (cols > MAX_COLS)
        cols = MAX_COLS;

    for (i = 0; i < l->nline; i++) {
        const struct html_line *ln = &l->lines[i];
        int used = 0;

        while (y < ln->y) {
            putchar('\n');
            y++;
        }
        memset(row, ' ', (unsigned long)cols);
        for (j = 0; j < ln->count; j++) {
            const struct html_run *r = &l->runs[ln->first + j];
            int x = r->x, n, k;

            if (x >= cols)
                continue;
            if (r->kind == HB_RULE) {
                int w = r->w;
                if (x + w > cols)
                    w = cols - x;
                for (k = 0; k < w; k++)
                    row[x + k] = '-';
                if (x + w > used)
                    used = x + w;
                continue;
            }
            if (!r->text)
                continue;
            n = (int)strlen(r->text);
            if (r->kind == HB_IMAGE) {
                if (x + 2 >= cols)
                    continue;
                if (n > cols - x - 2)
                    n = cols - x - 2;
                row[x] = '[';
                memcpy(row + x + 1, r->text, (unsigned long)n);
                row[x + 1 + n] = ']';
                if (x + n + 2 > used)
                    used = x + n + 2;
                continue;
            }
            if (x + n > cols)
                n = cols - x;
            memcpy(row + x, r->text, (unsigned long)n);
            if (x + n > used)
                used = x + n;
        }
        while (used > 0 && row[used - 1] == ' ')
            used--;
        row[used] = 0;
        printf("%s\n", row);
        y = ln->y + ln->h;
    }
}

/* List every distinct link target, resolved against the page URL so the
 * output can be fed straight back into `browser -t`. */
static void render_links(const struct html_layout *l, const char *base)
{
    int i, j, n = 0;
    char abs[URL_MAX];

    for (i = 0; i < l->nrun; i++) {
        const char *h = l->runs[i].href;
        if (!h || !*h)
            continue;
        for (j = 0; j < i; j++) {
            const char *p = l->runs[j].href;
            if (p && strcmp(p, h) == 0)
                break;
        }
        if (j < i)
            continue;
        if (n == 0)
            printf("\nLinks:\n");
        url_resolve(base, h, abs, sizeof abs);
        printf("  [%d] %s\n", ++n, abs);
    }
}

static int text_mode(const char *url, int cols, int show_links, int verbose)
{
    struct page pg;
    struct html_doc *d;
    struct html_layout *l;
    struct html_metrics m;
    int rc = 0;

    memset(&pg, 0, sizeof pg);
    if (fetch(url, &pg) != 0) {
        printf("browser: %s\n", pg.err);
        page_reset(&pg);
        return 1;
    }

    if (verbose) {
        printf("url: %s\n", pg.url);
        if (pg.status)
            printf("status: %d\n", pg.status);
        printf("bytes: %lu  type: %s%s\n", pg.len,
               pg.is_html ? "html" : "text",
               pg.truncated ? "  [truncated]" : "");
    }
    if (pg.status && (pg.status < 200 || pg.status >= 300)) {
        printf("browser: HTTP %d for %s\n", pg.status, pg.url);
        rc = 1;
    }

    d = doc_from_page(&pg);
    if (!d) {
        printf("browser: out of memory parsing %lu bytes\n", pg.len);
        page_reset(&pg);
        return 1;
    }
    if (verbose && d->title)
        printf("title: %s\n", d->title);
    if (verbose)
        printf("\n");

    html_metrics_chars(&m);
    l = html_layout_build(d, cols, &m);
    if (!l) {
        printf("browser: out of memory laying out the page\n");
        html_free(d);
        page_reset(&pg);
        return 1;
    }
    render_text(l, cols);
    if (pg.truncated)
        printf("\n[page truncated at %lu bytes]\n", FETCH_MAX);
    else if (d->truncated)
        printf("\n[part of the page was dropped: too many elements or "
               "nested too deeply]\n");
    if (show_links)
        render_links(l, pg.url);

    html_layout_free(l);
    html_free(d);
    page_reset(&pg);
    return rc;
}

/* ------------------------------------------------------------------ *
 * Graphical mode
 * ------------------------------------------------------------------ */

#ifdef HAVE_GUI

/* --- libgui adapter -------------------------------------------------
 * The ONLY part of the browser that touches libgui. It assumes the
 * primitive set documented in docs/browser.md:
 *
 *   gui_win *gui_open(const char *title, int w, int h);
 *   void gui_close(gui_win *);
 *   void gui_flush(gui_win *);
 *   int  gui_next_event(gui_win *, struct k_event *, int timeout_ms);
 *   void gui_clear(gui_win *, unsigned color);
 *   void gui_rect (gui_win *, int x, int y, int w, int h, unsigned color);
 *   void gui_frame(gui_win *, int x, int y, int w, int h, unsigned color);
 *   void gui_line (gui_win *, int x0,int y0,int x1,int y1, unsigned color);
 *   void gui_text (gui_win *, int x, int y, const char *s, unsigned color);
 *   int  gui_text_w(const char *s);
 *
 * If libgui landed with different names or argument orders, fix the ten
 * wrappers below and nothing else in this file has to change.
 * ------------------------------------------------------------------ */

/* libgui named its handle gui_window and takes a background colour on the
 * text calls; this file was written against a slightly different shape, so
 * the two are reconciled here rather than in either implementation. */
typedef gui_window gui_win;

static gui_win *br_open(const char *title, int w, int h)
{
    return gui_open(title, 40, 40, w, h, 0);
}

static int br_text(gui_win *w, int x, int y, const char *s, uint32_t fg)
{
    return gui_text(w, x, y, s, fg, GUI_TRANSPARENT);
}

#define gui_open(t, w, h) br_open((t), (w), (h))
#define gui_text(win, x, y, s, fg) br_text((win), (x), (y), (s), (fg))

#define GLYPH_W 8
#define GLYPH_H 16

#define WIN_W   900
#define WIN_H   620
#define BAR_H   30
#define STAT_H  20
#define SB_W    12
#define PAD     6

#define C_BG      0x00FFFFFFu
#define C_TEXT    0x00101010u
#define C_LINK    0x000B3FBFu
#define C_ITALIC  0x00303050u
#define C_CHROME  0x00D8D8D8u
#define C_FIELD   0x00FFFFFFu
#define C_FRAME   0x00707070u
#define C_BTN     0x00F0F0F0u
#define C_BTNDN   0x00A8A8A8u
#define C_DIM     0x00909090u
#define C_STATBG  0x00E8E8E8u
#define C_PREBG   0x00F2F2F2u
#define C_RULE    0x00A0A0A0u
#define C_CARET   0x00202020u

static int ui_text_w(const char *s)
{
    return gui_text_w(s);
}

/* Bold is a second pass one pixel to the right: the 8x16 bitmap font has
 * exactly one weight, so this is the only emphasis available. */
static void ui_text(gui_win *w, int x, int y, const char *s, unsigned c,
                    int bold)
{
    gui_text(w, x, y, s, c);
    if (bold)
        gui_text(w, x + 1, y, s, c);
}

static void ui_box(gui_win *w, int x, int y, int cw, int ch, unsigned fill,
                   unsigned edge)
{
    gui_rect(w, x, y, cw, ch, fill);
    gui_frame(w, x, y, cw, ch, edge);
}

static void ui_button(gui_win *w, int x, int y, int cw, int ch,
                      const char *label, int enabled, int down)
{
    int tw = ui_text_w(label);
    ui_box(w, x, y, cw, ch, down ? C_BTNDN : C_BTN, C_FRAME);
    ui_text(w, x + (cw - tw) / 2, y + (ch - GLYPH_H) / 2, label,
            enabled ? C_TEXT : C_DIM, 0);
}

/* --- browser state --- */

struct bstate {
    gui_win *win;
    char hist[HIST_MAX][URL_MAX];
    int hist_n, hist_i;

    struct page pg;
    struct html_doc *doc;
    struct html_layout *lay;

    char addr[URL_MAX];
    int  addr_len, addr_cur, addr_focus;

    int  scroll;
    int  view_w, view_h;
    char status[256];
    int  status_err;

    int  sb_drag;
    int  quit;
};

static int gm_text_w(void *ctx, const char *s, int len, unsigned int style,
                     int heading)
{
    (void)ctx;
    (void)s;
    (void)heading;
    (void)style;
    return len * GLYPH_W;
}

static int gm_line_h(void *ctx, unsigned int style, int heading)
{
    (void)ctx;
    (void)style;
    if (heading == 1)
        return GLYPH_H + 10;
    if (heading == 2)
        return GLYPH_H + 6;
    if (heading)
        return GLYPH_H + 3;
    return GLYPH_H + 2;
}

static void set_status(struct bstate *b, int err, const char *head,
                       const char *extra)
{
    snprintf(b->status, sizeof b->status, "%s%s%s", head,
             extra && *extra ? "  -  " : "", extra ? extra : "");
    b->status_err = err;
}

static void relayout(struct bstate *b)
{
    struct html_metrics m;

    html_layout_free(b->lay);
    b->lay = 0;
    if (!b->doc)
        return;
    m.text_w = gm_text_w;
    m.line_h = gm_line_h;
    m.ctx = 0;
    m.indent_w = GLYPH_W * 3;
    m.para_gap = 10;
    b->lay = html_layout_build(b->doc, b->view_w - 2 * PAD, &m);
}

static void draw(struct bstate *b);

static void load(struct bstate *b, const char *url, int push)
{
    struct html_doc *d;

    set_status(b, 0, "loading", url);
    if (b->win)
        draw(b);   /* the fetch can block for seconds; show it first */

    if (strncmp(url, "mailto:", 7) == 0 ||
        strncmp(url, "javascript:", 11) == 0 ||
        strncmp(url, "tel:", 4) == 0 || strncmp(url, "data:", 5) == 0) {
        set_status(b, 1, "cannot follow this kind of link:", url);
        return;
    }

    fetch(url, &b->pg);
    d = doc_from_page(&b->pg);
    if (!d)
        d = doc_from_error(url, "out of memory rendering this page");

    html_free(b->doc);
    b->doc = d;
    b->scroll = 0;
    relayout(b);

    if (push) {
        /* A new navigation discards whatever Forward pointed at. */
        if (b->hist_n > 0)
            b->hist_n = b->hist_i + 1;
        if (b->hist_n >= HIST_MAX) {
            memmove(b->hist[0], b->hist[1],
                    (unsigned long)(HIST_MAX - 1) * URL_MAX);
            b->hist_n = HIST_MAX - 1;
        }
        snprintf(b->hist[b->hist_n], URL_MAX, "%s", b->pg.url);
        b->hist_i = b->hist_n;
        b->hist_n++;
    }

    snprintf(b->addr, sizeof b->addr, "%s", b->pg.url);
    b->addr_len = (int)strlen(b->addr);
    b->addr_cur = b->addr_len;

    if (b->pg.err[0]) {
        set_status(b, 1, "error:", b->pg.err);
    } else {
        char info[96];
        snprintf(info, sizeof info, "%s%lu bytes%s",
                 b->pg.status ? "" : "local file, ", b->pg.len,
                 b->pg.truncated ? " (truncated)" : "");
        if (b->pg.status && (b->pg.status < 200 || b->pg.status >= 300)) {
            char m2[128];
            snprintf(m2, sizeof m2, "HTTP %d, %s", b->pg.status, info);
            set_status(b, 1, b->pg.url, m2);
        } else {
            set_status(b, 0, b->doc && b->doc->title ? b->doc->title
                                                     : b->pg.url, info);
        }
    }
}

static int content_h(struct bstate *b)
{
    return b->lay ? b->lay->height : 0;
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

static void draw_run(struct bstate *b, const struct html_run *r, int oy)
{
    unsigned int col = C_TEXT;
    int x = PAD + r->x;
    int y = oy + r->y;
    int bold = (r->style & HS_BOLD) || r->heading;

    if (r->kind == HB_RULE) {
        gui_rect(b->win, x, y + r->h / 2, r->w, 1, C_RULE);
        return;
    }
    if (r->kind == HB_IMAGE) {
        int w = r->w;
        ui_box(b->win, x, y, w, r->h, C_PREBG, C_FRAME);
        if (r->text)
            ui_text(b->win, x + GLYPH_W / 2, y + 1, r->text, C_DIM, 0);
        return;
    }
    if (!r->text || !*r->text)
        return;

    if (r->style & HS_LINK)
        col = C_LINK;
    else if (r->style & HS_ITALIC)
        col = C_ITALIC;

    if (r->style & HS_PRE)
        gui_rect(b->win, x, y, r->w, r->h, C_PREBG);

    ui_text(b->win, x, y + (r->h - GLYPH_H), r->text, col, bold);

    if (r->style & (HS_UNDER | HS_LINK))
        gui_line(b->win, x, y + r->h - 2, x + r->w - 1, y + r->h - 2, col);
}

static void draw(struct bstate *b)
{
    int i, y0 = BAR_H, cw;
    int bx;

    gui_clear(b->win, C_BG);

    /* toolbar */
    gui_rect(b->win, 0, 0, WIN_W, BAR_H, C_CHROME);
    ui_button(b->win, PAD, 4, 46, BAR_H - 8, "Back", b->hist_i > 0, 0);
    ui_button(b->win, PAD + 50, 4, 62, BAR_H - 8, "Fwd",
              b->hist_i + 1 < b->hist_n, 0);
    ui_button(b->win, PAD + 116, 4, 62, BAR_H - 8, "Reload", 1, 0);

    bx = PAD + 184;
    cw = WIN_W - bx - PAD;
    ui_box(b->win, bx, 4, cw, BAR_H - 8, C_FIELD,
           b->addr_focus ? C_LINK : C_FRAME);
    {
        /* Scroll the field so the caret stays visible. */
        int maxch = (cw - 8) / GLYPH_W;
        int from = 0;
        char tmp[URL_MAX];
        if (b->addr_cur > maxch)
            from = b->addr_cur - maxch;
        snprintf(tmp, sizeof tmp, "%s", b->addr + from);
        if ((int)strlen(tmp) > maxch)
            tmp[maxch] = 0;
        ui_text(b->win, bx + 4, 4 + (BAR_H - 8 - GLYPH_H) / 2, tmp, C_TEXT, 0);
        if (b->addr_focus) {
            int cx = bx + 4 + (b->addr_cur - from) * GLYPH_W;
            gui_rect(b->win, cx, 8, 1, BAR_H - 16, C_CARET);
        }
    }

    /* content */
    gui_rect(b->win, 0, y0, b->view_w, b->view_h, C_BG);
    if (b->lay) {
        for (i = 0; i < b->lay->nline; i++) {
            const struct html_line *ln = &b->lay->lines[i];
            int j;
            if (ln->y + ln->h <= b->scroll)
                continue;
            if (ln->y >= b->scroll + b->view_h)
                break;
            /* Whole lines only: without a clip rectangle a partially
             * visible line would bleed into the toolbar. */
            if (ln->y < b->scroll || ln->y + ln->h > b->scroll + b->view_h)
                continue;
            for (j = 0; j < ln->count; j++)
                draw_run(b, &b->lay->runs[ln->first + j], y0 - b->scroll);
        }
    }

    /* scrollbar */
    {
        int sx = b->view_w, ch = content_h(b);
        int th, ty;
        gui_rect(b->win, sx, y0, SB_W, b->view_h, C_CHROME);
        gui_frame(b->win, sx, y0, SB_W, b->view_h, C_FRAME);
        if (ch > b->view_h) {
            th = b->view_h * b->view_h / ch;
            if (th < 18)
                th = 18;
            ty = y0 + (b->view_h - th) * b->scroll / (ch - b->view_h);
            ui_box(b->win, sx + 1, ty, SB_W - 2, th, C_BTN, C_FRAME);
        }
    }

    /* status line */
    {
        int sy = WIN_H - STAT_H;
        char tmp[256];
        int maxch = (WIN_W - 2 * PAD) / GLYPH_W;
        gui_rect(b->win, 0, sy, WIN_W, STAT_H, C_STATBG);
        gui_line(b->win, 0, sy, WIN_W - 1, sy, C_FRAME);
        snprintf(tmp, sizeof tmp, "%s", b->status);
        if (maxch > 0 && (int)strlen(tmp) > maxch)
            tmp[maxch] = 0;
        ui_text(b->win, PAD, sy + 2, tmp,
                b->status_err ? 0x00A00000u : C_TEXT, 0);
    }

    gui_flush(b->win);
}

static void navigate_rel(struct bstate *b, const char *href)
{
    char full[URL_MAX];
    url_resolve(b->pg.url, href, full, sizeof full);
    load(b, full, 1);
}

static void go_back(struct bstate *b)
{
    if (b->hist_i <= 0) {
        set_status(b, 1, "no page to go back to", "");
        return;
    }
    b->hist_i--;
    load(b, b->hist[b->hist_i], 0);
}

static void go_fwd(struct bstate *b)
{
    if (b->hist_i + 1 >= b->hist_n) {
        set_status(b, 1, "no page to go forward to", "");
        return;
    }
    b->hist_i++;
    load(b, b->hist[b->hist_i], 0);
}

static void addr_insert(struct bstate *b, char c)
{
    int i;
    if (b->addr_len + 1 >= (int)sizeof b->addr)
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

static void on_key(struct bstate *b, unsigned int key)
{
    int page = b->view_h - GLYPH_H * 2;

    if (page < GLYPH_H)
        page = GLYPH_H;

    switch (key) {
    case 17:                     /* ctrl-Q */
        b->quit = 1;
        return;
    case 12:                     /* ctrl-L: focus the address bar */
        b->addr_focus = 1;
        b->addr_cur = b->addr_len;
        return;
    case 18:                     /* ctrl-R: reload */
        load(b, b->pg.url, 0);
        return;
    case 2:                      /* ctrl-B: back */
        go_back(b);
        return;
    case 6:                      /* ctrl-F: forward */
        go_fwd(b);
        return;
    case 27:                     /* ESC */
        b->addr_focus = 0;
        snprintf(b->addr, sizeof b->addr, "%s", b->pg.url);
        b->addr_len = (int)strlen(b->addr);
        b->addr_cur = b->addr_len;
        return;
    default:
        break;
    }

    if (b->addr_focus) {
        switch (key) {
        case '\n':
        case '\r': {
            char full[URL_MAX];
            b->addr_focus = 0;
            url_from_input(b->addr, full, sizeof full);
            load(b, full, 1);
            return;
        }
        case 8:
            addr_delete(b, 1);
            return;
        case KEY_DELETE:
            addr_delete(b, 0);
            return;
        case KEY_LEFT:
            if (b->addr_cur > 0)
                b->addr_cur--;
            return;
        case KEY_RIGHT:
            if (b->addr_cur < b->addr_len)
                b->addr_cur++;
            return;
        case KEY_HOME:
            b->addr_cur = 0;
            return;
        case KEY_END:
            b->addr_cur = b->addr_len;
            return;
        case 21:                 /* ctrl-U */
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
    case KEY_PGUP: b->scroll -= page;        break;
    case KEY_PGDN: b->scroll += page;        break;
    case ' ':      b->scroll += page;        break;
    case KEY_HOME: b->scroll = 0;            break;
    case KEY_END:  b->scroll = content_h(b); break;
    default: break;
    }
    clamp_scroll(b);
}

static void on_click(struct bstate *b, int x, int y)
{
    int bx = PAD + 184;

    if (y < BAR_H) {
        b->addr_focus = 0;
        if (x >= PAD && x < PAD + 46) {
            go_back(b);
        } else if (x >= PAD + 50 && x < PAD + 112) {
            go_fwd(b);
        } else if (x >= PAD + 116 && x < PAD + 178) {
            load(b, b->pg.url, 0);
        } else if (x >= bx) {
            b->addr_focus = 1;
            b->addr_cur = (x - bx - 4) / GLYPH_W;
            if (b->addr_cur > b->addr_len)
                b->addr_cur = b->addr_len;
            if (b->addr_cur < 0)
                b->addr_cur = 0;
        }
        return;
    }
    if (y >= WIN_H - STAT_H)
        return;

    b->addr_focus = 0;

    if (x >= b->view_w) {
        /* Scrollbar: jump so the click position becomes the thumb centre. */
        int ch = content_h(b);
        if (ch > b->view_h) {
            int rel = y - BAR_H;
            b->scroll = rel * (ch - b->view_h) / (b->view_h ? b->view_h : 1);
            clamp_scroll(b);
            b->sb_drag = 1;
        }
        return;
    }

    if (b->lay) {
        int hit = html_layout_hit(b->lay, x - PAD, y - BAR_H + b->scroll);
        if (hit >= 0 && b->lay->runs[hit].href &&
            b->lay->runs[hit].href[0]) {
            navigate_rel(b, b->lay->runs[hit].href);
        }
    }
}

static int gui_mode(const char *start_url)
{
    struct bstate b;
    struct k_event ev;
    struct k_fbinfo fb;

    memset(&b, 0, sizeof b);

    if (syscall(SYS_FBINFO, (long)&fb, 0, 0, 0) != 0 || !fb.present) {
        printf("browser: no framebuffer; use 'browser -t <url>'\n");
        return 1;
    }

    b.win = gui_open("Kestrel Browser", WIN_W, WIN_H);
    if (!b.win) {
        printf("browser: cannot open a window (is the desktop running?)\n");
        printf("browser: use 'browser -t <url>' for text mode\n");
        return 1;
    }
    b.view_w = WIN_W - SB_W;
    b.view_h = WIN_H - BAR_H - STAT_H;

    load(&b, start_url, 1);
    draw(&b);

    while (!b.quit) {
        int r = gui_next_event(b.win, &ev, 200);
        if (r < 0)
            break;
        if (r == 0)
            continue;
        switch (ev.type) {
        case KEV_KEY:
            on_key(&b, ev.key);
            break;
        case KEV_MOUSE_DOWN:
            on_click(&b, ev.x, ev.y);
            break;
        case KEV_MOUSE_UP:
            b.sb_drag = 0;
            break;
        case KEV_MOUSE_MOVE:
            if (b.sb_drag && (ev.buttons & K_MOUSE_LEFT)) {
                int ch = content_h(&b);
                if (ch > b.view_h) {
                    int rel = ev.y - BAR_H;
                    b.scroll = rel * (ch - b.view_h) /
                               (b.view_h ? b.view_h : 1);
                    clamp_scroll(&b);
                }
            } else {
                continue;        /* nothing changed: skip the repaint */
            }
            break;
        case KEV_CLOSE:
            b.quit = 1;
            break;
        default:
            continue;
        }
        draw(&b);
    }

    html_layout_free(b.lay);
    html_free(b.doc);
    page_reset(&b.pg);
    gui_close(b.win);
    return 0;
}

#endif /* HAVE_GUI */

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

static void usage(void)
{
    printf("usage: browser [-t] [-w cols] [-l] [-v] <url|file>\n");
    printf("  -t        text mode: render to stdout and exit\n");
    printf("  -w cols   text width (default %d, max %d)\n", TEXT_COLS,
           MAX_COLS);
    printf("  -l        list the page's links after the text\n");
    printf("  -v        print url, status, size and title first\n");
    printf("\n");
    printf("http://host[:port]/path and local files are supported.\n");
    printf("In the window: click links, Back/Fwd/Reload, arrows and\n");
    printf("PgUp/PgDn scroll, ctrl-L address bar, ctrl-R reload,\n");
    printf("ctrl-B back, ctrl-F forward, ctrl-Q quit.\n");
}

int main(int argc, char **argv)
{
    const char *target = 0;
    int text = 0, cols = TEXT_COLS, links = 0, verbose = 0;
    int i;
    char url[URL_MAX];

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) == 0) {
            g_cwd = argv[i] + 6;
            continue;
        }
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--text") == 0) {
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

    /* No target: in a window, open the start page — that is what the
     * desktop's Browser button does, and printing usage into a window
     * nobody opened just made the button look broken. Text mode still
     * needs something to render, so it keeps the usage message. */
    if (!target) {
#ifdef HAVE_GUI
        if (!text)
            target = BROWSER_HOME;
#endif
        if (!target) {
            usage();
            return 2;
        }
    }
    if (cols < 20)
        cols = 20;
    if (cols > MAX_COLS)
        cols = MAX_COLS;

    url_from_input(target, url, sizeof url);

    if (text)
        return text_mode(url, cols, links, verbose);

#ifdef HAVE_GUI
    return gui_mode(url);
#else
    printf("browser: this build has no GUI (libgui was not linked); "
           "rendering as text\n\n");
    return text_mode(url, cols, links, verbose);
#endif
}
