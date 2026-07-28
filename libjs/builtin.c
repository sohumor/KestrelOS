/* libjs: the global object, the standard library, and the regular
 * expression engine.
 *
 * The regex engine is a backtracking matcher over the common ES5 subset
 * (see the deviation list in js.h). Two things keep it from being a denial
 * of service: every matcher step is charged against a per-call budget, and
 * quantifiers over single-character terms are matched with a loop instead
 * of recursion, so `/a*b/` against a long string does not use stack in
 * proportion to the string length. General quantifiers still recurse, and
 * that recursion has its own depth cap.
 */

#define JS_INTERNAL
#include "js.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef JS_HOST
#include <kestrel.h>
#endif

/* ================================================================== */
/* Small helpers                                                       */
/* ================================================================== */

static js_value arg(int argc, js_value *argv, int i)
{
    return i < argc ? argv[i] : js_undefined();
}

static int ret_str(js_ctx *ctx, const char *s, unsigned long n, js_value *out)
{
    js_string *r = js_str_new(ctx, s, n);
    if (!r) { *out = js_undefined(); return JS_THROW; }
    *out = js_string_value(r);
    return JS_OK;
}

static double now_ms(js_ctx *ctx)
{
    if (ctx->cfg.now_ms)
        return ctx->cfg.now_ms(ctx->cfg.user);
#ifdef JS_HOST
    return 0.0;
#else
    return (double)syscall(SYS_TIME, 0, 0, 0, 0) * 1000.0;
#endif
}

/* ================================================================== */
/* Regular expressions                                                 */
/* ================================================================== */

enum {
    RE_CHAR = 0, RE_ANY, RE_CLASS, RE_BOL, RE_EOL, RE_WORDB, RE_NWORDB,
    RE_GROUP, RE_REPEAT, RE_BREF, RE_LOOK, RE_EMPTY
};

typedef struct renode {
    uint8_t  type;
    uint8_t  greedy;
    uint8_t  negate;        /* RE_CLASS / RE_LOOK */
    int      ch;            /* RE_CHAR */
    int      idx;           /* RE_GROUP capture index (-1 = non-capturing),
                               RE_BREF group number */
    int      min, max;      /* RE_REPEAT */
    unsigned char *cls;     /* RE_CLASS: 32-byte bitmap */
    struct renode *sub;     /* first alternative (GROUP/LOOK) or body (REPEAT) */
    struct renode *next;    /* next term in this sequence */
    struct renode *alt;     /* next alternative */
} renode;

typedef struct {
    js_ctx      *ctx;
    const char  *p;
    unsigned long len, pos;
    int          ngroups;
    int          nnodes;
    int          icase;
    int          err;
    int          depth;
} recomp;

#define RE_MAX_GROUPS 32
#define RE_MAX_NODES  4000
#define RE_MAX_DEPTH  64

static renode *re_alt(recomp *c);

static int re_fail(recomp *c, const char *msg)
{
    c->err = 1;
    js_throw_error(c->ctx, JS_ERR_SYNTAX, "invalid regular expression: %s", msg);
    return 0;
}

static renode *re_node(recomp *c, int type)
{
    renode *n;

    if (c->err)
        return 0;
    if (++c->nnodes > RE_MAX_NODES) { re_fail(c, "pattern too large"); return 0; }
    n = (renode *)js_alloc(c->ctx, sizeof(renode));
    if (!n) { c->err = 1; return 0; }
    n->type = (uint8_t)type;
    n->idx = -1;
    n->greedy = 1;
    return n;
}

static void cls_set(unsigned char *b, int ch) { b[(ch & 0xFF) >> 3] |= (unsigned char)(1 << (ch & 7)); }
static int  cls_get(const unsigned char *b, int ch) { return (b[(ch & 0xFF) >> 3] >> (ch & 7)) & 1; }

static void cls_add_ci(unsigned char *b, int ch)
{
    cls_set(b, ch);
    if (ch >= 'a' && ch <= 'z') cls_set(b, ch - 32);
    else if (ch >= 'A' && ch <= 'Z') cls_set(b, ch + 32);
}

static void cls_range(unsigned char *b, int lo, int hi, int icase)
{
    int i;
    if (hi > 255) hi = 255;
    for (i = lo; i <= hi; i++) {
        if (icase) cls_add_ci(b, i);
        else cls_set(b, i);
    }
}

/* \d \D \w \W \s \S applied to a bitmap. Returns 1 if it consumed one. */
static int cls_shorthand(unsigned char *b, int e)
{
    int i;
    switch (e) {
    case 'd': cls_range(b, '0', '9', 0); return 1;
    case 'w':
        cls_range(b, 'a', 'z', 0); cls_range(b, 'A', 'Z', 0);
        cls_range(b, '0', '9', 0); cls_set(b, '_');
        return 1;
    case 's':
        cls_set(b, ' '); cls_set(b, '\t'); cls_set(b, '\n');
        cls_set(b, '\r'); cls_set(b, '\v'); cls_set(b, '\f');
        cls_set(b, 0xA0);
        return 1;
    case 'D': case 'W': case 'S': {
        unsigned char t[32];
        memset(t, 0, 32);
        cls_shorthand(t, e + 32);
        for (i = 0; i < 32; i++) b[i] |= (unsigned char)~t[i];
        return 1;
    }
    default:
        return 0;
    }
}

static int re_hex(recomp *c, int n)
{
    int v = 0, i;

    for (i = 0; i < n; i++) {
        int h, ch;
        if (c->pos >= c->len) return -1;
        ch = (unsigned char)c->p[c->pos];
        if (ch >= '0' && ch <= '9') h = ch - '0';
        else if (ch >= 'a' && ch <= 'f') h = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') h = ch - 'A' + 10;
        else return -1;
        v = v * 16 + h;
        c->pos++;
    }
    return v;
}

/* An escape that produces a single character, or -1 if it is a class
 * shorthand / backreference (which the callers handle). */
static int re_escape_char(recomp *c, int e)
{
    switch (e) {
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return 0;
    case 'x': return re_hex(c, 2);
    case 'u': {
        int v = re_hex(c, 4);
        /* Bytes, not UTF-16: anything above Latin-1 cannot be one char. */
        return v < 0 ? -1 : (v & 0xFF);
    }
    case 'c':
        if (c->pos < c->len) {
            int ch = (unsigned char)c->p[c->pos++];
            return ch % 32;
        }
        return -1;
    default:
        return e;                        /* \. \\ \/ \$ ... are literal */
    }
}

static renode *re_atom(recomp *c)
{
    renode *n;
    int ch;

    if (c->err || c->pos >= c->len)
        return 0;
    ch = (unsigned char)c->p[c->pos];

    if (ch == '(') {
        int idx = -1;
        int look = 0, neg = 0;
        c->pos++;
        if (c->pos + 1 < c->len && c->p[c->pos] == '?') {
            char k = c->p[c->pos + 1];
            if (k == ':') { c->pos += 2; }
            else if (k == '=') { c->pos += 2; look = 1; }
            else if (k == '!') { c->pos += 2; look = 1; neg = 1; }
            else { re_fail(c, "unsupported (? group"); return 0; }
        } else {
            if (c->ngroups >= RE_MAX_GROUPS) { re_fail(c, "too many groups"); return 0; }
            idx = ++c->ngroups;
        }
        n = re_node(c, look ? RE_LOOK : RE_GROUP);
        if (!n) return 0;
        n->idx = idx;
        n->negate = (uint8_t)neg;
        if (++c->depth > RE_MAX_DEPTH) { re_fail(c, "nested too deeply"); return 0; }
        n->sub = re_alt(c);
        c->depth--;
        if (c->err) return 0;
        if (c->pos >= c->len || c->p[c->pos] != ')') { re_fail(c, "missing )"); return 0; }
        c->pos++;
        return n;
    }

    if (ch == '[') {
        unsigned char *b = (unsigned char *)js_alloc(c->ctx, 32);
        int neg = 0;
        if (!b) { c->err = 1; return 0; }
        c->pos++;
        if (c->pos < c->len && c->p[c->pos] == '^') { neg = 1; c->pos++; }
        while (c->pos < c->len && c->p[c->pos] != ']') {
            int lo, hi;
            int esc = 0;
            lo = (unsigned char)c->p[c->pos++];
            if (lo == '\\') {
                if (c->pos >= c->len) { re_fail(c, "trailing backslash"); return 0; }
                {
                    int e = (unsigned char)c->p[c->pos++];
                    if (cls_shorthand(b, e)) continue;
                    if (e == 'b') lo = '\b';
                    else { lo = re_escape_char(c, e); if (lo < 0) { re_fail(c, "bad escape"); return 0; } }
                    esc = 1;
                }
            }
            (void)esc;
            if (c->pos + 1 < c->len && c->p[c->pos] == '-' && c->p[c->pos + 1] != ']') {
                c->pos++;
                hi = (unsigned char)c->p[c->pos++];
                if (hi == '\\') {
                    if (c->pos >= c->len) { re_fail(c, "trailing backslash"); return 0; }
                    {
                        int e = (unsigned char)c->p[c->pos++];
                        if (e == 'b') hi = '\b';
                        else { hi = re_escape_char(c, e); if (hi < 0) { re_fail(c, "bad escape"); return 0; } }
                    }
                }
                if (hi < lo) { re_fail(c, "reversed range in a character class"); return 0; }
                cls_range(b, lo, hi, c->icase);
            } else {
                if (c->icase) cls_add_ci(b, lo); else cls_set(b, lo);
            }
        }
        if (c->pos >= c->len) { re_fail(c, "missing ]"); return 0; }
        c->pos++;
        n = re_node(c, RE_CLASS);
        if (!n) return 0;
        n->cls = b;
        n->negate = (uint8_t)neg;
        return n;
    }

    if (ch == '.') { c->pos++; return re_node(c, RE_ANY); }
    if (ch == '^') { c->pos++; return re_node(c, RE_BOL); }
    if (ch == '$') { c->pos++; return re_node(c, RE_EOL); }

    if (ch == '\\') {
        int e;
        c->pos++;
        if (c->pos >= c->len) { re_fail(c, "trailing backslash"); return 0; }
        e = (unsigned char)c->p[c->pos++];
        if (e == 'b') { return re_node(c, RE_WORDB); }
        if (e == 'B') { return re_node(c, RE_NWORDB); }
        if (e >= '1' && e <= '9') {
            int v = e - '0';
            while (c->pos < c->len && c->p[c->pos] >= '0' && c->p[c->pos] <= '9' &&
                   v * 10 + (c->p[c->pos] - '0') <= RE_MAX_GROUPS)
                v = v * 10 + (c->p[c->pos++] - '0');
            n = re_node(c, RE_BREF);
            if (!n) return 0;
            n->idx = v;
            return n;
        }
        {
            unsigned char t[32];
            memset(t, 0, 32);
            if (cls_shorthand(t, e)) {
                unsigned char *b = (unsigned char *)js_alloc(c->ctx, 32);
                if (!b) { c->err = 1; return 0; }
                memcpy(b, t, 32);
                n = re_node(c, RE_CLASS);
                if (!n) return 0;
                n->cls = b;
                return n;
            }
        }
        {
            int v = re_escape_char(c, e);
            if (v < 0) { re_fail(c, "bad escape"); return 0; }
            n = re_node(c, RE_CHAR);
            if (!n) return 0;
            n->ch = v;
            return n;
        }
    }

    if (ch == ')' || ch == '|')
        return 0;
    if (ch == '*' || ch == '+' || ch == '?') { re_fail(c, "nothing to repeat"); return 0; }

    c->pos++;
    n = re_node(c, RE_CHAR);
    if (!n) return 0;
    n->ch = ch;
    return n;
}

static renode *re_term(recomp *c)
{
    renode *a = re_atom(c);
    int min, max;

    if (!a || c->err)
        return a;
    if (c->pos >= c->len)
        return a;
    switch (c->p[c->pos]) {
    case '*': min = 0; max = -1; c->pos++; break;
    case '+': min = 1; max = -1; c->pos++; break;
    case '?': min = 0; max = 1; c->pos++; break;
    case '{': {
        unsigned long save = c->pos;
        unsigned long i = c->pos + 1;
        int lo = 0, hi, any = 0;
        while (i < c->len && c->p[i] >= '0' && c->p[i] <= '9') { lo = lo * 10 + (c->p[i++] - '0'); any = 1; }
        if (!any) return a;                          /* a literal '{' */
        hi = lo;
        if (i < c->len && c->p[i] == ',') {
            i++;
            if (i < c->len && c->p[i] == '}') hi = -1;
            else { hi = 0; while (i < c->len && c->p[i] >= '0' && c->p[i] <= '9') hi = hi * 10 + (c->p[i++] - '0'); }
        }
        if (i >= c->len || c->p[i] != '}') { c->pos = save; return a; }
        if (hi >= 0 && hi < lo) { re_fail(c, "reversed {n,m} bounds"); return 0; }
        if (lo > 10000 || hi > 10000) { re_fail(c, "{n,m} bound is too large"); return 0; }
        min = lo; max = hi;
        c->pos = i + 1;
        break;
    }
    default:
        return a;
    }
    {
        renode *r = re_node(c, RE_REPEAT);
        if (!r) return 0;
        r->sub = a;
        r->min = min;
        r->max = max;
        if (c->pos < c->len && c->p[c->pos] == '?') { r->greedy = 0; c->pos++; }
        return r;
    }
}

/* One alternative: a chain of terms. */
static renode *re_seq(recomp *c)
{
    renode *head = 0, *tail = 0;

    for (;;) {
        renode *t;
        if (c->err)
            return 0;
        if (c->pos >= c->len || c->p[c->pos] == '|' || c->p[c->pos] == ')')
            break;
        t = re_term(c);
        if (!t) break;
        if (tail) tail->next = t; else head = t;
        tail = t;
    }
    if (c->err)
        return 0;
    if (!head)
        head = re_node(c, RE_EMPTY);
    return head;
}

static renode *re_alt(recomp *c)
{
    renode *head = re_seq(c), *tail = head;

    while (!c->err && c->pos < c->len && c->p[c->pos] == '|') {
        renode *s;
        c->pos++;
        s = re_seq(c);
        if (!s) return 0;
        tail->alt = s;
        tail = s;
    }
    return head;
}

/* ---- matching ---- */

typedef struct recont {
    int             kind;   /* 0 = continue node chain, 1 = repeat, 2 = close */
    struct renode  *node;
    struct recont  *next;
    struct renode  *rep;
    int             count;
    long            mark;
} recont;

typedef struct {
    js_ctx      *ctx;
    const char  *s;
    long         len;
    long        *caps;
    int          ncaps;
    int          icase;
    int          multiline;
    long         steps, budget;
    int          depth, maxdepth;
    int          overrun;
    long         end;
} rectx;

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int char_at_matches(rectx *m, renode *n, long pos)
{
    int c;

    if (pos >= m->len)
        return 0;
    c = (unsigned char)m->s[pos];
    switch (n->type) {
    case RE_CHAR:
        return m->icase ? lower(c) == lower(n->ch) : c == n->ch;
    case RE_ANY:
        return c != '\n' && c != '\r';
    case RE_CLASS:
        return cls_get(n->cls, c) ^ n->negate;
    default:
        return 0;
    }
}

static int is_word(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int re_run(rectx *m, renode *n, recont *k, long pos);
static int re_run_inner(rectx *m, renode *n, recont *k, long pos);

static int re_pop(rectx *m, recont *k, long pos)
{
    if (!k) { m->end = pos; return 1; }
    if (k->kind == 0)
        return re_run(m, k->node, k->next, pos);
    if (k->kind == 2) {
        int i = k->rep->idx;
        long o0 = m->caps[2 * i], o1 = m->caps[2 * i + 1];
        m->caps[2 * i] = k->mark;
        m->caps[2 * i + 1] = pos;
        if (re_run(m, k->node, k->next, pos))
            return 1;
        m->caps[2 * i] = o0;
        m->caps[2 * i + 1] = o1;
        return 0;
    }
    /* kind 1: one iteration of a repeat finished */
    {
        renode *rep = k->rep;
        int count = k->count;
        int more = (rep->max < 0 || count < rep->max) &&
                   !(count > 0 && pos == k->mark);
        recont kk;

        kk.kind = 1; kk.node = 0; kk.next = k->next;
        kk.rep = rep; kk.count = count + 1; kk.mark = pos;
        if (rep->greedy) {
            if (more && re_run(m, rep->sub, &kk, pos))
                return 1;
            if (count >= rep->min)
                return re_run(m, rep->next, k->next, pos);
            return 0;
        }
        if (count >= rep->min && re_run(m, rep->next, k->next, pos))
            return 1;
        if (more)
            return re_run(m, rep->sub, &kk, pos);
        return 0;
    }
}

static int re_run(rectx *m, renode *n, recont *k, long pos)
{
    if (++m->steps > m->budget) { m->overrun = 1; return 0; }
    if (m->depth >= m->maxdepth) { m->overrun = 1; return 0; }
    m->depth++;
    {
        int r = re_run_inner(m, n, k, pos);
        m->depth--;
        return r;
    }
}

static int re_run_inner(rectx *m, renode *n, recont *k, long pos)
{
    if (!n)
        return re_pop(m, k, pos);

    switch (n->type) {
    case RE_EMPTY:
        return re_run(m, n->next, k, pos);

    case RE_CHAR: case RE_ANY: case RE_CLASS:
        /* Consume a whole run of single-character terms in a loop. A
         * literal like /abcdef.../ would otherwise cost one C frame per
         * character matched, which the 64 KiB user stack cannot afford. */
        for (;;) {
            if (!char_at_matches(m, n, pos))
                return 0;
            pos++;
            n = n->next;
            if (!n)
                return re_pop(m, k, pos);
            if (n->type != RE_CHAR && n->type != RE_ANY && n->type != RE_CLASS)
                break;
            if (++m->steps > m->budget) { m->overrun = 1; return 0; }
        }
        return re_run(m, n, k, pos);

    case RE_BOL:
        if (pos == 0) return re_run(m, n->next, k, pos);
        if (m->multiline && (m->s[pos - 1] == '\n' || m->s[pos - 1] == '\r'))
            return re_run(m, n->next, k, pos);
        return 0;

    case RE_EOL:
        if (pos == m->len) return re_run(m, n->next, k, pos);
        if (m->multiline && (m->s[pos] == '\n' || m->s[pos] == '\r'))
            return re_run(m, n->next, k, pos);
        return 0;

    case RE_WORDB: case RE_NWORDB: {
        int a = pos > 0 && is_word((unsigned char)m->s[pos - 1]);
        int b = pos < m->len && is_word((unsigned char)m->s[pos]);
        int at = a != b;
        if (at == (n->type == RE_WORDB))
            return re_run(m, n->next, k, pos);
        return 0;
    }

    case RE_BREF: {
        long a, b, l;
        if (n->idx >= m->ncaps) return 0;
        a = m->caps[2 * n->idx];
        b = m->caps[2 * n->idx + 1];
        if (a < 0 || b < 0) return re_run(m, n->next, k, pos);   /* unset: empty */
        l = b - a;
        if (pos + l > m->len) return 0;
        if (m->icase) {
            long i;
            for (i = 0; i < l; i++)
                if (lower((unsigned char)m->s[pos + i]) != lower((unsigned char)m->s[a + i]))
                    return 0;
        } else if (memcmp(m->s + pos, m->s + a, (unsigned long)l) != 0) {
            return 0;
        }
        return re_run(m, n->next, k, pos + l);
    }

    case RE_LOOK: {
        renode *alt;
        int hit = 0;
        long save_end = m->end;
        for (alt = n->sub; alt && !hit; alt = alt->alt)
            hit = re_run(m, alt, 0, pos);
        m->end = save_end;
        if (m->overrun) return 0;
        if (hit == (int)n->negate)
            return 0;
        return re_run(m, n->next, k, pos);
    }

    case RE_GROUP: {
        renode *alt;
        recont kk;

        if (n->idx < 0) {
            /* non-capturing: just splice the alternatives in */
            kk.kind = 0; kk.node = n->next; kk.next = k;
            kk.rep = 0; kk.count = 0; kk.mark = 0;
            for (alt = n->sub; alt; alt = alt->alt)
                if (re_run(m, alt, &kk, pos)) return 1;
            return 0;
        }
        /* The kind-2 continuation restores this group's own capture when a
         * later term backtracks past it, so no wholesale save is needed --
         * which matters, because a save array on this frame would cost more
         * stack than the 64 KiB user stack can spare at depth. */
        kk.kind = 2; kk.node = n->next; kk.next = k;
        kk.rep = n; kk.count = 0; kk.mark = pos;
        for (alt = n->sub; alt; alt = alt->alt)
            if (re_run(m, alt, &kk, pos)) return 1;
        return 0;
    }

    case RE_REPEAT: {
        renode *sub = n->sub;
        recont kk;

        /* Fast path: a quantifier over one character-consuming term needs
         * no recursion per repetition. This is the case that would
         * otherwise put a stack frame per input byte. */
        if (sub->type == RE_CHAR || sub->type == RE_ANY || sub->type == RE_CLASS) {
            long i = 0, lim = (n->max < 0) ? m->len - pos : n->max;
            while (i < lim && char_at_matches(m, sub, pos + i)) {
                i++;
                if (++m->steps > m->budget) { m->overrun = 1; return 0; }
            }
            if (i < n->min)
                return 0;
            if (n->greedy) {
                for (; i >= n->min; i--)
                    if (re_run(m, n->next, k, pos + i))
                        return 1;
            } else {
                long j;
                for (j = n->min; j <= i; j++)
                    if (re_run(m, n->next, k, pos + j))
                        return 1;
            }
            return 0;
        }
        kk.kind = 1; kk.node = 0; kk.next = k;
        kk.rep = n; kk.count = 1; kk.mark = pos;
        if (n->greedy) {
            if ((n->max < 0 || n->max > 0) && re_run(m, sub, &kk, pos))
                return 1;
            if (n->min == 0)
                return re_run(m, n->next, k, pos);
            return 0;
        }
        if (n->min == 0 && re_run(m, n->next, k, pos))
            return 1;
        return (n->max < 0 || n->max > 0) ? re_run(m, sub, &kk, pos) : 0;
    }

    default:
        return 0;
    }
}

js_regexp *js_regexp_compile(js_ctx *ctx, js_string *src, js_string *flags)
{
    js_regexp *re;
    recomp c;
    renode *root, *grp;
    unsigned long i;

    re = (js_regexp *)js_alloc(ctx, sizeof(js_regexp));
    if (!re)
        return 0;
    re->source = src->len ? src : js_str_newz(ctx, "(?:)");
    re->flags = flags;
    for (i = 0; i < flags->len; i++) {
        switch (flags->data[i]) {
        case 'g': re->global = 1; break;
        case 'i': re->ignore_case = 1; break;
        case 'm': re->multiline = 1; break;
        default:
            js_throw_error(ctx, JS_ERR_SYNTAX,
                           "unsupported regular expression flag '%c'",
                           flags->data[i]);
            return 0;
        }
    }

    memset(&c, 0, sizeof(c));
    c.ctx = ctx;
    c.p = src->data;
    c.len = src->len;
    c.icase = re->ignore_case;
    root = re_alt(&c);
    if (c.err || !root)
        return 0;
    if (c.pos != c.len) {
        js_throw_error(ctx, JS_ERR_SYNTAX,
                       "invalid regular expression: unbalanced )");
        return 0;
    }
    grp = re_node(&c, RE_GROUP);
    if (!grp)
        return 0;
    grp->idx = 0;
    grp->sub = root;
    re->prog = grp;
    re->ngroups = c.ngroups + 1;
    return re;
}

int js_regexp_exec_raw(js_ctx *ctx, js_regexp *re, const char *s,
                       long len, long start, long *caps)
{
    rectx m;
    long at;
    int i;

    if (start < 0) start = 0;
    if (start > len) return 0;

    memset(&m, 0, sizeof(m));
    m.ctx = ctx;
    m.s = s;
    m.len = len;
    m.caps = caps;
    m.ncaps = re->ngroups;
    m.icase = re->ignore_case;
    m.multiline = re->multiline;
    m.budget = 200000;
    m.maxdepth = 48;      /* ~11 KiB of C stack; see the note in js.h */

    for (at = start; at <= len; at++) {
        for (i = 0; i < 2 * re->ngroups; i++)
            caps[i] = -1;
        m.steps = 0;
        m.depth = 0;
        m.overrun = 0;
        if (re_run(&m, (renode *)re->prog, 0, at)) {
            if (js_step(ctx) != JS_OK)
                return -1;
            return 1;
        }
        if (m.overrun) {
            js_throw_error(ctx, JS_ERR_RANGE,
                           "regular expression exceeded its matching budget");
            return -1;
        }
        if (js_step(ctx) != JS_OK)
            return -1;
    }
    return 0;
}

/* ================================================================== */
/* Receiver and array-like helpers                                     */
/* ================================================================== */

static int this_string(js_ctx *ctx, js_value t, js_string **out)
{
    js_value s;

    if (t.type == JS_OBJECT && t.u.obj->cls == JS_CLASS_STRING)
        t = t.u.obj->prim;
    if (t.type == JS_UNDEFINED || t.type == JS_NULL)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "String.prototype method called on %s",
                              t.type == JS_NULL ? "null" : "undefined");
    if (js_to_string(ctx, t, &s) != JS_OK)
        return JS_THROW;
    *out = s.u.str;
    return JS_OK;
}

static int this_number(js_ctx *ctx, js_value t, double *out)
{
    if (t.type == JS_OBJECT && t.u.obj->cls == JS_CLASS_NUMBER)
        t = t.u.obj->prim;
    if (t.type != JS_NUMBER)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "Number.prototype method called on a non-number");
    *out = t.u.num;
    return JS_OK;
}

static int this_object(js_ctx *ctx, js_value t, js_object **out)
{
    js_value o;

    if (js_to_object(ctx, t, &o) != JS_OK)
        return JS_THROW;
    *out = o.u.obj;
    return JS_OK;
}

/* Array-like access that stays fast on real arrays but still works on
 * `arguments`, strings and plain objects, as ES5 requires. */
static int alen(js_ctx *ctx, js_object *o, unsigned long *n)
{
    js_value v;

    if (o->cls == JS_CLASS_ARRAY) { *n = o->elen; return JS_OK; }
    if (js_obj_get(ctx, o, ctx->s_length, js_object_value(o), &v) != JS_OK)
        return JS_THROW;
    {
        uint32_t u;
        if (js_to_uint32(ctx, v, &u) != JS_OK) return JS_THROW;
        *n = u;
    }
    return JS_OK;
}

static js_string *idx_key(js_ctx *ctx, unsigned long i)
{
    char buf[24], tmp[24];
    int m = 0, t = 0;

    if (i == 0) buf[m++] = '0';
    else {
        while (i) { tmp[t++] = (char)('0' + i % 10); i /= 10; }
        while (t) buf[m++] = tmp[--t];
    }
    return js_str_intern(ctx, buf, (unsigned long)m);
}

static int aget(js_ctx *ctx, js_object *o, unsigned long i, js_value *v)
{
    js_string *k;

    if (o->cls == JS_CLASS_ARRAY && i < o->elen) { *v = o->elems[i]; return JS_OK; }
    k = idx_key(ctx, i);
    if (!k) return JS_THROW;
    return js_obj_get(ctx, o, k, js_object_value(o), v);
}

static int aput(js_ctx *ctx, js_object *o, unsigned long i, js_value v)
{
    js_string *k;

    if (o->cls == JS_CLASS_ARRAY && i < o->elen) { o->elems[i] = v; return JS_OK; }
    k = idx_key(ctx, i);
    if (!k) return JS_THROW;
    return js_obj_put(ctx, o, k, v);
}

static int asetlen(js_ctx *ctx, js_object *o, unsigned long n)
{
    return js_obj_put(ctx, o, ctx->s_length, js_number((double)n));
}

/* ================================================================== */
/* Object                                                              */
/* ================================================================== */

static const char *class_name(int cls)
{
    switch (cls) {
    case JS_CLASS_ARRAY:     return "Array";
    case JS_CLASS_FUNCTION:  return "Function";
    case JS_CLASS_ERROR:     return "Error";
    case JS_CLASS_DATE:      return "Date";
    case JS_CLASS_REGEXP:    return "RegExp";
    case JS_CLASS_STRING:    return "String";
    case JS_CLASS_NUMBER:    return "Number";
    case JS_CLASS_BOOLEAN:   return "Boolean";
    case JS_CLASS_ARGUMENTS: return "Arguments";
    case JS_CLASS_MATH:      return "Math";
    case JS_CLASS_JSON:      return "JSON";
    case JS_CLASS_PROMISE:   return "Promise";
    case JS_CLASS_ARRAYBUFFER:return "ArrayBuffer";
    default:                 return "Object";
    }
}

static int bi_obj_tostring(js_ctx *ctx, js_value t, int argc, js_value *argv,
                           js_value *ret)
{
    char buf[48];

    (void)argc; (void)argv;
    if (t.type == JS_UNDEFINED) return ret_str(ctx, "[object Undefined]", 18, ret);
    if (t.type == JS_NULL)      return ret_str(ctx, "[object Null]", 13, ret);
    if (t.type != JS_OBJECT) {
        const char *n = t.type == JS_STRING ? "String"
                      : t.type == JS_NUMBER ? "Number" : "Boolean";
        snprintf(buf, sizeof(buf), "[object %s]", n);
        return ret_str(ctx, buf, strlen(buf), ret);
    }
    snprintf(buf, sizeof(buf), "[object %s]", class_name(t.u.obj->cls));
    return ret_str(ctx, buf, strlen(buf), ret);
}

static int bi_obj_valueof(js_ctx *ctx, js_value t, int argc, js_value *argv,
                          js_value *ret)
{
    (void)argc; (void)argv;
    return js_to_object(ctx, t, ret);
}

static int bi_obj_hasown(js_ctx *ctx, js_value t, int argc, js_value *argv,
                         js_value *ret)
{
    js_value k;
    js_object *o;

    if (js_to_string(ctx, arg(argc, argv, 0), &k) != JS_OK) return JS_THROW;
    if (this_object(ctx, t, &o) != JS_OK) return JS_THROW;
    *ret = js_bool(js_obj_has_own(ctx, o, k.u.str));
    return JS_OK;
}

static int bi_obj_isproto(js_ctx *ctx, js_value t, int argc, js_value *argv,
                          js_value *ret)
{
    js_value v = arg(argc, argv, 0);
    js_object *o, *p;
    int guard = 0;

    *ret = js_bool(0);
    if (v.type != JS_OBJECT) return JS_OK;
    if (this_object(ctx, t, &o) != JS_OK) return JS_THROW;
    for (p = v.u.obj->proto; p && guard < 1000; p = p->proto, guard++)
        if (p == o) { *ret = js_bool(1); return JS_OK; }
    return JS_OK;
}

static int bi_obj_propenum(js_ctx *ctx, js_value t, int argc, js_value *argv,
                           js_value *ret)
{
    js_value k;
    js_object *o;
    js_prop *p;
    long i;

    if (js_to_string(ctx, arg(argc, argv, 0), &k) != JS_OK) return JS_THROW;
    if (this_object(ctx, t, &o) != JS_OK) return JS_THROW;
    i = js_array_index(k.u.str);
    if (o->cls == JS_CLASS_ARRAY && i >= 0 && (uint32_t)i < o->elen) {
        *ret = js_bool(1);
        return JS_OK;
    }
    p = js_own_prop(o, k.u.str);
    *ret = js_bool(p && (p->flags & JS_P_ENUM));
    return JS_OK;
}

static int own_names(js_ctx *ctx, js_value v, int only_enum, js_value *ret)
{
    js_object *o, *a;
    uint32_t i;

    if (v.type != JS_OBJECT)
        return js_throw_error(ctx, JS_ERR_TYPE, "argument is not an object");
    o = v.u.obj;
    a = js_new_array(ctx);
    if (!a) return JS_THROW;
    if (o->cls == JS_CLASS_ARRAY) {
        for (i = 0; i < o->elen; i++) {
            js_string *k = idx_key(ctx, i);
            if (!k || js_array_push(ctx, a, js_string_value(k)) != JS_OK)
                return JS_THROW;
        }
        if (!only_enum &&
            js_array_push(ctx, a, js_string_value(ctx->s_length)) != JS_OK)
            return JS_THROW;
    }
    for (i = 0; i < o->nprops; i++) {
        js_prop *p = &o->props[i];
        if (p->flags & JS_P_DEAD) continue;
        if (only_enum && !(p->flags & JS_P_ENUM)) continue;
        if (js_array_push(ctx, a, js_string_value(p->key)) != JS_OK)
            return JS_THROW;
    }
    *ret = js_object_value(a);
    return JS_OK;
}

static int bi_object_keys(js_ctx *ctx, js_value t, int argc, js_value *argv,
                          js_value *ret)
{
    (void)t;
    return own_names(ctx, arg(argc, argv, 0), 1, ret);
}

static int bi_object_names(js_ctx *ctx, js_value t, int argc, js_value *argv,
                           js_value *ret)
{
    (void)t;
    return own_names(ctx, arg(argc, argv, 0), 0, ret);
}

static int bi_object_getproto(js_ctx *ctx, js_value t, int argc, js_value *argv,
                              js_value *ret)
{
    js_value v = arg(argc, argv, 0);

    (void)t;
    if (v.type != JS_OBJECT)
        return js_throw_error(ctx, JS_ERR_TYPE, "argument is not an object");
    *ret = v.u.obj->proto ? js_object_value(v.u.obj->proto) : js_null();
    return JS_OK;
}

static int define_one(js_ctx *ctx, js_object *o, js_string *key, js_value desc)
{
    js_value v;
    js_prop *p;
    uint32_t flags = 0;
    int is_acc = 0;
    js_value getter = js_undefined(), setter = js_undefined();

    if (desc.type != JS_OBJECT)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "property descriptor must be an object");
    if (js_obj_get(ctx, desc.u.obj, ctx->s_get, desc, &getter) != JS_OK) return JS_THROW;
    if (js_obj_get(ctx, desc.u.obj, ctx->s_set, desc, &setter) != JS_OK) return JS_THROW;
    if (getter.type != JS_UNDEFINED || setter.type != JS_UNDEFINED)
        is_acc = 1;
    if (js_obj_get(ctx, desc.u.obj, ctx->s_enumerable, desc, &v) != JS_OK) return JS_THROW;
    if (js_to_boolean(v)) flags |= JS_P_ENUM;
    if (js_obj_get(ctx, desc.u.obj, ctx->s_configurable, desc, &v) != JS_OK) return JS_THROW;
    if (js_to_boolean(v)) flags |= JS_P_CONFIG;
    if (js_obj_get(ctx, desc.u.obj, ctx->s_writable, desc, &v) != JS_OK) return JS_THROW;
    if (js_to_boolean(v)) flags |= JS_P_WRITE;

    /* An array index would otherwise land in the dense element vector,
     * where per-property attributes have nowhere to live. */
    if (o->cls == JS_CLASS_ARRAY && js_array_index(key) >= 0 && !is_acc) {
        if (js_obj_get(ctx, desc.u.obj, ctx->s_value, desc, &v) != JS_OK) return JS_THROW;
        return js_obj_put(ctx, o, key, v);
    }
    p = js_add_prop(ctx, o, key, flags);
    if (!p) return JS_THROW;
    p->flags = flags | (is_acc ? (uint32_t)JS_P_ACCESSOR : 0u);
    if (is_acc) {
        p->value = getter;
        p->setter = setter;
    } else {
        if (js_obj_get(ctx, desc.u.obj, ctx->s_value, desc, &v) != JS_OK) return JS_THROW;
        p->value = v;
        p->setter = js_undefined();
    }
    return JS_OK;
}

static int bi_object_defprops(js_ctx *ctx, js_value t, int argc, js_value *argv,
                              js_value *ret);

static int bi_object_defprop(js_ctx *ctx, js_value t, int argc, js_value *argv,
                             js_value *ret)
{
    js_value o = arg(argc, argv, 0), k;

    (void)t;
    if (o.type != JS_OBJECT)
        return js_throw_error(ctx, JS_ERR_TYPE, "argument is not an object");
    if (js_to_string(ctx, arg(argc, argv, 1), &k) != JS_OK) return JS_THROW;
    if (define_one(ctx, o.u.obj, k.u.str, arg(argc, argv, 2)) != JS_OK)
        return JS_THROW;
    *ret = o;
    return JS_OK;
}

static int bi_object_defprops(js_ctx *ctx, js_value t, int argc, js_value *argv,
                              js_value *ret)
{
    js_value o = arg(argc, argv, 0), d = arg(argc, argv, 1);
    uint32_t i;

    (void)t;
    if (o.type != JS_OBJECT || d.type != JS_OBJECT)
        return js_throw_error(ctx, JS_ERR_TYPE, "argument is not an object");
    for (i = 0; i < d.u.obj->nprops; i++) {
        js_prop *p = &d.u.obj->props[i];
        js_value desc;
        if ((p->flags & JS_P_DEAD) || !(p->flags & JS_P_ENUM)) continue;
        if (js_obj_get(ctx, d.u.obj, p->key, d, &desc) != JS_OK) return JS_THROW;
        if (define_one(ctx, o.u.obj, p->key, desc) != JS_OK) return JS_THROW;
    }
    *ret = o;
    return JS_OK;
}

static int bi_object_create(js_ctx *ctx, js_value t, int argc, js_value *argv,
                            js_value *ret)
{
    js_value pv = arg(argc, argv, 0);
    js_object *o;

    (void)t;
    if (pv.type != JS_OBJECT && pv.type != JS_NULL)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "Object.create needs an object or null");
    o = js_obj_alloc(ctx, JS_CLASS_OBJECT,
                     pv.type == JS_OBJECT ? pv.u.obj : 0);
    if (!o) return JS_THROW;
    *ret = js_object_value(o);
    if (argc > 1 && argv[1].type == JS_OBJECT) {
        js_value two[2];
        two[0] = *ret;
        two[1] = argv[1];
        return bi_object_defprops(ctx, t, 2, two, ret);
    }
    return JS_OK;
}

static int bi_object_getdesc(js_ctx *ctx, js_value t, int argc, js_value *argv,
                             js_value *ret)
{
    js_value ov = arg(argc, argv, 0), k;
    js_object *d;
    js_prop *p;

    (void)t;
    *ret = js_undefined();
    if (ov.type != JS_OBJECT)
        return js_throw_error(ctx, JS_ERR_TYPE, "argument is not an object");
    if (js_to_string(ctx, arg(argc, argv, 1), &k) != JS_OK) return JS_THROW;
    p = js_own_prop(ov.u.obj, k.u.str);
    if (!p) {
        long i = js_array_index(k.u.str);
        if (ov.u.obj->cls == JS_CLASS_ARRAY && i >= 0 &&
            (uint32_t)i < ov.u.obj->elen) {
            d = js_new_object(ctx);
            if (!d) return JS_THROW;
            if (js_define_enum(ctx, d, "value", ov.u.obj->elems[i]) != JS_OK ||
                js_define_enum(ctx, d, "writable", js_bool(1)) != JS_OK ||
                js_define_enum(ctx, d, "enumerable", js_bool(1)) != JS_OK ||
                js_define_enum(ctx, d, "configurable", js_bool(1)) != JS_OK)
                return JS_THROW;
            *ret = js_object_value(d);
        }
        return JS_OK;
    }
    d = js_new_object(ctx);
    if (!d) return JS_THROW;
    if (p->flags & JS_P_ACCESSOR) {
        if (js_define_enum(ctx, d, "get", p->value) != JS_OK ||
            js_define_enum(ctx, d, "set", p->setter) != JS_OK)
            return JS_THROW;
    } else {
        if (js_define_enum(ctx, d, "value", p->value) != JS_OK ||
            js_define_enum(ctx, d, "writable",
                           js_bool((p->flags & JS_P_WRITE) != 0)) != JS_OK)
            return JS_THROW;
    }
    if (js_define_enum(ctx, d, "enumerable",
                       js_bool((p->flags & JS_P_ENUM) != 0)) != JS_OK ||
        js_define_enum(ctx, d, "configurable",
                       js_bool((p->flags & JS_P_CONFIG) != 0)) != JS_OK)
        return JS_THROW;
    *ret = js_object_value(d);
    return JS_OK;
}

static int lock_object(js_ctx *ctx, js_value v, int freeze, js_value *ret)
{
    uint32_t i;

    (void)ctx;
    *ret = v;
    if (v.type != JS_OBJECT)
        return JS_OK;
    v.u.obj->extensible = 0;
    for (i = 0; i < v.u.obj->nprops; i++) {
        js_prop *p = &v.u.obj->props[i];
        if (p->flags & JS_P_DEAD) continue;
        p->flags &= ~(uint32_t)JS_P_CONFIG;
        if (freeze && !(p->flags & JS_P_ACCESSOR))
            p->flags &= ~(uint32_t)JS_P_WRITE;
    }
    return JS_OK;
}

static int bi_object_freeze(js_ctx *ctx, js_value t, int argc, js_value *argv,
                            js_value *ret)
{ (void)t; return lock_object(ctx, arg(argc, argv, 0), 1, ret); }

static int bi_object_seal(js_ctx *ctx, js_value t, int argc, js_value *argv,
                          js_value *ret)
{ (void)t; return lock_object(ctx, arg(argc, argv, 0), 0, ret); }

static int bi_object_prevext(js_ctx *ctx, js_value t, int argc, js_value *argv,
                             js_value *ret)
{
    js_value v = arg(argc, argv, 0);
    (void)ctx; (void)t;
    if (v.type == JS_OBJECT) v.u.obj->extensible = 0;
    *ret = v;
    return JS_OK;
}

static int bi_object_isext(js_ctx *ctx, js_value t, int argc, js_value *argv,
                           js_value *ret)
{
    js_value v = arg(argc, argv, 0);
    (void)ctx; (void)t;
    *ret = js_bool(v.type == JS_OBJECT && v.u.obj->extensible);
    return JS_OK;
}

static int is_locked(js_value v, int frozen)
{
    uint32_t i;

    if (v.type != JS_OBJECT) return 1;
    if (v.u.obj->extensible) return 0;
    if (frozen && v.u.obj->cls == JS_CLASS_ARRAY && v.u.obj->elen) return 0;
    for (i = 0; i < v.u.obj->nprops; i++) {
        js_prop *p = &v.u.obj->props[i];
        if (p->flags & JS_P_DEAD) continue;
        if (p->flags & JS_P_CONFIG) return 0;
        if (frozen && !(p->flags & JS_P_ACCESSOR) && (p->flags & JS_P_WRITE))
            return 0;
    }
    return 1;
}

static int bi_object_isfrozen(js_ctx *ctx, js_value t, int argc, js_value *argv,
                              js_value *ret)
{ (void)ctx; (void)t; *ret = js_bool(is_locked(arg(argc, argv, 0), 1)); return JS_OK; }

static int bi_object_issealed(js_ctx *ctx, js_value t, int argc, js_value *argv,
                              js_value *ret)
{ (void)ctx; (void)t; *ret = js_bool(is_locked(arg(argc, argv, 0), 0)); return JS_OK; }

static int bi_object_ctor(js_ctx *ctx, js_value t, int argc, js_value *argv,
                          js_value *ret)
{
    js_value v = arg(argc, argv, 0);

    (void)t;
    if (v.type == JS_UNDEFINED || v.type == JS_NULL) {
        js_object *o = js_new_object(ctx);
        if (!o) return JS_THROW;
        *ret = js_object_value(o);
        return JS_OK;
    }
    return js_to_object(ctx, v, ret);
}

/* ================================================================== */
/* Function.prototype                                                  */
/* ================================================================== */

static int bi_fn_call(js_ctx *ctx, js_value t, int argc, js_value *argv,
                      js_value *ret)
{
    return js_call(ctx, t, arg(argc, argv, 0),
                   argc > 1 ? argc - 1 : 0, argc > 1 ? argv + 1 : 0, ret);
}

static int bi_fn_apply(js_ctx *ctx, js_value t, int argc, js_value *argv,
                       js_value *ret)
{
    js_value list = arg(argc, argv, 1);
    js_value *args;
    unsigned long n = 0, i;
    int r;

    if (list.type == JS_UNDEFINED || list.type == JS_NULL)
        return js_call(ctx, t, arg(argc, argv, 0), 0, 0, ret);
    if (list.type != JS_OBJECT)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "the second argument to apply must be an array");
    if (alen(ctx, list.u.obj, &n) != JS_OK) return JS_THROW;
    if (n > JS_MAXARGS)
        return js_throw_error(ctx, JS_ERR_RANGE,
                              "apply with more than %d arguments", JS_MAXARGS);
    if (ctx->argsp + n > JS_ARGSTACK)
        return js_throw_error(ctx, JS_ERR_RANGE, "argument stack exhausted");
    args = ctx->argstack + ctx->argsp;
    ctx->argsp += (uint32_t)n;
    for (i = 0; i < n; i++)
        if (aget(ctx, list.u.obj, i, &args[i]) != JS_OK) {
            ctx->argsp -= (uint32_t)n;
            return JS_THROW;
        }
    r = js_call(ctx, t, arg(argc, argv, 0), (int)n, args, ret);
    ctx->argsp -= (uint32_t)n;
    return r;
}

/* Never reached: js_call_func intercepts bound functions before dispatch. */
static int bi_fn_bound(js_ctx *ctx, js_value t, int argc, js_value *argv,
                       js_value *ret)
{
    (void)t; (void)argc; (void)argv; (void)ret;
    return js_throw_error(ctx, JS_ERR_TYPE, "bound function called directly");
}

static int bi_fn_bind(js_ctx *ctx, js_value t, int argc, js_value *argv,
                      js_value *ret)
{
    js_object *f;
    js_func *fn;
    int i, n = argc > 1 ? argc - 1 : 0;

    if (!js_is_function(t))
        return js_throw_error(ctx, JS_ERR_TYPE, "bind called on a non-function");
    f = js_new_native(ctx, bi_fn_bound, "bound", 0);
    if (!f) return JS_THROW;
    fn = f->fn;
    fn->bound_target = t.u.obj;
    fn->bound_this = arg(argc, argv, 0);
    fn->nbound = n;
    fn->ctor_kind = t.u.obj->fn->ctor_kind;
    if (n) {
        fn->bound_args = (js_value *)js_alloc(ctx,
                             (unsigned long)n * sizeof(js_value));
        if (!fn->bound_args) return JS_THROW;
        for (i = 0; i < n; i++)
            fn->bound_args[i] = argv[i + 1];
    }
    *ret = js_object_value(f);
    return JS_OK;
}

static int bi_fn_tostring(js_ctx *ctx, js_value t, int argc, js_value *argv,
                          js_value *ret)
{
    char buf[128];

    (void)argc; (void)argv;
    if (!js_is_function(t))
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "Function.prototype.toString on a non-function");
    snprintf(buf, sizeof(buf), "function %s() { [native code] }",
             t.u.obj->fn->name ? t.u.obj->fn->name->data : "");
    return ret_str(ctx, buf, strlen(buf), ret);
}

/* ================================================================== */
/* Array                                                               */
/* ================================================================== */

static int bi_array_ctor(js_ctx *ctx, js_value t, int argc, js_value *argv,
                         js_value *ret)
{
    js_object *a = js_new_array(ctx);
    int i;

    (void)t;
    if (!a) return JS_THROW;
    if (argc == 1 && argv[0].type == JS_NUMBER) {
        double d = argv[0].u.num;
        uint32_t n;
        if (d < 0 || d != js_floor(d) || d > 4294967295.0)
            return js_throw_error(ctx, JS_ERR_RANGE, "invalid array length");
        n = (uint32_t)d;
        *ret = js_object_value(a);
        return js_obj_put(ctx, a, ctx->s_length, js_number((double)n));
    }
    for (i = 0; i < argc; i++)
        if (js_array_push(ctx, a, argv[i]) != JS_OK) return JS_THROW;
    *ret = js_object_value(a);
    return JS_OK;
}

static int bi_array_isarray(js_ctx *ctx, js_value t, int argc, js_value *argv,
                            js_value *ret)
{
    (void)ctx; (void)t;
    *ret = js_bool(js_is_array(arg(argc, argv, 0)));
    return JS_OK;
}

/* ES5 relative-index rule: negative counts back from the end. */
static unsigned long rel_index(double d, unsigned long len)
{
    if (js_isnan(d)) return 0;
    if (d < 0) {
        d += (double)len;
        if (d < 0) return 0;
    }
    if (d > (double)len) return len;
    return (unsigned long)d;
}

static int bi_array_join(js_ctx *ctx, js_value t, int argc, js_value *argv,
                         js_value *ret)
{
    js_object *o;
    unsigned long n, i;
    js_sbuf b;
    js_string *sep;

    if (this_object(ctx, t, &o) != JS_OK) return JS_THROW;
    if (alen(ctx, o, &n) != JS_OK) return JS_THROW;
    if (argc > 0 && argv[0].type != JS_UNDEFINED) {
        js_value s;
        if (js_to_string(ctx, argv[0], &s) != JS_OK) return JS_THROW;
        sep = s.u.str;
    } else {
        sep = js_str_newz(ctx, ",");
        if (!sep) return JS_THROW;
    }
    js_sb_init(&b, ctx);
    for (i = 0; i < n; i++) {
        js_value v, s;
        if (i && js_sb_put(&b, sep->data, sep->len) != JS_OK) goto fail;
        if (aget(ctx, o, i, &v) != JS_OK) goto fail;
        if (v.type == JS_UNDEFINED || v.type == JS_NULL) continue;
        if (js_to_string(ctx, v, &s) != JS_OK) goto fail;
        if (js_sb_put(&b, s.u.str->data, s.u.str->len) != JS_OK) goto fail;
    }
    return js_sb_finish(&b, ret);
fail:
    js_sb_free(&b);
    return JS_THROW;
}

static int bi_array_tostring(js_ctx *ctx, js_value t, int argc, js_value *argv,
                             js_value *ret)
{
    (void)argc; (void)argv;
    return bi_array_join(ctx, t, 0, 0, ret);
}

static int bi_array_push(js_ctx *ctx, js_value t, int argc, js_value *argv,
                         js_value *ret)
{
    js_object *o;
    unsigned long n;
    int i;

    if (this_object(ctx, t, &o) != JS_OK) return JS_THROW;
    if (alen(ctx, o, &n) != JS_OK) return JS_THROW;
    for (i = 0; i < argc; i++) {
        if (o->cls == JS_CLASS_ARRAY) {
            if (js_array_push(ctx, o, argv[i]) != JS_OK) return JS_THROW;
        } else {
            if (aput(ctx, o, n + (unsigned long)i, argv[i]) != JS_OK) return JS_THROW;
        }
    }
    n += (unsigned long)argc;
    if (o->cls != JS_CLASS_ARRAY && asetlen(ctx, o, n) != JS_OK) return JS_THROW;
    *ret = js_number((double)n);
    return JS_OK;
}

static int bi_array_pop(js_ctx *ctx, js_value t, int argc, js_value *argv,
                        js_value *ret)
{
    js_object *o;
    unsigned long n;

    (void)argc; (void)argv;
    if (this_object(ctx, t, &o) != JS_OK) return JS_THROW;
    if (alen(ctx, o, &n) != JS_OK) return JS_THROW;
    if (n == 0) {
        *ret = js_undefined();
        return o->cls == JS_CLASS_ARRAY ? JS_OK : asetlen(ctx, o, 0);
    }
    if (aget(ctx, o, n - 1, ret) != JS_OK) return JS_THROW;
    if (o->cls == JS_CLASS_ARRAY) o->elen = (uint32_t)(n - 1);
    else {
        js_string *k = idx_key(ctx, n - 1);
        if (!k) return JS_THROW;
        js_obj_delete(ctx, o, k);
        if (asetlen(ctx, o, n - 1) != JS_OK) return JS_THROW;
    }
    return JS_OK;
}

static int bi_array_shift(js_ctx *ctx, js_value t, int argc, js_value *argv,
                          js_value *ret)
{
    js_object *o;
    unsigned long n, i;

    (void)argc; (void)argv;
    if (this_object(ctx, t, &o) != JS_OK) return JS_THROW;
    if (alen(ctx, o, &n) != JS_OK) return JS_THROW;
    if (n == 0) { *ret = js_undefined(); return JS_OK; }
    if (aget(ctx, o, 0, ret) != JS_OK) return JS_THROW;
    if (o->cls == JS_CLASS_ARRAY) {
        memmove(o->elems, o->elems + 1, (unsigned long)(n - 1) * sizeof(js_value));
        o->elen = (uint32_t)(n - 1);
        return JS_OK;
    }
    for (i = 1; i < n; i++) {
        js_value v;
        if (aget(ctx, o, i, &v) != JS_OK) return JS_THROW;
        if (aput(ctx, o, i - 1, v) != JS_OK) return JS_THROW;
    }
    return asetlen(ctx, o, n - 1);
}

static int bi_array_unshift(js_ctx *ctx, js_value t, int argc, js_value *argv,
                            js_value *ret)
{
    js_object *o;
    unsigned long n, i;

    if (this_object(ctx, t, &o) != JS_OK) return JS_THROW;
    if (alen(ctx, o, &n) != JS_OK) return JS_THROW;
    if (argc > 0) {
        for (i = 0; i < (unsigned long)argc; i++)
            if (aput(ctx, o, n + i, js_undefined()) != JS_OK) return JS_THROW;
        if (o->cls != JS_CLASS_ARRAY && asetlen(ctx, o, n + (unsigned long)argc) != JS_OK)
            return JS_THROW;
        for (i = n; i > 0; i--) {
            js_value v;
            if (aget(ctx, o, i - 1, &v) != JS_OK) return JS_THROW;
            if (aput(ctx, o, i - 1 + (unsigned long)argc, v) != JS_OK) return JS_THROW;
        }
        for (i = 0; i < (unsigned long)argc; i++)
            if (aput(ctx, o, i, argv[i]) != JS_OK) return JS_THROW;
    }
    *ret = js_number((double)(n + (unsigned long)argc));
    return JS_OK;
}

static int bi_array_slice(js_ctx *ctx, js_value t, int argc, js_value *argv,
                          js_value *ret)
{
    js_object *o, *a;
    unsigned long n, from, to, i;
    double d;

    if (this_object(ctx, t, &o) != JS_OK) return JS_THROW;
    if (alen(ctx, o, &n) != JS_OK) return JS_THROW;
    if (js_to_integer(ctx, arg(argc, argv, 0), &d) != JS_OK) return JS_THROW;
    from = rel_index(d, n);
    if (argc > 1 && argv[1].type != JS_UNDEFINED) {
        if (js_to_integer(ctx, argv[1], &d) != JS_OK) return JS_THROW;
        to = rel_index(d, n);
    } else {
        to = n;
    }
    a = js_new_array(ctx);
    if (!a) return JS_THROW;
    for (i = from; i < to; i++) {
        js_value v;
        if (aget(ctx, o, i, &v) != JS_OK) return JS_THROW;
        if (js_array_push(ctx, a, v) != JS_OK) return JS_THROW;
    }
    *ret = js_object_value(a);
    return JS_OK;
}

static int bi_array_splice(js_ctx *ctx, js_value t, int argc, js_value *argv,
                           js_value *ret)
{
    js_object *o, *removed;
    unsigned long n, start, dcount, ins, i;
    double d;

    if (this_object(ctx, t, &o) != JS_OK) return JS_THROW;
    if (alen(ctx, o, &n) != JS_OK) return JS_THROW;
    if (js_to_integer(ctx, arg(argc, argv, 0), &d) != JS_OK) return JS_THROW;
    start = rel_index(d, n);
    if (argc >= 2) {
        if (js_to_integer(ctx, argv[1], &d) != JS_OK) return JS_THROW;
        if (d < 0) d = 0;
        if (d > (double)(n - start)) d = (double)(n - start);
        dcount = (unsigned long)d;
    } else {
        dcount = argc ? n - start : 0;
    }
    ins = argc > 2 ? (unsigned long)(argc - 2) : 0;

    removed = js_new_array(ctx);
    if (!removed) return JS_THROW;
    for (i = 0; i < dcount; i++) {
        js_value v;
        if (aget(ctx, o, start + i, &v) != JS_OK) return JS_THROW;
        if (js_array_push(ctx, removed, v) != JS_OK) return JS_THROW;
    }

    if (ins > dcount) {
        unsigned long grow = ins - dcount;
        for (i = 0; i < grow; i++)
            if (aput(ctx, o, n + i, js_undefined()) != JS_OK) return JS_THROW;
        if (o->cls != JS_CLASS_ARRAY && asetlen(ctx, o, n + grow) != JS_OK)
            return JS_THROW;
        for (i = n; i > start + dcount; i--) {
            js_value v;
            if (aget(ctx, o, i - 1, &v) != JS_OK) return JS_THROW;
            if (aput(ctx, o, i - 1 + grow, v) != JS_OK) return JS_THROW;
        }
    } else if (ins < dcount) {
        unsigned long shrink = dcount - ins;
        for (i = start + dcount; i < n; i++) {
            js_value v;
            if (aget(ctx, o, i, &v) != JS_OK) return JS_THROW;
            if (aput(ctx, o, i - shrink, v) != JS_OK) return JS_THROW;
        }
        if (o->cls == JS_CLASS_ARRAY) o->elen = (uint32_t)(n - shrink);
        else if (asetlen(ctx, o, n - shrink) != JS_OK) return JS_THROW;
    }
    for (i = 0; i < ins; i++)
        if (aput(ctx, o, start + i, argv[i + 2]) != JS_OK) return JS_THROW;
    *ret = js_object_value(removed);
    return JS_OK;
}

static int bi_array_concat(js_ctx *ctx, js_value t, int argc, js_value *argv,
                           js_value *ret)
{
    js_object *a = js_new_array(ctx);
    int k;

    if (!a) return JS_THROW;
    for (k = -1; k < argc; k++) {
        js_value v = (k < 0) ? t : argv[k];
        if (js_is_array(v)) {
            unsigned long n, i;
            if (alen(ctx, v.u.obj, &n) != JS_OK) return JS_THROW;
            for (i = 0; i < n; i++) {
                js_value e;
                if (aget(ctx, v.u.obj, i, &e) != JS_OK) return JS_THROW;
                if (js_array_push(ctx, a, e) != JS_OK) return JS_THROW;
            }
        } else {
            if (js_array_push(ctx, a, v) != JS_OK) return JS_THROW;
        }
    }
    *ret = js_object_value(a);
    return JS_OK;
}

static int bi_array_indexof(js_ctx *ctx, js_value t, int argc, js_value *argv,
                            js_value *ret)
{
    js_object *o;
    unsigned long n, i;
    double d = 0;
    js_value needle = arg(argc, argv, 0);

    if (this_object(ctx, t, &o) != JS_OK) return JS_THROW;
    if (alen(ctx, o, &n) != JS_OK) return JS_THROW;
    *ret = js_number(-1);
    if (n == 0) return JS_OK;
    if (argc > 1 && js_to_integer(ctx, argv[1], &d) != JS_OK) return JS_THROW;
    if (d < 0) { d += (double)n; if (d < 0) d = 0; }
    if (d >= (double)n) return JS_OK;
    for (i = (unsigned long)d; i < n; i++) {
        js_value v;
        if (aget(ctx, o, i, &v) != JS_OK) return JS_THROW;
        if (js_strict_equals(v, needle)) { *ret = js_number((double)i); return JS_OK; }
    }
    return JS_OK;
}

static int bi_array_lastindexof(js_ctx *ctx, js_value t, int argc,
                                js_value *argv, js_value *ret)
{
    js_object *o;
    unsigned long n, i;
    double d;
    js_value needle = arg(argc, argv, 0);

    if (this_object(ctx, t, &o) != JS_OK) return JS_THROW;
    if (alen(ctx, o, &n) != JS_OK) return JS_THROW;
    *ret = js_number(-1);
    if (n == 0) return JS_OK;
    d = (double)n - 1;
    if (argc > 1) {
        if (js_to_integer(ctx, argv[1], &d) != JS_OK) return JS_THROW;
        if (d < 0) d += (double)n;
        if (d < 0) return JS_OK;
        if (d > (double)n - 1) d = (double)n - 1;
    }
    for (i = (unsigned long)d + 1; i > 0; i--) {
        js_value v;
        if (aget(ctx, o, i - 1, &v) != JS_OK) return JS_THROW;
        if (js_strict_equals(v, needle)) { *ret = js_number((double)(i - 1)); return JS_OK; }
    }
    return JS_OK;
}

static int bi_array_reverse(js_ctx *ctx, js_value t, int argc, js_value *argv,
                            js_value *ret)
{
    js_object *o;
    unsigned long n, i;

    (void)argc; (void)argv;
    if (this_object(ctx, t, &o) != JS_OK) return JS_THROW;
    if (alen(ctx, o, &n) != JS_OK) return JS_THROW;
    for (i = 0; i < n / 2; i++) {
        js_value a, b;
        if (aget(ctx, o, i, &a) != JS_OK) return JS_THROW;
        if (aget(ctx, o, n - 1 - i, &b) != JS_OK) return JS_THROW;
        if (aput(ctx, o, i, b) != JS_OK) return JS_THROW;
        if (aput(ctx, o, n - 1 - i, a) != JS_OK) return JS_THROW;
    }
    *ret = js_object_value(o);
    return JS_OK;
}

/* ---- sort ---- */

/* Returns -1, 0 or 1; JS_THROW is signalled through *err. */
static int sort_cmp(js_ctx *ctx, js_value cmp, js_value a, js_value b, int *err)
{
    *err = 0;
    /* undefined sorts to the end regardless of the comparator */
    if (a.type == JS_UNDEFINED) return b.type == JS_UNDEFINED ? 0 : 1;
    if (b.type == JS_UNDEFINED) return -1;
    if (js_is_function(cmp)) {
        js_value args[2], r;
        double d;
        args[0] = a; args[1] = b;
        if (js_call(ctx, cmp, js_undefined(), 2, args, &r) != JS_OK) { *err = 1; return 0; }
        if (js_to_number(ctx, r, &d) != JS_OK) { *err = 1; return 0; }
        if (js_isnan(d)) return 0;
        return d < 0 ? -1 : (d > 0 ? 1 : 0);
    }
    {
        js_value sa, sb;
        int c;
        if (js_to_string(ctx, a, &sa) != JS_OK) { *err = 1; return 0; }
        if (js_to_string(ctx, b, &sb) != JS_OK) { *err = 1; return 0; }
        {
            unsigned long m = sa.u.str->len < sb.u.str->len ? sa.u.str->len : sb.u.str->len;
            c = memcmp(sa.u.str->data, sb.u.str->data, m);
            if (c == 0) {
                if (sa.u.str->len == sb.u.str->len) return 0;
                return sa.u.str->len < sb.u.str->len ? -1 : 1;
            }
        }
        return c < 0 ? -1 : 1;
    }
}

static int bi_array_sort(js_ctx *ctx, js_value t, int argc, js_value *argv,
                         js_value *ret)
{
    js_object *o;
    unsigned long n, width, i;
    js_value cmp = arg(argc, argv, 0);
    js_value *buf, *tmp;
    int err = 0;

    if (this_object(ctx, t, &o) != JS_OK) return JS_THROW;
    if (alen(ctx, o, &n) != JS_OK) return JS_THROW;
    *ret = js_object_value(o);
    if (n < 2) return JS_OK;
    if (n > 1000000UL)
        return js_throw_error(ctx, JS_ERR_RANGE, "array is too large to sort");

    buf = (js_value *)js_alloc(ctx, n * sizeof(js_value));
    tmp = (js_value *)js_alloc(ctx, n * sizeof(js_value));
    if (!buf || !tmp) return JS_THROW;
    for (i = 0; i < n; i++)
        if (aget(ctx, o, i, &buf[i]) != JS_OK) return JS_THROW;

    /* Bottom-up merge sort: stable, O(n log n) comparator calls, and no
     * recursion, which matters on a 64 KiB stack. */
    for (width = 1; width < n; width *= 2) {
        unsigned long lo;
        for (lo = 0; lo < n; lo += 2 * width) {
            unsigned long mid = lo + width, hi = lo + 2 * width;
            unsigned long a = lo, b = mid, k = lo;
            if (mid > n) mid = n;
            if (hi > n) hi = n;
            b = mid;
            while (a < mid && b < hi) {
                if (js_step(ctx) != JS_OK) return JS_THROW;
                if (sort_cmp(ctx, cmp, buf[b], buf[a], &err) < 0)
                    tmp[k++] = buf[b++];
                else
                    tmp[k++] = buf[a++];
                if (err) return JS_THROW;
            }
            while (a < mid) tmp[k++] = buf[a++];
            while (b < hi) tmp[k++] = buf[b++];
        }
        for (i = 0; i < n; i++) buf[i] = tmp[i];
    }
    for (i = 0; i < n; i++)
        if (aput(ctx, o, i, buf[i]) != JS_OK) return JS_THROW;
    return JS_OK;
}

/* ---- iteration methods ---- */

enum { IT_FOREACH, IT_MAP, IT_FILTER, IT_EVERY, IT_SOME };

static int array_iter(js_ctx *ctx, int kind, js_value t, int argc,
                      js_value *argv, js_value *ret)
{
    js_object *o, *out = 0;
    unsigned long n, i;
    js_value fn = arg(argc, argv, 0), self = arg(argc, argv, 1);

    if (this_object(ctx, t, &o) != JS_OK) return JS_THROW;
    if (alen(ctx, o, &n) != JS_OK) return JS_THROW;
    if (!js_is_function(fn))
        return js_throw_error(ctx, JS_ERR_TYPE, "callback is not a function");
    if (kind == IT_MAP || kind == IT_FILTER) {
        out = js_new_array(ctx);
        if (!out) return JS_THROW;
    }
    *ret = kind == IT_EVERY ? js_bool(1)
         : kind == IT_SOME  ? js_bool(0) : js_undefined();
    for (i = 0; i < n; i++) {
        js_value args[3], r;
        if (aget(ctx, o, i, &args[0]) != JS_OK) return JS_THROW;
        args[1] = js_number((double)i);
        args[2] = js_object_value(o);
        if (js_call(ctx, fn, self, 3, args, &r) != JS_OK) return JS_THROW;
        switch (kind) {
        case IT_MAP:
            if (js_array_push(ctx, out, r) != JS_OK) return JS_THROW;
            break;
        case IT_FILTER:
            if (js_to_boolean(r) && js_array_push(ctx, out, args[0]) != JS_OK)
                return JS_THROW;
            break;
        case IT_EVERY:
            if (!js_to_boolean(r)) { *ret = js_bool(0); return JS_OK; }
            break;
        case IT_SOME:
            if (js_to_boolean(r)) { *ret = js_bool(1); return JS_OK; }
            break;
        default:
            break;
        }
    }
    if (out) *ret = js_object_value(out);
    return JS_OK;
}

static int bi_array_foreach(js_ctx *ctx, js_value t, int argc, js_value *argv, js_value *ret)
{ return array_iter(ctx, IT_FOREACH, t, argc, argv, ret); }
static int bi_array_map(js_ctx *ctx, js_value t, int argc, js_value *argv, js_value *ret)
{ return array_iter(ctx, IT_MAP, t, argc, argv, ret); }
static int bi_array_filter(js_ctx *ctx, js_value t, int argc, js_value *argv, js_value *ret)
{ return array_iter(ctx, IT_FILTER, t, argc, argv, ret); }
static int bi_array_every(js_ctx *ctx, js_value t, int argc, js_value *argv, js_value *ret)
{ return array_iter(ctx, IT_EVERY, t, argc, argv, ret); }
static int bi_array_some(js_ctx *ctx, js_value t, int argc, js_value *argv, js_value *ret)
{ return array_iter(ctx, IT_SOME, t, argc, argv, ret); }

static int array_reduce(js_ctx *ctx, int right, js_value t, int argc,
                        js_value *argv, js_value *ret)
{
    js_object *o;
    unsigned long n, i;
    js_value fn = arg(argc, argv, 0), acc;
    int have = argc > 1;

    if (this_object(ctx, t, &o) != JS_OK) return JS_THROW;
    if (alen(ctx, o, &n) != JS_OK) return JS_THROW;
    if (!js_is_function(fn))
        return js_throw_error(ctx, JS_ERR_TYPE, "callback is not a function");
    if (!have && n == 0)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "reduce of an empty array with no initial value");
    acc = have ? argv[1] : js_undefined();
    for (i = 0; i < n; i++) {
        unsigned long idx = right ? n - 1 - i : i;
        js_value args[4], v, r;
        if (aget(ctx, o, idx, &v) != JS_OK) return JS_THROW;
        if (!have) { acc = v; have = 1; continue; }
        args[0] = acc; args[1] = v;
        args[2] = js_number((double)idx);
        args[3] = js_object_value(o);
        if (js_call(ctx, fn, js_undefined(), 4, args, &r) != JS_OK) return JS_THROW;
        acc = r;
    }
    *ret = acc;
    return JS_OK;
}

static int bi_array_reduce(js_ctx *ctx, js_value t, int argc, js_value *argv, js_value *ret)
{ return array_reduce(ctx, 0, t, argc, argv, ret); }
static int bi_array_reduceright(js_ctx *ctx, js_value t, int argc, js_value *argv, js_value *ret)
{ return array_reduce(ctx, 1, t, argc, argv, ret); }

/* ================================================================== */
/* RegExp objects                                                      */
/* ================================================================== */

#define RE_CAPSLOTS (2 * (RE_MAX_GROUPS + 1))

static js_object *new_regexp_obj(js_ctx *ctx, js_regexp *re)
{
    js_object *o = js_obj_alloc(ctx, JS_CLASS_REGEXP, ctx->proto[P_REGEXP]);

    if (!o) return 0;
    o->re = re;
    if (js_define(ctx, o, "lastIndex", js_number(0)) != JS_OK) return 0;
    return o;
}

static int bi_regexp_ctor(js_ctx *ctx, js_value t, int argc, js_value *argv,
                          js_value *ret)
{
    js_value pv = arg(argc, argv, 0), fv = arg(argc, argv, 1);
    js_string *src, *flags;
    js_regexp *re;
    js_object *o;

    (void)t;
    if (pv.type == JS_OBJECT && pv.u.obj->cls == JS_CLASS_REGEXP) {
        if (fv.type == JS_UNDEFINED) { *ret = pv; return JS_OK; }
        src = pv.u.obj->re->source;
    } else if (pv.type == JS_UNDEFINED) {
        src = ctx->s_empty;
    } else {
        js_value s;
        if (js_to_string(ctx, pv, &s) != JS_OK) return JS_THROW;
        src = s.u.str;
    }
    if (fv.type == JS_UNDEFINED) {
        flags = ctx->s_empty;
    } else {
        js_value s;
        if (js_to_string(ctx, fv, &s) != JS_OK) return JS_THROW;
        flags = s.u.str;
    }
    re = js_regexp_compile(ctx, src, flags);
    if (!re) return JS_THROW;
    o = new_regexp_obj(ctx, re);
    if (!o) return JS_THROW;
    *ret = js_object_value(o);
    return JS_OK;
}

static int this_regexp(js_ctx *ctx, js_value t, js_object **out)
{
    if (t.type != JS_OBJECT || t.u.obj->cls != JS_CLASS_REGEXP || !t.u.obj->re)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "RegExp.prototype method called on a non-RegExp");
    *out = t.u.obj;
    return JS_OK;
}

/* Build the array that exec() and match() return. */
static int match_result(js_ctx *ctx, js_string *s, long *caps, int ngroups,
                        js_value *ret)
{
    js_object *a = js_new_array(ctx);
    int i;

    if (!a) return JS_THROW;
    for (i = 0; i < ngroups; i++) {
        js_value v = js_undefined();
        if (caps[2 * i] >= 0 && caps[2 * i + 1] >= 0) {
            js_string *g = js_str_new(ctx, s->data + caps[2 * i],
                                      (unsigned long)(caps[2 * i + 1] - caps[2 * i]));
            if (!g) return JS_THROW;
            v = js_string_value(g);
        }
        if (js_array_push(ctx, a, v) != JS_OK) return JS_THROW;
    }
    if (js_define_enum(ctx, a, "index", js_number((double)caps[0])) != JS_OK ||
        js_define_enum(ctx, a, "input", js_string_value(s)) != JS_OK)
        return JS_THROW;
    *ret = js_object_value(a);
    return JS_OK;
}

static int regexp_exec(js_ctx *ctx, js_object *ro, js_string *s, js_value *ret)
{
    long caps[RE_CAPSLOTS];
    js_regexp *re = ro->re;
    long start = 0;
    int r;

    *ret = js_null();
    if (re->global) {
        js_value li;
        double d;
        if (js_obj_get(ctx, ro, ctx->s_lastIndex, js_object_value(ro), &li) != JS_OK)
            return JS_THROW;
        if (js_to_integer(ctx, li, &d) != JS_OK) return JS_THROW;
        if (d < 0 || d > (double)s->len) {
            if (js_obj_put(ctx, ro, ctx->s_lastIndex, js_number(0)) != JS_OK)
                return JS_THROW;
            return JS_OK;
        }
        start = (long)d;
    }
    r = js_regexp_exec_raw(ctx, re, s->data, (long)s->len, start, caps);
    if (r < 0) return JS_THROW;
    if (r == 0) {
        if (re->global &&
            js_obj_put(ctx, ro, ctx->s_lastIndex, js_number(0)) != JS_OK)
            return JS_THROW;
        return JS_OK;
    }
    if (re->global &&
        js_obj_put(ctx, ro, ctx->s_lastIndex, js_number((double)caps[1])) != JS_OK)
        return JS_THROW;
    return match_result(ctx, s, caps, re->ngroups, ret);
}

static int bi_regexp_exec(js_ctx *ctx, js_value t, int argc, js_value *argv,
                          js_value *ret)
{
    js_object *ro = 0;
    js_value s;

    if (this_regexp(ctx, t, &ro) != JS_OK) return JS_THROW;
    if (js_to_string(ctx, arg(argc, argv, 0), &s) != JS_OK) return JS_THROW;
    return regexp_exec(ctx, ro, s.u.str, ret);
}

static int bi_regexp_test(js_ctx *ctx, js_value t, int argc, js_value *argv,
                          js_value *ret)
{
    js_value r;

    if (bi_regexp_exec(ctx, t, argc, argv, &r) != JS_OK) return JS_THROW;
    *ret = js_bool(r.type != JS_NULL);
    return JS_OK;
}

static int bi_regexp_tostring(js_ctx *ctx, js_value t, int argc, js_value *argv,
                              js_value *ret)
{
    js_object *ro = 0;
    js_sbuf b;

    (void)argc; (void)argv;
    if (this_regexp(ctx, t, &ro) != JS_OK) return JS_THROW;
    js_sb_init(&b, ctx);
    if (js_sb_putc(&b, '/') != JS_OK ||
        js_sb_put(&b, ro->re->source->data, ro->re->source->len) != JS_OK ||
        js_sb_putc(&b, '/') != JS_OK ||
        js_sb_put(&b, ro->re->flags->data, ro->re->flags->len) != JS_OK) {
        js_sb_free(&b);
        return JS_THROW;
    }
    return js_sb_finish(&b, ret);
}

static int re_getter(js_ctx *ctx, js_value t, js_value *ret, int which)
{
    js_object *ro = 0;

    if (this_regexp(ctx, t, &ro) != JS_OK) return JS_THROW;
    switch (which) {
    case 0: *ret = js_string_value(ro->re->source); break;
    case 1: *ret = js_bool(ro->re->global); break;
    case 2: *ret = js_bool(ro->re->ignore_case); break;
    default: *ret = js_bool(ro->re->multiline); break;
    }
    return JS_OK;
}

static int bi_re_source(js_ctx *ctx, js_value t, int argc, js_value *argv, js_value *ret)
{ (void)argc; (void)argv; return re_getter(ctx, t, ret, 0); }
static int bi_re_global(js_ctx *ctx, js_value t, int argc, js_value *argv, js_value *ret)
{ (void)argc; (void)argv; return re_getter(ctx, t, ret, 1); }
static int bi_re_icase(js_ctx *ctx, js_value t, int argc, js_value *argv, js_value *ret)
{ (void)argc; (void)argv; return re_getter(ctx, t, ret, 2); }
static int bi_re_multi(js_ctx *ctx, js_value t, int argc, js_value *argv, js_value *ret)
{ (void)argc; (void)argv; return re_getter(ctx, t, ret, 3); }

/* Coerce a String.prototype argument to a regexp, as match/search/split do. */
static js_object *to_regexp(js_ctx *ctx, js_value v)
{
    js_value s;
    js_regexp *re;

    if (v.type == JS_OBJECT && v.u.obj->cls == JS_CLASS_REGEXP && v.u.obj->re)
        return v.u.obj;
    if (js_to_string(ctx, v, &s) != JS_OK)
        return 0;
    re = js_regexp_compile(ctx, s.u.str, ctx->s_empty);
    if (!re) return 0;
    return new_regexp_obj(ctx, re);
}

/* ================================================================== */
/* String                                                              */
/* ================================================================== */

static int bi_string_ctor(js_ctx *ctx, js_value t, int argc, js_value *argv,
                          js_value *ret)
{
    js_value s;

    (void)t;
    if (argc == 0)
        s = js_string_value(ctx->s_empty);
    else if (js_to_string(ctx, argv[0], &s) != JS_OK)
        return JS_THROW;
    if (ctx->new_target)
        return js_wrap_primitive(ctx, s, ret);
    *ret = s;
    return JS_OK;
}

static int bi_string_fromcharcode(js_ctx *ctx, js_value t, int argc,
                                  js_value *argv, js_value *ret)
{
    js_sbuf b;
    int i;

    (void)t;
    js_sb_init(&b, ctx);
    for (i = 0; i < argc; i++) {
        uint32_t u;
        if (js_to_uint32(ctx, argv[i], &u) != JS_OK) { js_sb_free(&b); return JS_THROW; }
        u &= 0xFFFF;
        if (u < 256) {
            if (js_sb_putc(&b, (char)u) != JS_OK) { js_sb_free(&b); return JS_THROW; }
        } else {
            char e[3];
            e[0] = (char)(0xE0 | (u >> 12));
            e[1] = (char)(0x80 | ((u >> 6) & 0x3F));
            e[2] = (char)(0x80 | (u & 0x3F));
            if (js_sb_put(&b, e, 3) != JS_OK) { js_sb_free(&b); return JS_THROW; }
        }
    }
    return js_sb_finish(&b, ret);
}

static int bi_string_tostring(js_ctx *ctx, js_value t, int argc, js_value *argv,
                              js_value *ret)
{
    (void)argc; (void)argv;
    if (t.type == JS_STRING) { *ret = t; return JS_OK; }
    if (t.type == JS_OBJECT && t.u.obj->cls == JS_CLASS_STRING) {
        *ret = t.u.obj->prim;
        return JS_OK;
    }
    return js_throw_error(ctx, JS_ERR_TYPE,
                          "String.prototype.toString on a non-string");
}

static int bi_string_charat(js_ctx *ctx, js_value t, int argc, js_value *argv,
                            js_value *ret)
{
    js_string *s;
    double d = 0;

    if (this_string(ctx, t, &s) != JS_OK) return JS_THROW;
    if (js_to_integer(ctx, arg(argc, argv, 0), &d) != JS_OK) return JS_THROW;
    if (d < 0 || d >= (double)s->len)
        return ret_str(ctx, "", 0, ret);
    return ret_str(ctx, s->data + (unsigned long)d, 1, ret);
}

static int bi_string_charcodeat(js_ctx *ctx, js_value t, int argc,
                                js_value *argv, js_value *ret)
{
    js_string *s;
    double d = 0;

    if (this_string(ctx, t, &s) != JS_OK) return JS_THROW;
    if (js_to_integer(ctx, arg(argc, argv, 0), &d) != JS_OK) return JS_THROW;
    if (d < 0 || d >= (double)s->len) { *ret = js_number(js_nan()); return JS_OK; }
    *ret = js_number((double)(unsigned char)s->data[(unsigned long)d]);
    return JS_OK;
}

static long find_sub(const char *h, unsigned long hn, const char *n,
                     unsigned long nn, unsigned long from)
{
    unsigned long i;

    if (nn > hn) return -1;
    for (i = from; i + nn <= hn; i++)
        if (memcmp(h + i, n, nn) == 0)
            return (long)i;
    return -1;
}

static int bi_string_indexof(js_ctx *ctx, js_value t, int argc, js_value *argv,
                             js_value *ret)
{
    js_string *s;
    js_value nv;
    double d = 0;

    if (this_string(ctx, t, &s) != JS_OK) return JS_THROW;
    if (js_to_string(ctx, arg(argc, argv, 0), &nv) != JS_OK) return JS_THROW;
    if (argc > 1 && js_to_integer(ctx, argv[1], &d) != JS_OK) return JS_THROW;
    if (d < 0) d = 0;
    if (d > (double)s->len) d = (double)s->len;
    *ret = js_number((double)find_sub(s->data, s->len, nv.u.str->data,
                                      nv.u.str->len, (unsigned long)d));
    return JS_OK;
}

static int bi_string_lastindexof(js_ctx *ctx, js_value t, int argc,
                                 js_value *argv, js_value *ret)
{
    js_string *s;
    js_value nv;
    double d;
    unsigned long i, limit;

    if (this_string(ctx, t, &s) != JS_OK) return JS_THROW;
    if (js_to_string(ctx, arg(argc, argv, 0), &nv) != JS_OK) return JS_THROW;
    d = js_inf(0);
    if (argc > 1) {
        if (js_to_number(ctx, argv[1], &d) != JS_OK) return JS_THROW;
        if (js_isnan(d)) d = js_inf(0);
        else d = js_trunc(d);
    }
    *ret = js_number(-1);
    if (nv.u.str->len > s->len) return JS_OK;
    limit = s->len - nv.u.str->len;
    if (d >= 0 && d < (double)limit) limit = (unsigned long)d;
    if (d < 0) limit = 0;
    for (i = limit + 1; i > 0; i--)
        if (memcmp(s->data + i - 1, nv.u.str->data, nv.u.str->len) == 0) {
            *ret = js_number((double)(i - 1));
            return JS_OK;
        }
    return JS_OK;
}

static int bi_string_slice(js_ctx *ctx, js_value t, int argc, js_value *argv,
                           js_value *ret)
{
    js_string *s;
    unsigned long from, to;
    double d;

    if (this_string(ctx, t, &s) != JS_OK) return JS_THROW;
    if (js_to_integer(ctx, arg(argc, argv, 0), &d) != JS_OK) return JS_THROW;
    from = rel_index(d, s->len);
    if (argc > 1 && argv[1].type != JS_UNDEFINED) {
        if (js_to_integer(ctx, argv[1], &d) != JS_OK) return JS_THROW;
        to = rel_index(d, s->len);
    } else {
        to = s->len;
    }
    if (to < from) to = from;
    return ret_str(ctx, s->data + from, to - from, ret);
}

static int bi_string_substring(js_ctx *ctx, js_value t, int argc,
                               js_value *argv, js_value *ret)
{
    js_string *s;
    double a, b;
    unsigned long from, to, tmp;

    if (this_string(ctx, t, &s) != JS_OK) return JS_THROW;
    if (js_to_integer(ctx, arg(argc, argv, 0), &a) != JS_OK) return JS_THROW;
    if (argc > 1 && argv[1].type != JS_UNDEFINED) {
        if (js_to_integer(ctx, argv[1], &b) != JS_OK) return JS_THROW;
    } else {
        b = (double)s->len;
    }
    if (a < 0 || js_isnan(a)) a = 0;
    if (b < 0 || js_isnan(b)) b = 0;
    if (a > (double)s->len) a = (double)s->len;
    if (b > (double)s->len) b = (double)s->len;
    from = (unsigned long)a;
    to = (unsigned long)b;
    if (from > to) { tmp = from; from = to; to = tmp; }
    return ret_str(ctx, s->data + from, to - from, ret);
}

static int bi_string_substr(js_ctx *ctx, js_value t, int argc, js_value *argv,
                            js_value *ret)
{
    js_string *s;
    double a, n;
    unsigned long from, len;

    if (this_string(ctx, t, &s) != JS_OK) return JS_THROW;
    if (js_to_integer(ctx, arg(argc, argv, 0), &a) != JS_OK) return JS_THROW;
    if (a < 0) { a += (double)s->len; if (a < 0) a = 0; }
    if (a > (double)s->len) a = (double)s->len;
    from = (unsigned long)a;
    if (argc > 1 && argv[1].type != JS_UNDEFINED) {
        if (js_to_integer(ctx, argv[1], &n) != JS_OK) return JS_THROW;
        if (n < 0) n = 0;
    } else {
        n = (double)(s->len - from);
    }
    if (n > (double)(s->len - from)) n = (double)(s->len - from);
    len = (unsigned long)n;
    return ret_str(ctx, s->data + from, len, ret);
}

static int bi_string_case(js_ctx *ctx, js_value t, int argc, js_value *argv,
                          js_value *ret, int up)
{
    js_string *s;
    js_sbuf b;
    unsigned long i;

    (void)argc; (void)argv;
    if (this_string(ctx, t, &s) != JS_OK) return JS_THROW;
    js_sb_init(&b, ctx);
    for (i = 0; i < s->len; i++) {
        char c = s->data[i];
        if (up && c >= 'a' && c <= 'z') c = (char)(c - 32);
        else if (!up && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (js_sb_putc(&b, c) != JS_OK) { js_sb_free(&b); return JS_THROW; }
    }
    return js_sb_finish(&b, ret);
}

static int bi_string_upper(js_ctx *ctx, js_value t, int argc, js_value *argv, js_value *ret)
{ return bi_string_case(ctx, t, argc, argv, ret, 1); }
static int bi_string_lower(js_ctx *ctx, js_value t, int argc, js_value *argv, js_value *ret)
{ return bi_string_case(ctx, t, argc, argv, ret, 0); }

static int is_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f' || c == 0xA0;
}

static int bi_string_trim(js_ctx *ctx, js_value t, int argc, js_value *argv,
                          js_value *ret)
{
    js_string *s;
    unsigned long a = 0, b;

    (void)argc; (void)argv;
    if (this_string(ctx, t, &s) != JS_OK) return JS_THROW;
    b = s->len;
    while (a < b && is_space((unsigned char)s->data[a])) a++;
    while (b > a && is_space((unsigned char)s->data[b - 1])) b--;
    return ret_str(ctx, s->data + a, b - a, ret);
}

static int bi_string_concat(js_ctx *ctx, js_value t, int argc, js_value *argv,
                            js_value *ret)
{
    js_string *s;
    js_sbuf b;
    int i;

    if (this_string(ctx, t, &s) != JS_OK) return JS_THROW;
    js_sb_init(&b, ctx);
    if (js_sb_put(&b, s->data, s->len) != JS_OK) { js_sb_free(&b); return JS_THROW; }
    for (i = 0; i < argc; i++) {
        js_value v;
        if (js_to_string(ctx, argv[i], &v) != JS_OK) { js_sb_free(&b); return JS_THROW; }
        if (js_sb_put(&b, v.u.str->data, v.u.str->len) != JS_OK) {
            js_sb_free(&b);
            return JS_THROW;
        }
    }
    return js_sb_finish(&b, ret);
}

static int bi_string_localecompare(js_ctx *ctx, js_value t, int argc,
                                   js_value *argv, js_value *ret)
{
    js_string *s;
    js_value o;
    unsigned long m;
    int c;

    if (this_string(ctx, t, &s) != JS_OK) return JS_THROW;
    if (js_to_string(ctx, arg(argc, argv, 0), &o) != JS_OK) return JS_THROW;
    m = s->len < o.u.str->len ? s->len : o.u.str->len;
    c = memcmp(s->data, o.u.str->data, m);
    if (c == 0 && s->len != o.u.str->len)
        c = s->len < o.u.str->len ? -1 : 1;
    *ret = js_number(c < 0 ? -1 : (c > 0 ? 1 : 0));
    return JS_OK;
}

static int bi_string_search(js_ctx *ctx, js_value t, int argc, js_value *argv,
                            js_value *ret)
{
    js_string *s;
    js_object *ro;
    long caps[RE_CAPSLOTS];
    int r;

    if (this_string(ctx, t, &s) != JS_OK) return JS_THROW;
    ro = to_regexp(ctx, arg(argc, argv, 0));
    if (!ro) return JS_THROW;
    r = js_regexp_exec_raw(ctx, ro->re, s->data, (long)s->len, 0, caps);
    if (r < 0) return JS_THROW;
    *ret = js_number(r ? (double)caps[0] : -1);
    return JS_OK;
}

static int bi_string_match(js_ctx *ctx, js_value t, int argc, js_value *argv,
                           js_value *ret)
{
    js_string *s;
    js_object *ro;

    if (this_string(ctx, t, &s) != JS_OK) return JS_THROW;
    ro = to_regexp(ctx, arg(argc, argv, 0));
    if (!ro) return JS_THROW;
    if (!ro->re->global)
        return regexp_exec(ctx, ro, s, ret);
    {
        js_object *a = js_new_array(ctx);
        long caps[RE_CAPSLOTS];
        long pos = 0;
        if (!a) return JS_THROW;
        for (;;) {
            int r = js_regexp_exec_raw(ctx, ro->re, s->data, (long)s->len, pos, caps);
            js_string *m;
            if (r < 0) return JS_THROW;
            if (r == 0) break;
            m = js_str_new(ctx, s->data + caps[0], (unsigned long)(caps[1] - caps[0]));
            if (!m || js_array_push(ctx, a, js_string_value(m)) != JS_OK)
                return JS_THROW;
            pos = (caps[1] == caps[0]) ? caps[1] + 1 : caps[1];
            if (pos > (long)s->len) break;
        }
        if (js_obj_put(ctx, ro, ctx->s_lastIndex, js_number(0)) != JS_OK)
            return JS_THROW;
        *ret = a->elen ? js_object_value(a) : js_null();
        return JS_OK;
    }
}

/* Expand $&, $`, $', $1..$99 and $$ in a replacement template. */
static int expand_repl(js_ctx *ctx, js_sbuf *b, js_string *tpl, js_string *s,
                       long *caps, int ngroups)
{
    unsigned long i;

    (void)ctx;

    for (i = 0; i < tpl->len; i++) {
        char c = tpl->data[i];
        if (c != '$' || i + 1 >= tpl->len) {
            if (js_sb_putc(b, c) != JS_OK) return JS_THROW;
            continue;
        }
        {
            char n = tpl->data[i + 1];
            if (n == '$') { if (js_sb_putc(b, '$') != JS_OK) return JS_THROW; i++; continue; }
            if (n == '&') {
                if (js_sb_put(b, s->data + caps[0],
                              (unsigned long)(caps[1] - caps[0])) != JS_OK)
                    return JS_THROW;
                i++;
                continue;
            }
            if (n == '`') {
                if (js_sb_put(b, s->data, (unsigned long)caps[0]) != JS_OK)
                    return JS_THROW;
                i++;
                continue;
            }
            if (n == '\'') {
                if (js_sb_put(b, s->data + caps[1],
                              s->len - (unsigned long)caps[1]) != JS_OK)
                    return JS_THROW;
                i++;
                continue;
            }
            if (n >= '0' && n <= '9') {
                int g = n - '0';
                int used = 1;
                if (i + 2 < tpl->len && tpl->data[i + 2] >= '0' &&
                    tpl->data[i + 2] <= '9' &&
                    g * 10 + (tpl->data[i + 2] - '0') < ngroups) {
                    g = g * 10 + (tpl->data[i + 2] - '0');
                    used = 2;
                }
                if (g >= 1 && g < ngroups) {
                    if (caps[2 * g] >= 0 &&
                        js_sb_put(b, s->data + caps[2 * g],
                                  (unsigned long)(caps[2 * g + 1] - caps[2 * g])) != JS_OK)
                        return JS_THROW;
                    i += (unsigned long)used;
                    continue;
                }
            }
            if (js_sb_putc(b, c) != JS_OK) return JS_THROW;
        }
    }
    return JS_OK;
}

static int call_repl(js_ctx *ctx, js_sbuf *b, js_value fn, js_string *s,
                     long *caps, int ngroups)
{
    js_value args[RE_MAX_GROUPS + 4];
    js_value r, rs;
    int i, n = 0;

    for (i = 0; i < ngroups && n < RE_MAX_GROUPS + 2; i++) {
        if (caps[2 * i] >= 0) {
            js_string *g = js_str_new(ctx, s->data + caps[2 * i],
                                      (unsigned long)(caps[2 * i + 1] - caps[2 * i]));
            if (!g) return JS_THROW;
            args[n++] = js_string_value(g);
        } else {
            args[n++] = js_undefined();
        }
    }
    args[n++] = js_number((double)caps[0]);
    args[n++] = js_string_value(s);
    if (js_call(ctx, fn, js_undefined(), n, args, &r) != JS_OK) return JS_THROW;
    if (js_to_string(ctx, r, &rs) != JS_OK) return JS_THROW;
    return js_sb_put(b, rs.u.str->data, rs.u.str->len);
}

static int bi_string_replace(js_ctx *ctx, js_value t, int argc, js_value *argv,
                             js_value *ret)
{
    js_string *s;
    js_value pat = arg(argc, argv, 0), rep = arg(argc, argv, 1);
    js_sbuf b;
    int use_fn = js_is_function(rep);
    js_string *tpl = 0;

    if (this_string(ctx, t, &s) != JS_OK) return JS_THROW;
    if (!use_fn) {
        js_value rs;
        if (js_to_string(ctx, rep, &rs) != JS_OK) return JS_THROW;
        tpl = rs.u.str;
    }
    js_sb_init(&b, ctx);

    if (pat.type == JS_OBJECT && pat.u.obj->cls == JS_CLASS_REGEXP && pat.u.obj->re) {
        js_regexp *re = pat.u.obj->re;
        long caps[RE_CAPSLOTS];
        long pos = 0;
        for (;;) {
            int r = js_regexp_exec_raw(ctx, re, s->data, (long)s->len, pos, caps);
            if (r < 0) goto fail;
            if (r == 0) break;
            if (js_sb_put(&b, s->data + pos, (unsigned long)(caps[0] - pos)) != JS_OK)
                goto fail;
            if (use_fn) {
                if (call_repl(ctx, &b, rep, s, caps, re->ngroups) != JS_OK) goto fail;
            } else if (expand_repl(ctx, &b, tpl, s, caps, re->ngroups) != JS_OK) {
                goto fail;
            }
            if (caps[1] == caps[0]) {
                if (caps[1] < (long)s->len &&
                    js_sb_put(&b, s->data + caps[1], 1) != JS_OK) goto fail;
                pos = caps[1] + 1;
            } else {
                pos = caps[1];
            }
            if (!re->global || pos > (long)s->len) break;
        }
        if (pos <= (long)s->len &&
            js_sb_put(&b, s->data + pos, s->len - (unsigned long)pos) != JS_OK)
            goto fail;
        if (re->global &&
            js_obj_put(ctx, pat.u.obj, ctx->s_lastIndex, js_number(0)) != JS_OK)
            goto fail;
        return js_sb_finish(&b, ret);
    }

    {
        js_value ps;
        long at;
        long caps[2];
        if (js_to_string(ctx, pat, &ps) != JS_OK) goto fail;
        at = find_sub(s->data, s->len, ps.u.str->data, ps.u.str->len, 0);
        if (at < 0) {
            js_sb_free(&b);
            *ret = js_string_value(s);
            return JS_OK;
        }
        if (js_sb_put(&b, s->data, (unsigned long)at) != JS_OK) goto fail;
        caps[0] = at;
        caps[1] = at + (long)ps.u.str->len;
        if (use_fn) {
            if (call_repl(ctx, &b, rep, s, caps, 1) != JS_OK) goto fail;
        } else if (expand_repl(ctx, &b, tpl, s, caps, 1) != JS_OK) {
            goto fail;
        }
        if (js_sb_put(&b, s->data + caps[1], s->len - (unsigned long)caps[1]) != JS_OK)
            goto fail;
        return js_sb_finish(&b, ret);
    }
fail:
    js_sb_free(&b);
    return JS_THROW;
}

static int bi_string_split(js_ctx *ctx, js_value t, int argc, js_value *argv,
                           js_value *ret)
{
    js_string *s;
    js_value sep = arg(argc, argv, 0);
    js_object *a;
    unsigned long limit = 0xFFFFFFFFUL;

    if (this_string(ctx, t, &s) != JS_OK) return JS_THROW;
    if (argc > 1 && argv[1].type != JS_UNDEFINED) {
        uint32_t u;
        if (js_to_uint32(ctx, argv[1], &u) != JS_OK) return JS_THROW;
        limit = u;
    }
    a = js_new_array(ctx);
    if (!a) return JS_THROW;
    *ret = js_object_value(a);
    if (limit == 0) return JS_OK;
    if (sep.type == JS_UNDEFINED) {
        if (js_array_push(ctx, a, js_string_value(s)) != JS_OK) return JS_THROW;
        return JS_OK;
    }

    if (sep.type == JS_OBJECT && sep.u.obj->cls == JS_CLASS_REGEXP && sep.u.obj->re) {
        js_regexp *re = sep.u.obj->re;
        long caps[RE_CAPSLOTS];
        long pos = 0, last = 0;
        while (pos <= (long)s->len) {
            int r = js_regexp_exec_raw(ctx, re, s->data, (long)s->len, pos, caps);
            js_string *piece;
            int g;
            if (r < 0) return JS_THROW;
            if (r == 0) break;
            if (caps[1] == caps[0] && caps[0] == last) { pos = caps[0] + 1; continue; }
            if (caps[0] >= (long)s->len) break;
            piece = js_str_new(ctx, s->data + last, (unsigned long)(caps[0] - last));
            if (!piece || js_array_push(ctx, a, js_string_value(piece)) != JS_OK)
                return JS_THROW;
            if (a->elen >= limit) return JS_OK;
            for (g = 1; g < re->ngroups; g++) {
                js_value v = js_undefined();
                if (caps[2 * g] >= 0) {
                    js_string *cg = js_str_new(ctx, s->data + caps[2 * g],
                                       (unsigned long)(caps[2 * g + 1] - caps[2 * g]));
                    if (!cg) return JS_THROW;
                    v = js_string_value(cg);
                }
                if (js_array_push(ctx, a, v) != JS_OK) return JS_THROW;
                if (a->elen >= limit) return JS_OK;
            }
            last = caps[1];
            pos = (caps[1] == caps[0]) ? caps[1] + 1 : caps[1];
        }
        {
            js_string *piece = js_str_new(ctx, s->data + last,
                                          s->len - (unsigned long)last);
            if (!piece || js_array_push(ctx, a, js_string_value(piece)) != JS_OK)
                return JS_THROW;
        }
        return JS_OK;
    }

    {
        js_value ss;
        unsigned long i, last = 0;
        if (js_to_string(ctx, sep, &ss) != JS_OK) return JS_THROW;
        if (ss.u.str->len == 0) {
            for (i = 0; i < s->len && a->elen < limit; i++) {
                js_string *c = js_str_new(ctx, s->data + i, 1);
                if (!c || js_array_push(ctx, a, js_string_value(c)) != JS_OK)
                    return JS_THROW;
            }
            return JS_OK;
        }
        for (i = 0; i + ss.u.str->len <= s->len; ) {
            if (memcmp(s->data + i, ss.u.str->data, ss.u.str->len) == 0) {
                js_string *piece = js_str_new(ctx, s->data + last, i - last);
                if (!piece || js_array_push(ctx, a, js_string_value(piece)) != JS_OK)
                    return JS_THROW;
                if (a->elen >= limit) return JS_OK;
                i += ss.u.str->len;
                last = i;
            } else {
                i++;
            }
        }
        {
            js_string *piece = js_str_new(ctx, s->data + last, s->len - last);
            if (!piece || js_array_push(ctx, a, js_string_value(piece)) != JS_OK)
                return JS_THROW;
        }
        return JS_OK;
    }
}

/* ================================================================== */
/* Number and Boolean                                                  */
/* ================================================================== */

static int bi_number_ctor(js_ctx *ctx, js_value t, int argc, js_value *argv,
                          js_value *ret)
{
    double d = 0;

    (void)t;
    if (argc > 0 && js_to_number(ctx, argv[0], &d) != JS_OK) return JS_THROW;
    if (ctx->new_target)
        return js_wrap_primitive(ctx, js_number(d), ret);
    *ret = js_number(d);
    return JS_OK;
}

static int bi_number_tostring(js_ctx *ctx, js_value t, int argc, js_value *argv,
                              js_value *ret)
{
    double d = 0;
    char buf[80];
    double r = 10;

    if (this_number(ctx, t, &d) != JS_OK) return JS_THROW;
    if (argc > 0 && argv[0].type != JS_UNDEFINED &&
        js_to_integer(ctx, argv[0], &r) != JS_OK)
        return JS_THROW;
    if (r < 2 || r > 36)
        return js_throw_error(ctx, JS_ERR_RANGE, "toString() radix must be 2..36");
    js_dtoa_radix(d, (int)r, buf, sizeof(buf));
    return ret_str(ctx, buf, strlen(buf), ret);
}

static int bi_number_valueof(js_ctx *ctx, js_value t, int argc, js_value *argv,
                             js_value *ret)
{
    double d = 0;
    (void)argc; (void)argv;
    if (this_number(ctx, t, &d) != JS_OK) return JS_THROW;
    *ret = js_number(d);
    return JS_OK;
}

static int bi_number_tofixed(js_ctx *ctx, js_value t, int argc, js_value *argv,
                             js_value *ret)
{
    double d = 0, f = 0;
    char buf[128];

    if (this_number(ctx, t, &d) != JS_OK) return JS_THROW;
    if (js_to_integer(ctx, arg(argc, argv, 0), &f) != JS_OK) return JS_THROW;
    if (f < 0 || f > 20)
        return js_throw_error(ctx, JS_ERR_RANGE,
                              "toFixed() digits must be 0..20");
    js_dtoa_fixed(d, (int)f, buf, sizeof(buf));
    return ret_str(ctx, buf, strlen(buf), ret);
}

static int bi_number_toprecision(js_ctx *ctx, js_value t, int argc,
                                 js_value *argv, js_value *ret)
{
    double d = 0, p = 0;
    char buf[128];

    if (this_number(ctx, t, &d) != JS_OK) return JS_THROW;
    if (argc == 0 || argv[0].type == JS_UNDEFINED) {
        js_dtoa(d, buf);
        return ret_str(ctx, buf, strlen(buf), ret);
    }
    if (js_to_integer(ctx, argv[0], &p) != JS_OK) return JS_THROW;
    if (p < 1 || p > 21)
        return js_throw_error(ctx, JS_ERR_RANGE,
                              "toPrecision() argument must be 1..21");
    js_dtoa_precision(d, (int)p, buf, sizeof(buf));
    return ret_str(ctx, buf, strlen(buf), ret);
}

static int bi_number_toexponential(js_ctx *ctx, js_value t, int argc,
                                   js_value *argv, js_value *ret)
{
    double d = 0, f = -1;
    char buf[128];

    if (this_number(ctx, t, &d) != JS_OK) return JS_THROW;
    if (argc > 0 && argv[0].type != JS_UNDEFINED) {
        if (js_to_integer(ctx, argv[0], &f) != JS_OK) return JS_THROW;
        if (f < 0 || f > 20)
            return js_throw_error(ctx, JS_ERR_RANGE,
                                  "toExponential() digits must be 0..20");
    }
    js_dtoa_exponential(d, (int)f, buf, sizeof(buf));
    return ret_str(ctx, buf, strlen(buf), ret);
}

static int bi_bool_ctor(js_ctx *ctx, js_value t, int argc, js_value *argv,
                        js_value *ret)
{
    js_value b = js_bool(js_to_boolean(arg(argc, argv, 0)));

    (void)t;
    if (ctx->new_target)
        return js_wrap_primitive(ctx, b, ret);
    *ret = b;
    return JS_OK;
}

static int this_bool(js_ctx *ctx, js_value t, int *out)
{
    if (t.type == JS_OBJECT && t.u.obj->cls == JS_CLASS_BOOLEAN)
        t = t.u.obj->prim;
    if (t.type != JS_BOOL)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "Boolean.prototype method called on a non-boolean");
    *out = t.u.b;
    return JS_OK;
}

static int bi_bool_tostring(js_ctx *ctx, js_value t, int argc, js_value *argv,
                            js_value *ret)
{
    int b = 0;
    (void)argc; (void)argv;
    if (this_bool(ctx, t, &b) != JS_OK) return JS_THROW;
    *ret = js_string_value(b ? ctx->s_true : ctx->s_false);
    return JS_OK;
}

static int bi_bool_valueof(js_ctx *ctx, js_value t, int argc, js_value *argv,
                           js_value *ret)
{
    int b = 0;
    (void)argc; (void)argv;
    if (this_bool(ctx, t, &b) != JS_OK) return JS_THROW;
    *ret = js_bool(b);
    return JS_OK;
}

/* ================================================================== */
/* Math                                                                */
/* ================================================================== */

enum {
    M_ABS, M_ACOS, M_ASIN, M_ATAN, M_CEIL, M_COS, M_EXP, M_FLOOR,
    M_LOG, M_ROUND, M_SIN, M_SQRT, M_TAN
};

static int bi_math1(js_ctx *ctx, js_value t, int argc, js_value *argv,
                    js_value *ret, int which)
{
    double d;

    (void)t;
    if (js_to_number(ctx, arg(argc, argv, 0), &d) != JS_OK) return JS_THROW;
    switch (which) {
    case M_ABS:   d = js_fabs(d); break;
    case M_ACOS:  d = js_acos(d); break;
    case M_ASIN:  d = js_asin(d); break;
    case M_ATAN:  d = js_atan(d); break;
    case M_CEIL:  d = js_ceil(d); break;
    case M_COS:   d = js_cos(d); break;
    case M_EXP:   d = js_exp(d); break;
    case M_FLOOR: d = js_floor(d); break;
    case M_LOG:   d = js_log(d); break;
    case M_ROUND: d = js_round(d); break;
    case M_SIN:   d = js_sin(d); break;
    case M_SQRT:  d = js_sqrt(d); break;
    default:      d = js_tan(d); break;
    }
    *ret = js_number(d);
    return JS_OK;
}

#define MATH1(name, code) \
    static int bi_math_##name(js_ctx *c, js_value t, int a, js_value *v, js_value *r) \
    { return bi_math1(c, t, a, v, r, code); }

MATH1(abs, M_ABS) MATH1(acos, M_ACOS) MATH1(asin, M_ASIN) MATH1(atan, M_ATAN)
MATH1(ceil, M_CEIL) MATH1(cos, M_COS) MATH1(exp, M_EXP) MATH1(floor, M_FLOOR)
MATH1(log, M_LOG) MATH1(round, M_ROUND) MATH1(sin, M_SIN) MATH1(sqrt, M_SQRT)
MATH1(tan, M_TAN)

static int bi_math_atan2(js_ctx *ctx, js_value t, int argc, js_value *argv,
                         js_value *ret)
{
    double y, x;
    (void)t;
    if (js_to_number(ctx, arg(argc, argv, 0), &y) != JS_OK) return JS_THROW;
    if (js_to_number(ctx, arg(argc, argv, 1), &x) != JS_OK) return JS_THROW;
    *ret = js_number(js_atan2(y, x));
    return JS_OK;
}

static int bi_math_pow(js_ctx *ctx, js_value t, int argc, js_value *argv,
                       js_value *ret)
{
    double x, y;
    (void)t;
    if (js_to_number(ctx, arg(argc, argv, 0), &x) != JS_OK) return JS_THROW;
    if (js_to_number(ctx, arg(argc, argv, 1), &y) != JS_OK) return JS_THROW;
    *ret = js_number(js_pow(x, y));
    return JS_OK;
}

static int bi_math_minmax(js_ctx *ctx, js_value t, int argc, js_value *argv,
                          js_value *ret, int want_max)
{
    double best = want_max ? js_inf(1) : js_inf(0);
    int i, nan = 0;

    (void)t;
    for (i = 0; i < argc; i++) {
        double d;
        if (js_to_number(ctx, argv[i], &d) != JS_OK) return JS_THROW;
        if (js_isnan(d)) { nan = 1; continue; }
        if (want_max) {
            if (d > best || (d == 0 && best == 0 && !(1.0 / d < 0))) best = d;
        } else {
            if (d < best || (d == 0 && best == 0 && 1.0 / d < 0)) best = d;
        }
    }
    *ret = js_number(nan ? js_nan() : best);
    return JS_OK;
}

static int bi_math_max(js_ctx *ctx, js_value t, int argc, js_value *argv, js_value *ret)
{ return bi_math_minmax(ctx, t, argc, argv, ret, 1); }
static int bi_math_min(js_ctx *ctx, js_value t, int argc, js_value *argv, js_value *ret)
{ return bi_math_minmax(ctx, t, argc, argv, ret, 0); }

static int bi_math_random(js_ctx *ctx, js_value t, int argc, js_value *argv,
                          js_value *ret)
{
    /* xorshift64*, seeded from the clock; not cryptographic and not
     * claimed to be. */
    static uint64_t st;

    (void)t; (void)argc; (void)argv;
    if (!st) {
        st = (uint64_t)now_ms(ctx);
        st ^= 0x9E3779B97F4A7C15ULL;
        if (!st) st = 88172645463325252ULL;
    }
    st ^= st >> 12;
    st ^= st << 25;
    st ^= st >> 27;
    *ret = js_number((double)((st * 2685821657736338717ULL) >> 11) /
                     9007199254740992.0);
    return JS_OK;
}

/* ================================================================== */
/* JSON                                                                */
/* ================================================================== */

#define JSON_MAX_DEPTH 32

static int json_quote(js_sbuf *b, js_string *s)
{
    unsigned long i;

    if (js_sb_putc(b, '"') != JS_OK) return JS_THROW;
    for (i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->data[i];
        switch (c) {
        case '"':  if (js_sb_puts(b, "\\\"") != JS_OK) return JS_THROW; break;
        case '\\': if (js_sb_puts(b, "\\\\") != JS_OK) return JS_THROW; break;
        case '\n': if (js_sb_puts(b, "\\n") != JS_OK) return JS_THROW; break;
        case '\r': if (js_sb_puts(b, "\\r") != JS_OK) return JS_THROW; break;
        case '\t': if (js_sb_puts(b, "\\t") != JS_OK) return JS_THROW; break;
        case '\b': if (js_sb_puts(b, "\\b") != JS_OK) return JS_THROW; break;
        case '\f': if (js_sb_puts(b, "\\f") != JS_OK) return JS_THROW; break;
        default:
            if (c < 0x20) {
                char e[8];
                snprintf(e, sizeof(e), "\\u%04x", c);
                if (js_sb_puts(b, e) != JS_OK) return JS_THROW;
            } else if (js_sb_putc(b, (char)c) != JS_OK) {
                return JS_THROW;
            }
        }
    }
    return js_sb_putc(b, '"');
}

typedef struct {
    js_ctx   *ctx;
    js_sbuf  *b;
    js_string *gap;
    js_value  replacer;
    int       depth;
} jsonw;

static int json_indent(jsonw *w, int depth)
{
    int i;

    if (!w->gap || !w->gap->len)
        return JS_OK;
    if (js_sb_putc(w->b, '\n') != JS_OK) return JS_THROW;
    for (i = 0; i < depth; i++)
        if (js_sb_put(w->b, w->gap->data, w->gap->len) != JS_OK) return JS_THROW;
    return JS_OK;
}

/* Returns 1 if something was written, 0 if the value is not serialisable
 * (undefined / function), and -1 on error. */
static int json_write(jsonw *w, js_value v)
{
    js_ctx *ctx = w->ctx;

    if (w->depth > JSON_MAX_DEPTH) {
        js_throw_error(ctx, JS_ERR_TYPE, "JSON structure is nested too deeply");
        return -1;
    }
    if (js_step(ctx) != JS_OK)
        return -1;

    if (v.type == JS_OBJECT) {
        js_value tj;
        if (js_obj_get(ctx, v.u.obj, js_str_intern(ctx, "toJSON", 6), v, &tj) != JS_OK)
            return -1;
        if (js_is_function(tj)) {
            js_value r;
            if (js_call(ctx, tj, v, 0, 0, &r) != JS_OK) return -1;
            v = r;
        }
    }

    switch (v.type) {
    case JS_UNDEFINED:
        return 0;
    case JS_NULL:
        return js_sb_puts(w->b, "null") == JS_OK ? 1 : -1;
    case JS_BOOL:
        return js_sb_puts(w->b, v.u.b ? "true" : "false") == JS_OK ? 1 : -1;
    case JS_NUMBER: {
        char buf[48];
        if (js_isnan(v.u.num) || js_isinf(v.u.num))
            return js_sb_puts(w->b, "null") == JS_OK ? 1 : -1;
        js_dtoa(v.u.num, buf);
        return js_sb_puts(w->b, buf) == JS_OK ? 1 : -1;
    }
    case JS_STRING:
        return json_quote(w->b, v.u.str) == JS_OK ? 1 : -1;
    default:
        break;
    }

    if (v.u.obj->cls == JS_CLASS_FUNCTION)
        return 0;
    if (v.u.obj->cls == JS_CLASS_NUMBER || v.u.obj->cls == JS_CLASS_STRING ||
        v.u.obj->cls == JS_CLASS_BOOLEAN) {
        js_value p;
        if (js_to_primitive(ctx, v, JS_HINT_NONE, &p) != JS_OK) return -1;
        return json_write(w, p);
    }

    w->depth++;
    if (v.u.obj->cls == JS_CLASS_ARRAY) {
        unsigned long n, i;
        if (alen(ctx, v.u.obj, &n) != JS_OK) { w->depth--; return -1; }
        if (js_sb_putc(w->b, '[') != JS_OK) { w->depth--; return -1; }
        for (i = 0; i < n; i++) {
            js_value e;
            int r;
            if (i && js_sb_putc(w->b, ',') != JS_OK) { w->depth--; return -1; }
            if (json_indent(w, w->depth) != JS_OK) { w->depth--; return -1; }
            if (aget(ctx, v.u.obj, i, &e) != JS_OK) { w->depth--; return -1; }
            r = json_write(w, e);
            if (r < 0) { w->depth--; return -1; }
            if (r == 0 && js_sb_puts(w->b, "null") != JS_OK) { w->depth--; return -1; }
        }
        w->depth--;
        if (n && json_indent(w, w->depth) != JS_OK) return -1;
        return js_sb_putc(w->b, ']') == JS_OK ? 1 : -1;
    }
    {
        uint32_t i;
        int first = 1;
        if (js_sb_putc(w->b, '{') != JS_OK) { w->depth--; return -1; }
        for (i = 0; i < v.u.obj->nprops; i++) {
            js_prop *p = &v.u.obj->props[i];
            js_value e;
            int r;
            unsigned long mark;
            if ((p->flags & JS_P_DEAD) || !(p->flags & JS_P_ENUM)) continue;
            if (js_obj_get(ctx, v.u.obj, p->key, v, &e) != JS_OK) { w->depth--; return -1; }
            mark = w->b->n;
            if (!first && js_sb_putc(w->b, ',') != JS_OK) { w->depth--; return -1; }
            if (json_indent(w, w->depth) != JS_OK) { w->depth--; return -1; }
            if (json_quote(w->b, p->key) != JS_OK) { w->depth--; return -1; }
            if (js_sb_putc(w->b, ':') != JS_OK) { w->depth--; return -1; }
            if (w->gap && w->gap->len && js_sb_putc(w->b, ' ') != JS_OK) {
                w->depth--; return -1;
            }
            r = json_write(w, e);
            if (r < 0) { w->depth--; return -1; }
            if (r == 0) {
                w->b->n = mark;          /* roll back an unserialisable member */
                if (w->b->p) w->b->p[mark] = 0;
                continue;
            }
            first = 0;
        }
        w->depth--;
        if (!first && json_indent(w, w->depth) != JS_OK) return -1;
        return js_sb_putc(w->b, '}') == JS_OK ? 1 : -1;
    }
}

static int bi_json_stringify(js_ctx *ctx, js_value t, int argc, js_value *argv,
                             js_value *ret)
{
    js_sbuf b;
    jsonw w;
    js_value space = arg(argc, argv, 2);
    int r;

    (void)t;
    js_sb_init(&b, ctx);
    memset(&w, 0, sizeof(w));
    w.ctx = ctx;
    w.b = &b;
    w.gap = 0;
    if (space.type == JS_NUMBER) {
        double d;
        char pad[12];
        int i, k;
        if (js_to_integer(ctx, space, &d) != JS_OK) { js_sb_free(&b); return JS_THROW; }
        if (d > 10) d = 10;
        k = d > 0 ? (int)d : 0;
        for (i = 0; i < k; i++) pad[i] = ' ';
        w.gap = js_str_new(ctx, pad, (unsigned long)k);
        if (!w.gap) { js_sb_free(&b); return JS_THROW; }
    } else if (space.type == JS_STRING) {
        w.gap = space.u.str;
    }
    r = json_write(&w, arg(argc, argv, 0));
    if (r < 0) { js_sb_free(&b); return JS_THROW; }
    if (r == 0) { js_sb_free(&b); *ret = js_undefined(); return JS_OK; }
    return js_sb_finish(&b, ret);
}

typedef struct {
    js_ctx     *ctx;
    const char *s;
    unsigned long n, i;
    int         depth;
    int         err;
} jsonr;

static void json_ws(jsonr *p)
{
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->i++;
        else break;
    }
}

static int json_err(jsonr *p, const char *msg)
{
    p->err = 1;
    js_throw_error(p->ctx, JS_ERR_SYNTAX, "JSON.parse: %s at position %lu",
                   msg, p->i);
    return JS_THROW;
}

static int json_value(jsonr *p, js_value *out);

static int json_string(jsonr *p, js_value *out)
{
    js_sbuf b;

    js_sb_init(&b, p->ctx);
    p->i++;                              /* opening quote */
    while (p->i < p->n && p->s[p->i] != '"') {
        char c = p->s[p->i];
        if (c == '\\') {
            p->i++;
            if (p->i >= p->n) break;
            c = p->s[p->i++];
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '/': case '"': case '\\': break;
            case 'u': {
                unsigned long cp = 0;
                int k;
                char e[4];
                int m = 0;
                if (p->i + 3 >= p->n) { js_sb_free(&b); return json_err(p, "bad \\u escape"); }
                for (k = 0; k < 4; k++) {
                    int h = p->s[p->i + k];
                    if (h >= '0' && h <= '9') h -= '0';
                    else if (h >= 'a' && h <= 'f') h = h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') h = h - 'A' + 10;
                    else { js_sb_free(&b); return json_err(p, "bad \\u escape"); }
                    cp = cp * 16 + (unsigned long)h;
                }
                p->i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && p->i + 5 < p->n &&
                    p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                    unsigned long lo = 0;
                    int ok = 1;
                    for (k = 0; k < 4; k++) {
                        int h = p->s[p->i + 2 + k];
                        if (h >= '0' && h <= '9') h -= '0';
                        else if (h >= 'a' && h <= 'f') h = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') h = h - 'A' + 10;
                        else { ok = 0; break; }
                        lo = lo * 16 + (unsigned long)h;
                    }
                    if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p->i += 6;
                    }
                }
                if (cp < 0x80) e[m++] = (char)cp;
                else if (cp < 0x800) {
                    e[m++] = (char)(0xC0 | (cp >> 6));
                    e[m++] = (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    e[m++] = (char)(0xE0 | (cp >> 12));
                    e[m++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    e[m++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    e[m++] = (char)(0xF0 | (cp >> 18));
                    e[m++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    e[m++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    e[m++] = (char)(0x80 | (cp & 0x3F));
                }
                if (js_sb_put(&b, e, (unsigned long)m) != JS_OK) {
                    js_sb_free(&b);
                    return JS_THROW;
                }
                continue;
            }
            default:
                js_sb_free(&b);
                return json_err(p, "bad escape");
            }
            if (js_sb_putc(&b, c) != JS_OK) { js_sb_free(&b); return JS_THROW; }
            continue;
        }
        if ((unsigned char)c < 0x20) { js_sb_free(&b); return json_err(p, "control character in a string"); }
        if (js_sb_putc(&b, c) != JS_OK) { js_sb_free(&b); return JS_THROW; }
        p->i++;
    }
    if (p->i >= p->n) { js_sb_free(&b); return json_err(p, "unterminated string"); }
    p->i++;
    return js_sb_finish(&b, out);
}

static int json_value(jsonr *p, js_value *out)
{
    *out = js_undefined();
    if (p->err) return JS_THROW;
    if (js_step(p->ctx) != JS_OK) return JS_THROW;
    if (++p->depth > JSON_MAX_DEPTH) { p->depth--; return json_err(p, "nested too deeply"); }
    json_ws(p);
    if (p->i >= p->n) { p->depth--; return json_err(p, "unexpected end of input"); }

    switch (p->s[p->i]) {
    case '{': {
        js_object *o = js_new_object(p->ctx);
        if (!o) { p->depth--; return JS_THROW; }
        p->i++;
        json_ws(p);
        if (p->i < p->n && p->s[p->i] == '}') { p->i++; p->depth--; *out = js_object_value(o); return JS_OK; }
        for (;;) {
            js_value k, v;
            json_ws(p);
            if (p->i >= p->n || p->s[p->i] != '"') { p->depth--; return json_err(p, "expected a key"); }
            if (json_string(p, &k) != JS_OK) { p->depth--; return JS_THROW; }
            json_ws(p);
            if (p->i >= p->n || p->s[p->i] != ':') { p->depth--; return json_err(p, "expected :"); }
            p->i++;
            if (json_value(p, &v) != JS_OK) { p->depth--; return JS_THROW; }
            if (js_obj_put(p->ctx, o, k.u.str, v) != JS_OK) { p->depth--; return JS_THROW; }
            json_ws(p);
            if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
            break;
        }
        if (p->i >= p->n || p->s[p->i] != '}') { p->depth--; return json_err(p, "expected }"); }
        p->i++;
        p->depth--;
        *out = js_object_value(o);
        return JS_OK;
    }
    case '[': {
        js_object *a = js_new_array(p->ctx);
        if (!a) { p->depth--; return JS_THROW; }
        p->i++;
        json_ws(p);
        if (p->i < p->n && p->s[p->i] == ']') { p->i++; p->depth--; *out = js_object_value(a); return JS_OK; }
        for (;;) {
            js_value v;
            if (json_value(p, &v) != JS_OK) { p->depth--; return JS_THROW; }
            if (js_array_push(p->ctx, a, v) != JS_OK) { p->depth--; return JS_THROW; }
            json_ws(p);
            if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
            break;
        }
        if (p->i >= p->n || p->s[p->i] != ']') { p->depth--; return json_err(p, "expected ]"); }
        p->i++;
        p->depth--;
        *out = js_object_value(a);
        return JS_OK;
    }
    case '"':
        p->depth--;
        return json_string(p, out);
    case 't':
        if (p->i + 4 <= p->n && memcmp(p->s + p->i, "true", 4) == 0) {
            p->i += 4; p->depth--; *out = js_bool(1); return JS_OK;
        }
        p->depth--;
        return json_err(p, "unexpected token");
    case 'f':
        if (p->i + 5 <= p->n && memcmp(p->s + p->i, "false", 5) == 0) {
            p->i += 5; p->depth--; *out = js_bool(0); return JS_OK;
        }
        p->depth--;
        return json_err(p, "unexpected token");
    case 'n':
        if (p->i + 4 <= p->n && memcmp(p->s + p->i, "null", 4) == 0) {
            p->i += 4; p->depth--; *out = js_null(); return JS_OK;
        }
        p->depth--;
        return json_err(p, "unexpected token");
    default: {
        unsigned long used = 0;
        double d;
        char c = p->s[p->i];
        if (c != '-' && !(c >= '0' && c <= '9')) {
            p->depth--;
            return json_err(p, "unexpected token");
        }
        d = js_strtod(p->s + p->i, p->n - p->i, &used);
        if (used == 0) { p->depth--; return json_err(p, "bad number"); }
        p->i += used;
        p->depth--;
        *out = js_number(d);
        return JS_OK;
    }
    }
}

/* Walk the result applying a reviver, as ES5 does. */
static int json_revive(js_ctx *ctx, js_value holder, js_string *key,
                       js_value reviver, int depth, js_value *out)
{
    js_value v, args[2], r;

    if (depth > JSON_MAX_DEPTH)
        return js_throw_error(ctx, JS_ERR_TYPE, "reviver recursion is too deep");
    if (js_obj_get(ctx, holder.u.obj, key, holder, &v) != JS_OK) return JS_THROW;
    if (v.type == JS_OBJECT) {
        if (v.u.obj->cls == JS_CLASS_ARRAY) {
            unsigned long i;
            for (i = 0; i < v.u.obj->elen; i++) {
                js_value nv;
                js_string *k = idx_key(ctx, i);
                if (!k) return JS_THROW;
                if (json_revive(ctx, v, k, reviver, depth + 1, &nv) != JS_OK)
                    return JS_THROW;
                if (nv.type == JS_UNDEFINED) v.u.obj->elems[i] = js_undefined();
                else v.u.obj->elems[i] = nv;
            }
        } else {
            uint32_t i;
            for (i = 0; i < v.u.obj->nprops; i++) {
                js_prop *p = &v.u.obj->props[i];
                js_value nv;
                if (p->flags & JS_P_DEAD) continue;
                if (json_revive(ctx, v, p->key, reviver, depth + 1, &nv) != JS_OK)
                    return JS_THROW;
                if (nv.type == JS_UNDEFINED) js_obj_delete(ctx, v.u.obj, p->key);
                else p->value = nv;
            }
        }
    }
    args[0] = js_string_value(key);
    args[1] = v;
    if (js_call(ctx, reviver, holder, 2, args, &r) != JS_OK) return JS_THROW;
    *out = r;
    return JS_OK;
}

static int bi_json_parse(js_ctx *ctx, js_value t, int argc, js_value *argv,
                         js_value *ret)
{
    js_value s;
    jsonr p;

    (void)t;
    if (js_to_string(ctx, arg(argc, argv, 0), &s) != JS_OK) return JS_THROW;
    memset(&p, 0, sizeof(p));
    p.ctx = ctx;
    p.s = s.u.str->data;
    p.n = s.u.str->len;
    if (json_value(&p, ret) != JS_OK) return JS_THROW;
    json_ws(&p);
    if (p.i != p.n)
        return json_err(&p, "trailing characters");
    if (argc > 1 && js_is_function(argv[1])) {
        js_object *holder = js_new_object(ctx);
        if (!holder) return JS_THROW;
        if (js_define_enum(ctx, holder, "", *ret) != JS_OK) return JS_THROW;
        return json_revive(ctx, js_object_value(holder), ctx->s_empty,
                           argv[1], 0, ret);
    }
    return JS_OK;
}

/* ================================================================== */
/* Date                                                                */
/* ================================================================== */

/* Days since 1970-01-01 for a proleptic Gregorian date (Hinnant's
 * algorithm), and its inverse. Valid well beyond the ES5 time range. */
static long days_from_civil(long y, long m, long d)
{
    long era, yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(long z, long *y, long *m, long *d)
{
    long era, doe, yoe, yy, doy, mp, dd, mm;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    yy = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    dd = doy - (153 * mp + 2) / 5 + 1;
    mm = mp + (mp < 10 ? 3 : -9);
    *y = yy + (mm <= 2);
    *m = mm;
    *d = dd;
}

#define MS_DAY 86400000.0

static double make_time(double y, double mo, double d, double h, double mi,
                        double s, double ms)
{
    long ym, mn;
    double days;

    if (js_isnan(y) || js_isnan(mo) || js_isnan(d) || js_isnan(h) ||
        js_isnan(mi) || js_isnan(s) || js_isnan(ms))
        return js_nan();
    if (js_fabs(y) > 400000 || js_fabs(mo) > 400000)
        return js_nan();
    ym = (long)y + (long)js_floor(mo / 12);
    mn = (long)js_fmod(mo, 12);
    if (mn < 0) mn += 12;
    days = (double)days_from_civil(ym, mn + 1, 1) + (d - 1);
    return days * MS_DAY + h * 3600000.0 + mi * 60000.0 + s * 1000.0 + ms;
}

static double clip_time(double t)
{
    if (js_isnan(t) || js_fabs(t) > 8.64e15)
        return js_nan();
    return js_trunc(t) + 0.0;
}

static void break_time(double t, long *y, long *mo, long *d, long *wd,
                       int *h, int *mi, int *s, int *ms)
{
    double day = js_floor(t / MS_DAY);
    double rem = t - day * MS_DAY;

    civil_from_days((long)day, y, mo, d);
    *wd = (long)js_fmod(day + 4, 7);
    if (*wd < 0) *wd += 7;
    *h = (int)js_floor(rem / 3600000.0);
    rem -= (double)*h * 3600000.0;
    *mi = (int)js_floor(rem / 60000.0);
    rem -= (double)*mi * 60000.0;
    *s = (int)js_floor(rem / 1000.0);
    *ms = (int)(rem - (double)*s * 1000.0);
}

/* ISO 8601 only; anything else is NaN. There is no timezone database in
 * the kernel, so an absent offset is read as UTC and local time is UTC. */
static double parse_date(const char *s, unsigned long n)
{
    long y = 0, mo = 1, d = 1;
    int h = 0, mi = 0, sec = 0, ms = 0, tzs = 0, tzh = 0, tzm = 0;
    unsigned long i = 0;
    int neg = 0;

    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < n && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
    if (i + 4 > n) return js_nan();
    { int k; for (k = 0; k < 4; k++) { if (s[i] < '0' || s[i] > '9') return js_nan(); y = y * 10 + (s[i++] - '0'); } }
    if (neg) y = -y;
    if (i < n && s[i] == '-') {
        i++;
        if (i + 2 > n) return js_nan();
        mo = (s[i] - '0') * 10 + (s[i + 1] - '0');
        i += 2;
        if (i < n && s[i] == '-') {
            i++;
            if (i + 2 > n) return js_nan();
            d = (s[i] - '0') * 10 + (s[i + 1] - '0');
            i += 2;
        }
    }
    if (i < n && (s[i] == 'T' || s[i] == ' ')) {
        i++;
        if (i + 5 > n) return js_nan();
        h = (s[i] - '0') * 10 + (s[i + 1] - '0');
        if (s[i + 2] != ':') return js_nan();
        mi = (s[i + 3] - '0') * 10 + (s[i + 4] - '0');
        i += 5;
        if (i + 3 <= n && s[i] == ':') {
            sec = (s[i + 1] - '0') * 10 + (s[i + 2] - '0');
            i += 3;
            if (i < n && s[i] == '.') {
                int k = 0;
                i++;
                while (i < n && s[i] >= '0' && s[i] <= '9') {
                    if (k < 3) ms = ms * 10 + (s[i] - '0');
                    k++;
                    i++;
                }
                while (k < 3) { ms *= 10; k++; }
            }
        }
        if (i < n && (s[i] == 'Z' || s[i] == 'z')) i++;
        else if (i + 5 <= n && (s[i] == '+' || s[i] == '-')) {
            tzs = s[i] == '-' ? -1 : 1;
            tzh = (s[i + 1] - '0') * 10 + (s[i + 2] - '0');
            tzm = (s[i + 4] - '0') * 10 + (s[i + 5 - 1] - '0');
            i += 6;
        }
    }
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i != n) return js_nan();
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 24 || mi > 59 || sec > 59)
        return js_nan();
    return clip_time(make_time((double)y, (double)(mo - 1), (double)d,
                               (double)h, (double)mi, (double)sec, (double)ms)
                     - (double)tzs * ((double)tzh * 3600000.0 + (double)tzm * 60000.0));
}

static int this_date(js_ctx *ctx, js_value t, double *out)
{
    if (t.type != JS_OBJECT || t.u.obj->cls != JS_CLASS_DATE)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "Date.prototype method called on a non-Date");
    *out = t.u.obj->prim.type == JS_NUMBER ? t.u.obj->prim.u.num : js_nan();
    return JS_OK;
}

static int bi_date_ctor(js_ctx *ctx, js_value t, int argc, js_value *argv,
                        js_value *ret)
{
    js_object *o;
    double tv = 0;

    (void)t;
    if (!ctx->new_target) {
        /* Date() as a plain call returns a string, per ES5. */
        double n = now_ms(ctx);
        long y, mo, d, wd;
        int h, mi, s, ms;
        char buf[64];
        break_time(n, &y, &mo, &d, &wd, &h, &mi, &s, &ms);
        snprintf(buf, sizeof(buf), "%04ld-%02ld-%02ldT%02d:%02d:%02dZ",
                 y, mo, d, h, mi, s);
        return ret_str(ctx, buf, strlen(buf), ret);
    }
    if (argc == 0) {
        tv = clip_time(js_floor(now_ms(ctx)));
    } else if (argc == 1) {
        js_value p;
        if (js_to_primitive(ctx, argv[0], JS_HINT_NONE, &p) != JS_OK) return JS_THROW;
        if (p.type == JS_STRING) {
            tv = parse_date(p.u.str->data, p.u.str->len);
        } else {
            double d;
            if (js_to_number(ctx, p, &d) != JS_OK) return JS_THROW;
            tv = clip_time(d);
        }
    } else {
        double f[7] = { 0, 0, 1, 0, 0, 0, 0 };
        int i;
        for (i = 0; i < 7 && i < argc; i++)
            if (js_to_number(ctx, argv[i], &f[i]) != JS_OK) return JS_THROW;
        for (i = 0; i < 7; i++)
            f[i] = js_isnan(f[i]) ? js_nan() : js_trunc(f[i]);
        if (f[0] >= 0 && f[0] <= 99 && !js_isnan(f[0])) f[0] += 1900;
        tv = clip_time(make_time(f[0], f[1], f[2], f[3], f[4], f[5], f[6]));
    }
    o = js_obj_alloc(ctx, JS_CLASS_DATE, ctx->proto[P_DATE]);
    if (!o) return JS_THROW;
    o->prim = js_number(tv);
    *ret = js_object_value(o);
    return JS_OK;
}

static int bi_date_now(js_ctx *ctx, js_value t, int argc, js_value *argv,
                       js_value *ret)
{
    (void)t; (void)argc; (void)argv;
    *ret = js_number(clip_time(js_floor(now_ms(ctx))));
    return JS_OK;
}

static int bi_date_parse(js_ctx *ctx, js_value t, int argc, js_value *argv,
                         js_value *ret)
{
    js_value s;
    (void)t;
    if (js_to_string(ctx, arg(argc, argv, 0), &s) != JS_OK) return JS_THROW;
    *ret = js_number(parse_date(s.u.str->data, s.u.str->len));
    return JS_OK;
}

static int bi_date_utc(js_ctx *ctx, js_value t, int argc, js_value *argv,
                       js_value *ret)
{
    double f[7] = { 0, 0, 1, 0, 0, 0, 0 };
    int i;

    (void)t;
    for (i = 0; i < 7 && i < argc; i++)
        if (js_to_number(ctx, argv[i], &f[i]) != JS_OK) return JS_THROW;
    for (i = 0; i < 7; i++)
        f[i] = js_isnan(f[i]) ? js_nan() : js_trunc(f[i]);
    if (f[0] >= 0 && f[0] <= 99 && !js_isnan(f[0])) f[0] += 1900;
    *ret = js_number(clip_time(make_time(f[0], f[1], f[2], f[3], f[4], f[5], f[6])));
    return JS_OK;
}

enum { D_TIME, D_YEAR, D_MONTH, D_DATE, D_DAY, D_HOURS, D_MIN, D_SEC, D_MS, D_TZ };

static int date_field(js_ctx *ctx, js_value t, js_value *ret, int which)
{
    double tv = 0;
    long y, mo, d, wd;
    int h, mi, s, ms;

    if (this_date(ctx, t, &tv) != JS_OK) return JS_THROW;
    if (which == D_TIME) { *ret = js_number(tv); return JS_OK; }
    if (which == D_TZ) { *ret = js_number(0); return JS_OK; }
    if (js_isnan(tv)) { *ret = js_number(js_nan()); return JS_OK; }
    break_time(tv, &y, &mo, &d, &wd, &h, &mi, &s, &ms);
    switch (which) {
    case D_YEAR:  *ret = js_number((double)y); break;
    case D_MONTH: *ret = js_number((double)(mo - 1)); break;
    case D_DATE:  *ret = js_number((double)d); break;
    case D_DAY:   *ret = js_number((double)wd); break;
    case D_HOURS: *ret = js_number((double)h); break;
    case D_MIN:   *ret = js_number((double)mi); break;
    case D_SEC:   *ret = js_number((double)s); break;
    default:      *ret = js_number((double)ms); break;
    }
    return JS_OK;
}

#define DATEF(nm, code) \
    static int bi_date_##nm(js_ctx *c, js_value t, int a, js_value *v, js_value *r) \
    { (void)a; (void)v; return date_field(c, t, r, code); }

DATEF(gettime, D_TIME) DATEF(getyear, D_YEAR) DATEF(getmonth, D_MONTH)
DATEF(getdate, D_DATE) DATEF(getday, D_DAY) DATEF(gethours, D_HOURS)
DATEF(getmin, D_MIN) DATEF(getsec, D_SEC) DATEF(getms, D_MS)
DATEF(gettz, D_TZ)

static int bi_date_settime(js_ctx *ctx, js_value t, int argc, js_value *argv,
                           js_value *ret)
{
    double d, old;

    if (this_date(ctx, t, &old) != JS_OK) return JS_THROW;
    if (js_to_number(ctx, arg(argc, argv, 0), &d) != JS_OK) return JS_THROW;
    t.u.obj->prim = js_number(clip_time(d));
    *ret = t.u.obj->prim;
    return JS_OK;
}

static int bi_date_toiso(js_ctx *ctx, js_value t, int argc, js_value *argv,
                         js_value *ret)
{
    double tv = 0;
    long y, mo, d, wd;
    int h, mi, s, ms;
    char buf[64];

    (void)argc; (void)argv;
    if (this_date(ctx, t, &tv) != JS_OK) return JS_THROW;
    if (js_isnan(tv))
        return js_throw_error(ctx, JS_ERR_RANGE, "invalid time value");
    break_time(tv, &y, &mo, &d, &wd, &h, &mi, &s, &ms);
    if (y >= 0 && y <= 9999)
        snprintf(buf, sizeof(buf), "%04ld-%02ld-%02ldT%02d:%02d:%02d.%03dZ",
                 y, mo, d, h, mi, s, ms);
    else
        snprintf(buf, sizeof(buf), "%+07ld-%02ld-%02ldT%02d:%02d:%02d.%03dZ",
                 y, mo, d, h, mi, s, ms);
    return ret_str(ctx, buf, strlen(buf), ret);
}

static int bi_date_tostring(js_ctx *ctx, js_value t, int argc, js_value *argv,
                            js_value *ret)
{
    static const char *wdn[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    double tv = 0;
    long y, mo, d, wd;
    int h, mi, s, ms;
    char buf[80];

    (void)argc; (void)argv;
    if (this_date(ctx, t, &tv) != JS_OK) return JS_THROW;
    if (js_isnan(tv))
        return ret_str(ctx, "Invalid Date", 12, ret);
    break_time(tv, &y, &mo, &d, &wd, &h, &mi, &s, &ms);
    snprintf(buf, sizeof(buf), "%s %s %02ld %04ld %02d:%02d:%02d GMT+0000 (UTC)",
             wdn[wd], mon[mo - 1], d, y, h, mi, s);
    return ret_str(ctx, buf, strlen(buf), ret);
}

static int bi_date_valueof(js_ctx *ctx, js_value t, int argc, js_value *argv,
                           js_value *ret)
{ (void)argc; (void)argv; return date_field(ctx, t, ret, D_TIME); }

/* ================================================================== */
/* Error                                                               */
/* ================================================================== */

static const char *err_names[JS_ERR__COUNT] = {
    "Error", "TypeError", "RangeError", "SyntaxError",
    "ReferenceError", "EvalError", "URIError"
};

js_object *js_new_error(js_ctx *ctx, int kind, const char *msg)
{
    js_object *e;

    if (kind < 0 || kind >= JS_ERR__COUNT)
        kind = JS_ERR_ERROR;
    e = js_obj_alloc(ctx, JS_CLASS_ERROR, ctx->proto[P_ERROR + kind]);
    if (!e)
        return 0;
    if (msg && msg[0]) {
        js_prop *p = js_add_prop(ctx, e, ctx->s_message, JS_P_HIDDEN);
        if (!p) return 0;
        p->value = js_mkcstring(ctx, msg);
    }
    return e;
}

static int error_ctor_kind(js_ctx *ctx, js_object *f)
{
    int i;

    for (i = 0; i < JS_ERR__COUNT; i++) {
        js_value c;
        if (js_obj_get(ctx, ctx->proto[P_ERROR + i], ctx->s_constructor,
                       js_object_value(ctx->proto[P_ERROR + i]), &c) != JS_OK)
            return 0;
        if (c.type == JS_OBJECT && c.u.obj == f)
            return i;
    }
    return 0;
}

static int bi_error_ctor(js_ctx *ctx, js_value t, int argc, js_value *argv,
                         js_value *ret)
{
    int kind = ctx->new_target ? error_ctor_kind(ctx, ctx->new_target) : 0;
    js_object *e;

    (void)t;
    e = js_obj_alloc(ctx, JS_CLASS_ERROR, ctx->proto[P_ERROR + kind]);
    if (!e) return JS_THROW;
    if (argc > 0 && argv[0].type != JS_UNDEFINED) {
        js_value s;
        js_prop *p;
        if (js_to_string(ctx, argv[0], &s) != JS_OK) return JS_THROW;
        p = js_add_prop(ctx, e, ctx->s_message, JS_P_HIDDEN);
        if (!p) return JS_THROW;
        p->value = s;
    }
    *ret = js_object_value(e);
    return JS_OK;
}

static int bi_error_tostring(js_ctx *ctx, js_value t, int argc, js_value *argv,
                             js_value *ret)
{
    js_value n, m, ns, msv;
    js_sbuf b;

    (void)argc; (void)argv;
    if (t.type != JS_OBJECT)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "Error.prototype.toString on a non-object");
    if (js_obj_get(ctx, t.u.obj, ctx->s_name, t, &n) != JS_OK) return JS_THROW;
    if (js_obj_get(ctx, t.u.obj, ctx->s_message, t, &m) != JS_OK) return JS_THROW;
    if (n.type == JS_UNDEFINED) n = js_mkcstring(ctx, "Error");
    if (js_to_string(ctx, n, &ns) != JS_OK) return JS_THROW;
    if (m.type == JS_UNDEFINED) msv = js_string_value(ctx->s_empty);
    else if (js_to_string(ctx, m, &msv) != JS_OK) return JS_THROW;
    if (msv.u.str->len == 0) { *ret = ns; return JS_OK; }
    if (ns.u.str->len == 0) { *ret = msv; return JS_OK; }
    js_sb_init(&b, ctx);
    if (js_sb_put(&b, ns.u.str->data, ns.u.str->len) != JS_OK ||
        js_sb_puts(&b, ": ") != JS_OK ||
        js_sb_put(&b, msv.u.str->data, msv.u.str->len) != JS_OK) {
        js_sb_free(&b);
        return JS_THROW;
    }
    return js_sb_finish(&b, ret);
}

/* ================================================================== */
/* Global functions                                                    */
/* ================================================================== */

static int bi_parseint(js_ctx *ctx, js_value t, int argc, js_value *argv,
                       js_value *ret)
{
    js_value s;
    const char *p;
    unsigned long n, i = 0;
    int neg = 0, radix = 0, any = 0;
    double v = 0, rr = 0;

    (void)t;
    if (js_to_string(ctx, arg(argc, argv, 0), &s) != JS_OK) return JS_THROW;
    if (argc > 1 && js_to_integer(ctx, argv[1], &rr) != JS_OK) return JS_THROW;
    radix = (int)rr;
    p = s.u.str->data;
    n = s.u.str->len;
    while (i < n && is_space((unsigned char)p[i])) i++;
    if (i < n && (p[i] == '+' || p[i] == '-')) { neg = p[i] == '-'; i++; }
    if (radix == 0) {
        if (i + 1 < n && p[i] == '0' && (p[i + 1] == 'x' || p[i + 1] == 'X')) {
            radix = 16; i += 2;
        } else {
            radix = 10;
        }
    } else if (radix == 16) {
        if (i + 1 < n && p[i] == '0' && (p[i + 1] == 'x' || p[i + 1] == 'X'))
            i += 2;
    }
    if (radix < 2 || radix > 36) { *ret = js_number(js_nan()); return JS_OK; }
    for (; i < n; i++) {
        int c = (unsigned char)p[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if (d >= radix) break;
        v = v * radix + d;
        any = 1;
    }
    *ret = js_number(any ? (neg ? -v : v) : js_nan());
    return JS_OK;
}

static int bi_parsefloat(js_ctx *ctx, js_value t, int argc, js_value *argv,
                         js_value *ret)
{
    js_value s;
    unsigned long i = 0, used = 0;
    double d;

    (void)t;
    if (js_to_string(ctx, arg(argc, argv, 0), &s) != JS_OK) return JS_THROW;
    while (i < s.u.str->len && is_space((unsigned char)s.u.str->data[i])) i++;
    d = js_strtod(s.u.str->data + i, s.u.str->len - i, &used);
    *ret = js_number(used ? d : js_nan());
    return JS_OK;
}

static int bi_isnan(js_ctx *ctx, js_value t, int argc, js_value *argv,
                    js_value *ret)
{
    double d;
    (void)t;
    if (js_to_number(ctx, arg(argc, argv, 0), &d) != JS_OK) return JS_THROW;
    *ret = js_bool(js_isnan(d));
    return JS_OK;
}

static int bi_isfinite(js_ctx *ctx, js_value t, int argc, js_value *argv,
                       js_value *ret)
{
    double d;
    (void)t;
    if (js_to_number(ctx, arg(argc, argv, 0), &d) != JS_OK) return JS_THROW;
    *ret = js_bool(!js_isnan(d) && !js_isinf(d));
    return JS_OK;
}

static int uri_encode(js_ctx *ctx, js_value t, int argc, js_value *argv,
                      js_value *ret, const char *keep)
{
    js_value s;
    js_sbuf b;
    unsigned long i;
    static const char hex[] = "0123456789ABCDEF";

    (void)t;
    if (js_to_string(ctx, arg(argc, argv, 0), &s) != JS_OK) return JS_THROW;
    js_sb_init(&b, ctx);
    for (i = 0; i < s.u.str->len; i++) {
        unsigned char c = (unsigned char)s.u.str->data[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || strchr(keep, c) != 0;
        if (ok) {
            if (js_sb_putc(&b, (char)c) != JS_OK) { js_sb_free(&b); return JS_THROW; }
        } else {
            char e[3];
            e[0] = '%'; e[1] = hex[c >> 4]; e[2] = hex[c & 15];
            if (js_sb_put(&b, e, 3) != JS_OK) { js_sb_free(&b); return JS_THROW; }
        }
    }
    return js_sb_finish(&b, ret);
}

static int bi_encodeuricomponent(js_ctx *ctx, js_value t, int argc,
                                 js_value *argv, js_value *ret)
{ return uri_encode(ctx, t, argc, argv, ret, "-_.!~*'()"); }

static int bi_encodeuri(js_ctx *ctx, js_value t, int argc, js_value *argv,
                        js_value *ret)
{ return uri_encode(ctx, t, argc, argv, ret, "-_.!~*'();/?:@&=+$,#"); }

static int bi_decodeuri(js_ctx *ctx, js_value t, int argc, js_value *argv,
                        js_value *ret)
{
    js_value s;
    js_sbuf b;
    unsigned long i;

    (void)t;
    if (js_to_string(ctx, arg(argc, argv, 0), &s) != JS_OK) return JS_THROW;
    js_sb_init(&b, ctx);
    for (i = 0; i < s.u.str->len; i++) {
        char c = s.u.str->data[i];
        if (c == '%') {
            int h1, h2;
            if (i + 2 >= s.u.str->len) {
                js_sb_free(&b);
                return js_throw_error(ctx, JS_ERR_URI, "malformed URI sequence");
            }
            h1 = s.u.str->data[i + 1];
            h2 = s.u.str->data[i + 2];
            h1 = (h1 >= '0' && h1 <= '9') ? h1 - '0'
               : (h1 >= 'a' && h1 <= 'f') ? h1 - 'a' + 10
               : (h1 >= 'A' && h1 <= 'F') ? h1 - 'A' + 10 : -1;
            h2 = (h2 >= '0' && h2 <= '9') ? h2 - '0'
               : (h2 >= 'a' && h2 <= 'f') ? h2 - 'a' + 10
               : (h2 >= 'A' && h2 <= 'F') ? h2 - 'A' + 10 : -1;
            if (h1 < 0 || h2 < 0) {
                js_sb_free(&b);
                return js_throw_error(ctx, JS_ERR_URI, "malformed URI sequence");
            }
            c = (char)(h1 * 16 + h2);
            i += 2;
        }
        if (js_sb_putc(&b, c) != JS_OK) { js_sb_free(&b); return JS_THROW; }
    }
    return js_sb_finish(&b, ret);
}

static int bi_eval(js_ctx *ctx, js_value t, int argc, js_value *argv,
                   js_value *ret)
{
    (void)t; (void)argc; (void)argv; (void)ret;
    return js_throw_error(ctx, JS_ERR_EVAL,
        "eval is not implemented; this engine cannot compile code at run time");
}

static int bi_function_ctor(js_ctx *ctx, js_value t, int argc, js_value *argv,
                            js_value *ret)
{
    (void)t; (void)argc; (void)argv; (void)ret;
    return js_throw_error(ctx, JS_ERR_EVAL,
        "the Function constructor is not implemented; this engine cannot "
        "compile code at run time");
}

static int bi_console_log(js_ctx *ctx, js_value t, int argc, js_value *argv,
                          js_value *ret)
{
    js_sbuf b;
    int i;

    (void)t;
    *ret = js_undefined();
    js_sb_init(&b, ctx);
    for (i = 0; i < argc; i++) {
        js_value s;
        if (i && js_sb_putc(&b, ' ') != JS_OK) { js_sb_free(&b); return JS_THROW; }
        if (js_to_string(ctx, argv[i], &s) != JS_OK) { js_sb_free(&b); return JS_THROW; }
        if (js_sb_put(&b, s.u.str->data, s.u.str->len) != JS_OK) {
            js_sb_free(&b);
            return JS_THROW;
        }
    }
    if (ctx->cfg.print)
        ctx->cfg.print(ctx->cfg.user, b.p ? b.p : "");
    else
        printf("%s\n", b.p ? b.p : "");
    js_sb_free(&b);
    return JS_OK;
}

/* ================================================================== */
/* Primitive wrappers                                                  */
/* ================================================================== */

int js_wrap_primitive(js_ctx *ctx, js_value v, js_value *out)
{
    int cls, pi;
    js_object *o;

    switch (v.type) {
    case JS_STRING: cls = JS_CLASS_STRING;  pi = P_STRING;  break;
    case JS_NUMBER: cls = JS_CLASS_NUMBER;  pi = P_NUMBER;  break;
    case JS_BOOL:   cls = JS_CLASS_BOOLEAN; pi = P_BOOLEAN; break;
    default:
        *out = v;
        return JS_OK;
    }
    o = js_obj_alloc(ctx, cls, ctx->proto[pi]);
    if (!o) { *out = js_undefined(); return JS_THROW; }
    o->prim = v;
    *out = js_object_value(o);
    return JS_OK;
}

/* ================================================================== */
/* Promise and microtasks                                              */
/* ================================================================== */

static js_object *promise_alloc(js_ctx *ctx)
{
    return js_obj_alloc(ctx, JS_CLASS_PROMISE, ctx->proto[P_PROMISE]);
}

js_value js_promise_new(js_ctx *ctx)
{
    js_object *p;

    if (!ctx || ctx->fatal)
        return js_undefined();
    p = promise_alloc(ctx);
    return p ? js_object_value(p) : js_undefined();
}

static int promise_queue(js_ctx *ctx, js_object *source,
                         js_promise_reaction *reaction)
{
    js_promise_job *job = (js_promise_job *)js_alloc(
        ctx, sizeof(js_promise_job));

    if (!job)
        return JS_THROW;
    job->source = source;
    job->reaction = reaction;
    if (ctx->jobs_tail)
        ctx->jobs_tail->next = job;
    else
        ctx->jobs_head = job;
    ctx->jobs_tail = job;
    return JS_OK;
}

static int promise_add_reaction(js_ctx *ctx, js_object *source,
                                js_promise_reaction *reaction)
{
    js_promise_reaction **tail;

    if (source->promise_state)
        return promise_queue(ctx, source, reaction);
    tail = &source->promise_reactions;
    while (*tail)
        tail = &(*tail)->next;
    *tail = reaction;
    return JS_OK;
}

static int promise_settle(js_ctx *ctx, js_object *promise, int state,
                          js_value value)
{
    js_promise_reaction *reaction, *next;

    if (!promise || promise->cls != JS_CLASS_PROMISE)
        return js_throw_error(ctx, JS_ERR_TYPE, "value is not a Promise");
    if (promise->promise_state)
        return JS_OK;                    /* first resolver wins */

    if (state == 1 && js_is_promise(value)) {
        js_object *other = value.u.obj;

        if (other == promise) {
            js_object *e = js_new_error(
                ctx, JS_ERR_TYPE, "a Promise cannot resolve to itself");
            if (!e)
                return JS_THROW;
            state = 2;
            value = js_object_value(e);
        } else if (!other->promise_state) {
            reaction = (js_promise_reaction *)js_alloc(
                ctx, sizeof(js_promise_reaction));
            if (!reaction)
                return JS_THROW;
            reaction->on_fulfilled = js_undefined();
            reaction->on_rejected = js_undefined();
            reaction->child = promise;
            return promise_add_reaction(ctx, other, reaction);
        } else {
            state = other->promise_state;
            value = other->promise_result;
        }
    }

    promise->promise_state = (uint8_t)state;
    promise->promise_result = value;
    reaction = promise->promise_reactions;
    promise->promise_reactions = 0;
    while (reaction) {
        next = reaction->next;
        reaction->next = 0;
        if (promise_queue(ctx, promise, reaction) != JS_OK)
            return JS_THROW;
        reaction = next;
    }
    return JS_OK;
}

int js_promise_resolve(js_ctx *ctx, js_value promise, js_value value)
{
    if (!js_is_promise(promise))
        return js_throw_error(ctx, JS_ERR_TYPE, "value is not a Promise");
    return promise_settle(ctx, promise.u.obj, 1, value);
}

int js_promise_reject(js_ctx *ctx, js_value promise, js_value reason)
{
    if (!js_is_promise(promise))
        return js_throw_error(ctx, JS_ERR_TYPE, "value is not a Promise");
    return promise_settle(ctx, promise.u.obj, 2, reason);
}

static int promise_then(js_ctx *ctx, js_value t, int argc, js_value *argv,
                        js_value *ret)
{
    js_promise_reaction *reaction;
    js_value child;

    if (!js_is_promise(t))
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "Promise.then called on a non-Promise");
    child = js_promise_new(ctx);
    if (!js_is_promise(child))
        return JS_THROW;
    reaction = (js_promise_reaction *)js_alloc(
        ctx, sizeof(js_promise_reaction));
    if (!reaction)
        return JS_THROW;
    reaction->on_fulfilled =
        argc > 0 && js_is_function(argv[0]) ? argv[0] : js_undefined();
    reaction->on_rejected =
        argc > 1 && js_is_function(argv[1]) ? argv[1] : js_undefined();
    reaction->child = child.u.obj;
    if (promise_add_reaction(ctx, t.u.obj, reaction) != JS_OK)
        return JS_THROW;
    *ret = child;
    return JS_OK;
}

static int promise_catch(js_ctx *ctx, js_value t, int argc, js_value *argv,
                         js_value *ret)
{
    js_value args[2];

    args[0] = js_undefined();
    args[1] = argc > 0 ? argv[0] : js_undefined();
    return promise_then(ctx, t, 2, args, ret);
}

static int promise_resolver(js_ctx *ctx, js_value t, int argc, js_value *argv,
                            js_value *ret)
{
    js_value value = argc ? argv[0] : js_undefined();

    *ret = js_undefined();
    if (!js_is_promise(t))
        return JS_OK;
    return js_promise_resolve(ctx, t, value);
}

static int promise_rejecter(js_ctx *ctx, js_value t, int argc, js_value *argv,
                            js_value *ret)
{
    js_value reason = argc ? argv[0] : js_undefined();

    *ret = js_undefined();
    if (!js_is_promise(t))
        return JS_OK;
    return js_promise_reject(ctx, t, reason);
}

static int bound_native(js_ctx *ctx, js_value this_value, js_native native,
                        const char *name, int argc, js_value *argv,
                        js_value *ret)
{
    js_object *fn = js_new_native(ctx, native, name, 1);
    js_value bind, args[4];
    int i;

    if (!fn)
        return JS_THROW;
    if (argc < 0 || argc > 3)
        return js_throw_error(ctx, JS_ERR_RANGE,
                              "too many bound native arguments");
    if (js_get(ctx, js_object_value(fn), "bind", &bind) != JS_OK)
        return JS_THROW;
    args[0] = this_value;
    for (i = 0; i < argc; i++)
        args[i + 1] = argv[i];
    return js_call(ctx, bind, js_object_value(fn), argc + 1, args, ret);
}

static int bound_resolver(js_ctx *ctx, js_value promise, js_native native,
                          const char *name, js_value *ret)
{
    return bound_native(ctx, promise, native, name, 0, 0, ret);
}

static int promise_ctor(js_ctx *ctx, js_value t, int argc, js_value *argv,
                        js_value *ret)
{
    js_value promise, resolve, reject, args[2], ignored, reason;
    int rc;

    (void)t;
    if (argc < 1 || !js_is_function(argv[0]))
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "Promise resolver is not a function");
    promise = js_promise_new(ctx);
    if (!js_is_promise(promise))
        return JS_THROW;
    if (bound_resolver(ctx, promise, promise_resolver,
                       "resolve", &resolve) != JS_OK ||
        bound_resolver(ctx, promise, promise_rejecter,
                       "reject", &reject) != JS_OK)
        return JS_THROW;
    args[0] = resolve;
    args[1] = reject;
    rc = js_call(ctx, argv[0], js_undefined(), 2, args, &ignored);
    if (rc != JS_OK) {
        reason = js_exception(ctx);
        if (ctx->fatal)
            return JS_THROW;
        js_clear_exception(ctx);
        if (js_promise_reject(ctx, promise, reason) != JS_OK)
            return JS_THROW;
    }
    *ret = promise;
    return JS_OK;
}

static int promise_static_resolve(js_ctx *ctx, js_value t, int argc,
                                  js_value *argv, js_value *ret)
{
    js_value value = argc ? argv[0] : js_undefined();
    (void)t;

    if (js_is_promise(value)) {
        *ret = value;
        return JS_OK;
    }
    *ret = js_promise_new(ctx);
    if (!js_is_promise(*ret))
        return JS_THROW;
    return js_promise_resolve(ctx, *ret, value);
}

static int promise_static_reject(js_ctx *ctx, js_value t, int argc,
                                 js_value *argv, js_value *ret)
{
    js_value reason = argc ? argv[0] : js_undefined();
    (void)t;

    *ret = js_promise_new(ctx);
    if (!js_is_promise(*ret))
        return JS_THROW;
    return js_promise_reject(ctx, *ret, reason);
}

#define ARRAYBUFFER_BYTE_MAX (8UL * 1024UL * 1024UL)

static int arraybuffer_ctor(js_ctx *ctx, js_value t, int argc,
                            js_value *argv, js_value *ret)
{
    uint32_t len = 0;
    (void)t;

    if (argc && js_to_uint32(ctx, argv[0], &len) != JS_OK)
        return JS_THROW;
    if (len > ARRAYBUFFER_BYTE_MAX)
        return js_throw_error(ctx, JS_ERR_RANGE,
                              "ArrayBuffer is too large");
    *ret = js_arraybuffer_new(ctx, 0, len);
    return js_is_arraybuffer(*ret) ? JS_OK : JS_THROW;
}

static int arraybuffer_slice(js_ctx *ctx, js_value t, int argc,
                             js_value *argv, js_value *ret)
{
    const uint8_t *data;
    unsigned long len;
    int32_t begin = 0, end;

    data = (const uint8_t *)js_arraybuffer_data(t, &len);
    if (!data)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "ArrayBuffer.slice receiver is invalid");
    end = (int32_t)len;
    if (argc > 0 && js_to_int32(ctx, argv[0], &begin) != JS_OK)
        return JS_THROW;
    if (argc > 1 && js_to_int32(ctx, argv[1], &end) != JS_OK)
        return JS_THROW;
    if (begin < 0) begin += (int32_t)len;
    if (end < 0) end += (int32_t)len;
    if (begin < 0) begin = 0;
    if (end < begin) end = begin;
    if ((unsigned long)begin > len) begin = (int32_t)len;
    if ((unsigned long)end > len) end = (int32_t)len;
    *ret = js_arraybuffer_new(ctx, data + begin,
                             (unsigned long)(end - begin));
    return js_is_arraybuffer(*ret) ? JS_OK : JS_THROW;
}

static int promise_all_item(js_ctx *ctx, js_value t, int argc,
                            js_value *argv, js_value *ret)
{
    js_value values, result, remaining;
    uint32_t index, left;

    if (!js_is_object(t) || argc < 2 ||
        js_to_uint32(ctx, argv[0], &index) != JS_OK ||
        js_get(ctx, t, "values", &values) != JS_OK ||
        js_get(ctx, t, "result", &result) != JS_OK ||
        js_get(ctx, t, "remaining", &remaining) != JS_OK ||
        !js_is_object(values) ||
        js_to_uint32(ctx, remaining, &left) != JS_OK)
        return JS_THROW;
    if (aput(ctx, values.u.obj, index, argv[1]) != JS_OK)
        return JS_THROW;
    if (left)
        left--;
    if (js_set(ctx, t, "remaining", js_number((double)left)) != JS_OK)
        return JS_THROW;
    *ret = js_undefined();
    return left ? JS_OK : js_promise_resolve(ctx, result, values);
}

static int promise_static_all(js_ctx *ctx, js_value t, int argc,
                              js_value *argv, js_value *ret)
{
    js_object *input, *values, *state;
    js_value result, statev, reject;
    unsigned long n, i;
    (void)t;

    if (argc < 1)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "Promise.all requires an array-like value");
    if (this_object(ctx, argv[0], &input) != JS_OK)
        return JS_THROW;
    if (alen(ctx, input, &n) != JS_OK)
        return JS_THROW;
    if (n > 65535)
        return js_throw_error(ctx, JS_ERR_RANGE,
                              "Promise.all input is too large");
    result = js_promise_new(ctx);
    values = js_new_array(ctx);
    state = js_new_object(ctx);
    if (!js_is_promise(result) || !values || !state)
        return JS_THROW;
    for (i = 0; i < n; i++)
        if (js_array_push(ctx, values, js_undefined()) != JS_OK)
            return JS_THROW;
    statev = js_object_value(state);
    if (js_set(ctx, statev, "values", js_object_value(values)) != JS_OK ||
        js_set(ctx, statev, "result", result) != JS_OK ||
        js_set(ctx, statev, "remaining", js_number((double)n)) != JS_OK ||
        bound_resolver(ctx, result, promise_rejecter,
                       "reject", &reject) != JS_OK)
        return JS_THROW;
    if (!n) {
        *ret = result;
        return js_promise_resolve(ctx, result, js_object_value(values));
    }
    for (i = 0; i < n; i++) {
        js_value item, normalized, on_item, args[2], index;

        if (aget(ctx, input, i, &item) != JS_OK ||
            promise_static_resolve(ctx, js_undefined(), 1,
                                   &item, &normalized) != JS_OK)
            return JS_THROW;
        index = js_number((double)i);
        if (bound_native(ctx, statev, promise_all_item, "all item",
                         1, &index, &on_item) != JS_OK)
            return JS_THROW;
        args[0] = on_item;
        args[1] = reject;
        if (promise_then(ctx, normalized, 2, args, &item) != JS_OK)
            return JS_THROW;
    }
    *ret = result;
    return JS_OK;
}

static int promise_static_race(js_ctx *ctx, js_value t, int argc,
                               js_value *argv, js_value *ret)
{
    js_object *input;
    js_value result, resolve, reject;
    unsigned long n, i;
    (void)t;

    if (argc < 1)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "Promise.race requires an array-like value");
    if (this_object(ctx, argv[0], &input) != JS_OK)
        return JS_THROW;
    if (alen(ctx, input, &n) != JS_OK)
        return JS_THROW;
    if (n > 65535)
        return js_throw_error(ctx, JS_ERR_RANGE,
                              "Promise.race input is too large");
    result = js_promise_new(ctx);
    if (!js_is_promise(result) ||
        bound_resolver(ctx, result, promise_resolver,
                       "resolve", &resolve) != JS_OK ||
        bound_resolver(ctx, result, promise_rejecter,
                       "reject", &reject) != JS_OK)
        return JS_THROW;
    for (i = 0; i < n; i++) {
        js_value item, normalized, ignored, args[2];

        if (aget(ctx, input, i, &item) != JS_OK ||
            promise_static_resolve(ctx, js_undefined(), 1,
                                   &item, &normalized) != JS_OK)
            return JS_THROW;
        args[0] = resolve;
        args[1] = reject;
        if (promise_then(ctx, normalized, 2, args, &ignored) != JS_OK)
            return JS_THROW;
    }
    *ret = result;
    return JS_OK;
}

static int promise_finally_pass(js_ctx *ctx, js_value t, int argc,
                                js_value *argv, js_value *ret)
{
    uint32_t rejected = 0;
    js_value original = argc ? argv[0] : js_undefined();
    (void)t;

    if (argc > 1 && js_to_uint32(ctx, argv[1], &rejected) != JS_OK)
        return JS_THROW;
    if (rejected)
        return js_throw(ctx, original);
    *ret = original;
    return JS_OK;
}

static int promise_finally_branch(js_ctx *ctx, js_value t, int argc,
                                  js_value *argv, js_value *ret)
{
    js_value cleanup, normalized, pass, ignored, bound[2], args[2];
    (void)t;

    if (argc < 3 || !js_is_function(argv[0]))
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "invalid Promise.finally callback");
    if (js_call(ctx, argv[0], js_undefined(), 0, 0, &cleanup) != JS_OK)
        return JS_THROW;
    if (promise_static_resolve(ctx, js_undefined(), 1,
                               &cleanup, &normalized) != JS_OK)
        return JS_THROW;
    bound[0] = argv[2];          /* original value or rejection reason */
    bound[1] = argv[1];          /* zero = return, one = rethrow */
    if (bound_native(ctx, js_undefined(), promise_finally_pass,
                     "finally pass", 2, bound, &pass) != JS_OK)
        return JS_THROW;
    args[0] = pass;
    args[1] = js_undefined();
    if (promise_then(ctx, normalized, 2, args, &ignored) != JS_OK)
        return JS_THROW;
    *ret = ignored;
    return JS_OK;
}

static int promise_finally(js_ctx *ctx, js_value t, int argc,
                           js_value *argv, js_value *ret)
{
    js_value on_finally, fulfilled, rejected, b0, b1, args[2];

    if (!js_is_promise(t))
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "Promise.finally called on a non-Promise");
    on_finally = argc ? argv[0] : js_undefined();
    if (!js_is_function(on_finally)) {
        args[0] = on_finally;
        args[1] = on_finally;
        return promise_then(ctx, t, 2, args, ret);
    }
    b0 = on_finally;
    b1 = js_number(0);
    {
        js_value bound[2] = { b0, b1 };
        if (bound_native(ctx, js_undefined(), promise_finally_branch,
                         "finally fulfilled", 2, bound, &fulfilled) != JS_OK)
            return JS_THROW;
    }
    b1 = js_number(1);
    {
        js_value bound[2] = { b0, b1 };
        if (bound_native(ctx, js_undefined(), promise_finally_branch,
                         "finally rejected", 2, bound, &rejected) != JS_OK)
            return JS_THROW;
    }
    args[0] = fulfilled;
    args[1] = rejected;
    return promise_then(ctx, t, 2, args, ret);
}

int js_run_jobs(js_ctx *ctx, unsigned long max_jobs)
{
    unsigned long ran = 0;

    if (!ctx || ctx->fatal)
        return -1;
    if (!max_jobs)
        max_jobs = 1024;
    while (ctx->jobs_head && ran < max_jobs) {
        js_promise_job *job = ctx->jobs_head;
        js_promise_reaction *reaction = job->reaction;
        js_object *source = job->source;
        js_value handler, result;
        int rc;

        ctx->jobs_head = job->next;
        if (!ctx->jobs_head)
            ctx->jobs_tail = 0;
        handler = source->promise_state == 1
            ? reaction->on_fulfilled : reaction->on_rejected;
        if (!js_is_function(handler)) {
            rc = promise_settle(ctx, reaction->child,
                                source->promise_state,
                                source->promise_result);
        } else {
            result = js_undefined();
            rc = js_call(ctx, handler, js_undefined(), 1,
                         &source->promise_result, &result);
            if (rc == JS_OK) {
                rc = promise_settle(ctx, reaction->child, 1, result);
            } else if (!ctx->fatal) {
                js_value reason = js_exception(ctx);
                js_clear_exception(ctx);
                rc = promise_settle(ctx, reaction->child, 2, reason);
            }
        }
        if (rc != JS_OK || ctx->fatal)
            return -1;
        ran++;
    }
    return (int)ran;
}

/* ================================================================== */
/* WebAssembly MVP execution core                                      */
/* ================================================================== */

#define WASM_MODULE_TAG   0x574D4F44u
#define WASM_INSTANCE_TAG 0x574D494Eu
#define WASM_CALL_TAG     0x574D4341u
#define WASM_BYTE_MAX     (1024UL * 1024UL)
#define WASM_FUNC_MAX     256
#define WASM_TYPE_MAX     256
#define WASM_EXPORT_MAX   256
#define WASM_LOCAL_MAX    256
#define WASM_STACK_MAX    256
#define WASM_CALL_MAX     32
#define WASM_STEP_MAX     100000

struct wasm_reader {
    const uint8_t *p, *end;
};

struct wasm_type {
    uint8_t nparam, nresult;
};

struct wasm_func {
    uint32_t type;
    const uint8_t *code;
    uint32_t code_len;
};

struct wasm_export {
    char *name;
    uint32_t func;
};

struct wasm_module {
    uint8_t *bytes;
    unsigned long len;
    struct wasm_type *types;
    struct wasm_func *funcs;
    struct wasm_export *exports;
    uint32_t ntypes, nfuncs, nexports;
};

struct wasm_callable {
    struct wasm_module *module;
    uint32_t func;
};

static int wasm_u32(struct wasm_reader *r, uint32_t *out)
{
    uint32_t v = 0;
    int shift = 0, i;

    for (i = 0; i < 5 && r->p < r->end; i++) {
        uint8_t b = *r->p++;

        if (i == 4 && (b & 0xF0))
            return 0;
        v |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return 1;
        }
        shift += 7;
    }
    return 0;
}

static int wasm_i32(struct wasm_reader *r, int32_t *out)
{
    uint32_t v = 0;
    uint8_t b = 0;
    int shift = 0, i;

    for (i = 0; i < 5 && r->p < r->end; i++) {
        b = *r->p++;
        v |= (uint32_t)(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80)) {
            if (shift < 32 && (b & 0x40))
                v |= ~0u << shift;
            *out = (int32_t)v;
            return 1;
        }
    }
    return 0;
}

static int wasm_error(js_ctx *ctx, const char *msg)
{
    return js_throw_error(ctx, JS_ERR_SYNTAX, "WebAssembly: %s", msg);
}

static int wasm_source(js_ctx *ctx, js_value value, uint8_t **out,
                       unsigned long *out_len)
{
    js_object *source = 0;
    unsigned long n, i;
    uint8_t *bytes;
    const void *buffer = js_arraybuffer_data(value, &n);

    if (!buffer) {
        if (this_object(ctx, value, &source) != JS_OK)
            return JS_THROW;
        if (alen(ctx, source, &n) != JS_OK)
            return JS_THROW;
    }
    if (n > WASM_BYTE_MAX)
        return js_throw_error(ctx, JS_ERR_RANGE,
                              "WebAssembly input is too large");
    bytes = (uint8_t *)js_alloc(ctx, n ? n : 1);
    if (!bytes)
        return JS_THROW;
    if (buffer && n) {
        memcpy(bytes, buffer, n);
        *out = bytes;
        *out_len = n;
        return JS_OK;
    }
    for (i = 0; i < n; i++) {
        js_value v;
        uint32_t byte;

        if (aget(ctx, source, i, &v) != JS_OK ||
            js_to_uint32(ctx, v, &byte) != JS_OK)
            return JS_THROW;
        if (byte > 255)
            return js_throw_error(ctx, JS_ERR_TYPE,
                                  "WebAssembly bytes must be 0..255");
        bytes[i] = (uint8_t)byte;
    }
    *out = bytes;
    *out_len = n;
    return JS_OK;
}

static int wasm_scan_code(js_ctx *ctx, struct wasm_module *m,
                          uint32_t fi)
{
    struct wasm_func *f = &m->funcs[fi];
    struct wasm_type *ft = &m->types[f->type];
    struct wasm_reader r = { f->code, f->code + f->code_len };
    uint32_t groups, locals = ft->nparam, i, stack = 0, steps = 0;
    int ended = 0;

    if (!wasm_u32(&r, &groups) || groups > WASM_LOCAL_MAX)
        return wasm_error(ctx, "invalid local declaration vector");
    for (i = 0; i < groups; i++) {
        uint32_t count;

        if (!wasm_u32(&r, &count) || r.p >= r.end || *r.p++ != 0x7F ||
            count > WASM_LOCAL_MAX - locals)
            return wasm_error(ctx, "only bounded i32 locals are supported");
        locals += count;
    }
    while (r.p < r.end && steps++ < WASM_STEP_MAX) {
        uint8_t op = *r.p++;
        uint32_t index;
        int32_t constant;

        switch (op) {
        case 0x0B: /* end */
            ended = r.p == r.end;
            if (!ended)
                return wasm_error(ctx, "structured control is not supported");
            break;
        case 0x1A: /* drop */
            if (!stack) return wasm_error(ctx, "operand stack underflow");
            stack--;
            break;
        case 0x20: /* local.get */
            if (!wasm_u32(&r, &index) || index >= locals)
                return wasm_error(ctx, "invalid local.get");
            if (stack >= WASM_STACK_MAX)
                return wasm_error(ctx, "operand stack is too deep");
            stack++;
            break;
        case 0x21: /* local.set */
            if (!wasm_u32(&r, &index) || index >= locals || !stack)
                return wasm_error(ctx, "invalid local.set");
            stack--;
            break;
        case 0x22: /* local.tee */
            if (!wasm_u32(&r, &index) || index >= locals || !stack)
                return wasm_error(ctx, "invalid local.tee");
            break;
        case 0x41: /* i32.const */
            if (!wasm_i32(&r, &constant))
                return wasm_error(ctx, "invalid i32 constant");
            if (stack >= WASM_STACK_MAX)
                return wasm_error(ctx, "operand stack is too deep");
            stack++;
            break;
        case 0x10: /* call */
            if (!wasm_u32(&r, &index) || index >= m->nfuncs)
                return wasm_error(ctx, "invalid function call");
            if (stack < m->types[m->funcs[index].type].nparam)
                return wasm_error(ctx, "call operand stack underflow");
            stack -= m->types[m->funcs[index].type].nparam;
            stack += m->types[m->funcs[index].type].nresult;
            if (stack > WASM_STACK_MAX)
                return wasm_error(ctx, "operand stack is too deep");
            break;
        case 0x45: /* i32.eqz */
            if (!stack) return wasm_error(ctx, "operand stack underflow");
            break;
        case 0x46: case 0x47: case 0x48: case 0x4A:
        case 0x4C: case 0x4E:
        case 0x6A: case 0x6B: case 0x6C:
        case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76:
            if (stack < 2)
                return wasm_error(ctx, "operand stack underflow");
            stack--;
            break;
        default:
            return wasm_error(ctx,
                              "opcode is outside the bounded i32 MVP core");
        }
        if (ended)
            break;
    }
    if (!ended || stack != ft->nresult)
        return wasm_error(ctx, "function result does not match its type");
    return JS_OK;
}

static struct wasm_module *wasm_compile_value(js_ctx *ctx, js_value input)
{
    struct wasm_module *m;
    struct wasm_reader r;
    uint8_t *bytes = 0;
    unsigned long len = 0;
    uint32_t seen = 0, last = 0, code_count = 0;

    if (wasm_source(ctx, input, &bytes, &len) != JS_OK)
        return 0;
    if (len < 8 || memcmp(bytes, "\0asm\1\0\0\0", 8))
        return wasm_error(ctx, "bad magic or version"), (struct wasm_module *)0;
    m = (struct wasm_module *)js_alloc(ctx, sizeof(*m));
    if (!m)
        return 0;
    memset(m, 0, sizeof(*m));
    m->bytes = bytes;
    m->len = len;
    r.p = bytes + 8;
    r.end = bytes + len;
    while (r.p < r.end) {
        uint8_t id = *r.p++;
        uint32_t size;
        struct wasm_reader s;

        if (!wasm_u32(&r, &size) ||
            (unsigned long)(r.end - r.p) < size)
            return wasm_error(ctx, "truncated section"), (struct wasm_module *)0;
        s.p = r.p;
        s.end = r.p + size;
        r.p = s.end;
        if (id > 12)
            return wasm_error(ctx, "unknown section id"), (struct wasm_module *)0;
        if (id && (id < last || (seen & (1u << id))))
            return wasm_error(ctx, "duplicate or out-of-order section"),
                   (struct wasm_module *)0;
        if (id) {
            last = id;
            seen |= 1u << id;
        }
        if (id == 0)
            continue;
        if (id == 1) {
            uint32_t count, i;

            if (!wasm_u32(&s, &count) || count > WASM_TYPE_MAX)
                return wasm_error(ctx, "too many function types"),
                       (struct wasm_module *)0;
            m->types = (struct wasm_type *)js_alloc(
                ctx, (count ? count : 1) * sizeof(*m->types));
            if (!m->types) return 0;
            memset(m->types, 0, (count ? count : 1) * sizeof(*m->types));
            m->ntypes = count;
            for (i = 0; i < count; i++) {
                uint32_t n, j;

                if (s.p >= s.end || *s.p++ != 0x60 ||
                    !wasm_u32(&s, &n) || n > 16)
                    return wasm_error(ctx, "invalid function type"),
                           (struct wasm_module *)0;
                m->types[i].nparam = (uint8_t)n;
                for (j = 0; j < n; j++)
                    if (s.p >= s.end || *s.p++ != 0x7F)
                        return wasm_error(ctx, "only i32 parameters are supported"),
                               (struct wasm_module *)0;
                if (!wasm_u32(&s, &n) || n > 1)
                    return wasm_error(ctx, "invalid result vector"),
                           (struct wasm_module *)0;
                m->types[i].nresult = (uint8_t)n;
                if (n && (s.p >= s.end || *s.p++ != 0x7F))
                    return wasm_error(ctx, "only i32 results are supported"),
                           (struct wasm_module *)0;
            }
        } else if (id == 2) {
            uint32_t count;

            if (!wasm_u32(&s, &count) || count)
                return wasm_error(ctx, "imports are not supported yet"),
                       (struct wasm_module *)0;
        } else if (id == 3) {
            uint32_t count, i;

            if (!wasm_u32(&s, &count) || count > WASM_FUNC_MAX)
                return wasm_error(ctx, "too many functions"),
                       (struct wasm_module *)0;
            m->funcs = (struct wasm_func *)js_alloc(
                ctx, (count ? count : 1) * sizeof(*m->funcs));
            if (!m->funcs) return 0;
            memset(m->funcs, 0, (count ? count : 1) * sizeof(*m->funcs));
            m->nfuncs = count;
            for (i = 0; i < count; i++)
                if (!wasm_u32(&s, &m->funcs[i].type) ||
                    m->funcs[i].type >= m->ntypes)
                    return wasm_error(ctx, "invalid function type index"),
                           (struct wasm_module *)0;
        } else if (id == 7) {
            uint32_t count, i;

            if (!wasm_u32(&s, &count) || count > WASM_EXPORT_MAX)
                return wasm_error(ctx, "too many exports"),
                       (struct wasm_module *)0;
            m->exports = (struct wasm_export *)js_alloc(
                ctx, (count ? count : 1) * sizeof(*m->exports));
            if (!m->exports) return 0;
            memset(m->exports, 0, (count ? count : 1) * sizeof(*m->exports));
            for (i = 0; i < count; i++) {
                uint32_t name_len, index;
                uint8_t kind;
                char *name;

                if (!wasm_u32(&s, &name_len) || name_len > 128 ||
                    (unsigned long)(s.end - s.p) < name_len + 1)
                    return wasm_error(ctx, "invalid export"),
                           (struct wasm_module *)0;
                name = (char *)js_alloc(ctx, name_len + 1);
                if (!name) return 0;
                memcpy(name, s.p, name_len);
                name[name_len] = 0;
                s.p += name_len;
                kind = *s.p++;
                if (!wasm_u32(&s, &index))
                    return wasm_error(ctx, "invalid export index"),
                           (struct wasm_module *)0;
                if (kind == 0) {
                    if (index >= m->nfuncs)
                        return wasm_error(ctx, "function export is out of range"),
                               (struct wasm_module *)0;
                    m->exports[m->nexports].name = name;
                    m->exports[m->nexports].func = index;
                    m->nexports++;
                }
            }
        } else if (id == 8) {
            return wasm_error(ctx, "start functions are not supported yet"),
                   (struct wasm_module *)0;
        } else if (id == 10) {
            uint32_t count, i;

            if (!wasm_u32(&s, &count) || count != m->nfuncs)
                return wasm_error(ctx, "code/function count mismatch"),
                       (struct wasm_module *)0;
            code_count = count;
            for (i = 0; i < count; i++) {
                uint32_t body_len;

                if (!wasm_u32(&s, &body_len) ||
                    (unsigned long)(s.end - s.p) < body_len)
                    return wasm_error(ctx, "truncated function body"),
                           (struct wasm_module *)0;
                m->funcs[i].code = s.p;
                m->funcs[i].code_len = body_len;
                s.p += body_len;
            }
        }
        if (s.p != s.end)
            return wasm_error(ctx, "section has trailing bytes"),
                   (struct wasm_module *)0;
    }
    if (m->nfuncs != code_count)
        return wasm_error(ctx, "functions require a code section"),
               (struct wasm_module *)0;
    {
        uint32_t i;
        for (i = 0; i < m->nfuncs; i++)
            if (wasm_scan_code(ctx, m, i) != JS_OK)
                return 0;
    }
    return m;
}

static int wasm_exec(struct wasm_module *m, uint32_t fi,
                     const int32_t *args, int32_t *result,
                     int depth, uint32_t *steps)
{
    struct wasm_func *f;
    struct wasm_type *ft;
    struct wasm_reader r;
    int32_t locals[WASM_LOCAL_MAX], stack[WASM_STACK_MAX];
    uint32_t groups, nlocals, sp = 0, i;

    if (depth >= WASM_CALL_MAX || fi >= m->nfuncs)
        return 0;
    f = &m->funcs[fi];
    ft = &m->types[f->type];
    r.p = f->code;
    r.end = f->code + f->code_len;
    memset(locals, 0, sizeof(locals));
    for (i = 0; i < ft->nparam; i++)
        locals[i] = args[i];
    nlocals = ft->nparam;
    if (!wasm_u32(&r, &groups))
        return 0;
    for (i = 0; i < groups; i++) {
        uint32_t count;
        if (!wasm_u32(&r, &count) || r.p >= r.end || *r.p++ != 0x7F ||
            count > WASM_LOCAL_MAX - nlocals)
            return 0;
        nlocals += count;
    }
    while (r.p < r.end && (*steps)++ < WASM_STEP_MAX) {
        uint8_t op = *r.p++;
        uint32_t index;
        int32_t a, b;

        if (op == 0x0B) {
            *result = ft->nresult ? stack[sp - 1] : 0;
            return 1;
        }
        switch (op) {
        case 0x1A: sp--; break;
        case 0x20:
            if (!wasm_u32(&r, &index)) return 0;
            stack[sp++] = locals[index];
            break;
        case 0x21:
            if (!wasm_u32(&r, &index)) return 0;
            locals[index] = stack[--sp];
            break;
        case 0x22:
            if (!wasm_u32(&r, &index)) return 0;
            locals[index] = stack[sp - 1];
            break;
        case 0x41:
            if (!wasm_i32(&r, &stack[sp++])) return 0;
            break;
        case 0x10: {
            int32_t call_args[16], call_result = 0;
            struct wasm_type *ct;
            uint32_t j;

            if (!wasm_u32(&r, &index) || index >= m->nfuncs) return 0;
            ct = &m->types[m->funcs[index].type];
            for (j = ct->nparam; j > 0; j--)
                call_args[j - 1] = stack[--sp];
            if (!wasm_exec(m, index, call_args, &call_result,
                           depth + 1, steps))
                return 0;
            if (ct->nresult) stack[sp++] = call_result;
            break;
        }
        case 0x45: stack[sp - 1] = stack[sp - 1] == 0; break;
        default:
            b = stack[--sp];
            a = stack[--sp];
            switch (op) {
            case 0x46: a = a == b; break;
            case 0x47: a = a != b; break;
            case 0x48: a = a < b; break;
            case 0x4A: a = a > b; break;
            case 0x4C: a = a <= b; break;
            case 0x4E: a = a >= b; break;
            case 0x6A: a = (int32_t)((uint32_t)a + (uint32_t)b); break;
            case 0x6B: a = (int32_t)((uint32_t)a - (uint32_t)b); break;
            case 0x6C: a = (int32_t)((uint32_t)a * (uint32_t)b); break;
            case 0x71: a &= b; break;
            case 0x72: a |= b; break;
            case 0x73: a ^= b; break;
            case 0x74: a = (int32_t)((uint32_t)a << ((uint32_t)b & 31)); break;
            case 0x75: a >>= (uint32_t)b & 31; break;
            case 0x76: a = (int32_t)((uint32_t)a >>
                                     ((uint32_t)b & 31)); break;
            default: return 0;
            }
            stack[sp++] = a;
            break;
        }
    }
    return 0;
}

static int wasm_invoke(js_ctx *ctx, js_value t, int argc,
                       js_value *argv, js_value *ret)
{
    struct wasm_callable *call =
        (struct wasm_callable *)js_host_ptr(t, WASM_CALL_TAG);
    struct wasm_type *type;
    int32_t args[16], result = 0;
    uint32_t steps = 0, i;

    if (!call)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "invalid WebAssembly function receiver");
    type = &call->module->types[call->module->funcs[call->func].type];
    for (i = 0; i < type->nparam; i++) {
        js_value v = (int)i < argc ? argv[i] : js_undefined();
        if (js_to_int32(ctx, v, &args[i]) != JS_OK)
            return JS_THROW;
    }
    if (!wasm_exec(call->module, call->func, args, &result, 0, &steps))
        return js_throw_error(ctx, JS_ERR_ERROR,
                              "WebAssembly execution limit exceeded");
    *ret = type->nresult ? js_number((double)result) : js_undefined();
    return JS_OK;
}

static int wasm_wrap_module(js_ctx *ctx, struct wasm_module *m, js_value *ret)
{
    js_object *o = js_new_host(ctx, m, WASM_MODULE_TAG,
                               ctx->proto[P_WASM_MODULE]);
    if (!o) return JS_THROW;
    *ret = js_object_value(o);
    return JS_OK;
}

static int wasm_module_ctor(js_ctx *ctx, js_value t, int argc,
                            js_value *argv, js_value *ret)
{
    struct wasm_module *m;
    (void)t;

    if (argc < 1)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "WebAssembly.Module requires bytes");
    m = wasm_compile_value(ctx, argv[0]);
    return m ? wasm_wrap_module(ctx, m, ret) : JS_THROW;
}

static int wasm_make_instance(js_ctx *ctx, struct wasm_module *m,
                              js_value *ret)
{
    js_object *instance, *exports;
    js_value iv, ev;
    uint32_t i;

    instance = js_new_host(ctx, m, WASM_INSTANCE_TAG,
                           ctx->proto[P_WASM_INSTANCE]);
    exports = js_new_object(ctx);
    if (!instance || !exports)
        return JS_THROW;
    iv = js_object_value(instance);
    ev = js_object_value(exports);
    for (i = 0; i < m->nexports; i++) {
        struct wasm_callable *call = (struct wasm_callable *)
            js_alloc(ctx, sizeof(*call));
        js_object *host;
        js_value fn, hostv;

        if (!call)
            return JS_THROW;
        call->module = m;
        call->func = m->exports[i].func;
        host = js_new_host(ctx, call, WASM_CALL_TAG, ctx->proto[P_OBJECT]);
        if (!host)
            return JS_THROW;
        hostv = js_object_value(host);
        if (bound_native(ctx, hostv, wasm_invoke, m->exports[i].name,
                         0, 0, &fn) != JS_OK ||
            js_set(ctx, ev, m->exports[i].name, fn) != JS_OK)
            return JS_THROW;
    }
    if (js_set(ctx, iv, "exports", ev) != JS_OK)
        return JS_THROW;
    *ret = iv;
    return JS_OK;
}

static int wasm_instance_ctor(js_ctx *ctx, js_value t, int argc,
                              js_value *argv, js_value *ret)
{
    struct wasm_module *m;
    (void)t;

    if (argc < 1 ||
        !(m = (struct wasm_module *)js_host_ptr(argv[0], WASM_MODULE_TAG)))
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "WebAssembly.Instance requires a Module");
    return wasm_make_instance(ctx, m, ret);
}

static int wasm_validate(js_ctx *ctx, js_value t, int argc,
                         js_value *argv, js_value *ret)
{
    struct wasm_module *m;
    (void)t;

    if (argc < 1) {
        *ret = js_bool(0);
        return JS_OK;
    }
    m = wasm_compile_value(ctx, argv[0]);
    if (!m) {
        if (ctx->fatal)
            return JS_THROW;
        js_clear_exception(ctx);
    }
    *ret = js_bool(m != 0);
    return JS_OK;
}

static int wasm_compile_async(js_ctx *ctx, js_value t, int argc,
                              js_value *argv, js_value *ret)
{
    js_value module, reason;
    int rc = wasm_module_ctor(ctx, t, argc, argv, &module);

    if (rc == JS_OK)
        return promise_static_resolve(ctx, js_undefined(), 1, &module, ret);
    if (ctx->fatal)
        return JS_THROW;
    reason = js_exception(ctx);
    js_clear_exception(ctx);
    return promise_static_reject(ctx, js_undefined(), 1, &reason, ret);
}

static int wasm_instantiate_async(js_ctx *ctx, js_value t, int argc,
                                  js_value *argv, js_value *ret)
{
    struct wasm_module *m;
    js_value module, instance, result, reason;
    int supplied_module;
    (void)t;

    if (argc < 1)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "WebAssembly.instantiate requires bytes");
    m = (struct wasm_module *)js_host_ptr(argv[0], WASM_MODULE_TAG);
    supplied_module = m != 0;
    if (!m) {
        m = wasm_compile_value(ctx, argv[0]);
        if (!m)
            goto reject;
        if (wasm_wrap_module(ctx, m, &module) != JS_OK)
            goto reject;
    } else {
        module = argv[0];
    }
    if (wasm_make_instance(ctx, m, &instance) != JS_OK)
        goto reject;
    if (supplied_module) {
        result = instance;
    } else {
        js_object *pair = js_new_object(ctx);
        if (!pair)
            goto reject;
        result = js_object_value(pair);
        if (js_set(ctx, result, "module", module) != JS_OK ||
            js_set(ctx, result, "instance", instance) != JS_OK)
            goto reject;
    }
    return promise_static_resolve(ctx, js_undefined(), 1, &result, ret);

reject:
    if (ctx->fatal)
        return JS_THROW;
    reason = js_exception(ctx);
    js_clear_exception(ctx);
    return promise_static_reject(ctx, js_undefined(), 1, &reason, ret);
}

/* ================================================================== */
/* Bootstrap                                                           */
/* ================================================================== */

#define DEFN(o, n, f, a) \
    do { if (js_define_native(ctx, (o), (n), (f), (a)) != JS_OK) return 0; } while (0)
#define DEFV(o, n, v) \
    do { if (js_define(ctx, (o), (n), (v)) != JS_OK) return 0; } while (0)

static js_object *mkctor(js_ctx *ctx, js_native fn, const char *name, int nargs,
                         js_object *proto)
{
    js_object *c = js_new_native(ctx, fn, name, nargs);

    if (!c) return 0;
    c->fn->ctor_kind = JS_CTOR_NATIVE;
    if (proto) {
        js_prop *p = js_add_prop(ctx, c, ctx->s_prototype, 0);
        if (!p) return 0;
        p->value = js_object_value(proto);
        p = js_add_prop(ctx, proto, ctx->s_constructor, JS_P_HIDDEN);
        if (!p) return 0;
        p->value = js_object_value(c);
    }
    if (js_define(ctx, ctx->global, name, js_object_value(c)) != JS_OK)
        return 0;
    return c;
}

int js_init_builtins(js_ctx *ctx)
{
    js_object *g, *p;
    int i;

    ctx->argstack = (js_value *)js_alloc(ctx, JS_ARGSTACK * sizeof(js_value));
    if (!ctx->argstack)
        return 0;

#define INTERN(field, text) \
    do { ctx->field = js_str_intern(ctx, text, sizeof(text) - 1); \
         if (!ctx->field) return 0; } while (0)
    INTERN(s_empty, "");
    INTERN(s_length, "length");
    INTERN(s_prototype, "prototype");
    INTERN(s_constructor, "constructor");
    INTERN(s_proto__, "__proto__");
    INTERN(s_name, "name");
    INTERN(s_message, "message");
    INTERN(s_arguments, "arguments");
    INTERN(s_callee, "callee");
    INTERN(s_value, "value");
    INTERN(s_writable, "writable");
    INTERN(s_enumerable, "enumerable");
    INTERN(s_configurable, "configurable");
    INTERN(s_get, "get");
    INTERN(s_set, "set");
    INTERN(s_index, "index");
    INTERN(s_input, "input");
    INTERN(s_lastIndex, "lastIndex");
    INTERN(s_source, "source");
    INTERN(s_toString, "toString");
    INTERN(s_valueOf, "valueOf");
    INTERN(s_undefined, "undefined");
    INTERN(s_null, "null");
    INTERN(s_true, "true");
    INTERN(s_false, "false");
    INTERN(s_number, "number");
    INTERN(s_string, "string");
    INTERN(s_boolean, "boolean");
    INTERN(s_object, "object");
    INTERN(s_function, "function");
#undef INTERN

    ctx->proto[P_OBJECT] = js_obj_alloc(ctx, JS_CLASS_OBJECT, 0);
    if (!ctx->proto[P_OBJECT]) return 0;
    ctx->proto[P_FUNCTION] = js_obj_alloc(ctx, JS_CLASS_OBJECT, ctx->proto[P_OBJECT]);
    if (!ctx->proto[P_FUNCTION]) return 0;

    g = js_obj_alloc(ctx, JS_CLASS_OBJECT, ctx->proto[P_OBJECT]);
    if (!g) return 0;
    ctx->global = g;
    ctx->genv = js_env_new(ctx, 0, g);
    if (!ctx->genv) return 0;
    ctx->genv->this_val = js_object_value(g);

    for (i = P_ARRAY; i <= P_WASM_INSTANCE; i++) {
        ctx->proto[i] = js_obj_alloc(ctx,
            i == P_ARRAY ? JS_CLASS_ARRAY : JS_CLASS_OBJECT, ctx->proto[P_OBJECT]);
        if (!ctx->proto[i]) return 0;
    }
    for (i = P_TYPEERROR; i <= P_URIERROR; i++)
        ctx->proto[i]->proto = ctx->proto[P_ERROR];

    /* ---- Object ---- */
    p = ctx->proto[P_OBJECT];
    DEFN(p, "toString", bi_obj_tostring, 0);
    DEFN(p, "toLocaleString", bi_obj_tostring, 0);
    DEFN(p, "valueOf", bi_obj_valueof, 0);
    DEFN(p, "hasOwnProperty", bi_obj_hasown, 1);
    DEFN(p, "isPrototypeOf", bi_obj_isproto, 1);
    DEFN(p, "propertyIsEnumerable", bi_obj_propenum, 1);
    ctx->ctor_object = mkctor(ctx, bi_object_ctor, "Object", 1, p);
    if (!ctx->ctor_object) return 0;
    DEFN(ctx->ctor_object, "keys", bi_object_keys, 1);
    DEFN(ctx->ctor_object, "getOwnPropertyNames", bi_object_names, 1);
    DEFN(ctx->ctor_object, "getPrototypeOf", bi_object_getproto, 1);
    DEFN(ctx->ctor_object, "defineProperty", bi_object_defprop, 3);
    DEFN(ctx->ctor_object, "defineProperties", bi_object_defprops, 2);
    DEFN(ctx->ctor_object, "create", bi_object_create, 2);
    DEFN(ctx->ctor_object, "getOwnPropertyDescriptor", bi_object_getdesc, 2);
    DEFN(ctx->ctor_object, "freeze", bi_object_freeze, 1);
    DEFN(ctx->ctor_object, "seal", bi_object_seal, 1);
    DEFN(ctx->ctor_object, "isFrozen", bi_object_isfrozen, 1);
    DEFN(ctx->ctor_object, "isSealed", bi_object_issealed, 1);
    DEFN(ctx->ctor_object, "preventExtensions", bi_object_prevext, 1);
    DEFN(ctx->ctor_object, "isExtensible", bi_object_isext, 1);

    /* ---- Function ---- */
    p = ctx->proto[P_FUNCTION];
    DEFN(p, "call", bi_fn_call, 1);
    DEFN(p, "apply", bi_fn_apply, 2);
    DEFN(p, "bind", bi_fn_bind, 1);
    DEFN(p, "toString", bi_fn_tostring, 0);
    if (!mkctor(ctx, bi_function_ctor, "Function", 1, p)) return 0;

    /* ---- Array ---- */
    p = ctx->proto[P_ARRAY];
    DEFN(p, "toString", bi_array_tostring, 0);
    DEFN(p, "toLocaleString", bi_array_tostring, 0);
    DEFN(p, "join", bi_array_join, 1);
    DEFN(p, "push", bi_array_push, 1);
    DEFN(p, "pop", bi_array_pop, 0);
    DEFN(p, "shift", bi_array_shift, 0);
    DEFN(p, "unshift", bi_array_unshift, 1);
    DEFN(p, "slice", bi_array_slice, 2);
    DEFN(p, "splice", bi_array_splice, 2);
    DEFN(p, "concat", bi_array_concat, 1);
    DEFN(p, "indexOf", bi_array_indexof, 1);
    DEFN(p, "lastIndexOf", bi_array_lastindexof, 1);
    DEFN(p, "reverse", bi_array_reverse, 0);
    DEFN(p, "sort", bi_array_sort, 1);
    DEFN(p, "forEach", bi_array_foreach, 1);
    DEFN(p, "map", bi_array_map, 1);
    DEFN(p, "filter", bi_array_filter, 1);
    DEFN(p, "every", bi_array_every, 1);
    DEFN(p, "some", bi_array_some, 1);
    DEFN(p, "reduce", bi_array_reduce, 1);
    DEFN(p, "reduceRight", bi_array_reduceright, 1);
    ctx->ctor_array = mkctor(ctx, bi_array_ctor, "Array", 1, p);
    if (!ctx->ctor_array) return 0;
    DEFN(ctx->ctor_array, "isArray", bi_array_isarray, 1);

    /* ---- String ---- */
    p = ctx->proto[P_STRING];
    DEFN(p, "toString", bi_string_tostring, 0);
    DEFN(p, "valueOf", bi_string_tostring, 0);
    DEFN(p, "charAt", bi_string_charat, 1);
    DEFN(p, "charCodeAt", bi_string_charcodeat, 1);
    DEFN(p, "indexOf", bi_string_indexof, 1);
    DEFN(p, "lastIndexOf", bi_string_lastindexof, 1);
    DEFN(p, "slice", bi_string_slice, 2);
    DEFN(p, "substring", bi_string_substring, 2);
    DEFN(p, "substr", bi_string_substr, 2);
    DEFN(p, "toUpperCase", bi_string_upper, 0);
    DEFN(p, "toLowerCase", bi_string_lower, 0);
    DEFN(p, "toLocaleUpperCase", bi_string_upper, 0);
    DEFN(p, "toLocaleLowerCase", bi_string_lower, 0);
    DEFN(p, "trim", bi_string_trim, 0);
    DEFN(p, "concat", bi_string_concat, 1);
    DEFN(p, "localeCompare", bi_string_localecompare, 1);
    DEFN(p, "split", bi_string_split, 2);
    DEFN(p, "replace", bi_string_replace, 2);
    DEFN(p, "match", bi_string_match, 1);
    DEFN(p, "search", bi_string_search, 1);
    {
        js_object *c = mkctor(ctx, bi_string_ctor, "String", 1, p);
        if (!c) return 0;
        DEFN(c, "fromCharCode", bi_string_fromcharcode, 1);
    }

    /* ---- Number ---- */
    p = ctx->proto[P_NUMBER];
    DEFN(p, "toString", bi_number_tostring, 1);
    DEFN(p, "toLocaleString", bi_number_tostring, 0);
    DEFN(p, "valueOf", bi_number_valueof, 0);
    DEFN(p, "toFixed", bi_number_tofixed, 1);
    DEFN(p, "toPrecision", bi_number_toprecision, 1);
    DEFN(p, "toExponential", bi_number_toexponential, 1);
    {
        js_object *c = mkctor(ctx, bi_number_ctor, "Number", 1, p);
        if (!c) return 0;
        DEFV(c, "MAX_VALUE", js_number(1.7976931348623157e308));
        DEFV(c, "MIN_VALUE", js_number(5e-324));
        DEFV(c, "NaN", js_number(js_nan()));
        DEFV(c, "POSITIVE_INFINITY", js_number(js_inf(0)));
        DEFV(c, "NEGATIVE_INFINITY", js_number(js_inf(1)));
    }

    /* ---- Boolean ---- */
    p = ctx->proto[P_BOOLEAN];
    DEFN(p, "toString", bi_bool_tostring, 0);
    DEFN(p, "valueOf", bi_bool_valueof, 0);
    if (!mkctor(ctx, bi_bool_ctor, "Boolean", 1, p)) return 0;

    /* ---- Error hierarchy ---- */
    for (i = 0; i < JS_ERR__COUNT; i++) {
        js_object *ep = ctx->proto[P_ERROR + i];
        js_object *c;
        ep->cls = JS_CLASS_ERROR;
        DEFV(ep, "name", js_mkcstring(ctx, err_names[i]));
        DEFV(ep, "message", js_string_value(ctx->s_empty));
        if (i == 0)
            DEFN(ep, "toString", bi_error_tostring, 0);
        c = mkctor(ctx, bi_error_ctor, err_names[i], 1, ep);
        if (!c) return 0;
    }

    /* ---- Date ---- */
    p = ctx->proto[P_DATE];
    DEFN(p, "getTime", bi_date_gettime, 0);
    DEFN(p, "valueOf", bi_date_valueof, 0);
    DEFN(p, "getFullYear", bi_date_getyear, 0);
    DEFN(p, "getUTCFullYear", bi_date_getyear, 0);
    DEFN(p, "getMonth", bi_date_getmonth, 0);
    DEFN(p, "getUTCMonth", bi_date_getmonth, 0);
    DEFN(p, "getDate", bi_date_getdate, 0);
    DEFN(p, "getUTCDate", bi_date_getdate, 0);
    DEFN(p, "getDay", bi_date_getday, 0);
    DEFN(p, "getUTCDay", bi_date_getday, 0);
    DEFN(p, "getHours", bi_date_gethours, 0);
    DEFN(p, "getUTCHours", bi_date_gethours, 0);
    DEFN(p, "getMinutes", bi_date_getmin, 0);
    DEFN(p, "getUTCMinutes", bi_date_getmin, 0);
    DEFN(p, "getSeconds", bi_date_getsec, 0);
    DEFN(p, "getUTCSeconds", bi_date_getsec, 0);
    DEFN(p, "getMilliseconds", bi_date_getms, 0);
    DEFN(p, "getUTCMilliseconds", bi_date_getms, 0);
    DEFN(p, "getTimezoneOffset", bi_date_gettz, 0);
    DEFN(p, "setTime", bi_date_settime, 1);
    DEFN(p, "toISOString", bi_date_toiso, 0);
    DEFN(p, "toJSON", bi_date_toiso, 0);
    DEFN(p, "toString", bi_date_tostring, 0);
    DEFN(p, "toUTCString", bi_date_tostring, 0);
    DEFN(p, "toLocaleString", bi_date_tostring, 0);
    {
        js_object *c = mkctor(ctx, bi_date_ctor, "Date", 7, p);
        if (!c) return 0;
        DEFN(c, "now", bi_date_now, 0);
        DEFN(c, "parse", bi_date_parse, 1);
        DEFN(c, "UTC", bi_date_utc, 7);
    }

    /* ---- RegExp ---- */
    p = ctx->proto[P_REGEXP];
    DEFN(p, "exec", bi_regexp_exec, 1);
    DEFN(p, "test", bi_regexp_test, 1);
    DEFN(p, "toString", bi_regexp_tostring, 0);
    if (js_define_accessor(ctx, p, "source", bi_re_source, 0, 0) != JS_OK ||
        js_define_accessor(ctx, p, "global", bi_re_global, 0, 0) != JS_OK ||
        js_define_accessor(ctx, p, "ignoreCase", bi_re_icase, 0, 0) != JS_OK ||
        js_define_accessor(ctx, p, "multiline", bi_re_multi, 0, 0) != JS_OK)
        return 0;
    ctx->ctor_regexp = mkctor(ctx, bi_regexp_ctor, "RegExp", 2, p);
    if (!ctx->ctor_regexp) return 0;

    /* ---- Promise ---- */
    p = ctx->proto[P_PROMISE];
    DEFN(p, "then", promise_then, 2);
    DEFN(p, "catch", promise_catch, 1);
    DEFN(p, "finally", promise_finally, 1);
    {
        js_object *promise_ctor_obj =
            mkctor(ctx, promise_ctor, "Promise", 1, p);
        if (!promise_ctor_obj) return 0;
        DEFN(promise_ctor_obj, "resolve", promise_static_resolve, 1);
        DEFN(promise_ctor_obj, "reject", promise_static_reject, 1);
        DEFN(promise_ctor_obj, "all", promise_static_all, 1);
        DEFN(promise_ctor_obj, "race", promise_static_race, 1);
    }

    /* ---- ArrayBuffer ---- */
    p = ctx->proto[P_ARRAYBUFFER];
    DEFN(p, "slice", arraybuffer_slice, 2);
    if (!mkctor(ctx, arraybuffer_ctor, "ArrayBuffer", 1, p))
        return 0;

    /* ---- WebAssembly ---- */
    {
        js_object *w = js_new_object(ctx);
        js_object *module_ctor_obj =
            js_new_native(ctx, wasm_module_ctor, "Module", 1);
        js_object *instance_ctor_obj =
            js_new_native(ctx, wasm_instance_ctor, "Instance", 2);
        js_prop *constructor;

        if (!w || !module_ctor_obj || !instance_ctor_obj)
            return 0;
        module_ctor_obj->fn->ctor_kind = JS_CTOR_NATIVE;
        instance_ctor_obj->fn->ctor_kind = JS_CTOR_NATIVE;
        DEFV(module_ctor_obj, "prototype",
             js_object_value(ctx->proto[P_WASM_MODULE]));
        DEFV(instance_ctor_obj, "prototype",
             js_object_value(ctx->proto[P_WASM_INSTANCE]));
        constructor = js_add_prop(ctx, ctx->proto[P_WASM_MODULE],
                                  ctx->s_constructor, JS_P_HIDDEN);
        if (!constructor) return 0;
        constructor->value = js_object_value(module_ctor_obj);
        constructor = js_add_prop(ctx, ctx->proto[P_WASM_INSTANCE],
                                  ctx->s_constructor, JS_P_HIDDEN);
        if (!constructor) return 0;
        constructor->value = js_object_value(instance_ctor_obj);
        DEFV(w, "Module", js_object_value(module_ctor_obj));
        DEFV(w, "Instance", js_object_value(instance_ctor_obj));
        DEFN(w, "validate", wasm_validate, 1);
        DEFN(w, "compile", wasm_compile_async, 1);
        DEFN(w, "instantiate", wasm_instantiate_async, 2);
        DEFV(g, "WebAssembly", js_object_value(w));
    }

    /* ---- Math ---- */
    {
        js_object *m = js_obj_alloc(ctx, JS_CLASS_MATH, ctx->proto[P_OBJECT]);
        if (!m) return 0;
        DEFV(m, "E", js_number(2.7182818284590452354));
        DEFV(m, "LN10", js_number(2.302585092994046));
        DEFV(m, "LN2", js_number(0.6931471805599453));
        DEFV(m, "LOG2E", js_number(1.4426950408889634));
        DEFV(m, "LOG10E", js_number(0.4342944819032518));
        DEFV(m, "PI", js_number(3.1415926535897932));
        DEFV(m, "SQRT1_2", js_number(0.7071067811865476));
        DEFV(m, "SQRT2", js_number(1.4142135623730951));
        DEFN(m, "abs", bi_math_abs, 1);
        DEFN(m, "acos", bi_math_acos, 1);
        DEFN(m, "asin", bi_math_asin, 1);
        DEFN(m, "atan", bi_math_atan, 1);
        DEFN(m, "atan2", bi_math_atan2, 2);
        DEFN(m, "ceil", bi_math_ceil, 1);
        DEFN(m, "cos", bi_math_cos, 1);
        DEFN(m, "exp", bi_math_exp, 1);
        DEFN(m, "floor", bi_math_floor, 1);
        DEFN(m, "log", bi_math_log, 1);
        DEFN(m, "max", bi_math_max, 2);
        DEFN(m, "min", bi_math_min, 2);
        DEFN(m, "pow", bi_math_pow, 2);
        DEFN(m, "random", bi_math_random, 0);
        DEFN(m, "round", bi_math_round, 1);
        DEFN(m, "sin", bi_math_sin, 1);
        DEFN(m, "sqrt", bi_math_sqrt, 1);
        DEFN(m, "tan", bi_math_tan, 1);
        DEFV(g, "Math", js_object_value(m));
    }

    /* ---- JSON ---- */
    {
        js_object *j = js_obj_alloc(ctx, JS_CLASS_JSON, ctx->proto[P_OBJECT]);
        if (!j) return 0;
        DEFN(j, "parse", bi_json_parse, 2);
        DEFN(j, "stringify", bi_json_stringify, 3);
        DEFV(g, "JSON", js_object_value(j));
    }

    /* ---- console ---- */
    {
        js_object *c = js_obj_alloc(ctx, JS_CLASS_OBJECT, ctx->proto[P_OBJECT]);
        if (!c) return 0;
        DEFN(c, "log", bi_console_log, 1);
        DEFN(c, "info", bi_console_log, 1);
        DEFN(c, "warn", bi_console_log, 1);
        DEFN(c, "error", bi_console_log, 1);
        DEFN(c, "debug", bi_console_log, 1);
        DEFV(g, "console", js_object_value(c));
    }

    /* ---- global values and functions ---- */
    {
        js_prop *pr = js_add_prop(ctx, g, ctx->s_undefined, 0);
        if (!pr) return 0;
        pr->value = js_undefined();
    }
    DEFV(g, "NaN", js_number(js_nan()));
    DEFV(g, "Infinity", js_number(js_inf(0)));
    DEFN(g, "parseInt", bi_parseint, 2);
    DEFN(g, "parseFloat", bi_parsefloat, 1);
    DEFN(g, "isNaN", bi_isnan, 1);
    DEFN(g, "isFinite", bi_isfinite, 1);
    DEFN(g, "encodeURIComponent", bi_encodeuricomponent, 1);
    DEFN(g, "encodeURI", bi_encodeuri, 1);
    DEFN(g, "decodeURIComponent", bi_decodeuri, 1);
    DEFN(g, "decodeURI", bi_decodeuri, 1);
    DEFN(g, "eval", bi_eval, 1);
    DEFV(g, "globalThis", js_object_value(g));
    return 1;
}
