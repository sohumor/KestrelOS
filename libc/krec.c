/* KestrelOS libc: @krec typed structured record stream library. */

#include <krec.h>
#include <kestrel.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CTRL_D 4

/* ---- small local helpers --------------------------------------------- */

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t off = 0;

    while (off < len) {
        long w = write(fd, p + off, (unsigned long)(len - off));
        if (w <= 0)
            return -1;
        off += (size_t)w;
    }
    return 0;
}

static int lower_ch(int c)
{
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

static int is_ident_ch(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int is_space_ch(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static int rest_is_ws(const char *s)
{
    while (*s) {
        if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n')
            return 0;
        s++;
    }
    return 1;
}

/* ---- public CLI helpers ---------------------------------------------- */

int krec_strcasecmp(const char *a, const char *b)
{
    while (*a && lower_ch((unsigned char)*a) == lower_ch((unsigned char)*b)) {
        a++;
        b++;
    }
    return lower_ch((unsigned char)*a) - lower_ch((unsigned char)*b);
}

const char *krec_strcasestr(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    size_t hlen = strlen(hay);
    size_t i, j;

    if (nlen == 0)
        return hay;
    if (nlen > hlen)
        return 0;
    for (i = 0; i + nlen <= hlen; i++) {
        for (j = 0; j < nlen; j++) {
            if (lower_ch((unsigned char)hay[i + j]) !=
                lower_ch((unsigned char)needle[j]))
                break;
        }
        if (j == nlen)
            return hay + i;
    }
    return 0;
}

void krec_err(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = (int)sizeof(buf) - 1;
    write_all(2, buf, (size_t)n);
}

/* ---- type metadata --------------------------------------------------- */

const char *krec_type_name(enum krec_type t)
{
    switch (t) {
    case KREC_STR:   return "str";
    case KREC_INT:   return "int";
    case KREC_U64:   return "u64";
    case KREC_HEX:   return "hex";
    case KREC_BYTES: return "bytes";
    case KREC_IP:    return "ip";
    case KREC_BOOL:  return "bool";
    default:         return "str";
    }
}

int krec_parse_type(const char *s, enum krec_type *out)
{
    if (!s || !out)
        return -1;
    if (strcmp(s, "str") == 0)        { *out = KREC_STR;   return 0; }
    if (strcmp(s, "int") == 0)        { *out = KREC_INT;   return 0; }
    if (strcmp(s, "u64") == 0)        { *out = KREC_U64;   return 0; }
    if (strcmp(s, "hex") == 0)        { *out = KREC_HEX;   return 0; }
    if (strcmp(s, "bytes") == 0)      { *out = KREC_BYTES; return 0; }
    if (strcmp(s, "ip") == 0)         { *out = KREC_IP;    return 0; }
    if (strcmp(s, "bool") == 0)       { *out = KREC_BOOL;  return 0; }
    return -1;
}

int krec_is_numeric_type(enum krec_type t)
{
    return t == KREC_INT || t == KREC_U64 || t == KREC_HEX || t == KREC_BYTES;
}

/* ---- writer API ------------------------------------------------------ */

struct fd_buf {
    int fd;
    size_t pos;
    int err;
    char data[512];
};

static void fb_init(struct fd_buf *fb, int fd)
{
    fb->fd = fd;
    fb->pos = 0;
    fb->err = 0;
}

static void fb_flush(struct fd_buf *fb)
{
    if (!fb->err && fb->pos > 0) {
        if (write_all(fb->fd, fb->data, fb->pos) < 0)
            fb->err = 1;
    }
    fb->pos = 0;
}

static void fb_putc(struct fd_buf *fb, char c)
{
    if (fb->err)
        return;
    fb->data[fb->pos++] = c;
    if (fb->pos == sizeof(fb->data))
        fb_flush(fb);
}

static void fb_puts(struct fd_buf *fb, const char *s)
{
    while (*s)
        fb_putc(fb, *s++);
}

static void fb_escape(struct fd_buf *fb, const char *s)
{
    if (!s)
        return;
    for (; *s; s++) {
        switch (*s) {
        case '\\': fb_putc(fb, '\\'); fb_putc(fb, '\\'); break;
        case '\t': fb_putc(fb, '\\'); fb_putc(fb, 't');  break;
        case '\n': fb_putc(fb, '\\'); fb_putc(fb, 'n');  break;
        case '\r': fb_putc(fb, '\\'); fb_putc(fb, 'r');  break;
        default:   fb_putc(fb, *s); break;
        }
    }
}

int krec_escape_fd(int fd, const char *s)
{
    struct fd_buf fb;

    fb_init(&fb, fd);
    fb_escape(&fb, s);
    fb_flush(&fb);
    return fb.err ? -1 : 0;
}

int krec_escape_print(const char *s)
{
    return krec_escape_fd(1, s);
}

int krec_begin(int fd, const struct krec_col *cols, int ncols)
{
    struct fd_buf fb;
    int i;

    if (!cols || ncols <= 0 || ncols > KREC_MAX_COLS)
        return -1;

    fb_init(&fb, fd);
    fb_puts(&fb, "@krec");
    for (i = 0; i < ncols; i++) {
        fb_putc(&fb, ' ');
        fb_puts(&fb, cols[i].name);
        fb_putc(&fb, ':');
        fb_puts(&fb, krec_type_name(cols[i].type));
    }
    fb_putc(&fb, '\n');
    fb_flush(&fb);
    return fb.err ? -1 : 0;
}

int krec_begin_schema(int fd, const struct krec_schema *s)
{
    if (!s)
        return -1;
    return krec_begin(fd, s->cols, s->ncols);
}

int krec_row_vals(int fd, int ncols, const char *const *vals)
{
    struct fd_buf fb;
    int i;

    if (!vals || ncols <= 0 || ncols > KREC_MAX_COLS)
        return -1;

    fb_init(&fb, fd);
    for (i = 0; i < ncols; i++) {
        if (i > 0)
            fb_putc(&fb, '\t');
        fb_escape(&fb, vals[i] ? vals[i] : "");
    }
    fb_putc(&fb, '\n');
    fb_flush(&fb);
    return fb.err ? -1 : 0;
}

int krec_row(int fd, int ncols, ...)
{
    const char *vals[KREC_MAX_COLS];
    va_list ap;
    int i;

    if (ncols <= 0 || ncols > KREC_MAX_COLS)
        return -1;

    va_start(ap, ncols);
    for (i = 0; i < ncols; i++)
        vals[i] = va_arg(ap, const char *);
    va_end(ap);
    return krec_row_vals(fd, ncols, vals);
}

/* ---- reader & schema API --------------------------------------------- */

int krec_parse_header(const char *line, struct krec_schema *s)
{
    const char *p = line;

    if (!line || !s)
        return -1;
    memset(s, 0, sizeof(*s));

    if (strncmp(p, "@krec", 5) != 0)
        return -1;
    p += 5;
    if (*p != ' ' && *p != '\t')
        return -1;

    while (*p) {
        const char *tok, *colon;
        size_t nlen, tlen;
        char tbuf[16];
        size_t i;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '\r' || *p == '\n')
            break;
        if (s->ncols >= KREC_MAX_COLS)
            return -1;

        tok = p;
        while (*p && !is_space_ch(*p))
            p++;

        colon = 0;
        for (const char *q = tok; q < p; q++) {
            if (*q == ':') {
                colon = q;
                break;
            }
        }
        if (!colon || colon == tok || colon + 1 >= p)
            return -1;

        nlen = (size_t)(colon - tok);
        if (nlen >= KREC_NAME_MAX)
            return -1;
        for (i = 0; i < nlen; i++) {
            if (!is_ident_ch(tok[i]))
                return -1;
        }
        memcpy(s->cols[s->ncols].name, tok, nlen);
        s->cols[s->ncols].name[nlen] = '\0';

        tlen = (size_t)(p - (colon + 1));
        if (tlen >= sizeof(tbuf))
            return -1;
        memcpy(tbuf, colon + 1, tlen);
        tbuf[tlen] = '\0';
        if (krec_parse_type(tbuf, &s->cols[s->ncols].type) < 0)
            return -1;

        s->ncols++;
    }

    return s->ncols > 0 ? 0 : -1;
}

int krec_find_col(const struct krec_schema *s, const char *name)
{
    int i;

    if (!s || !name)
        return -1;
    for (i = 0; i < s->ncols; i++) {
        if (strcmp(s->cols[i].name, name) == 0)
            return i;
    }
    return -1;
}

void krec_reader_init(struct krec_reader *r, int fd)
{
    memset(r, 0, sizeof(*r));
    r->fd = fd;
    r->tty = (fd == 0);
}

/* Read one line (up to '\n' or EOF). Returns:
 *   1 = line read (length in *len_out, '\n' and trailing '\r' stripped)
 *   0 = clean EOF before any byte
 *  -1 = read error or line exceeded bufsz - 1
 */
static int reader_getline(struct krec_reader *r, char *out, size_t outsz,
                          size_t *len_out)
{
    size_t n = 0;
    int got = 0;
    int overflow = 0;

    if (r->eof || r->err)
        return r->err ? -1 : 0;

    for (;;) {
        char c;
        if (r->pos >= r->len) {
            long g = read(r->fd, r->buf, sizeof(r->buf));
            if (g < 0) {
                r->err = 1;
                return -1;
            }
            if (g == 0) {
                r->eof = 1;
                break;
            }
            r->len = (int)g;
            r->pos = 0;
        }
        c = r->buf[r->pos++];
        if (r->tty && c == CTRL_D) {
            r->eof = 1;
            break;
        }
        got = 1;
        if (c == '\n')
            break;
        if (n + 1 < outsz)
            out[n++] = c;
        else
            overflow = 1;
    }

    if (!got)
        return 0;
    if (overflow) {
        r->err = 1;
        return -1;
    }
    if (n > 0 && out[n - 1] == '\r')
        n--;
    out[n] = '\0';
    if (len_out)
        *len_out = n;
    return 1;
}

static void unescape_inplace(char *s)
{
    char *r = s, *w = s;

    while (*r) {
        if (*r == '\\' && r[1] != '\0') {
            r++;
            switch (*r) {
            case 't':  *w++ = '\t'; break;
            case 'n':  *w++ = '\n'; break;
            case 'r':  *w++ = '\r'; break;
            case '\\': *w++ = '\\'; break;
            default:   *w++ = *r;   break;
            }
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

int krec_read_header(struct krec_reader *r, struct krec_schema *out)
{
    char line[KREC_LINE_MAX];
    size_t len = 0;
    int rc;

    if (!r)
        return -1;
    if (r->has_schema) {
        if (out)
            *out = r->schema;
        return 1;
    }

    for (;;) {
        rc = reader_getline(r, line, sizeof(line), &len);
        if (rc < 0)
            return -1;
        if (rc == 0)
            return 0;
        if (len == 0)
            continue;
        break;
    }

    if (krec_parse_header(line, &r->schema) < 0) {
        r->err = 1;
        return -1;
    }
    r->has_schema = 1;
    if (out)
        *out = r->schema;
    return 1;
}

int krec_next(struct krec_reader *r, char *buf, size_t bufsz,
              char **fields, int max_fields)
{
    size_t len = 0;
    int rc, nf, i;
    char *p;

    if (!r || !buf || bufsz == 0 || !fields)
        return -1;

    if (!r->has_schema) {
        rc = krec_read_header(r, 0);
        if (rc <= 0)
            return rc;
    }
    if (max_fields < r->schema.ncols)
        return -1;

    for (;;) {
        rc = reader_getline(r, buf, bufsz, &len);
        if (rc <= 0)
            return rc;
        if (len == 0 && r->schema.ncols > 1)
            continue;
        break;
    }

    nf = 0;
    p = buf;
    fields[nf++] = p;
    while (*p) {
        if (*p == '\t') {
            *p++ = '\0';
            if (nf >= r->schema.ncols) {
                r->err = 1;
                return -1;
            }
            fields[nf++] = p;
        } else {
            p++;
        }
    }

    if (nf != r->schema.ncols) {
        r->err = 1;
        return -1;
    }

    for (i = 0; i < nf; i++)
        unescape_inplace(fields[i]);

    return 1;
}

/* ---- typed value parsers & comparison -------------------------------- */

int krec_parse_i64(const char *s, int64_t *out)
{
    uint64_t acc = 0, limit;
    int neg = 0, digits = 0;

    if (!s || !out)
        return -1;
    s = skip_ws(s);
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    limit = neg ? ((uint64_t)INT64_MAX + 1ULL) : (uint64_t)INT64_MAX;
    while (*s >= '0' && *s <= '9') {
        uint64_t d = (uint64_t)(*s - '0');
        if (acc > (limit - d) / 10ULL)
            return -1;
        acc = acc * 10ULL + d;
        digits = 1;
        s++;
    }
    if (!digits || !rest_is_ws(s))
        return -1;
    if (neg) {
        if (acc == (uint64_t)INT64_MAX + 1ULL)
            *out = INT64_MIN;
        else
            *out = -(int64_t)acc;
    } else {
        *out = (int64_t)acc;
    }
    return 0;
}

int krec_parse_u64(const char *s, uint64_t *out)
{
    uint64_t acc = 0;
    int digits = 0;

    if (!s || !out)
        return -1;
    s = skip_ws(s);
    if (*s == '+')
        s++;
    while (*s >= '0' && *s <= '9') {
        uint64_t d = (uint64_t)(*s - '0');
        if (acc > (UINT64_MAX - d) / 10ULL)
            return -1;
        acc = acc * 10ULL + d;
        digits = 1;
        s++;
    }
    if (!digits || !rest_is_ws(s))
        return -1;
    *out = acc;
    return 0;
}

int krec_parse_hex(const char *s, uint64_t *out)
{
    uint64_t acc = 0;
    int digits = 0;

    if (!s || !out)
        return -1;
    s = skip_ws(s);
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    while (*s) {
        uint64_t d;
        if (*s >= '0' && *s <= '9')
            d = (uint64_t)(*s - '0');
        else if (*s >= 'a' && *s <= 'f')
            d = (uint64_t)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F')
            d = (uint64_t)(*s - 'A' + 10);
        else
            break;
        if (acc > (UINT64_MAX - d) / 16ULL)
            return -1;
        acc = acc * 16ULL + d;
        digits = 1;
        s++;
    }
    if (!digits || !rest_is_ws(s))
        return -1;
    *out = acc;
    return 0;
}

int krec_parse_bytes(const char *s, uint64_t *out)
{
    uint64_t acc = 0, mult = 1;
    int digits = 0;

    if (!s || !out)
        return -1;
    s = skip_ws(s);
    while (*s >= '0' && *s <= '9') {
        uint64_t d = (uint64_t)(*s - '0');
        if (acc > (UINT64_MAX - d) / 10ULL)
            return -1;
        acc = acc * 10ULL + d;
        digits = 1;
        s++;
    }
    if (!digits)
        return -1;
    s = skip_ws(s);
    if (*s) {
        char u = (char)lower_ch((unsigned char)*s);
        if (u == 'k')      { mult = 1024ULL; s++; }
        else if (u == 'm') { mult = 1024ULL * 1024ULL; s++; }
        else if (u == 'g') { mult = 1024ULL * 1024ULL * 1024ULL; s++; }
        else if (u == 't') { mult = 1024ULL * 1024ULL * 1024ULL * 1024ULL; s++; }
        if (mult > 1 && (*s == 'i' || *s == 'I'))
            s++;
        if (*s == 'b' || *s == 'B')
            s++;
    }
    if (!rest_is_ws(s))
        return -1;
    if (acc > UINT64_MAX / mult)
        return -1;
    *out = acc * mult;
    return 0;
}

int krec_parse_bool(const char *s, int *out)
{
    char buf[16];
    size_t i = 0;

    if (!s || !out)
        return -1;
    s = skip_ws(s);
    while (*s && !is_space_ch(*s) && i + 1 < sizeof(buf))
        buf[i++] = (char)lower_ch((unsigned char)*s++);
    buf[i] = '\0';
    if (!rest_is_ws(s))
        return -1;
    if (strcmp(buf, "true") == 0 || strcmp(buf, "1") == 0 ||
        strcmp(buf, "yes") == 0 || strcmp(buf, "on") == 0) {
        *out = 1;
        return 0;
    }
    if (strcmp(buf, "false") == 0 || strcmp(buf, "0") == 0 ||
        strcmp(buf, "no") == 0 || strcmp(buf, "off") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

int krec_parse_ip(const char *s, uint32_t *out_host)
{
    uint32_t ip = 0;
    int part;

    if (!s || !out_host)
        return -1;
    s = skip_ws(s);
    for (part = 0; part < 4; part++) {
        unsigned int v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10U + (unsigned int)(*s - '0');
            if (v > 255U)
                return -1;
            digits++;
            s++;
        }
        if (digits == 0 || digits > 3)
            return -1;
        ip = (ip << 8) | (uint32_t)v;
        if (part < 3) {
            if (*s != '.')
                return -1;
            s++;
        }
    }
    if (!rest_is_ws(s))
        return -1;
    *out_host = ip;
    return 0;
}

static int cmp_str(const char *a, const char *b, int icase)
{
    int c = icase ? krec_strcasecmp(a, b) : strcmp(a, b);
    return (c > 0) - (c < 0);
}

int krec_compare(enum krec_type t, const char *a, const char *b,
                 int force_numeric, int icase)
{
    if (!a) a = "";
    if (!b) b = "";

    if (force_numeric && t != KREC_INT && t != KREC_U64 &&
        t != KREC_HEX && t != KREC_BYTES) {
        int64_t ia, ib;
        uint64_t ua, ub;
        int aok = (krec_parse_i64(a, &ia) == 0);
        int bok = (krec_parse_i64(b, &ib) == 0);
        if (aok && bok)
            return (ia > ib) - (ia < ib);
        if (aok != bok)
            return aok ? 1 : -1;
        aok = (krec_parse_u64(a, &ua) == 0);
        bok = (krec_parse_u64(b, &ub) == 0);
        if (aok && bok)
            return (ua > ub) - (ua < ub);
        if (aok != bok)
            return aok ? 1 : -1;
        return cmp_str(a, b, icase);
    }

    switch (t) {
    case KREC_INT: {
        int64_t ia = 0, ib = 0;
        int aok = (krec_parse_i64(a, &ia) == 0);
        int bok = (krec_parse_i64(b, &ib) == 0);
        if (aok && bok)
            return (ia > ib) - (ia < ib);
        if (aok != bok)
            return aok ? 1 : -1;
        break;
    }
    case KREC_U64: {
        uint64_t ua = 0, ub = 0;
        int aok = (krec_parse_u64(a, &ua) == 0);
        int bok = (krec_parse_u64(b, &ub) == 0);
        if (aok && bok)
            return (ua > ub) - (ua < ub);
        if (aok != bok)
            return aok ? 1 : -1;
        break;
    }
    case KREC_HEX: {
        uint64_t ua = 0, ub = 0;
        int aok = (krec_parse_hex(a, &ua) == 0);
        int bok = (krec_parse_hex(b, &ub) == 0);
        if (aok && bok)
            return (ua > ub) - (ua < ub);
        if (aok != bok)
            return aok ? 1 : -1;
        break;
    }
    case KREC_BYTES: {
        uint64_t ua = 0, ub = 0;
        int aok = (krec_parse_bytes(a, &ua) == 0);
        int bok = (krec_parse_bytes(b, &ub) == 0);
        if (aok && bok)
            return (ua > ub) - (ua < ub);
        if (aok != bok)
            return aok ? 1 : -1;
        break;
    }
    case KREC_IP: {
        uint32_t ipa = 0, ipb = 0;
        int aok = (krec_parse_ip(a, &ipa) == 0);
        int bok = (krec_parse_ip(b, &ipb) == 0);
        if (aok && bok)
            return (ipa > ipb) - (ipa < ipb);
        if (aok != bok)
            return aok ? 1 : -1;
        break;
    }
    case KREC_BOOL: {
        int ba = 0, bb = 0;
        int aok = (krec_parse_bool(a, &ba) == 0);
        int bok = (krec_parse_bool(b, &bb) == 0);
        if (aok && bok)
            return (ba > bb) - (ba < bb);
        if (aok != bok)
            return aok ? 1 : -1;
        break;
    }
    case KREC_STR:
    default:
        break;
    }
    return cmp_str(a, b, icase);
}

/* ---- aligned table printer ------------------------------------------- */

static void fb_pad_cell(struct fd_buf *fb, const char *s, int width,
                        int right_align, int is_last)
{
    int len = (int)strlen(s);
    int pad = width - len;
    int i;

    if (right_align && pad > 0) {
        for (i = 0; i < pad; i++)
            fb_putc(fb, ' ');
    }
    for (; *s; s++) {
        char ch = (*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s;
        fb_putc(fb, ch);
    }
    if (!is_last && !right_align && pad > 0) {
        for (i = 0; i < pad; i++)
            fb_putc(fb, ' ');
    }
}

static int table_right_align(enum krec_type t)
{
    return t == KREC_INT || t == KREC_U64 || t == KREC_BYTES;
}

void krec_print_table(int fd, const struct krec_schema *s,
                      const char *const *const *rows, int nrows,
                      int show_header)
{
    int widths[KREC_MAX_COLS];
    struct fd_buf fb;
    int c, r;

    if (!s || s->ncols <= 0 || s->ncols > KREC_MAX_COLS)
        return;

    for (c = 0; c < s->ncols; c++) {
        widths[c] = show_header ? (int)strlen(s->cols[c].name) : 0;
        for (r = 0; r < nrows; r++) {
            const char *v = (rows && rows[r] && rows[r][c]) ? rows[r][c] : "";
            int l = (int)strlen(v);
            if (l > widths[c])
                widths[c] = l;
        }
    }

    fb_init(&fb, fd);
    if (show_header) {
        for (c = 0; c < s->ncols; c++) {
            int last = (c == s->ncols - 1);
            if (c > 0)
                fb_puts(&fb, "  ");
            fb_pad_cell(&fb, s->cols[c].name, widths[c],
                        table_right_align(s->cols[c].type), last);
        }
        fb_putc(&fb, '\n');
        for (c = 0; c < s->ncols; c++) {
            int i;
            if (c > 0)
                fb_puts(&fb, "  ");
            for (i = 0; i < widths[c]; i++)
                fb_putc(&fb, '-');
        }
        fb_putc(&fb, '\n');
    }

    for (r = 0; r < nrows; r++) {
        for (c = 0; c < s->ncols; c++) {
            int last = (c == s->ncols - 1);
            const char *v = (rows && rows[r] && rows[r][c]) ? rows[r][c] : "";
            if (c > 0)
                fb_puts(&fb, "  ");
            fb_pad_cell(&fb, v, widths[c],
                        table_right_align(s->cols[c].type), last);
        }
        fb_putc(&fb, '\n');
    }
    fb_flush(&fb);
}
