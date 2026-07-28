/* libjs: recursive-descent parser producing an AST.
 *
 * Covers the ES5 expression grammar with correct precedence and
 * associativity, the full ES5 statement set, object/array literals,
 * function declarations and expressions, and automatic semicolon
 * insertion. `var` and function declarations are hoisted here rather than
 * at run time: every function node carries the list of names to create in
 * its activation record and the list of nested function declarations to
 * instantiate before the body runs.
 *
 * Recursion is bounded by cfg.max_parse_depth. That limit exists because
 * the user stack is 64 KiB, not because deep nesting is uninteresting.
 */

#define JS_INTERNAL
#include "js.h"

#include <string.h>
#include <stdio.h>

typedef struct {
    js_node *head, *tail;
    int      n;
} nodelist;

/* A cheap lexer bookmark. Copying the whole js_lexer would put ~350 bytes
 * on every recursive parser frame, which the 64 KiB user stack cannot
 * afford; backtracking over a single token never disturbs the paren stack
 * contents, so these fields are enough. */
typedef struct {
    unsigned long pos;
    int line, tok, nl_before, tok_line, prev_tok, paren_sp, last_paren;
    js_string *text, *flags;
    double num;
} lexmark;

static void lx_mark(const js_lexer *lx, lexmark *m)
{
    m->pos = lx->pos; m->line = lx->line; m->tok = lx->tok;
    m->nl_before = lx->nl_before; m->tok_line = lx->tok_line;
    m->prev_tok = lx->prev_tok; m->paren_sp = lx->paren_sp;
    m->last_paren = lx->last_paren; m->text = lx->text;
    m->flags = lx->flags; m->num = lx->num;
}

static void lx_reset(js_lexer *lx, const lexmark *m)
{
    lx->pos = m->pos; lx->line = m->line; lx->tok = m->tok;
    lx->nl_before = m->nl_before; lx->tok_line = m->tok_line;
    lx->prev_tok = m->prev_tok; lx->paren_sp = m->paren_sp;
    lx->last_paren = m->last_paren; lx->text = m->text;
    lx->flags = m->flags; lx->num = m->num;
}

typedef struct {
    js_ctx  *ctx;
    js_lexer lx;
    int      depth;
    int      err;
    /* hoisting collectors for the function currently being parsed */
    nodelist vars;
    nodelist funcs;
    int      iter_depth;
    int      switch_depth;
} parser;

static js_node *parse_assign(parser *p, int no_in);
static js_node *parse_expr(parser *p, int no_in);
static js_node *parse_stmt(parser *p);
static js_node *parse_function(parser *p, int is_decl);
static int      parse_func_rest(parser *p, js_node *f);

/* ------------------------------------------------------------------ */

static void list_add(nodelist *l, js_node *n)
{
    if (!n) return;
    n->next = 0;
    if (l->tail) l->tail->next = n;
    else l->head = n;
    l->tail = n;
    l->n++;
}

static js_node *nd(parser *p, int type)
{
    js_node *n;

    if (p->err)
        return 0;
    n = (js_node *)js_alloc(p->ctx, sizeof(js_node));
    if (!n) { p->err = 1; return 0; }
    n->type = (uint8_t)type;
    n->line = (uint32_t)p->lx.tok_line;
    return n;
}

/* Records a syntax error and always evaluates to NULL, so callers can
 * write `return perr(...)` from anything that returns a node. */
static js_node *perr(parser *p, const char *msg)
{
    if (!p->err) {
        p->err = 1;
        js_throw_error(p->ctx, JS_ERR_SYNTAX, "%s (line %d)", msg,
                       p->lx.tok_line);
    }
    return 0;
}

static int advance(parser *p)
{
    if (p->err)
        return 0;
    if (js_lex_next(&p->lx) != JS_OK) { p->err = 1; return 0; }
    return 1;
}

static int expect(parser *p, int tok, const char *what)
{
    if (p->err)
        return 0;
    if (p->lx.tok != tok) {
        char buf[64];
        snprintf(buf, sizeof(buf), "expected %s", what);
        perr(p, buf);
        return 0;
    }
    return advance(p);
}

static int enter(parser *p)
{
    if (p->err)
        return 0;
    if (++p->depth > p->ctx->cfg.max_parse_depth) {
        p->err = 1;
        js_throw_error(p->ctx, JS_ERR_SYNTAX,
                       "source nests deeper than %d levels (line %d)",
                       p->ctx->cfg.max_parse_depth, p->lx.tok_line);
        return 0;
    }
    return 1;
}

static void leave(parser *p) { p->depth--; }

/* Automatic semicolon insertion. */
static int semicolon(parser *p)
{
    if (p->err)
        return 0;
    if (p->lx.tok == TK_SEMI)
        return advance(p);
    if (p->lx.tok == TK_RBRACE || p->lx.tok == TK_EOF || p->lx.nl_before)
        return 1;
    perr(p, "expected ; or a line break");
    return 0;
}

/* Keywords are valid property names after `.` and inside object literals. */
static int is_name_token(int t)
{
    return t == TK_IDENT || t >= TK_BREAK;
}

/* ------------------------------------------------------------------ */
/* Expressions                                                         */
/* ------------------------------------------------------------------ */

static js_node *parse_args(parser *p, int *count)
{
    nodelist l;

    memset(&l, 0, sizeof(l));
    if (!expect(p, TK_LPAREN, "("))
        return 0;
    while (p->lx.tok != TK_RPAREN) {
        js_node *a = parse_assign(p, 0);
        if (!a) return 0;
        list_add(&l, a);
        if (p->lx.tok == TK_COMMA) { if (!advance(p)) return 0; continue; }
        break;
    }
    if (!expect(p, TK_RPAREN, ") to close the argument list"))
        return 0;
    if (count) *count = l.n;
    return l.head ? l.head : (js_node *)1;   /* 1 = empty but valid */
}

static js_node *parse_object(parser *p)
{
    js_node *n = nd(p, N_OBJECT);
    nodelist l;

    if (!n) return 0;
    memset(&l, 0, sizeof(l));
    if (!advance(p)) return 0;               /* past '{' */
    while (p->lx.tok != TK_RBRACE) {
        js_node *pr;
        js_string *key = 0;
        int kind = 0;

        if (p->err) return 0;
        /* get/set accessors -- only when a property name follows */
        if (p->lx.tok == TK_IDENT && p->lx.text->len == 3 &&
            (memcmp(p->lx.text->data, "get", 3) == 0 ||
             memcmp(p->lx.text->data, "set", 3) == 0)) {
            int want = p->lx.text->data[0] == 'g' ? 1 : 2;
            lexmark save;
            lx_mark(&p->lx, &save);
            if (!advance(p)) return 0;
            if (is_name_token(p->lx.tok) || p->lx.tok == TK_STR ||
                p->lx.tok == TK_NUM)
                kind = want;
            else
                lx_reset(&p->lx, &save);     /* it was just a key named get */
        }
        if (p->lx.tok == TK_STR) {
            key = p->lx.text;
        } else if (p->lx.tok == TK_NUM) {
            char buf[48];
            js_dtoa(p->lx.num, buf);
            key = js_str_intern(p->ctx, buf, strlen(buf));
        } else if (is_name_token(p->lx.tok)) {
            key = p->lx.text;
            if (!key) {                      /* keyword: rebuild its spelling */
                const char *w = js_token_name(p->lx.tok);
                key = js_str_intern(p->ctx, w, strlen(w));
            }
        } else {
            return perr(p, "expected a property name");
        }
        if (!key) { p->err = 1; return 0; }
        if (!advance(p)) return 0;

        pr = nd(p, N_PROP);
        if (!pr) return 0;
        pr->str = key;
        pr->op = (uint8_t)kind;
        if (kind) {
            js_node *f = nd(p, N_FUNC);
            if (!f || !parse_func_rest(p, f)) return 0;
            pr->a = f;
        } else {
            if (!expect(p, TK_COLON, ": after the property name")) return 0;
            pr->a = parse_assign(p, 0);
            if (!pr->a) return 0;
        }
        list_add(&l, pr);
        if (p->lx.tok == TK_COMMA) { if (!advance(p)) return 0; continue; }
        break;
    }
    if (!expect(p, TK_RBRACE, "} to close the object literal"))
        return 0;
    n->a = l.head;
    return n;
}

static js_node *parse_array(parser *p)
{
    js_node *n = nd(p, N_ARRAY);
    nodelist l;

    if (!n) return 0;
    memset(&l, 0, sizeof(l));
    if (!advance(p)) return 0;               /* past '[' */
    while (p->lx.tok != TK_RBRACKET) {
        if (p->err) return 0;
        if (p->lx.tok == TK_COMMA) {         /* elision */
            js_node *h = nd(p, N_EMPTY);
            if (!h) return 0;
            list_add(&l, h);
            if (!advance(p)) return 0;
            continue;
        }
        {
            js_node *e = parse_assign(p, 0);
            if (!e) return 0;
            list_add(&l, e);
        }
        if (p->lx.tok == TK_COMMA) { if (!advance(p)) return 0; continue; }
        break;
    }
    if (!expect(p, TK_RBRACKET, "] to close the array literal"))
        return 0;
    n->a = l.head;
    n->num = (double)l.n;
    return n;
}

static js_node *parse_primary(parser *p)
{
    js_node *n;

    if (p->err) return 0;
    switch (p->lx.tok) {
    case TK_THIS:
        n = nd(p, N_THIS);
        return (n && advance(p)) ? n : 0;
    case TK_NULL_KW:
        n = nd(p, N_NULL);
        return (n && advance(p)) ? n : 0;
    case TK_TRUE: case TK_FALSE:
        n = nd(p, N_BOOL);
        if (!n) return 0;
        n->num = p->lx.tok == TK_TRUE ? 1 : 0;
        return advance(p) ? n : 0;
    case TK_NUM:
        n = nd(p, N_NUM);
        if (!n) return 0;
        n->num = p->lx.num;
        return advance(p) ? n : 0;
    case TK_STR:
        n = nd(p, N_STR);
        if (!n) return 0;
        n->str = p->lx.text;
        return advance(p) ? n : 0;
    case TK_REGEX:
        n = nd(p, N_REGEX);
        if (!n) return 0;
        n->str = p->lx.text;
        n->str2 = p->lx.flags;
        return advance(p) ? n : 0;
    case TK_IDENT:
        n = nd(p, N_IDENT);
        if (!n) return 0;
        n->str = p->lx.text;
        return advance(p) ? n : 0;
    case TK_FUNCTION:
        return parse_function(p, 0);
    case TK_LBRACKET:
        return parse_array(p);
    case TK_LBRACE:
        return parse_object(p);
    case TK_LPAREN: {
        js_node *e;
        if (!advance(p)) return 0;
        e = parse_expr(p, 0);
        if (!e) return 0;
        if (!expect(p, TK_RPAREN, ") to close the group")) return 0;
        return e;
    }
    default:
        return perr(p, "unexpected token in an expression");
    }
}

/* `.name` and `[expr]` only; used for the callee of `new`. */
static js_node *parse_member_tail(parser *p, js_node *base)
{
    for (;;) {
        if (p->err) return 0;
        if (p->lx.tok == TK_DOT) {
            js_node *m;
            if (!advance(p)) return 0;
            if (!is_name_token(p->lx.tok))
                return perr(p, "expected a property name after .");
            m = nd(p, N_MEMBER);
            if (!m) return 0;
            m->a = base;
            m->str = p->lx.text;
            if (!m->str) {
                const char *w = js_token_name(p->lx.tok);
                m->str = js_str_intern(p->ctx, w, strlen(w));
                if (!m->str) { p->err = 1; return 0; }
            }
            if (!advance(p)) return 0;
            base = m;
            continue;
        }
        if (p->lx.tok == TK_LBRACKET) {
            js_node *m;
            if (!advance(p)) return 0;
            m = nd(p, N_INDEX);
            if (!m) return 0;
            m->a = base;
            m->b = parse_expr(p, 0);
            if (!m->b) return 0;
            if (!expect(p, TK_RBRACKET, "]")) return 0;
            base = m;
            continue;
        }
        return base;
    }
}

static js_node *parse_new(parser *p)
{
    js_node *n, *callee;

    if (!enter(p)) return 0;
    if (!advance(p)) { leave(p); return 0; }     /* past 'new' */
    callee = (p->lx.tok == TK_NEW) ? parse_new(p) : parse_primary(p);
    if (!callee) { leave(p); return 0; }
    callee = parse_member_tail(p, callee);
    if (!callee) { leave(p); return 0; }
    n = nd(p, N_NEW);
    if (!n) { leave(p); return 0; }
    n->a = callee;
    if (p->lx.tok == TK_LPAREN) {
        int cnt = 0;
        js_node *args = parse_args(p, &cnt);
        if (!args) { leave(p); return 0; }
        n->b = (args == (js_node *)1) ? 0 : args;
        n->num = cnt;
    }
    leave(p);
    return n;
}

static js_node *parse_lhs(parser *p)
{
    js_node *e;

    if (!enter(p)) return 0;
    e = (p->lx.tok == TK_NEW) ? parse_new(p) : parse_primary(p);
    if (!e) { leave(p); return 0; }
    for (;;) {
        if (p->err) { leave(p); return 0; }
        if (p->lx.tok == TK_DOT || p->lx.tok == TK_LBRACKET) {
            js_node *b = parse_member_tail(p, e);
            if (!b) { leave(p); return 0; }
            if (b == e) break;
            e = b;
            continue;
        }
        if (p->lx.tok == TK_LPAREN) {
            int cnt = 0;
            js_node *args = parse_args(p, &cnt);
            js_node *c;
            if (!args) { leave(p); return 0; }
            c = nd(p, N_CALL);
            if (!c) { leave(p); return 0; }
            c->a = e;
            c->b = (args == (js_node *)1) ? 0 : args;
            c->num = cnt;
            e = c;
            continue;
        }
        break;
    }
    leave(p);
    return e;
}

static int is_target(js_node *n)
{
    return n && (n->type == N_IDENT || n->type == N_MEMBER || n->type == N_INDEX);
}

static js_node *parse_unary(parser *p)
{
    int t = p->lx.tok;
    js_node *n;

    if (p->err) return 0;
    switch (t) {
    case TK_DELETE: case TK_VOID: case TK_TYPEOF:
    case TK_ADD: case TK_SUB: case TK_BNOT: case TK_NOT:
        if (!enter(p)) return 0;
        n = nd(p, N_UNARY);
        if (!n) { leave(p); return 0; }
        n->op = (uint8_t)t;
        if (!advance(p)) { leave(p); return 0; }
        n->a = parse_unary(p);
        leave(p);
        return n->a ? n : 0;
    case TK_INC: case TK_DEC:
        if (!enter(p)) return 0;
        n = nd(p, N_PREFIX);
        if (!n) { leave(p); return 0; }
        n->op = (uint8_t)t;
        if (!advance(p)) { leave(p); return 0; }
        n->a = parse_unary(p);
        leave(p);
        if (!n->a) return 0;
        if (!is_target(n->a))
            return perr(p, "invalid target for ++/--");
        return n;
    default:
        break;
    }
    n = parse_lhs(p);
    if (!n) return 0;
    if ((p->lx.tok == TK_INC || p->lx.tok == TK_DEC) && !p->lx.nl_before) {
        js_node *q = nd(p, N_POSTFIX);
        if (!q) return 0;
        q->op = (uint8_t)p->lx.tok;
        q->a = n;
        if (!is_target(n))
            return perr(p, "invalid target for ++/--");
        if (!advance(p)) return 0;
        return q;
    }
    return n;
}

static int binprec(int tok, int no_in)
{
    switch (tok) {
    case TK_OROR:   return 1;
    case TK_ANDAND: return 2;
    case TK_BOR:    return 3;
    case TK_BXOR:   return 4;
    case TK_BAND:   return 5;
    case TK_EQ: case TK_NE: case TK_SEQ: case TK_SNE: return 6;
    case TK_LT: case TK_GT: case TK_LE: case TK_GE:
    case TK_INSTANCEOF: return 7;
    case TK_IN:     return no_in ? 0 : 7;
    case TK_SHL: case TK_SHR: case TK_USHR: return 8;
    case TK_ADD: case TK_SUB: return 9;
    case TK_MUL: case TK_DIV: case TK_MOD: return 10;
    default: return 0;
    }
}

static js_node *parse_bin(parser *p, int minprec, int no_in)
{
    js_node *lhs;

    if (!enter(p)) return 0;
    lhs = parse_unary(p);
    if (!lhs) { leave(p); return 0; }
    for (;;) {
        int op = p->lx.tok;
        int prec = binprec(op, no_in);
        js_node *rhs, *b;

        if (p->err) { leave(p); return 0; }
        if (prec == 0 || prec < minprec)
            break;
        if (!advance(p)) { leave(p); return 0; }
        rhs = parse_bin(p, prec + 1, no_in);
        if (!rhs) { leave(p); return 0; }
        b = nd(p, (op == TK_ANDAND || op == TK_OROR) ? N_LOGICAL : N_BINARY);
        if (!b) { leave(p); return 0; }
        b->op = (uint8_t)op;
        b->a = lhs;
        b->b = rhs;
        lhs = b;
    }
    leave(p);
    return lhs;
}

static js_node *parse_cond(parser *p, int no_in)
{
    js_node *t = parse_bin(p, 1, no_in);

    if (!t) return 0;
    if (p->lx.tok == TK_QUESTION) {
        js_node *n;
        if (!enter(p)) return 0;
        n = nd(p, N_COND);
        if (!n) { leave(p); return 0; }
        n->a = t;
        if (!advance(p)) { leave(p); return 0; }
        n->b = parse_assign(p, 0);
        if (!n->b || !expect(p, TK_COLON, ": in the conditional expression")) {
            leave(p); return 0;
        }
        n->c = parse_assign(p, no_in);
        leave(p);
        return n->c ? n : 0;
    }
    return t;
}

static int is_assign_tok(int t)
{
    return t == TK_ASSIGN || (t >= TK_ADD_A && t <= TK_BXOR_A);
}

static js_node *parse_assign(parser *p, int no_in)
{
    js_node *lhs;

    if (!enter(p)) return 0;
    lhs = parse_cond(p, no_in);
    if (!lhs) { leave(p); return 0; }
    if (is_assign_tok(p->lx.tok)) {
        js_node *n;
        if (!is_target(lhs)) {
            leave(p);
            return perr(p, "invalid assignment target");
        }
        n = nd(p, N_ASSIGN);
        if (!n) { leave(p); return 0; }
        n->op = (uint8_t)p->lx.tok;
        n->a = lhs;
        if (!advance(p)) { leave(p); return 0; }
        n->b = parse_assign(p, no_in);
        leave(p);
        return n->b ? n : 0;
    }
    leave(p);
    return lhs;
}

static js_node *parse_expr(parser *p, int no_in)
{
    js_node *e = parse_assign(p, no_in);

    if (!e) return 0;
    while (p->lx.tok == TK_COMMA) {
        js_node *n;
        if (!enter(p)) return 0;
        n = nd(p, N_SEQ);
        if (!n) { leave(p); return 0; }
        n->a = e;
        if (!advance(p)) { leave(p); return 0; }
        n->b = parse_assign(p, no_in);
        leave(p);
        if (!n->b) return 0;
        e = n;
    }
    return e;
}

/* ------------------------------------------------------------------ */
/* Functions                                                           */
/* ------------------------------------------------------------------ */

static void hoist_var(parser *p, js_string *name)
{
    js_node *q;

    for (q = p->vars.head; q; q = q->next)
        if (q->str == name)
            return;
    q = nd(p, N_IDENT);
    if (!q) return;
    q->str = name;
    list_add(&p->vars, q);
}

/* Parses `( params ) { body }` into an already-allocated N_FUNC node.
 * Split out because object-literal accessors have no `function` keyword. */
static int parse_func_rest(parser *p, js_node *f)
{
    nodelist params, body;
    nodelist saved_vars = p->vars, saved_funcs = p->funcs;
    int saved_iter = p->iter_depth, saved_switch = p->switch_depth;

    if (!enter(p)) return 0;
    memset(&params, 0, sizeof(params));
    memset(&body, 0, sizeof(body));
    memset(&p->vars, 0, sizeof(p->vars));
    memset(&p->funcs, 0, sizeof(p->funcs));
    p->iter_depth = 0;
    p->switch_depth = 0;

    if (!expect(p, TK_LPAREN, "( after the function name")) goto fail;
    while (p->lx.tok != TK_RPAREN) {
        js_node *a;
        if (p->lx.tok != TK_IDENT) { perr(p, "expected a parameter name"); goto fail; }
        a = nd(p, N_IDENT);
        if (!a) goto fail;
        a->str = p->lx.text;
        list_add(&params, a);
        if (!advance(p)) goto fail;
        if (p->lx.tok == TK_COMMA) { if (!advance(p)) goto fail; continue; }
        break;
    }
    if (!expect(p, TK_RPAREN, ") to close the parameter list")) goto fail;
    if (!expect(p, TK_LBRACE, "{ to open the function body")) goto fail;
    while (p->lx.tok != TK_RBRACE && p->lx.tok != TK_EOF) {
        js_node *st = parse_stmt(p);
        if (!st) goto fail;
        list_add(&body, st);
    }
    if (!expect(p, TK_RBRACE, "} to close the function body")) goto fail;

    f->a = params.head;
    f->b = body.head;
    f->c = p->vars.head;
    f->d = p->funcs.head;
    f->num = params.n;

    p->vars = saved_vars;
    p->funcs = saved_funcs;
    p->iter_depth = saved_iter;
    p->switch_depth = saved_switch;
    leave(p);
    return 1;

fail:
    p->vars = saved_vars;
    p->funcs = saved_funcs;
    p->iter_depth = saved_iter;
    p->switch_depth = saved_switch;
    leave(p);
    return 0;
}

static js_node *parse_function(parser *p, int is_decl)
{
    js_node *f;

    if (p->err) return 0;
    f = nd(p, N_FUNC);
    if (!f) return 0;
    if (!advance(p)) return 0;                      /* past 'function' */
    if (p->lx.tok == TK_IDENT) {
        f->str = p->lx.text;
        if (!advance(p)) return 0;
    } else if (is_decl) {
        return perr(p, "a function declaration needs a name");
    }
    return parse_func_rest(p, f) ? f : 0;
}

/* ------------------------------------------------------------------ */
/* Statements                                                          */
/* ------------------------------------------------------------------ */

static js_node *parse_var(parser *p, int no_in, int *count)
{
    js_node *n = nd(p, N_VAR);
    nodelist l;

    if (!n) return 0;
    memset(&l, 0, sizeof(l));
    if (!advance(p)) return 0;               /* past 'var' */
    for (;;) {
        js_node *d;
        if (p->lx.tok != TK_IDENT)
            return perr(p, "expected a variable name");
        d = nd(p, N_VARDECL);
        if (!d) return 0;
        d->str = p->lx.text;
        hoist_var(p, d->str);
        if (!advance(p)) return 0;
        if (p->lx.tok == TK_ASSIGN) {
            if (!advance(p)) return 0;
            d->a = parse_assign(p, no_in);
            if (!d->a) return 0;
        }
        list_add(&l, d);
        if (p->lx.tok == TK_COMMA) { if (!advance(p)) return 0; continue; }
        break;
    }
    n->a = l.head;
    if (count) *count = l.n;
    return n;
}

static js_node *parse_block(parser *p)
{
    js_node *n = nd(p, N_BLOCK);
    nodelist l;

    if (!n) return 0;
    memset(&l, 0, sizeof(l));
    if (!advance(p)) return 0;               /* past '{' */
    while (p->lx.tok != TK_RBRACE && p->lx.tok != TK_EOF) {
        js_node *st = parse_stmt(p);
        if (!st) return 0;
        list_add(&l, st);
    }
    if (!expect(p, TK_RBRACE, "} to close the block")) return 0;
    n->a = l.head;
    return n;
}

static js_node *parse_for(parser *p)
{
    js_node *init = 0;
    int is_var = 0, ndecl = 0;

    if (!advance(p)) return 0;               /* past 'for' */
    if (!expect(p, TK_LPAREN, "( after for")) return 0;

    if (p->lx.tok == TK_VAR || p->lx.tok == TK_CONST ||
        p->lx.tok == TK_LET) {
        init = parse_var(p, 1, &ndecl);
        if (!init) return 0;
        is_var = 1;
    } else if (p->lx.tok != TK_SEMI) {
        init = parse_expr(p, 1);
        if (!init) return 0;
    }

    if (p->lx.tok == TK_IN) {
        js_node *n = nd(p, N_FORIN);
        if (!n) return 0;
        if (is_var) {
            if (ndecl != 1)
                return perr(p,
                    "for-in accepts exactly one variable declaration");
            n->a = init->a;                  /* the single N_VARDECL */
            n->flags = 1;
        } else {
            if (!is_target(init))
                return perr(p, "invalid for-in target");
            n->a = init;
        }
        if (!advance(p)) return 0;
        n->b = parse_expr(p, 0);
        if (!n->b) return 0;
        if (!expect(p, TK_RPAREN, ") to close the for-in header")) return 0;
        p->iter_depth++;
        n->c = parse_stmt(p);
        p->iter_depth--;
        return n->c ? n : 0;
    }

    {
        js_node *n = nd(p, N_FOR);
        if (!n) return 0;
        if (init) {
            if (is_var) n->a = init;
            else {
                js_node *e = nd(p, N_EXPRSTMT);
                if (!e) return 0;
                e->a = init;
                n->a = e;
            }
        }
        if (!expect(p, TK_SEMI, "; in the for header")) return 0;
        if (p->lx.tok != TK_SEMI) {
            n->b = parse_expr(p, 0);
            if (!n->b) return 0;
        }
        if (!expect(p, TK_SEMI, "; in the for header")) return 0;
        if (p->lx.tok != TK_RPAREN) {
            n->c = parse_expr(p, 0);
            if (!n->c) return 0;
        }
        if (!expect(p, TK_RPAREN, ") to close the for header")) return 0;
        p->iter_depth++;
        n->d = parse_stmt(p);
        p->iter_depth--;
        return n->d ? n : 0;
    }
}

static js_node *parse_switch(parser *p)
{
    js_node *n = nd(p, N_SWITCH);
    nodelist cases;
    int seen_default = 0;

    if (!n) return 0;
    memset(&cases, 0, sizeof(cases));
    if (!advance(p)) return 0;
    if (!expect(p, TK_LPAREN, "( after switch")) return 0;
    n->a = parse_expr(p, 0);
    if (!n->a) return 0;
    if (!expect(p, TK_RPAREN, ") after the switch expression")) return 0;
    if (!expect(p, TK_LBRACE, "{ to open the switch body")) return 0;

    p->switch_depth++;
    while (p->lx.tok != TK_RBRACE && p->lx.tok != TK_EOF) {
        js_node *c = nd(p, N_CASE);
        nodelist body;
        if (!c) { p->switch_depth--; return 0; }
        memset(&body, 0, sizeof(body));
        if (p->lx.tok == TK_CASE) {
            if (!advance(p)) { p->switch_depth--; return 0; }
            c->a = parse_expr(p, 0);
            if (!c->a) { p->switch_depth--; return 0; }
        } else if (p->lx.tok == TK_DEFAULT) {
            if (seen_default) {
                p->switch_depth--;
                return perr(p, "duplicate default clause");
            }
            seen_default = 1;
            if (!advance(p)) { p->switch_depth--; return 0; }
        } else {
            p->switch_depth--;
            return perr(p, "expected case or default");
        }
        if (!expect(p, TK_COLON, ": after the case label")) { p->switch_depth--; return 0; }
        while (p->lx.tok != TK_CASE && p->lx.tok != TK_DEFAULT &&
               p->lx.tok != TK_RBRACE && p->lx.tok != TK_EOF) {
            js_node *st = parse_stmt(p);
            if (!st) { p->switch_depth--; return 0; }
            list_add(&body, st);
        }
        c->b = body.head;
        list_add(&cases, c);
    }
    p->switch_depth--;
    if (!expect(p, TK_RBRACE, "} to close the switch body")) return 0;
    n->b = cases.head;
    return n;
}

static js_node *parse_try(parser *p)
{
    js_node *n = nd(p, N_TRY);

    if (!n) return 0;
    if (!advance(p)) return 0;
    if (p->lx.tok != TK_LBRACE)
        return perr(p, "expected { after try");
    n->a = parse_block(p);
    if (!n->a) return 0;
    if (p->lx.tok == TK_CATCH) {
        if (!advance(p)) return 0;
        if (!expect(p, TK_LPAREN, "( after catch")) return 0;
        if (p->lx.tok != TK_IDENT)
            return perr(p, "expected the catch parameter");
        n->str = p->lx.text;
        if (!advance(p)) return 0;
        if (!expect(p, TK_RPAREN, ") after the catch parameter")) return 0;
        if (p->lx.tok != TK_LBRACE)
            return perr(p, "expected { after catch(...)");
        n->b = parse_block(p);
        if (!n->b) return 0;
    }
    if (p->lx.tok == TK_FINALLY) {
        if (!advance(p)) return 0;
        if (p->lx.tok != TK_LBRACE)
            return perr(p, "expected { after finally");
        n->c = parse_block(p);
        if (!n->c) return 0;
    }
    if (!n->b && !n->c)
        return perr(p, "try needs a catch or a finally");
    return n;
}

static js_node *parse_stmt(parser *p)
{
    js_node *n;

    if (p->err) return 0;
    if (!enter(p)) return 0;

    switch (p->lx.tok) {
    case TK_LBRACE:
        n = parse_block(p);
        leave(p);
        return n;

    case TK_SEMI:
        n = nd(p, N_EMPTY);
        leave(p);
        return (n && advance(p)) ? n : 0;

    case TK_VAR:
    case TK_CONST:
    case TK_LET:
        n = parse_var(p, 0, 0);
        leave(p);
        return (n && semicolon(p)) ? n : 0;

    case TK_FUNCTION: {
        /* Declarations are instantiated on scope entry, so the statement
         * position itself does nothing. */
        js_node *f = parse_function(p, 1);
        js_node *ref;
        if (!f) { leave(p); return 0; }
        ref = nd(p, N_FUNCDECL);
        if (!ref) { leave(p); return 0; }
        ref->a = f;
        ref->str = f->str;
        list_add(&p->funcs, ref);
        hoist_var(p, f->str);
        n = nd(p, N_EMPTY);
        leave(p);
        return n;
    }

    case TK_IF:
        n = nd(p, N_IF);
        if (!n) { leave(p); return 0; }
        if (!advance(p) || !expect(p, TK_LPAREN, "( after if")) { leave(p); return 0; }
        n->a = parse_expr(p, 0);
        if (!n->a || !expect(p, TK_RPAREN, ") after the if condition")) { leave(p); return 0; }
        n->b = parse_stmt(p);
        if (!n->b) { leave(p); return 0; }
        if (p->lx.tok == TK_ELSE) {
            if (!advance(p)) { leave(p); return 0; }
            n->c = parse_stmt(p);
            if (!n->c) { leave(p); return 0; }
        }
        leave(p);
        return n;

    case TK_DO:
        n = nd(p, N_DO);
        if (!n) { leave(p); return 0; }
        if (!advance(p)) { leave(p); return 0; }
        p->iter_depth++;
        n->a = parse_stmt(p);
        p->iter_depth--;
        if (!n->a) { leave(p); return 0; }
        if (!expect(p, TK_WHILE, "while after the do body") ||
            !expect(p, TK_LPAREN, "( after while")) { leave(p); return 0; }
        n->b = parse_expr(p, 0);
        if (!n->b || !expect(p, TK_RPAREN, ") after the while condition")) {
            leave(p); return 0;
        }
        if (p->lx.tok == TK_SEMI && !advance(p)) { leave(p); return 0; }
        leave(p);
        return n;

    case TK_WHILE:
        n = nd(p, N_WHILE);
        if (!n) { leave(p); return 0; }
        if (!advance(p) || !expect(p, TK_LPAREN, "( after while")) { leave(p); return 0; }
        n->a = parse_expr(p, 0);
        if (!n->a || !expect(p, TK_RPAREN, ") after the while condition")) {
            leave(p); return 0;
        }
        p->iter_depth++;
        n->b = parse_stmt(p);
        p->iter_depth--;
        leave(p);
        return n->b ? n : 0;

    case TK_FOR:
        n = parse_for(p);
        leave(p);
        return n;

    case TK_CONTINUE:
    case TK_BREAK: {
        int is_break = p->lx.tok == TK_BREAK;
        n = nd(p, is_break ? N_BREAK : N_CONTINUE);
        if (!n) { leave(p); return 0; }
        if (!advance(p)) { leave(p); return 0; }
        if (p->lx.tok == TK_IDENT && !p->lx.nl_before) {
            n->str = p->lx.text;
            if (!advance(p)) { leave(p); return 0; }
        } else if (is_break) {
            if (p->iter_depth == 0 && p->switch_depth == 0) {
                leave(p);
                return perr(p, "break outside a loop or switch");
            }
        } else if (p->iter_depth == 0) {
            leave(p);
            return perr(p, "continue outside a loop");
        }
        leave(p);
        return semicolon(p) ? n : 0;
    }

    case TK_RETURN:
        n = nd(p, N_RETURN);
        if (!n) { leave(p); return 0; }
        if (!advance(p)) { leave(p); return 0; }
        if (p->lx.tok != TK_SEMI && p->lx.tok != TK_RBRACE &&
            p->lx.tok != TK_EOF && !p->lx.nl_before) {
            n->a = parse_expr(p, 0);
            if (!n->a) { leave(p); return 0; }
        }
        leave(p);
        return semicolon(p) ? n : 0;

    case TK_THROW:
        n = nd(p, N_THROW);
        if (!n) { leave(p); return 0; }
        if (!advance(p)) { leave(p); return 0; }
        if (p->lx.nl_before) {
            leave(p);
            return perr(p, "no line break allowed after throw");
        }
        n->a = parse_expr(p, 0);
        leave(p);
        if (!n->a) return 0;
        return semicolon(p) ? n : 0;

    case TK_WITH:
        n = nd(p, N_WITH);
        if (!n) { leave(p); return 0; }
        if (!advance(p) || !expect(p, TK_LPAREN, "( after with")) { leave(p); return 0; }
        n->a = parse_expr(p, 0);
        if (!n->a || !expect(p, TK_RPAREN, ") after the with expression")) {
            leave(p); return 0;
        }
        n->b = parse_stmt(p);
        leave(p);
        return n->b ? n : 0;

    case TK_SWITCH:
        n = parse_switch(p);
        leave(p);
        return n;

    case TK_TRY:
        n = parse_try(p);
        leave(p);
        return n;

    case TK_DEBUGGER:
        n = nd(p, N_DEBUGGER);
        if (!n || !advance(p)) { leave(p); return 0; }
        leave(p);
        return semicolon(p) ? n : 0;

    default:
        break;
    }

    /* labelled statement, or an expression statement */
    if (p->lx.tok == TK_IDENT) {
        lexmark save;
        js_string *label = p->lx.text;
        lx_mark(&p->lx, &save);
        if (!advance(p)) { leave(p); return 0; }
        if (p->lx.tok == TK_COLON) {
            js_node *l = nd(p, N_LABEL);
            if (!l || !advance(p)) { leave(p); return 0; }
            l->str = label;
            /* a label makes `continue label` legal for the loop it names */
            p->iter_depth++;
            l->a = parse_stmt(p);
            p->iter_depth--;
            leave(p);
            return l->a ? l : 0;
        }
        lx_reset(&p->lx, &save);
    }
    if (p->lx.tok == TK_LBRACE || p->lx.tok == TK_FUNCTION) {
        leave(p);
        return perr(p,
            "an expression statement may not start with { or function");
    }
    n = nd(p, N_EXPRSTMT);
    if (!n) { leave(p); return 0; }
    n->a = parse_expr(p, 0);
    leave(p);
    if (!n->a) return 0;
    return semicolon(p) ? n : 0;
}

/* ------------------------------------------------------------------ */

js_node *js_parse(js_ctx *ctx, const char *src, const char *name)
{
    parser p;
    js_node *prog;
    nodelist body;

    memset(&p, 0, sizeof(p));
    p.ctx = ctx;
    ctx->script_name = name ? name : "<script>";
    js_lex_init(&p.lx, ctx, src);
    if (js_lex_next(&p.lx) != JS_OK)
        return 0;

    prog = nd(&p, N_PROGRAM);
    if (!prog)
        return 0;
    memset(&body, 0, sizeof(body));
    while (p.lx.tok != TK_EOF) {
        js_node *st = parse_stmt(&p);
        if (!st || p.err)
            return 0;
        list_add(&body, st);
    }
    prog->a = body.head;
    prog->c = p.vars.head;
    prog->d = p.funcs.head;
    return prog;
}
