/* libjs: the tree-walking evaluator.
 *
 * Statements return a completion type (normal / break / continue / return
 * / throw) plus a value and, for break and continue, a target label.
 * Expressions return JS_OK or JS_THROW with the exception parked in the
 * context. Scope chains are lists of environments whose variable objects
 * are ordinary objects, which is what makes closures, `with` and the
 * global object all fall out of one mechanism.
 *
 * Every eval and exec charges one step against cfg.max_steps and one level
 * against cfg.max_depth. Step exhaustion is fatal and uncatchable so that
 * `try { for (;;) {} } catch (e) {}` still terminates; depth exhaustion is
 * an ordinary RangeError. cfg.max_depth is small because the user stack is
 * 64 KiB, and this interpreter uses the C stack for the JS stack.
 */

#define JS_INTERNAL
#include "js.h"

#include <string.h>

enum { CT_NORMAL = 0, CT_BREAK, CT_CONTINUE, CT_RETURN, CT_THROW };

static int exec(js_ctx *ctx, js_node *n, js_env *env,
                js_value *cv, js_string **lbl);
static int eval(js_ctx *ctx, js_node *n, js_env *env, js_value *out);

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
/* Argument stack                                                      */
/* ------------------------------------------------------------------ */

static js_value *push_args(js_ctx *ctx, int n)
{
    js_value *p;
    int i;

    if (n > JS_MAXARGS) {
        js_throw_error(ctx, JS_ERR_RANGE, "more than %d call arguments",
                       JS_MAXARGS);
        return 0;
    }
    if (ctx->argsp + (uint32_t)n > JS_ARGSTACK) {
        js_throw_error(ctx, JS_ERR_RANGE, "argument stack exhausted");
        return 0;
    }
    p = ctx->argstack + ctx->argsp;
    ctx->argsp += (uint32_t)n;
    for (i = 0; i < n; i++)
        p[i] = js_undefined();
    return p;
}

static void pop_args(js_ctx *ctx, int n) { ctx->argsp -= (uint32_t)n; }

/* ------------------------------------------------------------------ */
/* Scope resolution                                                    */
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

/* ------------------------------------------------------------------ */
/* References                                                          */
/* ------------------------------------------------------------------ */

enum { REF_NONE = 0, REF_VAR, REF_PROP };

typedef struct {
    int        kind;
    js_object *scope;     /* REF_VAR: the object that holds (or will hold) it */
    js_value   base;      /* REF_PROP */
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
        r->kind = REF_PROP;
        r->key = n->str;
        return eval(ctx, n->a, env, &r->base);
    case N_INDEX: {
        js_value k, ks;
        if (eval(ctx, n->a, env, &r->base) != JS_OK)
            return JS_THROW;
        if (eval(ctx, n->b, env, &k) != JS_OK)
            return JS_THROW;
        if (js_to_string(ctx, k, &ks) != JS_OK)
            return JS_THROW;
        r->kind = REF_PROP;
        r->key = ks.u.str;
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
        return js_obj_get(ctx, r->scope, r->key, js_object_value(r->scope), out);
    }
    return js_wrap_get(ctx, r->base, r->key, out);
}

static int ref_put(js_ctx *ctx, jsref *r, js_value v)
{
    if (r->kind == REF_VAR) {
        js_object *o = r->scope ? r->scope : ctx->global;
        return js_obj_put(ctx, o, r->key, v);
    }
    if (r->base.type == JS_UNDEFINED || r->base.type == JS_NULL)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "cannot set property '%s' of %s", r->key->data,
                              r->base.type == JS_NULL ? "null" : "undefined");
    if (r->base.type != JS_OBJECT)
        return JS_OK;                    /* writes to primitives are dropped */
    return js_obj_put(ctx, r->base.u.obj, r->key, v);
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

    if (!fn->native && fn->ctor_kind != JS_CTOR_NONE) {
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


/* Both of these run once on scope entry and return before the body does,
 * so keeping them out of invoke() takes their locals off the stack for the
 * whole duration of the call rather than for a few microseconds. */
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
    p = js_add_prop(ctx, act, ctx->s_arguments, JS_P_WRITE);
    if (!p) return JS_THROW;
    p->value = js_object_value(ao);
    return JS_OK;
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
        nf->name = q->a->str;
        nf->params = q->a->a;
        nf->body = q->a->b;
        nf->vars = q->a->c;
        nf->funcs = q->a->d;
        nf->nparams = (int)q->a->num;
        fo = js_make_function(ctx, nf, env);
        if (!fo) return JS_THROW;
        p = js_add_prop(ctx, act, q->str, JS_P_ENUM | JS_P_WRITE);
        if (!p) return JS_THROW;
        p->value = js_object_value(fo);
    }
    return JS_OK;
}

/* Create the activation record for a call and run the body. */
static int invoke(js_ctx *ctx, js_object *f, js_value this_val,
                  int argc, js_value *argv, js_value *ret)
{
    js_func *fn = f->fn;
    js_object *act;
    js_env *env;
    js_node *q;
    js_value cv = js_undefined();
    js_string *lbl = 0;
    int i, r;

    act = js_obj_alloc(ctx, JS_CLASS_OBJECT, 0);
    if (!act)
        return JS_THROW;
    env = js_env_new(ctx, fn->closure, act);
    if (!env)
        return JS_THROW;
    env->this_val = fn->ctor_kind == JS_CTOR_NONE && fn->closure
        ? fn->closure->this_val : this_val;

    /* Hoisted var names, then parameters, then the arguments object, then
     * nested function declarations -- the ES5 order, so a parameter beats a
     * bare `var` of the same name and a function declaration beats both. */
    for (q = fn->vars; q; q = q->next) {
        js_prop *p = js_add_prop(ctx, act, q->str, JS_P_ENUM | JS_P_WRITE);
        if (!p) return JS_THROW;
    }
    {
        js_node *pn = fn->params;
        for (i = 0; pn; pn = pn->next, i++) {
            js_prop *p = js_add_prop(ctx, act, pn->str, JS_P_ENUM | JS_P_WRITE);
            if (!p) return JS_THROW;
            p->value = (i < argc) ? argv[i] : js_undefined();
        }
    }
    if (fn->ctor_kind != JS_CTOR_NONE &&
        !js_own_prop(act, ctx->s_arguments) &&
        make_arguments(ctx, f, act, argc, argv) != JS_OK)
        return JS_THROW;
    if (instantiate_funcs(ctx, fn->funcs, act, env) != JS_OK)
        return JS_THROW;

    for (q = fn->body; q; q = q->next) {
        r = exec(ctx, q, env, &cv, &lbl);
        if (r == CT_RETURN) { *ret = cv; return JS_OK; }
        if (r == CT_THROW) return JS_THROW;
        if (r != CT_NORMAL) break;     /* stray break/continue: stop the body */
    }
    *ret = js_undefined();
    return JS_OK;
}

int js_call_func(js_ctx *ctx, js_object *f, js_value this_val,
                 int argc, js_value *argv, js_value *ret)
{
    int r;

    *ret = js_undefined();
    if (ctx->fatal)
        return JS_THROW;
    if (!f->fn)
        return js_throw_error(ctx, JS_ERR_TYPE, "not a function");
    if (ctx->call_depth >= ctx->cfg.max_call_depth)
        return js_throw_error(ctx, JS_ERR_RANGE,
                              "maximum call depth (%d) exceeded",
                              ctx->cfg.max_call_depth);
    if (too_deep(ctx))
        return JS_THROW;
    if (js_step(ctx) != JS_OK)
        return JS_THROW;

    /* Function.prototype.bind: splice the bound arguments in front and
     * forward to the target with the bound `this`. */
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
        r = js_call_func(ctx, bf->bound_target, bf->bound_this, total, all, ret);
        ctx->argsp -= (uint32_t)total;
        return r;
    }

    ctx->call_depth++;
    ctx->depth++;
    if (f->fn->native) {
        r = f->fn->native(ctx, this_val, argc, argv, ret);
    } else {
        /* Non-strict `this` coercion. */
        if (this_val.type == JS_UNDEFINED || this_val.type == JS_NULL)
            this_val = js_object_value(ctx->global);
        else if (this_val.type != JS_OBJECT) {
            js_value w;
            if (js_wrap_primitive(ctx, this_val, &w) == JS_OK)
                this_val = w;
        }
        r = invoke(ctx, f, this_val, argc, argv, ret);
    }
    ctx->depth--;
    ctx->call_depth--;
    return r;
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

int js_construct(js_ctx *ctx, js_value fnv, int argc, js_value *argv,
                 js_value *ret)
{
    js_object *f;
    js_object *obj;
    js_value proto, rv;
    int r;

    *ret = js_undefined();
    if (!js_is_function(fnv))
        return js_throw_error(ctx, JS_ERR_TYPE, "value is not a constructor");
    f = fnv.u.obj;
    if (f->fn->ctor_kind == JS_CTOR_NONE)
        return js_throw_error(ctx, JS_ERR_TYPE, "%s is not a constructor",
                              f->fn->name ? f->fn->name->data : "value");
    if (f->fn->bound_target) {
        /* `new boundFn(...)` constructs the target, ignoring the bound
         * `this` exactly as ES5 specifies. */
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
        r = js_construct(ctx, js_object_value(bf->bound_target), total, all, ret);
        ctx->argsp -= (uint32_t)total;
        return r;
    }
    if (f->fn->native) {
        js_object *saved = ctx->new_target;
        ctx->new_target = f;
        r = js_call_func(ctx, f, js_undefined(), argc, argv, ret);
        ctx->new_target = saved;
        if (r != JS_OK)
            return r;
        if (ret->type != JS_OBJECT)
            return js_throw_error(ctx, JS_ERR_TYPE,
                                  "constructor did not return an object");
        return JS_OK;
    }
    if (js_obj_get(ctx, f, ctx->s_prototype, fnv, &proto) != JS_OK)
        return JS_THROW;
    obj = js_obj_alloc(ctx, JS_CLASS_OBJECT,
                       proto.type == JS_OBJECT ? proto.u.obj : ctx->proto[P_OBJECT]);
    if (!obj)
        return JS_THROW;
    r = js_call_func(ctx, f, js_object_value(obj), argc, argv, &rv);
    if (r != JS_OK)
        return r;
    *ret = (rv.type == JS_OBJECT) ? rv : js_object_value(obj);
    return JS_OK;
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

/* ES5 11.8.5. *out: 0 false, 1 true, 2 undefined (a NaN was involved). */
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

    case TK_SUB: case TK_MUL: case TK_DIV: case TK_MOD:
        if (js_to_number(ctx, a, &x) != JS_OK) return JS_THROW;
        if (js_to_number(ctx, b, &y) != JS_OK) return JS_THROW;
        switch (op) {
        case TK_SUB: *out = js_number(x - y); break;
        case TK_MUL: *out = js_number(x * y); break;
        case TK_DIV: *out = js_number(x / y); break;
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
        js_value ks;
        if (b.type != JS_OBJECT)
            return js_throw_error(ctx, JS_ERR_TYPE,
                                  "right operand of `in` is not an object");
        if (js_to_string(ctx, a, &ks) != JS_OK) return JS_THROW;
        *out = js_bool(js_obj_has(ctx, b.u.obj, ks.u.str));
        return JS_OK;
    }
    default:
        return js_throw_error(ctx, JS_ERR_SYNTAX, "unsupported operator");
    }
}

/* ------------------------------------------------------------------ */
/* Expressions                                                         */
/* ------------------------------------------------------------------ */

static int eval_call(js_ctx *ctx, js_node *n, js_env *env, js_value *out)
{
    js_node *callee = n->a;
    js_value fnv, thisv = js_undefined();
    js_value *args;
    int argc = 0, r;
    js_node *a;

    if (callee->type == N_MEMBER || callee->type == N_INDEX) {
        jsref r2;
        if (eval_ref(ctx, callee, env, &r2) != JS_OK) return JS_THROW;
        thisv = r2.base;
        if (ref_get(ctx, &r2, &fnv) != JS_OK) return JS_THROW;
        if (!js_is_function(fnv))
            return js_throw_error(ctx, JS_ERR_TYPE, "%s is not a function",
                                  r2.key->data);
    } else if (callee->type == N_IDENT) {
        js_object *scope = resolve(ctx, env, callee->str);
        if (!scope)
            return js_throw_error(ctx, JS_ERR_REFERENCE, "%s is not defined",
                                  callee->str->data);
        if (js_obj_get(ctx, scope, callee->str, js_object_value(scope), &fnv) != JS_OK)
            return JS_THROW;
        /* `with (o) f()` calls f with o as the receiver */
        {
            js_env *e;
            for (e = env; e; e = e->parent)
                if (e->is_with && e->vars == scope) {
                    thisv = js_object_value(scope);
                    break;
                }
        }
        if (!js_is_function(fnv))
            return js_throw_error(ctx, JS_ERR_TYPE, "%s is not a function",
                                  callee->str->data);
    } else {
        if (eval(ctx, callee, env, &fnv) != JS_OK) return JS_THROW;
        if (!js_is_function(fnv))
            return js_throw_error(ctx, JS_ERR_TYPE, "value is not a function");
    }

    for (a = n->b; a; a = a->next)
        argc++;
    args = push_args(ctx, argc);
    if (!args)
        return JS_THROW;
    {
        int i = 0;
        for (a = n->b; a; a = a->next, i++)
            if (eval(ctx, a, env, &args[i]) != JS_OK) {
                pop_args(ctx, argc);
                return JS_THROW;
            }
    }
    r = js_call_func(ctx, fnv.u.obj, thisv, argc, args, out);
    pop_args(ctx, argc);
    return r;
}

static int eval_new(js_ctx *ctx, js_node *n, js_env *env, js_value *out)
{
    js_value fnv;
    js_value *args;
    int argc = 0, r;
    js_node *a;

    if (eval(ctx, n->a, env, &fnv) != JS_OK)
        return JS_THROW;
    for (a = n->b; a; a = a->next)
        argc++;
    args = push_args(ctx, argc);
    if (!args)
        return JS_THROW;
    {
        int i = 0;
        for (a = n->b; a; a = a->next, i++)
            if (eval(ctx, a, env, &args[i]) != JS_OK) {
                pop_args(ctx, argc);
                return JS_THROW;
            }
    }
    r = js_construct(ctx, fnv, argc, args, out);
    pop_args(ctx, argc);
    return r;
}

static int eval_func(js_ctx *ctx, js_node *n, js_env *env, js_value *out)
{
    js_func *fn = (js_func *)js_alloc(ctx, sizeof(js_func));
    js_object *fo;
    js_env *fenv = env;

    if (!fn)
        return JS_THROW;
    fn->name = n->str;
    fn->params = n->a;
    fn->body = n->b;
    fn->vars = n->c;
    fn->funcs = n->d;
    fn->nparams = (int)n->num;
    fn->ctor_kind = (n->flags & JS_NODE_ARROW)
        ? JS_CTOR_NONE : JS_CTOR_NORMAL;

    /* A named function expression can see its own name. */
    if (n->str) {
        js_object *scope = js_obj_alloc(ctx, JS_CLASS_OBJECT, 0);
        if (!scope) return JS_THROW;
        fenv = js_env_new(ctx, env, scope);
        if (!fenv) return JS_THROW;
        fenv->this_val = env->this_val;
        fo = js_make_function(ctx, fn, fenv);
        if (!fo) return JS_THROW;
        {
            js_prop *p = js_add_prop(ctx, scope, n->str, JS_P_ENUM);
            if (!p) return JS_THROW;
            p->value = js_object_value(fo);
        }
    } else {
        fo = js_make_function(ctx, fn, fenv);
        if (!fo) return JS_THROW;
    }
    *out = js_object_value(fo);
    return JS_OK;
}

static int eval_object(js_ctx *ctx, js_node *n, js_env *env, js_value *out)
{
    js_object *o = js_new_object(ctx);
    js_node *q;

    if (!o)
        return JS_THROW;
    for (q = n->a; q; q = q->next) {
        js_prop *p;
        if (q->op) {                     /* get / set */
            js_value f;
            if (eval_func(ctx, q->a, env, &f) != JS_OK) return JS_THROW;
            p = js_own_prop(o, q->str);
            if (!p || !(p->flags & JS_P_ACCESSOR)) {
                p = js_add_prop(ctx, o, q->str, 0);
                if (!p) return JS_THROW;
                p->flags = JS_P_ACCESSOR | JS_P_ENUM | JS_P_CONFIG;
                p->value = js_undefined();
                p->setter = js_undefined();
            }
            if (q->op == 1) p->value = f;
            else p->setter = f;
            continue;
        }
        {
            js_value v;
            if (eval(ctx, q->a, env, &v) != JS_OK) return JS_THROW;
            p = js_own_prop(o, q->str);
            if (p && (p->flags & JS_P_ACCESSOR)) {
                p->flags = JS_P_DEFAULT;
                p->setter = js_undefined();
            }
            if (!p) {
                p = js_add_prop(ctx, o, q->str, JS_P_DEFAULT);
                if (!p) return JS_THROW;
                p->flags = JS_P_DEFAULT;
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
        if (q->type != N_EMPTY) {
            if (eval(ctx, q, env, &v) != JS_OK) return JS_THROW;
            trailing_hole = 0;
        } else {
            trailing_hole = 1;
        }
        if (js_array_push(ctx, a, v) != JS_OK) return JS_THROW;
    }
    /* `[1,2,]` has length 2, not 3: the final elision is a separator. */
    if (trailing_hole && a->elen)
        a->elen--;
    *out = js_object_value(a);
    return JS_OK;
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
            if (js_obj_get(ctx, scope, n->a->str, js_object_value(scope), &v) != JS_OK)
                return JS_THROW;
        } else if (eval(ctx, n->a, env, &v) != JS_OK) {
            return JS_THROW;
        }
        *out = js_mkcstring(ctx, js_typeof(v));
        return JS_OK;

    case TK_DELETE:
        if (n->a->type == N_IDENT) {
            js_object *scope = resolve(ctx, env, n->a->str);
            *out = js_bool(scope ? js_obj_delete(ctx, scope, n->a->str) : 1);
            return JS_OK;
        }
        if (n->a->type == N_MEMBER || n->a->type == N_INDEX) {
            jsref r;
            if (eval_ref(ctx, n->a, env, &r) != JS_OK) return JS_THROW;
            if (r.base.type == JS_UNDEFINED || r.base.type == JS_NULL)
                return js_throw_error(ctx, JS_ERR_TYPE,
                                      "cannot delete a property of %s",
                                      r.base.type == JS_NULL ? "null" : "undefined");
            *out = js_bool(r.base.type != JS_OBJECT ? 1
                           : js_obj_delete(ctx, r.base.u.obj, r.key));
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
    case N_BOOL: *out = js_bool(n->num != 0); return JS_OK;
    case N_NULL: *out = js_null(); return JS_OK;
    case N_THIS: *out = env->this_val; return JS_OK;

    case N_IDENT: {
        js_object *scope = resolve(ctx, env, n->str);
        if (!scope)
            return js_throw_error(ctx, JS_ERR_REFERENCE, "%s is not defined",
                                  n->str->data);
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

    case N_ARRAY:  return eval_array(ctx, n, env, out);
    case N_OBJECT: return eval_object(ctx, n, env, out);
    case N_FUNC:   return eval_func(ctx, n, env, out);
    case N_CALL:   return eval_call(ctx, n, env, out);
    case N_NEW:    return eval_new(ctx, n, env, out);

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
        if (eval_ref(ctx, n->a, env, &r) != JS_OK) return JS_THROW;
        if (n->op == TK_ASSIGN) {
            if (eval(ctx, n->b, env, &v) != JS_OK) return JS_THROW;
        } else {
            js_value old, rhs;
            static const struct { int a; int b; } map[] = {
                { TK_ADD_A, TK_ADD }, { TK_SUB_A, TK_SUB },
                { TK_MUL_A, TK_MUL }, { TK_DIV_A, TK_DIV },
                { TK_MOD_A, TK_MOD }, { TK_SHL_A, TK_SHL },
                { TK_SHR_A, TK_SHR }, { TK_USHR_A, TK_USHR },
                { TK_BAND_A, TK_BAND }, { TK_BOR_A, TK_BOR },
                { TK_BXOR_A, TK_BXOR }
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

        /* dense array elements first, then named properties */
        if (level->cls == JS_CLASS_ARRAY) {
            for (i = 0; i < level->elen && n < JS_MAX_ENUM; i++) {
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
            if ((p->flags & JS_P_DEAD) || !(p->flags & JS_P_ENUM))
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

/* Does a break/continue carrying `lbl` target the statement whose label
 * set is `mine`? An unlabelled one always targets the nearest loop. */
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

    case N_BLOCK:
        ctx->labels = 0;
        return exec_list(ctx, n->a, env, cv, lbl);

    case N_EXPRSTMT: {
        js_value v;
        ctx->labels = 0;
        if (eval(ctx, n->a, env, &v) != JS_OK) return CT_THROW;
        *cv = v;
        return CT_NORMAL;
    }

    case N_VAR: {
        js_node *d;
        ctx->labels = 0;
        for (d = n->a; d; d = d->next) {
            js_value v;
            js_object *scope;
            if (!d->a)
                continue;
            if (eval(ctx, d->a, env, &v) != JS_OK) return CT_THROW;
            scope = resolve(ctx, env, d->str);
            if (!scope) scope = ctx->global;
            if (js_obj_put(ctx, scope, d->str, v) != JS_OK) return CT_THROW;
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
        ctx->labels = 0;
        if (n->a) {
            int r = exec(ctx, n->a, env, cv, lbl);
            if (r != CT_NORMAL) return r;
        }
        for (;;) {
            js_value t;
            int r;
            if (n->b) {
                if (eval(ctx, n->b, env, &t) != JS_OK) return CT_THROW;
                if (!js_to_boolean(t)) break;
            }
            r = exec(ctx, n->d, env, cv, lbl);
            if (r == CT_BREAK && targets_me(mine, *lbl)) { *lbl = 0; break; }
            if (r == CT_CONTINUE && targets_me(mine, *lbl)) { *lbl = 0; }
            else if (r != CT_NORMAL) return r;
            if (n->c) {
                js_value u;
                if (eval(ctx, n->c, env, &u) != JS_OK) return CT_THROW;
            }
        }
        return CT_NORMAL;
    }

    case N_FORIN: {
        js_value ov;
        js_string **keys;
        int nkeys = 0, i;

        ctx->labels = 0;
        if (eval(ctx, n->b, env, &ov) != JS_OK) return CT_THROW;
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
            int r;
            if (js_step(ctx) != JS_OK) return CT_THROW;
            /* Skip keys deleted since the snapshot, as ES5 requires. */
            if (!js_obj_has(ctx, ov.u.obj, keys[i]))
                continue;
            if (n->flags) {
                js_object *scope = resolve(ctx, env, n->a->str);
                if (!scope) scope = ctx->global;
                if (js_obj_put(ctx, scope, n->a->str,
                               js_string_value(keys[i])) != JS_OK)
                    return CT_THROW;
            } else {
                jsref r2;
                if (eval_ref(ctx, n->a, env, &r2) != JS_OK) return CT_THROW;
                if (ref_put(ctx, &r2, js_string_value(keys[i])) != JS_OK)
                    return CT_THROW;
            }
            r = exec(ctx, n->c, env, cv, lbl);
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
        int matched = 0, r;

        ctx->labels = 0;
        if (eval(ctx, n->a, env, &d) != JS_OK) return CT_THROW;
        for (c = n->b; c; c = c->next) {
            js_value t;
            if (!c->a) { dflt = c; continue; }
            if (eval(ctx, c->a, env, &t) != JS_OK) return CT_THROW;
            if (js_strict_equals(d, t)) { matched = 1; break; }
        }
        if (!matched)
            c = dflt;
        for (; c; c = c->next) {
            r = exec_list(ctx, c->b, env, cv, lbl);
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
            js_prop *p;
            ctx->has_exception = 0;
            ctx->exception = js_undefined();
            if (!scope) return CT_THROW;
            cenv = js_env_new(ctx, env, scope);
            if (!cenv) return CT_THROW;
            cenv->this_val = env->this_val;
            p = js_add_prop(ctx, scope, n->str, JS_P_ENUM | JS_P_WRITE);
            if (!p) return CT_THROW;
            p->value = exc;
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
                /* the finally block's own completion wins */
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
        return CT_NORMAL;             /* instantiated on scope entry */

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
        nf->name = q->a->str;
        nf->params = q->a->a;
        nf->body = q->a->b;
        nf->vars = q->a->c;
        nf->funcs = q->a->d;
        nf->nparams = (int)q->a->num;
        fo = js_make_function(ctx, nf, ctx->genv);
        if (!fo) { *result = js_exception(ctx); return JS_THROW; }
        p = js_add_prop(ctx, ctx->global, q->str, JS_P_ENUM | JS_P_WRITE);
        if (!p) { *result = js_exception(ctx); return JS_THROW; }
        p->value = js_object_value(fo);
    }

    for (q = prog->a; q; q = q->next) {
        r = exec(ctx, q, ctx->genv, &cv, &lbl);
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
