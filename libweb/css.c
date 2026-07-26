/* css.c - CSS tokenizer, selector parser, value parser and rule index.
 *
 * See css.h for the shape of everything here. Three things are worth
 * knowing before reading:
 *
 *   1. All parser output lives in one bump arena that is freed in a
 *      single call, so a stylesheet is never a graph of small mallocs.
 *      brk-backed malloc is real but slow; this touches it a few dozen
 *      times for a whole sheet instead of a hundred thousand.
 *   2. Nothing here allocates during tokenizing. Token text lands in a
 *      fixed CSS_MAX_IDENT buffer inside the lexer; the parser copies
 *      into the arena only the strings it decides to keep.
 *   3. Error recovery is the CSS 2.1 rule and it is not optional: a bad
 *      declaration is skipped to the next ';', a bad selector skips the
 *      whole following block, and an unterminated string or block is
 *      closed at end of input. The stylesheet always survives.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "css.h"

/* ================================================================== *
 * small helpers
 * ================================================================== */

static int lc(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int css_stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        int x = lc((unsigned char)*a), y = lc((unsigned char)*b);
        if (x != y)
            return x - y;
        a++; b++;
    }
    return lc((unsigned char)*a) - lc((unsigned char)*b);
}

static int is_ws(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int is_digit(int c) { return c >= '0' && c <= '9'; }

static int is_hex(int c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hexval(int c)
{
    if (is_digit(c)) return c - '0';
    return (lc(c) - 'a') + 10;
}

static int is_name_start(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           c >= 0x80;
}

static int is_name(int c)
{
    return is_name_start(c) || is_digit(c) || c == '-';
}

static int32_t clamp32(int64_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return (int32_t)v;
}

/* Round a thousandths value to whole pixels, half away from zero. */
static int32_t milli_round(int64_t milli)
{
    if (milli >= 0)
        return clamp32((milli + 500) / 1000, -1000000, 1000000);
    return clamp32(-((-milli + 500) / 1000), -1000000, 1000000);
}

/* ================================================================== *
 * arena
 * ================================================================== */

struct css_chunk {
    struct css_chunk *next;
    unsigned long used, cap;
    /* payload follows */
};

#define CSS_CHUNK_MIN 8192UL

struct css_arena {
    struct css_chunk *head;
    unsigned long total;
    int oom;              /* set once the cap or malloc says no */
};

static void *arena_alloc(struct css_arena *a, unsigned long n)
{
    struct css_chunk *c;
    unsigned long cap;
    char *p;

    n = (n + 7UL) & ~7UL;
    if (n == 0)
        n = 8;
    if (a->oom)
        return 0;
    if (a->total + n > CSS_MAX_ARENA) {
        a->oom = 1;
        return 0;
    }
    c = a->head;
    if (c && c->cap - c->used >= n) {
        p = (char *)(c + 1) + c->used;
        c->used += n;
        a->total += n;
        return p;
    }
    cap = CSS_CHUNK_MIN;
    while (cap < n)
        cap *= 2;
    c = (struct css_chunk *)malloc(sizeof(*c) + cap);
    if (!c) {
        a->oom = 1;
        return 0;
    }
    c->next = a->head;
    c->used = n;
    c->cap = cap;
    a->head = c;
    a->total += n;
    return (char *)(c + 1);
}

static void arena_free(struct css_arena *a)
{
    struct css_chunk *c = a->head, *n;
    while (c) {
        n = c->next;
        free(c);
        c = n;
    }
    a->head = 0;
    a->total = 0;
}

static const char *arena_str(struct css_arena *a, const char *s, unsigned long n)
{
    char *p = (char *)arena_alloc(a, n + 1);
    if (!p)
        return 0;
    if (n)
        memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static const char *arena_str_lower(struct css_arena *a, const char *s,
                                   unsigned long n)
{
    unsigned long i;
    char *p = (char *)arena_alloc(a, n + 1);
    if (!p)
        return 0;
    for (i = 0; i < n; i++)
        p[i] = (char)lc((unsigned char)s[i]);
    p[n] = 0;
    return p;
}

/* ================================================================== *
 * named colours - CSS Color Level 4, 148 names, sorted for bsearch
 * ================================================================== */

struct named_color { const char *name; uint32_t rgb; };

static const struct named_color named_colors[] = {
    {"aliceblue",0xF0F8FF},{"antiquewhite",0xFAEBD7},{"aqua",0x00FFFF},
    {"aquamarine",0x7FFFD4},{"azure",0xF0FFFF},{"beige",0xF5F5DC},
    {"bisque",0xFFE4C4},{"black",0x000000},{"blanchedalmond",0xFFEBCD},
    {"blue",0x0000FF},{"blueviolet",0x8A2BE2},{"brown",0xA52A2A},
    {"burlywood",0xDEB887},{"cadetblue",0x5F9EA0},{"chartreuse",0x7FFF00},
    {"chocolate",0xD2691E},{"coral",0xFF7F50},{"cornflowerblue",0x6495ED},
    {"cornsilk",0xFFF8DC},{"crimson",0xDC143C},{"cyan",0x00FFFF},
    {"darkblue",0x00008B},{"darkcyan",0x008B8B},{"darkgoldenrod",0xB8860B},
    {"darkgray",0xA9A9A9},{"darkgreen",0x006400},{"darkgrey",0xA9A9A9},
    {"darkkhaki",0xBDB76B},{"darkmagenta",0x8B008B},{"darkolivegreen",0x556B2F},
    {"darkorange",0xFF8C00},{"darkorchid",0x9932CC},{"darkred",0x8B0000},
    {"darksalmon",0xE9967A},{"darkseagreen",0x8FBC8F},{"darkslateblue",0x483D8B},
    {"darkslategray",0x2F4F4F},{"darkslategrey",0x2F4F4F},
    {"darkturquoise",0x00CED1},{"darkviolet",0x9400D3},{"deeppink",0xFF1493},
    {"deepskyblue",0x00BFFF},{"dimgray",0x696969},{"dimgrey",0x696969},
    {"dodgerblue",0x1E90FF},{"firebrick",0xB22222},{"floralwhite",0xFFFAF0},
    {"forestgreen",0x228B22},{"fuchsia",0xFF00FF},{"gainsboro",0xDCDCDC},
    {"ghostwhite",0xF8F8FF},{"gold",0xFFD700},{"goldenrod",0xDAA520},
    {"gray",0x808080},{"green",0x008000},{"greenyellow",0xADFF2F},
    {"grey",0x808080},{"honeydew",0xF0FFF0},{"hotpink",0xFF69B4},
    {"indianred",0xCD5C5C},{"indigo",0x4B0082},{"ivory",0xFFFFF0},
    {"khaki",0xF0E68C},{"lavender",0xE6E6FA},{"lavenderblush",0xFFF0F5},
    {"lawngreen",0x7CFC00},{"lemonchiffon",0xFFFACD},{"lightblue",0xADD8E6},
    {"lightcoral",0xF08080},{"lightcyan",0xE0FFFF},
    {"lightgoldenrodyellow",0xFAFAD2},{"lightgray",0xD3D3D3},
    {"lightgreen",0x90EE90},{"lightgrey",0xD3D3D3},{"lightpink",0xFFB6C1},
    {"lightsalmon",0xFFA07A},{"lightseagreen",0x20B2AA},
    {"lightskyblue",0x87CEFA},{"lightslategray",0x778899},
    {"lightslategrey",0x778899},{"lightsteelblue",0xB0C4DE},
    {"lightyellow",0xFFFFE0},{"lime",0x00FF00},{"limegreen",0x32CD32},
    {"linen",0xFAF0E6},{"magenta",0xFF00FF},{"maroon",0x800000},
    {"mediumaquamarine",0x66CDAA},{"mediumblue",0x0000CD},
    {"mediumorchid",0xBA55D3},{"mediumpurple",0x9370DB},
    {"mediumseagreen",0x3CB371},{"mediumslateblue",0x7B68EE},
    {"mediumspringgreen",0x00FA9A},{"mediumturquoise",0x48D1CC},
    {"mediumvioletred",0xC71585},{"midnightblue",0x191970},
    {"mintcream",0xF5FFFA},{"mistyrose",0xFFE4E1},{"moccasin",0xFFE4B5},
    {"navajowhite",0xFFDEAD},{"navy",0x000080},{"oldlace",0xFDF5E6},
    {"olive",0x808000},{"olivedrab",0x6B8E23},{"orange",0xFFA500},
    {"orangered",0xFF4500},{"orchid",0xDA70D6},{"palegoldenrod",0xEEE8AA},
    {"palegreen",0x98FB98},{"paleturquoise",0xAFEEEE},
    {"palevioletred",0xDB7093},{"papayawhip",0xFFEFD5},{"peachpuff",0xFFDAB9},
    {"peru",0xCD853F},{"pink",0xFFC0CB},{"plum",0xDDA0DD},
    {"powderblue",0xB0E0E6},{"purple",0x800080},{"rebeccapurple",0x663399},
    {"red",0xFF0000},{"rosybrown",0xBC8F8F},{"royalblue",0x4169E1},
    {"saddlebrown",0x8B4513},{"salmon",0xFA8072},{"sandybrown",0xF4A460},
    {"seagreen",0x2E8B57},{"seashell",0xFFF5EE},{"sienna",0xA0522D},
    {"silver",0xC0C0C0},{"skyblue",0x87CEEB},{"slateblue",0x6A5ACD},
    {"slategray",0x708090},{"slategrey",0x708090},{"snow",0xFFFAFA},
    {"springgreen",0x00FF7F},{"steelblue",0x4682B4},{"tan",0xD2B48C},
    {"teal",0x008080},{"thistle",0xD8BFD8},{"tomato",0xFF6347},
    {"turquoise",0x40E0D0},{"violet",0xEE82EE},{"wheat",0xF5DEB3},
    {"white",0xFFFFFF},{"whitesmoke",0xF5F5F5},{"yellow",0xFFFF00},
    {"yellowgreen",0x9ACD32}
};
#define NAMED_COLOR_COUNT ((int)(sizeof(named_colors)/sizeof(named_colors[0])))

int css_named_color_count(void) { return NAMED_COLOR_COUNT; }

static int named_color_lookup(const char *name, uint32_t *out)
{
    int lo = 0, hi = NAMED_COLOR_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = css_stricmp(name, named_colors[mid].name);
        if (c == 0) {
            *out = 0xFF000000u | named_colors[mid].rgb;
            return 1;
        }
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return 0;
}

/* HSL -> RGB, integer only. h is milli-degrees, s and l are permille. */
static uint32_t hsl_to_rgb(int64_t h, int64_t s, int64_t l, int alpha)
{
    int64_t c, x, m, hp, k, r, g, b;
    int sect;

    h %= 360000;
    if (h < 0)
        h += 360000;
    if (s < 0) s = 0;
    if (s > 1000) s = 1000;
    if (l < 0) l = 0;
    if (l > 1000) l = 1000;

    c = 2 * l - 1000;
    if (c < 0) c = -c;
    c = (1000 - c) * s / 1000;
    hp = h / 60;                 /* milli-deg / 60 -> 0..6000 */
    k = hp % 2000;
    x = k - 1000;
    if (x < 0) x = -x;
    x = c * (1000 - x) / 1000;
    m = l - c / 2;

    sect = (int)(hp / 1000);
    switch (sect) {
    case 0:  r = c; g = x; b = 0; break;
    case 1:  r = x; g = c; b = 0; break;
    case 2:  r = 0; g = c; b = x; break;
    case 3:  r = 0; g = x; b = c; break;
    case 4:  r = x; g = 0; b = c; break;
    default: r = c; g = 0; b = x; break;
    }
    r = ((r + m) * 255 + 500) / 1000;
    g = ((g + m) * 255 + 500) / 1000;
    b = ((b + m) * 255 + 500) / 1000;
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return ((uint32_t)(alpha & 0xFF) << 24) | ((uint32_t)r << 16) |
           ((uint32_t)g << 8) | (uint32_t)b;
}

static int parse_hex_color(const char *s, unsigned long n, uint32_t *out)
{
    unsigned long i;
    uint32_t v[8];

    if (n != 3 && n != 4 && n != 6 && n != 8)
        return 0;
    for (i = 0; i < n; i++) {
        if (!is_hex((unsigned char)s[i]))
            return 0;
        v[i] = (uint32_t)hexval((unsigned char)s[i]);
    }
    if (n == 3 || n == 4) {
        uint32_t a = (n == 4) ? (v[3] * 17) : 255;
        *out = (a << 24) | (v[0] * 17 << 16) | (v[1] * 17 << 8) | (v[2] * 17);
    } else {
        uint32_t a = (n == 8) ? (v[6] * 16 + v[7]) : 255;
        *out = (a << 24) | ((v[0] * 16 + v[1]) << 16) |
               ((v[2] * 16 + v[3]) << 8) | (v[4] * 16 + v[5]);
    }
    return 1;
}

int css_parse_color_string(const char *s, uint32_t *out)
{
    if (!s || !out)
        return 0;
    while (is_ws((unsigned char)*s))
        s++;
    if (*s == '#')
        return parse_hex_color(s + 1, strlen(s + 1), out);
    if (css_stricmp(s, "transparent") == 0) {
        *out = CSS_TRANSPARENT;
        return 1;
    }
    return named_color_lookup(s, out);
}

/* ================================================================== *
 * tokenizer
 * ================================================================== */

enum {
    T_EOF = 0, T_WS, T_IDENT, T_FUNCTION, T_AT, T_HASH, T_STRING, T_BADSTRING,
    T_URL, T_BADURL, T_NUMBER, T_PERCENT, T_DIMENSION, T_DELIM,
    T_COLON, T_SEMI, T_COMMA, T_LBRACK, T_RBRACK, T_LPAREN, T_RPAREN,
    T_LBRACE, T_RBRACE, T_INCLUDES, T_DASHMATCH, T_PREFIX, T_SUFFIX,
    T_SUBSTR, T_CDO, T_CDC
};

struct css_tok {
    int type;
    int tlen;
    int hash_id;      /* a HASH whose body is a valid identifier */
    int is_int;
    char delim;
    css_num num;
    char text[CSS_MAX_IDENT];
    char unit[32];
};

struct css_lex {
    const char *s;
    unsigned long n, i;
};

static void buf_push(char *buf, int *len, int cap, int c)
{
    if (*len < cap - 1)
        buf[(*len)++] = (char)c;
}

/* Consume the character(s) after a backslash and append the result. */
static void lex_escape(struct css_lex *L, char *buf, int *len, int cap)
{
    uint32_t cp = 0;
    int nd = 0;

    if (L->i >= L->n) {                     /* trailing backslash: U+FFFD */
        buf_push(buf, len, cap, 0xEF);
        buf_push(buf, len, cap, 0xBF);
        buf_push(buf, len, cap, 0xBD);
        return;
    }
    if (!is_hex((unsigned char)L->s[L->i])) {
        unsigned char c = (unsigned char)L->s[L->i++];
        if (c != '\n')
            buf_push(buf, len, cap, c);
        return;
    }
    while (nd < 6 && L->i < L->n && is_hex((unsigned char)L->s[L->i])) {
        cp = cp * 16 + (uint32_t)hexval((unsigned char)L->s[L->i]);
        L->i++;
        nd++;
    }
    if (L->i < L->n && is_ws((unsigned char)L->s[L->i]))
        L->i++;
    if (cp == 0 || cp > 0x10FFFF)
        cp = 0xFFFD;
    if (cp < 0x80) {
        buf_push(buf, len, cap, (int)cp);
    } else if (cp < 0x800) {
        buf_push(buf, len, cap, (int)(0xC0 | (cp >> 6)));
        buf_push(buf, len, cap, (int)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        buf_push(buf, len, cap, (int)(0xE0 | (cp >> 12)));
        buf_push(buf, len, cap, (int)(0x80 | ((cp >> 6) & 0x3F)));
        buf_push(buf, len, cap, (int)(0x80 | (cp & 0x3F)));
    } else {
        buf_push(buf, len, cap, (int)(0xF0 | (cp >> 18)));
        buf_push(buf, len, cap, (int)(0x80 | ((cp >> 12) & 0x3F)));
        buf_push(buf, len, cap, (int)(0x80 | ((cp >> 6) & 0x3F)));
        buf_push(buf, len, cap, (int)(0x80 | (cp & 0x3F)));
    }
}

static void lex_name(struct css_lex *L, char *buf, int *len, int cap)
{
    for (;;) {
        int c;
        if (L->i >= L->n)
            break;
        c = (unsigned char)L->s[L->i];
        if (c == '\\') {
            L->i++;
            lex_escape(L, buf, len, cap);
            continue;
        }
        if (!is_name(c))
            break;
        L->i++;
        buf_push(buf, len, cap, c);
    }
    buf[*len] = 0;
}

/* Is a valid identifier starting at offset k? */
static int starts_ident(struct css_lex *L, unsigned long k)
{
    int c;
    if (k >= L->n)
        return 0;
    c = (unsigned char)L->s[k];
    if (c == '\\')
        return k + 1 >= L->n || L->s[k + 1] != '\n';
    if (c == '-') {
        if (k + 1 >= L->n)
            return 0;
        c = (unsigned char)L->s[k + 1];
        if (c == '-' || c == '\\')
            return 1;
        return is_name_start(c);
    }
    return is_name_start(c);
}

static int starts_number(struct css_lex *L, unsigned long k)
{
    int c;
    if (k >= L->n)
        return 0;
    c = (unsigned char)L->s[k];
    if (is_digit(c))
        return 1;
    if (c == '.')
        return k + 1 < L->n && is_digit((unsigned char)L->s[k + 1]);
    if (c == '+' || c == '-') {
        if (k + 1 >= L->n)
            return 0;
        if (is_digit((unsigned char)L->s[k + 1]))
            return 1;
        return L->s[k + 1] == '.' && k + 2 < L->n &&
               is_digit((unsigned char)L->s[k + 2]);
    }
    return 0;
}

/* Read a number into thousandths. Saturates rather than overflowing. */
static void lex_number(struct css_lex *L, struct css_tok *t)
{
    int64_t ip = 0, fp = 0;
    int sign = 1, fdigits = 0, expsign = 1;
    int64_t expv = 0;
    int had_frac = 0, had_exp = 0;

    if (L->i < L->n && (L->s[L->i] == '+' || L->s[L->i] == '-')) {
        if (L->s[L->i] == '-')
            sign = -1;
        L->i++;
    }
    while (L->i < L->n && is_digit((unsigned char)L->s[L->i])) {
        if (ip < 100000000LL)
            ip = ip * 10 + (L->s[L->i] - '0');
        L->i++;
    }
    if (L->i + 1 < L->n && L->s[L->i] == '.' &&
        is_digit((unsigned char)L->s[L->i + 1])) {
        had_frac = 1;
        L->i++;
        while (L->i < L->n && is_digit((unsigned char)L->s[L->i])) {
            if (fdigits < 3) {
                fp = fp * 10 + (L->s[L->i] - '0');
                fdigits++;
            }
            L->i++;
        }
    }
    if (L->i < L->n && (L->s[L->i] == 'e' || L->s[L->i] == 'E')) {
        unsigned long k = L->i + 1;
        int esign = 1;
        if (k < L->n && (L->s[k] == '+' || L->s[k] == '-')) {
            if (L->s[k] == '-')
                esign = -1;
            k++;
        }
        if (k < L->n && is_digit((unsigned char)L->s[k])) {
            had_exp = 1;
            expsign = esign;
            L->i = k;
            while (L->i < L->n && is_digit((unsigned char)L->s[L->i])) {
                if (expv < 100)
                    expv = expv * 10 + (L->s[L->i] - '0');
                L->i++;
            }
        }
    }
    while (fdigits < 3) {
        fp *= 10;
        fdigits++;
    }
    {
        int64_t v = ip * 1000 + fp;
        if (had_exp) {
            int64_t e = expv;
            while (e-- > 0) {
                if (expsign > 0) {
                    if (v > 2000000000LL / 10) { v = 2000000000LL; break; }
                    v *= 10;
                } else {
                    v /= 10;
                }
            }
        }
        v *= sign;
        t->num = clamp32(v, -2000000000, 2000000000);
    }
    t->is_int = (!had_frac && !had_exp);
}

static void lex_string(struct css_lex *L, struct css_tok *t, int quote)
{
    t->tlen = 0;
    t->type = T_STRING;
    while (L->i < L->n) {
        int c = (unsigned char)L->s[L->i];
        if (c == quote) {
            L->i++;
            t->text[t->tlen] = 0;
            return;
        }
        if (c == '\n') {                 /* unterminated: bad-string */
            t->type = T_BADSTRING;
            t->text[t->tlen] = 0;
            return;
        }
        if (c == '\\') {
            L->i++;
            if (L->i < L->n && L->s[L->i] == '\n') {
                L->i++;                  /* escaped newline: continuation */
                continue;
            }
            lex_escape(L, t->text, &t->tlen, CSS_MAX_IDENT);
            continue;
        }
        L->i++;
        buf_push(t->text, &t->tlen, CSS_MAX_IDENT, c);
    }
    t->type = T_BADSTRING;               /* ran off the end of the input */
    t->text[t->tlen] = 0;
}

static void lex_url(struct css_lex *L, struct css_tok *t)
{
    t->tlen = 0;
    t->type = T_URL;
    while (L->i < L->n && is_ws((unsigned char)L->s[L->i]))
        L->i++;
    while (L->i < L->n) {
        int c = (unsigned char)L->s[L->i];
        if (c == ')') {
            L->i++;
            t->text[t->tlen] = 0;
            return;
        }
        if (is_ws(c)) {
            while (L->i < L->n && is_ws((unsigned char)L->s[L->i]))
                L->i++;
            if (L->i < L->n && L->s[L->i] == ')') {
                L->i++;
                t->text[t->tlen] = 0;
                return;
            }
            t->type = T_BADURL;
            break;
        }
        if (c == '"' || c == '\'' || c == '(') {
            t->type = T_BADURL;
            break;
        }
        if (c == '\\') {
            L->i++;
            lex_escape(L, t->text, &t->tlen, CSS_MAX_IDENT);
            continue;
        }
        L->i++;
        buf_push(t->text, &t->tlen, CSS_MAX_IDENT, c);
    }
    if (t->type == T_BADURL) {           /* swallow to the closing paren */
        while (L->i < L->n && L->s[L->i] != ')')
            L->i++;
        if (L->i < L->n)
            L->i++;
    }
    t->text[t->tlen] = 0;
}

static void lex_next(struct css_lex *L, struct css_tok *t)
{
    int c;
    int saw_ws = 0;

    t->tlen = 0;
    t->text[0] = 0;
    t->unit[0] = 0;
    t->num = 0;
    t->is_int = 0;
    t->hash_id = 0;
    t->delim = 0;

    for (;;) {
        if (L->i < L->n && is_ws((unsigned char)L->s[L->i])) {
            while (L->i < L->n && is_ws((unsigned char)L->s[L->i]))
                L->i++;
            saw_ws = 1;
            continue;
        }
        if (L->i + 1 < L->n && L->s[L->i] == '/' && L->s[L->i + 1] == '*') {
            L->i += 2;
            while (L->i + 1 < L->n &&
                   !(L->s[L->i] == '*' && L->s[L->i + 1] == '/'))
                L->i++;
            L->i = (L->i + 1 < L->n) ? L->i + 2 : L->n;
            continue;
        }
        break;
    }
    if (saw_ws) {
        t->type = T_WS;
        return;
    }
    if (L->i >= L->n) {
        t->type = T_EOF;
        return;
    }
    c = (unsigned char)L->s[L->i];

    if (c == '"' || c == '\'') {
        L->i++;
        lex_string(L, t, c);
        return;
    }
    if (c == '#') {
        if (L->i + 1 < L->n &&
            (is_name((unsigned char)L->s[L->i + 1]) || L->s[L->i + 1] == '\\')) {
            L->i++;
            t->hash_id = starts_ident(L, L->i);
            lex_name(L, t->text, &t->tlen, CSS_MAX_IDENT);
            t->type = T_HASH;
            return;
        }
        L->i++;
        t->type = T_DELIM;
        t->delim = '#';
        return;
    }
    if (starts_number(L, L->i)) {
        lex_number(L, t);
        if (L->i < L->n && L->s[L->i] == '%') {
            L->i++;
            t->type = T_PERCENT;
            return;
        }
        if (starts_ident(L, L->i)) {
            int ul = 0;
            lex_name(L, t->unit, &ul, 32);
            t->type = T_DIMENSION;
            return;
        }
        t->type = T_NUMBER;
        return;
    }
    if (c == '@') {
        if (starts_ident(L, L->i + 1)) {
            L->i++;
            lex_name(L, t->text, &t->tlen, CSS_MAX_IDENT);
            t->type = T_AT;
            return;
        }
        L->i++;
        t->type = T_DELIM;
        t->delim = '@';
        return;
    }
    if (starts_ident(L, L->i)) {
        lex_name(L, t->text, &t->tlen, CSS_MAX_IDENT);
        if (L->i < L->n && L->s[L->i] == '(') {
            L->i++;
            if (css_stricmp(t->text, "url") == 0) {
                unsigned long k = L->i;
                while (k < L->n && is_ws((unsigned char)L->s[k]))
                    k++;
                if (k >= L->n || (L->s[k] != '"' && L->s[k] != '\'')) {
                    lex_url(L, t);
                    return;
                }
            }
            t->type = T_FUNCTION;
            return;
        }
        t->type = T_IDENT;
        return;
    }
    L->i++;
    switch (c) {
    case ':': t->type = T_COLON;  return;
    case ';': t->type = T_SEMI;   return;
    case ',': t->type = T_COMMA;  return;
    case '[': t->type = T_LBRACK; return;
    case ']': t->type = T_RBRACK; return;
    case '(': t->type = T_LPAREN; return;
    case ')': t->type = T_RPAREN; return;
    case '{': t->type = T_LBRACE; return;
    case '}': t->type = T_RBRACE; return;
    default: break;
    }
    if (L->i < L->n && L->s[L->i] == '=') {
        int t2 = 0;
        switch (c) {
        case '~': t2 = T_INCLUDES;   break;
        case '|': t2 = T_DASHMATCH;  break;
        case '^': t2 = T_PREFIX;     break;
        case '$': t2 = T_SUFFIX;     break;
        case '*': t2 = T_SUBSTR;     break;
        default: break;
        }
        if (t2) {
            L->i++;
            t->type = t2;
            return;
        }
    }
    if (c == '<' && L->i + 2 < L->n && L->s[L->i] == '!' &&
        L->s[L->i + 1] == '-' && L->s[L->i + 2] == '-') {
        L->i += 3;
        t->type = T_CDO;
        return;
    }
    if (c == '-' && L->i + 1 < L->n && L->s[L->i] == '-' &&
        L->s[L->i + 1] == '>') {
        L->i += 2;
        t->type = T_CDC;
        return;
    }
    t->type = T_DELIM;
    t->delim = (char)c;
}

/* Append the source text a token stood for. Used to rebuild unquoted
 * attribute selector values, which the tokenizer has already split into
 * several tokens: [href$=.pdf] is DELIM '.' followed by IDENT "pdf". */
static void append_num(css_num v, char *b, int *len, int cap)
{
    char tmp[24];
    int i = 0, j;
    int32_t w, f;
    int neg = v < 0;

    if (neg)
        v = -v;
    w = v / 1000;
    f = v % 1000;
    if (w == 0) {
        tmp[i++] = '0';
    } else {
        char d[16];
        int k = 0;
        while (w > 0 && k < 15) {
            d[k++] = (char)('0' + (w % 10));
            w /= 10;
        }
        while (k > 0)
            tmp[i++] = d[--k];
    }
    if (f) {
        tmp[i++] = '.';
        tmp[i++] = (char)('0' + (f / 100));
        if (f % 100) {
            tmp[i++] = (char)('0' + ((f / 10) % 10));
            if (f % 10)
                tmp[i++] = (char)('0' + (f % 10));
        }
    }
    if (neg)
        buf_push(b, len, cap, '-');
    for (j = 0; j < i; j++)
        buf_push(b, len, cap, tmp[j]);
}

static void append_tok_text(const struct css_tok *t, char *b, int *len, int cap)
{
    int i;

    switch (t->type) {
    case T_IDENT:
    case T_STRING:
        for (i = 0; i < t->tlen; i++)
            buf_push(b, len, cap, (unsigned char)t->text[i]);
        break;
    case T_HASH:
        buf_push(b, len, cap, '#');
        for (i = 0; i < t->tlen; i++)
            buf_push(b, len, cap, (unsigned char)t->text[i]);
        break;
    case T_FUNCTION:
        for (i = 0; i < t->tlen; i++)
            buf_push(b, len, cap, (unsigned char)t->text[i]);
        buf_push(b, len, cap, '(');
        break;
    case T_NUMBER:
        append_num(t->num, b, len, cap);
        break;
    case T_PERCENT:
        append_num(t->num, b, len, cap);
        buf_push(b, len, cap, '%');
        break;
    case T_DIMENSION:
        append_num(t->num, b, len, cap);
        for (i = 0; t->unit[i]; i++)
            buf_push(b, len, cap, (unsigned char)t->unit[i]);
        break;
    case T_DELIM:  buf_push(b, len, cap, (unsigned char)t->delim); break;
    case T_COLON:  buf_push(b, len, cap, ':'); break;
    case T_SEMI:   buf_push(b, len, cap, ';'); break;
    case T_COMMA:  buf_push(b, len, cap, ','); break;
    case T_LPAREN: buf_push(b, len, cap, '('); break;
    case T_RPAREN: buf_push(b, len, cap, ')'); break;
    default: break;
    }
}

/* ================================================================== *
 * property registry
 * ================================================================== */

#define CSS_STR_X(n, s) s,

static const char *const kw_display[]     = { CSS_DISPLAY_LIST(CSS_STR_X) 0 };
static const char *const kw_position[]    = { CSS_POSITION_LIST(CSS_STR_X) 0 };
static const char *const kw_float[]       = { CSS_FLOAT_LIST(CSS_STR_X) 0 };
static const char *const kw_clear[]       = { CSS_CLEAR_LIST(CSS_STR_X) 0 };
static const char *const kw_textalign[]   = { CSS_TEXTALIGN_LIST(CSS_STR_X) 0 };
static const char *const kw_valign[]      = { CSS_VALIGN_LIST(CSS_STR_X) 0 };
static const char *const kw_whitespace[]  = { CSS_WHITESPACE_LIST(CSS_STR_X) 0 };
static const char *const kw_overflow[]    = { CSS_OVERFLOW_LIST(CSS_STR_X) 0 };
static const char *const kw_visibility[]  = { CSS_VISIBILITY_LIST(CSS_STR_X) 0 };
static const char *const kw_fontstyle[]   = { CSS_FONTSTYLE_LIST(CSS_STR_X) 0 };
static const char *const kw_fontfamily[]  = { CSS_FONTFAMILY_LIST(CSS_STR_X) 0 };
static const char *const kw_borderstyle[] = { CSS_BORDERSTYLE_LIST(CSS_STR_X) 0 };
static const char *const kw_liststyle[]   = { CSS_LISTSTYLE_LIST(CSS_STR_X) 0 };
static const char *const kw_listpos[]     = { CSS_LISTPOS_LIST(CSS_STR_X) 0 };
static const char *const kw_bcollapse[]   = { CSS_BORDERCOLLAPSE_LIST(CSS_STR_X) 0 };
static const char *const kw_ttransform[]  = { CSS_TEXTTRANSFORM_LIST(CSS_STR_X) 0 };
static const char *const kw_fontsize[]    = { CSS_FONTSIZEKW_LIST(CSS_STR_X) 0 };
static const char *const kw_fontweight[]  = { CSS_FONTWEIGHTKW_LIST(CSS_STR_X) 0 };
static const char *const kw_borderwidth[] = { "thin", "medium", "thick", 0 };
static const char *const kw_none_only[]   = { "none", 0 };
static const char *const kw_decor[]       = { "none", "underline", "overline",
                                              "line-through", "blink", 0 };

static const char *const prop_names[] = { CSS_PROP_LIST(CSS_STR_X) 0 };

#define PA_LEN   0x01u
#define PA_PCT   0x02u
#define PA_AUTO  0x04u
#define PA_COLOR 0x08u
#define PA_NUM   0x10u
#define PA_URL   0x20u
#define PA_NEG   0x40u

struct prop_info {
    uint8_t inherited;
    uint8_t accepts;
    const char *const *kw;
};

static const struct prop_info props[CSS_PROP_COUNT] = {
    [CSS_PROP_DISPLAY]             = { 0, 0, kw_display },
    [CSS_PROP_POSITION]            = { 0, 0, kw_position },
    [CSS_PROP_FLOAT]               = { 0, 0, kw_float },
    [CSS_PROP_CLEAR]               = { 0, 0, kw_clear },
    [CSS_PROP_WIDTH]               = { 0, PA_LEN|PA_PCT|PA_AUTO, 0 },
    [CSS_PROP_HEIGHT]              = { 0, PA_LEN|PA_PCT|PA_AUTO, 0 },
    [CSS_PROP_MIN_WIDTH]           = { 0, PA_LEN|PA_PCT|PA_AUTO, 0 },
    [CSS_PROP_MAX_WIDTH]           = { 0, PA_LEN|PA_PCT, kw_none_only },
    [CSS_PROP_MIN_HEIGHT]          = { 0, PA_LEN|PA_PCT|PA_AUTO, 0 },
    [CSS_PROP_MAX_HEIGHT]          = { 0, PA_LEN|PA_PCT, kw_none_only },
    [CSS_PROP_TOP]                 = { 0, PA_LEN|PA_PCT|PA_AUTO|PA_NEG, 0 },
    [CSS_PROP_RIGHT]               = { 0, PA_LEN|PA_PCT|PA_AUTO|PA_NEG, 0 },
    [CSS_PROP_BOTTOM]              = { 0, PA_LEN|PA_PCT|PA_AUTO|PA_NEG, 0 },
    [CSS_PROP_LEFT]                = { 0, PA_LEN|PA_PCT|PA_AUTO|PA_NEG, 0 },
    [CSS_PROP_MARGIN_TOP]          = { 0, PA_LEN|PA_PCT|PA_AUTO|PA_NEG, 0 },
    [CSS_PROP_MARGIN_RIGHT]        = { 0, PA_LEN|PA_PCT|PA_AUTO|PA_NEG, 0 },
    [CSS_PROP_MARGIN_BOTTOM]       = { 0, PA_LEN|PA_PCT|PA_AUTO|PA_NEG, 0 },
    [CSS_PROP_MARGIN_LEFT]         = { 0, PA_LEN|PA_PCT|PA_AUTO|PA_NEG, 0 },
    [CSS_PROP_PADDING_TOP]         = { 0, PA_LEN|PA_PCT, 0 },
    [CSS_PROP_PADDING_RIGHT]       = { 0, PA_LEN|PA_PCT, 0 },
    [CSS_PROP_PADDING_BOTTOM]      = { 0, PA_LEN|PA_PCT, 0 },
    [CSS_PROP_PADDING_LEFT]        = { 0, PA_LEN|PA_PCT, 0 },
    [CSS_PROP_BORDER_TOP_WIDTH]    = { 0, PA_LEN, kw_borderwidth },
    [CSS_PROP_BORDER_RIGHT_WIDTH]  = { 0, PA_LEN, kw_borderwidth },
    [CSS_PROP_BORDER_BOTTOM_WIDTH] = { 0, PA_LEN, kw_borderwidth },
    [CSS_PROP_BORDER_LEFT_WIDTH]   = { 0, PA_LEN, kw_borderwidth },
    [CSS_PROP_BORDER_TOP_STYLE]    = { 0, 0, kw_borderstyle },
    [CSS_PROP_BORDER_RIGHT_STYLE]  = { 0, 0, kw_borderstyle },
    [CSS_PROP_BORDER_BOTTOM_STYLE] = { 0, 0, kw_borderstyle },
    [CSS_PROP_BORDER_LEFT_STYLE]   = { 0, 0, kw_borderstyle },
    [CSS_PROP_BORDER_TOP_COLOR]    = { 0, PA_COLOR, 0 },
    [CSS_PROP_BORDER_RIGHT_COLOR]  = { 0, PA_COLOR, 0 },
    [CSS_PROP_BORDER_BOTTOM_COLOR] = { 0, PA_COLOR, 0 },
    [CSS_PROP_BORDER_LEFT_COLOR]   = { 0, PA_COLOR, 0 },
    [CSS_PROP_FONT_FAMILY]         = { 1, 0, kw_fontfamily },
    [CSS_PROP_FONT_SIZE]           = { 1, PA_LEN|PA_PCT, kw_fontsize },
    [CSS_PROP_FONT_WEIGHT]         = { 1, PA_NUM, kw_fontweight },
    [CSS_PROP_FONT_STYLE]          = { 1, 0, kw_fontstyle },
    [CSS_PROP_COLOR]               = { 1, PA_COLOR, 0 },
    [CSS_PROP_BACKGROUND_COLOR]    = { 0, PA_COLOR, 0 },
    [CSS_PROP_BACKGROUND_IMAGE]    = { 0, PA_URL, kw_none_only },
    [CSS_PROP_TEXT_ALIGN]          = { 1, 0, kw_textalign },
    [CSS_PROP_TEXT_DECORATION]     = { 0, 0, kw_decor },
    [CSS_PROP_TEXT_INDENT]         = { 1, PA_LEN|PA_PCT|PA_NEG, 0 },
    [CSS_PROP_TEXT_TRANSFORM]      = { 1, 0, kw_ttransform },
    [CSS_PROP_VERTICAL_ALIGN]      = { 0, PA_LEN|PA_PCT|PA_NEG, kw_valign },
    [CSS_PROP_LINE_HEIGHT]         = { 1, PA_LEN|PA_PCT|PA_NUM, 0 },
    [CSS_PROP_LIST_STYLE_TYPE]     = { 1, 0, kw_liststyle },
    [CSS_PROP_LIST_STYLE_POSITION] = { 1, 0, kw_listpos },
    [CSS_PROP_WHITE_SPACE]         = { 1, 0, kw_whitespace },
    [CSS_PROP_OVERFLOW]            = { 0, 0, kw_overflow },
    [CSS_PROP_VISIBILITY]          = { 1, 0, kw_visibility },
    [CSS_PROP_BORDER_COLLAPSE]     = { 1, 0, kw_bcollapse },
    [CSS_PROP_BORDER_SPACING]      = { 1, PA_LEN, 0 },
    [CSS_PROP_Z_INDEX]             = { 0, PA_NUM|PA_AUTO|PA_NEG, 0 }
};

const char *css_property_name(int prop)
{
    if (prop <= 0 || prop >= CSS_PROP_COUNT)
        return "";
    return prop_names[prop];
}

int css_property_id(const char *name)
{
    int i;
    if (!name)
        return CSS_PROP_NONE;
    for (i = 1; i < CSS_PROP_COUNT; i++)
        if (css_stricmp(name, prop_names[i]) == 0)
            return i;
    return CSS_PROP_NONE;
}

int css_property_inherited(int prop)
{
    if (prop <= 0 || prop >= CSS_PROP_COUNT)
        return 0;
    return props[prop].inherited;
}

static int kw_index(const char *const *table, const char *s)
{
    int i;
    if (!table)
        return -1;
    for (i = 0; table[i]; i++)
        if (css_stricmp(table[i], s) == 0)
            return i;
    return -1;
}

/* ================================================================== *
 * media queries
 * ================================================================== */

enum { MT_ALL = 0, MT_SCREEN, MT_PRINT, MT_OTHER };
enum { MF_UNKNOWN = 0, MF_WIDTH, MF_HEIGHT, MF_ORIENTATION, MF_RESOLUTION,
       MF_COLOR, MF_MONOCHROME };
enum { MOP_EQ = 0, MOP_MIN, MOP_MAX };

struct media_feat {
    uint8_t feature, op, unit;
    int32_t value;      /* px for lengths, 0/1 for orientation-landscape */
};

struct media_query {
    uint8_t negated, type, nfeat, only;
    struct media_feat f[CSS_MAX_MEDIA_FEATS];
};

#define CSS_MAX_MEDIA_Q 8

struct media_block {
    int16_t parent;
    uint8_t matches;
    uint8_t nq;
    struct media_query q[CSS_MAX_MEDIA_Q];
};

/* ================================================================== *
 * rule index
 * ================================================================== */

#define IDX_BUCKETS 512

struct idx_node {
    struct idx_node *next;
    const char *key;
    int *rules;
    int n, cap;
};

struct idx_map {
    struct idx_node *b[IDX_BUCKETS];
};

enum { KEY_UNIVERSAL = 0, KEY_TAG, KEY_CLASS, KEY_ID };

struct rule_key {
    uint8_t kind;
    const char *name;
};

/* ================================================================== *
 * stylesheet
 * ================================================================== */

struct css_stylesheet {
    struct css_arena arena;
    struct css_rule *rules;
    int nrule, crule;
    struct media_block **media;
    int nmedia, cmedia;
    const char *imports[CSS_MAX_IMPORTS];
    int nimport;
    int truncated;
    int origin;
    struct css_media env;
    struct rule_key *keys;         /* one per rule */
    struct idx_map id_map, class_map, tag_map;
    int *universal;
    int nuniv;
};

/* ================================================================== *
 * parser state
 * ================================================================== */

#define VAL_COMMA 100
#define VAL_SLASH 101
#define VAL_BAD   102

struct css_parser {
    struct css_lex lex;
    struct css_tok tok;
    struct css_stylesheet *ss;
    int media_stack[8];
    int media_depth;
    struct css_simple simples[CSS_MAX_COMPOUNDS * CSS_MAX_SIMPLES];
    struct css_compound compounds[CSS_MAX_COMPOUNDS];
    struct css_decl block[CSS_MAX_BLOCK_DECLS];
    struct css_selector sels[CSS_MAX_SEL_LIST];
    int nblock;
    struct css_value comp[CSS_MAX_COMPONENTS];
    int ncomp;
};

static void nx(struct css_parser *p) { lex_next(&p->lex, &p->tok); }

static void skip_ws(struct css_parser *p)
{
    while (p->tok.type == T_WS)
        nx(p);
}

/* Consume tokens up to and including the matching '}' of a block that has
 * already had its opening brace consumed. Also stops at end of input. */
static void skip_block(struct css_parser *p)
{
    int depth = 1;
    while (p->tok.type != T_EOF) {
        if (p->tok.type == T_LBRACE || p->tok.type == T_LPAREN ||
            p->tok.type == T_LBRACK || p->tok.type == T_FUNCTION) {
            if (depth < CSS_MAX_NEST)
                depth++;
        } else if (p->tok.type == T_RBRACE || p->tok.type == T_RPAREN ||
                   p->tok.type == T_RBRACK) {
            depth--;
            if (depth <= 0) {
                nx(p);
                return;
            }
        }
        nx(p);
    }
}

/* Skip forward to the start of the next declaration or the end of the
 * current block. Leaves the ';' consumed, the '}' not. */
static void skip_declaration(struct css_parser *p)
{
    int depth = 0;
    while (p->tok.type != T_EOF) {
        if (depth == 0 && p->tok.type == T_SEMI) {
            nx(p);
            return;
        }
        if (depth == 0 && p->tok.type == T_RBRACE)
            return;
        if (p->tok.type == T_LBRACE || p->tok.type == T_LPAREN ||
            p->tok.type == T_LBRACK || p->tok.type == T_FUNCTION) {
            if (depth < CSS_MAX_NEST)
                depth++;
        } else if (p->tok.type == T_RBRACE || p->tok.type == T_RPAREN ||
                   p->tok.type == T_RBRACK) {
            if (depth > 0)
                depth--;
        }
        nx(p);
    }
}

/* ================================================================== *
 * units
 * ================================================================== */

static int unit_id(const char *u)
{
    if (css_stricmp(u, "px") == 0)  return CSS_UNIT_PX;
    if (css_stricmp(u, "em") == 0)  return CSS_UNIT_EM;
    if (css_stricmp(u, "rem") == 0) return CSS_UNIT_REM;
    if (css_stricmp(u, "ex") == 0)  return CSS_UNIT_EX;
    if (css_stricmp(u, "ch") == 0)  return CSS_UNIT_CH;
    if (css_stricmp(u, "pt") == 0)  return CSS_UNIT_PT;
    if (css_stricmp(u, "pc") == 0)  return CSS_UNIT_PC;
    if (css_stricmp(u, "cm") == 0)  return CSS_UNIT_CM;
    if (css_stricmp(u, "mm") == 0)  return CSS_UNIT_MM;
    if (css_stricmp(u, "in") == 0)  return CSS_UNIT_IN;
    if (css_stricmp(u, "vw") == 0)  return CSS_UNIT_VW;
    if (css_stricmp(u, "vh") == 0)  return CSS_UNIT_VH;
    return CSS_UNIT_NONE;
}

/* Absolute units only. Returns 1 and writes thousandths-of-a-pixel. */
static int unit_to_px_milli(css_num v, int unit, int64_t *out)
{
    int64_t x = v;
    switch (unit) {
    case CSS_UNIT_PX: *out = x;                 return 1;
    case CSS_UNIT_PT: *out = x * 4 / 3;         return 1;  /* 96/72 */
    case CSS_UNIT_PC: *out = x * 16;            return 1;
    case CSS_UNIT_IN: *out = x * 96;            return 1;
    case CSS_UNIT_CM: *out = x * 9600 / 254;    return 1;
    case CSS_UNIT_MM: *out = x * 960 / 254;     return 1;
    default: return 0;
    }
}

/* ================================================================== *
 * component values
 * ================================================================== */

static int color_function(struct css_parser *p, const char *fname,
                          struct css_value *out);

/* Read one component value. Returns 0 at the end of the declaration. */
static int read_component(struct css_parser *p, struct css_value *v)
{
    memset(v, 0, sizeof(*v));
    switch (p->tok.type) {
    case T_IDENT:
        if (css_stricmp(p->tok.text, "inherit") == 0) {
            v->type = CSS_VAL_INHERIT;
        } else if (css_stricmp(p->tok.text, "initial") == 0) {
            v->type = CSS_VAL_INITIAL;
        } else if (css_stricmp(p->tok.text, "unset") == 0) {
            v->type = CSS_VAL_UNSET;
        } else {
            v->type = CSS_VAL_IDENT;
            v->str = arena_str(&p->ss->arena, p->tok.text,
                               (unsigned long)p->tok.tlen);
        }
        nx(p);
        return 1;
    case T_STRING:
        v->type = CSS_VAL_STRING;
        v->str = arena_str(&p->ss->arena, p->tok.text,
                           (unsigned long)p->tok.tlen);
        nx(p);
        return 1;
    case T_URL:
        v->type = CSS_VAL_URL;
        v->str = arena_str(&p->ss->arena, p->tok.text,
                           (unsigned long)p->tok.tlen);
        nx(p);
        return 1;
    case T_NUMBER:
        v->type = CSS_VAL_NUMBER;
        v->num = p->tok.num;
        v->kw = (int16_t)p->tok.is_int;
        nx(p);
        return 1;
    case T_PERCENT:
        v->type = CSS_VAL_PERCENT;
        v->num = p->tok.num;
        nx(p);
        return 1;
    case T_DIMENSION: {
        int u = unit_id(p->tok.unit);
        if (u == CSS_UNIT_NONE) {
            v->type = VAL_BAD;
        } else {
            v->type = CSS_VAL_LENGTH;
            v->unit = (uint8_t)u;
            v->num = p->tok.num;
        }
        nx(p);
        return 1;
    }
    case T_HASH: {
        uint32_t c;
        if (parse_hex_color(p->tok.text, (unsigned long)p->tok.tlen, &c)) {
            v->type = CSS_VAL_COLOR;
            v->color = c;
        } else {
            v->type = VAL_BAD;
        }
        nx(p);
        return 1;
    }
    case T_FUNCTION: {
        char fname[32];
        int i;
        for (i = 0; i < 31 && p->tok.text[i]; i++)
            fname[i] = p->tok.text[i];
        fname[i] = 0;
        nx(p);
        /* url("x") lexes as a function; url(x) already lexed as a URL
         * token in the tokenizer, because the two forms have different
         * escaping rules. */
        if (css_stricmp(fname, "url") == 0) {
            skip_ws(p);
            if (p->tok.type == T_STRING) {
                v->type = CSS_VAL_URL;
                v->str = arena_str(&p->ss->arena, p->tok.text,
                                   (unsigned long)p->tok.tlen);
                nx(p);
                skip_ws(p);
                if (p->tok.type == T_RPAREN)
                    nx(p);
                return 1;
            }
        }
        if (!color_function(p, fname, v)) {
            /* Unknown function: swallow it balanced and poison the value. */
            int depth = 1;
            while (p->tok.type != T_EOF && depth > 0) {
                if (p->tok.type == T_LPAREN || p->tok.type == T_FUNCTION)
                    depth++;
                else if (p->tok.type == T_RPAREN)
                    depth--;
                nx(p);
            }
            v->type = VAL_BAD;
        }
        return 1;
    }
    case T_COMMA:
        v->type = VAL_COMMA;
        nx(p);
        return 1;
    case T_DELIM:
        if (p->tok.delim == '/') {
            v->type = VAL_SLASH;
            nx(p);
            return 1;
        }
        v->type = VAL_BAD;
        nx(p);
        return 1;
    case T_BADSTRING:
    case T_BADURL:
        v->type = VAL_BAD;
        nx(p);
        return 1;
    default:
        break;
    }
    return 0;
}

/* rgb()/rgba()/hsl()/hsla(). The opening paren is already consumed. */
static int color_function(struct css_parser *p, const char *fname,
                          struct css_value *out)
{
    int is_hsl, i, n = 0;
    int64_t arg[4];
    int argpct[4];
    int alpha = 255;

    if (css_stricmp(fname, "rgb") == 0 || css_stricmp(fname, "rgba") == 0)
        is_hsl = 0;
    else if (css_stricmp(fname, "hsl") == 0 || css_stricmp(fname, "hsla") == 0)
        is_hsl = 1;
    else
        return 0;

    for (;;) {
        skip_ws(p);
        if (p->tok.type == T_RPAREN) {
            nx(p);
            break;
        }
        if (p->tok.type == T_COMMA ||
            (p->tok.type == T_DELIM && p->tok.delim == '/')) {
            nx(p);
            continue;
        }
        if (p->tok.type == T_NUMBER || p->tok.type == T_PERCENT) {
            if (n < 4) {
                arg[n] = p->tok.num;
                argpct[n] = (p->tok.type == T_PERCENT);
                n++;
            }
            nx(p);
            continue;
        }
        if (p->tok.type == T_IDENT && css_stricmp(p->tok.text, "none") == 0) {
            if (n < 4) { arg[n] = 0; argpct[n] = 0; n++; }
            nx(p);
            continue;
        }
        if (p->tok.type == T_EOF)
            return 0;
        /* anything else inside a colour function makes it invalid */
        while (p->tok.type != T_EOF && p->tok.type != T_RPAREN)
            nx(p);
        if (p->tok.type == T_RPAREN)
            nx(p);
        return 0;
    }
    if (n < 3)
        return 0;
    if (n >= 4) {
        int64_t a = arg[3];
        if (argpct[3])
            a = a * 1000 / 100000;      /* pct thousandths -> 0..1000 */
        alpha = (int)(a * 255 / 1000);
        if (alpha < 0) alpha = 0;
        if (alpha > 255) alpha = 255;
    }
    if (is_hsl) {
        int64_t h = arg[0];             /* thousandths of a degree */
        int64_t s = argpct[1] ? arg[1] / 100 : arg[1];
        int64_t l = argpct[2] ? arg[2] / 100 : arg[2];
        out->type = CSS_VAL_COLOR;
        out->color = hsl_to_rgb(h, s, l, alpha);
        return 1;
    }
    for (i = 0; i < 3; i++) {
        /* Rounded, not truncated: 50% must be 128, the value every other
         * engine produces, not 127. */
        int64_t c = argpct[i] ? ((arg[i] * 255 + 50000) / 100000)
                              : ((arg[i] + 500) / 1000);
        if (c < 0) c = 0;
        if (c > 255) c = 255;
        arg[i] = c;
    }
    out->type = CSS_VAL_COLOR;
    out->color = ((uint32_t)alpha << 24) | ((uint32_t)arg[0] << 16) |
                 ((uint32_t)arg[1] << 8) | (uint32_t)arg[2];
    return 1;
}

/* Read the value of a declaration into p->comp. Returns 0 if any part of
 * it was malformed, in which case the declaration must be dropped. */
static int read_value(struct css_parser *p, int *important)
{
    int ok = 1;

    p->ncomp = 0;
    *important = 0;
    for (;;) {
        skip_ws(p);
        if (p->tok.type == T_SEMI || p->tok.type == T_RBRACE ||
            p->tok.type == T_EOF)
            break;
        if (p->tok.type == T_DELIM && p->tok.delim == '!') {
            nx(p);
            skip_ws(p);
            if (p->tok.type == T_IDENT &&
                css_stricmp(p->tok.text, "important") == 0) {
                *important = 1;
                nx(p);
            } else {
                ok = 0;
            }
            continue;
        }
        if (p->ncomp >= CSS_MAX_COMPONENTS) {
            ok = 0;
            /* keep consuming so recovery lands in the right place */
            nx(p);
            continue;
        }
        if (!read_component(p, &p->comp[p->ncomp]))
            break;
        if (p->comp[p->ncomp].type == VAL_BAD)
            ok = 0;
        if (p->ss->arena.oom)
            ok = 0;
        p->ncomp++;
    }
    if (p->ncomp == 0)
        ok = 0;
    return ok;
}

/* ================================================================== *
 * turning components into longhand declarations
 * ================================================================== */

static void emit(struct css_parser *p, int prop, const struct css_value *v,
                 int important)
{
    if (p->nblock >= CSS_MAX_BLOCK_DECLS) {
        p->ss->truncated = 1;
        return;
    }
    p->block[p->nblock].prop = (uint16_t)prop;
    p->block[p->nblock].important = (uint8_t)important;
    p->block[p->nblock].pad = 0;
    p->block[p->nblock].val = *v;
    p->nblock++;
}

static void emit_kw(struct css_parser *p, int prop, int kw, int important)
{
    struct css_value v;
    memset(&v, 0, sizeof(v));
    v.type = CSS_VAL_KEYWORD;
    v.kw = (int16_t)kw;
    emit(p, prop, &v, important);
}

static void emit_len(struct css_parser *p, int prop, css_num num, int unit,
                     int important)
{
    struct css_value v;
    memset(&v, 0, sizeof(v));
    v.type = CSS_VAL_LENGTH;
    v.num = num;
    v.unit = (uint8_t)unit;
    emit(p, prop, &v, important);
}

static void emit_color(struct css_parser *p, int prop, uint32_t c,
                       int important)
{
    struct css_value v;
    memset(&v, 0, sizeof(v));
    v.type = CSS_VAL_COLOR;
    v.color = c;
    emit(p, prop, &v, important);
}

/* Does this component satisfy the generic accepts mask of `prop`? On
 * success `out` is the value to store. */
static int coerce(int prop, const struct css_value *in, struct css_value *out)
{
    const struct prop_info *pi = &props[prop];
    uint32_t acc = pi->accepts;

    *out = *in;
    if (in->type == CSS_VAL_INHERIT || in->type == CSS_VAL_INITIAL ||
        in->type == CSS_VAL_UNSET)
        return 1;
    if (in->type == CSS_VAL_IDENT) {
        int k;
        if ((acc & PA_AUTO) && css_stricmp(in->str, "auto") == 0) {
            out->type = CSS_VAL_AUTO;
            return 1;
        }
        k = kw_index(pi->kw, in->str);
        if (k >= 0) {
            out->type = CSS_VAL_KEYWORD;
            out->kw = (int16_t)k;
            return 1;
        }
        if (acc & PA_COLOR) {
            uint32_t c;
            if (css_stricmp(in->str, "transparent") == 0) {
                out->type = CSS_VAL_COLOR;
                out->color = CSS_TRANSPARENT;
                return 1;
            }
            if (css_stricmp(in->str, "currentcolor") == 0) {
                out->type = CSS_VAL_IDENT;
                out->str = in->str;
                return 1;
            }
            if (named_color_lookup(in->str, &c)) {
                out->type = CSS_VAL_COLOR;
                out->color = c;
                return 1;
            }
        }
        if ((acc & PA_URL) && css_stricmp(in->str, "none") == 0) {
            out->type = CSS_VAL_KEYWORD;
            out->kw = 0;
            return 1;
        }
        return 0;
    }
    if (in->type == CSS_VAL_COLOR)
        return (acc & PA_COLOR) ? 1 : 0;
    if (in->type == CSS_VAL_URL)
        return (acc & PA_URL) ? 1 : 0;
    if (in->type == CSS_VAL_PERCENT) {
        if (!(acc & PA_PCT))
            return 0;
        if (in->num < 0 && !(acc & PA_NEG))
            return 0;
        return 1;
    }
    if (in->type == CSS_VAL_LENGTH) {
        if (!(acc & PA_LEN))
            return 0;
        if (in->num < 0 && !(acc & PA_NEG))
            return 0;
        return 1;
    }
    if (in->type == CSS_VAL_NUMBER) {
        if (in->num == 0 && (acc & PA_LEN)) {   /* bare 0 is a valid length */
            out->type = CSS_VAL_LENGTH;
            out->unit = CSS_UNIT_PX;
            out->num = 0;
            return 1;
        }
        if (!(acc & PA_NUM))
            return 0;
        if (in->num < 0 && !(acc & PA_NEG))
            return 0;
        return 1;
    }
    return 0;
}

/* --- shorthand expanders --- */

static const int side_margin[4]  = { CSS_PROP_MARGIN_TOP, CSS_PROP_MARGIN_RIGHT,
                                     CSS_PROP_MARGIN_BOTTOM, CSS_PROP_MARGIN_LEFT };
static const int side_padding[4] = { CSS_PROP_PADDING_TOP, CSS_PROP_PADDING_RIGHT,
                                     CSS_PROP_PADDING_BOTTOM, CSS_PROP_PADDING_LEFT };
static const int side_bw[4]      = { CSS_PROP_BORDER_TOP_WIDTH,
                                     CSS_PROP_BORDER_RIGHT_WIDTH,
                                     CSS_PROP_BORDER_BOTTOM_WIDTH,
                                     CSS_PROP_BORDER_LEFT_WIDTH };
static const int side_bs[4]      = { CSS_PROP_BORDER_TOP_STYLE,
                                     CSS_PROP_BORDER_RIGHT_STYLE,
                                     CSS_PROP_BORDER_BOTTOM_STYLE,
                                     CSS_PROP_BORDER_LEFT_STYLE };
static const int side_bc[4]      = { CSS_PROP_BORDER_TOP_COLOR,
                                     CSS_PROP_BORDER_RIGHT_COLOR,
                                     CSS_PROP_BORDER_BOTTOM_COLOR,
                                     CSS_PROP_BORDER_LEFT_COLOR };

/* Expand the 1-to-4 value box shorthand onto the four sides. */
static int expand_box(struct css_parser *p, const int *sides, int important)
{
    struct css_value v[4];
    int i, n = p->ncomp;

    if (n < 1 || n > 4)
        return 0;
    for (i = 0; i < n; i++)
        if (!coerce(sides[0], &p->comp[i], &v[i]))
            return 0;
    if (n == 1) { v[1] = v[0]; v[2] = v[0]; v[3] = v[0]; }
    else if (n == 2) { v[2] = v[0]; v[3] = v[1]; }
    else if (n == 3) { v[3] = v[1]; }
    for (i = 0; i < 4; i++)
        emit(p, sides[i], &v[i], important);
    return 1;
}

/* border / border-top / ... : width || style || colour, in any order. */
static int expand_border(struct css_parser *p, int t, int r, int b, int l,
                         int important)
{
    struct css_value w, st, co, tmp;
    int have_w = 0, have_s = 0, have_c = 0, i;
    const int sides[4] = { t, r, b, l };

    memset(&w, 0, sizeof(w));
    memset(&st, 0, sizeof(st));
    memset(&co, 0, sizeof(co));

    if (p->ncomp == 1 && p->comp[0].type == CSS_VAL_IDENT &&
        css_stricmp(p->comp[0].str, "none") == 0) {
        for (i = 0; i < 4; i++) {
            if (sides[i] < 0) continue;
            emit_kw(p, side_bs[i], CSS_BORDERSTYLE_NONE, important);
            emit_len(p, side_bw[i], 0, CSS_UNIT_PX, important);
        }
        return 1;
    }
    for (i = 0; i < p->ncomp; i++) {
        if (!have_s && coerce(CSS_PROP_BORDER_TOP_STYLE, &p->comp[i], &tmp) &&
            tmp.type == CSS_VAL_KEYWORD) {
            st = tmp; have_s = 1; continue;
        }
        if (!have_w && coerce(CSS_PROP_BORDER_TOP_WIDTH, &p->comp[i], &tmp)) {
            w = tmp; have_w = 1; continue;
        }
        if (!have_c && coerce(CSS_PROP_BORDER_TOP_COLOR, &p->comp[i], &tmp)) {
            co = tmp; have_c = 1; continue;
        }
        return 0;
    }
    if (!have_w && !have_s && !have_c)
        return 0;
    if (!have_w) {
        w.type = CSS_VAL_KEYWORD;   /* medium */
        w.kw = 1;
    }
    if (!have_s) {
        st.type = CSS_VAL_KEYWORD;
        st.kw = CSS_BORDERSTYLE_NONE;
    }
    for (i = 0; i < 4; i++) {
        if (sides[i] < 0)
            continue;
        emit(p, side_bw[i], &w, important);
        emit(p, side_bs[i], &st, important);
        if (have_c)
            emit(p, side_bc[i], &co, important);
    }
    return 1;
}

static int expand_background(struct css_parser *p, int important)
{
    int i, have_color = 0, have_image = 0;
    struct css_value col, img, tmp;

    memset(&col, 0, sizeof(col));
    memset(&img, 0, sizeof(img));
    for (i = 0; i < p->ncomp; i++) {
        struct css_value *c = &p->comp[i];
        if (c->type == CSS_VAL_URL) {
            img = *c; have_image = 1; continue;
        }
        if (!have_color &&
            coerce(CSS_PROP_BACKGROUND_COLOR, c, &tmp) &&
            (tmp.type == CSS_VAL_COLOR || tmp.type == CSS_VAL_IDENT)) {
            col = tmp; have_color = 1; continue;
        }
        if (c->type == CSS_VAL_IDENT) {
            /* repeat / attachment / position keywords are parsed and
             * dropped: the painter cannot honour them anyway. */
            if (css_stricmp(c->str, "none") == 0) {
                img.type = CSS_VAL_KEYWORD;
                img.kw = 0;
                have_image = 1;
            }
            continue;
        }
        if (c->type == CSS_VAL_LENGTH || c->type == CSS_VAL_PERCENT ||
            c->type == VAL_SLASH || c->type == CSS_VAL_NUMBER)
            continue;
        return 0;
    }
    if (!have_color && !have_image)
        return 0;
    if (have_color)
        emit(p, CSS_PROP_BACKGROUND_COLOR, &col, important);
    else
        emit_color(p, CSS_PROP_BACKGROUND_COLOR, CSS_TRANSPARENT, important);
    if (have_image)
        emit(p, CSS_PROP_BACKGROUND_IMAGE, &img, important);
    return 1;
}

static int store_font_family(struct css_parser *p, int first, int important);

/* font: [style || weight]? size [/ line-height]? family */
static int expand_font(struct css_parser *p, int important)
{
    int i = 0, size_at = -1, saw_style = 0, saw_weight = 0;
    struct css_value tmp;

    if (p->ncomp == 1 && p->comp[0].type == CSS_VAL_IDENT) {
        /* the system font keywords: caption, icon, menu, ... - accepted
         * and ignored, which is what a single-face renderer can do. */
        return 1;
    }
    while (i < p->ncomp) {
        struct css_value *c = &p->comp[i];
        if (c->type == CSS_VAL_IDENT) {
            if (!saw_style &&
                kw_index(kw_fontstyle, c->str) > 0) {
                emit_kw(p, CSS_PROP_FONT_STYLE,
                        kw_index(kw_fontstyle, c->str), important);
                saw_style = 1; i++; continue;
            }
            if (!saw_weight && kw_index(kw_fontweight, c->str) >= 0) {
                emit_kw(p, CSS_PROP_FONT_WEIGHT,
                        kw_index(kw_fontweight, c->str), important);
                saw_weight = 1; i++; continue;
            }
            if (css_stricmp(c->str, "normal") == 0 ||
                css_stricmp(c->str, "small-caps") == 0) {
                i++; continue;                 /* variant, ignored */
            }
        }
        if (c->type == CSS_VAL_NUMBER && c->num >= 100000 &&
            c->num <= 900000 && !saw_weight) {
            emit(p, CSS_PROP_FONT_WEIGHT, c, important);
            saw_weight = 1; i++; continue;
        }
        break;
    }
    if (i >= p->ncomp)
        return 0;
    if (!coerce(CSS_PROP_FONT_SIZE, &p->comp[i], &tmp))
        return 0;
    emit(p, CSS_PROP_FONT_SIZE, &tmp, important);
    size_at = i;
    i++;
    if (i < p->ncomp && p->comp[i].type == VAL_SLASH) {
        i++;
        if (i >= p->ncomp)
            return 0;
        if (!coerce(CSS_PROP_LINE_HEIGHT, &p->comp[i], &tmp)) {
            if (p->comp[i].type == CSS_VAL_IDENT &&
                css_stricmp(p->comp[i].str, "normal") == 0) {
                emit_kw(p, CSS_PROP_LINE_HEIGHT, -1, important);
            } else {
                return 0;
            }
        } else {
            emit(p, CSS_PROP_LINE_HEIGHT, &tmp, important);
        }
        i++;
    }
    if (i >= p->ncomp)
        return size_at >= 0;
    return store_font_family(p, i, important);
}

static int expand_list_style(struct css_parser *p, int important)
{
    int i, ok = 0;
    for (i = 0; i < p->ncomp; i++) {
        struct css_value *c = &p->comp[i];
        int k;
        if (c->type == CSS_VAL_URL)
            continue;                      /* list-style-image: ignored */
        if (c->type != CSS_VAL_IDENT)
            return 0;
        k = kw_index(kw_listpos, c->str);
        if (k >= 0) {
            emit_kw(p, CSS_PROP_LIST_STYLE_POSITION, k, important);
            ok = 1;
            continue;
        }
        k = kw_index(kw_liststyle, c->str);
        if (k >= 0) {
            emit_kw(p, CSS_PROP_LIST_STYLE_TYPE, k, important);
            ok = 1;
            continue;
        }
        return 0;
    }
    return ok;
}

/* font-family: keep the first generic class we recognise, plus the very
 * first concrete name for anyone who later grows more faces. */
static int store_font_family(struct css_parser *p, int first, int important)
{
    int i, cls = -1;
    const char *name = 0;

    for (i = first; i < p->ncomp; i++) {
        struct css_value *c = &p->comp[i];
        int k;
        if (c->type == VAL_COMMA)
            continue;
        if (c->type != CSS_VAL_IDENT && c->type != CSS_VAL_STRING)
            return 0;
        if (!name)
            name = c->str;
        k = kw_index(kw_fontfamily, c->str);
        if (k >= 0 && cls < 0)
            cls = k;
        if (cls < 0) {
            /* Common concrete families mapped onto the classes we have. */
            static const struct { const char *n; int c; } fam[] = {
                { "arial",           CSS_FONTFAMILY_SANS },
                { "helvetica",       CSS_FONTFAMILY_SANS },
                { "verdana",         CSS_FONTFAMILY_SANS },
                { "tahoma",          CSS_FONTFAMILY_SANS },
                { "segoe ui",        CSS_FONTFAMILY_SANS },
                { "roboto",          CSS_FONTFAMILY_SANS },
                { "ui-sans-serif",   CSS_FONTFAMILY_SANS },
                { "-apple-system",   CSS_FONTFAMILY_SANS },
                { "times",           CSS_FONTFAMILY_SERIF },
                { "times new roman", CSS_FONTFAMILY_SERIF },
                { "georgia",         CSS_FONTFAMILY_SERIF },
                { "garamond",        CSS_FONTFAMILY_SERIF },
                { "ui-serif",        CSS_FONTFAMILY_SERIF },
                { "courier",         CSS_FONTFAMILY_MONO },
                { "courier new",     CSS_FONTFAMILY_MONO },
                { "consolas",        CSS_FONTFAMILY_MONO },
                { "menlo",           CSS_FONTFAMILY_MONO },
                { "monaco",          CSS_FONTFAMILY_MONO },
                { "ui-monospace",    CSS_FONTFAMILY_MONO }
            };
            unsigned j;
            for (j = 0; j < sizeof(fam) / sizeof(fam[0]); j++)
                if (css_stricmp(c->str, fam[j].n) == 0) {
                    cls = fam[j].c;
                    break;
                }
        }
    }
    if (!name)
        return 0;
    {
        struct css_value v;
        memset(&v, 0, sizeof(v));
        v.type = CSS_VAL_KEYWORD;
        v.kw = (int16_t)(cls < 0 ? CSS_FONTFAMILY_SERIF : cls);
        v.str = name;
        emit(p, CSS_PROP_FONT_FAMILY, &v, important);
    }
    return 1;
}

static int store_text_decoration(struct css_parser *p, int important)
{
    int i, bits = 0;
    for (i = 0; i < p->ncomp; i++) {
        struct css_value *c = &p->comp[i];
        int k;
        if (c->type != CSS_VAL_IDENT)
            return 0;
        k = kw_index(kw_decor, c->str);
        if (k < 0) {
            /* CSS3 adds colour and line style to the shorthand; skip them
             * rather than throwing the whole declaration away. */
            continue;
        }
        if (k > 0)
            bits |= 1 << (k - 1);
    }
    {
        struct css_value v;
        memset(&v, 0, sizeof(v));
        v.type = CSS_VAL_BITS;
        v.kw = (int16_t)bits;
        emit(p, CSS_PROP_TEXT_DECORATION, &v, important);
    }
    return 1;
}

static int store_line_height(struct css_parser *p, int important)
{
    struct css_value *c = &p->comp[0];
    struct css_value v;

    if (p->ncomp != 1)
        return 0;
    memset(&v, 0, sizeof(v));
    if (c->type == CSS_VAL_IDENT && css_stricmp(c->str, "normal") == 0) {
        v.type = CSS_VAL_KEYWORD;
        v.kw = -1;
        emit(p, CSS_PROP_LINE_HEIGHT, &v, important);
        return 1;
    }
    if (c->type == CSS_VAL_NUMBER && c->num >= 0) {
        emit(p, CSS_PROP_LINE_HEIGHT, c, important);
        return 1;
    }
    if ((c->type == CSS_VAL_LENGTH || c->type == CSS_VAL_PERCENT) &&
        c->num >= 0) {
        emit(p, CSS_PROP_LINE_HEIGHT, c, important);
        return 1;
    }
    if (c->type == CSS_VAL_INHERIT || c->type == CSS_VAL_INITIAL ||
        c->type == CSS_VAL_UNSET) {
        emit(p, CSS_PROP_LINE_HEIGHT, c, important);
        return 1;
    }
    return 0;
}

static int store_font_weight(struct css_parser *p, int important)
{
    struct css_value *c = &p->comp[0];
    if (p->ncomp != 1)
        return 0;
    if (c->type == CSS_VAL_NUMBER) {
        if (c->num < 1000 || c->num > 1000000)
            return 0;
        emit(p, CSS_PROP_FONT_WEIGHT, c, important);
        return 1;
    }
    if (c->type == CSS_VAL_IDENT) {
        int k = kw_index(kw_fontweight, c->str);
        if (k < 0)
            return 0;
        emit_kw(p, CSS_PROP_FONT_WEIGHT, k, important);
        return 1;
    }
    if (c->type == CSS_VAL_INHERIT || c->type == CSS_VAL_INITIAL ||
        c->type == CSS_VAL_UNSET) {
        emit(p, CSS_PROP_FONT_WEIGHT, c, important);
        return 1;
    }
    return 0;
}

enum {
    SH_NONE = 0, SH_MARGIN, SH_PADDING, SH_BORDER, SH_BORDER_TOP,
    SH_BORDER_RIGHT, SH_BORDER_BOTTOM, SH_BORDER_LEFT, SH_BORDER_WIDTH,
    SH_BORDER_STYLE, SH_BORDER_COLOR, SH_BACKGROUND, SH_FONT, SH_LIST_STYLE
};

static int shorthand_id(const char *n)
{
    if (css_stricmp(n, "margin") == 0)        return SH_MARGIN;
    if (css_stricmp(n, "padding") == 0)       return SH_PADDING;
    if (css_stricmp(n, "border") == 0)        return SH_BORDER;
    if (css_stricmp(n, "border-top") == 0)    return SH_BORDER_TOP;
    if (css_stricmp(n, "border-right") == 0)  return SH_BORDER_RIGHT;
    if (css_stricmp(n, "border-bottom") == 0) return SH_BORDER_BOTTOM;
    if (css_stricmp(n, "border-left") == 0)   return SH_BORDER_LEFT;
    if (css_stricmp(n, "border-width") == 0)  return SH_BORDER_WIDTH;
    if (css_stricmp(n, "border-style") == 0)  return SH_BORDER_STYLE;
    if (css_stricmp(n, "border-color") == 0)  return SH_BORDER_COLOR;
    if (css_stricmp(n, "background") == 0)    return SH_BACKGROUND;
    if (css_stricmp(n, "font") == 0)          return SH_FONT;
    if (css_stricmp(n, "list-style") == 0)    return SH_LIST_STYLE;
    return SH_NONE;
}

/* Apply one property name + parsed value. Returns 0 if the declaration is
 * invalid and should be discarded. */
static int apply_declaration(struct css_parser *p, const char *name,
                             int important)
{
    int prop, sh, saved = p->nblock;
    struct css_value v;

    sh = shorthand_id(name);
    if (sh != SH_NONE) {
        int ok = 0;
        /* "inherit" and "initial" on a shorthand fan out to every
         * longhand it controls. */
        if (p->ncomp == 1 && (p->comp[0].type == CSS_VAL_INHERIT ||
                              p->comp[0].type == CSS_VAL_UNSET ||
                              p->comp[0].type == CSS_VAL_INITIAL)) {
            const int *set = 0;
            int i;
            switch (sh) {
            case SH_MARGIN:  set = side_margin;  break;
            case SH_PADDING: set = side_padding; break;
            case SH_BORDER_WIDTH: set = side_bw; break;
            case SH_BORDER_STYLE: set = side_bs; break;
            case SH_BORDER_COLOR: set = side_bc; break;
            default: break;
            }
            if (set) {
                for (i = 0; i < 4; i++)
                    emit(p, set[i], &p->comp[0], important);
                return 1;
            }
        }
        switch (sh) {
        case SH_MARGIN:       ok = expand_box(p, side_margin, important); break;
        case SH_PADDING:      ok = expand_box(p, side_padding, important); break;
        case SH_BORDER_WIDTH: ok = expand_box(p, side_bw, important); break;
        case SH_BORDER_STYLE: ok = expand_box(p, side_bs, important); break;
        case SH_BORDER_COLOR: ok = expand_box(p, side_bc, important); break;
        case SH_BORDER:
            ok = expand_border(p, 0, 1, 2, 3, important); break;
        case SH_BORDER_TOP:
            ok = expand_border(p, 0, -1, -1, -1, important); break;
        case SH_BORDER_RIGHT:
            ok = expand_border(p, -1, 1, -1, -1, important); break;
        case SH_BORDER_BOTTOM:
            ok = expand_border(p, -1, -1, 2, -1, important); break;
        case SH_BORDER_LEFT:
            ok = expand_border(p, -1, -1, -1, 3, important); break;
        case SH_BACKGROUND:   ok = expand_background(p, important); break;
        case SH_FONT:         ok = expand_font(p, important); break;
        case SH_LIST_STYLE:   ok = expand_list_style(p, important); break;
        default: break;
        }
        if (!ok)
            p->nblock = saved;
        return ok;
    }

    prop = css_property_id(name);
    if (prop == CSS_PROP_NONE)
        return 0;

    switch (prop) {
    case CSS_PROP_FONT_FAMILY:
        if (p->ncomp == 1 && (p->comp[0].type == CSS_VAL_INHERIT ||
                              p->comp[0].type == CSS_VAL_UNSET ||
                              p->comp[0].type == CSS_VAL_INITIAL)) {
            emit(p, prop, &p->comp[0], important);
            return 1;
        }
        return store_font_family(p, 0, important);
    case CSS_PROP_TEXT_DECORATION:
        if (p->ncomp == 1 && (p->comp[0].type == CSS_VAL_INHERIT ||
                              p->comp[0].type == CSS_VAL_UNSET ||
                              p->comp[0].type == CSS_VAL_INITIAL)) {
            emit(p, prop, &p->comp[0], important);
            return 1;
        }
        return store_text_decoration(p, important);
    case CSS_PROP_LINE_HEIGHT:
        return store_line_height(p, important);
    case CSS_PROP_FONT_WEIGHT:
        return store_font_weight(p, important);
    default:
        break;
    }
    if (p->ncomp != 1)
        return 0;
    if (!coerce(prop, &p->comp[0], &v))
        return 0;
    emit(p, prop, &v, important);
    return 1;
}

/* ================================================================== *
 * declaration block
 * ================================================================== */

static void parse_block(struct css_parser *p)
{
    p->nblock = 0;
    for (;;) {
        skip_ws(p);
        if (p->tok.type == T_RBRACE) {
            nx(p);
            return;
        }
        if (p->tok.type == T_EOF)
            return;
        if (p->tok.type == T_SEMI) {
            nx(p);
            continue;
        }
        if (p->tok.type != T_IDENT) {
            skip_declaration(p);
            continue;
        }
        {
            char name[CSS_MAX_IDENT];
            int important = 0, ok;
            memcpy(name, p->tok.text, (unsigned long)p->tok.tlen + 1);
            nx(p);
            skip_ws(p);
            if (p->tok.type != T_COLON) {
                skip_declaration(p);
                continue;
            }
            nx(p);
            ok = read_value(p, &important);
            if (ok)
                ok = apply_declaration(p, name, important);
            (void)ok;
            if (p->tok.type == T_SEMI)
                nx(p);
        }
    }
}

/* ================================================================== *
 * selectors
 * ================================================================== */

struct sel_build {
    struct css_simple *simples;   /* p->simples */
    int nsimple;
    struct css_compound *parts;   /* p->compounds */
    int nparts;
    int spec_a, spec_b, spec_c;
    int overflowed;
    /* A selector this engine understands but cannot honour - a pseudo
     * element, an unknown pseudo class - only removes itself from the
     * list. An actual syntax error invalidates the whole rule, which is
     * what browsers do and what feature-detection CSS relies on. */
    int unsupported;
};

static struct css_simple *sb_add(struct sel_build *sb)
{
    if (sb->nsimple >= CSS_MAX_COMPOUNDS * CSS_MAX_SIMPLES) {
        sb->overflowed = 1;
        return 0;
    }
    memset(&sb->simples[sb->nsimple], 0, sizeof(struct css_simple));
    return &sb->simples[sb->nsimple++];
}

static int parse_nth(struct css_parser *p, int32_t *a, int32_t *b);

/* Parse one compound selector into sb. Returns 1 on success, 0 on a
 * parse error (the caller then abandons the whole selector). */
static int parse_compound(struct css_parser *p, struct sel_build *sb,
                          int allow_pseudo)
{
    int first = sb->nsimple;
    int count = 0;

    for (;;) {
        struct css_simple *s;
        if (p->tok.type == T_DELIM && p->tok.delim == '*') {
            s = sb_add(sb);
            if (!s) return 0;
            s->kind = CSS_SIMPLE_UNIVERSAL;
            count++;
            nx(p);
            continue;
        }
        if (p->tok.type == T_IDENT) {
            if (count > 0 && sb->nsimple > first) {
                /* "div p" without whitespace is impossible; a bare ident
                 * after another simple ends the compound. */
                break;
            }
            s = sb_add(sb);
            if (!s) return 0;
            s->kind = CSS_SIMPLE_TYPE;
            s->name = arena_str_lower(&p->ss->arena, p->tok.text,
                                      (unsigned long)p->tok.tlen);
            if (!s->name) return 0;
            sb->spec_c++;
            count++;
            nx(p);
            continue;
        }
        if (p->tok.type == T_HASH) {
            s = sb_add(sb);
            if (!s) return 0;
            s->kind = CSS_SIMPLE_ID;
            s->name = arena_str(&p->ss->arena, p->tok.text,
                                (unsigned long)p->tok.tlen);
            if (!s->name) return 0;
            sb->spec_a++;
            count++;
            nx(p);
            continue;
        }
        if (p->tok.type == T_DELIM && p->tok.delim == '.') {
            nx(p);
            if (p->tok.type != T_IDENT)
                return 0;
            s = sb_add(sb);
            if (!s) return 0;
            s->kind = CSS_SIMPLE_CLASS;
            s->name = arena_str(&p->ss->arena, p->tok.text,
                                (unsigned long)p->tok.tlen);
            if (!s->name) return 0;
            sb->spec_b++;
            count++;
            nx(p);
            continue;
        }
        if (p->tok.type == T_LBRACK) {
            nx(p);
            skip_ws(p);
            if (p->tok.type != T_IDENT)
                return 0;
            s = sb_add(sb);
            if (!s) return 0;
            s->kind = CSS_SIMPLE_ATTR;
            s->name = arena_str_lower(&p->ss->arena, p->tok.text,
                                      (unsigned long)p->tok.tlen);
            if (!s->name) return 0;
            sb->spec_b++;
            count++;
            nx(p);
            skip_ws(p);
            if (p->tok.type == T_RBRACK) {
                s->op = CSS_ATTR_EXISTS;
                nx(p);
                continue;
            }
            switch (p->tok.type) {
            case T_DELIM:
                if (p->tok.delim != '=') return 0;
                s->op = CSS_ATTR_EQ; break;
            case T_INCLUDES:  s->op = CSS_ATTR_INCLUDES; break;
            case T_DASHMATCH: s->op = CSS_ATTR_DASH;     break;
            case T_PREFIX:    s->op = CSS_ATTR_PREFIX;   break;
            case T_SUFFIX:    s->op = CSS_ATTR_SUFFIX;   break;
            case T_SUBSTR:    s->op = CSS_ATTR_SUBSTR;   break;
            default: return 0;
            }
            nx(p);
            skip_ws(p);
            if (p->tok.type == T_STRING) {
                s->value = arena_str(&p->ss->arena, p->tok.text,
                                     (unsigned long)p->tok.tlen);
                if (!s->value) return 0;
                nx(p);
            } else {
                /* An unquoted value is strictly a single identifier, but
                 * [href$=.pdf] and [type=text/css] are everywhere, so the
                 * tokens up to the ']' are stitched back together. */
                char vb[CSS_MAX_IDENT];
                int vl = 0;
                while (p->tok.type != T_RBRACK && p->tok.type != T_WS &&
                       p->tok.type != T_EOF && p->tok.type != T_LBRACE)
                    append_tok_text(&p->tok, vb, &vl, CSS_MAX_IDENT), nx(p);
                if (vl == 0)
                    return 0;
                vb[vl] = 0;
                s->value = arena_str(&p->ss->arena, vb, (unsigned long)vl);
                if (!s->value) return 0;
            }
            skip_ws(p);
            /* an optional case-sensitivity flag: [a=v i] */
            if (p->tok.type == T_IDENT) {
                nx(p);
                skip_ws(p);
            }
            if (p->tok.type != T_RBRACK)
                return 0;
            nx(p);
            continue;
        }
        if (p->tok.type == T_COLON) {
            char pname[CSS_MAX_IDENT];
            int is_element = 0, k;
            nx(p);
            if (p->tok.type == T_COLON) {   /* ::before and friends */
                is_element = 1;
                nx(p);
            }
            if (p->tok.type != T_IDENT && p->tok.type != T_FUNCTION)
                return 0;
            memcpy(pname, p->tok.text, (unsigned long)p->tok.tlen + 1);
            if (!allow_pseudo)
                return 0;
            if (p->tok.type == T_FUNCTION) {
                int fn_not = (css_stricmp(pname, "not") == 0);
                int fn_nth = (css_stricmp(pname, "nth-child") == 0);
                int fn_nthl = (css_stricmp(pname, "nth-last-child") == 0);
                nx(p);
                if (fn_not) {
                    struct css_compound *cp;
                    int base = sb->nsimple;
                    int base_parts = sb->nparts;
                    int n;
                    skip_ws(p);
                    /* :not() takes one compound and no combinators. Its
                     * simples are parsed into the shared pool and then
                     * detached into a private compound; the compound
                     * parse_compound() pushed is popped again, because
                     * the argument is not part of the outer selector.
                     * Its specificity does count, which is why spec_*
                     * is deliberately left alone here. */
                    if (!parse_compound(p, sb, 0))
                        return 0;
                    sb->nparts = base_parts;
                    skip_ws(p);
                    if (p->tok.type != T_RPAREN)
                        return 0;
                    nx(p);
                    if (sb->nsimple == base)
                        return 0;
                    cp = (struct css_compound *)
                         arena_alloc(&p->ss->arena, sizeof(*cp));
                    if (!cp)
                        return 0;
                    n = sb->nsimple - base;
                    {
                        struct css_simple *cpy = (struct css_simple *)
                            arena_alloc(&p->ss->arena,
                                        sizeof(struct css_simple) * (unsigned long)n);
                        if (!cpy)
                            return 0;
                        memcpy(cpy, &sb->simples[base],
                               sizeof(struct css_simple) * (unsigned long)n);
                        cp->simples = cpy;
                        cp->n = n;
                        cp->combinator = CSS_COMB_NONE;
                    }
                    sb->nsimple = base;      /* pop the borrowed simples */
                    s = sb_add(sb);
                    if (!s) return 0;
                    s->kind = CSS_SIMPLE_PSEUDO;
                    s->pseudo = CSS_PSEUDO_NOT;
                    s->sub = cp;
                    count++;
                    continue;
                }
                if (fn_nth || fn_nthl) {
                    int32_t a = 0, b = 0;
                    if (!parse_nth(p, &a, &b))
                        return 0;
                    s = sb_add(sb);
                    if (!s) return 0;
                    s->kind = CSS_SIMPLE_PSEUDO;
                    s->pseudo = (uint8_t)(fn_nth ? CSS_PSEUDO_NTH_CHILD
                                                 : CSS_PSEUDO_NTH_LAST_CHILD);
                    s->a = a;
                    s->b = b;
                    sb->spec_b++;
                    count++;
                    continue;
                }
                /* An unknown functional pseudo - :is(), :where(),
                 * :nth-of-type() - is consumed balanced and this one
                 * selector is dropped. */
                {
                    int depth = 1;
                    while (p->tok.type != T_EOF && depth > 0) {
                        if (p->tok.type == T_LPAREN || p->tok.type == T_FUNCTION)
                            depth++;
                        else if (p->tok.type == T_RPAREN)
                            depth--;
                        nx(p);
                    }
                }
                sb->unsupported = 1;
                return 0;
            }
            nx(p);
            if (is_element) {
                /* ::before / ::after generate content we cannot render,
                 * so this selector is dropped - but its siblings in the
                 * list survive. */
                sb->unsupported = 1;
                return 0;
            }
            {
                static const struct { const char *n; int v; } pt[] = {
                    { "first-child",     CSS_PSEUDO_FIRST_CHILD },
                    { "last-child",      CSS_PSEUDO_LAST_CHILD },
                    { "only-child",      CSS_PSEUDO_ONLY_CHILD },
                    { "first-of-type",   CSS_PSEUDO_FIRST_OF_TYPE },
                    { "last-of-type",    CSS_PSEUDO_LAST_OF_TYPE },
                    { "empty",           CSS_PSEUDO_EMPTY },
                    { "root",            CSS_PSEUDO_ROOT },
                    { "link",            CSS_PSEUDO_LINK },
                    { "visited",         CSS_PSEUDO_VISITED },
                    { "hover",           CSS_PSEUDO_HOVER },
                    { "active",          CSS_PSEUDO_ACTIVE },
                    { "focus",           CSS_PSEUDO_FOCUS },
                    { "checked",         CSS_PSEUDO_CHECKED },
                    { "disabled",        CSS_PSEUDO_DISABLED },
                    { "enabled",         CSS_PSEUDO_ENABLED }
                };
                unsigned j;
                k = -1;
                for (j = 0; j < sizeof(pt) / sizeof(pt[0]); j++)
                    if (css_stricmp(pname, pt[j].n) == 0) {
                        k = pt[j].v;
                        break;
                    }
                if (k < 0) {
                    sb->unsupported = 1;
                    return 0;
                }
                s = sb_add(sb);
                if (!s) return 0;
                s->kind = CSS_SIMPLE_PSEUDO;
                s->pseudo = (uint8_t)k;
                sb->spec_b++;
                count++;
            }
            continue;
        }
        break;
    }
    if (count == 0)
        return 0;
    if (sb->nparts >= CSS_MAX_COMPOUNDS) {
        sb->overflowed = 1;
        return 0;
    }
    sb->parts[sb->nparts].simples = &sb->simples[first];
    sb->parts[sb->nparts].n = sb->nsimple - first;
    sb->parts[sb->nparts].combinator = CSS_COMB_NONE;
    sb->nparts++;
    return 1;
}

/* :nth-child argument. The opening paren is already consumed. */
static int parse_nth(struct css_parser *p, int32_t *a, int32_t *b)
{
    int sign = 1;

    *a = 0;
    *b = 0;
    skip_ws(p);
    if (p->tok.type == T_IDENT) {
        const char *s = p->tok.text;
        if (css_stricmp(s, "odd") == 0) {
            *a = 2; *b = 1; nx(p);
            goto close;
        }
        if (css_stricmp(s, "even") == 0) {
            *a = 2; *b = 0; nx(p);
            goto close;
        }
        /* n, -n, n-3, -n-3 */
        {
            int i = 0, neg = 0;
            if (s[i] == '-' || s[i] == '+') {
                neg = (s[i] == '-');
                i++;
            }
            if (lc((unsigned char)s[i]) != 'n')
                return 0;
            i++;
            *a = neg ? -1 : 1;
            if (s[i] == '-' || s[i] == '+') {
                int bneg = (s[i] == '-');
                int v = 0;
                i++;
                if (!is_digit((unsigned char)s[i]))
                    return 0;
                while (is_digit((unsigned char)s[i]) && v < 100000)
                    v = v * 10 + (s[i++] - '0');
                if (s[i])
                    return 0;
                *b = bneg ? -v : v;
                nx(p);
                goto close;
            }
            if (s[i])
                return 0;
            nx(p);
        }
    } else if (p->tok.type == T_DIMENSION) {
        const char *u = p->tok.unit;
        int i = 0;
        if (lc((unsigned char)u[0]) != 'n')
            return 0;
        *a = (int32_t)(p->tok.num / 1000);
        i = 1;
        if (u[i] == '-' || u[i] == '+') {
            int bneg = (u[i] == '-');
            int v = 0;
            i++;
            if (!is_digit((unsigned char)u[i]))
                return 0;
            while (is_digit((unsigned char)u[i]) && v < 100000)
                v = v * 10 + (u[i++] - '0');
            if (u[i])
                return 0;
            *b = bneg ? -v : v;
            nx(p);
            goto close;
        }
        if (u[i])
            return 0;
        nx(p);
    } else if (p->tok.type == T_NUMBER) {
        if (!p->tok.is_int)
            return 0;
        *a = 0;
        *b = (int32_t)(p->tok.num / 1000);
        nx(p);
        goto close;
    } else {
        return 0;
    }

    /* optional "+ b" / "- b" after the an term */
    skip_ws(p);
    if (p->tok.type == T_DELIM && (p->tok.delim == '+' || p->tok.delim == '-')) {
        sign = (p->tok.delim == '-') ? -1 : 1;
        nx(p);
        skip_ws(p);
        if (p->tok.type != T_NUMBER || !p->tok.is_int)
            return 0;
        *b = sign * (int32_t)(p->tok.num / 1000);
        nx(p);
    } else if (p->tok.type == T_NUMBER) {
        if (!p->tok.is_int)
            return 0;
        *b = (int32_t)(p->tok.num / 1000);
        nx(p);
    }

close:
    skip_ws(p);
    if (p->tok.type != T_RPAREN)
        return 0;
    nx(p);
    return 1;
}

/* Parse one full selector (compounds joined by combinators). Returns 1
 * and fills sb; the caller then copies it into the arena. */
static int parse_selector(struct css_parser *p, struct sel_build *sb)
{
    sb->nsimple = 0;
    sb->nparts = 0;
    sb->spec_a = sb->spec_b = sb->spec_c = 0;
    sb->overflowed = 0;
    sb->unsupported = 0;

    skip_ws(p);
    if (!parse_compound(p, sb, 1))
        return 0;
    for (;;) {
        int comb = CSS_COMB_NONE, had_ws = 0;
        while (p->tok.type == T_WS) {
            had_ws = 1;
            nx(p);
        }
        if (p->tok.type == T_DELIM &&
            (p->tok.delim == '>' || p->tok.delim == '+' || p->tok.delim == '~')) {
            comb = (p->tok.delim == '>') ? CSS_COMB_CHILD :
                   (p->tok.delim == '+') ? CSS_COMB_ADJACENT : CSS_COMB_SIBLING;
            nx(p);
            skip_ws(p);
        } else if (had_ws) {
            if (p->tok.type == T_COMMA || p->tok.type == T_LBRACE ||
                p->tok.type == T_EOF)
                break;
            comb = CSS_COMB_DESCENDANT;
        } else {
            break;
        }
        if (!parse_compound(p, sb, 1))
            return 0;
        sb->parts[sb->nparts - 1].combinator = (uint8_t)comb;
    }
    return !sb->overflowed;
}

static uint32_t pack_spec(int a, int b, int c)
{
    if (a > 1023) a = 1023;
    if (b > 1023) b = 1023;
    if (c > 1023) c = 1023;
    return ((uint32_t)a << 20) | ((uint32_t)b << 10) | (uint32_t)c;
}

/* ================================================================== *
 * assembling rules
 * ================================================================== */

static int sheet_grow_rules(struct css_stylesheet *ss)
{
    int nc;
    struct css_rule *r;

    if (ss->nrule < ss->crule)
        return 1;
    if (ss->nrule >= CSS_MAX_RULES) {
        ss->truncated = 1;
        return 0;
    }
    nc = ss->crule ? ss->crule * 2 : 64;
    if (nc > CSS_MAX_RULES)
        nc = CSS_MAX_RULES;
    r = (struct css_rule *)realloc(ss->rules,
                                   sizeof(struct css_rule) * (unsigned long)nc);
    if (!r) {
        ss->truncated = 1;
        return 0;
    }
    ss->rules = r;
    ss->crule = nc;
    return 1;
}

static int current_media(struct css_parser *p)
{
    if (p->media_depth == 0)
        return -1;
    return p->media_stack[p->media_depth - 1];
}

/* Parse "selectors { declarations }". */
static void parse_qualified_rule(struct css_parser *p)
{
    struct sel_build sb;
    struct css_selector *sels = p->sels;
    int nsel = 0, i, bad = 0;
    const struct css_decl *shared = 0;
    int ndecl;

    sb.simples = p->simples;
    sb.parts = p->compounds;

    for (;;) {
        int ok = parse_selector(p, &sb);
        if (!ok && !sb.unsupported)
            bad = 1;
        if (ok && sb.nparts > 0 && nsel >= CSS_MAX_SEL_LIST)
            p->ss->truncated = 1;
        if (ok && sb.nparts > 0 && nsel < CSS_MAX_SEL_LIST) {
            struct css_compound *parts;
            struct css_simple *sim;
            int j;
            parts = (struct css_compound *)arena_alloc(&p->ss->arena,
                        sizeof(struct css_compound) * (unsigned long)sb.nparts);
            sim = (struct css_simple *)arena_alloc(&p->ss->arena,
                        sizeof(struct css_simple) * (unsigned long)sb.nsimple);
            if (!parts || !sim) {
                ok = 0;
            } else {
                memcpy(sim, sb.simples,
                       sizeof(struct css_simple) * (unsigned long)sb.nsimple);
                for (j = 0; j < sb.nparts; j++) {
                    long off = sb.parts[j].simples - sb.simples;
                    parts[j].simples = sim + off;
                    parts[j].n = sb.parts[j].n;
                    parts[j].combinator = sb.parts[j].combinator;
                }
                sels[nsel].parts = parts;
                sels[nsel].nparts = sb.nparts;
                sels[nsel].specificity =
                    pack_spec(sb.spec_a, sb.spec_b, sb.spec_c);
                nsel++;
            }
        }
        skip_ws(p);
        if (p->tok.type == T_COMMA) {
            nx(p);
            continue;
        }
        if (p->tok.type == T_LBRACE || p->tok.type == T_EOF)
            break;
        /* Junk between selectors: bail out to the next block. */
        while (p->tok.type != T_LBRACE && p->tok.type != T_EOF &&
               p->tok.type != T_COMMA)
            nx(p);
        if (p->tok.type == T_COMMA) {
            nx(p);
            continue;
        }
        break;
    }
    if (p->tok.type != T_LBRACE)
        return;
    nx(p);
    parse_block(p);
    ndecl = p->nblock;
    /* A syntax error anywhere in the selector list throws the entire
     * rule away, per CSS 2.1 section 4.1.7. The block was still parsed,
     * which is what put the reader back in a known place. */
    if (bad || nsel == 0 || ndecl == 0)
        return;
    {
        struct css_decl *d = (struct css_decl *)arena_alloc(&p->ss->arena,
                sizeof(struct css_decl) * (unsigned long)ndecl);
        if (!d)
            return;
        memcpy(d, p->block, sizeof(struct css_decl) * (unsigned long)ndecl);
        shared = d;
    }
    for (i = 0; i < nsel; i++) {
        struct css_rule *r;
        if (!sheet_grow_rules(p->ss))
            return;
        r = &p->ss->rules[p->ss->nrule];
        r->sel = sels[i];
        r->decls = shared;
        r->ndecl = ndecl;
        r->order = (uint32_t)p->ss->nrule;
        r->media = (int16_t)current_media(p);
        r->origin = (uint8_t)p->ss->origin;
        p->ss->nrule++;
    }
}

/* ================================================================== *
 * media query parsing and evaluation
 * ================================================================== */

static int media_feature_id(const char *n, int *op)
{
    const char *base = n;
    *op = MOP_EQ;
    if (strncmp(n, "min-", 4) == 0) { *op = MOP_MIN; base = n + 4; }
    else if (strncmp(n, "max-", 4) == 0) { *op = MOP_MAX; base = n + 4; }
    if (strncmp(base, "device-", 7) == 0)
        base += 7;
    if (css_stricmp(base, "width") == 0)       return MF_WIDTH;
    if (css_stricmp(base, "height") == 0)      return MF_HEIGHT;
    if (css_stricmp(base, "orientation") == 0) return MF_ORIENTATION;
    if (css_stricmp(base, "resolution") == 0)  return MF_RESOLUTION;
    if (css_stricmp(base, "color") == 0)       return MF_COLOR;
    if (css_stricmp(base, "monochrome") == 0)  return MF_MONOCHROME;
    return MF_UNKNOWN;
}

static int eval_query(const struct media_query *q, const struct css_media *m)
{
    int r = 1, i;

    if (q->type == MT_SCREEN && !m->screen) r = 0;
    if (q->type == MT_PRINT && m->screen)   r = 0;
    if (q->type == MT_OTHER)                r = 0;
    for (i = 0; r && i < q->nfeat; i++) {
        const struct media_feat *f = &q->f[i];
        int32_t have;
        switch (f->feature) {
        case MF_WIDTH:      have = m->width; break;
        case MF_HEIGHT:     have = m->height; break;
        case MF_RESOLUTION: have = m->dpi; break;
        case MF_ORIENTATION:
            have = (m->width >= m->height) ? 1 : 0;
            if (have != f->value) r = 0;
            continue;
        case MF_MONOCHROME: have = m->monochrome ? 1 : 0; break;
        case MF_COLOR:      have = m->monochrome ? 0 : 8; break;
        default:            r = 0; continue;
        }
        if (f->op == MOP_MIN && have < f->value) r = 0;
        else if (f->op == MOP_MAX && have > f->value) r = 0;
        else if (f->op == MOP_EQ && have != f->value) r = 0;
    }
    return q->negated ? !r : r;
}

static int eval_block(struct css_stylesheet *ss, int idx,
                      const struct css_media *m)
{
    struct media_block *mb;
    int i, any;

    if (idx < 0 || idx >= ss->nmedia)
        return 1;
    mb = ss->media[idx];
    if (mb->parent >= 0 && !eval_block(ss, mb->parent, m))
        return 0;
    if (mb->nq == 0)
        return 1;
    any = 0;
    for (i = 0; i < mb->nq; i++)
        if (eval_query(&mb->q[i], m))
            any = 1;
    return any;
}

static int sheet_add_media(struct css_stylesheet *ss, struct media_block *mb)
{
    if (ss->nmedia >= CSS_MAX_MEDIA) {
        ss->truncated = 1;
        return -1;
    }
    if (ss->nmedia >= ss->cmedia) {
        int nc = ss->cmedia ? ss->cmedia * 2 : 8;
        struct media_block **n = (struct media_block **)
            realloc(ss->media, sizeof(*n) * (unsigned long)nc);
        if (!n) {
            ss->truncated = 1;
            return -1;
        }
        ss->media = n;
        ss->cmedia = nc;
    }
    ss->media[ss->nmedia] = mb;
    return ss->nmedia++;
}

/* Parse the query list after "@media", stopping at '{'. */
static int parse_media_list(struct css_parser *p, int parent)
{
    struct media_block *mb = (struct media_block *)
        arena_alloc(&p->ss->arena, sizeof(*mb));
    struct media_query *q;
    int idx;

    if (!mb)
        return -1;
    memset(mb, 0, sizeof(*mb));
    mb->parent = (int16_t)parent;

    for (;;) {
        skip_ws(p);
        if (p->tok.type == T_LBRACE || p->tok.type == T_EOF)
            break;
        if (mb->nq >= CSS_MAX_MEDIA_Q) {
            while (p->tok.type != T_LBRACE && p->tok.type != T_EOF)
                nx(p);
            break;
        }
        q = &mb->q[mb->nq];
        memset(q, 0, sizeof(*q));
        q->type = MT_ALL;

        if (p->tok.type == T_IDENT && css_stricmp(p->tok.text, "not") == 0) {
            q->negated = 1;
            nx(p);
            skip_ws(p);
        } else if (p->tok.type == T_IDENT &&
                   css_stricmp(p->tok.text, "only") == 0) {
            q->only = 1;
            nx(p);
            skip_ws(p);
        }
        if (p->tok.type == T_IDENT) {
            if (css_stricmp(p->tok.text, "screen") == 0)      q->type = MT_SCREEN;
            else if (css_stricmp(p->tok.text, "print") == 0)  q->type = MT_PRINT;
            else if (css_stricmp(p->tok.text, "all") == 0)    q->type = MT_ALL;
            else                                              q->type = MT_OTHER;
            nx(p);
            skip_ws(p);
        }
        for (;;) {
            if (p->tok.type == T_IDENT &&
                css_stricmp(p->tok.text, "and") == 0) {
                nx(p);
                skip_ws(p);
                continue;
            }
            if (p->tok.type != T_LPAREN)
                break;
            nx(p);
            skip_ws(p);
            if (p->tok.type != T_IDENT) {
                while (p->tok.type != T_RPAREN && p->tok.type != T_LBRACE &&
                       p->tok.type != T_EOF)
                    nx(p);
                if (p->tok.type == T_RPAREN)
                    nx(p);
                q->type = MT_OTHER;      /* unparseable feature: never match */
                continue;
            }
            {
                char fname[CSS_MAX_IDENT];
                int op, fid;
                memcpy(fname, p->tok.text, (unsigned long)p->tok.tlen + 1);
                fid = media_feature_id(fname, &op);
                nx(p);
                skip_ws(p);
                if (p->tok.type == T_COLON) {
                    nx(p);
                    skip_ws(p);
                    if (q->nfeat < CSS_MAX_MEDIA_FEATS) {
                        struct media_feat *f = &q->f[q->nfeat];
                        f->feature = (uint8_t)fid;
                        f->op = (uint8_t)op;
                        f->unit = 0;
                        f->value = 0;
                        if (p->tok.type == T_DIMENSION) {
                            int64_t px;
                            int u = unit_id(p->tok.unit);
                            if (unit_to_px_milli(p->tok.num, u, &px))
                                f->value = milli_round(px);
                            else
                                f->feature = MF_UNKNOWN;
                            nx(p);
                        } else if (p->tok.type == T_NUMBER) {
                            f->value = (int32_t)(p->tok.num / 1000);
                            nx(p);
                        } else if (p->tok.type == T_IDENT) {
                            if (css_stricmp(p->tok.text, "landscape") == 0)
                                f->value = 1;
                            else if (css_stricmp(p->tok.text, "portrait") == 0)
                                f->value = 0;
                            else
                                f->feature = MF_UNKNOWN;
                            nx(p);
                        } else {
                            f->feature = MF_UNKNOWN;
                        }
                        q->nfeat++;
                    }
                } else if (q->nfeat < CSS_MAX_MEDIA_FEATS) {
                    /* a boolean feature: (color) */
                    struct media_feat *f = &q->f[q->nfeat];
                    f->feature = (uint8_t)fid;
                    f->op = MOP_MIN;
                    f->value = 1;
                    f->unit = 0;
                    q->nfeat++;
                }
            }
            while (p->tok.type != T_RPAREN && p->tok.type != T_LBRACE &&
                   p->tok.type != T_EOF)
                nx(p);
            if (p->tok.type == T_RPAREN)
                nx(p);
            skip_ws(p);
        }
        mb->nq++;
        skip_ws(p);
        if (p->tok.type == T_COMMA) {
            nx(p);
            continue;
        }
        if (p->tok.type == T_LBRACE || p->tok.type == T_EOF)
            break;
        /* garbage: skip to the comma or the brace */
        while (p->tok.type != T_COMMA && p->tok.type != T_LBRACE &&
               p->tok.type != T_EOF)
            nx(p);
        if (p->tok.type == T_COMMA)
            nx(p);
    }
    idx = sheet_add_media(p->ss, mb);
    if (idx >= 0)
        mb->matches = (uint8_t)eval_block(p->ss, idx, &p->ss->env);
    return idx;
}

static void parse_rule_list(struct css_parser *p, int in_block);

static void parse_at_rule(struct css_parser *p)
{
    char name[CSS_MAX_IDENT];

    memcpy(name, p->tok.text, (unsigned long)p->tok.tlen + 1);
    nx(p);

    if (css_stricmp(name, "media") == 0) {
        int idx = parse_media_list(p, current_media(p));
        if (p->tok.type != T_LBRACE) {
            if (p->tok.type == T_SEMI)
                nx(p);
            return;
        }
        nx(p);
        if (p->media_depth < 8) {
            p->media_stack[p->media_depth++] = idx;
            parse_rule_list(p, 1);
            p->media_depth--;
        } else {
            skip_block(p);
        }
        return;
    }
    if (css_stricmp(name, "import") == 0) {
        skip_ws(p);
        if (p->tok.type == T_URL || p->tok.type == T_STRING) {
            if (p->ss->nimport < CSS_MAX_IMPORTS) {
                const char *u = arena_str(&p->ss->arena, p->tok.text,
                                          (unsigned long)p->tok.tlen);
                if (u)
                    p->ss->imports[p->ss->nimport++] = u;
            } else {
                p->ss->truncated = 1;
            }
            nx(p);
        }
        while (p->tok.type != T_SEMI && p->tok.type != T_EOF &&
               p->tok.type != T_LBRACE)
            nx(p);
        if (p->tok.type == T_SEMI)
            nx(p);
        else if (p->tok.type == T_LBRACE) {
            nx(p);
            skip_block(p);
        }
        return;
    }
    /* Everything else - @font-face, @page, @keyframes, @supports,
     * @charset, @namespace - is parsed far enough to be skipped safely.
     * @supports in particular is skipped rather than included: its blocks
     * are gated on features this engine does not have. */
    while (p->tok.type != T_SEMI && p->tok.type != T_LBRACE &&
           p->tok.type != T_EOF)
        nx(p);
    if (p->tok.type == T_SEMI) {
        nx(p);
        return;
    }
    if (p->tok.type == T_LBRACE) {
        nx(p);
        skip_block(p);
    }
}

static void parse_rule_list(struct css_parser *p, int in_block)
{
    for (;;) {
        skip_ws(p);
        while (p->tok.type == T_CDO || p->tok.type == T_CDC) {
            nx(p);
            skip_ws(p);
        }
        if (p->tok.type == T_EOF)
            return;
        if (in_block && p->tok.type == T_RBRACE) {
            nx(p);
            return;
        }
        if (p->tok.type == T_AT) {
            parse_at_rule(p);
            continue;
        }
        if (p->tok.type == T_RBRACE) {   /* stray close brace at top level */
            nx(p);
            continue;
        }
        parse_qualified_rule(p);
        if (p->ss->arena.oom)
            return;
        /* Once the rule cap is reached there is nothing left to gain by
         * reading the rest, and continuing would burn the whole arena on
         * rules that get thrown away. */
        if (p->ss->nrule >= CSS_MAX_RULES) {
            p->ss->truncated = 1;
            return;
        }
    }
}

/* ================================================================== *
 * index construction
 * ================================================================== */

static unsigned long hash_str(const char *s)
{
    unsigned long h = 2166136261UL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619UL;
    }
    return h;
}

static struct idx_node *idx_find(struct idx_map *m, const char *key, int create,
                                 struct css_arena *a)
{
    unsigned long h = hash_str(key) % IDX_BUCKETS;
    struct idx_node *n = m->b[h];

    while (n) {
        if (strcmp(n->key, key) == 0)
            return n;
        n = n->next;
    }
    if (!create)
        return 0;
    n = (struct idx_node *)arena_alloc(a, sizeof(*n));
    if (!n)
        return 0;
    n->next = m->b[h];
    n->key = key;
    n->rules = 0;
    n->n = 0;
    n->cap = 0;
    m->b[h] = n;
    return n;
}

/* The key a rule is indexed under is taken from its rightmost compound:
 * an id if it has one, else a class, else a type, else universal. */
static void rule_key_of(const struct css_rule *r, struct rule_key *k)
{
    const struct css_compound *cp;
    int i;

    k->kind = KEY_UNIVERSAL;
    k->name = 0;
    if (r->sel.nparts <= 0)
        return;
    cp = &r->sel.parts[r->sel.nparts - 1];
    for (i = 0; i < cp->n; i++)
        if (cp->simples[i].kind == CSS_SIMPLE_ID) {
            k->kind = KEY_ID;
            k->name = cp->simples[i].name;
            return;
        }
    for (i = 0; i < cp->n; i++)
        if (cp->simples[i].kind == CSS_SIMPLE_CLASS) {
            k->kind = KEY_CLASS;
            k->name = cp->simples[i].name;
            return;
        }
    for (i = 0; i < cp->n; i++)
        if (cp->simples[i].kind == CSS_SIMPLE_TYPE) {
            k->kind = KEY_TAG;
            k->name = cp->simples[i].name;
            return;
        }
}

static void build_index(struct css_stylesheet *ss)
{
    int i, pass;

    ss->keys = (struct rule_key *)arena_alloc(&ss->arena,
                   sizeof(struct rule_key) * (unsigned long)(ss->nrule + 1));
    if (!ss->keys)
        return;
    for (i = 0; i < ss->nrule; i++)
        rule_key_of(&ss->rules[i], &ss->keys[i]);

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < ss->nrule; i++) {
            struct rule_key *k = &ss->keys[i];
            struct idx_map *m;
            struct idx_node *n;
            if (k->kind == KEY_UNIVERSAL) {
                if (pass == 0)
                    ss->nuniv++;
                else
                    ss->universal[ss->nuniv++] = i;
                continue;
            }
            m = (k->kind == KEY_ID) ? &ss->id_map :
                (k->kind == KEY_CLASS) ? &ss->class_map : &ss->tag_map;
            n = idx_find(m, k->name, 1, &ss->arena);
            if (!n)
                return;
            if (pass == 0) {
                n->cap++;
            } else {
                if (n->rules && n->n < n->cap)
                    n->rules[n->n++] = i;
            }
        }
        if (pass == 0) {
            int b;
            if (ss->nuniv) {
                ss->universal = (int *)arena_alloc(&ss->arena,
                                    sizeof(int) * (unsigned long)ss->nuniv);
                if (!ss->universal)
                    return;
            }
            ss->nuniv = 0;
            for (b = 0; b < IDX_BUCKETS; b++) {
                struct idx_map *maps[3];
                int mi;
                maps[0] = &ss->id_map;
                maps[1] = &ss->class_map;
                maps[2] = &ss->tag_map;
                for (mi = 0; mi < 3; mi++) {
                    struct idx_node *n = maps[mi]->b[b];
                    while (n) {
                        if (n->cap > 0 && !n->rules) {
                            n->rules = (int *)arena_alloc(&ss->arena,
                                sizeof(int) * (unsigned long)n->cap);
                            if (!n->rules)
                                return;
                        }
                        n->n = 0;
                        n = n->next;
                    }
                }
            }
        }
    }
}

/* ================================================================== *
 * public parse entry points
 * ================================================================== */

static const struct css_media default_media = { 1024, 768, 96, 1, 0 };

static struct css_stylesheet *sheet_new(int origin, const struct css_media *m)
{
    struct css_stylesheet *ss =
        (struct css_stylesheet *)calloc(1, sizeof(struct css_stylesheet));
    if (!ss)
        return 0;
    ss->origin = origin;
    ss->env = m ? *m : default_media;
    if (ss->env.dpi <= 0)
        ss->env.dpi = 96;
    return ss;
}

struct css_stylesheet *css_parse(const char *src, unsigned long len,
                                 int origin, const struct css_media *media)
{
    struct css_stylesheet *ss;
    struct css_parser *p;

    ss = sheet_new(origin, media);
    if (!ss)
        return 0;
    if (!src)
        len = 0;
    if (len > CSS_MAX_SOURCE) {
        len = CSS_MAX_SOURCE;
        ss->truncated = 1;
    }
    p = (struct css_parser *)calloc(1, sizeof(struct css_parser));
    if (!p) {
        ss->truncated = 1;
        return ss;
    }
    p->ss = ss;
    p->lex.s = src;
    p->lex.n = len;
    p->lex.i = 0;
    nx(p);
    parse_rule_list(p, 0);
    free(p);
    if (ss->arena.oom)
        ss->truncated = 1;
    build_index(ss);
    return ss;
}

struct css_stylesheet *css_parse_style_attr(const char *text)
{
    struct css_stylesheet *ss;
    struct css_parser *p;
    unsigned long len;

    ss = sheet_new(CSS_ORIGIN_INLINE, 0);
    if (!ss)
        return 0;
    if (!text)
        return ss;
    len = strlen(text);
    if (len > CSS_MAX_SOURCE) {
        len = CSS_MAX_SOURCE;
        ss->truncated = 1;
    }
    p = (struct css_parser *)calloc(1, sizeof(struct css_parser));
    if (!p) {
        ss->truncated = 1;
        return ss;
    }
    p->ss = ss;
    p->lex.s = text;
    p->lex.n = len;
    nx(p);
    parse_block(p);
    if (p->nblock > 0 && sheet_grow_rules(ss)) {
        struct css_decl *d = (struct css_decl *)arena_alloc(&ss->arena,
                sizeof(struct css_decl) * (unsigned long)p->nblock);
        struct css_compound *cp = (struct css_compound *)
                arena_alloc(&ss->arena, sizeof(*cp));
        struct css_simple *sim = (struct css_simple *)
                arena_alloc(&ss->arena, sizeof(*sim));
        if (d && cp && sim) {
            memcpy(d, p->block,
                   sizeof(struct css_decl) * (unsigned long)p->nblock);
            memset(sim, 0, sizeof(*sim));
            sim->kind = CSS_SIMPLE_UNIVERSAL;
            cp->simples = sim;
            cp->n = 1;
            cp->combinator = CSS_COMB_NONE;
            ss->rules[0].sel.parts = cp;
            ss->rules[0].sel.nparts = 1;
            ss->rules[0].sel.specificity = 0;
            ss->rules[0].decls = d;
            ss->rules[0].ndecl = p->nblock;
            ss->rules[0].order = 0;
            ss->rules[0].media = -1;
            ss->rules[0].origin = CSS_ORIGIN_INLINE;
            ss->nrule = 1;
        }
    }
    free(p);
    if (ss->arena.oom)
        ss->truncated = 1;
    build_index(ss);
    return ss;
}

void css_free(struct css_stylesheet *ss)
{
    if (!ss)
        return;
    arena_free(&ss->arena);
    free(ss->rules);
    free(ss->media);
    free(ss);
}

int css_rule_count(const struct css_stylesheet *ss)
{
    return ss ? ss->nrule : 0;
}

const struct css_rule *css_rule_at(const struct css_stylesheet *ss, int i)
{
    if (!ss || i < 0 || i >= ss->nrule)
        return 0;
    return &ss->rules[i];
}

int css_truncated(const struct css_stylesheet *ss)
{
    return ss ? ss->truncated : 0;
}

unsigned long css_memory_used(const struct css_stylesheet *ss)
{
    if (!ss)
        return 0;
    return ss->arena.total +
           (unsigned long)ss->crule * sizeof(struct css_rule) +
           (unsigned long)ss->nmedia * sizeof(struct media_block);
}

int css_import_count(const struct css_stylesheet *ss)
{
    return ss ? ss->nimport : 0;
}

const char *css_import_url(const struct css_stylesheet *ss, int i)
{
    if (!ss || i < 0 || i >= ss->nimport)
        return 0;
    return ss->imports[i];
}

uint32_t css_set_order_base(struct css_stylesheet *ss, uint32_t base)
{
    int i;
    if (!ss)
        return base;
    for (i = 0; i < ss->nrule; i++)
        ss->rules[i].order = base + (uint32_t)i;
    return base + (uint32_t)ss->nrule;
}

int css_set_media(struct css_stylesheet *ss, const struct css_media *m)
{
    int i, changed = 0;
    if (!ss || !m)
        return 0;
    ss->env = *m;
    if (ss->env.dpi <= 0)
        ss->env.dpi = 96;
    for (i = 0; i < ss->nmedia; i++) {
        uint8_t v = (uint8_t)eval_block(ss, i, &ss->env);
        if (v != ss->media[i]->matches)
            changed++;
        ss->media[i]->matches = v;
    }
    return changed;
}

static int rule_enabled(const struct css_stylesheet *ss,
                        const struct css_rule *r)
{
    if (r->media < 0)
        return 1;
    if (r->media >= ss->nmedia)
        return 0;
    return ss->media[r->media]->matches != 0;
}

/* ================================================================== *
 * matching
 * ================================================================== */

static unsigned long stat_considered, stat_matched;

void css_match_stats(unsigned long *considered, unsigned long *matched)
{
    if (considered) *considered = stat_considered;
    if (matched)    *matched = stat_matched;
}

void css_match_stats_reset(void)
{
    stat_considered = 0;
    stat_matched = 0;
}

static const char *elem_id(void *e, const struct css_elem_ops *ops)
{
    if (ops->id)
        return ops->id(e);
    return ops->attr ? ops->attr(e, "id") : 0;
}

static int elem_has_class(void *e, const struct css_elem_ops *ops,
                          const char *cls)
{
    const char *c;
    unsigned long n;

    if (ops->has_class)
        return ops->has_class(e, cls);
    if (!ops->attr)
        return 0;
    c = ops->attr(e, "class");
    if (!c || !cls || !*cls)
        return 0;
    n = strlen(cls);
    while (*c) {
        const char *st;
        while (*c && is_ws((unsigned char)*c))
            c++;
        st = c;
        while (*c && !is_ws((unsigned char)*c))
            c++;
        if ((unsigned long)(c - st) == n && memcmp(st, cls, n) == 0)
            return 1;
    }
    return 0;
}

static int attr_match(const char *have, const char *want, int op)
{
    unsigned long hl, wl;

    if (!have)
        return 0;
    if (op == CSS_ATTR_EXISTS)
        return 1;
    if (!want)
        return 0;
    hl = strlen(have);
    wl = strlen(want);
    switch (op) {
    case CSS_ATTR_EQ:
        return strcmp(have, want) == 0;
    case CSS_ATTR_INCLUDES: {
        const char *c = have;
        if (wl == 0)
            return 0;
        while (*c) {
            const char *st;
            while (*c && is_ws((unsigned char)*c))
                c++;
            st = c;
            while (*c && !is_ws((unsigned char)*c))
                c++;
            if ((unsigned long)(c - st) == wl && memcmp(st, want, wl) == 0)
                return 1;
        }
        return 0;
    }
    case CSS_ATTR_DASH:
        if (wl == 0)
            return 0;
        if (strcmp(have, want) == 0)
            return 1;
        return hl > wl && memcmp(have, want, wl) == 0 && have[wl] == '-';
    case CSS_ATTR_PREFIX:
        return wl > 0 && hl >= wl && memcmp(have, want, wl) == 0;
    case CSS_ATTR_SUFFIX:
        return wl > 0 && hl >= wl && memcmp(have + hl - wl, want, wl) == 0;
    case CSS_ATTR_SUBSTR:
        if (wl == 0)
            return 0;
        return strstr(have, want) != 0;
    default:
        return 0;
    }
}

static int sibling_index(void *e, const struct css_elem_ops *ops, int from_end)
{
    int i = 1;
    void *(*step)(void *) = from_end ? ops->next : ops->prev;
    void *q;

    if (!step)
        return 1;
    for (q = step(e); q; q = step(q)) {
        i++;
        if (i > 100000)
            break;
    }
    return i;
}

static int nth_ok(int index, int32_t a, int32_t b)
{
    if (a == 0)
        return index == b;
    if (((int64_t)index - b) % a != 0)
        return 0;
    return ((int64_t)index - b) / a >= 0;
}

static int match_compound(const struct css_compound *cp, void *e,
                          const struct css_elem_ops *ops, int depth);

static int match_pseudo(const struct css_simple *s, void *e,
                        const struct css_elem_ops *ops, int depth)
{
    unsigned st = ops->state ? ops->state(e) : 0;

    switch (s->pseudo) {
    case CSS_PSEUDO_FIRST_CHILD:
        return ops->prev ? ops->prev(e) == 0 : 0;
    case CSS_PSEUDO_LAST_CHILD:
        return ops->next ? ops->next(e) == 0 : 0;
    case CSS_PSEUDO_ONLY_CHILD:
        return (ops->prev && ops->next) &&
               ops->prev(e) == 0 && ops->next(e) == 0;
    case CSS_PSEUDO_NTH_CHILD:
        return nth_ok(sibling_index(e, ops, 0), s->a, s->b);
    case CSS_PSEUDO_NTH_LAST_CHILD:
        return nth_ok(sibling_index(e, ops, 1), s->a, s->b);
    case CSS_PSEUDO_FIRST_OF_TYPE: {
        const char *t = ops->tag(e);
        void *q;
        if (!ops->prev)
            return 0;
        for (q = ops->prev(e); q; q = ops->prev(q))
            if (t && ops->tag(q) && strcmp(t, ops->tag(q)) == 0)
                return 0;
        return 1;
    }
    case CSS_PSEUDO_LAST_OF_TYPE: {
        const char *t = ops->tag(e);
        void *q;
        if (!ops->next)
            return 0;
        for (q = ops->next(e); q; q = ops->next(q))
            if (t && ops->tag(q) && strcmp(t, ops->tag(q)) == 0)
                return 0;
        return 1;
    }
    case CSS_PSEUDO_EMPTY:
        /* No is_empty() means we can only see element children, and
         * claiming <p>text</p> is empty would hide real content, so the
         * conservative answer is "no". */
        if (ops->is_empty)
            return ops->is_empty(e);
        return 0;
    case CSS_PSEUDO_ROOT:
        return ops->parent ? ops->parent(e) == 0 : 0;
    case CSS_PSEUDO_LINK: {
        const char *t = ops->tag(e);
        if (!t || !ops->attr)
            return 0;
        if (strcmp(t, "a") && strcmp(t, "area") && strcmp(t, "link"))
            return 0;
        if (!ops->attr(e, "href"))
            return 0;
        return (st & CSS_STATE_VISITED) ? 0 : 1;
    }
    case CSS_PSEUDO_VISITED:
        return (st & CSS_STATE_VISITED) != 0;
    case CSS_PSEUDO_HOVER:
        return (st & CSS_STATE_HOVER) != 0;
    case CSS_PSEUDO_ACTIVE:
        return (st & CSS_STATE_ACTIVE) != 0;
    case CSS_PSEUDO_FOCUS:
        return (st & CSS_STATE_FOCUS) != 0;
    case CSS_PSEUDO_CHECKED:
        return (st & CSS_STATE_CHECKED) != 0;
    case CSS_PSEUDO_DISABLED:
        return (st & CSS_STATE_DISABLED) != 0 ||
               (ops->attr && ops->attr(e, "disabled") != 0);
    case CSS_PSEUDO_ENABLED:
        return !((st & CSS_STATE_DISABLED) ||
                 (ops->attr && ops->attr(e, "disabled")));
    case CSS_PSEUDO_NOT:
        if (!s->sub || depth >= 2)
            return 0;
        return !match_compound(s->sub, e, ops, depth + 1);
    default:
        return 0;
    }
}

static int match_compound(const struct css_compound *cp, void *e,
                          const struct css_elem_ops *ops, int depth)
{
    int i;

    for (i = 0; i < cp->n; i++) {
        const struct css_simple *s = &cp->simples[i];
        switch (s->kind) {
        case CSS_SIMPLE_UNIVERSAL:
            break;
        case CSS_SIMPLE_TYPE: {
            const char *t = ops->tag(e);
            if (!t || !s->name || strcmp(t, s->name) != 0)
                return 0;
            break;
        }
        case CSS_SIMPLE_CLASS:
            if (!elem_has_class(e, ops, s->name))
                return 0;
            break;
        case CSS_SIMPLE_ID: {
            const char *id = elem_id(e, ops);
            if (!id || !s->name || strcmp(id, s->name) != 0)
                return 0;
            break;
        }
        case CSS_SIMPLE_ATTR: {
            const char *have = ops->attr ? ops->attr(e, s->name) : 0;
            if (!attr_match(have, s->value, s->op))
                return 0;
            break;
        }
        case CSS_SIMPLE_PSEUDO:
            if (!match_pseudo(s, e, ops, depth))
                return 0;
            break;
        default:
            return 0;
        }
    }
    return 1;
}

/* Match parts[0..i] against e, walking leftwards. Recursion is bounded
 * by CSS_MAX_COMPOUNDS, so at most 16 small frames. */
static int match_chain(const struct css_selector *sel, int i, void *e,
                       const struct css_elem_ops *ops)
{
    void *q;

    if (i < 0)
        return 1;
    if (!match_compound(&sel->parts[i], e, ops, 0))
        return 0;
    if (i == 0)
        return 1;
    switch (sel->parts[i].combinator) {
    case CSS_COMB_CHILD:
        q = ops->parent ? ops->parent(e) : 0;
        return q && match_chain(sel, i - 1, q, ops);
    case CSS_COMB_ADJACENT:
        q = ops->prev ? ops->prev(e) : 0;
        return q && match_chain(sel, i - 1, q, ops);
    case CSS_COMB_SIBLING:
        if (!ops->prev)
            return 0;
        for (q = ops->prev(e); q; q = ops->prev(q))
            if (match_chain(sel, i - 1, q, ops))
                return 1;
        return 0;
    case CSS_COMB_DESCENDANT:
    default:
        if (!ops->parent)
            return 0;
        for (q = ops->parent(e); q; q = ops->parent(q))
            if (match_chain(sel, i - 1, q, ops))
                return 1;
        return 0;
    }
}

int css_selector_matches(const struct css_selector *sel, void *e,
                         const struct css_elem_ops *ops)
{
    if (!sel || !e || !ops || !ops->tag || sel->nparts <= 0)
        return 0;
    return match_chain(sel, sel->nparts - 1, e, ops);
}

#define MAX_CAND_LISTS 40

int css_match(const struct css_stylesheet *ss, void *e,
              const struct css_elem_ops *ops, css_match_fn cb, void *ctx)
{
    const int *lists[MAX_CAND_LISTS];
    int lens[MAX_CAND_LISTS], pos[MAX_CAND_LISTS];
    int nl = 0, found = 0;
    const char *tag, *id;

    if (!ss || !e || !ops || !ops->tag)
        return 0;
    tag = ops->tag(e);
    id = elem_id(e, ops);

    if (ss->nuniv) {
        lists[nl] = ss->universal;
        lens[nl] = ss->nuniv;
        pos[nl] = 0;
        nl++;
    }
    if (tag) {
        struct idx_node *n = idx_find((struct idx_map *)&ss->tag_map, tag, 0, 0);
        if (n && n->n) {
            lists[nl] = n->rules; lens[nl] = n->n; pos[nl] = 0; nl++;
        }
    }
    if (id) {
        struct idx_node *n = idx_find((struct idx_map *)&ss->id_map, id, 0, 0);
        if (n && n->n && nl < MAX_CAND_LISTS) {
            lists[nl] = n->rules; lens[nl] = n->n; pos[nl] = 0; nl++;
        }
    }
    if (ops->attr) {
        const char *c = ops->attr(e, "class");
        char buf[128];
        while (c && *c && nl < MAX_CAND_LISTS) {
            const char *st;
            unsigned long len;
            struct idx_node *n;
            while (*c && is_ws((unsigned char)*c))
                c++;
            st = c;
            while (*c && !is_ws((unsigned char)*c))
                c++;
            len = (unsigned long)(c - st);
            if (len == 0 || len >= sizeof(buf))
                continue;
            memcpy(buf, st, len);
            buf[len] = 0;
            n = idx_find((struct idx_map *)&ss->class_map, buf, 0, 0);
            if (n && n->n) {
                lists[nl] = n->rules; lens[nl] = n->n; pos[nl] = 0; nl++;
            }
        }
    }

    /* k-way merge so rules arrive in ascending source order. */
    for (;;) {
        int best = -1, bestv = 0, i;
        for (i = 0; i < nl; i++) {
            if (pos[i] >= lens[i])
                continue;
            if (best < 0 || lists[i][pos[i]] < bestv) {
                best = i;
                bestv = lists[i][pos[i]];
            }
        }
        if (best < 0)
            break;
        pos[best]++;
        stat_considered++;
        {
            const struct css_rule *r = &ss->rules[bestv];
            if (!rule_enabled(ss, r))
                continue;
            if (!match_chain(&r->sel, r->sel.nparts - 1, e, ops))
                continue;
            stat_matched++;
            found++;
            if (cb)
                cb(ctx, r);
        }
    }
    return found;
}
