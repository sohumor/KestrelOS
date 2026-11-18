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
    js_string *text, *flags, *raw;
    int tpl_err;
    double num;
} lexmark;

static void lx_mark(const js_lexer *lx, lexmark *m)
{
    m->pos = lx->pos; m->line = lx->line; m->tok = lx->tok;
    m->nl_before = lx->nl_before; m->tok_line = lx->tok_line;
    m->prev_tok = lx->prev_tok; m->paren_sp = lx->paren_sp;
    m->last_paren = lx->last_paren; m->text = lx->text;
    m->flags = lx->flags; m->raw = lx->raw; m->tpl_err = lx->tpl_err;
    m->num = lx->num;
}

static void lx_reset(js_lexer *lx, const lexmark *m)
{
    lx->pos = m->pos; lx->line = m->line; lx->tok = m->tok;
    lx->nl_before = m->nl_before; lx->tok_line = m->tok_line;
    lx->prev_tok = m->prev_tok; lx->paren_sp = m->paren_sp;
    lx->last_paren = m->last_paren; lx->text = m->text;
    lx->flags = m->flags; lx->raw = m->raw; lx->tpl_err = m->tpl_err;
    lx->num = m->num;
}

typedef struct {
    js_ctx  *ctx;
    js_lexer lx;
    int      depth;
    int      err;
    /* hoisting collectors for the function / block currently being parsed */
    nodelist vars;
    nodelist funcs;
    nodelist lex;
    int      iter_depth;
    int      switch_depth;
    int      in_async;
    int      has_await;
    int      in_method;
    int      in_derived_ctor;
} parser;

static js_node *parse_assign(parser *p, int no_in);
static js_node *parse_expr(parser *p, int no_in);
static js_node *parse_stmt(parser *p);
static js_node *parse_function(parser *p, int is_decl, int is_async);
static int      parse_func_rest(parser *p, js_node *f, int is_async,
                                int is_method, int is_derived_ctor);
static js_node *parse_class(parser *p, int is_decl);
static js_node *parse_template(parser *p, int tagged);
static js_node *parse_pattern(parser *p);
static js_node *expr_to_pattern(parser *p, js_node *e, int is_decl, int allow_paren);

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

static int is_kw_ident(const parser *p, const char *w, unsigned long len)
{
    return p->lx.tok == TK_IDENT && p->lx.text &&
           p->lx.text->len == len &&
           memcmp(p->lx.text->data, w, len) == 0;
}

static js_string *token_name_str(parser *p)
{
    js_string *s = p->lx.text;
    if (!s) {
        const char *w = js_token_name(p->lx.tok);
        s = js_str_intern(p->ctx, w, strlen(w));
        if (!s) { p->err = 1; return 0; }
    }
    return s;
}

static int reject_bad_expr(parser *p, js_node *n)
{
    if (!n) return 0;
    if (n->flags & JS_NODE_SHORTHAND_DEF) {
        perr(p, "invalid shorthand property initializer");
        return 0;
    }
    return 1;
}

static int mark_await(js_node *n, int depth)
{
    int has = 0;

    if (!n || depth > 128)
        return 0;
    if (n->type == N_FUNC)
        return 0;
    if (n->type == N_AWAIT)
        has = 1;
    if (mark_await(n->a, depth + 1)) has = 1;
    if (mark_await(n->b, depth + 1)) has = 1;
    if (n->type != N_CLASS) {
        if (mark_await(n->c, depth + 1)) has = 1;
        if (mark_await(n->d, depth + 1)) has = 1;
        if (mark_await(n->e, depth + 1)) has = 1;
    } else {
        /* For N_CLASS inside an async function, check computed method keys
         * in n->c without descending into method bodies. */
        js_node *m;
        for (m = n->c; m; m = m->next)
            if (mark_await(m->b, depth + 1))
                has = 1;
    }
    if (has)
        n->flags |= JS_NODE_HAS_AWAIT;
    if (mark_await(n->next, depth + 1)) {
        /* sibling carries its own flag; parent list walker checks */
        return 1;
    }
    return has;
}

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

static int hoist_lex(parser *p, js_string *name, int is_const)
{
    js_node *q;

    for (q = p->lex.head; q; q = q->next) {
        if (q->str == name) {
            perr(p, "identifier has already been declared");
            return 0;
        }
    }
    q = nd(p, N_IDENT);
    if (!q) return 0;
    q->str = name;
    q->op = (uint8_t)is_const;
    list_add(&p->lex, q);
    return 1;
}

static int collect_pat_names(parser *p, js_node *pat, int decl_kind)
{
    js_node *q;

    if (!pat)
        return 1;
    switch (pat->type) {
    case N_IDENT:
        if (decl_kind == 0) {
            hoist_var(p, pat->str);
            return !p->err;
        }
        return hoist_lex(p, pat->str, decl_kind == 2);
    case N_DEFAULT:
    case N_REST:
        return collect_pat_names(p, pat->a, decl_kind);
    case N_ARR_PAT:
        for (q = pat->a; q; q = q->next)
            if (!collect_pat_names(p, q, decl_kind))
                return 0;
        return 1;
    case N_OBJ_PAT:
        for (q = pat->a; q; q = q->next) {
            if (q->type == N_PROP) {
                if (!collect_pat_names(p, q->a, decl_kind))
                    return 0;
            } else if (!collect_pat_names(p, q, decl_kind)) {
                return 0;
            }
        }
        return 1;
    default:
        return 1;
    }
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
        js_node *a;
        if (p->lx.tok == TK_DOTDOTDOT) {
            js_node *sp;
            if (!advance(p)) return 0;
            sp = nd(p, N_SPREAD);
            if (!sp) return 0;
            sp->a = parse_assign(p, 0);
            if (!sp->a || !reject_bad_expr(p, sp->a)) return 0;
            a = sp;
        } else {
            a = parse_assign(p, 0);
            if (!a || !reject_bad_expr(p, a)) return 0;
        }
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
        js_node *pr, *comp_key = 0;
        js_string *key = 0;
        int kind = 0, is_async = 0, was_ident = 0;

        if (p->err) return 0;

        /* object spread: { ...expr } */
        if (p->lx.tok == TK_DOTDOTDOT) {
            if (!advance(p)) return 0;
            pr = nd(p, N_PROP);
            if (!pr) return 0;
            pr->op = 3;
            pr->a = parse_assign(p, 0);
            if (!pr->a) return 0;
            if (pr->a->flags & JS_NODE_SHORTHAND_DEF)
                n->flags |= JS_NODE_SHORTHAND_DEF;
            list_add(&l, pr);
            if (p->lx.tok == TK_COMMA) { if (!advance(p)) return 0; continue; }
            break;
        }

        /* async method: { async foo() {} } or { async [expr]() {} } */
        if (is_kw_ident(p, "async", 5)) {
            lexmark save;
            lx_mark(&p->lx, &save);
            if (!advance(p)) return 0;
            if (!p->lx.nl_before &&
                (p->lx.tok == TK_LBRACKET || is_name_token(p->lx.tok) ||
                 p->lx.tok == TK_STR || p->lx.tok == TK_NUM)) {
                is_async = 1;
            } else {
                lx_reset(&p->lx, &save);
            }
        }

        /* get/set accessors -- only when a property name follows */
        if (!is_async && p->lx.tok == TK_IDENT && p->lx.text->len == 3 &&
            (memcmp(p->lx.text->data, "get", 3) == 0 ||
             memcmp(p->lx.text->data, "set", 3) == 0)) {
            int want = p->lx.text->data[0] == 'g' ? 1 : 2;
            lexmark save;
            lx_mark(&p->lx, &save);
            if (!advance(p)) return 0;
            if (p->lx.tok == TK_LBRACKET || is_name_token(p->lx.tok) ||
                p->lx.tok == TK_STR || p->lx.tok == TK_NUM)
                kind = want;
            else
                lx_reset(&p->lx, &save);     /* it was just a key named get */
        }

        if (p->lx.tok == TK_LBRACKET) {
            if (!advance(p)) return 0;
            comp_key = parse_assign(p, 0);
            if (!comp_key || !reject_bad_expr(p, comp_key) ||
                !expect(p, TK_RBRACKET, "]"))
                return 0;
        } else if (p->lx.tok == TK_STR) {
            key = p->lx.text;
            if (!advance(p)) return 0;
        } else if (p->lx.tok == TK_NUM) {
            char buf[48];
            js_dtoa(p->lx.num, buf);
            key = js_str_intern(p->ctx, buf, strlen(buf));
            if (!key || !advance(p)) return 0;
        } else if (is_name_token(p->lx.tok)) {
            was_ident = (p->lx.tok == TK_IDENT);
            key = token_name_str(p);
            if (!key || !advance(p)) return 0;
        } else {
            return perr(p, "expected a property name");
        }

        pr = nd(p, N_PROP);
        if (!pr) return 0;
        pr->str = key;
        pr->b = comp_key;
        pr->op = (uint8_t)kind;

        if (kind) {
            js_node *f = nd(p, N_FUNC);
            if (!f || !parse_func_rest(p, f, 0, 1, 0)) return 0;
            f->str = key;
            f->op = 1;
            pr->a = f;
        } else if (is_async || p->lx.tok == TK_LPAREN) {
            js_node *f = nd(p, N_FUNC);
            if (!f || !parse_func_rest(p, f, is_async, 1, 0)) return 0;
            f->str = key;
            f->op = 1;
            pr->a = f;
        } else if (p->lx.tok == TK_COLON) {
            if (!advance(p)) return 0;
            pr->a = parse_assign(p, 0);
            if (!pr->a) return 0;
            if (pr->a->flags & JS_NODE_SHORTHAND_DEF)
                n->flags |= JS_NODE_SHORTHAND_DEF;
        } else if (was_ident && !comp_key &&
                   (p->lx.tok == TK_COMMA || p->lx.tok == TK_RBRACE)) {
            js_node *id = nd(p, N_IDENT);
            if (!id) return 0;
            id->str = key;
            pr->a = id;
        } else if (was_ident && !comp_key && p->lx.tok == TK_ASSIGN) {
            /* Cover grammar for `{ a = default }` destructuring */
            if (!advance(p)) return 0;
            pr->op = 4;
            pr->b = parse_assign(p, 0);
            if (!pr->b || !reject_bad_expr(p, pr->b)) return 0;
            n->flags |= JS_NODE_SHORTHAND_DEF;
        } else {
            return perr(p, "expected : after the property name");
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
    int trailing_comma = 0;

    if (!n) return 0;
    memset(&l, 0, sizeof(l));
    if (!advance(p)) return 0;               /* past '[' */
    while (p->lx.tok != TK_RBRACKET) {
        if (p->err) return 0;
        trailing_comma = 0;
        if (p->lx.tok == TK_COMMA) {         /* elision */
            js_node *h = nd(p, N_EMPTY);
            if (!h) return 0;
            list_add(&l, h);
            if (!advance(p)) return 0;
            continue;
        }
        if (p->lx.tok == TK_DOTDOTDOT) {
            js_node *sp;
            if (!advance(p)) return 0;
            sp = nd(p, N_SPREAD);
            if (!sp) return 0;
            sp->a = parse_assign(p, 0);
            if (!sp->a) return 0;
            if (sp->a->flags & JS_NODE_SHORTHAND_DEF)
                n->flags |= JS_NODE_SHORTHAND_DEF;
            list_add(&l, sp);
        } else {
            js_node *e = parse_assign(p, 0);
            if (!e) return 0;
            if (e->flags & JS_NODE_SHORTHAND_DEF)
                n->flags |= JS_NODE_SHORTHAND_DEF;
            list_add(&l, e);
        }
        if (p->lx.tok == TK_COMMA) {
            trailing_comma = 1;
            if (!advance(p)) return 0;
            continue;
        }
        break;
    }
    if (!expect(p, TK_RBRACKET, "] to close the array literal"))
        return 0;
    if (trailing_comma)
        n->flags |= JS_NODE_COMPUTED; /* reuse flag bit: trailing comma after last elem */
    n->a = l.head;
    n->num = (double)l.n;
    return n;
}

static js_node *parse_template(parser *p, int tagged)
{
    js_node *n;
    nodelist parts;

    if (!enter(p)) return 0;
    n = nd(p, N_TPL);
    if (!n) { leave(p); return 0; }
    memset(&parts, 0, sizeof(parts));

    for (;;) {
        js_node *chunk;
        int tok = p->lx.tok;

        if (tok != TK_TPL_FULL && tok != TK_TPL_HEAD &&
            tok != TK_TPL_MID && tok != TK_TPL_TAIL) {
            leave(p);
            return perr(p, "expected template literal");
        }
        if (!tagged && p->lx.tpl_err) {
            leave(p);
            return perr(p, "invalid escape sequence in template literal");
        }
        chunk = nd(p, p->lx.tpl_err ? N_EMPTY : N_STR);
        if (!chunk) { leave(p); return 0; }
        chunk->str = p->lx.text;
        chunk->str2 = p->lx.raw;
        list_add(&parts, chunk);

        if (tok == TK_TPL_FULL || tok == TK_TPL_TAIL) {
            if (!advance(p)) { leave(p); return 0; }
            break;
        }
        if (!advance(p)) { leave(p); return 0; }
        {
            js_node *e = parse_expr(p, 0);
            if (!e || !reject_bad_expr(p, e)) { leave(p); return 0; }
            list_add(&parts, e);
        }
        if (p->lx.tok != TK_RBRACE) {
            leave(p);
            return perr(p, "expected } to close template interpolation");
        }
        if (js_lex_template_cont(&p->lx) != JS_OK) {
            p->err = 1;
            leave(p);
            return 0;
        }
    }
    n->a = parts.head;
    n->num = (double)parts.n;
    leave(p);
    return n;
}

static js_node *parse_arrow_params_and_body(parser *p, js_node *items,
                                            int trailing_comma, int is_async)
{
    js_node *f, *q, *next, *ret;
    nodelist params, body;
    nodelist saved_vars = p->vars, saved_funcs = p->funcs, saved_lex = p->lex;
    int saved_iter = p->iter_depth, saved_switch = p->switch_depth;
    int saved_async = p->in_async, saved_saw_await = p->has_await;
    int nparams = 0, seen_default = 0, has_complex = 0;

    memset(&params, 0, sizeof(params));
    memset(&body, 0, sizeof(body));
    if (p->lx.nl_before)
        return perr(p, "line break before =>");

    for (q = items; q; q = next) {
        js_node *pat;
        next = q->next;
        if (q->type == N_SPREAD) {
            js_node *r;
            if (next || trailing_comma)
                return perr(p, "rest parameter must be last formal parameter");
            if (q->a->type == N_ASSIGN)
                return perr(p, "rest parameter may not have a default initializer");
            pat = expr_to_pattern(p, q->a, 1, 0);
            if (!pat) return 0;
            r = nd(p, N_REST);
            if (!r) return 0;
            r->a = pat;
            has_complex = 1;
            list_add(&params, r);
        } else {
            pat = expr_to_pattern(p, q, 1, 0);
            if (!pat) return 0;
            if (pat->type != N_IDENT) has_complex = 1;
            if (pat->type == N_DEFAULT) seen_default = 1;
            else if (!seen_default) nparams++;
            list_add(&params, pat);
        }
    }

    f = nd(p, N_FUNC);
    if (!f) return 0;
    f->flags |= JS_NODE_ARROW;
    if (is_async) f->flags |= JS_NODE_ASYNC;
    if (has_complex) f->flags |= JS_NODE_HAS_COMPLEX;
    f->a = params.head;
    f->num = nparams;
    if (!advance(p))                         /* past => */
        return 0;

    memset(&p->vars, 0, sizeof(p->vars));
    memset(&p->funcs, 0, sizeof(p->funcs));
    memset(&p->lex, 0, sizeof(p->lex));
    p->iter_depth = 0;
    p->switch_depth = 0;
    p->in_async = is_async;
    p->has_await = 0;
    if (p->lx.tok == TK_LBRACE) {
        if (!advance(p))
            goto fail;
        while (p->lx.tok != TK_RBRACE && p->lx.tok != TK_EOF) {
            js_node *st = parse_stmt(p);
            if (!st)
                goto fail;
            list_add(&body, st);
        }
        if (!expect(p, TK_RBRACE, "} to close the arrow body"))
            goto fail;
    } else {
        ret = nd(p, N_RETURN);
        if (!ret)
            goto fail;
        ret->a = parse_assign(p, 0);
        if (!ret->a || !reject_bad_expr(p, ret->a))
            goto fail;
        list_add(&body, ret);
    }
    f->b = body.head;
    f->c = p->vars.head;
    f->d = p->funcs.head;
    f->e = p->lex.head;
    if (is_async && p->has_await) {
        mark_await(f->b, 0);
        f->flags |= JS_NODE_HAS_AWAIT;
    }
    p->vars = saved_vars;
    p->funcs = saved_funcs;
    p->lex = saved_lex;
    p->iter_depth = saved_iter;
    p->switch_depth = saved_switch;
    p->in_async = saved_async;
    p->has_await = saved_saw_await;
    return f;

fail:
    p->vars = saved_vars;
    p->funcs = saved_funcs;
    p->lex = saved_lex;
    p->iter_depth = saved_iter;
    p->switch_depth = saved_switch;
    p->in_async = saved_async;
    p->has_await = saved_saw_await;
    return 0;
}

static js_node *parse_paren(parser *p, int is_async)
{
    nodelist items;
    int trailing_comma = 0;

    memset(&items, 0, sizeof(items));
    if (!advance(p)) return 0;               /* past '(' */
    while (p->lx.tok != TK_RPAREN) {
        js_node *it;
        trailing_comma = 0;
        if (p->lx.tok == TK_DOTDOTDOT) {
            js_node *sp;
            if (!advance(p)) return 0;
            sp = nd(p, N_SPREAD);
            if (!sp) return 0;
            sp->a = parse_assign(p, 0);
            if (!sp->a) return 0;
            it = sp;
        } else {
            it = parse_assign(p, 0);
            if (!it) return 0;
        }
        list_add(&items, it);
        if (p->lx.tok == TK_COMMA) {
            trailing_comma = 1;
            if (!advance(p)) return 0;
            continue;
        }
        break;
    }
    if (!expect(p, TK_RPAREN, ") to close the group"))
        return 0;
    if (p->lx.tok == TK_ARROW) {
        if (p->lx.nl_before)
            return perr(p, "line break before =>");
        return parse_arrow_params_and_body(p, items.head, trailing_comma, is_async);
    }
    if (is_async) {
        js_node *id = nd(p, N_IDENT), *c, *q;
        if (!id) return 0;
        id->str = js_str_intern(p->ctx, "async", 5);
        if (!id->str) { p->err = 1; return 0; }
        for (q = items.head; q; q = q->next)
            if (!reject_bad_expr(p, q->type == N_SPREAD ? q->a : q))
                return 0;
        c = nd(p, N_CALL);
        if (!c) return 0;
        c->a = id;
        c->b = items.head;
        c->num = items.n;
        return c;
    }
    if (items.n == 0)
        return perr(p, "empty parentheses are only valid in an arrow function");
    if (trailing_comma)
        return perr(p, "unexpected trailing comma in parenthesized expression");
    {
        js_node *q = items.head, *acc;
        if (q->type == N_SPREAD)
            return perr(p, "unexpected ... in parenthesized expression");
        acc = q;
        for (q = q->next; q; q = q->next) {
            js_node *seq;
            if (q->type == N_SPREAD)
                return perr(p, "unexpected ... in parenthesized expression");
            if (!reject_bad_expr(p, acc) || !reject_bad_expr(p, q))
                return 0;
            seq = nd(p, N_SEQ);
            if (!seq) return 0;
            seq->a = acc;
            seq->b = q;
            acc = seq;
        }
        acc->next = 0;
        acc->flags |= JS_NODE_PAREN;
        return acc;
    }
}

static js_node *parse_primary(parser *p)
{
    js_node *n;

    if (p->err) return 0;
    switch (p->lx.tok) {
    case TK_THIS:
        n = nd(p, N_THIS);
        return (n && advance(p)) ? n : 0;
    case TK_SUPER:
        n = nd(p, N_SUPER);
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
    case TK_TPL_FULL:
    case TK_TPL_HEAD:
        return parse_template(p, 0);
    case TK_REGEX:
        n = nd(p, N_REGEX);
        if (!n) return 0;
        n->str = p->lx.text;
        n->str2 = p->lx.flags;
        return advance(p) ? n : 0;
    case TK_IDENT:
        if (is_kw_ident(p, "async", 5)) {
            lexmark save;
            lx_mark(&p->lx, &save);
            if (!advance(p)) return 0;
            if (!p->lx.nl_before && p->lx.tok == TK_FUNCTION)
                return parse_function(p, 0, 1);
            if (!p->lx.nl_before && p->lx.tok == TK_LPAREN)
                return parse_paren(p, 1);
            if (!p->lx.nl_before && p->lx.tok == TK_IDENT) {
                js_string *pname = p->lx.text;
                if (!advance(p)) return 0;
                if (!p->lx.nl_before && p->lx.tok == TK_ARROW) {
                    js_node *param = nd(p, N_IDENT);
                    if (!param) return 0;
                    param->str = pname;
                    return parse_arrow_params_and_body(p, param, 0, 1);
                }
            }
            lx_reset(&p->lx, &save);
        }
        n = nd(p, N_IDENT);
        if (!n) return 0;
        n->str = p->lx.text;
        return advance(p) ? n : 0;
    case TK_FUNCTION:
        return parse_function(p, 0, 0);
    case TK_CLASS:
        return parse_class(p, 0);
    case TK_LBRACKET:
        return parse_array(p);
    case TK_LBRACE:
        return parse_object(p);
    case TK_LPAREN:
        return parse_paren(p, 0);
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
            if (base->type == N_SUPER && !p->in_method)
                return perr(p, "super property access outside method");
            if (!advance(p)) return 0;
            if (!is_name_token(p->lx.tok))
                return perr(p, "expected a property name after .");
            m = nd(p, N_MEMBER);
            if (!m) return 0;
            m->a = base;
            m->str = token_name_str(p);
            if (!m->str || !advance(p)) return 0;
            base = m;
            continue;
        }
        if (p->lx.tok == TK_LBRACKET) {
            js_node *m;
            if (base->type == N_SUPER && !p->in_method)
                return perr(p, "super property access outside method");
            if (!advance(p)) return 0;
            m = nd(p, N_INDEX);
            if (!m) return 0;
            m->a = base;
            m->b = parse_expr(p, 0);
            if (!m->b || !reject_bad_expr(p, m->b) || !expect(p, TK_RBRACKET, "]"))
                return 0;
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
    if (!callee || !reject_bad_expr(p, callee)) { leave(p); return 0; }
    if (callee->type == N_SUPER) {
        leave(p);
        return perr(p, "super cannot be called with new");
    }
    callee = parse_member_tail(p, callee);
    if (!callee) { leave(p); return 0; }
    if (p->lx.tok == TK_OPTCHAIN) {
        leave(p);
        return perr(p, "optional chaining cannot be used inside new");
    }
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
    int has_opt = 0;

    if (!enter(p)) return 0;
    e = (p->lx.tok == TK_NEW) ? parse_new(p) : parse_primary(p);
    if (!e) { leave(p); return 0; }
    for (;;) {
        if (p->err) { leave(p); return 0; }
        if (p->lx.tok == TK_DOT || p->lx.tok == TK_LBRACKET) {
            js_node *b;
            if (!reject_bad_expr(p, e)) { leave(p); return 0; }
            b = parse_member_tail(p, e);
            if (!b) { leave(p); return 0; }
            if (b == e) break;
            e = b;
            continue;
        }
        if (p->lx.tok == TK_OPTCHAIN) {
            if (!reject_bad_expr(p, e)) { leave(p); return 0; }
            if (e->type == N_SUPER) {
                leave(p);
                return perr(p, "super cannot be used with optional chaining");
            }
            has_opt = 1;
            if (!advance(p)) { leave(p); return 0; }
            if (p->lx.tok == TK_LBRACKET) {
                js_node *m = nd(p, N_INDEX);
                if (!m || !advance(p)) { leave(p); return 0; }
                m->flags |= JS_NODE_OPT;
                m->a = e;
                m->b = parse_expr(p, 0);
                if (!m->b || !reject_bad_expr(p, m->b) ||
                    !expect(p, TK_RBRACKET, "]")) {
                    leave(p); return 0;
                }
                e = m;
                continue;
            }
            if (p->lx.tok == TK_LPAREN) {
                int cnt = 0;
                js_node *args = parse_args(p, &cnt);
                js_node *c;
                if (!args) { leave(p); return 0; }
                c = nd(p, N_CALL);
                if (!c) { leave(p); return 0; }
                c->flags |= JS_NODE_OPT;
                c->a = e;
                c->b = (args == (js_node *)1) ? 0 : args;
                c->num = cnt;
                e = c;
                continue;
            }
            if (is_name_token(p->lx.tok)) {
                js_node *m = nd(p, N_MEMBER);
                if (!m) { leave(p); return 0; }
                m->flags |= JS_NODE_OPT;
                m->a = e;
                m->str = token_name_str(p);
                if (!m->str || !advance(p)) { leave(p); return 0; }
                e = m;
                continue;
            }
            leave(p);
            return perr(p, "expected property name, [ or ( after ?.");
        }
        if (p->lx.tok == TK_LPAREN) {
            int cnt = 0;
            js_node *args, *c;
            if (!reject_bad_expr(p, e)) { leave(p); return 0; }
            if (e->type == N_SUPER && !p->in_derived_ctor) {
                leave(p);
                return perr(p, "super() call only allowed in derived constructor");
            }
            args = parse_args(p, &cnt);
            if (!args) { leave(p); return 0; }
            c = nd(p, N_CALL);
            if (!c) { leave(p); return 0; }
            c->a = e;
            c->b = (args == (js_node *)1) ? 0 : args;
            c->num = cnt;
            e = c;
            continue;
        }
        if (p->lx.tok == TK_TPL_FULL || p->lx.tok == TK_TPL_HEAD) {
            js_node *tpl, *c;
            if (!reject_bad_expr(p, e)) { leave(p); return 0; }
            if (has_opt) {
                leave(p);
                return perr(p, "tagged template cannot follow optional chaining");
            }
            if (e->type == N_SUPER) {
                leave(p);
                return perr(p, "super cannot be a template tag");
            }
            tpl = parse_template(p, 1);
            if (!tpl) { leave(p); return 0; }
            c = nd(p, N_CALL);
            if (!c) { leave(p); return 0; }
            c->flags |= JS_NODE_TAGGED;
            c->a = e;
            c->b = tpl;
            e = c;
            continue;
        }
        break;
    }
    if (e->type == N_SUPER) {
        leave(p);
        return perr(p, "'super' keyword unexpected here");
    }
    if (has_opt) {
        js_node *opt = nd(p, N_OPTCHAIN);
        if (!opt) { leave(p); return 0; }
        opt->a = e;
        e = opt;
    }
    leave(p);
    return e;
}

static int is_target(js_node *n)
{
    return n && !(n->flags & JS_NODE_OPT) && n->type != N_OPTCHAIN &&
           (n->type == N_IDENT || n->type == N_MEMBER || n->type == N_INDEX);
}

static js_node *parse_unary(parser *p)
{
    int t = p->lx.tok;
    js_node *n;

    if (p->err) return 0;
    if (p->in_async && is_kw_ident(p, "await", 5)) {
        if (!enter(p)) return 0;
        n = nd(p, N_AWAIT);
        if (!n || !advance(p)) { leave(p); return 0; }
        n->a = parse_unary(p);
        leave(p);
        if (!n->a || !reject_bad_expr(p, n->a)) return 0;
        p->has_await = 1;
        return n;
    }
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
        return (n->a && reject_bad_expr(p, n->a)) ? n : 0;
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
    case TK_POW: return 11;
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
        if (!reject_bad_expr(p, lhs)) { leave(p); return 0; }
        if (op == TK_POW &&
            (lhs->type == N_UNARY || lhs->type == N_AWAIT) &&
            !(lhs->flags & JS_NODE_PAREN)) {
            leave(p);
            return perr(p, "unary operator cannot appear immediately before **");
        }
        if (!advance(p)) { leave(p); return 0; }
        rhs = parse_bin(p, op == TK_POW ? prec : prec + 1, no_in);
        if (!rhs || !reject_bad_expr(p, rhs)) { leave(p); return 0; }
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

static js_node *parse_logical(parser *p, int no_in)
{
    js_node *lhs;

    if (!enter(p)) return 0;
    lhs = parse_bin(p, 3, no_in);
    if (!lhs) { leave(p); return 0; }
    if (p->lx.tok == TK_QQ) {
        while (p->lx.tok == TK_QQ) {
            js_node *rhs, *b;
            if (!reject_bad_expr(p, lhs) || !advance(p)) { leave(p); return 0; }
            rhs = parse_bin(p, 3, no_in);
            if (!rhs || !reject_bad_expr(p, rhs)) { leave(p); return 0; }
            b = nd(p, N_LOGICAL);
            if (!b) { leave(p); return 0; }
            b->op = TK_QQ;
            b->a = lhs;
            b->b = rhs;
            lhs = b;
        }
        if (p->lx.tok == TK_ANDAND || p->lx.tok == TK_OROR) {
            leave(p);
            return perr(p, "cannot mix ?? with && or || without parentheses");
        }
    } else if (p->lx.tok == TK_ANDAND || p->lx.tok == TK_OROR) {
        while (p->lx.tok == TK_OROR || p->lx.tok == TK_ANDAND) {
            int op = p->lx.tok;
            js_node *rhs, *b;
            if (!reject_bad_expr(p, lhs) || !advance(p)) { leave(p); return 0; }
            rhs = parse_bin(p, op == TK_OROR ? 2 : 3, no_in);
            if (!rhs || !reject_bad_expr(p, rhs)) { leave(p); return 0; }
            b = nd(p, N_LOGICAL);
            if (!b) { leave(p); return 0; }
            b->op = (uint8_t)op;
            b->a = lhs;
            b->b = rhs;
            lhs = b;
        }
        if (p->lx.tok == TK_QQ) {
            leave(p);
            return perr(p, "cannot mix ?? with && or || without parentheses");
        }
    }
    leave(p);
    return lhs;
}

static js_node *parse_cond(parser *p, int no_in)
{
    js_node *t = parse_logical(p, no_in);

    if (!t) return 0;
    if (p->lx.tok == TK_QUESTION) {
        js_node *n;
        if (!reject_bad_expr(p, t)) return 0;
        if (!enter(p)) return 0;
        n = nd(p, N_COND);
        if (!n) { leave(p); return 0; }
        n->a = t;
        if (!advance(p)) { leave(p); return 0; }
        n->b = parse_assign(p, 0);
        if (!n->b || !reject_bad_expr(p, n->b) ||
            !expect(p, TK_COLON, ": in the conditional expression")) {
            leave(p); return 0;
        }
        n->c = parse_assign(p, no_in);
        leave(p);
        return (n->c && reject_bad_expr(p, n->c)) ? n : 0;
    }
    return t;
}

static int is_assign_tok(int t)
{
    return t == TK_ASSIGN || (t >= TK_ADD_A && t <= TK_QQ_A);
}

static js_node *expr_to_pattern(parser *p, js_node *e, int is_decl, int allow_paren)
{
    if (!e || !enter(p))
        return 0;
    e->flags &= ~JS_NODE_SHORTHAND_DEF;
    if (!allow_paren && (e->flags & JS_NODE_PAREN) &&
        (e->type == N_ARRAY || e->type == N_OBJECT)) {
        leave(p);
        return perr(p, "invalid parenthesized destructuring target");
    }
    switch (e->type) {
    case N_IDENT:
        leave(p);
        return e;
    case N_MEMBER:
    case N_INDEX:
        leave(p);
        if (is_decl || (e->flags & JS_NODE_OPT))
            return perr(p, "invalid binding pattern");
        return e;
    case N_ASSIGN: {
        js_node *d;
        if (e->op != TK_ASSIGN) {
            leave(p);
            return perr(p, "invalid default pattern");
        }
        d = nd(p, N_DEFAULT);
        if (!d) { leave(p); return 0; }
        d->a = expr_to_pattern(p, e->a, is_decl, 0);
        d->b = e->b;
        leave(p);
        return (d->a && reject_bad_expr(p, d->b)) ? d : 0;
    }
    case N_ARRAY: {
        nodelist elems;
        js_node *q, *next;
        int trailing_comma = (e->flags & JS_NODE_COMPUTED) != 0;
        memset(&elems, 0, sizeof(elems));
        for (q = e->a; q; q = next) {
            next = q->next;
            if (q->type == N_EMPTY) {
                list_add(&elems, q);
            } else if (q->type == N_SPREAD) {
                js_node *r;
                if (next || trailing_comma) {
                    leave(p);
                    return perr(p, "rest element must be last in array pattern");
                }
                if (q->a->type == N_ASSIGN) {
                    leave(p);
                    return perr(p, "rest element may not have a default initializer");
                }
                r = nd(p, N_REST);
                if (!r) { leave(p); return 0; }
                r->a = expr_to_pattern(p, q->a, is_decl, 0);
                if (!r->a) { leave(p); return 0; }
                list_add(&elems, r);
            } else {
                js_node *pe = expr_to_pattern(p, q, is_decl, 0);
                if (!pe) { leave(p); return 0; }
                list_add(&elems, pe);
            }
        }
        e->type = N_ARR_PAT;
        e->a = elems.head;
        e->flags &= ~JS_NODE_COMPUTED;
        leave(p);
        return e;
    }
    case N_OBJECT: {
        nodelist props;
        js_node *q, *next;
        memset(&props, 0, sizeof(props));
        for (q = e->a; q; q = next) {
            next = q->next;
            if (q->op == 1 || q->op == 2 ||
                (q->op == 0 && q->a && q->a->type == N_FUNC && q->a->op == 1)) {
                leave(p);
                return perr(p, "method/accessor not allowed in object pattern");
            }
            if (q->op == 3) {
                js_node *r;
                if (next) {
                    leave(p);
                    return perr(p, "rest element must be last in object pattern");
                }
                if (q->a->type == N_ASSIGN) {
                    leave(p);
                    return perr(p, "rest element may not have a default initializer");
                }
                r = nd(p, N_REST);
                if (!r) { leave(p); return 0; }
                r->a = expr_to_pattern(p, q->a, is_decl, 0);
                if (!r->a || (r->a->type != N_IDENT &&
                              r->a->type != N_MEMBER &&
                              r->a->type != N_INDEX)) {
                    leave(p);
                    return perr(p, "object rest target must be a simple identifier");
                }
                list_add(&props, r);
            } else if (q->op == 4) {
                js_node *d = nd(p, N_DEFAULT), *id = nd(p, N_IDENT);
                if (!d || !id) { leave(p); return 0; }
                id->str = q->str;
                d->a = id;
                d->b = q->b;
                q->op = 0;
                q->a = d;
                q->b = 0;
                list_add(&props, q);
            } else {
                q->a = expr_to_pattern(p, q->a, is_decl, 0);
                if (!q->a) { leave(p); return 0; }
                list_add(&props, q);
            }
        }
        e->type = N_OBJ_PAT;
        e->a = props.head;
        leave(p);
        return e;
    }
    default:
        leave(p);
        return perr(p, "invalid destructuring target");
    }
}

static js_node *parse_assign(parser *p, int no_in)
{
    js_node *lhs;

    if (!enter(p)) return 0;
    lhs = parse_cond(p, no_in);
    if (!lhs) { leave(p); return 0; }
    if (p->lx.tok == TK_ARROW) {
        if (lhs->type != N_IDENT || (lhs->flags & JS_NODE_PAREN)) {
            leave(p);
            return perr(p, "malformed arrow function parameter list");
        }
        lhs = parse_arrow_params_and_body(p, lhs, 0, 0);
        leave(p);
        return lhs;
    }
    if (is_assign_tok(p->lx.tok)) {
        js_node *n;
        int op = p->lx.tok;
        if (op == TK_ASSIGN && (lhs->type == N_ARRAY || lhs->type == N_OBJECT)) {
            lhs = expr_to_pattern(p, lhs, 0, 0);
            if (!lhs) { leave(p); return 0; }
        } else if (!is_target(lhs)) {
            leave(p);
            return perr(p, "invalid assignment target");
        }
        n = nd(p, N_ASSIGN);
        if (!n) { leave(p); return 0; }
        n->op = (uint8_t)op;
        n->a = lhs;
        if (!advance(p)) { leave(p); return 0; }
        n->b = parse_assign(p, no_in);
        leave(p);
        return (n->b && reject_bad_expr(p, n->b)) ? n : 0;
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
        if (!reject_bad_expr(p, e)) return 0;
        if (!enter(p)) return 0;
        n = nd(p, N_SEQ);
        if (!n) { leave(p); return 0; }
        n->a = e;
        if (!advance(p)) { leave(p); return 0; }
        n->b = parse_assign(p, no_in);
        leave(p);
        if (!n->b || !reject_bad_expr(p, n->b)) return 0;
        e = n;
    }
    return e;
}

static js_node *parse_pattern(parser *p)
{
    js_node *e;

    if (p->lx.tok == TK_IDENT) {
        e = nd(p, N_IDENT);
        if (!e) return 0;
        e->str = p->lx.text;
        return advance(p) ? e : 0;
    }
    if (p->lx.tok == TK_LBRACKET) {
        e = parse_array(p);
        return e ? expr_to_pattern(p, e, 1, 0) : 0;
    }
    if (p->lx.tok == TK_LBRACE) {
        e = parse_object(p);
        return e ? expr_to_pattern(p, e, 1, 0) : 0;
    }
    return perr(p, "expected a variable name or destructuring pattern");
}

/* ------------------------------------------------------------------ */
/* Functions and Classes                                               */
/* ------------------------------------------------------------------ */

/* Parses `( params ) { body }` into an already-allocated N_FUNC node. */
static int parse_func_rest(parser *p, js_node *f, int is_async,
                           int is_method, int is_derived_ctor)
{
    nodelist params, body;
    nodelist saved_vars = p->vars, saved_funcs = p->funcs, saved_lex = p->lex;
    int saved_iter = p->iter_depth, saved_switch = p->switch_depth;
    int saved_async = p->in_async, saved_saw_await = p->has_await;
    int saved_method = p->in_method, saved_dctor = p->in_derived_ctor;
    int nparams = 0, seen_default = 0, seen_rest = 0, has_complex = 0;

    if (!enter(p)) return 0;
    memset(&params, 0, sizeof(params));
    memset(&body, 0, sizeof(body));
    memset(&p->vars, 0, sizeof(p->vars));
    memset(&p->funcs, 0, sizeof(p->funcs));
    memset(&p->lex, 0, sizeof(p->lex));
    p->iter_depth = 0;
    p->switch_depth = 0;
    p->in_async = 0;
    p->has_await = 0;
    p->in_method = is_method;
    p->in_derived_ctor = is_derived_ctor;

    if (!expect(p, TK_LPAREN, "( after the function name")) goto fail;
    while (p->lx.tok != TK_RPAREN) {
        js_node *pat;
        if (seen_rest) {
            perr(p, "rest parameter must be last formal parameter");
            goto fail;
        }
        if (p->lx.tok == TK_DOTDOTDOT) {
            js_node *r;
            seen_rest = 1;
            has_complex = 1;
            if (!advance(p)) goto fail;
            pat = parse_pattern(p);
            if (!pat) goto fail;
            if (p->lx.tok == TK_ASSIGN) {
                perr(p, "rest parameter may not have a default initializer");
                goto fail;
            }
            if (p->lx.tok == TK_COMMA) {
                perr(p, "rest parameter must be last formal parameter");
                goto fail;
            }
            r = nd(p, N_REST);
            if (!r) goto fail;
            r->a = pat;
            list_add(&params, r);
            break;
        }
        pat = parse_pattern(p);
        if (!pat) goto fail;
        if (pat->type != N_IDENT)
            has_complex = 1;
        if (p->lx.tok == TK_ASSIGN) {
            js_node *d, *def;
            seen_default = 1;
            has_complex = 1;
            if (!advance(p)) goto fail;
            def = parse_assign(p, 0);
            if (!def || !reject_bad_expr(p, def)) goto fail;
            d = nd(p, N_DEFAULT);
            if (!d) goto fail;
            d->a = pat;
            d->b = def;
            pat = d;
        } else if (!seen_default) {
            nparams++;
        }
        list_add(&params, pat);
        if (p->lx.tok == TK_COMMA) { if (!advance(p)) goto fail; continue; }
        break;
    }
    if (!expect(p, TK_RPAREN, ") to close the parameter list")) goto fail;

    p->in_async = is_async;
    if (!expect(p, TK_LBRACE, "{ to open the function body")) goto fail;
    while (p->lx.tok != TK_RBRACE && p->lx.tok != TK_EOF) {
        js_node *st = parse_stmt(p);
        if (!st) goto fail;
        list_add(&body, st);
    }
    if (!expect(p, TK_RBRACE, "} to close the function body")) goto fail;

    if (is_async) f->flags |= JS_NODE_ASYNC;
    if (has_complex) f->flags |= JS_NODE_HAS_COMPLEX;
    f->a = params.head;
    f->b = body.head;
    f->c = p->vars.head;
    f->d = p->funcs.head;
    f->e = p->lex.head;
    f->num = nparams;
    if (is_async && p->has_await) {
        mark_await(f->b, 0);
        f->flags |= JS_NODE_HAS_AWAIT;
    }

    p->vars = saved_vars;
    p->funcs = saved_funcs;
    p->lex = saved_lex;
    p->iter_depth = saved_iter;
    p->switch_depth = saved_switch;
    p->in_async = saved_async;
    p->has_await = saved_saw_await;
    p->in_method = saved_method;
    p->in_derived_ctor = saved_dctor;
    leave(p);
    return 1;

fail:
    p->vars = saved_vars;
    p->funcs = saved_funcs;
    p->lex = saved_lex;
    p->iter_depth = saved_iter;
    p->switch_depth = saved_switch;
    p->in_async = saved_async;
    p->has_await = saved_saw_await;
    p->in_method = saved_method;
    p->in_derived_ctor = saved_dctor;
    leave(p);
    return 0;
}

static js_node *parse_function(parser *p, int is_decl, int is_async)
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
    return parse_func_rest(p, f, is_async, 0, 0) ? f : 0;
}

static js_node *parse_class(parser *p, int is_decl)
{
    js_node *cls;
    nodelist methods;
    int has_ctor = 0;

    if (!enter(p)) return 0;
    cls = nd(p, N_CLASS);
    if (!cls) { leave(p); return 0; }
    if (is_decl) cls->flags |= JS_NODE_CLASS_DECL;
    memset(&methods, 0, sizeof(methods));

    if (!advance(p)) { leave(p); return 0; }         /* past 'class' */
    if (p->lx.tok == TK_IDENT) {
        cls->str = p->lx.text;
        if (!advance(p)) { leave(p); return 0; }
        if (is_decl && !hoist_lex(p, cls->str, 0)) {
            leave(p); return 0;
        }
    } else if (is_decl) {
        leave(p);
        return perr(p, "a class declaration needs a name");
    }

    if (p->lx.tok == TK_EXTENDS) {
        if (!advance(p)) { leave(p); return 0; }
        cls->a = parse_lhs(p);
        if (!cls->a || !reject_bad_expr(p, cls->a)) { leave(p); return 0; }
    }

    if (!expect(p, TK_LBRACE, "{ to open the class body")) {
        leave(p); return 0;
    }
    while (p->lx.tok != TK_RBRACE && p->lx.tok != TK_EOF) {
        int is_static = 0, is_async = 0, kind = 0;
        js_node *comp_key = 0, *mfn, *pr;
        js_string *key = 0;

        if (p->lx.tok == TK_SEMI) {
            if (!advance(p)) { leave(p); return 0; }
            continue;
        }
        if (is_kw_ident(p, "static", 6)) {
            lexmark save;
            lx_mark(&p->lx, &save);
            if (!advance(p)) { leave(p); return 0; }
            if (p->lx.tok != TK_LPAREN)
                is_static = 1;
            else
                lx_reset(&p->lx, &save);
        }
        if (is_kw_ident(p, "async", 5)) {
            lexmark save;
            lx_mark(&p->lx, &save);
            if (!advance(p)) { leave(p); return 0; }
            if (!p->lx.nl_before && p->lx.tok != TK_LPAREN)
                is_async = 1;
            else
                lx_reset(&p->lx, &save);
        }
        if (!is_async && p->lx.tok == TK_IDENT && p->lx.text->len == 3 &&
            (memcmp(p->lx.text->data, "get", 3) == 0 ||
             memcmp(p->lx.text->data, "set", 3) == 0)) {
            int want = p->lx.text->data[0] == 'g' ? 1 : 2;
            lexmark save;
            lx_mark(&p->lx, &save);
            if (!advance(p)) { leave(p); return 0; }
            if (p->lx.tok != TK_LPAREN)
                kind = want;
            else
                lx_reset(&p->lx, &save);
        }
        if (p->lx.tok == TK_LBRACKET) {
            if (!advance(p)) { leave(p); return 0; }
            comp_key = parse_assign(p, 0);
            if (!comp_key || !reject_bad_expr(p, comp_key) ||
                !expect(p, TK_RBRACKET, "]")) {
                leave(p); return 0;
            }
        } else if (p->lx.tok == TK_STR) {
            key = p->lx.text;
            if (!advance(p)) { leave(p); return 0; }
        } else if (p->lx.tok == TK_NUM) {
            char buf[48];
            js_dtoa(p->lx.num, buf);
            key = js_str_intern(p->ctx, buf, strlen(buf));
            if (!key || !advance(p)) { leave(p); return 0; }
        } else if (is_name_token(p->lx.tok)) {
            key = token_name_str(p);
            if (!key || !advance(p)) { leave(p); return 0; }
        } else {
            leave(p);
            return perr(p, "expected a class method name");
        }

        if (!is_static && !comp_key && key &&
            key->len == 11 && memcmp(key->data, "constructor", 11) == 0) {
            if (kind || is_async) {
                leave(p);
                return perr(p, "class constructor may not be async or an accessor");
            }
            if (has_ctor) {
                leave(p);
                return perr(p, "duplicate class constructor");
            }
            has_ctor = 1;
            mfn = nd(p, N_FUNC);
            if (!mfn || !parse_func_rest(p, mfn, 0, 1, cls->a != 0)) {
                leave(p); return 0;
            }
            mfn->str = cls->str ? cls->str : key;
            mfn->op = cls->a ? 3 : 2;
            cls->b = mfn;
            continue;
        }

        mfn = nd(p, N_FUNC);
        if (!mfn || !parse_func_rest(p, mfn, is_async, 1, 0)) {
            leave(p); return 0;
        }
        mfn->str = key;
        mfn->op = 1;
        pr = nd(p, N_PROP);
        if (!pr) { leave(p); return 0; }
        pr->op = (uint8_t)kind;
        if (is_static) pr->flags |= JS_NODE_STATIC;
        pr->str = key;
        pr->b = comp_key;
        pr->a = mfn;
        list_add(&methods, pr);
    }
    if (!expect(p, TK_RBRACE, "} to close the class body")) {
        leave(p); return 0;
    }
    cls->c = methods.head;
    leave(p);
    return cls;
}

/* ------------------------------------------------------------------ */
/* Statements                                                          */
/* ------------------------------------------------------------------ */

static js_node *parse_var(parser *p, int no_in, int *count, int kind,
                          int allow_no_const_init)
{
    js_node *n = nd(p, N_VAR);
    nodelist l;

    if (!n) return 0;
    n->op = (uint8_t)kind;
    memset(&l, 0, sizeof(l));
    if (!advance(p)) return 0;               /* past var/let/const */
    for (;;) {
        js_node *d, *pat;
        pat = parse_pattern(p);
        if (!pat) return 0;
        if (!collect_pat_names(p, pat, kind)) return 0;
        d = nd(p, N_VARDECL);
        if (!d) return 0;
        if (pat->type == N_IDENT)
            d->str = pat->str;
        else
            d->b = pat;
        if (p->lx.tok == TK_ASSIGN) {
            if (!advance(p)) return 0;
            d->a = parse_assign(p, no_in);
            if (!d->a || !reject_bad_expr(p, d->a)) return 0;
        } else if (kind == 2 && !allow_no_const_init) {
            return perr(p, "missing initializer in const declaration");
        } else if (pat->type != N_IDENT && !allow_no_const_init) {
            return perr(p, "missing initializer in destructuring declaration");
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
    nodelist l, saved_lex = p->lex;

    if (!n) return 0;
    memset(&l, 0, sizeof(l));
    memset(&p->lex, 0, sizeof(p->lex));
    if (!advance(p)) { p->lex = saved_lex; return 0; } /* past '{' */
    while (p->lx.tok != TK_RBRACE && p->lx.tok != TK_EOF) {
        js_node *st = parse_stmt(p);
        if (!st) { p->lex = saved_lex; return 0; }
        list_add(&l, st);
    }
    if (!expect(p, TK_RBRACE, "} to close the block")) {
        p->lex = saved_lex;
        return 0;
    }
    n->a = l.head;
    n->b = p->lex.head;
    p->lex = saved_lex;
    return n;
}

static js_node *parse_for(parser *p)
{
    js_node *init = 0;
    nodelist saved_lex = p->lex, for_lex;
    int is_var = 0, kind = 0, ndecl = 0;

    memset(&p->lex, 0, sizeof(p->lex));
    if (!advance(p)) { p->lex = saved_lex; return 0; } /* past 'for' */
    if (!expect(p, TK_LPAREN, "( after for")) { p->lex = saved_lex; return 0; }

    if (p->lx.tok == TK_VAR || p->lx.tok == TK_CONST ||
        p->lx.tok == TK_LET) {
        kind = (p->lx.tok == TK_VAR) ? 0 : (p->lx.tok == TK_LET ? 1 : 2);
        init = parse_var(p, 1, &ndecl, kind, 1);
        if (!init) { p->lex = saved_lex; return 0; }
        is_var = 1;
    } else if (p->lx.tok != TK_SEMI) {
        init = parse_expr(p, 1);
        if (!init) { p->lex = saved_lex; return 0; }
    }

    for_lex = p->lex;
    p->lex = saved_lex;

    if (p->lx.tok == TK_IN || is_kw_ident(p, "of", 2)) {
        int is_of = (p->lx.tok != TK_IN);
        js_node *n = nd(p, is_of ? N_FOROF : N_FORIN);
        if (!n) return 0;
        if (is_var) {
            js_node *d = init->a;
            if (ndecl != 1 || d->a)
                return perr(p,
                    "for-in/of accepts a single uninitialized declaration");
            n->op = (uint8_t)kind;
            n->a = d;                        /* N_VARDECL (str or b=pattern) */
            n->flags = 1;
            n->e = for_lex.head;
        } else {
            js_node *pat = expr_to_pattern(p, init, 0, 0);
            if (!pat) return 0;
            n->op = 3;
            n->a = pat;
        }
        if (!advance(p)) return 0;
        n->b = is_of ? parse_assign(p, 0) : parse_expr(p, 0);
        if (!n->b || !reject_bad_expr(p, n->b)) return 0;
        if (!expect(p, TK_RPAREN, ") to close the for-in/of header")) return 0;
        p->iter_depth++;
        n->c = parse_stmt(p);
        p->iter_depth--;
        return n->c ? n : 0;
    }

    {
        js_node *n = nd(p, N_FOR);
        if (!n) return 0;
        if (is_var) {
            js_node *d;
            for (d = init->a; d; d = d->next) {
                if (kind == 2 && !d->a)
                    return perr(p, "missing initializer in const declaration");
                if (d->b && !d->a)
                    return perr(p, "missing initializer in destructuring declaration");
            }
            n->a = init;
            n->e = for_lex.head;
        } else if (init) {
            js_node *e;
            if (!reject_bad_expr(p, init)) return 0;
            e = nd(p, N_EXPRSTMT);
            if (!e) return 0;
            e->a = init;
            n->a = e;
        }
        if (!expect(p, TK_SEMI, "; in the for header")) return 0;
        if (p->lx.tok != TK_SEMI) {
            n->b = parse_expr(p, 0);
            if (!n->b || !reject_bad_expr(p, n->b)) return 0;
        }
        if (!expect(p, TK_SEMI, "; in the for header")) return 0;
        if (p->lx.tok != TK_RPAREN) {
            n->c = parse_expr(p, 0);
            if (!n->c || !reject_bad_expr(p, n->c)) return 0;
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
    nodelist cases, saved_lex = p->lex;
    int seen_default = 0;

    if (!n) return 0;
    memset(&cases, 0, sizeof(cases));
    if (!advance(p)) return 0;
    if (!expect(p, TK_LPAREN, "( after switch")) return 0;
    n->a = parse_expr(p, 0);
    if (!n->a || !reject_bad_expr(p, n->a)) return 0;
    if (!expect(p, TK_RPAREN, ") after the switch expression")) return 0;
    if (!expect(p, TK_LBRACE, "{ to open the switch body")) return 0;

    memset(&p->lex, 0, sizeof(p->lex));
    p->switch_depth++;
    while (p->lx.tok != TK_RBRACE && p->lx.tok != TK_EOF) {
        js_node *c = nd(p, N_CASE);
        nodelist body;
        if (!c) { p->switch_depth--; p->lex = saved_lex; return 0; }
        memset(&body, 0, sizeof(body));
        if (p->lx.tok == TK_CASE) {
            if (!advance(p)) { p->switch_depth--; p->lex = saved_lex; return 0; }
            c->a = parse_expr(p, 0);
            if (!c->a || !reject_bad_expr(p, c->a)) {
                p->switch_depth--; p->lex = saved_lex; return 0;
            }
        } else if (p->lx.tok == TK_DEFAULT) {
            if (seen_default) {
                p->switch_depth--;
                p->lex = saved_lex;
                return perr(p, "duplicate default clause");
            }
            seen_default = 1;
            if (!advance(p)) { p->switch_depth--; p->lex = saved_lex; return 0; }
        } else {
            p->switch_depth--;
            p->lex = saved_lex;
            return perr(p, "expected case or default");
        }
        if (!expect(p, TK_COLON, ": after the case label")) {
            p->switch_depth--; p->lex = saved_lex; return 0;
        }
        while (p->lx.tok != TK_CASE && p->lx.tok != TK_DEFAULT &&
               p->lx.tok != TK_RBRACE && p->lx.tok != TK_EOF) {
            js_node *st = parse_stmt(p);
            if (!st) { p->switch_depth--; p->lex = saved_lex; return 0; }
            list_add(&body, st);
        }
        c->b = body.head;
        list_add(&cases, c);
    }
    p->switch_depth--;
    n->c = p->lex.head;
    p->lex = saved_lex;
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
        if (p->lx.tok == TK_LPAREN) {
            js_node *pat;
            if (!advance(p)) return 0;
            pat = parse_pattern(p);
            if (!pat) return 0;
            if (pat->type == N_IDENT)
                n->str = pat->str;
            else
                n->d = pat;
            if (!expect(p, TK_RPAREN, ") after the catch parameter")) return 0;
        } else {
            n->flags |= 1;  /* optional catch binding */
        }
        if (p->lx.tok != TK_LBRACE)
            return perr(p, "expected { after catch");
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
    case TK_LET: {
        int kind = (p->lx.tok == TK_VAR) ? 0 : (p->lx.tok == TK_LET ? 1 : 2);
        n = parse_var(p, 0, 0, kind, 0);
        leave(p);
        return (n && semicolon(p)) ? n : 0;
    }

    case TK_FUNCTION: {
        js_node *f = parse_function(p, 1, 0);
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

    case TK_CLASS:
        n = parse_class(p, 1);
        leave(p);
        return n;

    case TK_IF:
        n = nd(p, N_IF);
        if (!n) { leave(p); return 0; }
        if (!advance(p) || !expect(p, TK_LPAREN, "( after if")) { leave(p); return 0; }
        n->a = parse_expr(p, 0);
        if (!n->a || !reject_bad_expr(p, n->a) ||
            !expect(p, TK_RPAREN, ") after the if condition")) {
            leave(p); return 0;
        }
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
        if (!n->b || !reject_bad_expr(p, n->b) ||
            !expect(p, TK_RPAREN, ") after the while condition")) {
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
        if (!n->a || !reject_bad_expr(p, n->a) ||
            !expect(p, TK_RPAREN, ") after the while condition")) {
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
            if (!n->a || !reject_bad_expr(p, n->a)) { leave(p); return 0; }
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
        if (!n->a || !reject_bad_expr(p, n->a)) return 0;
        return semicolon(p) ? n : 0;

    case TK_WITH:
        n = nd(p, N_WITH);
        if (!n) { leave(p); return 0; }
        if (!advance(p) || !expect(p, TK_LPAREN, "( after with")) { leave(p); return 0; }
        n->a = parse_expr(p, 0);
        if (!n->a || !reject_bad_expr(p, n->a) ||
            !expect(p, TK_RPAREN, ") after the with expression")) {
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

    /* async function declaration */
    if (is_kw_ident(p, "async", 5)) {
        lexmark save;
        lx_mark(&p->lx, &save);
        if (!advance(p)) { leave(p); return 0; }
        if (!p->lx.nl_before && p->lx.tok == TK_FUNCTION) {
            js_node *f = parse_function(p, 1, 1);
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
        lx_reset(&p->lx, &save);
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
    if (p->lx.tok == TK_LBRACE || p->lx.tok == TK_FUNCTION ||
        p->lx.tok == TK_CLASS) {
        leave(p);
        return perr(p,
            "an expression statement may not start with {, function, or class");
    }
    n = nd(p, N_EXPRSTMT);
    if (!n) { leave(p); return 0; }
    n->a = parse_expr(p, 0);
    leave(p);
    if (!n->a || !reject_bad_expr(p, n->a)) return 0;
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
    prog->b = p.lex.head;
    prog->c = p.vars.head;
    prog->d = p.funcs.head;
    return prog;
}
