/* html.c - HTML subset parser and layout engine. See apps/html.h.
 *
 * Portable between KestrelOS userspace and the host: it includes only
 * <stdlib.h> and <string.h> and uses plain 0 rather than NULL, because
 * the KestrelOS headers do not define one. No printf, no syscalls, no
 * GUI, no globals - everything hangs off the parser or layout struct.
 */

#include <stdlib.h>
#include <string.h>

#include "html.h"

/* ------------------------------------------------------------------ *
 * ASCII helpers. The kernel and libc have no ctype.h, and locale-aware
 * versions would be wrong here anyway: HTML tag names are ASCII.
 * ------------------------------------------------------------------ */

static int h_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int h_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int h_alpha(int c)
{
    c = h_lower(c);
    return c >= 'a' && c <= 'z';
}

static int h_digit(int c)
{
    return c >= '0' && c <= '9';
}

/* Case-insensitive compare of a counted string against a C string.
 * `lit` must already be lower case. */
static int h_ieq(const char *s, int n, const char *lit)
{
    int i;
    for (i = 0; i < n; i++) {
        if (!lit[i] || h_lower((unsigned char)s[i]) != lit[i])
            return 0;
    }
    return lit[n] == 0;
}

/* Exact compare of a counted string against a C string. */
static int h_eq(const char *s, int n, const char *lit)
{
    int i;
    for (i = 0; i < n; i++) {
        if (!lit[i] || s[i] != lit[i])
            return 0;
    }
    return lit[n] == 0;
}

/* ------------------------------------------------------------------ *
 * Tag table
 * ------------------------------------------------------------------ */

enum {
    T_OTHER = 0, T_HTML, T_HEAD, T_BODY, T_TITLE, T_SCRIPT, T_STYLE,
    T_H1, T_H2, T_H3, T_H4, T_H5, T_H6,
    T_P, T_BR, T_HR, T_A, T_B, T_I, T_U, T_S,
    T_UL, T_OL, T_LI, T_PRE, T_CODE, T_BLOCKQUOTE, T_DIV, T_SPAN,
    T_TABLE, T_TR, T_TD, T_TH, T_IMG, T_DL, T_DT, T_DD, T_TEXTAREA,
    T_VOIDISH, T_GENERIC_BLOCK
};

#define TF_VOID  0x01u   /* no end tag, never pushed on the stack */
#define TF_BLOCK 0x02u   /* starts/ends a block: paragraph-sized break */
#define TF_SOFT  0x04u   /* starts/ends a line: single break */
#define TF_RAW   0x08u   /* content is raw text, not markup */

struct tag_info {
    const char *name;
    int id;
    unsigned int flags;
    unsigned int style;   /* style bits added while the element is open */
};

/* Longest names first is not required; lookup is an exact match. */
static const struct tag_info g_tags[] = {
    { "html",       T_HTML,       TF_BLOCK, 0 },
    { "head",       T_HEAD,       TF_BLOCK, 0 },
    { "body",       T_BODY,       TF_BLOCK, 0 },
    { "title",      T_TITLE,      TF_RAW,   0 },
    { "script",     T_SCRIPT,     TF_RAW,   0 },
    { "style",      T_STYLE,      TF_RAW,   0 },
    { "textarea",   T_TEXTAREA,   TF_RAW,   0 },
    { "h1",         T_H1,         TF_BLOCK, HS_BOLD },
    { "h2",         T_H2,         TF_BLOCK, HS_BOLD },
    { "h3",         T_H3,         TF_BLOCK, HS_BOLD },
    { "h4",         T_H4,         TF_BLOCK, HS_BOLD },
    { "h5",         T_H5,         TF_BLOCK, HS_BOLD },
    { "h6",         T_H6,         TF_BLOCK, HS_BOLD },
    { "p",          T_P,          TF_BLOCK, 0 },
    { "br",         T_BR,         TF_VOID,  0 },
    { "hr",         T_HR,         TF_VOID,  0 },
    { "a",          T_A,          0,        HS_LINK | HS_UNDER },
    { "b",          T_B,          0,        HS_BOLD },
    { "strong",     T_B,          0,        HS_BOLD },
    { "i",          T_I,          0,        HS_ITALIC },
    { "em",         T_I,          0,        HS_ITALIC },
    { "cite",       T_I,          0,        HS_ITALIC },
    { "var",        T_I,          0,        HS_ITALIC },
    { "u",          T_U,          0,        HS_UNDER },
    { "ins",        T_U,          0,        HS_UNDER },
    { "s",          T_S,          0,        0 },
    { "strike",     T_S,          0,        0 },
    { "del",        T_S,          0,        0 },
    { "ul",         T_UL,         TF_BLOCK, 0 },
    { "ol",         T_OL,         TF_BLOCK, 0 },
    { "menu",       T_UL,         TF_BLOCK, 0 },
    { "li",         T_LI,         TF_SOFT,  0 },
    { "pre",        T_PRE,        TF_BLOCK, HS_PRE | HS_MONO },
    { "code",       T_CODE,       0,        HS_MONO },
    { "tt",         T_CODE,       0,        HS_MONO },
    { "kbd",        T_CODE,       0,        HS_MONO },
    { "samp",       T_CODE,       0,        HS_MONO },
    { "blockquote", T_BLOCKQUOTE, TF_BLOCK, 0 },
    { "div",        T_DIV,        TF_BLOCK, 0 },
    { "span",       T_SPAN,       0,        0 },
    { "table",      T_TABLE,      TF_BLOCK, 0 },
    { "thead",      T_GENERIC_BLOCK, TF_SOFT, 0 },
    { "tbody",      T_GENERIC_BLOCK, TF_SOFT, 0 },
    { "tfoot",      T_GENERIC_BLOCK, TF_SOFT, 0 },
    { "tr",         T_TR,         TF_SOFT,  0 },
    { "td",         T_TD,         0,        0 },
    { "th",         T_TH,         0,        HS_BOLD },
    { "caption",    T_GENERIC_BLOCK, TF_SOFT, HS_ITALIC },
    { "img",        T_IMG,        TF_VOID,  0 },
    { "dl",         T_DL,         TF_BLOCK, 0 },
    { "dt",         T_DT,         TF_SOFT,  HS_BOLD },
    { "dd",         T_DD,         TF_SOFT,  0 },
    /* HTML5 sectioning and other wrappers: block boxes, no style */
    { "section",    T_GENERIC_BLOCK, TF_BLOCK, 0 },
    { "article",    T_GENERIC_BLOCK, TF_BLOCK, 0 },
    { "header",     T_GENERIC_BLOCK, TF_BLOCK, 0 },
    { "footer",     T_GENERIC_BLOCK, TF_BLOCK, 0 },
    { "nav",        T_GENERIC_BLOCK, TF_BLOCK, 0 },
    { "main",       T_GENERIC_BLOCK, TF_BLOCK, 0 },
    { "aside",      T_GENERIC_BLOCK, TF_BLOCK, 0 },
    { "figure",     T_GENERIC_BLOCK, TF_BLOCK, 0 },
    { "figcaption", T_GENERIC_BLOCK, TF_BLOCK, HS_ITALIC },
    { "form",       T_GENERIC_BLOCK, TF_BLOCK, 0 },
    { "fieldset",   T_GENERIC_BLOCK, TF_BLOCK, 0 },
    { "address",    T_GENERIC_BLOCK, TF_BLOCK, HS_ITALIC },
    { "center",     T_GENERIC_BLOCK, TF_BLOCK, 0 },
    { "details",    T_GENERIC_BLOCK, TF_BLOCK, 0 },
    { "summary",    T_GENERIC_BLOCK, TF_SOFT,  HS_BOLD },
    /* void elements with nothing to render */
    { "meta",       T_VOIDISH,    TF_VOID,  0 },
    { "link",       T_VOIDISH,    TF_VOID,  0 },
    { "base",       T_VOIDISH,    TF_VOID,  0 },
    { "input",      T_VOIDISH,    TF_VOID,  0 },
    { "col",        T_VOIDISH,    TF_VOID,  0 },
    { "area",       T_VOIDISH,    TF_VOID,  0 },
    { "source",     T_VOIDISH,    TF_VOID,  0 },
    { "track",      T_VOIDISH,    TF_VOID,  0 },
    { "embed",      T_VOIDISH,    TF_VOID,  0 },
    { "param",      T_VOIDISH,    TF_VOID,  0 },
    { "wbr",        T_VOIDISH,    TF_VOID,  0 },
    { 0, 0, 0, 0 }
};

static const struct tag_info *tag_lookup(const char *name, int n)
{
    int i;
    if (n <= 0 || n > 16)
        return 0;
    for (i = 0; g_tags[i].name; i++) {
        if (h_ieq(name, n, g_tags[i].name))
            return &g_tags[i];
    }
    return 0;
}

static int heading_level(int id)
{
    if (id >= T_H1 && id <= T_H6)
        return id - T_H1 + 1;
    return 0;
}

/* ------------------------------------------------------------------ *
 * Entities
 * ------------------------------------------------------------------ */

struct ent_map { const char *name; const char *out; };

/* Everything folds to ASCII: the console font and the 8x16 bitmap font
 * are both US-ASCII only, so a faithful UTF-8 expansion would render as
 * garbage. Lookalikes are the honest compromise. */
static const struct ent_map g_ents[] = {
    { "amp", "&" },      { "lt", "<" },       { "gt", ">" },
    { "quot", "\"" },    { "apos", "'" },     { "nbsp", " " },
    { "copy", "(c)" },   { "reg", "(R)" },    { "trade", "(TM)" },
    { "mdash", "--" },   { "ndash", "-" },    { "minus", "-" },
    { "hellip", "..." }, { "middot", "*" },   { "bull", "*" },
    { "laquo", "<<" },   { "raquo", ">>" },   { "deg", " deg" },
    { "ldquo", "\"" },   { "rdquo", "\"" },   { "lsquo", "'" },
    { "rsquo", "'" },    { "sbquo", "," },    { "bdquo", "\"" },
    { "times", "x" },    { "divide", "/" },   { "frasl", "/" },
    { "euro", "EUR" },   { "pound", "GBP" },  { "yen", "JPY" },
    { "cent", "c" },     { "sect", "S" },     { "para", "P" },
    { "dagger", "+" },   { "permil", "o/oo" },{ "larr", "<-" },
    { "rarr", "->" },    { "harr", "<->" },   { "shy", "" },
    { "ensp", " " },     { "emsp", " " },     { "thinsp", " " },
    { "zwnj", "" },      { "zwj", "" },       { "lrm", "" },
    { "rlm", "" },
    /* Latin-1 letters, folded to their unaccented ASCII base. */
    { "aacute", "a" },   { "agrave", "a" },   { "acirc", "a" },
    { "auml", "ae" },    { "atilde", "a" },   { "aring", "a" },
    { "aelig", "ae" },   { "ccedil", "c" },   { "eacute", "e" },
    { "egrave", "e" },   { "ecirc", "e" },    { "euml", "e" },
    { "iacute", "i" },   { "igrave", "i" },   { "icirc", "i" },
    { "iuml", "i" },     { "ntilde", "n" },   { "oacute", "o" },
    { "ograve", "o" },   { "ocirc", "o" },    { "ouml", "oe" },
    { "otilde", "o" },   { "oslash", "o" },   { "uacute", "u" },
    { "ugrave", "u" },   { "ucirc", "u" },    { "uuml", "ue" },
    { "yacute", "y" },   { "yuml", "y" },     { "szlig", "ss" },
    { "Aacute", "A" },   { "Agrave", "A" },   { "Acirc", "A" },
    { "Auml", "Ae" },    { "Atilde", "A" },   { "Aring", "A" },
    { "AElig", "AE" },   { "Ccedil", "C" },   { "Eacute", "E" },
    { "Egrave", "E" },   { "Ecirc", "E" },    { "Euml", "E" },
    { "Iacute", "I" },   { "Ntilde", "N" },   { "Oacute", "O" },
    { "Ograve", "O" },   { "Ocirc", "O" },    { "Ouml", "Oe" },
    { "Oslash", "O" },   { "Uacute", "U" },   { "Uuml", "Ue" },
    { 0, 0 }
};

/* Map a Unicode code point to an ASCII stand-in. */
static const char *cp_to_ascii(long cp, char *tmp)
{
    if (cp == 9 || cp == 10) {
        tmp[0] = (char)cp;
        tmp[1] = 0;
        return tmp;
    }
    if (cp >= 32 && cp < 127) {
        tmp[0] = (char)cp;
        tmp[1] = 0;
        return tmp;
    }
    switch (cp) {
    case 160:  return " ";
    case 8211: return "-";
    case 8212: return "--";
    case 8216: case 8217: case 700: return "'";
    case 8220: case 8221: return "\"";
    case 8230: return "...";
    case 8226: return "*";
    case 8592: return "<-";
    case 8594: return "->";
    case 8722: return "-";
    case 169:  return "(c)";
    case 174:  return "(R)";
    case 8482: return "(TM)";
    case 173:  return "";
    default:   break;
    }
    return "?";
}

/* Decode the entity starting at s[0] == '&'. On success stores the
 * replacement in *out (a pointer into `tmp` or a literal) and the number
 * of input bytes consumed in *used, and returns 1. Returns 0 when this
 * is not a well-formed entity, in which case the caller emits '&'. */
static int decode_entity(const char *s, unsigned long avail,
                         char *tmp, const char **out, int *used)
{
    unsigned long i;
    long cp = 0;

    if (avail < 3 || s[0] != '&')
        return 0;

    if (s[1] == '#') {
        int hex = (s[2] == 'x' || s[2] == 'X');
        i = hex ? 3 : 2;
        if (i >= avail || (hex ? !((h_digit(s[i])) ||
                                   (h_lower((unsigned char)s[i]) >= 'a' &&
                                    h_lower((unsigned char)s[i]) <= 'f'))
                               : !h_digit(s[i])))
            return 0;
        for (; i < avail && i < 12; i++) {
            int c = h_lower((unsigned char)s[i]);
            int v;
            if (h_digit(c))
                v = c - '0';
            else if (hex && c >= 'a' && c <= 'f')
                v = c - 'a' + 10;
            else
                break;
            cp = cp * (hex ? 16 : 10) + v;
            if (cp > 0x10FFFF)
                cp = 0xFFFD;
        }
        if (i >= avail || s[i] != ';')
            return 0;
        *out = cp_to_ascii(cp, tmp);
        *used = (int)i + 1;
        return 1;
    }

    for (i = 1; i < avail && i < 12; i++) {
        if (s[i] == ';')
            break;
        if (!h_alpha((unsigned char)s[i]) && !h_digit((unsigned char)s[i]))
            return 0;
    }
    if (i >= avail || i == 1 || s[i] != ';')
        return 0;
    {
        /* Entity names are case sensitive (&Eacute; is not &eacute;), but
         * the legacy all-caps spellings (&AMP;) are common enough to be
         * worth a second, case-insensitive pass. */
        int n = (int)i - 1, k;
        for (k = 0; g_ents[k].name; k++) {
            if (h_eq(s + 1, n, g_ents[k].name)) {
                *out = g_ents[k].out;
                *used = (int)i + 1;
                return 1;
            }
        }
        for (k = 0; g_ents[k].name; k++) {
            if (h_ieq(s + 1, n, g_ents[k].name)) {
                *out = g_ents[k].out;
                *used = (int)i + 1;
                return 1;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * Parser
 * ------------------------------------------------------------------ */

/* Boxes are built with arena offsets because the arena moves as it grows;
 * the offsets become pointers once parsing is done. */
struct bbox {
    int kind;
    unsigned int style;
    int heading;
    int indent;
    long text;
    long href;
};

#define STACK_MAX 64

struct stk_ent {
    int id;
    unsigned int flags;
    unsigned int style;
    long href;
    int heading;
    int indent;
    int list_kind;     /* 0 none, 1 ul, 2 ol */
    int counter;       /* <ol> item number */
};

struct parser {
    const char *src;
    unsigned long len, pos;

    struct bbox *box;
    int nbox, cbox;

    char *arena;
    unsigned long alen, acap;

    char *pend;        /* text of the run being accumulated */
    int plen, pcap;

    unsigned int style;
    long href;
    int heading;
    int indent;
    int in_pre;

    int want;          /* queued break: 0 none, 1 line, 2 block */
    int sp_pending;    /* collapsed whitespace waiting to be emitted */
    int inline_started;
    int cell;          /* cells seen in the current table row */

    struct stk_ent stk[STACK_MAX];
    int sp;

    long title;
    int truncated;
    int oom;
};

static int arena_reserve(struct parser *p, unsigned long extra)
{
    unsigned long need = p->alen + extra;
    char *n;
    unsigned long cap;

    if (need <= p->acap)
        return 1;
    cap = p->acap ? p->acap : 1024;
    while (cap < need)
        cap *= 2;
    n = (char *)realloc(p->arena, cap);
    if (!n) {
        p->oom = 1;
        return 0;
    }
    p->arena = n;
    p->acap = cap;
    return 1;
}

/* Copy n bytes plus a NUL into the arena; returns the offset or -1. */
static long arena_put(struct parser *p, const char *s, int n)
{
    long off;
    if (n < 0)
        n = 0;
    if (!arena_reserve(p, (unsigned long)n + 1))
        return -1;
    off = (long)p->alen;
    if (n)
        memcpy(p->arena + p->alen, s, (unsigned long)n);
    p->alen += (unsigned long)n;
    p->arena[p->alen++] = 0;
    return off;
}

static int emit(struct parser *p, int kind, long text, long href)
{
    struct bbox *b;

    if (p->nbox >= HTML_MAX_BOXES) {
        p->truncated = 1;
        return 0;
    }
    if (p->nbox == p->cbox) {
        int cap = p->cbox ? p->cbox * 2 : 64;
        struct bbox *n = (struct bbox *)realloc(p->box,
                                                (unsigned long)cap * sizeof *n);
        if (!n) {
            p->oom = 1;
            return 0;
        }
        p->box = n;
        p->cbox = cap;
    }
    b = &p->box[p->nbox++];
    b->kind = kind;
    b->style = p->style;
    b->heading = p->heading;
    b->indent = p->indent;
    b->text = text;
    b->href = href;
    return 1;
}

/* Emit the queued break, if any. Leading breaks are dropped. */
static void flush_want(struct parser *p)
{
    if (p->want) {
        if (p->nbox > 0)
            emit(p, p->want == 2 ? HB_BLOCK : HB_BREAK, -1, -1);
        p->want = 0;
    }
}

static void want_break(struct parser *p, int level)
{
    if (level > p->want)
        p->want = level;
    p->sp_pending = 0;
    p->inline_started = 0;
}

static int pend_reserve(struct parser *p, int extra)
{
    if (p->plen + extra + 1 > p->pcap) {
        int cap = p->pcap ? p->pcap : 128;
        char *n;
        while (cap < p->plen + extra + 1)
            cap *= 2;
        n = (char *)realloc(p->pend, (unsigned long)cap);
        if (!n) {
            p->oom = 1;
            return 0;
        }
        p->pend = n;
        p->pcap = cap;
    }
    return 1;
}

/* Append text verbatim, bypassing whitespace collapsing (table cell
 * separators, list markers). */
static void pend_raw(struct parser *p, const char *s, int n)
{
    if (!pend_reserve(p, n))
        return;
    memcpy(p->pend + p->plen, s, (unsigned long)n);
    p->plen += n;
    p->inline_started = 1;
}

/* Append one run of document text, collapsing whitespace outside <pre>. */
static void pend_text(struct parser *p, const char *s, int n)
{
    int i;

    if (p->in_pre) {
        for (i = 0; i < n; i++) {
            if (s[i] == '\r')
                continue;
            if (s[i] == '\t') {
                pend_raw(p, "    ", 4);
                continue;
            }
            pend_raw(p, s + i, 1);
        }
        return;
    }
    for (i = 0; i < n; i++) {
        if (h_space((unsigned char)s[i])) {
            if (p->inline_started || p->plen > 0)
                p->sp_pending = 1;
            continue;
        }
        if (p->sp_pending) {
            pend_raw(p, " ", 1);
            p->sp_pending = 0;
        }
        pend_raw(p, s + i, 1);
    }
}

/* Turn the accumulated text into boxes. Preformatted text is split at
 * newlines here so that the layout pass never has to look for one. */
static void flush_text(struct parser *p)
{
    int i, start;

    if (p->plen <= 0)
        return;

    if (!(p->style & HS_PRE)) {
        flush_want(p);
        emit(p, HB_TEXT, arena_put(p, p->pend, p->plen), p->href);
        p->plen = 0;
        return;
    }

    start = 0;
    for (i = 0; i <= p->plen; i++) {
        if (i == p->plen || p->pend[i] == '\n') {
            if (start > 0) {
                flush_want(p);
                if (p->nbox > 0)
                    emit(p, HB_BREAK, -1, -1);
            }
            if (i > start) {
                flush_want(p);
                emit(p, HB_TEXT, arena_put(p, p->pend + start, i - start),
                     p->href);
            }
            start = i + 1;
        }
    }
    p->plen = 0;
}

/* <br>: always its own break, so a<br><br>b really leaves a blank line. */
static void emit_br(struct parser *p)
{
    flush_text(p);
    flush_want(p);
    if (p->nbox > 0)
        emit(p, HB_BREAK, -1, -1);
    p->sp_pending = 0;
    p->inline_started = 0;
}

/* ---- element stack ---- */

static void apply_top(struct parser *p)
{
    /* Recompute the inline state from the stack. Cheaper to recompute
     * than to unwind, and it cannot drift out of sync. */
    int i;
    p->style = 0;
    p->href = -1;
    p->heading = 0;
    p->indent = 0;
    p->in_pre = 0;
    for (i = 0; i < p->sp; i++) {
        struct stk_ent *e = &p->stk[i];
        p->style |= e->style;
        if (e->href >= 0)
            p->href = e->href;
        if (heading_level(e->id))
            p->heading = heading_level(e->id);
        if (e->id == T_UL || e->id == T_OL || e->id == T_BLOCKQUOTE ||
            e->id == T_DL || e->id == T_DD)
            p->indent++;
        if (e->id == T_PRE)
            p->in_pre = 1;
    }
    if (p->href < 0)
        p->style &= ~(HS_LINK | HS_UNDER);
}

static void pop_one(struct parser *p)
{
    struct stk_ent *e;

    if (p->sp <= 0)
        return;
    e = &p->stk[p->sp - 1];
    flush_text(p);
    if (e->flags & TF_BLOCK)
        want_break(p, 2);
    else if (e->flags & TF_SOFT)
        want_break(p, 1);
    p->sp--;
    apply_top(p);
}

static int stack_find(struct parser *p, int id)
{
    int i;
    for (i = p->sp - 1; i >= 0; i--)
        if (p->stk[i].id == id)
            return i;
    return -1;
}

/* Close `id` if it is open, along with everything nested inside it.
 * This is what fixes unclosed <p>, <li> and stray inline tags. */
static void close_tag(struct parser *p, int id)
{
    int at = stack_find(p, id);
    if (at < 0)
        return;
    while (p->sp > at)
        pop_one(p);
}

/* Would `top` stop the search for an implied end tag of `want`?
 *
 * Without this, <ul><li>a<ul><li>b</ul></ul> collapses: the inner <li>
 * would find the OUTER <li> and close the nested <ul> along with it.
 * Implied end tags only reach up to the enclosing container. */
static int scope_boundary(int top, int want)
{
    switch (want) {
    case T_LI:
        return top == T_UL || top == T_OL || top == T_TABLE ||
               top == T_TD || top == T_TH || top == T_BLOCKQUOTE;
    case T_DT:
    case T_DD:
        return top == T_DL || top == T_TABLE || top == T_TD || top == T_TH;
    case T_TD:
    case T_TH:
    case T_TR:
        return top == T_TABLE;
    case T_P:
    default:
        return top == T_TABLE || top == T_TD || top == T_TH ||
               top == T_UL || top == T_OL || top == T_LI ||
               top == T_BLOCKQUOTE || top == T_DL || top == T_DIV;
    }
}

/* Close `id` only if it is open inside the current container. */
static void close_scoped(struct parser *p, int id)
{
    int i;

    for (i = p->sp - 1; i >= 0; i--) {
        if (p->stk[i].id == id) {
            while (p->sp > i)
                pop_one(p);
            return;
        }
        if (scope_boundary(p->stk[i].id, id))
            return;
    }
}

static void push(struct parser *p, const struct tag_info *t, long href)
{
    struct stk_ent *e;

    if (p->sp >= STACK_MAX) {
        /* Pathologically deep markup: keep the content, drop the box.
         * The matching close tag will simply find nothing to pop. */
        p->truncated = 1;
        return;
    }
    e = &p->stk[p->sp++];
    e->id = t->id;
    e->flags = t->flags;
    e->style = t->style;
    e->href = href;
    e->heading = heading_level(t->id);
    e->indent = 0;
    e->list_kind = (t->id == T_UL) ? 1 : (t->id == T_OL) ? 2 : 0;
    e->counter = 0;
    apply_top(p);
}

/* Format the marker for a list item: "* " in a <ul>, "N. " in an <ol>. */
static void list_marker(struct parser *p, char *out, int outsz)
{
    int i, n;
    struct stk_ent *lst = 0;
    char digits[16];

    for (i = p->sp - 1; i >= 0; i--) {
        if (p->stk[i].list_kind) {
            lst = &p->stk[i];
            break;
        }
    }
    if (!lst || lst->list_kind == 1) {
        if (outsz >= 3) {
            out[0] = '*';
            out[1] = ' ';
            out[2] = 0;
        } else if (outsz > 0) {
            out[0] = 0;
        }
        return;
    }
    n = ++lst->counter;
    i = 0;
    do {
        digits[i++] = (char)('0' + n % 10);
        n /= 10;
    } while (n && i < (int)sizeof digits);
    n = 0;
    while (i > 0 && n < outsz - 3)
        out[n++] = digits[--i];
    if (n < outsz - 2) {
        out[n++] = '.';
        out[n++] = ' ';
    }
    out[n] = 0;
}

/* ---- tag handling ---- */

static void open_element(struct parser *p, const struct tag_info *t,
                         long href, long alt, long src)
{
    int id = t->id;

    /* Implied end tags, the part that keeps real-world pages sane. */
    if ((t->flags & (TF_BLOCK | TF_SOFT)) || id == T_TD || id == T_TH)
        close_scoped(p, T_P);
    if (id == T_LI)
        close_scoped(p, T_LI);
    if (id == T_TR) {
        close_scoped(p, T_TD);
        close_scoped(p, T_TH);
        close_scoped(p, T_TR);
        p->cell = 0;
    }
    if (id == T_TD || id == T_TH) {
        close_scoped(p, T_TD);
        close_scoped(p, T_TH);
    }
    if (id == T_DT || id == T_DD) {
        close_scoped(p, T_DT);
        close_scoped(p, T_DD);
    }

    if (t->flags & TF_BLOCK) {
        flush_text(p);
        want_break(p, 2);
    } else if (t->flags & TF_SOFT) {
        flush_text(p);
        want_break(p, 1);
    }

    switch (id) {
    case T_BR:
        emit_br(p);
        return;
    case T_HR:
        flush_text(p);
        want_break(p, 2);
        flush_want(p);
        emit(p, HB_RULE, -1, -1);
        want_break(p, 2);
        return;
    case T_IMG:
        /* alt text is what the author wanted read out; the src is only a
         * fallback so the placeholder still says something useful. */
        flush_text(p);
        flush_want(p);
        emit(p, HB_IMAGE,
             alt >= 0 ? alt : src >= 0 ? src : arena_put(p, "image", 5),
             p->href);
        p->inline_started = 1;
        return;
    case T_VOIDISH:
        return;
    default:
        break;
    }

    /* The style is about to change, so whatever text is pending belongs
     * to the OLD style and has to become a box first. */
    flush_text(p);
    push(p, t, href);

    if (id == T_LI) {
        char mark[24];
        list_marker(p, mark, (int)sizeof mark);
        flush_want(p);
        emit(p, HB_BULLET, arena_put(p, mark, (int)strlen(mark)), -1);
    } else if (id == T_TD || id == T_TH) {
        /* Tables degrade to "cell | cell | cell" - see html.h. */
        if (p->cell++ > 0)
            pend_raw(p, " | ", 3);
    }
}

/* ---- attribute scanning ---- */

/* Copy an attribute value into the arena, decoding entities. */
static long attr_value(struct parser *p, const char *s, int n)
{
    char *tmp;
    int ti = 0, i = 0;
    long off;

    tmp = (char *)malloc((unsigned long)n + 8);
    if (!tmp) {
        p->oom = 1;
        return -1;
    }
    while (i < n) {
        if (s[i] == '&') {
            char ebuf[8];
            const char *rep = 0;
            int used = 0;
            if (decode_entity(s + i, (unsigned long)(n - i), ebuf, &rep,
                              &used)) {
                int rl = (int)strlen(rep);
                char *bigger = (char *)realloc(tmp,
                                               (unsigned long)ti + rl + 8);
                if (!bigger) {
                    free(tmp);
                    p->oom = 1;
                    return -1;
                }
                tmp = bigger;
                memcpy(tmp + ti, rep, (unsigned long)rl);
                ti += rl;
                i += used;
                continue;
            }
        }
        tmp[ti++] = s[i++];
    }
    off = arena_put(p, tmp, ti);
    free(tmp);
    return off;
}

/* Parse the attribute list of an open tag. `pos` is just past the tag
 * name; on return it is just past '>'. Sets *self_close. */
static void parse_attrs(struct parser *p, int tag_id, long *href, long *alt,
                        long *src, int *self_close)
{
    const char *s = p->src;
    unsigned long n = p->len;

    *self_close = 0;
    for (;;) {
        unsigned long ns, ne, vs, ve;

        while (p->pos < n && h_space((unsigned char)s[p->pos]))
            p->pos++;
        if (p->pos >= n)
            return;
        if (s[p->pos] == '>') {
            p->pos++;
            return;
        }
        if (s[p->pos] == '/') {
            p->pos++;
            *self_close = 1;
            continue;
        }
        /* attribute name */
        ns = p->pos;
        while (p->pos < n && !h_space((unsigned char)s[p->pos]) &&
               s[p->pos] != '=' && s[p->pos] != '>' && s[p->pos] != '/')
            p->pos++;
        ne = p->pos;
        if (ne == ns) {
            /* Not a name and not a delimiter: junk like `<a "">`. Skip
             * the byte so a malformed tag can never wedge the loop. */
            p->pos++;
            continue;
        }
        while (p->pos < n && h_space((unsigned char)s[p->pos]))
            p->pos++;
        vs = ve = p->pos;
        if (p->pos < n && s[p->pos] == '=') {
            p->pos++;
            while (p->pos < n && h_space((unsigned char)s[p->pos]))
                p->pos++;
            if (p->pos < n && (s[p->pos] == '"' || s[p->pos] == '\'')) {
                char q = s[p->pos++];
                vs = p->pos;
                while (p->pos < n && s[p->pos] != q)
                    p->pos++;
                ve = p->pos;
                if (p->pos < n)
                    p->pos++;   /* closing quote; missing one runs to EOF */
            } else {
                vs = p->pos;
                while (p->pos < n && !h_space((unsigned char)s[p->pos]) &&
                       s[p->pos] != '>')
                    p->pos++;
                ve = p->pos;
            }
        }

        if (tag_id == T_A && h_ieq(s + ns, (int)(ne - ns), "href") && ve > vs)
            *href = attr_value(p, s + vs, (int)(ve - vs));
        else if (tag_id == T_IMG && h_ieq(s + ns, (int)(ne - ns), "alt") &&
                 ve > vs && *alt < 0)
            *alt = attr_value(p, s + vs, (int)(ve - vs));
        else if (tag_id == T_IMG && h_ieq(s + ns, (int)(ne - ns), "src") &&
                 ve > vs && *src < 0)
            *src = attr_value(p, s + vs, (int)(ve - vs));
    }
}

/* Skip to the end of a raw-text element, returning the content range. */
static void raw_content(struct parser *p, const char *name,
                        unsigned long *cs, unsigned long *ce)
{
    const char *s = p->src;
    unsigned long n = p->len;
    int nl = (int)strlen(name);

    *cs = p->pos;
    while (p->pos < n) {
        if (s[p->pos] == '<' && p->pos + 1 < n && s[p->pos + 1] == '/' &&
            p->pos + 2 + (unsigned long)nl <= n &&
            h_ieq(s + p->pos + 2, nl, name)) {
            unsigned long after = p->pos + 2 + (unsigned long)nl;
            if (after >= n || h_space((unsigned char)s[after]) ||
                s[after] == '>') {
                *ce = p->pos;
                while (p->pos < n && s[p->pos] != '>')
                    p->pos++;
                if (p->pos < n)
                    p->pos++;
                return;
            }
        }
        p->pos++;
    }
    *ce = n;
}

/* ------------------------------------------------------------------ *
 * html_parse
 * ------------------------------------------------------------------ */

struct html_doc *html_parse(const char *src, unsigned long len)
{
    struct parser p;
    struct html_doc *d;
    int i;

    memset(&p, 0, sizeof p);
    p.src = src ? src : "";
    p.len = src ? len : 0;
    p.href = -1;
    p.title = -1;
    if (p.len > HTML_MAX_INPUT) {
        p.len = HTML_MAX_INPUT;
        p.truncated = 1;
    }

    while (p.pos < p.len && !p.oom) {
        char c = p.src[p.pos];

        if (c != '<') {
            unsigned long start = p.pos;
            while (p.pos < p.len && p.src[p.pos] != '<' &&
                   p.src[p.pos] != '&')
                p.pos++;
            if (p.pos > start)
                pend_text(&p, p.src + start, (int)(p.pos - start));
            if (p.pos < p.len && p.src[p.pos] == '&') {
                char ebuf[8];
                const char *rep = 0;
                int used = 0;
                if (decode_entity(p.src + p.pos, p.len - p.pos, ebuf, &rep,
                                  &used)) {
                    int rl = (int)strlen(rep);
                    if (rl) {
                        /* Decoded text is literal: a decoded '<' must not
                         * be re-scanned as markup, and &nbsp; is a real
                         * space that does not collapse with its
                         * neighbours (pages indent with runs of them). */
                        if (p.sp_pending) {
                            pend_raw(&p, " ", 1);
                            p.sp_pending = 0;
                        }
                        pend_raw(&p, rep, rl);
                    }
                    p.pos += (unsigned long)used;
                } else {
                    pend_text(&p, "&", 1);
                    p.pos++;
                }
            }
            continue;
        }

        /* markup */
        if (p.pos + 1 >= p.len) {
            pend_text(&p, "<", 1);
            p.pos++;
            continue;
        }

        if (p.src[p.pos + 1] == '!') {
            if (p.pos + 3 < p.len && p.src[p.pos + 2] == '-' &&
                p.src[p.pos + 3] == '-') {
                p.pos += 4;
                while (p.pos + 2 < p.len &&
                       !(p.src[p.pos] == '-' && p.src[p.pos + 1] == '-' &&
                         p.src[p.pos + 2] == '>'))
                    p.pos++;
                p.pos = (p.pos + 3 <= p.len) ? p.pos + 3 : p.len;
            } else {
                /* DOCTYPE, CDATA and friends: skipped whole. */
                while (p.pos < p.len && p.src[p.pos] != '>')
                    p.pos++;
                if (p.pos < p.len)
                    p.pos++;
            }
            continue;
        }
        if (p.src[p.pos + 1] == '?') {
            while (p.pos < p.len && p.src[p.pos] != '>')
                p.pos++;
            if (p.pos < p.len)
                p.pos++;
            continue;
        }

        if (p.src[p.pos + 1] == '/') {
            unsigned long ns, ne;
            const struct tag_info *t;

            p.pos += 2;
            ns = p.pos;
            while (p.pos < p.len && (h_alpha((unsigned char)p.src[p.pos]) ||
                                     h_digit((unsigned char)p.src[p.pos])))
                p.pos++;
            ne = p.pos;
            while (p.pos < p.len && p.src[p.pos] != '>')
                p.pos++;
            if (p.pos < p.len)
                p.pos++;
            t = tag_lookup(p.src + ns, (int)(ne - ns));
            if (t && !(t->flags & TF_VOID)) {
                flush_text(&p);
                close_tag(&p, t->id);
            }
            /* A close tag for something never opened, or for an unknown
             * element, is simply dropped. */
            continue;
        }

        if (h_alpha((unsigned char)p.src[p.pos + 1])) {
            unsigned long ns, ne;
            const struct tag_info *t;
            struct tag_info other;
            long href = -1, alt = -1, src = -1;
            int self_close = 0;

            p.pos++;
            ns = p.pos;
            while (p.pos < p.len && (h_alpha((unsigned char)p.src[p.pos]) ||
                                     h_digit((unsigned char)p.src[p.pos]) ||
                                     p.src[p.pos] == '-'))
                p.pos++;
            ne = p.pos;
            t = tag_lookup(p.src + ns, (int)(ne - ns));
            if (!t) {
                other.name = "";
                other.id = T_OTHER;
                other.flags = 0;
                other.style = 0;
                t = &other;
            }
            parse_attrs(&p, t->id, &href, &alt, &src, &self_close);

            if (t->flags & TF_RAW) {
                unsigned long cs, ce;
                char nbuf[16];
                int nl = (int)(ne - ns);
                if (nl > 15)
                    nl = 15;
                for (i = 0; i < nl; i++)
                    nbuf[i] = (char)h_lower((unsigned char)p.src[ns + i]);
                nbuf[nl] = 0;
                raw_content(&p, nbuf, &cs, &ce);
                if (t->id == T_TITLE && p.title < 0) {
                    /* Reuse the text pipeline for collapsing + entities. */
                    int saved_plen = p.plen;
                    char *saved = 0;
                    if (saved_plen > 0) {
                        saved = (char *)malloc((unsigned long)saved_plen);
                        if (saved)
                            memcpy(saved, p.pend, (unsigned long)saved_plen);
                        p.plen = 0;
                    }
                    p.sp_pending = 0;
                    p.inline_started = 0;
                    {
                        unsigned long q = cs;
                        while (q < ce) {
                            if (p.src[q] == '&') {
                                char ebuf[8];
                                const char *rep = 0;
                                int used = 0;
                                if (decode_entity(p.src + q, ce - q, ebuf,
                                                  &rep, &used)) {
                                    pend_text(&p, rep, (int)strlen(rep));
                                    q += (unsigned long)used;
                                    continue;
                                }
                            }
                            pend_text(&p, p.src + q, 1);
                            q++;
                        }
                    }
                    p.title = arena_put(&p, p.pend, p.plen);
                    p.plen = 0;
                    if (saved) {
                        pend_reserve(&p, saved_plen);
                        if (p.pcap >= saved_plen) {
                            memcpy(p.pend, saved, (unsigned long)saved_plen);
                            p.plen = saved_plen;
                        }
                        free(saved);
                    }
                    p.sp_pending = 0;
                }
                continue;
            }

            open_element(&p, t, href, alt, src);
            if (self_close && !(t->flags & TF_VOID) && t->id != T_OTHER)
                close_tag(&p, t->id);
            else if (self_close && t->id == T_OTHER)
                close_tag(&p, T_OTHER);
            continue;
        }

        /* '<' followed by something that is not markup: literal text. */
        pend_text(&p, "<", 1);
        p.pos++;
    }

    while (p.sp > 0)
        pop_one(&p);
    flush_text(&p);

    free(p.pend);

    if (p.oom) {
        free(p.box);
        free(p.arena);
        return 0;
    }

    d = (struct html_doc *)malloc(sizeof *d);
    if (!d) {
        free(p.box);
        free(p.arena);
        return 0;
    }
    d->nbox = p.nbox;
    d->truncated = p.truncated;
    d->arena = p.arena;
    d->title = (p.title >= 0 && p.arena) ? p.arena + p.title : 0;
    d->boxes = 0;
    if (p.nbox > 0) {
        d->boxes = (struct html_box *)malloc((unsigned long)p.nbox *
                                             sizeof *d->boxes);
        if (!d->boxes) {
            free(p.box);
            free(p.arena);
            free(d);
            return 0;
        }
        for (i = 0; i < p.nbox; i++) {
            d->boxes[i].kind = p.box[i].kind;
            d->boxes[i].style = p.box[i].style;
            d->boxes[i].heading = p.box[i].heading;
            d->boxes[i].indent = p.box[i].indent;
            d->boxes[i].text = p.box[i].text >= 0 ? p.arena + p.box[i].text : 0;
            d->boxes[i].href = p.box[i].href >= 0 ? p.arena + p.box[i].href : 0;
        }
    }
    free(p.box);
    return d;
}

void html_free(struct html_doc *d)
{
    if (!d)
        return;
    free(d->boxes);
    free(d->arena);
    free(d);
}

/* ------------------------------------------------------------------ *
 * Layout
 * ------------------------------------------------------------------ */

struct lstate {
    struct html_layout *l;
    const struct html_metrics *m;
    int crun, cline;
    long *off;          /* arena offset per run, patched at the end */
    char *arena;
    unsigned long alen, acap;
    int x;              /* pen position on the current line */
    int base;           /* left edge of the current block */
    int hang;           /* left edge of continuation lines */
    int y;
    int lh;             /* tallest run on the current line */
    int line_first;
    int line_runs;
    int oom;
};

static int lay_reserve_runs(struct lstate *s, int need)
{
    struct html_layout *l = s->l;
    if (need <= s->crun)
        return 1;
    {
        int cap = s->crun ? s->crun : 64;
        struct html_run *nr;
        long *no;
        while (cap < need)
            cap *= 2;
        nr = (struct html_run *)realloc(l->runs,
                                        (unsigned long)cap * sizeof *nr);
        if (!nr) {
            s->oom = 1;
            return 0;
        }
        l->runs = nr;
        no = (long *)realloc(s->off, (unsigned long)cap * sizeof *no);
        if (!no) {
            s->oom = 1;
            return 0;
        }
        s->off = no;
        s->crun = cap;
    }
    return 1;
}

static long lay_str(struct lstate *s, const char *t, int n)
{
    long off;
    if (s->alen + (unsigned long)n + 1 > s->acap) {
        unsigned long cap = s->acap ? s->acap : 1024;
        char *na;
        while (cap < s->alen + (unsigned long)n + 1)
            cap *= 2;
        na = (char *)realloc(s->arena, cap);
        if (!na) {
            s->oom = 1;
            return -1;
        }
        s->arena = na;
        s->acap = cap;
    }
    off = (long)s->alen;
    if (n)
        memcpy(s->arena + s->alen, t, (unsigned long)n);
    s->alen += (unsigned long)n;
    s->arena[s->alen++] = 0;
    return off;
}

static void lay_add_run(struct lstate *s, const struct html_box *b, int box,
                        const char *text, int n, int w, int h)
{
    struct html_run *r;

    if (!lay_reserve_runs(s, s->l->nrun + 1))
        return;
    r = &s->l->runs[s->l->nrun];
    r->x = s->x;
    r->y = 0;               /* set when the line is closed */
    r->w = w;
    r->h = h;
    r->kind = b->kind;
    r->style = b->style;
    r->heading = b->heading;
    r->text = 0;
    r->href = b->href;
    r->box = box;
    s->off[s->l->nrun] = text ? lay_str(s, text, n) : -1;
    if (s->oom)
        return;
    s->l->nrun++;
    s->line_runs++;
    if (h > s->lh)
        s->lh = h;
    s->x += w;
}

static int lay_reserve_lines(struct lstate *s, int need)
{
    struct html_layout *l = s->l;
    int cap;
    struct html_line *nl;

    if (need <= s->cline)
        return 1;
    cap = s->cline ? s->cline : 32;
    while (cap < need)
        cap *= 2;
    nl = (struct html_line *)realloc(l->lines,
                                     (unsigned long)cap * sizeof *nl);
    if (!nl) {
        s->oom = 1;
        return 0;
    }
    l->lines = nl;
    s->cline = cap;
    return 1;
}

/* Close the current line. `force` records an empty line too (a <br> on a
 * line with no content still has to take vertical space). */
static void lay_endline(struct lstate *s, int force)
{
    struct html_line *ln;
    int i, h;

    if (s->line_runs == 0 && !force) {
        s->x = s->base;
        s->hang = s->base;
        return;
    }
    h = s->lh;
    if (h <= 0)
        h = s->m->line_h(s->m->ctx, 0, 0);
    if (!lay_reserve_lines(s, s->l->nline + 1))
        return;
    ln = &s->l->lines[s->l->nline++];
    ln->y = s->y;
    ln->h = h;
    ln->first = s->line_first;
    ln->count = s->line_runs;
    for (i = 0; i < s->line_runs; i++) {
        struct html_run *r = &s->l->runs[s->line_first + i];
        r->y = s->y + (h - r->h);      /* sit runs on the baseline */
    }
    s->y += h;
    s->line_first = s->l->nrun;
    s->line_runs = 0;
    s->lh = 0;
    s->x = s->hang;
}

/* Longest prefix of s[0..n) that fits in `avail`; at least one char. */
static int fit_prefix(struct lstate *s, const struct html_box *b,
                      const char *t, int n, int avail, int *wout)
{
    int lo = 1, hi = n, best = 1, bw;

    bw = s->m->text_w(s->m->ctx, t, 1, b->style, b->heading);
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int w = s->m->text_w(s->m->ctx, t, mid, b->style, b->heading);
        if (w <= avail) {
            best = mid;
            bw = w;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    *wout = bw;
    return best;
}

static void lay_text(struct lstate *s, const struct html_box *b, int box,
                     int width)
{
    const char *t = b->text;
    int n, i, h, placed;

    if (!t || !*t)
        return;
    n = (int)strlen(t);
    h = s->m->line_h(s->m->ctx, b->style, b->heading);

    if (b->style & HS_PRE) {
        /* No word wrapping in <pre>; hard-split so nothing is lost. */
        i = 0;
        while (i < n && !s->oom) {
            int avail = width - s->x;
            int w = s->m->text_w(s->m->ctx, t + i, n - i, b->style,
                                 b->heading);
            /* Once the line is empty there is nothing left to gain from
             * wrapping again, so hard-split rather than loop forever. */
            if (w <= avail || s->line_runs == 0) {
                if (w > avail) {
                    int take = fit_prefix(s, b, t + i, n - i, avail, &w);
                    lay_add_run(s, b, box, t + i, take, w, h);
                    i += take;
                    lay_endline(s, 1);
                    continue;
                }
                lay_add_run(s, b, box, t + i, n - i, w, h);
                break;
            }
            lay_endline(s, 1);
        }
        return;
    }

    i = 0;
    placed = 0;
    while (i < n && !s->oom) {
        int start, end, w, avail, lead = 0;

        while (i < n && t[i] == ' ') {
            i++;
            lead = 1;
        }
        if (i >= n)
            break;
        start = i;
        while (i < n && t[i] != ' ')
            i++;
        end = i;

        /* A leading space only survives if the line already has content. */
        if (lead && s->line_runs > 0) {
            int sw = s->m->text_w(s->m->ctx, " ", 1, b->style, b->heading);
            if (s->x + sw < width) {
                if (placed) {
                    lay_add_run(s, b, box, " ", 1, sw, h);
                } else {
                    /* The space before the first word came from whatever
                     * preceded this box, so it must not be underlined or
                     * clickable as part of a link. */
                    struct html_box sb = *b;
                    sb.style &= ~(HS_LINK | HS_UNDER);
                    sb.href = 0;
                    lay_add_run(s, &sb, box, " ", 1, sw, h);
                }
            }
        }
        placed = 1;

        w = s->m->text_w(s->m->ctx, t + start, end - start, b->style,
                         b->heading);
        avail = width - s->x;
        if (w > avail && s->line_runs > 0) {
            /* Drop the trailing space run we may have just placed. */
            if (s->l->nrun > 0 && s->l->runs[s->l->nrun - 1].w > 0 &&
                s->off[s->l->nrun - 1] >= 0 &&
                s->arena[s->off[s->l->nrun - 1]] == ' ' &&
                s->arena[s->off[s->l->nrun - 1] + 1] == 0) {
                s->x -= s->l->runs[s->l->nrun - 1].w;
                s->l->nrun--;
                s->line_runs--;
            }
            lay_endline(s, 0);
            avail = width - s->x;
        }
        while (w > avail && !s->oom) {
            /* Still too wide on an empty line: break inside the word. */
            int take = fit_prefix(s, b, t + start, end - start, avail, &w);
            if (take >= end - start)
                break;
            lay_add_run(s, b, box, t + start, take, w, h);
            start += take;
            lay_endline(s, 0);
            avail = width - s->x;
            w = s->m->text_w(s->m->ctx, t + start, end - start, b->style,
                             b->heading);
        }
        if (end > start)
            lay_add_run(s, b, box, t + start, end - start, w, h);
    }
}

struct html_layout *html_layout_build(const struct html_doc *d, int width,
                                      const struct html_metrics *m)
{
    struct lstate s;
    struct html_layout *l;
    int i;

    if (!d || !m || !m->text_w || !m->line_h)
        return 0;

    l = (struct html_layout *)malloc(sizeof *l);
    if (!l)
        return 0;
    memset(l, 0, sizeof *l);
    memset(&s, 0, sizeof s);
    s.l = l;
    s.m = m;

    if (width < m->indent_w * 2 + 8)
        width = m->indent_w * 2 + 8;
    l->width = width;

    for (i = 0; i < d->nbox && !s.oom; i++) {
        const struct html_box *b = &d->boxes[i];
        int bx = b->indent * m->indent_w;
        int h;

        if (bx > width / 2)
            bx = width / 2;

        switch (b->kind) {
        case HB_BREAK:
            lay_endline(&s, 1);
            s.base = bx;
            s.hang = bx;
            s.x = bx;
            break;
        case HB_BLOCK:
            lay_endline(&s, 0);
            s.y += m->para_gap;
            s.base = bx;
            s.hang = bx;
            s.x = bx;
            break;
        case HB_RULE:
            lay_endline(&s, 0);
            s.base = s.hang = s.x = bx;
            h = m->line_h(m->ctx, 0, 0);
            lay_add_run(&s, b, i, 0, 0, width - bx, h);
            lay_endline(&s, 1);
            break;
        case HB_BULLET: {
            int n, w;
            lay_endline(&s, 0);
            s.base = s.hang = s.x = bx;
            if (!b->text)
                break;
            n = (int)strlen(b->text);
            w = m->text_w(m->ctx, b->text, n, b->style, b->heading);
            h = m->line_h(m->ctx, b->style, b->heading);
            lay_add_run(&s, b, i, b->text, n, w, h);
            s.hang = s.x;    /* wrapped item text lines up under the text */
            break;
        }
        case HB_IMAGE: {
            const char *alt = b->text ? b->text : "image";
            int n = (int)strlen(alt);
            int w = m->text_w(m->ctx, alt, n, b->style, b->heading) +
                    m->text_w(m->ctx, "[]", 2, b->style, b->heading);
            h = m->line_h(m->ctx, b->style, b->heading);
            /* Placeholders always get breathing room from their
             * neighbours; source whitespace around <img> is long gone by
             * the time the box list is built. */
            if (s.line_runs > 0)
                s.x += m->text_w(m->ctx, " ", 1, b->style, b->heading);
            if (s.x + w > width && s.line_runs > 0)
                lay_endline(&s, 0);
            lay_add_run(&s, b, i, alt, n, w, h);
            break;
        }
        case HB_TEXT:
        default:
            if (s.line_runs == 0 && s.x < bx) {
                s.base = s.hang = s.x = bx;
            }
            lay_text(&s, b, i, width);
            break;
        }
    }
    lay_endline(&s, 0);

    if (s.oom) {
        free(s.off);
        free(s.arena);
        free(l->runs);
        free(l->lines);
        free(l);
        return 0;
    }

    /* Offsets become pointers now that the arena has stopped moving. */
    for (i = 0; i < l->nrun; i++)
        l->runs[i].text = s.off[i] >= 0 ? s.arena + s.off[i] : 0;
    free(s.off);
    l->arena = s.arena;
    l->height = s.y;
    return l;
}

void html_layout_free(struct html_layout *l)
{
    if (!l)
        return;
    free(l->runs);
    free(l->lines);
    free(l->arena);
    free(l);
}

int html_layout_hit(const struct html_layout *l, int x, int y)
{
    int i;

    if (!l)
        return -1;
    for (i = 0; i < l->nrun; i++) {
        const struct html_run *r = &l->runs[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
            return i;
    }
    return -1;
}

/* ---- character-cell metrics ---- */

static int chars_w(void *ctx, const char *s, int len, unsigned int style,
                   int heading)
{
    (void)ctx;
    (void)s;
    (void)style;
    (void)heading;
    return len;
}

static int chars_h(void *ctx, unsigned int style, int heading)
{
    (void)ctx;
    (void)style;
    (void)heading;
    return 1;
}

void html_metrics_chars(struct html_metrics *m)
{
    m->text_w = chars_w;
    m->line_h = chars_h;
    m->ctx = 0;
    m->indent_w = 2;
    m->para_gap = 1;
}
