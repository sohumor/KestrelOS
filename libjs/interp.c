/* libjs: the tree-walking evaluator + resumable async/await trampoline.
 *
 * Statements return a completion type (normal / break / continue / return
 * / throw) plus a value and, for break and continue, a target label.
 * Expressions return JS_OK or JS_THROW with the exception parked in the
 * context. Scope chains are lists of environments whose variable objects
 * are ordinary objects, which makes closures, block scoping (`let`/`const`
 * with TDZ), `with`, and the global object all share one mechanism.
 *
 * `async` functions run via a heap-allocated frame stack (`js_async_task`)
 * driven iteratively by `async_run()`, suspending cleanly at `await` onto the
 * Promise reaction queue (`js_run_jobs`) without pinning C stack frames.
 */

#define JS_INTERNAL
#include "js.h"

#include <string.h>

enum { CT_NORMAL = 0, CT_BREAK, CT_CONTINUE, CT_RETURN, CT_THROW };
enum { BIND_ASSIGN = 0, BIND_VAR = 1, BIND_LEX_INIT = 2 };

#define JS_CTOR_ARROW 3

static int exec(js_ctx *ctx, js_node *n, js_env *env,
                js_value *cv, js_string **lbl);
static int eval(js_ctx *ctx, js_node *n, js_env *env, js_value *out);
static int eval_class(js_ctx *ctx, js_node *n, js_env *env, js_value *out);

/* ------------------------------------------------------------------ */
/* Budgets                                                             */
/* ------------------------------------------------------------------ */

static int too_deep(js_ctx *ctx)
{
    if (ctx->depth >= ctx->cfg.max_depth) {
        js_throw_error(ctx, JS_ERR_RANGE,
                       "interpreter recursion limit (%d) exceeded",
                       ctx->cfg.max_depth);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Scope resolution and lexical environments                           */
/* ------------------------------------------------------------------ */

static js_object *resolve(js_ctx *ctx, js_env *env, js_string *name)
{
    js_env *e;

    for (e = env; e; e = e->parent) {
        if (e->is_with || !e->parent) {
            if (js_obj_has(ctx, e->vars, name))
                return e->vars;
        } else if (js_own_prop(e->vars, name)) {
            return e->vars;
        }
    }
    return 0;
}

static js_env *env_this_owner(js_env *env)
{
    js_env *e;

    for (e = env; e; e = e->parent) {
        if (!e->is_lex && !e->is_with)
            return e;
    }
    return env;
}

static int env_get_this(js_ctx *ctx, js_env *env, js_value *out)
{
    js_env *e = env_this_owner(env);

    if (e && e->this_tdz)
        return js_throw_error(ctx, JS_ERR_REFERENCE,
                              "must call super constructor before accessing 'this'");
    *out = e ? e->this_val : env->this_val;
    return JS_OK;
}

static js_env *env_home_owner(js_env *env)
{
    js_env *e;

    for (e = env; e; e = e->parent) {
        if (e->home_obj)
            return e;
    }
    return 0;
}

static js_env *env_derived_ctor(js_env *env)
{
    js_env *e;

    for (e = env; e; e = e->parent) {
        if (e->active_ctor)
            return e;
    }
    return 0;
}

static __attribute__((noinline)) js_env *create_lex_env(
    js_ctx *ctx, js_env *parent, js_node *lex_list, int allow_reinit)
{
    js_object *scope = js_obj_alloc(ctx, JS_CLASS_OBJECT, 0);
    js_env *benv;
    js_node *q;

    if (!scope)
        return 0;
    benv = js_env_new(ctx, parent, scope);
    if (!benv)
        return 0;
    benv->is_lex = 1;
    benv->this_val = parent ? parent->this_val : js_undefined();
    for (q = lex_list; q; q = q->next) {
        uint32_t flags = JS_P_ENUM | JS_P_TDZ |
                         (q->op ? (uint32_t)JS_P_CONST : (uint32_t)JS_P_WRITE);
        js_prop *p;
        if (!allow_reinit && js_own_prop(scope, q->str))
            continue;
        p = js_add_prop(ctx, scope, q->str, flags);
        if (!p)
            return 0;
        p->flags = flags;
        p->value = js_undefined();
    }
    return benv;
}

static __attribute__((noinline)) js_env *copy_lex_env(
    js_ctx *ctx, js_env *src_env)
{
    js_object *scope = js_obj_alloc(ctx, JS_CLASS_OBJECT, 0);
    js_env *nenv;
    uint32_t i;

    if (!scope)
        return 0;
    nenv = js_env_new(ctx, src_env->parent, scope);
    if (!nenv)
        return 0;
    nenv->is_lex = 1;
    nenv->this_val = src_env->this_val;
    for (i = 0; i < src_env->vars->nprops; i++) {
        js_prop *sp = &src_env->vars->props[i];
        js_prop *dp;
        if (sp->flags & JS_P_DEAD)
            continue;
        dp = js_add_prop(ctx, scope, sp->key, sp->flags);
        if (!dp)
            return 0;
        dp->flags = sp->flags;
        dp->value = sp->value;
    }
    return nenv;
}

static int init_lex_var(js_ctx *ctx, js_env *env, js_string *name, js_value val)
{
    js_env *e;

    for (e = env; e; e = e->parent) {
        js_prop *p;
        if (e->is_with)
            continue;
        p = js_own_prop(e->vars, name);
        if (p) {
            p->flags &= ~(uint32_t)JS_P_TDZ;
            p->value = val;
            return JS_OK;
        }
    }
    return js_obj_put(ctx, ctx->global, name, val);
}

static int check_scope_tdz(js_ctx *ctx, js_object *scope, js_string *name)
{
    js_prop *p = js_own_prop(scope, name);

    if (p && (p->flags & JS_P_TDZ))
        return js_throw_error(ctx, JS_ERR_REFERENCE,
                              "cannot access '%s' before initialization",
                              name->data);
    return JS_OK;
}

/* ------------------------------------------------------------------ */
/* References                                                          */
/* ------------------------------------------------------------------ */

enum { REF_NONE = 0, REF_VAR, REF_PROP, REF_SUPER };

typedef struct {
    int        kind;
    int        is_sym;
    js_object *scope;     /* REF_VAR: scope obj; REF_SUPER: super proto */
    js_value   base;      /* REF_PROP / REF_SUPER receiver `this` */
    js_string *key;
} jsref;

static int eval_ref(js_ctx *ctx, js_node *n, js_env *env, jsref *r)
{
    memset(r, 0, sizeof(*r));
    switch (n->type) {
    case N_IDENT:
        r->kind = REF_VAR;
        r->key = n->str;
        r->scope = resolve(ctx, env, n->str);
        return JS_OK;
    case N_MEMBER:
        if (n->a->type == N_SUPER) {
            js_env *he = env_home_owner(env);
            js_value tv;
            if (!he || !he->home_obj)
                return js_throw_error(ctx, JS_ERR_SYNTAX,
                                      "super property outside method");
            if (env_get_this(ctx, env, &tv) != JS_OK)
                return JS_THROW;
            r->kind = REF_SUPER;
            r->scope = he->home_obj->proto;
            r->base = tv;
            r->key = n->str;
            return JS_OK;
        }
        r->kind = REF_PROP;
        r->key = n->str;
        return eval(ctx, n->a, env, &r->base);
    case N_INDEX: {
        js_value k;
        if (n->a->type == N_SUPER) {
            js_env *he = env_home_owner(env);
            js_value tv;
            if (!he || !he->home_obj)
                return js_throw_error(ctx, JS_ERR_SYNTAX,
                                      "super property outside method");
            if (env_get_this(ctx, env, &tv) != JS_OK)
                return JS_THROW;
            if (eval(ctx, n->b, env, &k) != JS_OK)
                return JS_THROW;
            if (js_to_propkey(ctx, k, &r->key, &r->is_sym) != JS_OK)
                return JS_THROW;
            r->kind = REF_SUPER;
            r->scope = he->home_obj->proto;
            r->base = tv;
            return JS_OK;
        }
        if (eval(ctx, n->a, env, &r->base) != JS_OK)
            return JS_THROW;
        if (eval(ctx, n->b, env, &k) != JS_OK)
            return JS_THROW;
        if (js_to_propkey(ctx, k, &r->key, &r->is_sym) != JS_OK)
            return JS_THROW;
        r->kind = REF_PROP;
        return JS_OK;
    }
    default:
        return js_throw_error(ctx, JS_ERR_REFERENCE, "invalid reference");
    }
}

static int ref_get(js_ctx *ctx, jsref *r, js_value *out)
{
    *out = js_undefined();
    if (r->kind == REF_VAR) {
        if (!r->scope)
            return js_throw_error(ctx, JS_ERR_REFERENCE,
                                  "%s is not defined", r->key->data);
        if (check_scope_tdz(ctx, r->scope, r->key) != JS_OK)
            return JS_THROW;
        return js_obj_get(ctx, r->scope, r->key, js_object_value(r->scope), out);
    }
    if (r->kind == REF_SUPER) {
        if (!r->scope)
            return JS_OK;
        return js_obj_get_key(ctx, r->scope, r->key, r->is_sym, r->base, out);
    }
    return js_wrap_get_key(ctx, r->base, r->key, r->is_sym, out);
}

static int ref_put(js_ctx *ctx, jsref *r, js_value v)
{
    if (r->kind == REF_VAR) {
        js_object *o = r->scope ? r->scope : ctx->global;
        if (r->scope) {
            js_prop *p = js_own_prop(r->scope, r->key);
            if (p && (p->flags & JS_P_TDZ))
                return js_throw_error(ctx, JS_ERR_REFERENCE,
                                      "cannot access '%s' before initialization",
                                      r->key->data);
            if (p && (p->flags & JS_P_CONST))
                return js_throw_error(ctx, JS_ERR_TYPE,
                                      "assignment to constant variable '%s'",
                                      r->key->data);
        }
        return js_obj_put(ctx, o, r->key, v);
    }
    if (r->kind == REF_SUPER) {
        js_object *p;
        int guard = 0;
        for (p = r->scope; p && guard < 1000; p = p->proto, guard++) {
            js_prop *pr = js_own_prop_key(p, r->key, r->is_sym);
            if (pr) {
                if (pr->flags & JS_P_ACCESSOR) {
                    if (pr->setter.type != JS_OBJECT)
                        return JS_OK;
                    return js_call(ctx, pr->setter, r->base, 1, &v, 0);
                }
                if (!(pr->flags & JS_P_WRITE))
                    return JS_OK;
                break;
            }
        }
        if (r->base.type != JS_OBJECT)
            return JS_OK;
        return js_obj_put_key(ctx, r->base.u.obj, r->key, r->is_sym, v);
    }
    if (r->base.type == JS_UNDEFINED || r->base.type == JS_NULL)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "cannot set property '%s' of %s",
                              r->is_sym ? "Symbol" : r->key->data,
                              r->base.type == JS_NULL ? "null" : "undefined");
    if (r->base.type != JS_OBJECT)
        return JS_OK;                    /* writes to primitives are dropped */
    return js_obj_put_key(ctx, r->base.u.obj, r->key, r->is_sym, v);
}

/* ------------------------------------------------------------------ */
/* Iterator protocol and destructuring                                 */
/* ------------------------------------------------------------------ */

static js_string *index_key(js_ctx *ctx, unsigned long i)
{
    char buf[16], tmp[16];
    int m = 0, t = 0;

    if (i == 0) buf[m++] = '0';
    else {
        while (i) { tmp[t++] = (char)('0' + i % 10); i /= 10; }
        while (t) buf[m++] = tmp[--t];
    }
    return js_str_intern(ctx, buf, (unsigned long)m);
}

int js_iter_open(js_ctx *ctx, js_value iterable, js_iter *it)
{
    memset(it, 0, sizeof(*it));
    if (iterable.type == JS_UNDEFINED || iterable.type == JS_NULL)
        return js_throw_error(ctx, JS_ERR_TYPE, "value is not iterable");

    if (ctx->sym_iterator.type == JS_SYMBOL && ctx->sym_iterator_key) {
        js_value iter_fn;
        if (js_wrap_get_key(ctx, iterable, ctx->sym_iterator_key, 1,
                            &iter_fn) != JS_OK)
            return JS_THROW;
        if (iter_fn.type != JS_UNDEFINED) {
            js_value iter_obj, next_fn;
            if (!js_is_function(iter_fn))
                return js_throw_error(ctx, JS_ERR_TYPE,
                                      "@@iterator is not a function");
            if (js_call(ctx, iter_fn, iterable, 0, 0, &iter_obj) != JS_OK)
                return JS_THROW;
            if (iter_obj.type != JS_OBJECT)
                return js_throw_error(ctx, JS_ERR_TYPE,
                                      "iterator must be an object");
            if (js_obj_get(ctx, iter_obj.u.obj, ctx->s_next, iter_obj,
                           &next_fn) != JS_OK)
                return JS_THROW;
            if (!js_is_function(next_fn))
                return js_throw_error(ctx, JS_ERR_TYPE,
                                      "iterator.next is not a function");
            it->kind = 0;
            it->iter = iter_obj;
            it->next_fn = next_fn;
            return JS_OK;
        }
    }

    if (iterable.type == JS_OBJECT &&
        iterable.u.obj->cls != JS_CLASS_FUNCTION &&
        (iterable.u.obj->cls == JS_CLASS_ARGUMENTS ||
         js_obj_has(ctx, iterable.u.obj, ctx->s_length))) {
        js_value lv;
        if (js_obj_get(ctx, iterable.u.obj, ctx->s_length, iterable, &lv) != JS_OK)
            return JS_THROW;
        if (lv.type == JS_NUMBER && !js_isnan(lv.u.num) && lv.u.num >= 0) {
            uint32_t u = 0;
            if (js_to_uint32(ctx, lv, &u) != JS_OK)
                return JS_THROW;
            it->kind = 1;
            it->target = iterable;
            it->index = 0;
            it->length = u;
            return JS_OK;
        }
    }

    return js_throw_error(ctx, JS_ERR_TYPE, "value is not iterable");
}

int js_iter_next(js_ctx *ctx, js_iter *it, js_value *out, int *done)
{
    if (it->kind == 0) {
        js_value step, dv;
        if (js_call(ctx, it->next_fn, it->iter, 0, 0, &step) != JS_OK)
            return JS_THROW;
        if (step.type != JS_OBJECT)
            return js_throw_error(ctx, JS_ERR_TYPE,
                                  "iterator result is not an object");
        if (js_obj_get(ctx, step.u.obj, ctx->s_done, step, &dv) != JS_OK)
            return JS_THROW;
        *done = js_to_boolean(dv);
        if (*done) {
            *out = js_undefined();
            return JS_OK;
        }
        return js_obj_get(ctx, step.u.obj, ctx->s_value, step, out);
    }
    if (it->index >= it->length) {
        *done = 1;
        *out = js_undefined();
        return JS_OK;
    }
    *done = 0;
    {
        js_string *k = index_key(ctx, it->index++);
        if (!k)
            return JS_THROW;
        return js_obj_get(ctx, it->target.u.obj, k, it->target, out);
    }
}

static int spread_array_iter(js_ctx *ctx, js_object *arr, js_value iterable)
{
    js_iter it;
    int done = 0;

    if (js_iter_open(ctx, iterable, &it) != JS_OK)
        return JS_THROW;
    for (;;) {
        js_value item;
        if (js_step(ctx) != JS_OK)
            return JS_THROW;
        if (js_iter_next(ctx, &it, &item, &done) != JS_OK)
            return JS_THROW;
        if (done)
            break;
        if (js_array_push(ctx, arr, item) != JS_OK)
            return JS_THROW;
    }
    return JS_OK;
}

static int copy_own_enum_props(js_ctx *ctx, js_object *dst, js_object *src,
                               js_object *excl)
{
    uint32_t i;
    js_value srcv = js_object_value(src);

    if (src->cls == JS_CLASS_ARRAY) {
        for (i = 0; i < src->elen; i++) {
            js_string *k = index_key(ctx, i);
            js_prop *dp;
            if (!k) return JS_THROW;
            if (excl && js_own_prop_key(excl, k, 0))
                continue;
            dp = js_add_prop_key(ctx, dst, k, JS_P_DEFAULT);
            if (!dp) return JS_THROW;
            dp->flags = JS_P_DEFAULT;
            dp->setter = js_undefined();
            dp->value = src->elems[i];
        }
    } else if (js_is_typedarray_cls(src->cls)) {
        for (i = 0; i < src->view_length; i++) {
            js_string *k = index_key(ctx, i);
            js_prop *dp;
            if (!k) return JS_THROW;
            if (excl && js_own_prop_key(excl, k, 0))
                continue;
            dp = js_add_prop_key(ctx, dst, k, JS_P_DEFAULT);
            if (!dp) return JS_THROW;
            dp->flags = JS_P_DEFAULT;
            dp->setter = js_undefined();
            dp->value = js_number(js_typedarray_read(src, i));
        }
    } else if (src->cls == JS_CLASS_STRING && src->prim.type == JS_STRING) {
        for (i = 0; i < src->prim.u.str->len; i++) {
            js_string *k = index_key(ctx, i);
            js_prop *dp;
            if (!k) return JS_THROW;
            if (excl && js_own_prop_key(excl, k, 0))
                continue;
            dp = js_add_prop_key(ctx, dst, k, JS_P_DEFAULT);
            if (!dp) return JS_THROW;
            dp->flags = JS_P_DEFAULT;
            dp->setter = js_undefined();
            dp->value = js_mkstring(ctx, src->prim.u.str->data + i, 1);
        }
    }

    for (i = 0; i < src->nprops; i++) {
        js_prop *sp = &src->props[i];
        js_prop *dp;
        js_value pv;
        int is_sym;
        uint32_t flags;

        if ((sp->flags & JS_P_DEAD) || !(sp->flags & JS_P_ENUM))
            continue;
        is_sym = (sp->flags & JS_P_SYMBOL) != 0;
        if (excl && js_own_prop_key(excl, sp->key, is_sym))
            continue;
        if (js_obj_get_key(ctx, src, sp->key, is_sym, srcv, &pv) != JS_OK)
            return JS_THROW;
        flags = JS_P_DEFAULT | (is_sym ? (uint32_t)JS_P_SYMBOL : 0u);
        dp = js_add_prop_key(ctx, dst, sp->key, flags);
        if (!dp) return JS_THROW;
        dp->flags = flags;
        dp->setter = js_undefined();
        dp->value = pv;
    }
    return JS_OK;
}

static __attribute__((noinline)) int bind_pattern(
    js_ctx *ctx, js_node *pat, js_value val, js_env *env, int mode)
{
    if (!pat)
        return JS_OK;
    if (too_deep(ctx))
        return JS_THROW;
    ctx->depth++;

    switch (pat->type) {
    case N_IDENT: {
        int r;
        if (mode == BIND_LEX_INIT) {
            r = init_lex_var(ctx, env, pat->str, val);
        } else if (mode == BIND_VAR) {
            js_object *scope = resolve(ctx, env, pat->str);
            if (!scope) scope = ctx->global;
            r = js_obj_put(ctx, scope, pat->str, val);
        } else {
            jsref rf;
            rf.kind = REF_VAR;
            rf.is_sym = 0;
            rf.scope = resolve(ctx, env, pat->str);
            rf.base = js_undefined();
            rf.key = pat->str;
            r = ref_put(ctx, &rf, val);
        }
        ctx->depth--;
        return r;
    }

    case N_MEMBER:
    case N_INDEX: {
        jsref rf;
        int r;
        if (eval_ref(ctx, pat, env, &rf) != JS_OK) { ctx->depth--; return JS_THROW; }
        r = ref_put(ctx, &rf, val);
        ctx->depth--;
        return r;
    }

    case N_DEFAULT: {
        int r;
        if (val.type == JS_UNDEFINED) {
            if (eval(ctx, pat->b, env, &val) != JS_OK) {
                ctx->depth--;
                return JS_THROW;
            }
        }
        r = bind_pattern(ctx, pat->a, val, env, mode);
        ctx->depth--;
        return r;
    }

    case N_ARR_PAT: {
        js_iter it;
        js_node *q;
        int done = 0;

        if (js_iter_open(ctx, val, &it) != JS_OK) {
            ctx->depth--;
            return JS_THROW;
        }
        for (q = pat->a; q; q = q->next) {
            js_value item = js_undefined();
            if (q->type == N_REST) {
                js_object *rest = js_new_array(ctx);
                if (!rest) { ctx->depth--; return JS_THROW; }
                while (!done) {
                    if (js_step(ctx) != JS_OK ||
                        js_iter_next(ctx, &it, &item, &done) != JS_OK) {
                        ctx->depth--;
                        return JS_THROW;
                    }
                    if (!done && js_array_push(ctx, rest, item) != JS_OK) {
                        ctx->depth--;
                        return JS_THROW;
                    }
                }
                if (bind_pattern(ctx, q->a, js_object_value(rest), env, mode) != JS_OK) {
                    ctx->depth--;
                    return JS_THROW;
                }
                break;
            }
            if (!done) {
                if (js_iter_next(ctx, &it, &item, &done) != JS_OK) {
                    ctx->depth--;
                    return JS_THROW;
                }
                if (done) item = js_undefined();
            }
            if (q->type != N_EMPTY) {
                if (bind_pattern(ctx, q, item, env, mode) != JS_OK) {
                    ctx->depth--;
                    return JS_THROW;
                }
            }
        }
        ctx->depth--;
        return JS_OK;
    }

    case N_OBJ_PAT: {
        js_value objv;
        js_object *excl = 0;
        js_node *q;

        if (val.type == JS_UNDEFINED || val.type == JS_NULL) {
            ctx->depth--;
            return js_throw_error(ctx, JS_ERR_TYPE,
                                  "cannot destructure property of %s",
                                  val.type == JS_NULL ? "null" : "undefined");
        }
        if (js_to_object(ctx, val, &objv) != JS_OK) {
            ctx->depth--;
            return JS_THROW;
        }
        for (q = pat->a; q; q = q->next) {
            if (q->type == N_REST) {
                excl = js_obj_alloc(ctx, JS_CLASS_OBJECT, 0);
                if (!excl) { ctx->depth--; return JS_THROW; }
                break;
            }
        }
        for (q = pat->a; q; q = q->next) {
            if (q->type == N_REST) {
                js_object *rest = js_new_object(ctx);
                if (!rest ||
                    copy_own_enum_props(ctx, rest, objv.u.obj, excl) != JS_OK ||
                    bind_pattern(ctx, q->a, js_object_value(rest), env, mode) != JS_OK) {
                    ctx->depth--;
                    return JS_THROW;
                }
                break;
            }
            {
                js_string *k;
                int is_sym = 0;
                js_value pv;
                if (q->b) {
                    js_value kv;
                    if (eval(ctx, q->b, env, &kv) != JS_OK ||
                        js_to_propkey(ctx, kv, &k, &is_sym) != JS_OK) {
                        ctx->depth--;
                        return JS_THROW;
                    }
                } else {
                    k = q->str;
                }
                if (excl) {
                    if (!js_add_prop_key(ctx, excl, k,
                                         is_sym ? (uint32_t)JS_P_SYMBOL : 0u)) {
                        ctx->depth--;
                        return JS_THROW;
                    }
                }
                if (js_wrap_get_key(ctx, val, k, is_sym, &pv) != JS_OK ||
                    bind_pattern(ctx, q->a, pv, env, mode) != JS_OK) {
                    ctx->depth--;
                    return JS_THROW;
                }
            }
        }
        ctx->depth--;
        return JS_OK;
    }

    default:
        ctx->depth--;
        return js_throw_error(ctx, JS_ERR_REFERENCE, "invalid assignment pattern");
    }
}

/* ------------------------------------------------------------------ */
/* Functions                                                           */
/* ------------------------------------------------------------------ */

js_object *js_make_function(js_ctx *ctx, js_func *fn, js_env *env)
{
    js_object *f = js_obj_alloc(ctx, JS_CLASS_FUNCTION, ctx->proto[P_FUNCTION]);
    js_prop *p;

    if (!f)
        return 0;
    f->fn = fn;
    fn->closure = env;
    p = js_add_prop(ctx, f, ctx->s_length, 0);
    if (!p) return 0;
    p->value = js_number((double)fn->nparams);
    p = js_add_prop(ctx, f, ctx->s_name, 0);
    if (!p) return 0;
    p->value = js_string_value(fn->name ? fn->name : ctx->s_empty);

    if (!fn->native && fn->ctor_kind == JS_CTOR_NORMAL && !fn->is_class_ctor) {
        js_object *proto = js_new_object(ctx);
        if (!proto) return 0;
        p = js_add_prop(ctx, proto, ctx->s_constructor, JS_P_HIDDEN);
        if (!p) return 0;
        p->value = js_object_value(f);
        p = js_add_prop(ctx, f, ctx->s_prototype, JS_P_WRITE);
        if (!p) return 0;
        p->value = js_object_value(proto);
    }
    return f;
}

js_object *js_new_native(js_ctx *ctx, js_native native, const char *name,
                         int nargs)
{
    js_func *fn = (js_func *)js_alloc(ctx, sizeof(js_func));

    if (!fn)
        return 0;
    fn->native = native;
    fn->nparams = nargs;
    fn->ctor_kind = JS_CTOR_NONE;
    fn->name = name ? js_str_intern(ctx, name, strlen(name)) : ctx->s_empty;
    if (!fn->name)
        return 0;
    return js_make_function(ctx, fn, 0);
}

js_object *js_new_native_constructor(js_ctx *ctx, js_native native,
                                     const char *name, int nargs,
                                     js_object *prototype)
{
    js_object *ctor = js_new_native(ctx, native, name, nargs);
    js_prop *property;

    if (!ctor)
        return 0;
    ctor->fn->ctor_kind = JS_CTOR_NATIVE;
    if (!prototype)
        return ctor;
    property = js_add_prop(ctx, ctor, ctx->s_prototype, 0);
    if (!property)
        return 0;
    property->value = js_object_value(prototype);
    property = js_add_prop(ctx, prototype, ctx->s_constructor, JS_P_HIDDEN);
    if (!property)
        return 0;
    property->value = js_object_value(ctor);
    return ctor;
}

int js_is_constructing(js_ctx *ctx)
{
    return ctx && ctx->new_target != 0;
}

int js_define_native(js_ctx *ctx, js_object *obj, const char *name,
                     js_native native, int nargs)
{
    js_object *f = js_new_native(ctx, native, name, nargs);
    if (!f)
        return JS_THROW;
    return js_define(ctx, obj, name, js_object_value(f));
}

int js_define_accessor(js_ctx *ctx, js_object *obj, const char *name,
                       js_native getter, js_native setter, int enumerable)
{
    js_value getter_value = js_undefined();
    js_value setter_value = js_undefined();

    if (getter) {
        js_object *g = js_new_native(ctx, getter, name, 0);
        if (!g)
            return JS_THROW;
        getter_value = js_object_value(g);
    }
    if (setter) {
        js_object *s = js_new_native(ctx, setter, name, 1);
        if (!s)
            return JS_THROW;
        setter_value = js_object_value(s);
    }
    return js_define_accessor_value(ctx, obj, name, getter_value,
                                    setter_value, enumerable);
}

int js_define_accessor_value(js_ctx *ctx, js_object *obj, const char *name,
                             js_value getter, js_value setter,
                             int enumerable)
{
    js_string *key = js_str_intern(ctx, name, strlen(name));
    js_prop *property;

    if (!key)
        return JS_THROW;
    if ((getter.type != JS_UNDEFINED && !js_is_function(getter)) ||
        (setter.type != JS_UNDEFINED && !js_is_function(setter)))
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "accessor must be a function");
    property = js_add_prop(ctx, obj, key, 0);
    if (!property)
        return JS_THROW;
    property->flags = JS_P_ACCESSOR | JS_P_CONFIG |
                      (enumerable ? JS_P_ENUM : 0);
    property->value = getter;
    property->setter = setter;
    return JS_OK;
}

static __attribute__((noinline)) int make_arguments(js_ctx *ctx, js_object *f,
                                                    js_object *act, int argc,
                                                    js_value *argv)
{
    js_object *ao = js_obj_alloc(ctx, JS_CLASS_ARGUMENTS, ctx->proto[P_OBJECT]);
    js_prop *p;
    int i;

    if (!ao) return JS_THROW;
    for (i = 0; i < argc; i++) {
        char buf[16], t[16];
        int m = 0, k = i, tn = 0;
        if (k == 0) buf[m++] = '0';
        else {
            while (k) { t[tn++] = (char)('0' + k % 10); k /= 10; }
            while (tn) buf[m++] = t[--tn];
        }
        p = js_add_prop(ctx, ao, js_str_intern(ctx, buf, (unsigned long)m),
                        JS_P_DEFAULT);
        if (!p) return JS_THROW;
        p->value = argv[i];
    }
    p = js_add_prop(ctx, ao, ctx->s_length, JS_P_HIDDEN);
    if (!p) return JS_THROW;
    p->value = js_number((double)argc);
    p = js_add_prop(ctx, ao, ctx->s_callee, JS_P_HIDDEN);
    if (!p) return JS_THROW;
    p->value = js_object_value(f);
    if (ctx->sym_iterator.type == JS_SYMBOL && ctx->sym_iterator_key) {
        js_value iter_fn;
        if (js_obj_get_key(ctx, ctx->proto[P_ARRAY], ctx->sym_iterator_key, 1,
                           js_object_value(ctx->proto[P_ARRAY]), &iter_fn) == JS_OK &&
            js_is_function(iter_fn)) {
            if (js_define_symbol_prop(ctx, ao, ctx->sym_iterator, iter_fn,
                                      JS_P_HIDDEN) != JS_OK)
                return JS_THROW;
        }
    }
    p = js_add_prop(ctx, act, ctx->s_arguments, JS_P_WRITE);
    if (!p) return JS_THROW;
    p->value = js_object_value(ao);
    return JS_OK;
}

static void init_func_from_node(js_func *nf, js_node *fnode)
{
    nf->name = fnode->str;
    nf->params = fnode->a;
    nf->body = fnode->b;
    nf->vars = fnode->c;
    nf->funcs = fnode->d;
    nf->lex = fnode->e;
    nf->nparams = (int)fnode->num;
    nf->is_async = (fnode->flags & JS_NODE_ASYNC) != 0;
    nf->has_complex_params = (fnode->flags & JS_NODE_HAS_COMPLEX) != 0;
    nf->ctor_kind = (fnode->flags & JS_NODE_ARROW) ? JS_CTOR_ARROW
                  : (fnode->op == 1 || nf->is_async) ? JS_CTOR_NONE
                  : JS_CTOR_NORMAL;
}

static __attribute__((noinline)) int instantiate_funcs(js_ctx *ctx, js_node *list,
                                                       js_object *act, js_env *env)
{
    js_node *q;

    for (q = list; q; q = q->next) {
        js_func *nf = (js_func *)js_alloc(ctx, sizeof(js_func));
        js_object *fo;
        js_prop *p;
        if (!nf) return JS_THROW;
        init_func_from_node(nf, q->a);
        fo = js_make_function(ctx, nf, env);
        if (!fo) return JS_THROW;
        p = js_add_prop(ctx, act, q->str, JS_P_ENUM | JS_P_WRITE);
        if (!p) return JS_THROW;
        p->value = js_object_value(fo);
    }
    return JS_OK;
}

/* ------------------------------------------------------------------ */
/* Resumable async/await execution trampoline                          */
/* ------------------------------------------------------------------ */

typedef struct js_async_frame {
    js_node    *node;
    js_env     *env;
    js_label   *labels;
    uint8_t     pc;
    uint8_t     sub_state;
    uint8_t     is_stmt;
    uint8_t     saved_ct;
    js_value    saved_cv;
    js_string  *saved_lbl;
    int         saved_has_exc;
    js_value    saved_exc;
    js_value    v0, v1;
    js_node    *cur;
    js_node    *cur2;
    js_object  *arr;
    js_object  *excl;
    js_value   *vals;
    int         nvals, cvals;
    js_iter     it;
    js_string **keys;
    int         nkeys, keyi;
    js_env     *for_env;
    js_node   **chain;
    int         nchain, chain_i;
} js_async_frame;

struct js_async_task {
    js_object      *promise;
    js_async_frame *frames;
    int             sp, cap;
    uint8_t         ct;
    js_value        cv;
    js_string      *lbl;
    js_value        res;
};

static int async_push_frame(js_ctx *ctx, js_async_task *t, js_node *n,
                            js_env *env, js_label *labels, int is_stmt)
{
    js_async_frame *f;

    if (t->sp >= ctx->cfg.max_depth)
        return js_throw_error(ctx, JS_ERR_RANGE,
                              "interpreter recursion limit (%d) exceeded",
                              ctx->cfg.max_depth);
    if (t->sp == t->cap) {
        int nc = t->cap ? t->cap * 2 : 16;
        js_async_frame *nf = (js_async_frame *)js_alloc(
            ctx, (unsigned long)nc * sizeof(js_async_frame));
        if (!nf)
            return JS_THROW;
        if (t->sp)
            memcpy(nf, t->frames, (unsigned long)t->sp * sizeof(js_async_frame));
        t->frames = nf;
        t->cap = nc;
    }
    f = &t->frames[t->sp++];
    memset(f, 0, sizeof(*f));
    f->node = n;
    f->env = env;
    f->labels = labels;
    f->is_stmt = (uint8_t)is_stmt;
    return JS_OK;
}

static int async_frame_add_val(js_ctx *ctx, js_async_frame *f, js_value v)
{
    if (f->nvals >= JS_MAXARGS)
        return js_throw_error(ctx, JS_ERR_RANGE, "more than %d call arguments",
                              JS_MAXARGS);
    if (f->nvals == f->cvals) {
        int nc = f->cvals ? f->cvals * 2 : 8;
        js_value *nv = (js_value *)js_alloc(
            ctx, (unsigned long)nc * sizeof(js_value));
        if (!nv) return JS_THROW;
        if (f->nvals)
            memcpy(nv, f->vals, (unsigned long)f->nvals * sizeof(js_value));
        f->vals = nv;
        f->cvals = nc;
    }
    f->vals[f->nvals++] = v;
    return JS_OK;
}

static int targets_me(js_label *mine, js_string *lbl)
{
    js_label *l;

    if (!lbl)
        return 1;
    for (l = mine; l; l = l->next)
        if (js_str_eq(l->name, lbl))
            return 1;
    return 0;
}

static js_string **enum_keys(js_ctx *ctx, js_object *o, int *count);
static int binop(js_ctx *ctx, int op, js_value a, js_value b, js_value *out);

static __attribute__((noinline)) int async_step_stmt(
    js_ctx *ctx, js_async_task *t, js_async_frame *f)
{
    js_node *n = f->node;

    if (!n) {
        t->sp--;
        t->ct = CT_NORMAL;
        return 0;
    }

    /* Fast path: synchronous statement with no await anywhere inside. */
    if (!(n->flags & JS_NODE_HAS_AWAIT)) {
        js_label *saved = ctx->labels;
        js_value cv = js_undefined();
        js_string *lbl = 0;
        int r;
        ctx->labels = f->labels;
        r = exec(ctx, n, f->env, &cv, &lbl);
        ctx->labels = saved;
        t->sp--;
        t->ct = (uint8_t)r;
        t->cv = cv;
        t->lbl = lbl;
        return 0;
    }

    switch (n->type) {
    case N_BLOCK:
        if (f->pc == 0) {
            if (n->b) {
                js_env *benv = create_lex_env(ctx, f->env, n->b, 0);
                if (!benv) { t->ct = CT_THROW; return 0; }
                f->env = benv;
            }
            f->cur = n->a;
            f->pc = 1;
        }
        if (t->ct != CT_NORMAL) {
            t->sp--;
            return 0;
        }
        if (f->cur) {
            js_node *st = f->cur;
            f->cur = f->cur->next;
            if (async_push_frame(ctx, t, st, f->env, 0, 1) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        t->sp--;
        return 0;

    case N_EXPRSTMT:
        if (f->pc == 0) {
            f->pc = 1;
            if (async_push_frame(ctx, t, n->a, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        t->sp--;
        if (t->ct == CT_THROW) return 0;
        t->ct = CT_NORMAL;
        t->cv = t->res;
        return 0;

    case N_VAR:
        if (f->pc == 0) {
            f->cur = n->a;
            f->pc = 1;
        }
        for (;;) {
            js_node *d = f->cur;
            if (f->pc == 2) {
                if (t->ct == CT_THROW) { t->sp--; return 0; }
                if (d->b) {
                    if (bind_pattern(ctx, d->b, t->res, f->env,
                                     n->op ? BIND_LEX_INIT : BIND_VAR) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                } else if (n->op) {
                    if (init_lex_var(ctx, f->env, d->str, t->res) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                } else {
                    js_object *scope = resolve(ctx, f->env, d->str);
                    if (!scope) scope = ctx->global;
                    if (js_obj_put(ctx, scope, d->str, t->res) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                }
                f->cur = d->next;
                f->pc = 1;
                continue;
            }
            if (!d) { t->sp--; t->ct = CT_NORMAL; return 0; }
            if (!d->a) {
                if (n->op && d->str) {
                    if (init_lex_var(ctx, f->env, d->str, js_undefined()) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                }
                f->cur = d->next;
                continue;
            }
            f->pc = 2;
            if (async_push_frame(ctx, t, d->a, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }

    case N_RETURN:
        if (f->pc == 0) {
            if (!n->a) {
                t->sp--;
                t->ct = CT_RETURN;
                t->cv = js_undefined();
                return 0;
            }
            f->pc = 1;
            if (async_push_frame(ctx, t, n->a, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        t->sp--;
        if (t->ct == CT_THROW) return 0;
        t->ct = CT_RETURN;
        t->cv = t->res;
        return 0;

    case N_THROW:
        if (f->pc == 0) {
            f->pc = 1;
            if (async_push_frame(ctx, t, n->a, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        t->sp--;
        if (t->ct == CT_THROW) return 0;
        js_throw(ctx, t->res);
        t->ct = CT_THROW;
        return 0;

    case N_IF:
        if (f->pc == 0) {
            f->pc = 1;
            if (async_push_frame(ctx, t, n->a, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        if (f->pc == 1) {
            if (t->ct == CT_THROW) { t->sp--; return 0; }
            f->pc = 2;
            if (js_to_boolean(t->res)) {
                if (async_push_frame(ctx, t, n->b, f->env, 0, 1) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            if (n->c) {
                if (async_push_frame(ctx, t, n->c, f->env, 0, 1) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            t->sp--;
            t->ct = CT_NORMAL;
            return 0;
        }
        t->sp--;
        return 0;

    case N_WHILE:
        for (;;) {
            if (f->pc == 0) {
                f->pc = 1;
                if (async_push_frame(ctx, t, n->a, f->env, 0, 0) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            if (f->pc == 1) {
                if (t->ct == CT_THROW) { t->sp--; return 0; }
                if (!js_to_boolean(t->res)) {
                    t->sp--;
                    t->ct = CT_NORMAL;
                    return 0;
                }
                f->pc = 2;
                if (async_push_frame(ctx, t, n->b, f->env, 0, 1) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            if (t->ct == CT_BREAK && targets_me(f->labels, t->lbl)) {
                t->ct = CT_NORMAL; t->lbl = 0; t->sp--; return 0;
            }
            if (t->ct == CT_CONTINUE && targets_me(f->labels, t->lbl)) {
                t->ct = CT_NORMAL; t->lbl = 0; f->pc = 0; continue;
            }
            if (t->ct != CT_NORMAL) { t->sp--; return 0; }
            f->pc = 0;
        }

    case N_DO:
        for (;;) {
            if (f->pc == 0) {
                f->pc = 1;
                if (async_push_frame(ctx, t, n->a, f->env, 0, 1) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            if (f->pc == 1) {
                if (t->ct == CT_BREAK && targets_me(f->labels, t->lbl)) {
                    t->ct = CT_NORMAL; t->lbl = 0; t->sp--; return 0;
                }
                if (t->ct == CT_CONTINUE && targets_me(f->labels, t->lbl)) {
                    t->ct = CT_NORMAL; t->lbl = 0;
                } else if (t->ct != CT_NORMAL) {
                    t->sp--; return 0;
                }
                f->pc = 2;
                if (async_push_frame(ctx, t, n->b, f->env, 0, 0) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            if (t->ct == CT_THROW) { t->sp--; return 0; }
            if (!js_to_boolean(t->res)) {
                t->sp--; t->ct = CT_NORMAL; return 0;
            }
            f->pc = 0;
        }

    case N_FOR:
        for (;;) {
            if (f->sub_state == 0) {
                if (n->e) {
                    f->for_env = create_lex_env(ctx, f->env, n->e, 0);
                    if (!f->for_env) { t->ct = CT_THROW; t->sp--; return 0; }
                } else {
                    f->for_env = f->env;
                }
                f->sub_state = 1;
                if (n->a) {
                    if (async_push_frame(ctx, t, n->a, f->for_env, 0, 1) != JS_OK)
                        t->ct = CT_THROW;
                    return 0;
                }
            }
            if (f->sub_state == 1) {
                if (t->ct != CT_NORMAL) { t->sp--; return 0; }
                if (n->e) {
                    f->for_env = copy_lex_env(ctx, f->for_env);
                    if (!f->for_env) { t->ct = CT_THROW; t->sp--; return 0; }
                }
                f->sub_state = 2;
                if (n->b) {
                    if (async_push_frame(ctx, t, n->b, f->for_env, 0, 0) != JS_OK)
                        t->ct = CT_THROW;
                    return 0;
                }
                t->res = js_bool(1);
            }
            if (f->sub_state == 2) {
                if (t->ct == CT_THROW) { t->sp--; return 0; }
                if (!js_to_boolean(t->res)) {
                    t->sp--; t->ct = CT_NORMAL; return 0;
                }
                f->sub_state = 3;
                if (async_push_frame(ctx, t, n->d, f->for_env, 0, 1) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            if (f->sub_state == 3) {
                if (t->ct == CT_BREAK && targets_me(f->labels, t->lbl)) {
                    t->ct = CT_NORMAL; t->lbl = 0; t->sp--; return 0;
                }
                if (t->ct == CT_CONTINUE && targets_me(f->labels, t->lbl)) {
                    t->ct = CT_NORMAL; t->lbl = 0;
                } else if (t->ct != CT_NORMAL) {
                    t->sp--; return 0;
                }
                if (n->e) {
                    f->for_env = copy_lex_env(ctx, f->for_env);
                    if (!f->for_env) { t->ct = CT_THROW; t->sp--; return 0; }
                }
                f->sub_state = 4;
                if (n->c) {
                    if (async_push_frame(ctx, t, n->c, f->for_env, 0, 0) != JS_OK)
                        t->ct = CT_THROW;
                    return 0;
                }
            }
            if (f->sub_state == 4) {
                if (t->ct == CT_THROW) { t->sp--; return 0; }
                f->sub_state = 2;
                if (n->b) {
                    if (async_push_frame(ctx, t, n->b, f->for_env, 0, 0) != JS_OK)
                        t->ct = CT_THROW;
                    return 0;
                }
                t->res = js_bool(1);
            }
        }

    case N_FOROF:
    case N_FORIN:
        for (;;) {
            if (f->sub_state == 0) {
                js_env *rhs_env = f->env;
                if (n->e) {
                    rhs_env = create_lex_env(ctx, f->env, n->e, 0);
                    if (!rhs_env) { t->ct = CT_THROW; t->sp--; return 0; }
                }
                f->sub_state = 1;
                if (async_push_frame(ctx, t, n->b, rhs_env, 0, 0) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            if (f->sub_state == 1) {
                if (t->ct == CT_THROW) { t->sp--; return 0; }
                if (n->type == N_FOROF) {
                    if (js_iter_open(ctx, t->res, &f->it) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                } else {
                    js_value ov = t->res;
                    if (ov.type == JS_UNDEFINED || ov.type == JS_NULL) {
                        t->sp--; t->ct = CT_NORMAL; return 0;
                    }
                    if (ov.type != JS_OBJECT && js_to_object(ctx, ov, &ov) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                    f->v0 = ov;
                    f->keys = enum_keys(ctx, ov.u.obj, &f->nkeys);
                    if (!f->keys) { t->ct = CT_THROW; t->sp--; return 0; }
                    f->keyi = 0;
                }
                f->sub_state = 2;
            }
            if (f->sub_state == 2) {
                js_value item = js_undefined();
                int done = 0;
                js_env *benv = f->env;
                if (n->type == N_FOROF) {
                    if (js_iter_next(ctx, &f->it, &item, &done) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                    if (done) { t->sp--; t->ct = CT_NORMAL; return 0; }
                } else {
                    while (f->keyi < f->nkeys &&
                           !js_obj_has(ctx, f->v0.u.obj, f->keys[f->keyi]))
                        f->keyi++;
                    if (f->keyi >= f->nkeys) {
                        t->sp--; t->ct = CT_NORMAL; return 0;
                    }
                    item = js_string_value(f->keys[f->keyi++]);
                }
                if (n->e && (n->op == 1 || n->op == 2)) {
                    benv = create_lex_env(ctx, f->env, n->e, 0);
                    if (!benv) { t->ct = CT_THROW; t->sp--; return 0; }
                }
                if (n->flags) {
                    js_node *d = n->a;
                    int mode = (n->op == 0) ? BIND_VAR : BIND_LEX_INIT;
                    if (d->b) {
                        if (bind_pattern(ctx, d->b, item, benv, mode) != JS_OK) {
                            t->ct = CT_THROW; t->sp--; return 0;
                        }
                    } else if (n->op) {
                        if (init_lex_var(ctx, benv, d->str, item) != JS_OK) {
                            t->ct = CT_THROW; t->sp--; return 0;
                        }
                    } else {
                        js_object *scope = resolve(ctx, benv, d->str);
                        if (!scope) scope = ctx->global;
                        if (js_obj_put(ctx, scope, d->str, item) != JS_OK) {
                            t->ct = CT_THROW; t->sp--; return 0;
                        }
                    }
                } else {
                    if (bind_pattern(ctx, n->a, item, benv, BIND_ASSIGN) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                }
                f->sub_state = 3;
                if (async_push_frame(ctx, t, n->c, benv, 0, 1) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            if (f->sub_state == 3) {
                if (t->ct == CT_BREAK && targets_me(f->labels, t->lbl)) {
                    t->ct = CT_NORMAL; t->lbl = 0; t->sp--; return 0;
                }
                if (t->ct == CT_CONTINUE && targets_me(f->labels, t->lbl)) {
                    t->ct = CT_NORMAL; t->lbl = 0;
                } else if (t->ct != CT_NORMAL) {
                    t->sp--; return 0;
                }
                f->sub_state = 2;
            }
        }

    case N_SWITCH:
        for (;;) {
            if (f->sub_state == 0) {
                f->sub_state = 1;
                if (async_push_frame(ctx, t, n->a, f->env, 0, 0) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            if (f->sub_state == 1) {
                if (t->ct == CT_THROW) { t->sp--; return 0; }
                f->v0 = t->res;
                if (n->c) {
                    f->env = create_lex_env(ctx, f->env, n->c, 0);
                    if (!f->env) { t->ct = CT_THROW; t->sp--; return 0; }
                }
                f->cur = n->b;
                f->cur2 = 0; /* tracks default clause */
                f->sub_state = 2;
            }
            if (f->sub_state == 2) {
                while (f->cur && !f->cur->a) {
                    f->cur2 = f->cur;
                    f->cur = f->cur->next;
                }
                if (!f->cur) {
                    f->cur = f->cur2;
                    f->cur2 = f->cur ? f->cur->b : 0;
                    f->sub_state = 4;
                    continue;
                }
                f->sub_state = 3;
                if (async_push_frame(ctx, t, f->cur->a, f->env, 0, 0) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            if (f->sub_state == 3) {
                if (t->ct == CT_THROW) { t->sp--; return 0; }
                if (js_strict_equals(f->v0, t->res)) {
                    f->cur2 = f->cur->b;
                    f->sub_state = 4;
                    continue;
                }
                f->cur = f->cur->next;
                f->sub_state = 2;
                continue;
            }
            /* sub_state == 4: run statements through fallthrough */
            if (t->ct == CT_BREAK && targets_me(f->labels, t->lbl)) {
                t->ct = CT_NORMAL; t->lbl = 0; t->sp--; return 0;
            }
            if (t->ct != CT_NORMAL) { t->sp--; return 0; }
            while (f->cur && !f->cur2) {
                f->cur = f->cur->next;
                f->cur2 = f->cur ? f->cur->b : 0;
            }
            if (!f->cur2) {
                t->sp--; t->ct = CT_NORMAL; return 0;
            }
            {
                js_node *st = f->cur2;
                f->cur2 = f->cur2->next;
                if (async_push_frame(ctx, t, st, f->env, 0, 1) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
        }

    case N_TRY:
        if (f->sub_state == 0) {
            f->sub_state = 1;
            if (async_push_frame(ctx, t, n->a, f->env, 0, 1) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        if (f->sub_state == 1) {
            if (t->ct == CT_THROW && n->b && !ctx->fatal) {
                js_value exc = js_exception(ctx);
                js_object *scope = js_obj_alloc(ctx, JS_CLASS_OBJECT, 0);
                js_env *cenv;
                ctx->has_exception = 0;
                ctx->exception = js_undefined();
                t->ct = CT_NORMAL;
                if (!scope) { t->ct = CT_THROW; t->sp--; return 0; }
                cenv = js_env_new(ctx, f->env, scope);
                if (!cenv) { t->ct = CT_THROW; t->sp--; return 0; }
                cenv->this_val = f->env->this_val;
                if (n->d) {
                    if (bind_pattern(ctx, n->d, exc, cenv, BIND_VAR) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                } else if (n->str) {
                    js_prop *p = js_add_prop(ctx, scope, n->str,
                                             JS_P_ENUM | JS_P_WRITE);
                    if (!p) { t->ct = CT_THROW; t->sp--; return 0; }
                    p->value = exc;
                }
                f->sub_state = 2;
                if (async_push_frame(ctx, t, n->b, cenv, 0, 1) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            f->sub_state = 2;
        }
        if (f->sub_state == 2) {
            if (!n->c || ctx->fatal) {
                t->sp--;
                return 0;
            }
            f->saved_ct = t->ct;
            f->saved_cv = t->cv;
            f->saved_lbl = t->lbl;
            f->saved_has_exc = ctx->has_exception;
            f->saved_exc = ctx->exception;
            ctx->has_exception = 0;
            ctx->exception = js_undefined();
            t->ct = CT_NORMAL;
            t->cv = js_undefined();
            t->lbl = 0;
            f->sub_state = 3;
            if (async_push_frame(ctx, t, n->c, f->env, 0, 1) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        if (t->ct != CT_NORMAL) {
            t->sp--;
            return 0;
        }
        ctx->exception = f->saved_exc;
        ctx->has_exception = f->saved_has_exc;
        t->ct = f->saved_ct;
        t->cv = f->saved_cv;
        t->lbl = f->saved_lbl;
        t->sp--;
        return 0;

    case N_LABEL:
        if (f->pc == 0) {
            js_label *lab = (js_label *)js_alloc(ctx, sizeof(js_label));
            if (!lab) { t->ct = CT_THROW; t->sp--; return 0; }
            lab->name = n->str;
            lab->next = f->labels;
            f->pc = 1;
            if (async_push_frame(ctx, t, n->a, f->env, lab, 1) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        if (t->ct == CT_BREAK && t->lbl && js_str_eq(t->lbl, n->str)) {
            t->ct = CT_NORMAL;
            t->lbl = 0;
        }
        t->sp--;
        return 0;

    case N_WITH:
        if (f->pc == 0) {
            f->pc = 1;
            if (async_push_frame(ctx, t, n->a, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        if (f->pc == 1) {
            js_value o;
            js_env *wenv;
            if (t->ct == CT_THROW) { t->sp--; return 0; }
            if (js_to_object(ctx, t->res, &o) != JS_OK) {
                t->ct = CT_THROW; t->sp--; return 0;
            }
            wenv = js_env_new(ctx, f->env, o.u.obj);
            if (!wenv) { t->ct = CT_THROW; t->sp--; return 0; }
            wenv->is_with = 1;
            wenv->this_val = f->env->this_val;
            f->pc = 2;
            if (async_push_frame(ctx, t, n->b, wenv, 0, 1) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        t->sp--;
        return 0;

    default:
        /* Expression statement fallback */
        if (f->pc == 0) {
            f->pc = 1;
            if (async_push_frame(ctx, t, n, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        t->sp--;
        if (t->ct == CT_THROW) return 0;
        t->ct = CT_NORMAL;
        t->cv = t->res;
        return 0;
    }
}

static void opt_collect(js_node *n, js_node **arr, int *idx)
{
    if (n->type == N_MEMBER || n->type == N_INDEX || n->type == N_CALL)
        opt_collect(n->a, arr, idx);
    arr[(*idx)++] = n;
}

static int opt_count(js_node *n)
{
    if (n->type == N_MEMBER || n->type == N_INDEX || n->type == N_CALL)
        return 1 + opt_count(n->a);
    return 1;
}

static __attribute__((noinline)) int async_step_expr(
    js_ctx *ctx, js_async_task *t, js_async_frame *f)
{
    js_node *n = f->node;

    if (!(n->flags & JS_NODE_HAS_AWAIT)) {
        js_value out = js_undefined();
        int r = eval(ctx, n, f->env, &out);
        t->sp--;
        if (r == JS_OK) {
            t->res = out;
            t->ct = CT_NORMAL;
        } else {
            t->ct = CT_THROW;
        }
        return 0;
    }

    switch (n->type) {
    case N_AWAIT:
        if (f->pc == 0) {
            f->pc = 1;
            if (async_push_frame(ctx, t, n->a, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        t->sp--;
        if (t->ct == CT_THROW) return 0;
        {
            js_value pv;
            js_promise_reaction *react;
            if (js_promise_resolve_value(ctx, t->res, &pv) != JS_OK) {
                t->ct = CT_THROW;
                return 0;
            }
            react = (js_promise_reaction *)js_alloc(ctx, sizeof(*react));
            if (!react) { t->ct = CT_THROW; return 0; }
            react->on_fulfilled = js_undefined();
            react->on_rejected = js_undefined();
            react->async_task = t;
            if (js_promise_add_reaction(ctx, pv.u.obj, react) != JS_OK) {
                t->ct = CT_THROW;
                return 0;
            }
        }
        return 1; /* suspend */

    case N_UNARY:
        if (n->op == TK_DELETE &&
            (n->a->type == N_MEMBER || n->a->type == N_INDEX)) {
            if (f->pc == 0) {
                f->pc = 1;
                if (async_push_frame(ctx, t, n->a->a, f->env, 0, 0) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            if (f->pc == 1) {
                if (t->ct == CT_THROW) { t->sp--; return 0; }
                f->v0 = t->res;
                if (n->a->type == N_INDEX) {
                    f->pc = 2;
                    if (async_push_frame(ctx, t, n->a->b, f->env, 0, 0) != JS_OK)
                        t->ct = CT_THROW;
                    return 0;
                }
                t->res = js_string_value(n->a->str);
            }
            if (t->ct == CT_THROW) { t->sp--; return 0; }
            {
                js_string *k;
                int is_sym = 0;
                if (n->a->type == N_INDEX) {
                    if (js_to_propkey(ctx, t->res, &k, &is_sym) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                } else {
                    k = n->a->str;
                }
                if (f->v0.type == JS_UNDEFINED || f->v0.type == JS_NULL) {
                    js_throw_error(ctx, JS_ERR_TYPE, "cannot delete property of %s",
                                   f->v0.type == JS_NULL ? "null" : "undefined");
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                t->res = js_bool(f->v0.type != JS_OBJECT ? 1
                                 : js_obj_delete_key(ctx, f->v0.u.obj, k, is_sym));
                t->sp--;
                return 0;
            }
        }
        if (f->pc == 0) {
            f->pc = 1;
            if (async_push_frame(ctx, t, n->a, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        t->sp--;
        if (t->ct == CT_THROW) return 0;
        {
            js_value v = t->res;
            double d;
            switch (n->op) {
            case TK_TYPEOF: t->res = js_mkcstring(ctx, js_typeof(v)); break;
            case TK_DELETE: t->res = js_bool(1); break;
            case TK_VOID:   t->res = js_undefined(); break;
            case TK_ADD:
                if (js_to_number(ctx, v, &d) != JS_OK) t->ct = CT_THROW;
                else t->res = js_number(d);
                break;
            case TK_SUB:
                if (js_to_number(ctx, v, &d) != JS_OK) t->ct = CT_THROW;
                else t->res = js_number(-d);
                break;
            case TK_BNOT: {
                int32_t i;
                if (js_to_int32(ctx, v, &i) != JS_OK) t->ct = CT_THROW;
                else t->res = js_number((double)(~i));
                break;
            }
            case TK_NOT:
                t->res = js_bool(!js_to_boolean(v));
                break;
            default:
                js_throw_error(ctx, JS_ERR_SYNTAX, "unsupported unary operator");
                t->ct = CT_THROW;
                break;
            }
        }
        return 0;

    case N_BINARY:
        if (f->pc == 0) {
            f->pc = 1;
            if (async_push_frame(ctx, t, n->a, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        if (f->pc == 1) {
            if (t->ct == CT_THROW) { t->sp--; return 0; }
            f->v0 = t->res;
            f->pc = 2;
            if (async_push_frame(ctx, t, n->b, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        t->sp--;
        if (t->ct == CT_THROW) return 0;
        if (binop(ctx, n->op, f->v0, t->res, &t->res) != JS_OK)
            t->ct = CT_THROW;
        return 0;

    case N_LOGICAL:
        if (f->pc == 0) {
            f->pc = 1;
            if (async_push_frame(ctx, t, n->a, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        if (f->pc == 1) {
            if (t->ct == CT_THROW) { t->sp--; return 0; }
            if (n->op == TK_ANDAND) {
                if (!js_to_boolean(t->res)) { t->sp--; return 0; }
            } else if (n->op == TK_QQ) {
                if (t->res.type != JS_UNDEFINED && t->res.type != JS_NULL) {
                    t->sp--; return 0;
                }
            } else {
                if (js_to_boolean(t->res)) { t->sp--; return 0; }
            }
            f->pc = 2;
            if (async_push_frame(ctx, t, n->b, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        t->sp--;
        return 0;

    case N_COND:
        if (f->pc == 0) {
            f->pc = 1;
            if (async_push_frame(ctx, t, n->a, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        if (f->pc == 1) {
            if (t->ct == CT_THROW) { t->sp--; return 0; }
            f->pc = 2;
            if (async_push_frame(ctx, t, js_to_boolean(t->res) ? n->b : n->c,
                                 f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        t->sp--;
        return 0;

    case N_SEQ:
        if (f->pc == 0) {
            f->pc = 1;
            if (async_push_frame(ctx, t, n->a, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        if (f->pc == 1) {
            if (t->ct == CT_THROW) { t->sp--; return 0; }
            f->pc = 2;
            if (async_push_frame(ctx, t, n->b, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        t->sp--;
        return 0;

    case N_MEMBER:
        if (f->pc == 0) {
            f->pc = 1;
            if (async_push_frame(ctx, t, n->a, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        t->sp--;
        if (t->ct == CT_THROW) return 0;
        if (js_wrap_get(ctx, t->res, n->str, &t->res) != JS_OK)
            t->ct = CT_THROW;
        return 0;

    case N_INDEX:
        if (f->pc == 0) {
            f->pc = 1;
            if (async_push_frame(ctx, t, n->a, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        if (f->pc == 1) {
            if (t->ct == CT_THROW) { t->sp--; return 0; }
            f->v0 = t->res;
            f->pc = 2;
            if (async_push_frame(ctx, t, n->b, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        t->sp--;
        if (t->ct == CT_THROW) return 0;
        if (js_get_value(ctx, f->v0, t->res, &t->res) != JS_OK)
            t->ct = CT_THROW;
        return 0;

    case N_ARRAY:
        if (f->pc == 0) {
            f->arr = js_new_array(ctx);
            if (!f->arr) { t->ct = CT_THROW; t->sp--; return 0; }
            f->cur = n->a;
            f->sub_state = 0;
            f->pc = 1;
        }
        for (;;) {
            if (f->pc == 2) {
                if (t->ct == CT_THROW) { t->sp--; return 0; }
                if (spread_array_iter(ctx, f->arr, t->res) != JS_OK) {
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                f->sub_state = 0;
                f->cur = f->cur->next;
                f->pc = 1;
            } else if (f->pc == 3) {
                if (t->ct == CT_THROW) { t->sp--; return 0; }
                if (js_array_push(ctx, f->arr, t->res) != JS_OK) {
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                f->sub_state = 0;
                f->cur = f->cur->next;
                f->pc = 1;
            }
            if (!f->cur) {
                if (f->sub_state && f->arr->elen) f->arr->elen--;
                t->res = js_object_value(f->arr);
                t->sp--;
                return 0;
            }
            if (f->cur->type == N_EMPTY) {
                if (js_array_push(ctx, f->arr, js_undefined()) != JS_OK) {
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                f->sub_state = 1;
                f->cur = f->cur->next;
                continue;
            }
            if (f->cur->type == N_SPREAD) {
                f->pc = 2;
                if (async_push_frame(ctx, t, f->cur->a, f->env, 0, 0) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            f->pc = 3;
            if (async_push_frame(ctx, t, f->cur, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }

    case N_OBJECT:
        if (f->pc == 0) {
            f->arr = js_new_object(ctx);
            if (!f->arr) { t->ct = CT_THROW; t->sp--; return 0; }
            f->cur = n->a;
            f->pc = 1;
        }
        for (;;) {
            js_node *q = f->cur;
            if (f->pc == 2) {
                if (t->ct == CT_THROW) { t->sp--; return 0; }
                if (t->res.type != JS_UNDEFINED && t->res.type != JS_NULL) {
                    js_value vo;
                    if (js_to_object(ctx, t->res, &vo) != JS_OK ||
                        copy_own_enum_props(ctx, f->arr, vo.u.obj, 0) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                }
                f->cur = q->next; f->pc = 1; continue;
            }
            if (f->pc == 3) {
                if (t->ct == CT_THROW) { t->sp--; return 0; }
                f->v0 = t->res;
                if (q->op == 1 || q->op == 2 ||
                    (q->a && q->a->type == N_FUNC && q->a->op == 1)) {
                    js_value fv;
                    js_string *k;
                    int is_sym = 0;
                    js_prop *p;
                    if (eval(ctx, q->a, f->env, &fv) != JS_OK ||
                        js_to_propkey(ctx, f->v0, &k, &is_sym) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                    fv.u.obj->fn->home_obj = f->arr;
                    if (q->op) {
                        uint32_t fl = JS_P_ACCESSOR | JS_P_ENUM | JS_P_CONFIG |
                                      (is_sym ? (uint32_t)JS_P_SYMBOL : 0u);
                        p = js_own_prop_key(f->arr, k, is_sym);
                        if (!p || !(p->flags & JS_P_ACCESSOR)) {
                            p = js_add_prop_key(ctx, f->arr, k, fl);
                            if (!p) { t->ct = CT_THROW; t->sp--; return 0; }
                            p->flags = fl;
                            p->value = js_undefined();
                            p->setter = js_undefined();
                        }
                        if (q->op == 1) p->value = fv; else p->setter = fv;
                    } else {
                        uint32_t fl = JS_P_DEFAULT | (is_sym ? (uint32_t)JS_P_SYMBOL : 0u);
                        p = js_add_prop_key(ctx, f->arr, k, fl);
                        if (!p) { t->ct = CT_THROW; t->sp--; return 0; }
                        p->flags = fl; p->setter = js_undefined(); p->value = fv;
                    }
                    f->cur = q->next; f->pc = 1; continue;
                }
                f->pc = 4;
                if (async_push_frame(ctx, t, q->a, f->env, 0, 0) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            if (f->pc == 4) {
                js_string *k;
                int is_sym = 0;
                uint32_t fl;
                js_prop *p;
                if (t->ct == CT_THROW) { t->sp--; return 0; }
                if (js_to_propkey(ctx, f->v0, &k, &is_sym) != JS_OK) {
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                fl = JS_P_DEFAULT | (is_sym ? (uint32_t)JS_P_SYMBOL : 0u);
                p = js_add_prop_key(ctx, f->arr, k, fl);
                if (!p) { t->ct = CT_THROW; t->sp--; return 0; }
                p->flags = fl; p->setter = js_undefined(); p->value = t->res;
                f->cur = q->next; f->pc = 1; continue;
            }
            if (!q) { t->res = js_object_value(f->arr); t->sp--; return 0; }
            if (q->op == 3) {
                f->pc = 2;
                if (async_push_frame(ctx, t, q->a, f->env, 0, 0) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            if (q->b) {
                f->pc = 3;
                if (async_push_frame(ctx, t, q->b, f->env, 0, 0) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            f->v0 = js_string_value(q->str);
            if (q->op == 1 || q->op == 2 ||
                (q->a && q->a->type == N_FUNC && q->a->op == 1)) {
                js_value fv;
                js_prop *p;
                if (eval(ctx, q->a, f->env, &fv) != JS_OK) {
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                fv.u.obj->fn->home_obj = f->arr;
                if (q->op) {
                    p = js_own_prop(f->arr, q->str);
                    if (!p || !(p->flags & JS_P_ACCESSOR)) {
                        p = js_add_prop(ctx, f->arr, q->str, 0);
                        if (!p) { t->ct = CT_THROW; t->sp--; return 0; }
                        p->flags = JS_P_ACCESSOR | JS_P_ENUM | JS_P_CONFIG;
                        p->value = js_undefined(); p->setter = js_undefined();
                    }
                    if (q->op == 1) p->value = fv; else p->setter = fv;
                } else {
                    p = js_add_prop(ctx, f->arr, q->str, JS_P_DEFAULT);
                    if (!p) { t->ct = CT_THROW; t->sp--; return 0; }
                    p->flags = JS_P_DEFAULT; p->setter = js_undefined(); p->value = fv;
                }
                f->cur = q->next; continue;
            }
            f->pc = 4;
            if (async_push_frame(ctx, t, q->a, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }

    case N_TPL:
        if (f->pc == 0) {
            f->arr = js_new_array(ctx);
            if (!f->arr) { t->ct = CT_THROW; t->sp--; return 0; }
            f->cur = n->a;
            f->pc = 1;
        }
        for (;;) {
            if (f->pc == 2) {
                js_value sv;
                if (t->ct == CT_THROW) { t->sp--; return 0; }
                if (js_to_string(ctx, t->res, &sv) != JS_OK ||
                    js_array_push(ctx, f->arr, sv) != JS_OK) {
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                f->cur = f->cur->next;
                f->pc = 1;
            }
            if (!f->cur) {
                js_sbuf b;
                uint32_t i;
                js_sb_init(&b, ctx);
                for (i = 0; i < f->arr->elen; i++) {
                    js_string *s = f->arr->elems[i].u.str;
                    if (js_sb_put(&b, s->data, s->len) != JS_OK) {
                        js_sb_free(&b);
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                }
                if (js_sb_finish(&b, &t->res) != JS_OK)
                    t->ct = CT_THROW;
                t->sp--;
                return 0;
            }
            if (f->cur->type == N_STR) {
                if (js_array_push(ctx, f->arr, js_string_value(f->cur->str)) != JS_OK) {
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                f->cur = f->cur->next;
                continue;
            }
            f->pc = 2;
            if (async_push_frame(ctx, t, f->cur, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }

    case N_CALL:
    case N_NEW:
        if (n->flags & JS_NODE_TAGGED) {
            /* Tagged call with awaited expressions inside template */
            if (f->pc == 0) {
                js_node *callee = n->a;
                f->v1 = js_undefined();
                if (callee->type == N_MEMBER || callee->type == N_INDEX) {
                    f->pc = 1;
                    if (async_push_frame(ctx, t, callee->a, f->env, 0, 0) != JS_OK)
                        t->ct = CT_THROW;
                    return 0;
                }
                f->pc = 4;
                if (async_push_frame(ctx, t, callee, f->env, 0, 0) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            if (f->pc == 1) {
                if (t->ct == CT_THROW) { t->sp--; return 0; }
                f->v1 = t->res; /* thisv */
                if (n->a->type == N_INDEX) {
                    f->pc = 2;
                    if (async_push_frame(ctx, t, n->a->b, f->env, 0, 0) != JS_OK)
                        t->ct = CT_THROW;
                    return 0;
                }
                if (js_wrap_get(ctx, f->v1, n->a->str, &f->v0) != JS_OK) {
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                f->pc = 5;
            } else if (f->pc == 2) {
                if (t->ct == CT_THROW) { t->sp--; return 0; }
                if (js_get_value(ctx, f->v1, t->res, &f->v0) != JS_OK) {
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                f->pc = 5;
            } else if (f->pc == 4) {
                if (t->ct == CT_THROW) { t->sp--; return 0; }
                f->v0 = t->res;
                f->pc = 5;
            }
            if (f->pc == 5) {
                js_object *raw;
                if (!js_is_function(f->v0)) {
                    js_throw_error(ctx, JS_ERR_TYPE, "tag is not a function");
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                f->arr = js_new_array(ctx);
                raw = js_new_array(ctx);
                if (!f->arr || !raw) { t->ct = CT_THROW; t->sp--; return 0; }
                f->excl = raw;
                if (async_frame_add_val(ctx, f, js_object_value(f->arr)) != JS_OK) {
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                f->cur = n->b->a;
                f->sub_state = 0;
                f->pc = 6;
            }
            for (;;) {
                if (f->pc == 7) {
                    if (t->ct == CT_THROW) { t->sp--; return 0; }
                    if (async_frame_add_val(ctx, f, t->res) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                    f->sub_state++;
                    f->cur = f->cur->next;
                    f->pc = 6;
                }
                if (!f->cur) {
                    f->excl->extensible = 0;
                    if (js_define(ctx, f->arr, "raw",
                                  js_object_value(f->excl)) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                    f->arr->extensible = 0;
                    if (js_call_func(ctx, f->v0.u.obj, f->v1, f->nvals,
                                     f->vals, &t->res) != JS_OK)
                        t->ct = CT_THROW;
                    t->sp--;
                    return 0;
                }
                if ((f->sub_state & 1) == 0) {
                    js_value ck = (f->cur->type == N_STR)
                        ? js_string_value(f->cur->str) : js_undefined();
                    if (js_array_push(ctx, f->arr, ck) != JS_OK ||
                        js_array_push(ctx, f->excl,
                                      js_string_value(f->cur->str2)) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                    f->sub_state++;
                    f->cur = f->cur->next;
                    continue;
                }
                f->pc = 7;
                if (async_push_frame(ctx, t, f->cur, f->env, 0, 0) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
        }
        if (f->pc == 0) {
            js_node *callee = n->a;
            f->v1 = js_undefined();
            if (n->type == N_CALL &&
                (callee->type == N_MEMBER || callee->type == N_INDEX)) {
                if (callee->a->type == N_SUPER) {
                    jsref r2;
                    if (eval_ref(ctx, callee, f->env, &r2) != JS_OK ||
                        ref_get(ctx, &r2, &f->v0) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                    f->v1 = r2.base;
                    f->pc = 5;
                } else {
                    f->pc = 1;
                    if (async_push_frame(ctx, t, callee->a, f->env, 0, 0) != JS_OK)
                        t->ct = CT_THROW;
                    return 0;
                }
            } else if (n->type == N_CALL && callee->type == N_IDENT) {
                js_object *scope = resolve(ctx, f->env, callee->str);
                js_env *e;
                if (!scope) {
                    js_throw_error(ctx, JS_ERR_REFERENCE, "%s is not defined",
                                   callee->str->data);
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                if (check_scope_tdz(ctx, scope, callee->str) != JS_OK ||
                    js_obj_get(ctx, scope, callee->str,
                               js_object_value(scope), &f->v0) != JS_OK) {
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                for (e = f->env; e; e = e->parent)
                    if (e->is_with && e->vars == scope) {
                        f->v1 = js_object_value(scope);
                        break;
                    }
                f->pc = 5;
            } else {
                f->pc = 4;
                if (async_push_frame(ctx, t, callee, f->env, 0, 0) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
        }
        if (f->pc == 1) {
            if (t->ct == CT_THROW) { t->sp--; return 0; }
            f->v1 = t->res;
            if (n->a->type == N_INDEX) {
                f->pc = 2;
                if (async_push_frame(ctx, t, n->a->b, f->env, 0, 0) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            if (js_wrap_get(ctx, f->v1, n->a->str, &f->v0) != JS_OK) {
                t->ct = CT_THROW; t->sp--; return 0;
            }
            f->pc = 5;
        } else if (f->pc == 2) {
            if (t->ct == CT_THROW) { t->sp--; return 0; }
            if (js_get_value(ctx, f->v1, t->res, &f->v0) != JS_OK) {
                t->ct = CT_THROW; t->sp--; return 0;
            }
            f->pc = 5;
        } else if (f->pc == 4) {
            if (t->ct == CT_THROW) { t->sp--; return 0; }
            f->v0 = t->res;
            f->pc = 5;
        }
        if (f->pc == 5) {
            if (n->type == N_CALL && !js_is_function(f->v0)) {
                js_throw_error(ctx, JS_ERR_TYPE, "value is not a function");
                t->ct = CT_THROW; t->sp--; return 0;
            }
            f->cur = n->b;
            f->pc = 6;
        }
        for (;;) {
            if (f->pc == 7) {
                js_iter it;
                int done = 0;
                if (t->ct == CT_THROW) { t->sp--; return 0; }
                if (js_iter_open(ctx, t->res, &it) != JS_OK) {
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                while (!done) {
                    js_value iv;
                    if (js_step(ctx) != JS_OK ||
                        js_iter_next(ctx, &it, &iv, &done) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                    if (!done && async_frame_add_val(ctx, f, iv) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                }
                f->cur = f->cur->next;
                f->pc = 6;
            } else if (f->pc == 8) {
                if (t->ct == CT_THROW) { t->sp--; return 0; }
                if (async_frame_add_val(ctx, f, t->res) != JS_OK) {
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                f->cur = f->cur->next;
                f->pc = 6;
            }
            if (!f->cur) {
                int r;
                if (n->type == N_NEW)
                    r = js_construct(ctx, f->v0, f->nvals, f->vals, &t->res);
                else
                    r = js_call_func(ctx, f->v0.u.obj, f->v1, f->nvals, f->vals, &t->res);
                if (r != JS_OK) t->ct = CT_THROW;
                t->sp--;
                return 0;
            }
            if (f->cur->type == N_SPREAD) {
                f->pc = 7;
                if (async_push_frame(ctx, t, f->cur->a, f->env, 0, 0) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            f->pc = 8;
            if (async_push_frame(ctx, t, f->cur, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }

    case N_OPTCHAIN:
        if (f->pc == 0) {
            int cnt = opt_count(n->a), i = 0;
            f->chain = (js_node **)js_alloc(ctx, (unsigned long)cnt * sizeof(js_node *));
            if (!f->chain) { t->ct = CT_THROW; t->sp--; return 0; }
            opt_collect(n->a, f->chain, &i);
            f->nchain = cnt;
            f->chain_i = 1;
            f->v1 = js_undefined();
            f->pc = 1;
            if (async_push_frame(ctx, t, f->chain[0], f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        for (;;) {
            js_node *step;
            if (t->ct == CT_THROW) { t->sp--; return 0; }
            if (f->pc == 1) {
                f->v0 = t->res; /* base / current value */
                if (f->chain_i >= f->nchain) {
                    t->res = f->v0; t->sp--; return 0;
                }
                step = f->chain[f->chain_i];
                if ((step->flags & JS_NODE_OPT) &&
                    (f->v0.type == JS_UNDEFINED || f->v0.type == JS_NULL)) {
                    t->res = js_undefined(); t->sp--; return 0;
                }
                if (step->type == N_MEMBER) {
                    f->v1 = f->v0;
                    if (js_wrap_get(ctx, f->v0, step->str, &t->res) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                    f->chain_i++;
                    continue;
                }
                if (step->type == N_INDEX) {
                    f->v1 = f->v0;
                    f->pc = 2;
                    if (async_push_frame(ctx, t, step->b, f->env, 0, 0) != JS_OK)
                        t->ct = CT_THROW;
                    return 0;
                }
                /* N_CALL */
                if (!js_is_function(f->v0)) {
                    js_throw_error(ctx, JS_ERR_TYPE, "value is not a function");
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                f->nvals = 0;
                f->cur = step->b;
                f->pc = 3;
            }
            step = f->chain[f->chain_i];
            if (f->pc == 2) {
                if (js_get_value(ctx, f->v0, t->res, &t->res) != JS_OK) {
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                f->chain_i++;
                f->pc = 1;
                continue;
            }
            if (f->pc == 4) {
                js_iter it;
                int done = 0;
                if (js_iter_open(ctx, t->res, &it) != JS_OK) {
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                while (!done) {
                    js_value iv;
                    if (js_step(ctx) != JS_OK ||
                        js_iter_next(ctx, &it, &iv, &done) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                    if (!done && async_frame_add_val(ctx, f, iv) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                }
                f->cur = f->cur->next;
                f->pc = 3;
            } else if (f->pc == 5) {
                if (async_frame_add_val(ctx, f, t->res) != JS_OK) {
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                f->cur = f->cur->next;
                f->pc = 3;
            }
            if (!f->cur) {
                if (js_call_func(ctx, f->v0.u.obj, f->v1, f->nvals, f->vals,
                                 &t->res) != JS_OK) {
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                f->v1 = js_undefined();
                f->chain_i++;
                f->pc = 1;
                continue;
            }
            if (f->cur->type == N_SPREAD) {
                f->pc = 4;
                if (async_push_frame(ctx, t, f->cur->a, f->env, 0, 0) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            f->pc = 5;
            if (async_push_frame(ctx, t, f->cur, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }

    case N_ASSIGN:
        if (n->a->type == N_ARR_PAT || n->a->type == N_OBJ_PAT) {
            if (f->pc == 0) {
                f->pc = 1;
                if (async_push_frame(ctx, t, n->b, f->env, 0, 0) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            t->sp--;
            if (t->ct == CT_THROW) return 0;
            if (bind_pattern(ctx, n->a, t->res, f->env, BIND_ASSIGN) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        if (f->pc == 0) {
            if (n->a->type == N_IDENT) {
                f->pc = 3;
            } else if (n->a->type == N_MEMBER && n->a->a->type == N_SUPER) {
                f->pc = 3;
            } else {
                f->pc = 1;
                if (async_push_frame(ctx, t, n->a->a, f->env, 0, 0) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
        }
        if (f->pc == 1) {
            if (t->ct == CT_THROW) { t->sp--; return 0; }
            f->v0 = t->res;
            if (n->a->type == N_INDEX) {
                f->pc = 2;
                if (async_push_frame(ctx, t, n->a->b, f->env, 0, 0) != JS_OK)
                    t->ct = CT_THROW;
                return 0;
            }
            f->v1 = js_string_value(n->a->str);
            f->pc = 3;
        } else if (f->pc == 2) {
            if (t->ct == CT_THROW) { t->sp--; return 0; }
            f->v1 = t->res;
            f->pc = 3;
        }
        if (f->pc == 3) {
            if (n->op == TK_ANDAND_A || n->op == TK_OROR_A || n->op == TK_QQ_A) {
                jsref rf;
                js_value old;
                if (n->a->type == N_IDENT ||
                    (n->a->type == N_MEMBER && n->a->a->type == N_SUPER)) {
                    if (eval_ref(ctx, n->a, f->env, &rf) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                } else {
                    rf.kind = REF_PROP; rf.base = f->v0;
                    if (js_to_propkey(ctx, f->v1, &rf.key, &rf.is_sym) != JS_OK) {
                        t->ct = CT_THROW; t->sp--; return 0;
                    }
                }
                if (ref_get(ctx, &rf, &old) != JS_OK) {
                    t->ct = CT_THROW; t->sp--; return 0;
                }
                if ((n->op == TK_ANDAND_A && !js_to_boolean(old)) ||
                    (n->op == TK_OROR_A && js_to_boolean(old)) ||
                    (n->op == TK_QQ_A && old.type != JS_UNDEFINED && old.type != JS_NULL)) {
                    t->res = old; t->sp--; return 0;
                }
            }
            f->pc = 4;
            if (async_push_frame(ctx, t, n->b, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        /* pc == 4 */
        t->sp--;
        if (t->ct == CT_THROW) return 0;
        {
            jsref rf;
            js_value rhs = t->res, val = rhs;
            if (n->a->type == N_IDENT ||
                (n->a->type == N_MEMBER && n->a->a->type == N_SUPER)) {
                if (eval_ref(ctx, n->a, f->env, &rf) != JS_OK) {
                    t->ct = CT_THROW; return 0;
                }
            } else {
                rf.kind = REF_PROP; rf.base = f->v0;
                if (js_to_propkey(ctx, f->v1, &rf.key, &rf.is_sym) != JS_OK) {
                    t->ct = CT_THROW; return 0;
                }
            }
            if (n->op != TK_ASSIGN && n->op != TK_ANDAND_A &&
                n->op != TK_OROR_A && n->op != TK_QQ_A) {
                static const struct { int a; int b; } map[] = {
                    { TK_ADD_A, TK_ADD }, { TK_SUB_A, TK_SUB },
                    { TK_MUL_A, TK_MUL }, { TK_DIV_A, TK_DIV },
                    { TK_MOD_A, TK_MOD }, { TK_POW_A, TK_POW },
                    { TK_SHL_A, TK_SHL }, { TK_SHR_A, TK_SHR },
                    { TK_USHR_A, TK_USHR }, { TK_BAND_A, TK_BAND },
                    { TK_BOR_A, TK_BOR }, { TK_BXOR_A, TK_BXOR }
                };
                int i, op = 0;
                js_value old;
                for (i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++)
                    if (map[i].a == n->op) { op = map[i].b; break; }
                if (!op || ref_get(ctx, &rf, &old) != JS_OK ||
                    binop(ctx, op, old, rhs, &val) != JS_OK) {
                    t->ct = CT_THROW; return 0;
                }
            }
            if (ref_put(ctx, &rf, val) != JS_OK) {
                t->ct = CT_THROW; return 0;
            }
            t->res = val;
        }
        return 0;

    case N_CLASS:
        /* Evaluate extends expression if any, then run eval_class */
        if (f->pc == 0 && n->a) {
            f->pc = 1;
            if (async_push_frame(ctx, t, n->a, f->env, 0, 0) != JS_OK)
                t->ct = CT_THROW;
            return 0;
        }
        t->sp--;
        if (t->ct == CT_THROW) return 0;
        if (eval_class(ctx, n, f->env, &t->res) != JS_OK)
            t->ct = CT_THROW;
        return 0;

    default:
        t->sp--;
        js_throw_error(ctx, JS_ERR_SYNTAX, "unsupported async expression");
        t->ct = CT_THROW;
        return 0;
    }
}

static __attribute__((noinline)) int async_run(js_ctx *ctx, js_async_task *t)
{
    for (;;) {
        js_async_frame *f;
        int suspended;

        if (ctx->fatal)
            return JS_THROW;
        if (js_step(ctx) != JS_OK)
            return JS_THROW;

        /* Unwind completions across frames that aren't catching them */
        while (t->sp > 0 && t->ct != CT_NORMAL) {
            f = &t->frames[t->sp - 1];
            if (!f->is_stmt) {
                t->sp--;
                continue;
            }
            if (f->node->type == N_TRY && f->sub_state <= 2)
                break;
            if ((f->node->type == N_WHILE || f->node->type == N_DO ||
                 f->node->type == N_FOR || f->node->type == N_FORIN ||
                 f->node->type == N_FOROF || f->node->type == N_SWITCH ||
                 f->node->type == N_LABEL) &&
                (t->ct == CT_BREAK || t->ct == CT_CONTINUE))
                break;
            t->sp--;
        }

        if (t->sp == 0) {
            if (t->ct == CT_THROW) {
                js_value reason = js_exception(ctx);
                js_clear_exception(ctx);
                return js_promise_reject(ctx, js_object_value(t->promise), reason);
            }
            return js_promise_resolve(ctx, js_object_value(t->promise),
                                      t->ct == CT_RETURN ? t->cv : js_undefined());
        }

        f = &t->frames[t->sp - 1];
        suspended = f->is_stmt
            ? async_step_stmt(ctx, t, f)
            : async_step_expr(ctx, t, f);
        if (ctx->fatal)
            return JS_THROW;
        if (suspended)
            return JS_OK;
    }
}

int js_async_resume(js_ctx *ctx, js_async_task *task, int state, js_value val)
{
    if (!task)
        return JS_OK;
    if (state == 1) {
        task->res = val;
        task->ct = CT_NORMAL;
        task->lbl = 0;
    } else {
        js_throw(ctx, val);
        task->ct = CT_THROW;
        task->lbl = 0;
    }
    return async_run(ctx, task);
}

/* ------------------------------------------------------------------ */
/* Function invocation and construction                                */
/* ------------------------------------------------------------------ */

static int bind_params(js_ctx *ctx, js_func *fn, js_object *act,
                       js_env *env, int argc, js_value *argv)
{
    js_node *pn;
    int i;

    if (!fn->has_complex_params) {
        for (pn = fn->params, i = 0; pn; pn = pn->next, i++) {
            js_prop *p = js_add_prop(ctx, act, pn->str, JS_P_ENUM | JS_P_WRITE);
            if (!p) return JS_THROW;
            p->value = (i < argc) ? argv[i] : js_undefined();
        }
        return JS_OK;
    }

    for (pn = fn->params, i = 0; pn; pn = pn->next, i++) {
        if (pn->type == N_REST) {
            js_object *rest = js_new_array(ctx);
            int j;
            if (!rest) return JS_THROW;
            for (j = i; j < argc; j++)
                if (js_array_push(ctx, rest, argv[j]) != JS_OK)
                    return JS_THROW;
            if (bind_pattern(ctx, pn->a, js_object_value(rest), env, BIND_VAR) != JS_OK)
                return JS_THROW;
            break;
        }
        {
            js_value av = (i < argc) ? argv[i] : js_undefined();
            if (bind_pattern(ctx, pn, av, env, BIND_VAR) != JS_OK)
                return JS_THROW;
        }
    }
    return JS_OK;
}

static __attribute__((noinline)) int invoke_async(
    js_ctx *ctx, js_object *f, js_value this_val, int argc, js_value *argv,
    js_value *ret)
{
    js_func *fn = f->fn;
    js_value promise = js_promise_new(ctx);
    js_object *act;
    js_env *env, *body_env;
    js_node *q, *root;
    js_async_task *task;

    if (!js_is_promise(promise))
        return JS_THROW;
    *ret = promise;

    act = js_obj_alloc(ctx, JS_CLASS_OBJECT, 0);
    if (!act) goto fail_reject;
    env = js_env_new(ctx, fn->closure, act);
    if (!env) goto fail_reject;
    if (fn->ctor_kind == JS_CTOR_ARROW) {
        env->is_lex = 2;
        env->this_val = fn->closure ? fn->closure->this_val : js_undefined();
    } else {
        env->this_val = this_val;
    }
    env->home_obj = fn->home_obj;

    for (q = fn->vars; q; q = q->next) {
        js_prop *p = js_add_prop(ctx, act, q->str, JS_P_ENUM | JS_P_WRITE);
        if (!p) goto fail_reject;
    }
    if (bind_params(ctx, fn, act, env, argc, argv) != JS_OK)
        goto fail_reject;
    if (fn->ctor_kind != JS_CTOR_ARROW &&
        !js_own_prop(act, ctx->s_arguments) &&
        make_arguments(ctx, f, act, argc, argv) != JS_OK)
        goto fail_reject;
    body_env = env;
    if (fn->lex) {
        body_env = create_lex_env(ctx, env, fn->lex, 0);
        if (!body_env) goto fail_reject;
    }
    if (instantiate_funcs(ctx, fn->funcs, act, body_env) != JS_OK)
        goto fail_reject;

    task = (js_async_task *)js_alloc(ctx, sizeof(js_async_task));
    root = (js_node *)js_alloc(ctx, sizeof(js_node));
    if (!task || !root) goto fail_reject;
    task->promise = promise.u.obj;
    root->type = N_BLOCK;
    root->a = fn->body;
    root->flags = JS_NODE_HAS_AWAIT;
    if (async_push_frame(ctx, task, root, body_env, 0, 1) != JS_OK)
        goto fail_reject;
    return async_run(ctx, task);

fail_reject:
    if (ctx->fatal)
        return JS_THROW;
    {
        js_value reason = js_exception(ctx);
        js_clear_exception(ctx);
        return js_promise_reject(ctx, promise, reason);
    }
}

/* Create the activation record for a synchronous call and run the body. */
static int invoke(js_ctx *ctx, js_object *f, js_value this_val,
                  js_object *derived_new_target, int argc, js_value *argv,
                  js_value *ret)
{
    js_func *fn = f->fn;
    js_object *act;
    js_env *env, *body_env;
    js_node *q;
    js_value cv = js_undefined();
    js_string *lbl = 0;
    int r;

    if (fn->is_async)
        return invoke_async(ctx, f, this_val, argc, argv, ret);

    act = js_obj_alloc(ctx, JS_CLASS_OBJECT, 0);
    if (!act)
        return JS_THROW;
    env = js_env_new(ctx, fn->closure, act);
    if (!env)
        return JS_THROW;
    if (fn->ctor_kind == JS_CTOR_ARROW) {
        env->is_lex = 2;
        env->this_val = fn->closure ? fn->closure->this_val : js_undefined();
    } else if (fn->is_derived) {
        env->this_tdz = 1;
        env->this_val = js_object_value(derived_new_target ? derived_new_target : f);
        env->active_ctor = f;
    } else {
        env->this_val = this_val;
    }
    env->home_obj = fn->home_obj;

    for (q = fn->vars; q; q = q->next) {
        js_prop *p = js_add_prop(ctx, act, q->str, JS_P_ENUM | JS_P_WRITE);
        if (!p) return JS_THROW;
    }
    if (bind_params(ctx, fn, act, env, argc, argv) != JS_OK)
        return JS_THROW;
    if (fn->ctor_kind != JS_CTOR_ARROW &&
        !js_own_prop(act, ctx->s_arguments) &&
        make_arguments(ctx, f, act, argc, argv) != JS_OK)
        return JS_THROW;

    body_env = env;
    if (fn->lex) {
        body_env = create_lex_env(ctx, env, fn->lex, 0);
        if (!body_env)
            return JS_THROW;
    }
    if (instantiate_funcs(ctx, fn->funcs, act, body_env) != JS_OK)
        return JS_THROW;

    for (q = fn->body; q; q = q->next) {
        r = exec(ctx, q, body_env, &cv, &lbl);
        if (r == CT_RETURN) {
            if (fn->is_derived && cv.type != JS_OBJECT) {
                if (cv.type != JS_UNDEFINED)
                    return js_throw_error(ctx, JS_ERR_TYPE,
                                          "derived constructor may only return an object or undefined");
                if (env->this_tdz)
                    return js_throw_error(ctx, JS_ERR_REFERENCE,
                                          "must call super constructor before returning");
                *ret = env->this_val;
                return JS_OK;
            }
            *ret = cv;
            return JS_OK;
        }
        if (r == CT_THROW) return JS_THROW;
        if (r != CT_NORMAL) break;     /* stray break/continue: stop the body */
    }
    if (fn->is_derived) {
        if (env->this_tdz)
            return js_throw_error(ctx, JS_ERR_REFERENCE,
                                  "must call super constructor before returning from derived class");
        *ret = env->this_val;
        return JS_OK;
    }
    *ret = js_undefined();
    return JS_OK;
}

static int call_func_internal(js_ctx *ctx, js_object *f, js_value this_val,
                              js_object *derived_new_target, int is_new,
                              int argc, js_value *argv, js_value *ret)
{
    int r;

    *ret = js_undefined();
    if (ctx->fatal)
        return JS_THROW;
    if (!f->fn)
        return js_throw_error(ctx, JS_ERR_TYPE, "not a function");
    if (!is_new && f->fn->is_class_ctor)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "class constructor %s cannot be invoked without 'new'",
                              f->fn->name ? f->fn->name->data : "");
    if (ctx->call_depth >= ctx->cfg.max_call_depth)
        return js_throw_error(ctx, JS_ERR_RANGE,
                              "maximum call depth (%d) exceeded",
                              ctx->cfg.max_call_depth);
    if (too_deep(ctx))
        return JS_THROW;
    if (js_step(ctx) != JS_OK)
        return JS_THROW;

    if (f->fn->bound_target) {
        js_func *bf = f->fn;
        int total = bf->nbound + argc;
        js_value *all;
        int i;

        if (total > JS_MAXARGS)
            return js_throw_error(ctx, JS_ERR_RANGE,
                                  "more than %d call arguments", JS_MAXARGS);
        if (ctx->argsp + (uint32_t)total > JS_ARGSTACK)
            return js_throw_error(ctx, JS_ERR_RANGE, "argument stack exhausted");
        all = ctx->argstack + ctx->argsp;
        ctx->argsp += (uint32_t)total;
        for (i = 0; i < bf->nbound; i++)
            all[i] = bf->bound_args[i];
        for (i = 0; i < argc; i++)
            all[bf->nbound + i] = argv[i];
        r = call_func_internal(ctx, bf->bound_target, bf->bound_this,
                               derived_new_target, is_new, total, all, ret);
        ctx->argsp -= (uint32_t)total;
        return r;
    }

    ctx->call_depth++;
    ctx->depth++;
    if (f->fn->native) {
        r = f->fn->native(ctx, this_val, argc, argv, ret);
    } else {
        if (!f->fn->is_derived) {
            if (this_val.type == JS_UNDEFINED || this_val.type == JS_NULL)
                this_val = js_object_value(ctx->global);
            else if (this_val.type != JS_OBJECT) {
                js_value w;
                if (js_wrap_primitive(ctx, this_val, &w) == JS_OK)
                    this_val = w;
            }
        }
        r = invoke(ctx, f, this_val, derived_new_target, argc, argv, ret);
    }
    ctx->depth--;
    ctx->call_depth--;
    return r;
}

int js_call_func(js_ctx *ctx, js_object *f, js_value this_val,
                 int argc, js_value *argv, js_value *ret)
{
    return call_func_internal(ctx, f, this_val, 0, 0, argc, argv, ret);
}

int js_call(js_ctx *ctx, js_value fnv, js_value this_val,
            int argc, js_value *argv, js_value *ret)
{
    js_value dummy;

    if (!ret)
        ret = &dummy;
    *ret = js_undefined();
    if (!js_is_function(fnv))
        return js_throw_error(ctx, JS_ERR_TYPE, "value is not a function");
    return js_call_func(ctx, fnv.u.obj, this_val, argc, argv, ret);
}

int js_construct_with_target(js_ctx *ctx, js_value fnv, int argc, js_value *argv,
                             js_object *new_target, js_value *ret)
{
    js_object *f, *nt, *obj;
    js_value proto, rv;
    int r;

    *ret = js_undefined();
    if (!js_is_function(fnv))
        return js_throw_error(ctx, JS_ERR_TYPE, "value is not a constructor");
    f = fnv.u.obj;
    if (f->fn->ctor_kind >= JS_CTOR_NONE)
        return js_throw_error(ctx, JS_ERR_TYPE, "%s is not a constructor",
                              f->fn->name ? f->fn->name->data : "value");
    nt = new_target ? new_target : f;
    if (f->fn->bound_target) {
        js_func *bf = f->fn;
        int total = bf->nbound + argc;
        js_value *all;
        int i;

        if (total > JS_MAXARGS || ctx->argsp + (uint32_t)total > JS_ARGSTACK)
            return js_throw_error(ctx, JS_ERR_RANGE, "too many arguments");
        all = ctx->argstack + ctx->argsp;
        ctx->argsp += (uint32_t)total;
        for (i = 0; i < bf->nbound; i++)
            all[i] = bf->bound_args[i];
        for (i = 0; i < argc; i++)
            all[bf->nbound + i] = argv[i];
        r = js_construct_with_target(ctx, js_object_value(bf->bound_target),
                                     total, all, nt == f ? bf->bound_target : nt,
                                     ret);
        ctx->argsp -= (uint32_t)total;
        return r;
    }
    if (f->fn->native) {
        js_object *saved = ctx->new_target;
        ctx->new_target = nt;
        r = js_call_func(ctx, f, js_undefined(), argc, argv, ret);
        ctx->new_target = saved;
        if (r != JS_OK)
            return r;
        if (ret->type != JS_OBJECT)
            return js_throw_error(ctx, JS_ERR_TYPE,
                                  "constructor did not return an object");
        if (nt != f) {
            if (js_obj_get(ctx, nt, ctx->s_prototype, js_object_value(nt),
                           &proto) == JS_OK && proto.type == JS_OBJECT)
                ret->u.obj->proto = proto.u.obj;
        }
        return JS_OK;
    }
    if (js_obj_get(ctx, nt, ctx->s_prototype, js_object_value(nt), &proto) != JS_OK)
        return JS_THROW;
    if (f->fn->is_class_ctor == 2) {
        if (f->fn->is_derived) {
            if (!f->proto || !js_is_function(js_object_value(f->proto)))
                return js_throw_error(ctx, JS_ERR_TYPE,
                                      "super constructor is not a function");
            return js_construct_with_target(ctx, js_object_value(f->proto),
                                            argc, argv, nt, ret);
        }
        obj = js_obj_alloc(ctx, JS_CLASS_OBJECT,
                           proto.type == JS_OBJECT ? proto.u.obj : ctx->proto[P_OBJECT]);
        if (!obj) return JS_THROW;
        *ret = js_object_value(obj);
        return JS_OK;
    }
    if (f->fn->is_derived) {
        return call_func_internal(ctx, f, js_undefined(), nt, 1, argc, argv, ret);
    }
    obj = js_obj_alloc(ctx, JS_CLASS_OBJECT,
                       proto.type == JS_OBJECT ? proto.u.obj : ctx->proto[P_OBJECT]);
    if (!obj)
        return JS_THROW;
    r = call_func_internal(ctx, f, js_object_value(obj), nt, 1, argc, argv, &rv);
    if (r != JS_OK)
        return r;
    *ret = (rv.type == JS_OBJECT) ? rv : js_object_value(obj);
    return JS_OK;
}

int js_construct(js_ctx *ctx, js_value fnv, int argc, js_value *argv,
                 js_value *ret)
{
    return js_construct_with_target(ctx, fnv, argc, argv, 0, ret);
}

/* ------------------------------------------------------------------ */
/* Operators                                                           */
/* ------------------------------------------------------------------ */

static int str_lt(js_string *a, js_string *b)
{
    unsigned long n = a->len < b->len ? a->len : b->len;
    int c = memcmp(a->data, b->data, n);

    if (c != 0)
        return c < 0;
    return a->len < b->len;
}

static int compare(js_ctx *ctx, js_value x, js_value y, int *out)
{
    js_value px, py;

    if (js_to_primitive(ctx, x, JS_HINT_NUMBER, &px) != JS_OK) return JS_THROW;
    if (js_to_primitive(ctx, y, JS_HINT_NUMBER, &py) != JS_OK) return JS_THROW;
    if (px.type == JS_STRING && py.type == JS_STRING) {
        *out = str_lt(px.u.str, py.u.str);
        return JS_OK;
    }
    {
        double a, b;
        if (js_to_number(ctx, px, &a) != JS_OK) return JS_THROW;
        if (js_to_number(ctx, py, &b) != JS_OK) return JS_THROW;
        if (js_isnan(a) || js_isnan(b)) { *out = 2; return JS_OK; }
        *out = a < b;
        return JS_OK;
    }
}

static int add_values(js_ctx *ctx, js_value a, js_value b, js_value *out)
{
    js_value pa, pb;

    if (js_to_primitive(ctx, a, JS_HINT_NONE, &pa) != JS_OK) return JS_THROW;
    if (js_to_primitive(ctx, b, JS_HINT_NONE, &pb) != JS_OK) return JS_THROW;
    if (pa.type == JS_STRING || pb.type == JS_STRING) {
        js_value sa, sb;
        js_string *r;
        if (js_to_string(ctx, pa, &sa) != JS_OK) return JS_THROW;
        if (js_to_string(ctx, pb, &sb) != JS_OK) return JS_THROW;
        r = js_str_cat(ctx, sa.u.str, sb.u.str);
        if (!r) return JS_THROW;
        *out = js_string_value(r);
        return JS_OK;
    }
    {
        double x, y;
        if (js_to_number(ctx, pa, &x) != JS_OK) return JS_THROW;
        if (js_to_number(ctx, pb, &y) != JS_OK) return JS_THROW;
        *out = js_number(x + y);
        return JS_OK;
    }
}

static int binop(js_ctx *ctx, int op, js_value a, js_value b, js_value *out)
{
    double x, y;

    *out = js_undefined();
    switch (op) {
    case TK_ADD:
        return add_values(ctx, a, b, out);

    case TK_SUB: case TK_MUL: case TK_DIV: case TK_MOD: case TK_POW:
        if (js_to_number(ctx, a, &x) != JS_OK) return JS_THROW;
        if (js_to_number(ctx, b, &y) != JS_OK) return JS_THROW;
        switch (op) {
        case TK_SUB: *out = js_number(x - y); break;
        case TK_MUL: *out = js_number(x * y); break;
        case TK_DIV: *out = js_number(x / y); break;
        case TK_POW: *out = js_number(js_pow(x, y)); break;
        default:     *out = js_number(js_fmod(x, y)); break;
        }
        return JS_OK;

    case TK_LT: case TK_GT: case TK_LE: case TK_GE: {
        int c;
        if (op == TK_LT || op == TK_GE) {
            if (compare(ctx, a, b, &c) != JS_OK) return JS_THROW;
            if (c == 2) { *out = js_bool(0); return JS_OK; }
            *out = js_bool(op == TK_LT ? c : !c);
        } else {
            if (compare(ctx, b, a, &c) != JS_OK) return JS_THROW;
            if (c == 2) { *out = js_bool(0); return JS_OK; }
            *out = js_bool(op == TK_GT ? c : !c);
        }
        return JS_OK;
    }

    case TK_EQ: case TK_NE: {
        int e;
        if (js_loose_equals(ctx, a, b, &e) != JS_OK) return JS_THROW;
        *out = js_bool(op == TK_EQ ? e : !e);
        return JS_OK;
    }
    case TK_SEQ: *out = js_bool(js_strict_equals(a, b)); return JS_OK;
    case TK_SNE: *out = js_bool(!js_strict_equals(a, b)); return JS_OK;

    case TK_BAND: case TK_BOR: case TK_BXOR:
    case TK_SHL: case TK_SHR: {
        int32_t ia, ib;
        uint32_t sh;
        if (js_to_int32(ctx, a, &ia) != JS_OK) return JS_THROW;
        if (op == TK_SHL || op == TK_SHR) {
            if (js_to_uint32(ctx, b, &sh) != JS_OK) return JS_THROW;
            sh &= 31;
            *out = js_number(op == TK_SHL
                             ? (double)(int32_t)((uint32_t)ia << sh)
                             : (double)(ia >> sh));
            return JS_OK;
        }
        if (js_to_int32(ctx, b, &ib) != JS_OK) return JS_THROW;
        *out = js_number(op == TK_BAND ? (double)(ia & ib)
                       : op == TK_BOR  ? (double)(ia | ib)
                                       : (double)(ia ^ ib));
        return JS_OK;
    }
    case TK_USHR: {
        uint32_t ua, sh;
        if (js_to_uint32(ctx, a, &ua) != JS_OK) return JS_THROW;
        if (js_to_uint32(ctx, b, &sh) != JS_OK) return JS_THROW;
        *out = js_number((double)(ua >> (sh & 31)));
        return JS_OK;
    }

    case TK_INSTANCEOF: {
        js_value proto;
        js_object *p;
        int guard = 0;
        if (!js_is_function(b))
            return js_throw_error(ctx, JS_ERR_TYPE,
                                  "right operand of instanceof is not callable");
        if (js_obj_get(ctx, b.u.obj, ctx->s_prototype, b, &proto) != JS_OK)
            return JS_THROW;
        if (proto.type != JS_OBJECT)
            return js_throw_error(ctx, JS_ERR_TYPE,
                                  "prototype of the right operand is not an object");
        if (a.type != JS_OBJECT) { *out = js_bool(0); return JS_OK; }
        for (p = a.u.obj->proto; p && guard < 1000; p = p->proto, guard++)
            if (p == proto.u.obj) { *out = js_bool(1); return JS_OK; }
        *out = js_bool(0);
        return JS_OK;
    }

    case TK_IN: {
        js_string *k;
        int is_sym = 0;
        if (b.type != JS_OBJECT)
            return js_throw_error(ctx, JS_ERR_TYPE,
                                  "right operand of `in` is not an object");
        if (js_to_propkey(ctx, a, &k, &is_sym) != JS_OK) return JS_THROW;
        *out = js_bool(js_obj_has_key(ctx, b.u.obj, k, is_sym));
        return JS_OK;
    }
    default:
        return js_throw_error(ctx, JS_ERR_SYNTAX, "unsupported operator");
    }
}

/* ------------------------------------------------------------------ */
/* Expressions                                                         */
/* ------------------------------------------------------------------ */

static int collect_call_args(js_ctx *ctx, js_node *list, js_env *env,
                             uint32_t base, int *out_argc)
{
    js_node *a;
    int argc = 0;

    for (a = list; a; a = a->next) {
        if (a->type == N_SPREAD) {
            js_value sv;
            js_iter it;
            int done = 0;
            if (eval(ctx, a->a, env, &sv) != JS_OK ||
                js_iter_open(ctx, sv, &it) != JS_OK)
                goto fail;
            while (!done) {
                js_value iv;
                if (js_step(ctx) != JS_OK ||
                    js_iter_next(ctx, &it, &iv, &done) != JS_OK)
                    goto fail;
                if (!done) {
                    if (argc >= JS_MAXARGS) {
                        js_throw_error(ctx, JS_ERR_RANGE,
                                       "more than %d call arguments", JS_MAXARGS);
                        goto fail;
                    }
                    if (ctx->argsp >= JS_ARGSTACK) {
                        js_throw_error(ctx, JS_ERR_RANGE, "argument stack exhausted");
                        goto fail;
                    }
                    ctx->argstack[ctx->argsp++] = iv;
                    argc++;
                }
            }
        } else {
            js_value v;
            if (eval(ctx, a, env, &v) != JS_OK)
                goto fail;
            if (argc >= JS_MAXARGS) {
                js_throw_error(ctx, JS_ERR_RANGE,
                               "more than %d call arguments", JS_MAXARGS);
                goto fail;
            }
            if (ctx->argsp >= JS_ARGSTACK) {
                js_throw_error(ctx, JS_ERR_RANGE, "argument stack exhausted");
                goto fail;
            }
            ctx->argstack[ctx->argsp++] = v;
            argc++;
        }
    }
    *out_argc = argc;
    return JS_OK;

fail:
    ctx->argsp = base;
    return JS_THROW;
}

static __attribute__((noinline)) int eval_tagged_call(
    js_ctx *ctx, js_node *n, js_env *env, js_value fnv, js_value thisv,
    js_value *out)
{
    js_object *cooked = js_new_array(ctx);
    js_object *raw = js_new_array(ctx);
    js_node *q;
    uint32_t base = ctx->argsp;
    int idx = 0, argc = 1, r;

    if (!cooked || !raw) return JS_THROW;
    if (ctx->argsp >= JS_ARGSTACK)
        return js_throw_error(ctx, JS_ERR_RANGE, "argument stack exhausted");
    ctx->argstack[ctx->argsp++] = js_object_value(cooked);

    for (q = n->b->a; q; q = q->next, idx++) {
        if ((idx & 1) == 0) {
            js_value ck = (q->type == N_STR)
                ? js_string_value(q->str) : js_undefined();
            if (js_array_push(ctx, cooked, ck) != JS_OK ||
                js_array_push(ctx, raw, js_string_value(q->str2)) != JS_OK) {
                ctx->argsp = base;
                return JS_THROW;
            }
        } else {
            js_value ev;
            if (eval(ctx, q, env, &ev) != JS_OK) {
                ctx->argsp = base;
                return JS_THROW;
            }
            if (argc >= JS_MAXARGS || ctx->argsp >= JS_ARGSTACK) {
                ctx->argsp = base;
                return js_throw_error(ctx, JS_ERR_RANGE, "too many call arguments");
            }
            ctx->argstack[ctx->argsp++] = ev;
            argc++;
        }
    }
    raw->extensible = 0;
    if (js_define(ctx, cooked, "raw", js_object_value(raw)) != JS_OK) {
        ctx->argsp = base;
        return JS_THROW;
    }
    cooked->extensible = 0;
    r = js_call_func(ctx, fnv.u.obj, thisv, argc, ctx->argstack + base, out);
    ctx->argsp = base;
    return r;
}

static int eval_call(js_ctx *ctx, js_node *n, js_env *env, js_value *out)
{
    js_node *callee = n->a;
    js_value fnv, thisv = js_undefined();
    uint32_t base = ctx->argsp;
    int argc = 0, r;

    if (callee->type == N_SUPER) {
        js_env *ce = env_derived_ctor(env);
        js_object *nt, *super_ctor;
        if (!ce || !ce->active_ctor)
            return js_throw_error(ctx, JS_ERR_SYNTAX,
                                  "super() outside derived constructor");
        if (!ce->this_tdz)
            return js_throw_error(ctx, JS_ERR_REFERENCE,
                                  "super constructor may only be called once");
        nt = ce->this_val.u.obj;
        super_ctor = ce->active_ctor->proto;
        if (!super_ctor || !js_is_function(js_object_value(super_ctor)))
            return js_throw_error(ctx, JS_ERR_TYPE,
                                  "super constructor is not a constructor");
        if (collect_call_args(ctx, n->b, env, base, &argc) != JS_OK)
            return JS_THROW;
        r = js_construct_with_target(ctx, js_object_value(super_ctor),
                                     argc, ctx->argstack + base, nt, out);
        ctx->argsp = base;
        if (r == JS_OK) {
            ce->this_val = *out;
            ce->this_tdz = 0;
        }
        return r;
    }

    if (callee->type == N_MEMBER || callee->type == N_INDEX) {
        jsref r2;
        if (eval_ref(ctx, callee, env, &r2) != JS_OK) return JS_THROW;
        thisv = r2.base;
        if (ref_get(ctx, &r2, &fnv) != JS_OK) return JS_THROW;
        if (!js_is_function(fnv))
            return js_throw_error(ctx, JS_ERR_TYPE, "%s is not a function",
                                  r2.is_sym ? "Symbol" : r2.key->data);
    } else if (callee->type == N_IDENT) {
        js_object *scope = resolve(ctx, env, callee->str);
        js_env *e;
        if (!scope)
            return js_throw_error(ctx, JS_ERR_REFERENCE, "%s is not defined",
                                  callee->str->data);
        if (check_scope_tdz(ctx, scope, callee->str) != JS_OK ||
            js_obj_get(ctx, scope, callee->str, js_object_value(scope), &fnv) != JS_OK)
            return JS_THROW;
        for (e = env; e; e = e->parent)
            if (e->is_with && e->vars == scope) {
                thisv = js_object_value(scope);
                break;
            }
        if (!js_is_function(fnv))
            return js_throw_error(ctx, JS_ERR_TYPE, "%s is not a function",
                                  callee->str->data);
    } else {
        if (eval(ctx, callee, env, &fnv) != JS_OK) return JS_THROW;
        if (!js_is_function(fnv))
            return js_throw_error(ctx, JS_ERR_TYPE, "value is not a function");
    }

    if (n->flags & JS_NODE_TAGGED)
        return eval_tagged_call(ctx, n, env, fnv, thisv, out);

    if (collect_call_args(ctx, n->b, env, base, &argc) != JS_OK)
        return JS_THROW;
    r = js_call_func(ctx, fnv.u.obj, thisv, argc, ctx->argstack + base, out);
    ctx->argsp = base;
    return r;
}

static int eval_new(js_ctx *ctx, js_node *n, js_env *env, js_value *out)
{
    js_value fnv;
    uint32_t base = ctx->argsp;
    int argc = 0, r;

    if (eval(ctx, n->a, env, &fnv) != JS_OK)
        return JS_THROW;
    if (collect_call_args(ctx, n->b, env, base, &argc) != JS_OK)
        return JS_THROW;
    r = js_construct(ctx, fnv, argc, ctx->argstack + base, out);
    ctx->argsp = base;
    return r;
}

static __attribute__((noinline)) int eval_opt_step(
    js_ctx *ctx, js_node *n, js_env *env, js_value *out,
    int *short_circuit, js_value *out_this)
{
    js_value base, k;
    js_value dummy_this = js_undefined();

    *short_circuit = 0;
    *out_this = js_undefined();
    *out = js_undefined();

    if (n->type != N_MEMBER && n->type != N_INDEX && n->type != N_CALL) {
        return eval(ctx, n, env, out);
    }

    if (n->type == N_CALL && n->a->type == N_IDENT) {
        /* Plain ident?.(...) */
        js_object *scope = resolve(ctx, env, n->a->str);
        js_env *e;
        if (!scope)
            return js_throw_error(ctx, JS_ERR_REFERENCE, "%s is not defined",
                                  n->a->str->data);
        if (check_scope_tdz(ctx, scope, n->a->str) != JS_OK ||
            js_obj_get(ctx, scope, n->a->str, js_object_value(scope), &base) != JS_OK)
            return JS_THROW;
        for (e = env; e; e = e->parent)
            if (e->is_with && e->vars == scope) {
                dummy_this = js_object_value(scope);
                break;
            }
    } else {
        if (eval_opt_step(ctx, n->a, env, &base, short_circuit, &dummy_this) != JS_OK)
            return JS_THROW;
        if (*short_circuit)
            return JS_OK;
    }

    if ((n->flags & JS_NODE_OPT) &&
        (base.type == JS_UNDEFINED || base.type == JS_NULL)) {
        *short_circuit = 1;
        *out = js_undefined();
        return JS_OK;
    }

    if (n->type == N_MEMBER) {
        *out_this = base;
        return js_wrap_get(ctx, base, n->str, out);
    }
    if (n->type == N_INDEX) {
        js_string *ks;
        int is_sym = 0;
        *out_this = base;
        if (eval(ctx, n->b, env, &k) != JS_OK ||
            js_to_propkey(ctx, k, &ks, &is_sym) != JS_OK)
            return JS_THROW;
        return js_wrap_get_key(ctx, base, ks, is_sym, out);
    }

    /* N_CALL */
    if (!js_is_function(base))
        return js_throw_error(ctx, JS_ERR_TYPE, "value is not a function");
    {
        uint32_t abase = ctx->argsp;
        int argc = 0, r;
        if (collect_call_args(ctx, n->b, env, abase, &argc) != JS_OK)
            return JS_THROW;
        r = js_call_func(ctx, base.u.obj, dummy_this, argc,
                         ctx->argstack + abase, out);
        ctx->argsp = abase;
        return r;
    }
}

static __attribute__((noinline)) int eval_optchain(
    js_ctx *ctx, js_node *n, js_env *env, js_value *out)
{
    int sc = 0;
    js_value tv;

    return eval_opt_step(ctx, n->a, env, out, &sc, &tv);
}

static __attribute__((noinline)) int eval_opt_delete(
    js_ctx *ctx, js_node *n, js_env *env, js_value *out)
{
    js_node *leaf = n->a;
    js_value base, tv;
    int sc = 0;

    if (leaf->type != N_MEMBER && leaf->type != N_INDEX) {
        if (eval_opt_step(ctx, leaf, env, &base, &sc, &tv) != JS_OK)
            return JS_THROW;
        *out = js_bool(1);
        return JS_OK;
    }
    if (eval_opt_step(ctx, leaf->a, env, &base, &sc, &tv) != JS_OK)
        return JS_THROW;
    if (sc || ((leaf->flags & JS_NODE_OPT) &&
               (base.type == JS_UNDEFINED || base.type == JS_NULL))) {
        *out = js_bool(1);
        return JS_OK;
    }
    if (base.type == JS_UNDEFINED || base.type == JS_NULL)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "cannot delete a property of %s",
                              base.type == JS_NULL ? "null" : "undefined");
    if (leaf->type == N_MEMBER) {
        *out = js_bool(base.type != JS_OBJECT ? 1
                       : js_obj_delete(ctx, base.u.obj, leaf->str));
        return JS_OK;
    }
    {
        js_value k;
        js_string *ks;
        int is_sym = 0;
        if (eval(ctx, leaf->b, env, &k) != JS_OK ||
            js_to_propkey(ctx, k, &ks, &is_sym) != JS_OK)
            return JS_THROW;
        *out = js_bool(base.type != JS_OBJECT ? 1
                       : js_obj_delete_key(ctx, base.u.obj, ks, is_sym));
        return JS_OK;
    }
}

static int eval_func(js_ctx *ctx, js_node *n, js_env *env, js_value *out)
{
    js_func *fn = (js_func *)js_alloc(ctx, sizeof(js_func));
    js_object *fo;
    js_env *fenv = env;

    if (!fn)
        return JS_THROW;
    init_func_from_node(fn, n);

    /* Named function expression (not method) sees its own name inside body. */
    if (n->str && n->op == 0) {
        js_object *scope = js_obj_alloc(ctx, JS_CLASS_OBJECT, 0);
        js_prop *p;
        if (!scope) return JS_THROW;
        fenv = js_env_new(ctx, env, scope);
        if (!fenv) return JS_THROW;
        fenv->is_lex = 1;
        fenv->this_val = env->this_val;
        fo = js_make_function(ctx, fn, fenv);
        if (!fo) return JS_THROW;
        p = js_add_prop(ctx, scope, n->str, JS_P_ENUM);
        if (!p) return JS_THROW;
        p->value = js_object_value(fo);
    } else {
        fo = js_make_function(ctx, fn, fenv);
        if (!fo) return JS_THROW;
    }
    *out = js_object_value(fo);
    return JS_OK;
}

static __attribute__((noinline)) int eval_class(
    js_ctx *ctx, js_node *n, js_env *env, js_value *out)
{
    js_object *super_ctor = 0, *super_proto = ctx->proto[P_OBJECT];
    js_object *proto, *cls;
    js_env *class_env = env;
    js_func *cfn;
    js_prop *p;
    js_node *m;

    if (n->a) {
        js_value sv;
        if (eval(ctx, n->a, env, &sv) != JS_OK)
            return JS_THROW;
        if (sv.type == JS_NULL) {
            super_ctor = 0;
            super_proto = 0;
        } else if (js_is_function(sv) && sv.u.obj->fn->ctor_kind < JS_CTOR_NONE) {
            js_value bp;
            super_ctor = sv.u.obj;
            if (js_obj_get(ctx, super_ctor, ctx->s_prototype, sv, &bp) != JS_OK)
                return JS_THROW;
            if (bp.type == JS_OBJECT)
                super_proto = bp.u.obj;
            else if (bp.type == JS_NULL)
                super_proto = 0;
            else
                return js_throw_error(ctx, JS_ERR_TYPE,
                                      "class extends value prototype is not an object or null");
        } else {
            return js_throw_error(ctx, JS_ERR_TYPE,
                                  "class extends value is not a constructor or null");
        }
    }

    proto = js_new_object_proto(ctx, super_proto);
    if (!proto)
        return JS_THROW;

    if (n->str) {
        js_object *scope = js_obj_alloc(ctx, JS_CLASS_OBJECT, 0);
        if (!scope) return JS_THROW;
        class_env = js_env_new(ctx, env, scope);
        if (!class_env) return JS_THROW;
        class_env->is_lex = 1;
        class_env->this_val = env->this_val;
    }

    cfn = (js_func *)js_alloc(ctx, sizeof(js_func));
    if (!cfn)
        return JS_THROW;
    if (n->b) {
        init_func_from_node(cfn, n->b);
        cfn->ctor_kind = JS_CTOR_NORMAL;
        cfn->is_class_ctor = 1;
    } else {
        cfn->name = n->str ? n->str : ctx->s_empty;
        cfn->ctor_kind = JS_CTOR_NORMAL;
        cfn->is_class_ctor = 2;
    }
    cfn->is_derived = (n->a != 0) ? 1 : 0;
    cfn->home_obj = proto;

    cls = js_obj_alloc(ctx, JS_CLASS_FUNCTION,
                       super_ctor ? super_ctor : ctx->proto[P_FUNCTION]);
    if (!cls)
        return JS_THROW;
    cls->fn = cfn;
    cfn->closure = class_env;

    p = js_add_prop(ctx, cls, ctx->s_length, 0);
    if (!p) return JS_THROW;
    p->value = js_number((double)cfn->nparams);
    p = js_add_prop(ctx, cls, ctx->s_name, 0);
    if (!p) return JS_THROW;
    p->value = js_string_value(cfn->name ? cfn->name : ctx->s_empty);
    p = js_add_prop(ctx, cls, ctx->s_prototype, 0);
    if (!p) return JS_THROW;
    p->value = js_object_value(proto);
    p = js_add_prop(ctx, proto, ctx->s_constructor, JS_P_HIDDEN);
    if (!p) return JS_THROW;
    p->value = js_object_value(cls);

    if (n->str) {
        p = js_add_prop(ctx, class_env->vars, n->str, JS_P_ENUM | JS_P_CONST);
        if (!p) return JS_THROW;
        p->value = js_object_value(cls);
    }

    for (m = n->c; m; m = m->next) {
        js_object *tgt = (m->flags & JS_NODE_STATIC) ? cls : proto;
        js_string *k;
        int is_sym = 0;
        js_func *mfn;
        js_object *mo;
        js_prop *mp;
        uint32_t fl;

        if (m->b) {
            js_value kv;
            if (eval(ctx, m->b, env, &kv) != JS_OK ||
                js_to_propkey(ctx, kv, &k, &is_sym) != JS_OK)
                return JS_THROW;
        } else {
            k = m->str;
        }
        mfn = (js_func *)js_alloc(ctx, sizeof(js_func));
        if (!mfn) return JS_THROW;
        init_func_from_node(mfn, m->a);
        mfn->ctor_kind = JS_CTOR_NONE;
        mfn->home_obj = tgt;
        if (!mfn->name) mfn->name = k;
        mo = js_make_function(ctx, mfn, class_env);
        if (!mo) return JS_THROW;

        if (m->op) {
            fl = JS_P_ACCESSOR | JS_P_CONFIG |
                 (is_sym ? (uint32_t)JS_P_SYMBOL : 0u);
            mp = js_own_prop_key(tgt, k, is_sym);
            if (!mp || !(mp->flags & JS_P_ACCESSOR)) {
                mp = js_add_prop_key(ctx, tgt, k, fl);
                if (!mp) return JS_THROW;
                mp->flags = fl;
                mp->value = js_undefined();
                mp->setter = js_undefined();
            }
            if (m->op == 1) mp->value = js_object_value(mo);
            else mp->setter = js_object_value(mo);
        } else {
            fl = JS_P_HIDDEN | (is_sym ? (uint32_t)JS_P_SYMBOL : 0u);
            mp = js_add_prop_key(ctx, tgt, k, fl);
            if (!mp) return JS_THROW;
            mp->flags = fl;
            mp->setter = js_undefined();
            mp->value = js_object_value(mo);
        }
    }

    if ((n->flags & JS_NODE_CLASS_DECL) && n->str) {
        if (init_lex_var(ctx, env, n->str, js_object_value(cls)) != JS_OK)
            return JS_THROW;
    }
    *out = js_object_value(cls);
    return JS_OK;
}

static __attribute__((noinline)) int eval_object(
    js_ctx *ctx, js_node *n, js_env *env, js_value *out)
{
    js_object *o = js_new_object(ctx);
    js_node *q;

    if (!o)
        return JS_THROW;
    for (q = n->a; q; q = q->next) {
        js_prop *p;
        js_string *k;
        int is_sym = 0;

        if (q->op == 3) {
            js_value sv, vo;
            if (eval(ctx, q->a, env, &sv) != JS_OK)
                return JS_THROW;
            if (sv.type == JS_UNDEFINED || sv.type == JS_NULL)
                continue;
            if (js_to_object(ctx, sv, &vo) != JS_OK ||
                copy_own_enum_props(ctx, o, vo.u.obj, 0) != JS_OK)
                return JS_THROW;
            continue;
        }
        if (q->b) {
            js_value kv;
            if (eval(ctx, q->b, env, &kv) != JS_OK ||
                js_to_propkey(ctx, kv, &k, &is_sym) != JS_OK)
                return JS_THROW;
        } else {
            k = q->str;
        }
        if (q->op == 1 || q->op == 2) {
            js_value f;
            uint32_t fl = JS_P_ACCESSOR | JS_P_ENUM | JS_P_CONFIG |
                          (is_sym ? (uint32_t)JS_P_SYMBOL : 0u);
            if (eval_func(ctx, q->a, env, &f) != JS_OK) return JS_THROW;
            f.u.obj->fn->home_obj = o;
            p = js_own_prop_key(o, k, is_sym);
            if (!p || !(p->flags & JS_P_ACCESSOR)) {
                p = js_add_prop_key(ctx, o, k, fl);
                if (!p) return JS_THROW;
                p->flags = fl;
                p->value = js_undefined();
                p->setter = js_undefined();
            }
            if (q->op == 1) p->value = f;
            else p->setter = f;
            continue;
        }
        {
            js_value v;
            uint32_t fl = JS_P_DEFAULT | (is_sym ? (uint32_t)JS_P_SYMBOL : 0u);
            if (eval(ctx, q->a, env, &v) != JS_OK) return JS_THROW;
            if (q->a->type == N_FUNC && q->a->op == 1 && v.type == JS_OBJECT)
                v.u.obj->fn->home_obj = o;
            p = js_own_prop_key(o, k, is_sym);
            if (p && (p->flags & JS_P_ACCESSOR)) {
                p->flags = fl;
                p->setter = js_undefined();
            }
            if (!p) {
                p = js_add_prop_key(ctx, o, k, fl);
                if (!p) return JS_THROW;
                p->flags = fl;
            }
            p->value = v;
        }
    }
    *out = js_object_value(o);
    return JS_OK;
}

static int eval_array(js_ctx *ctx, js_node *n, js_env *env, js_value *out)
{
    js_object *a = js_new_array(ctx);
    js_node *q;
    int trailing_hole = 0;

    if (!a)
        return JS_THROW;
    for (q = n->a; q; q = q->next) {
        js_value v = js_undefined();
        if (q->type == N_SPREAD) {
            if (eval(ctx, q->a, env, &v) != JS_OK ||
                spread_array_iter(ctx, a, v) != JS_OK)
                return JS_THROW;
            trailing_hole = 0;
            continue;
        }
        if (q->type != N_EMPTY) {
            if (eval(ctx, q, env, &v) != JS_OK) return JS_THROW;
            trailing_hole = 0;
        } else {
            trailing_hole = 1;
        }
        if (js_array_push(ctx, a, v) != JS_OK) return JS_THROW;
    }
    if (trailing_hole && a->elen)
        a->elen--;
    *out = js_object_value(a);
    return JS_OK;
}

static __attribute__((noinline)) int eval_tpl(
    js_ctx *ctx, js_node *n, js_env *env, js_value *out)
{
    js_sbuf b;
    js_node *q;
    int idx = 0;

    js_sb_init(&b, ctx);
    for (q = n->a; q; q = q->next, idx++) {
        if ((idx & 1) == 0) {
            if (q->str && js_sb_put(&b, q->str->data, q->str->len) != JS_OK)
                goto fail;
        } else {
            js_value v, s;
            if (eval(ctx, q, env, &v) != JS_OK ||
                js_to_string(ctx, v, &s) != JS_OK ||
                js_sb_put(&b, s.u.str->data, s.u.str->len) != JS_OK)
                goto fail;
        }
    }
    return js_sb_finish(&b, out);
fail:
    js_sb_free(&b);
    return JS_THROW;
}

static int eval_unary(js_ctx *ctx, js_node *n, js_env *env, js_value *out)
{
    js_value v;
    double d;

    switch (n->op) {
    case TK_TYPEOF:
        if (n->a->type == N_IDENT) {
            js_object *scope = resolve(ctx, env, n->a->str);
            if (!scope) { *out = js_mkcstring(ctx, "undefined"); return JS_OK; }
            if (check_scope_tdz(ctx, scope, n->a->str) != JS_OK ||
                js_obj_get(ctx, scope, n->a->str, js_object_value(scope), &v) != JS_OK)
                return JS_THROW;
        } else if (eval(ctx, n->a, env, &v) != JS_OK) {
            return JS_THROW;
        }
        *out = js_mkcstring(ctx, js_typeof(v));
        return JS_OK;

    case TK_DELETE:
        if (n->a->type == N_OPTCHAIN)
            return eval_opt_delete(ctx, n->a, env, out);
        if (n->a->type == N_IDENT) {
            js_object *scope = resolve(ctx, env, n->a->str);
            *out = js_bool(scope ? js_obj_delete(ctx, scope, n->a->str) : 1);
            return JS_OK;
        }
        if (n->a->type == N_MEMBER || n->a->type == N_INDEX) {
            jsref r;
            if (eval_ref(ctx, n->a, env, &r) != JS_OK) return JS_THROW;
            if (r.kind == REF_SUPER)
                return js_throw_error(ctx, JS_ERR_REFERENCE,
                                      "unsupported reference to 'super'");
            if (r.base.type == JS_UNDEFINED || r.base.type == JS_NULL)
                return js_throw_error(ctx, JS_ERR_TYPE,
                                      "cannot delete a property of %s",
                                      r.base.type == JS_NULL ? "null" : "undefined");
            *out = js_bool(r.base.type != JS_OBJECT ? 1
                           : js_obj_delete_key(ctx, r.base.u.obj, r.key, r.is_sym));
            return JS_OK;
        }
        if (eval(ctx, n->a, env, &v) != JS_OK) return JS_THROW;
        *out = js_bool(1);
        return JS_OK;

    case TK_VOID:
        if (eval(ctx, n->a, env, &v) != JS_OK) return JS_THROW;
        *out = js_undefined();
        return JS_OK;

    default:
        break;
    }

    if (eval(ctx, n->a, env, &v) != JS_OK)
        return JS_THROW;
    switch (n->op) {
    case TK_ADD:
        if (js_to_number(ctx, v, &d) != JS_OK) return JS_THROW;
        *out = js_number(d);
        return JS_OK;
    case TK_SUB:
        if (js_to_number(ctx, v, &d) != JS_OK) return JS_THROW;
        *out = js_number(-d);
        return JS_OK;
    case TK_BNOT: {
        int32_t i;
        if (js_to_int32(ctx, v, &i) != JS_OK) return JS_THROW;
        *out = js_number((double)(~i));
        return JS_OK;
    }
    case TK_NOT:
        *out = js_bool(!js_to_boolean(v));
        return JS_OK;
    default:
        return js_throw_error(ctx, JS_ERR_SYNTAX, "unsupported unary operator");
    }
}

static int eval_inner(js_ctx *ctx, js_node *n, js_env *env, js_value *out)
{
    *out = js_undefined();

    switch (n->type) {
    case N_NUM:  *out = js_number(n->num); return JS_OK;
    case N_STR:  *out = js_string_value(n->str); return JS_OK;
    case N_TPL:  return eval_tpl(ctx, n, env, out);
    case N_BOOL: *out = js_bool(n->num != 0); return JS_OK;
    case N_NULL: *out = js_null(); return JS_OK;
    case N_THIS: return env_get_this(ctx, env, out);

    case N_IDENT: {
        js_object *scope = resolve(ctx, env, n->str);
        if (!scope)
            return js_throw_error(ctx, JS_ERR_REFERENCE, "%s is not defined",
                                  n->str->data);
        if (check_scope_tdz(ctx, scope, n->str) != JS_OK)
            return JS_THROW;
        return js_obj_get(ctx, scope, n->str, js_object_value(scope), out);
    }

    case N_REGEX: {
        js_regexp *re = js_regexp_compile(ctx, n->str, n->str2);
        js_object *o;
        if (!re) return JS_THROW;
        o = js_obj_alloc(ctx, JS_CLASS_REGEXP, ctx->proto[P_REGEXP]);
        if (!o) return JS_THROW;
        o->re = re;
        if (js_define(ctx, o, "lastIndex", js_number(0)) != JS_OK) return JS_THROW;
        *out = js_object_value(o);
        return JS_OK;
    }

    case N_ARRAY:    return eval_array(ctx, n, env, out);
    case N_OBJECT:   return eval_object(ctx, n, env, out);
    case N_FUNC:     return eval_func(ctx, n, env, out);
    case N_CLASS:    return eval_class(ctx, n, env, out);
    case N_CALL:     return eval_call(ctx, n, env, out);
    case N_NEW:      return eval_new(ctx, n, env, out);
    case N_OPTCHAIN: return eval_optchain(ctx, n, env, out);

    case N_MEMBER:
    case N_INDEX: {
        jsref r;
        if (eval_ref(ctx, n, env, &r) != JS_OK) return JS_THROW;
        return ref_get(ctx, &r, out);
    }

    case N_UNARY: return eval_unary(ctx, n, env, out);

    case N_PREFIX:
    case N_POSTFIX: {
        jsref r;
        js_value old;
        double d;
        if (eval_ref(ctx, n->a, env, &r) != JS_OK) return JS_THROW;
        if (ref_get(ctx, &r, &old) != JS_OK) return JS_THROW;
        if (js_to_number(ctx, old, &d) != JS_OK) return JS_THROW;
        {
            double nv = (n->op == TK_INC) ? d + 1 : d - 1;
            if (ref_put(ctx, &r, js_number(nv)) != JS_OK) return JS_THROW;
            *out = js_number(n->type == N_PREFIX ? nv : d);
        }
        return JS_OK;
    }

    case N_BINARY: {
        js_value a, b;
        if (eval(ctx, n->a, env, &a) != JS_OK) return JS_THROW;
        if (eval(ctx, n->b, env, &b) != JS_OK) return JS_THROW;
        return binop(ctx, n->op, a, b, out);
    }

    case N_LOGICAL: {
        js_value a;
        if (eval(ctx, n->a, env, &a) != JS_OK) return JS_THROW;
        if (n->op == TK_ANDAND) {
            if (!js_to_boolean(a)) { *out = a; return JS_OK; }
        } else if (n->op == TK_QQ) {
            if (a.type != JS_UNDEFINED && a.type != JS_NULL) { *out = a; return JS_OK; }
        } else {
            if (js_to_boolean(a)) { *out = a; return JS_OK; }
        }
        return eval(ctx, n->b, env, out);
    }

    case N_COND: {
        js_value t;
        if (eval(ctx, n->a, env, &t) != JS_OK) return JS_THROW;
        return eval(ctx, js_to_boolean(t) ? n->b : n->c, env, out);
    }

    case N_SEQ: {
        js_value t;
        if (eval(ctx, n->a, env, &t) != JS_OK) return JS_THROW;
        return eval(ctx, n->b, env, out);
    }

    case N_ASSIGN: {
        jsref r;
        js_value v;
        if (n->a->type == N_ARR_PAT || n->a->type == N_OBJ_PAT) {
            if (eval(ctx, n->b, env, &v) != JS_OK) return JS_THROW;
            if (bind_pattern(ctx, n->a, v, env, BIND_ASSIGN) != JS_OK)
                return JS_THROW;
            *out = v;
            return JS_OK;
        }
        if (eval_ref(ctx, n->a, env, &r) != JS_OK) return JS_THROW;
        if (n->op == TK_ASSIGN) {
            if (eval(ctx, n->b, env, &v) != JS_OK) return JS_THROW;
        } else if (n->op == TK_ANDAND_A || n->op == TK_OROR_A || n->op == TK_QQ_A) {
            js_value old;
            if (ref_get(ctx, &r, &old) != JS_OK) return JS_THROW;
            if ((n->op == TK_ANDAND_A && !js_to_boolean(old)) ||
                (n->op == TK_OROR_A && js_to_boolean(old)) ||
                (n->op == TK_QQ_A && old.type != JS_UNDEFINED && old.type != JS_NULL)) {
                *out = old;
                return JS_OK;
            }
            if (eval(ctx, n->b, env, &v) != JS_OK) return JS_THROW;
        } else {
            js_value old, rhs;
            static const struct { int a; int b; } map[] = {
                { TK_ADD_A, TK_ADD }, { TK_SUB_A, TK_SUB },
                { TK_MUL_A, TK_MUL }, { TK_DIV_A, TK_DIV },
                { TK_MOD_A, TK_MOD }, { TK_POW_A, TK_POW },
                { TK_SHL_A, TK_SHL }, { TK_SHR_A, TK_SHR },
                { TK_USHR_A, TK_USHR }, { TK_BAND_A, TK_BAND },
                { TK_BOR_A, TK_BOR }, { TK_BXOR_A, TK_BXOR }
            };
            int i, op = 0;
            for (i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++)
                if (map[i].a == n->op) { op = map[i].b; break; }
            if (!op)
                return js_throw_error(ctx, JS_ERR_SYNTAX,
                                      "unsupported assignment operator");
            if (ref_get(ctx, &r, &old) != JS_OK) return JS_THROW;
            if (eval(ctx, n->b, env, &rhs) != JS_OK) return JS_THROW;
            if (binop(ctx, op, old, rhs, &v) != JS_OK) return JS_THROW;
        }
        if (ref_put(ctx, &r, v) != JS_OK) return JS_THROW;
        *out = v;
        return JS_OK;
    }

    default:
        return js_throw_error(ctx, JS_ERR_SYNTAX, "unsupported expression");
    }
}

static int eval(js_ctx *ctx, js_node *n, js_env *env, js_value *out)
{
    int r;

    if (js_step(ctx) != JS_OK) { *out = js_undefined(); return JS_THROW; }
    if (too_deep(ctx)) { *out = js_undefined(); return JS_THROW; }
    ctx->depth++;
    r = eval_inner(ctx, n, env, out);
    ctx->depth--;
    return r;
}

/* ------------------------------------------------------------------ */
/* for-in enumeration                                                  */
/* ------------------------------------------------------------------ */

#define JS_MAX_ENUM 65536

static js_string **enum_keys(js_ctx *ctx, js_object *o, int *count)
{
    js_object *level;
    int n = 0, cap = 16, depth = 0;
    js_string **keys = (js_string **)js_alloc(ctx, (unsigned long)cap * sizeof(js_string *));

    if (!keys)
        return 0;
    for (level = o; level && depth < 64; level = level->proto, depth++) {
        uint32_t i;
        js_string *k;

        if (level->cls == JS_CLASS_ARRAY || js_is_typedarray_cls(level->cls)) {
            uint32_t elen = level->cls == JS_CLASS_ARRAY ? level->elen : level->view_length;
            for (i = 0; i < elen && n < JS_MAX_ENUM; i++) {
                js_object *q;
                int shadowed = 0;
                k = index_key(ctx, i);
                if (!k) return 0;
                for (q = o; q != level; q = q->proto)
                    if (js_obj_has_own(ctx, q, k)) { shadowed = 1; break; }
                if (shadowed) continue;
                if (n == cap) {
                    js_string **nk = (js_string **)js_alloc(ctx,
                        (unsigned long)cap * 2 * sizeof(js_string *));
                    if (!nk) return 0;
                    memcpy(nk, keys, (unsigned long)n * sizeof(js_string *));
                    keys = nk;
                    cap *= 2;
                }
                keys[n++] = k;
            }
        }
        for (i = 0; i < level->nprops && n < JS_MAX_ENUM; i++) {
            js_prop *p = &level->props[i];
            js_object *q;
            int shadowed = 0;
            if ((p->flags & (JS_P_DEAD | JS_P_SYMBOL)) || !(p->flags & JS_P_ENUM))
                continue;
            for (q = o; q != level; q = q->proto)
                if (js_obj_has_own(ctx, q, p->key)) { shadowed = 1; break; }
            if (shadowed) continue;
            if (n == cap) {
                js_string **nk = (js_string **)js_alloc(ctx,
                    (unsigned long)cap * 2 * sizeof(js_string *));
                if (!nk) return 0;
                memcpy(nk, keys, (unsigned long)n * sizeof(js_string *));
                keys = nk;
                cap *= 2;
            }
            keys[n++] = p->key;
        }
    }
    *count = n;
    return keys;
}

/* ------------------------------------------------------------------ */
/* Statements                                                          */
/* ------------------------------------------------------------------ */

static int exec_list(js_ctx *ctx, js_node *list, js_env *env,
                     js_value *cv, js_string **lbl)
{
    js_node *q;

    for (q = list; q; q = q->next) {
        int r = exec(ctx, q, env, cv, lbl);
        if (r != CT_NORMAL)
            return r;
    }
    return CT_NORMAL;
}

static int exec_inner(js_ctx *ctx, js_node *n, js_env *env,
                      js_value *cv, js_string **lbl)
{
    js_label *mine = ctx->labels;

    switch (n->type) {
    case N_EMPTY:
    case N_DEBUGGER:
        return CT_NORMAL;

    case N_BLOCK: {
        js_env *benv = env;
        ctx->labels = 0;
        if (n->b) {
            benv = create_lex_env(ctx, env, n->b, 0);
            if (!benv) return CT_THROW;
        }
        return exec_list(ctx, n->a, benv, cv, lbl);
    }

    case N_EXPRSTMT: {
        js_value v;
        ctx->labels = 0;
        if (eval(ctx, n->a, env, &v) != JS_OK) return CT_THROW;
        *cv = v;
        return CT_NORMAL;
    }

    case N_CLASS: {
        js_value v;
        ctx->labels = 0;
        if (eval_class(ctx, n, env, &v) != JS_OK) return CT_THROW;
        return CT_NORMAL;
    }

    case N_VAR: {
        js_node *d;
        ctx->labels = 0;
        for (d = n->a; d; d = d->next) {
            js_value v = js_undefined();
            if (d->a) {
                if (eval(ctx, d->a, env, &v) != JS_OK) return CT_THROW;
            } else if (!n->op) {
                continue;
            }
            if (d->b) {
                int mode = n->op ? BIND_LEX_INIT : BIND_VAR;
                if (bind_pattern(ctx, d->b, v, env, mode) != JS_OK)
                    return CT_THROW;
            } else if (n->op) {
                if (init_lex_var(ctx, env, d->str, v) != JS_OK)
                    return CT_THROW;
            } else {
                js_object *scope = resolve(ctx, env, d->str);
                if (!scope) scope = ctx->global;
                if (js_obj_put(ctx, scope, d->str, v) != JS_OK)
                    return CT_THROW;
            }
        }
        return CT_NORMAL;
    }

    case N_IF: {
        js_value t;
        ctx->labels = 0;
        if (eval(ctx, n->a, env, &t) != JS_OK) return CT_THROW;
        if (js_to_boolean(t))
            return exec(ctx, n->b, env, cv, lbl);
        if (n->c)
            return exec(ctx, n->c, env, cv, lbl);
        return CT_NORMAL;
    }

    case N_WHILE: {
        ctx->labels = 0;
        for (;;) {
            js_value t;
            int r;
            if (eval(ctx, n->a, env, &t) != JS_OK) return CT_THROW;
            if (!js_to_boolean(t)) break;
            r = exec(ctx, n->b, env, cv, lbl);
            if (r == CT_BREAK && targets_me(mine, *lbl)) { *lbl = 0; break; }
            if (r == CT_CONTINUE && targets_me(mine, *lbl)) { *lbl = 0; continue; }
            if (r != CT_NORMAL) return r;
        }
        return CT_NORMAL;
    }

    case N_DO: {
        ctx->labels = 0;
        for (;;) {
            js_value t;
            int r = exec(ctx, n->a, env, cv, lbl);
            if (r == CT_BREAK && targets_me(mine, *lbl)) { *lbl = 0; break; }
            if (r == CT_CONTINUE && targets_me(mine, *lbl)) { *lbl = 0; }
            else if (r != CT_NORMAL) return r;
            if (eval(ctx, n->b, env, &t) != JS_OK) return CT_THROW;
            if (!js_to_boolean(t)) break;
        }
        return CT_NORMAL;
    }

    case N_FOR: {
        js_env *for_env = env;
        js_env *iter_env = env;
        ctx->labels = 0;
        if (n->e) {
            for_env = create_lex_env(ctx, env, n->e, 0);
            if (!for_env) return CT_THROW;
            iter_env = for_env;
        }
        if (n->a) {
            int r = exec(ctx, n->a, for_env, cv, lbl);
            if (r != CT_NORMAL) return r;
        }
        if (n->e) {
            iter_env = copy_lex_env(ctx, for_env);
            if (!iter_env) return CT_THROW;
        }
        for (;;) {
            js_value t;
            int r;
            if (n->b) {
                if (eval(ctx, n->b, iter_env, &t) != JS_OK) return CT_THROW;
                if (!js_to_boolean(t)) break;
            }
            r = exec(ctx, n->d, iter_env, cv, lbl);
            if (r == CT_BREAK && targets_me(mine, *lbl)) { *lbl = 0; break; }
            if (r == CT_CONTINUE && targets_me(mine, *lbl)) { *lbl = 0; }
            else if (r != CT_NORMAL) return r;
            if (n->e) {
                iter_env = copy_lex_env(ctx, iter_env);
                if (!iter_env) return CT_THROW;
            }
            if (n->c) {
                js_value u;
                if (eval(ctx, n->c, iter_env, &u) != JS_OK) return CT_THROW;
            }
        }
        return CT_NORMAL;
    }

    case N_FOROF: {
        js_value iv;
        js_env *rhs_env = env;
        js_iter it;
        int done = 0;

        ctx->labels = 0;
        if (n->e) {
            rhs_env = create_lex_env(ctx, env, n->e, 0);
            if (!rhs_env) return CT_THROW;
        }
        if (eval(ctx, n->b, rhs_env, &iv) != JS_OK) return CT_THROW;
        if (js_iter_open(ctx, iv, &it) != JS_OK) return CT_THROW;
        for (;;) {
            js_value item;
            js_env *benv = env;
            int r;
            if (js_step(ctx) != JS_OK) return CT_THROW;
            if (js_iter_next(ctx, &it, &item, &done) != JS_OK) return CT_THROW;
            if (done) break;
            if (n->e && (n->op == 1 || n->op == 2)) {
                benv = create_lex_env(ctx, env, n->e, 0);
                if (!benv) return CT_THROW;
            }
            if (n->flags) {
                js_node *d = n->a;
                int mode = (n->op == 0) ? BIND_VAR : BIND_LEX_INIT;
                if (d->b) {
                    if (bind_pattern(ctx, d->b, item, benv, mode) != JS_OK)
                        return CT_THROW;
                } else if (n->op) {
                    if (init_lex_var(ctx, benv, d->str, item) != JS_OK)
                        return CT_THROW;
                } else {
                    js_object *scope = resolve(ctx, benv, d->str);
                    if (!scope) scope = ctx->global;
                    if (js_obj_put(ctx, scope, d->str, item) != JS_OK)
                        return CT_THROW;
                }
            } else {
                if (bind_pattern(ctx, n->a, item, benv, BIND_ASSIGN) != JS_OK)
                    return CT_THROW;
            }
            r = exec(ctx, n->c, benv, cv, lbl);
            if (r == CT_BREAK && targets_me(mine, *lbl)) { *lbl = 0; break; }
            if (r == CT_CONTINUE && targets_me(mine, *lbl)) { *lbl = 0; continue; }
            if (r != CT_NORMAL) return r;
        }
        return CT_NORMAL;
    }

    case N_FORIN: {
        js_value ov;
        js_env *rhs_env = env;
        js_string **keys;
        int nkeys = 0, i;

        ctx->labels = 0;
        if (n->e) {
            rhs_env = create_lex_env(ctx, env, n->e, 0);
            if (!rhs_env) return CT_THROW;
        }
        if (eval(ctx, n->b, rhs_env, &ov) != JS_OK) return CT_THROW;
        if (ov.type == JS_UNDEFINED || ov.type == JS_NULL)
            return CT_NORMAL;
        if (ov.type != JS_OBJECT) {
            js_value w;
            if (js_to_object(ctx, ov, &w) != JS_OK) return CT_THROW;
            ov = w;
        }
        keys = enum_keys(ctx, ov.u.obj, &nkeys);
        if (!keys) return CT_THROW;
        for (i = 0; i < nkeys; i++) {
            js_env *benv = env;
            int r;
            if (js_step(ctx) != JS_OK) return CT_THROW;
            if (!js_obj_has(ctx, ov.u.obj, keys[i]))
                continue;
            if (n->e && (n->op == 1 || n->op == 2)) {
                benv = create_lex_env(ctx, env, n->e, 0);
                if (!benv) return CT_THROW;
            }
            if (n->flags) {
                js_node *d = n->a;
                js_value kv = js_string_value(keys[i]);
                int mode = (n->op == 0) ? BIND_VAR : BIND_LEX_INIT;
                if (d->b) {
                    if (bind_pattern(ctx, d->b, kv, benv, mode) != JS_OK)
                        return CT_THROW;
                } else if (n->op) {
                    if (init_lex_var(ctx, benv, d->str, kv) != JS_OK)
                        return CT_THROW;
                } else {
                    js_object *scope = resolve(ctx, benv, d->str);
                    if (!scope) scope = ctx->global;
                    if (js_obj_put(ctx, scope, d->str, kv) != JS_OK)
                        return CT_THROW;
                }
            } else {
                if (bind_pattern(ctx, n->a, js_string_value(keys[i]),
                                 benv, BIND_ASSIGN) != JS_OK)
                    return CT_THROW;
            }
            r = exec(ctx, n->c, benv, cv, lbl);
            if (r == CT_BREAK && targets_me(mine, *lbl)) { *lbl = 0; break; }
            if (r == CT_CONTINUE && targets_me(mine, *lbl)) { *lbl = 0; continue; }
            if (r != CT_NORMAL) return r;
        }
        return CT_NORMAL;
    }

    case N_BREAK:
        *lbl = n->str;
        return CT_BREAK;

    case N_CONTINUE:
        *lbl = n->str;
        return CT_CONTINUE;

    case N_RETURN:
        ctx->labels = 0;
        if (n->a) {
            js_value v;
            if (eval(ctx, n->a, env, &v) != JS_OK) return CT_THROW;
            *cv = v;
        } else {
            *cv = js_undefined();
        }
        return CT_RETURN;

    case N_THROW: {
        js_value v;
        ctx->labels = 0;
        if (eval(ctx, n->a, env, &v) != JS_OK) return CT_THROW;
        js_throw(ctx, v);
        return CT_THROW;
    }

    case N_LABEL: {
        js_label lab;
        int r;
        lab.name = n->str;
        lab.next = mine;
        ctx->labels = &lab;
        r = exec(ctx, n->a, env, cv, lbl);
        ctx->labels = mine;
        if (r == CT_BREAK && *lbl && js_str_eq(*lbl, n->str)) {
            *lbl = 0;
            return CT_NORMAL;
        }
        return r;
    }

    case N_WITH: {
        js_value ov, o;
        js_env *wenv;
        ctx->labels = 0;
        if (eval(ctx, n->a, env, &ov) != JS_OK) return CT_THROW;
        if (js_to_object(ctx, ov, &o) != JS_OK) return CT_THROW;
        wenv = js_env_new(ctx, env, o.u.obj);
        if (!wenv) return CT_THROW;
        wenv->is_with = 1;
        wenv->this_val = env->this_val;
        return exec(ctx, n->b, wenv, cv, lbl);
    }

    case N_SWITCH: {
        js_value d;
        js_node *c, *dflt = 0;
        js_env *senv = env;
        int matched = 0, r;

        ctx->labels = 0;
        if (eval(ctx, n->a, env, &d) != JS_OK) return CT_THROW;
        if (n->c) {
            senv = create_lex_env(ctx, env, n->c, 0);
            if (!senv) return CT_THROW;
        }
        for (c = n->b; c; c = c->next) {
            js_value t;
            if (!c->a) { dflt = c; continue; }
            if (eval(ctx, c->a, senv, &t) != JS_OK) return CT_THROW;
            if (js_strict_equals(d, t)) { matched = 1; break; }
        }
        if (!matched)
            c = dflt;
        for (; c; c = c->next) {
            r = exec_list(ctx, c->b, senv, cv, lbl);
            if (r == CT_BREAK && targets_me(mine, *lbl)) { *lbl = 0; return CT_NORMAL; }
            if (r != CT_NORMAL) return r;
        }
        return CT_NORMAL;
    }

    case N_TRY: {
        int r;
        ctx->labels = 0;
        r = exec(ctx, n->a, env, cv, lbl);

        if (r == CT_THROW && n->b && !ctx->fatal) {
            js_value exc = js_exception(ctx);
            js_object *scope = js_obj_alloc(ctx, JS_CLASS_OBJECT, 0);
            js_env *cenv;
            ctx->has_exception = 0;
            ctx->exception = js_undefined();
            if (!scope) return CT_THROW;
            cenv = js_env_new(ctx, env, scope);
            if (!cenv) return CT_THROW;
            cenv->this_val = env->this_val;
            if (n->d) {
                if (bind_pattern(ctx, n->d, exc, cenv, BIND_VAR) != JS_OK)
                    return CT_THROW;
            } else if (n->str) {
                js_prop *p = js_add_prop(ctx, scope, n->str,
                                         JS_P_ENUM | JS_P_WRITE);
                if (!p) return CT_THROW;
                p->value = exc;
            }
            r = exec(ctx, n->b, cenv, cv, lbl);
        }

        if (n->c) {
            js_value fv = js_undefined();
            js_string *flbl = 0;
            js_value sexc = ctx->exception;
            int shas = ctx->has_exception;
            js_value scv = *cv;
            js_string *slbl = *lbl;
            int fr;

            if (ctx->fatal)
                return CT_THROW;
            ctx->has_exception = 0;
            ctx->exception = js_undefined();
            fr = exec(ctx, n->c, env, &fv, &flbl);
            if (fr != CT_NORMAL) {
                *cv = fv;
                *lbl = flbl;
                return fr;
            }
            ctx->exception = sexc;
            ctx->has_exception = shas;
            *cv = scv;
            *lbl = slbl;
        }
        return r;
    }

    case N_FUNCDECL:
        return CT_NORMAL;

    default: {
        js_value v;
        ctx->labels = 0;
        if (eval(ctx, n, env, &v) != JS_OK) return CT_THROW;
        *cv = v;
        return CT_NORMAL;
    }
    }
}

static int exec(js_ctx *ctx, js_node *n, js_env *env,
                js_value *cv, js_string **lbl)
{
    js_label *saved = ctx->labels;
    int r;

    if (js_step(ctx) != JS_OK)
        return CT_THROW;
    if (too_deep(ctx))
        return CT_THROW;
    ctx->depth++;
    r = exec_inner(ctx, n, env, cv, lbl);
    ctx->depth--;
    ctx->labels = saved;
    return r;
}

/* ------------------------------------------------------------------ */
/* Program entry                                                       */
/* ------------------------------------------------------------------ */

int js_eval(js_ctx *ctx, const char *src, const char *name, js_value *result)
{
    js_node *prog, *q;
    js_value cv = js_undefined();
    js_string *lbl = 0;
    js_env *top_env;
    int r;

    *result = js_undefined();
    if (ctx->fatal) {
        *result = js_exception(ctx);
        return JS_THROW;
    }
    ctx->has_exception = 0;
    ctx->exception = js_undefined();
    ctx->depth = 0;
    ctx->call_depth = 0;
    ctx->argsp = 0;
    ctx->labels = 0;

    prog = js_parse(ctx, src, name);
    if (!prog) {
        *result = js_exception(ctx);
        return JS_THROW;
    }

    top_env = ctx->lex_env ? ctx->lex_env : ctx->genv;

    for (q = prog->c; q; q = q->next)
        if (!js_own_prop(ctx->global, q->str)) {
            js_prop *p = js_add_prop(ctx, ctx->global, q->str,
                                     JS_P_ENUM | JS_P_WRITE);
            if (!p) { *result = js_exception(ctx); return JS_THROW; }
        }
    for (q = prog->d; q; q = q->next) {
        js_func *nf = (js_func *)js_alloc(ctx, sizeof(js_func));
        js_object *fo;
        js_prop *p;
        if (!nf) { *result = js_exception(ctx); return JS_THROW; }
        init_func_from_node(nf, q->a);
        fo = js_make_function(ctx, nf, top_env);
        if (!fo) { *result = js_exception(ctx); return JS_THROW; }
        p = js_add_prop(ctx, ctx->global, q->str, JS_P_ENUM | JS_P_WRITE);
        if (!p) { *result = js_exception(ctx); return JS_THROW; }
        p->value = js_object_value(fo);
    }
    if (prog->b && ctx->lex_env) {
        for (q = prog->b; q; q = q->next) {
            uint32_t flags = JS_P_ENUM | JS_P_TDZ |
                             (q->op ? (uint32_t)JS_P_CONST : (uint32_t)JS_P_WRITE);
            js_prop *p = js_add_prop(ctx, ctx->lex_env->vars, q->str, flags);
            if (!p) { *result = js_exception(ctx); return JS_THROW; }
            p->flags = flags;
            p->value = js_undefined();
        }
    }

    for (q = prog->a; q; q = q->next) {
        r = exec(ctx, q, top_env, &cv, &lbl);
        if (r == CT_THROW) {
            *result = js_exception(ctx);
            return JS_THROW;
        }
        if (r != CT_NORMAL)
            break;
    }
    *result = cv;
    return JS_OK;
}
