/* libjs: values, objects, the ES5 abstract operations, and the numeric
 * kernels (double <-> string, and the pieces of libm the language needs).
 *
 * There is no libm in this environment, so everything from Math.floor to
 * Math.atan2 is implemented here from the series expansions. Accuracy is
 * around one or two ulp, which is fine for a browser and is not claimed to
 * be more than that.
 *
 * Number->string is the exact Steele & White / Burger & Dybvig "free
 * format" algorithm over a fixed-size bignum, so ToString(number) produces
 * the shortest decimal that round-trips, exactly as ES5 9.8.1 requires.
 */

#define JS_INTERNAL
#include "js.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================== */
/* Region allocator                                                    */
/* ================================================================== */

struct js_chunk {
    struct js_chunk *next;
    unsigned long    size;
};

#define JS_CHUNK 65536UL

int js_oom(js_ctx *ctx)
{
    if (!ctx->fatal) {
        ctx->fatal = 1;
        ctx->fatal_msg = "script heap limit exceeded";
        ctx->has_exception = 1;
        ctx->exception = js_undefined();
    }
    return JS_THROW;
}

void *js_alloc_raw(js_ctx *ctx, unsigned long n)
{
    char *p;

    n = (n + 15UL) & ~15UL;
    if (n == 0)
        n = 16;
    if (n <= ctx->cur_left) {
        p = ctx->cur;
        ctx->cur += n;
        ctx->cur_left -= n;
        return p;
    }
    {
        unsigned long want = n + sizeof(struct js_chunk) + 16;
        struct js_chunk *c;

        if (want < JS_CHUNK)
            want = JS_CHUNK;
        if (ctx->heap_used + want > ctx->cfg.max_heap) {
            js_oom(ctx);
            return 0;
        }
        c = (struct js_chunk *)malloc(want);
        if (!c) {
            js_oom(ctx);
            return 0;
        }
        c->size = want;
        c->next = ctx->chunks;
        ctx->chunks = c;
        ctx->heap_used += want;
        p = (char *)c + sizeof(struct js_chunk);
        p = (char *)(((uintptr_t)p + 15U) & ~(uintptr_t)15U);
        ctx->cur = p + n;
        ctx->cur_left = want - (unsigned long)(ctx->cur - (char *)c);
        return p;
    }
}

void *js_alloc(js_ctx *ctx, unsigned long n)
{
    void *p = js_alloc_raw(ctx, n);
    if (p)
        memset(p, 0, (n + 15UL) & ~15UL);
    return p;
}

/* ---- growable byte buffer ---- */

void js_sb_init(js_sbuf *b, js_ctx *ctx)
{
    b->ctx = ctx;
    b->p = 0;
    b->n = 0;
    b->cap = 0;
    b->err = 0;
}

int js_sb_put(js_sbuf *b, const char *s, unsigned long n)
{
    if (b->err)
        return JS_THROW;
    if (b->n + n + 1 > b->cap) {
        unsigned long nc = b->cap ? b->cap * 2 : 64;
        char *np;
        while (nc < b->n + n + 1)
            nc *= 2;
        if (nc > b->ctx->cfg.max_string ||
            b->ctx->heap_used + nc - b->cap > b->ctx->cfg.max_heap) {
            b->err = 1;
            return js_oom(b->ctx);
        }
        np = (char *)realloc(b->p, nc);
        if (!np) { b->err = 1; return js_oom(b->ctx); }
        b->ctx->heap_used += nc - b->cap;
        b->p = np;
        b->cap = nc;
    }
    if (n)
        memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
    return JS_OK;
}

int js_sb_putc(js_sbuf *b, char c) { return js_sb_put(b, &c, 1); }
int js_sb_puts(js_sbuf *b, const char *s) { return js_sb_put(b, s, strlen(s)); }

void js_sb_free(js_sbuf *b)
{
    if (b->p) {
        b->ctx->heap_used -= b->cap;
        free(b->p);
        b->p = 0;
        b->cap = 0;
        b->n = 0;
    }
}

int js_sb_finish(js_sbuf *b, js_value *out)
{
    js_string *s;

    if (b->err) { js_sb_free(b); *out = js_undefined(); return JS_THROW; }
    s = js_str_new(b->ctx, b->p ? b->p : "", b->n);
    js_sb_free(b);
    if (!s) { *out = js_undefined(); return JS_THROW; }
    *out = js_string_value(s);
    return JS_OK;
}

int js_step(js_ctx *ctx)
{
    if (ctx->fatal)
        return JS_THROW;
    ctx->steps++;
    if (ctx->cfg.max_steps && ctx->steps > ctx->cfg.max_steps) {
        ctx->fatal = 1;
        ctx->fatal_msg = "script step limit exceeded";
        ctx->has_exception = 1;
        ctx->exception = js_undefined();
        return JS_THROW;
    }
    return JS_OK;
}

int js_fatal(js_ctx *ctx) { return ctx->fatal; }
void js_reset_steps(js_ctx *ctx) { ctx->steps = 0; }

/* ================================================================== */
/* Value constructors                                                  */
/* ================================================================== */

js_value js_undefined(void) { js_value v; v.type = JS_UNDEFINED; v.pad = 0; v.u.num = 0; return v; }
js_value js_null(void)      { js_value v; v.type = JS_NULL; v.pad = 0; v.u.num = 0; return v; }
js_value js_bool(int b)     { js_value v; v.type = JS_BOOL; v.pad = 0; v.u.num = 0; v.u.b = b ? 1 : 0; return v; }
js_value js_number(double d){ js_value v; v.type = JS_NUMBER; v.pad = 0; v.u.num = d; return v; }

js_value js_object_value(js_object *o)
{
    js_value v;
    if (!o) return js_null();
    v.type = JS_OBJECT; v.pad = 0; v.u.num = 0; v.u.obj = o;
    return v;
}

js_value js_string_value(js_string *s)
{
    js_value v;
    if (!s) return js_undefined();
    v.type = JS_STRING; v.pad = 0; v.u.num = 0; v.u.str = s;
    return v;
}

int js_is_function(js_value v)
{
    return v.type == JS_OBJECT && v.u.obj->cls == JS_CLASS_FUNCTION;
}

int js_is_array(js_value v)
{
    return v.type == JS_OBJECT && v.u.obj->cls == JS_CLASS_ARRAY;
}

const char *js_string_bytes(js_value v, unsigned long *len)
{
    if (v.type != JS_STRING) { if (len) *len = 0; return 0; }
    if (len) *len = v.u.str->len;
    return v.u.str->data;
}

/* ================================================================== */
/* Strings                                                             */
/* ================================================================== */

#define JS_INTERN_MAX 64        /* strings up to this length are interned */

static uint32_t str_hash(const char *s, unsigned long n)
{
    uint32_t h = 2166136261u;
    unsigned long i;
    for (i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h ? h : 1;
}

static js_string *str_raw(js_ctx *ctx, const char *s, unsigned long n)
{
    js_string *r;

    if (n > ctx->cfg.max_string) {
        js_throw_error(ctx, JS_ERR_RANGE, "string too long");
        return 0;
    }
    r = (js_string *)js_alloc_raw(ctx, sizeof(js_string) + n + 1);
    if (!r)
        return 0;
    r->len = (uint32_t)n;
    r->inext = 0;
    if (n && s)
        memcpy(r->data, s, n);
    r->data[n] = 0;
    r->hash = str_hash(r->data, n);
    return r;
}

static int intern_grow(js_ctx *ctx)
{
    uint32_t ncap = ctx->intern_cap ? ctx->intern_cap * 2 : 256;
    js_string **nt = (js_string **)js_alloc(ctx, ncap * sizeof(js_string *));
    uint32_t i;

    if (!nt)
        return 0;
    for (i = 0; i < ctx->intern_cap; i++) {
        js_string *s = ctx->intern[i];
        while (s) {
            js_string *nx = s->inext;
            uint32_t b = s->hash & (ncap - 1);
            s->inext = nt[b];
            nt[b] = s;
            s = nx;
        }
    }
    ctx->intern = nt;
    ctx->intern_cap = ncap;
    return 1;
}

js_string *js_str_intern(js_ctx *ctx, const char *s, unsigned long n)
{
    uint32_t h, b;
    js_string *p;

    if (!ctx->intern_cap && !intern_grow(ctx))
        return 0;
    h = str_hash(s, n);
    b = h & (ctx->intern_cap - 1);
    for (p = ctx->intern[b]; p; p = p->inext)
        if (p->hash == h && p->len == n && memcmp(p->data, s, n) == 0)
            return p;
    p = str_raw(ctx, s, n);
    if (!p)
        return 0;
    if (ctx->intern_n + 1 > ctx->intern_cap * 2) {
        if (!intern_grow(ctx))
            return 0;
        b = h & (ctx->intern_cap - 1);
    }
    p->inext = ctx->intern[b];
    ctx->intern[b] = p;
    ctx->intern_n++;
    return p;
}

js_string *js_str_new(js_ctx *ctx, const char *s, unsigned long n)
{
    if (n <= JS_INTERN_MAX)
        return js_str_intern(ctx, s, n);
    return str_raw(ctx, s, n);
}

js_string *js_str_newz(js_ctx *ctx, const char *s)
{
    return js_str_new(ctx, s, s ? strlen(s) : 0);
}

js_string *js_str_cat(js_ctx *ctx, js_string *a, js_string *b)
{
    unsigned long n;
    js_string *r;

    if (!a || !b)
        return 0;
    if (a->len == 0) return b;
    if (b->len == 0) return a;
    n = (unsigned long)a->len + b->len;
    if (n > ctx->cfg.max_string) {
        js_throw_error(ctx, JS_ERR_RANGE, "string too long");
        return 0;
    }
    if (n <= JS_INTERN_MAX) {
        char tmp[JS_INTERN_MAX + 1];
        memcpy(tmp, a->data, a->len);
        memcpy(tmp + a->len, b->data, b->len);
        return js_str_intern(ctx, tmp, n);
    }
    r = (js_string *)js_alloc_raw(ctx, sizeof(js_string) + n + 1);
    if (!r)
        return 0;
    r->len = (uint32_t)n;
    r->inext = 0;
    memcpy(r->data, a->data, a->len);
    memcpy(r->data + a->len, b->data, b->len);
    r->data[n] = 0;
    r->hash = str_hash(r->data, n);
    return r;
}

int js_str_eq(js_string *a, js_string *b)
{
    if (a == b)
        return 1;
    if (!a || !b)
        return 0;
    return a->hash == b->hash && a->len == b->len &&
           memcmp(a->data, b->data, a->len) == 0;
}

js_value js_mkstring(js_ctx *ctx, const char *s, unsigned long n)
{
    js_string *r = js_str_new(ctx, s, n);
    return r ? js_string_value(r) : js_undefined();
}

js_value js_mkcstring(js_ctx *ctx, const char *s)
{
    return js_mkstring(ctx, s, s ? strlen(s) : 0);
}

/* ================================================================== */
/* Property tables                                                     */
/* ================================================================== */

static int rehash(js_ctx *ctx, js_object *o)
{
    uint32_t n = 8, i;
    uint32_t *b;

    while (n < o->nprops * 2)
        n *= 2;
    b = (uint32_t *)js_alloc(ctx, n * sizeof(uint32_t));
    if (!b)
        return 0;
    o->buckets = b;
    o->nbuckets = n;
    for (i = 0; i < o->nprops; i++) {
        uint32_t k = o->props[i].key->hash & (n - 1);
        o->props[i].hnext = b[k];
        b[k] = i + 1;
    }
    return 1;
}

static js_prop *find_prop(js_object *o, js_string *key, int with_dead)
{
    uint32_t i;

    if (!o->nbuckets)
        return 0;
    i = o->buckets[key->hash & (o->nbuckets - 1)];
    while (i) {
        js_prop *p = &o->props[i - 1];
        if (p->key == key ||
            (p->key->hash == key->hash && p->key->len == key->len &&
             memcmp(p->key->data, key->data, key->len) == 0)) {
            if ((p->flags & JS_P_DEAD) && !with_dead)
                return 0;
            return p;
        }
        i = p->hnext;
    }
    return 0;
}

js_prop *js_own_prop(js_object *o, js_string *key)
{
    return find_prop(o, key, 0);
}

js_prop *js_add_prop(js_ctx *ctx, js_object *o, js_string *key, uint32_t flags)
{
    js_prop *p = find_prop(o, key, 1);

    if (p) {
        if (p->flags & JS_P_DEAD) {
            p->flags = flags;
            p->value = js_undefined();
            p->setter = js_undefined();
        }
        return p;
    }
    if (o->nprops == o->cprops) {
        uint32_t nc = o->cprops ? o->cprops * 2 : 4;
        js_prop *np = (js_prop *)js_alloc(ctx, nc * sizeof(js_prop));
        if (!np)
            return 0;
        if (o->nprops)
            memcpy(np, o->props, o->nprops * sizeof(js_prop));
        o->props = np;
        o->cprops = nc;
    }
    p = &o->props[o->nprops];
    p->key = key;
    p->value = js_undefined();
    p->setter = js_undefined();
    p->flags = flags;
    p->hnext = 0;
    o->nprops++;
    if (!o->nbuckets || o->nprops * 2 > o->nbuckets) {
        if (!rehash(ctx, o))
            return 0;
    } else {
        uint32_t k = key->hash & (o->nbuckets - 1);
        p->hnext = o->buckets[k];
        o->buckets[k] = o->nprops;
    }
    return p;
}

js_object *js_obj_alloc(js_ctx *ctx, int cls, js_object *proto)
{
    js_object *o = (js_object *)js_alloc(ctx, sizeof(js_object));

    if (!o)
        return 0;
    o->cls = (uint8_t)cls;
    o->extensible = 1;
    o->proto = proto;
    o->prim = js_undefined();
    o->next = ctx->objects;
    ctx->objects = o;
    return o;
}

js_object *js_new_object_proto(js_ctx *ctx, js_object *proto)
{
    return js_obj_alloc(ctx, JS_CLASS_OBJECT, proto);
}

js_object *js_new_object(js_ctx *ctx)
{
    return js_obj_alloc(ctx, JS_CLASS_OBJECT, ctx->proto[P_OBJECT]);
}

js_object *js_new_array(js_ctx *ctx)
{
    return js_obj_alloc(ctx, JS_CLASS_ARRAY, ctx->proto[P_ARRAY]);
}

js_env *js_env_new(js_ctx *ctx, js_env *parent, js_object *vars)
{
    js_env *e = (js_env *)js_alloc(ctx, sizeof(js_env));

    if (!e)
        return 0;
    e->parent = parent;
    e->vars = vars;
    e->this_val = parent ? parent->this_val : js_undefined();
    e->next = ctx->envs;
    ctx->envs = e;
    return e;
}

/* ================================================================== */
/* Array index handling                                                */
/* ================================================================== */

long js_array_index(js_string *key)
{
    unsigned long i;
    unsigned long v = 0;

    if (key->len == 0 || key->len > 10)
        return -1;
    if (key->data[0] == '0' && key->len > 1)
        return -1;
    for (i = 0; i < key->len; i++) {
        char c = key->data[i];
        if (c < '0' || c > '9')
            return -1;
        v = v * 10 + (unsigned long)(c - '0');
    }
    if (v >= 0xFFFFFFFFUL)
        return -1;
    return (long)v;
}

/* Largest index gap we will fill with undefined before giving up on the
 * dense representation. Keeps a[1e9] = 1 from allocating 8 GB. */
#define JS_ARRAY_GAP 4096

static int array_reserve(js_ctx *ctx, js_object *o, uint32_t need)
{
    uint32_t nc;
    js_value *ne;

    if (need <= o->ecap)
        return 1;
    nc = o->ecap ? o->ecap * 2 : 8;
    while (nc < need)
        nc *= 2;
    if ((unsigned long)nc * sizeof(js_value) > ctx->cfg.max_heap)
        return js_oom(ctx) == JS_OK;
    ne = (js_value *)js_alloc(ctx, (unsigned long)nc * sizeof(js_value));
    if (!ne)
        return 0;
    if (o->elen)
        memcpy(ne, o->elems, o->elen * sizeof(js_value));
    o->elems = ne;
    o->ecap = nc;
    return 1;
}

static int array_set_len(js_ctx *ctx, js_object *o, uint32_t n)
{
    if (n < o->elen) {
        o->elen = n;
        return 1;
    }
    if (n == o->elen)
        return 1;
    if (n - o->elen > JS_ARRAY_GAP + 1) {
        /* Refuse to materialise an enormous hole run. */
        return js_throw_error(ctx, JS_ERR_RANGE,
                              "array length %u is too large for this engine",
                              (unsigned)n) == JS_OK;
    }
    if (!array_reserve(ctx, o, n))
        return 0;
    while (o->elen < n)
        o->elems[o->elen++] = js_undefined();
    return 1;
}

unsigned long js_array_length(js_object *a)
{
    return a ? a->elen : 0;
}

int js_array_push(js_ctx *ctx, js_object *a, js_value v)
{
    if (!array_reserve(ctx, a, a->elen + 1))
        return JS_THROW;
    a->elems[a->elen++] = v;
    return JS_OK;
}

int js_array_get(js_ctx *ctx, js_object *a, unsigned long i, js_value *out)
{
    (void)ctx;
    if (i < a->elen) { *out = a->elems[i]; return JS_OK; }
    *out = js_undefined();
    return JS_OK;
}

/* ================================================================== */
/* Property get / put / delete                                         */
/* ================================================================== */

int js_obj_get(js_ctx *ctx, js_object *o, js_string *key,
               js_value this_val, js_value *out)
{
    js_object *p = o;
    long idx;
    int guard = 0;

    *out = js_undefined();
    for (; p && guard < 1000; p = p->proto, guard++) {
        js_prop *pr;

        if (p->cls == JS_CLASS_ARRAY) {
            if (key == ctx->s_length || js_str_eq(key, ctx->s_length)) {
                *out = js_number((double)p->elen);
                return JS_OK;
            }
            idx = js_array_index(key);
            if (idx >= 0 && (uint32_t)idx < p->elen) {
                *out = p->elems[idx];
                return JS_OK;
            }
        } else if (p->cls == JS_CLASS_STRING && p->prim.type == JS_STRING) {
            if (js_str_eq(key, ctx->s_length)) {
                *out = js_number((double)p->prim.u.str->len);
                return JS_OK;
            }
            idx = js_array_index(key);
            if (idx >= 0 && (uint32_t)idx < p->prim.u.str->len) {
                *out = js_mkstring(ctx, p->prim.u.str->data + idx, 1);
                return JS_OK;
            }
        }
        pr = js_own_prop(p, key);
        if (pr) {
            if (pr->flags & JS_P_ACCESSOR) {
                if (pr->value.type != JS_OBJECT)
                    return JS_OK;              /* getter-less accessor */
                return js_call(ctx, pr->value, this_val, 0, 0, out);
            }
            *out = pr->value;
            return JS_OK;
        }
    }
    return JS_OK;
}

/* Find an inherited accessor or a read-only data property that blocks a
 * plain [[Put]]. Returns the property, or NULL. */
static js_prop *find_setter(js_object *o, js_string *key)
{
    js_object *p;
    int guard = 0;

    for (p = o; p && guard < 1000; p = p->proto, guard++) {
        js_prop *pr = js_own_prop(p, key);
        if (pr)
            return pr;
    }
    return 0;
}

int js_obj_put(js_ctx *ctx, js_object *o, js_string *key, js_value v)
{
    js_prop *pr;
    long idx;

    if (o->cls == JS_CLASS_ARRAY) {
        if (js_str_eq(key, ctx->s_length)) {
            double d;
            uint32_t n;
            if (js_to_number(ctx, v, &d) != JS_OK)
                return JS_THROW;
            if (d < 0 || d > 4294967295.0 || d != js_floor(d))
                return js_throw_error(ctx, JS_ERR_RANGE, "invalid array length");
            n = (uint32_t)d;
            if (!array_set_len(ctx, o, n))
                return JS_THROW;
            return JS_OK;
        }
        idx = js_array_index(key);
        if (idx >= 0) {
            uint32_t i = (uint32_t)idx;
            if (i < o->elen) { o->elems[i] = v; return JS_OK; }
            if (i <= o->elen + JS_ARRAY_GAP) {
                if (!array_reserve(ctx, o, i + 1))
                    return JS_THROW;
                while (o->elen < i)
                    o->elems[o->elen++] = js_undefined();
                o->elems[o->elen++] = v;
                return JS_OK;
            }
            o->sparse = 1;      /* fall through to a named property */
        }
    }

    pr = js_own_prop(o, key);
    if (!pr) {
        js_prop *inh = o->proto ? find_setter(o->proto, key) : 0;
        if (inh && (inh->flags & JS_P_ACCESSOR)) {
            if (inh->setter.type != JS_OBJECT)
                return JS_OK;                   /* setter-less: silently drop */
            return js_call(ctx, inh->setter, js_object_value(o), 1, &v, 0);
        }
        if (inh && !(inh->flags & JS_P_WRITE))
            return JS_OK;
        if (!o->extensible)
            return JS_OK;
        pr = js_add_prop(ctx, o, key, JS_P_DEFAULT);
        if (!pr)
            return JS_THROW;
        pr->value = v;
        return JS_OK;
    }
    if (pr->flags & JS_P_ACCESSOR) {
        if (pr->setter.type != JS_OBJECT)
            return JS_OK;
        return js_call(ctx, pr->setter, js_object_value(o), 1, &v, 0);
    }
    if (!(pr->flags & JS_P_WRITE))
        return JS_OK;
    pr->value = v;
    return JS_OK;
}

int js_obj_delete(js_ctx *ctx, js_object *o, js_string *key)
{
    js_prop *pr;
    long idx;

    if (o->cls == JS_CLASS_ARRAY) {
        idx = js_array_index(key);
        if (idx >= 0 && (uint32_t)idx < o->elen) {
            if ((uint32_t)idx == o->elen - 1)
                o->elen--;
            else
                o->elems[idx] = js_undefined();
            return 1;
        }
    }
    pr = js_own_prop(o, key);
    if (!pr)
        return 1;
    if (!(pr->flags & JS_P_CONFIG))
        return 0;
    pr->flags = JS_P_DEAD;
    pr->value = js_undefined();
    pr->setter = js_undefined();
    (void)ctx;
    return 1;
}

int js_obj_has_own(js_ctx *ctx, js_object *o, js_string *key)
{
    long idx;

    if (o->cls == JS_CLASS_ARRAY) {
        if (js_str_eq(key, ctx->s_length))
            return 1;
        idx = js_array_index(key);
        if (idx >= 0 && (uint32_t)idx < o->elen)
            return 1;
    } else if (o->cls == JS_CLASS_STRING && o->prim.type == JS_STRING) {
        if (js_str_eq(key, ctx->s_length))
            return 1;
        idx = js_array_index(key);
        if (idx >= 0 && (uint32_t)idx < o->prim.u.str->len)
            return 1;
    }
    return js_own_prop(o, key) != 0;
}

int js_obj_has(js_ctx *ctx, js_object *o, js_string *key)
{
    js_object *p;
    int guard = 0;

    for (p = o; p && guard < 1000; p = p->proto, guard++)
        if (js_obj_has_own(ctx, p, key))
            return 1;
    return 0;
}

/* ---- public wrappers, including the primitive receivers ---- */

int js_wrap_get(js_ctx *ctx, js_value base, js_string *key, js_value *out)
{
    *out = js_undefined();
    switch (base.type) {
    case JS_UNDEFINED:
    case JS_NULL:
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "cannot read property '%s' of %s",
                              key->data,
                              base.type == JS_NULL ? "null" : "undefined");
    case JS_STRING: {
        long idx;
        if (js_str_eq(key, ctx->s_length)) {
            *out = js_number((double)base.u.str->len);
            return JS_OK;
        }
        idx = js_array_index(key);
        if (idx >= 0) {
            if ((uint32_t)idx < base.u.str->len)
                *out = js_mkstring(ctx, base.u.str->data + idx, 1);
            return JS_OK;
        }
        return js_obj_get(ctx, ctx->proto[P_STRING], key, base, out);
    }
    case JS_NUMBER:
        return js_obj_get(ctx, ctx->proto[P_NUMBER], key, base, out);
    case JS_BOOL:
        return js_obj_get(ctx, ctx->proto[P_BOOLEAN], key, base, out);
    default:
        return js_obj_get(ctx, base.u.obj, key, base, out);
    }
}

int js_get_value(js_ctx *ctx, js_value obj, js_value key, js_value *out)
{
    js_value ks;

    if (js_to_string(ctx, key, &ks) != JS_OK)
        return JS_THROW;
    return js_wrap_get(ctx, obj, ks.u.str, out);
}

int js_get(js_ctx *ctx, js_value obj, const char *name, js_value *out)
{
    js_string *k = js_str_intern(ctx, name, strlen(name));
    if (!k) return JS_THROW;
    return js_wrap_get(ctx, obj, k, out);
}

int js_set_value(js_ctx *ctx, js_value obj, js_value key, js_value v)
{
    js_value ks;

    if (obj.type == JS_UNDEFINED || obj.type == JS_NULL)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "cannot set property of %s",
                              obj.type == JS_NULL ? "null" : "undefined");
    if (js_to_string(ctx, key, &ks) != JS_OK)
        return JS_THROW;
    if (obj.type != JS_OBJECT)
        return JS_OK;               /* writes to primitives are dropped */
    return js_obj_put(ctx, obj.u.obj, ks.u.str, v);
}

int js_set(js_ctx *ctx, js_value obj, const char *name, js_value v)
{
    js_string *k = js_str_intern(ctx, name, strlen(name));
    if (!k) return JS_THROW;
    if (obj.type != JS_OBJECT)
        return JS_OK;
    return js_obj_put(ctx, obj.u.obj, k, v);
}

int js_has(js_ctx *ctx, js_value obj, const char *name)
{
    js_string *k = js_str_intern(ctx, name, strlen(name));
    if (!k || obj.type != JS_OBJECT)
        return 0;
    return js_obj_has(ctx, obj.u.obj, k);
}

int js_delete(js_ctx *ctx, js_value obj, const char *name)
{
    js_string *k = js_str_intern(ctx, name, strlen(name));
    if (!k || obj.type != JS_OBJECT)
        return 1;
    return js_obj_delete(ctx, obj.u.obj, k);
}

int js_define(js_ctx *ctx, js_object *o, const char *name, js_value v)
{
    js_string *k = js_str_intern(ctx, name, strlen(name));
    js_prop *p;

    if (!k) return JS_THROW;
    p = js_add_prop(ctx, o, k, JS_P_HIDDEN);
    if (!p) return JS_THROW;
    p->flags = JS_P_HIDDEN;
    p->value = v;
    return JS_OK;
}

int js_define_enum(js_ctx *ctx, js_object *o, const char *name, js_value v)
{
    js_string *k = js_str_intern(ctx, name, strlen(name));
    js_prop *p;

    if (!k) return JS_THROW;
    p = js_add_prop(ctx, o, k, JS_P_DEFAULT);
    if (!p) return JS_THROW;
    p->flags = JS_P_DEFAULT;
    p->value = v;
    return JS_OK;
}

js_value js_global(js_ctx *ctx) { return js_object_value(ctx->global); }

/* ---- host objects ---- */

js_object *js_new_host(js_ctx *ctx, void *host, uint32_t tag, js_object *proto)
{
    js_object *o = js_obj_alloc(ctx, JS_CLASS_HOST,
                                proto ? proto : ctx->proto[P_OBJECT]);
    if (!o)
        return 0;
    o->host = host;
    o->host_tag = tag;
    return o;
}

void *js_host_ptr(js_value v, uint32_t tag)
{
    if (v.type != JS_OBJECT || v.u.obj->cls != JS_CLASS_HOST)
        return 0;
    if (v.u.obj->host_tag != tag)
        return 0;
    return v.u.obj->host;
}

int js_host_finalizer(js_ctx *ctx, uint32_t tag, js_host_free fn)
{
    if (ctx->nhostfin >= (int)(sizeof(ctx->hostfin) / sizeof(ctx->hostfin[0])))
        return -1;
    ctx->hostfin[ctx->nhostfin].tag = tag;
    ctx->hostfin[ctx->nhostfin].fn = fn;
    ctx->nhostfin++;
    return 0;
}

void js_run_host_finalizers(js_ctx *ctx)
{
    js_object *o;
    int i;

    if (!ctx->nhostfin)
        return;
    for (o = ctx->objects; o; o = o->next) {
        if (o->cls != JS_CLASS_HOST || !o->host)
            continue;
        for (i = 0; i < ctx->nhostfin; i++)
            if (ctx->hostfin[i].tag == o->host_tag) {
                ctx->hostfin[i].fn(ctx->cfg.user, o->host);
                o->host = 0;
                break;
            }
    }
}

/* ================================================================== */
/* Exceptions                                                          */
/* ================================================================== */

int js_throw(js_ctx *ctx, js_value v)
{
    if (ctx->fatal)
        return JS_THROW;
    ctx->exception = v;
    ctx->has_exception = 1;
    return JS_THROW;
}

js_value js_exception(js_ctx *ctx)
{
    return ctx->has_exception ? ctx->exception : js_undefined();
}

int js_throw_error(js_ctx *ctx, int kind, const char *fmt, ...)
{
    char buf[240];
    va_list ap;
    js_object *e;

    if (ctx->fatal)
        return JS_THROW;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    e = js_new_error(ctx, kind, buf);
    if (!e)
        return JS_THROW;
    return js_throw(ctx, js_object_value(e));
}

const char *js_error_text(js_ctx *ctx, js_value v)
{
    js_value name, msg;
    const char *ns = "Error", *ms = "";

    if (ctx->fatal && ctx->fatal_msg) {
        snprintf(ctx->msgbuf, sizeof(ctx->msgbuf), "FatalError: %s",
                 ctx->fatal_msg);
        return ctx->msgbuf;
    }
    if (v.type == JS_OBJECT && v.u.obj->cls == JS_CLASS_ERROR) {
        if (js_obj_get(ctx, v.u.obj, ctx->s_name, v, &name) == JS_OK &&
            name.type == JS_STRING)
            ns = name.u.str->data;
        if (js_obj_get(ctx, v.u.obj, ctx->s_message, v, &msg) == JS_OK &&
            msg.type == JS_STRING)
            ms = msg.u.str->data;
        if (ms[0])
            snprintf(ctx->msgbuf, sizeof(ctx->msgbuf), "%s: %s", ns, ms);
        else
            snprintf(ctx->msgbuf, sizeof(ctx->msgbuf), "%s", ns);
        return ctx->msgbuf;
    }
    {
        js_value s;
        int saved = ctx->has_exception;
        js_value savedv = ctx->exception;
        ctx->has_exception = 0;
        if (js_to_string(ctx, v, &s) == JS_OK && s.type == JS_STRING)
            snprintf(ctx->msgbuf, sizeof(ctx->msgbuf), "%s", s.u.str->data);
        else
            snprintf(ctx->msgbuf, sizeof(ctx->msgbuf), "<unprintable>");
        ctx->has_exception = saved;
        ctx->exception = savedv;
    }
    return ctx->msgbuf;
}

/* ================================================================== */
/* Doubles: classification and the libm substitutes                    */
/* ================================================================== */

static uint64_t dbits(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static double   dfrom(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }

int js_isnan(double d)
{
    uint64_t b = dbits(d);
    return ((b >> 52) & 0x7FF) == 0x7FF && (b & 0xFFFFFFFFFFFFFULL) != 0;
}

int js_isinf(double d)
{
    uint64_t b = dbits(d);
    return ((b >> 52) & 0x7FF) == 0x7FF && (b & 0xFFFFFFFFFFFFFULL) == 0;
}

double js_nan(void) { return dfrom(0x7FF8000000000000ULL); }
double js_inf(int neg) { return dfrom(neg ? 0xFFF0000000000000ULL : 0x7FF0000000000000ULL); }
double js_fabs(double d) { return dfrom(dbits(d) & 0x7FFFFFFFFFFFFFFFULL); }

double js_trunc(double d)
{
    uint64_t b = dbits(d);
    int e = (int)((b >> 52) & 0x7FF) - 1023;

    if (e < 0)
        return (b >> 63) ? -0.0 : 0.0;
    if (e >= 52)
        return d;                       /* integral already, or inf/nan */
    b &= ~((1ULL << (52 - e)) - 1);
    return dfrom(b);
}

double js_floor(double d)
{
    double t = js_trunc(d);
    if (t == d || js_isnan(d))
        return d == 0 ? d : t;
    return d < 0 ? t - 1.0 : t;
}

double js_ceil(double d)
{
    double t = js_trunc(d);
    if (t == d || js_isnan(d))
        return d == 0 ? d : t;
    if (d < 0 && t == 0.0)
        return -0.0;
    return d > 0 ? t + 1.0 : t;
}

double js_round(double d)
{
    double f;
    if (js_isnan(d) || js_isinf(d) || d == 0)
        return d;
    f = js_floor(d);
    if (d - f >= 0.5)
        f += 1.0;
    if (f == 0.0 && d < 0)
        return -0.0;
    return f;
}

double js_sqrt(double d)
{
    double r;
    __asm__ ("sqrtsd %1, %0" : "=x" (r) : "x" (d));
    return r;
}

double js_fmod(double a, double b)
{
    double x, y, yy;
    int k, i;

    if (js_isnan(a) || js_isnan(b) || js_isinf(a) || b == 0)
        return js_nan();
    if (js_isinf(b) || a == 0)
        return a;
    x = js_fabs(a);
    y = js_fabs(b);
    if (x < y)
        return a;
    yy = y;
    k = 0;
    while (yy * 2.0 <= x && k < 2100) { yy *= 2.0; k++; }
    for (i = k; i >= 0; i--) {
        if (yy <= x)
            x -= yy;
        yy *= 0.5;
    }
    return a < 0 ? -x : x;
}

/* ---- exp / log, computed in x87 extended precision ---- */

static long double expl_core(long double x)
{
    static const long double LN2HI = 0.693359375L;
    static const long double LN2LO = -2.121944400546905827679e-4L;
    long double r, s, t;
    int k, i;

    if (x > 11400.0L) return (long double)js_inf(0);
    if (x < -11400.0L) return 0.0L;
    k = (int)(x * 1.44269504088896340736L + (x < 0 ? -0.5L : 0.5L));
    r = x - (long double)k * LN2HI;
    r -= (long double)k * LN2LO;
    /* sum r^n / n! */
    s = 1.0L;
    t = 1.0L;
    for (i = 1; i <= 18; i++) {
        t *= r / (long double)i;
        s += t;
    }
    /* scale by 2^k, in two steps so extreme k does not overflow long double */
    while (k > 1000) { s *= 1.0715086071862673e301L; k -= 1000; }
    while (k < -1000) { s *= 9.332636185032189e-302L; k += 1000; }
    {
        long double p = 1.0L;
        int n = k < 0 ? -k : k;
        long double base = 2.0L;
        while (n) {
            if (n & 1) p *= base;
            base *= base;
            n >>= 1;
        }
        s = k < 0 ? s / p : s * p;
    }
    return s;
}

static long double logl_core(double x)
{
    uint64_t b = dbits(x);
    int e;
    long double m, s, s2, sum, term;
    int i;

    if (x <= 0)
        return 0.0L;
    e = (int)((b >> 52) & 0x7FF);
    if (e == 0) {                       /* denormal: scale up first */
        x *= 18014398509481984.0;       /* 2^54 */
        b = dbits(x);
        e = (int)((b >> 52) & 0x7FF);
        e -= 54;
    }
    e -= 1023;
    m = (long double)dfrom((b & 0x000FFFFFFFFFFFFFULL) | (1023ULL << 52));
    if (m > 1.4142135623730950488L) { m /= 2.0L; e += 1; }
    s = (m - 1.0L) / (m + 1.0L);
    s2 = s * s;
    sum = 0.0L;
    term = s;
    for (i = 1; i <= 31; i += 2) {
        sum += term / (long double)i;
        term *= s2;
    }
    return 2.0L * sum + (long double)e * 0.6931471805599453094172321L;
}

double js_exp(double d)
{
    if (js_isnan(d)) return d;
    if (js_isinf(d)) return d > 0 ? d : 0.0;
    return (double)expl_core((long double)d);
}

double js_log(double d)
{
    if (js_isnan(d)) return d;
    if (d < 0) return js_nan();
    if (d == 0) return js_inf(1);
    if (js_isinf(d)) return d;
    return (double)logl_core(d);
}

double js_pow(double x, double y)
{
    double ay;

    if (js_isnan(y))
        return js_nan();
    if (y == 0)
        return 1.0;
    if (js_isnan(x))
        return js_nan();
    ay = js_fabs(y);
    if (js_isinf(y)) {
        double ax = js_fabs(x);
        if (ax == 1.0) return js_nan();
        if (ax > 1.0) return y > 0 ? js_inf(0) : 0.0;
        return y > 0 ? 0.0 : js_inf(0);
    }
    if (js_isinf(x)) {
        if (x > 0) return y > 0 ? js_inf(0) : 0.0;
        {
            int odd = (ay == js_trunc(ay)) && js_fmod(ay, 2.0) == 1.0;
            if (y > 0) return odd ? js_inf(1) : js_inf(0);
            return odd ? -0.0 : 0.0;
        }
    }
    if (x == 0) {
        int odd = (ay == js_trunc(ay)) && js_fmod(ay, 2.0) == 1.0;
        int negzero = (dbits(x) >> 63) != 0;
        if (y > 0) return (odd && negzero) ? -0.0 : 0.0;
        return (odd && negzero) ? js_inf(1) : js_inf(0);
    }
    if (x < 0 && y != js_trunc(y))
        return js_nan();

    /* Exact integer exponents by repeated squaring: keeps 2**10 at 1024. */
    if (y == js_trunc(y) && ay <= 1024.0) {
        double r = 1.0, base = x;
        long n = (long)ay;
        while (n) {
            if (n & 1) r *= base;
            base *= base;
            n >>= 1;
            if (n && (js_isinf(base) || base == 0))
                break;
        }
        if (n) {   /* base saturated; finish in extended precision instead */
            r = (double)expl_core((long double)y * logl_core(js_fabs(x)));
            if (x < 0 && js_fmod(ay, 2.0) == 1.0)
                r = -r;
            return r;
        }
        return y < 0 ? 1.0 / r : r;
    }
    {
        double r = (double)expl_core((long double)y * logl_core(js_fabs(x)));
        if (x < 0 && js_fmod(ay, 2.0) == 1.0)
            r = -r;
        return r;
    }
}

/* ---- trigonometry ---- */

#define JS_PI 3.14159265358979323846264338328L

static void sincos_core(long double r, long double *sr, long double *cr)
{
    long double r2 = r * r, t, s, c;
    int i;

    s = r; t = r;
    for (i = 1; i <= 9; i++) {
        t *= -r2 / (long double)((2 * i) * (2 * i + 1));
        s += t;
    }
    c = 1.0L; t = 1.0L;
    for (i = 1; i <= 9; i++) {
        t *= -r2 / (long double)((2 * i - 1) * (2 * i));
        c += t;
    }
    *sr = s; *cr = c;
}

/* Reduce x to r in [-pi/4, pi/4] and the quadrant n. */
static int trig_reduce(double x, long double *r)
{
    long double v = (long double)x;
    long double q = v / (JS_PI / 2.0L);
    long double n;
    int quad;

    n = (q < 0) ? -(long double)(unsigned long long)(-q + 0.5L)
                : (long double)(unsigned long long)(q + 0.5L);
    /* Cody-Waite: pi/2 in three pieces so the subtraction stays accurate. */
    v -= n * 1.57079632679489661923L;
    v -= n * 6.123233995736766035868e-17L;
    v -= n * (-1.4973849048591698e-33L);
    *r = v;
    quad = (int)js_fmod((double)n, 4.0);
    if (quad < 0)
        quad += 4;
    return quad;
}

double js_sin(double x)
{
    long double r, s, c;
    int q;

    if (js_isnan(x) || js_isinf(x)) return js_nan();
    if (x == 0) return x;
    q = trig_reduce(x, &r);
    sincos_core(r, &s, &c);
    switch (q) {
    case 0: return (double)s;
    case 1: return (double)c;
    case 2: return (double)(-s);
    default: return (double)(-c);
    }
}

double js_cos(double x)
{
    long double r, s, c;
    int q;

    if (js_isnan(x) || js_isinf(x)) return js_nan();
    q = trig_reduce(x, &r);
    sincos_core(r, &s, &c);
    switch (q) {
    case 0: return (double)c;
    case 1: return (double)(-s);
    case 2: return (double)(-c);
    default: return (double)s;
    }
}

double js_tan(double x)
{
    long double r, s, c;
    int q;

    if (js_isnan(x) || js_isinf(x)) return js_nan();
    if (x == 0) return x;
    q = trig_reduce(x, &r);
    sincos_core(r, &s, &c);
    if (q & 1)
        return (double)(-c / s);
    return (double)(s / c);
}

static long double atan_core(long double x)
{
    /* atan(x) for |x| <= tan(pi/12) via the alternating series */
    long double x2 = x * x, t = x, sum = 0.0L;
    int i;
    for (i = 1; i <= 41; i += 2) {
        sum += ((i & 2) ? -t : t) / (long double)i;
        t *= x2;
    }
    return sum;
}

double js_atan(double x)
{
    long double v, r;
    int neg = 0, inv = 0, shift = 0;

    if (js_isnan(x)) return x;
    if (js_isinf(x)) return x > 0 ? (double)(JS_PI / 2) : (double)(-JS_PI / 2);
    v = (long double)x;
    if (v < 0) { v = -v; neg = 1; }
    if (v > 1.0L) { v = 1.0L / v; inv = 1; }
    if (v > 0.2679491924311227065L) {        /* tan(pi/12) */
        v = (v * 1.7320508075688772935L - 1.0L) /
            (1.7320508075688772935L + v);
        shift = 1;
    }
    r = atan_core(v);
    if (shift) r += JS_PI / 6.0L;
    if (inv) r = JS_PI / 2.0L - r;
    return (double)(neg ? -r : r);
}

double js_asin(double x)
{
    if (js_isnan(x)) return x;
    if (x > 1.0 || x < -1.0) return js_nan();
    if (x == 1.0) return (double)(JS_PI / 2);
    if (x == -1.0) return (double)(-JS_PI / 2);
    return js_atan(x / js_sqrt(1.0 - x * x));
}

double js_acos(double x)
{
    if (js_isnan(x)) return x;
    if (x > 1.0 || x < -1.0) return js_nan();
    return (double)(JS_PI / 2) - js_asin(x);
}

double js_atan2(double y, double x)
{
    if (js_isnan(y) || js_isnan(x))
        return js_nan();
    if (js_isinf(y)) {
        if (js_isinf(x))
            return (double)((x > 0 ? 1 : 3) * JS_PI / 4) * (y > 0 ? 1 : -1);
        return y > 0 ? (double)(JS_PI / 2) : (double)(-JS_PI / 2);
    }
    if (js_isinf(x))
        return x > 0 ? (y < 0 || (dbits(y) >> 63) ? -0.0 : 0.0)
                     : (y < 0 || (dbits(y) >> 63) ? (double)(-JS_PI) : (double)JS_PI);
    if (x == 0) {
        if (y == 0)
            return (dbits(x) >> 63) ? ((dbits(y) >> 63) ? (double)(-JS_PI) : (double)JS_PI)
                                    : ((dbits(y) >> 63) ? -0.0 : 0.0);
        return y > 0 ? (double)(JS_PI / 2) : (double)(-JS_PI / 2);
    }
    if (y == 0)
        return (dbits(x) >> 63) ? ((dbits(y) >> 63) ? (double)(-JS_PI) : (double)JS_PI)
                                : y;
    {
        double a = js_atan(y / x);
        if (x > 0)
            return a;
        return y > 0 ? a + (double)JS_PI : a - (double)JS_PI;
    }
}

/* ================================================================== */
/* Fixed-size bignum, used only by the decimal conversions             */
/* ================================================================== */

#define BN_LIMBS 52                     /* 1664 bits; the worst case is ~1130 */

typedef struct { uint32_t v[BN_LIMBS]; int n; } bn;

static void bn_set(bn *a, uint64_t x)
{
    memset(a->v, 0, sizeof(a->v));
    a->v[0] = (uint32_t)x;
    a->v[1] = (uint32_t)(x >> 32);
    a->n = a->v[1] ? 2 : (a->v[0] ? 1 : 0);
}

static void bn_mul_small(bn *a, uint32_t m)
{
    uint64_t carry = 0;
    int i;

    for (i = 0; i < a->n; i++) {
        uint64_t t = (uint64_t)a->v[i] * m + carry;
        a->v[i] = (uint32_t)t;
        carry = t >> 32;
    }
    while (carry && a->n < BN_LIMBS) {
        a->v[a->n++] = (uint32_t)carry;
        carry >>= 32;
    }
}

static void bn_shl(bn *a, int bits)
{
    int words = bits / 32, sh = bits % 32;
    int i;

    if (words) {
        for (i = a->n - 1; i >= 0; i--)
            if (i + words < BN_LIMBS)
                a->v[i + words] = a->v[i];
        for (i = 0; i < words && i < BN_LIMBS; i++)
            a->v[i] = 0;
        a->n += words;
        if (a->n > BN_LIMBS) a->n = BN_LIMBS;
    }
    if (sh) {
        uint32_t carry = 0;
        for (i = 0; i < a->n; i++) {
            uint32_t nv = (a->v[i] << sh) | carry;
            carry = a->v[i] >> (32 - sh);
            a->v[i] = nv;
        }
        if (carry && a->n < BN_LIMBS)
            a->v[a->n++] = carry;
    }
    while (a->n > 0 && a->v[a->n - 1] == 0)
        a->n--;
}

static const uint32_t pow10_u32[10] = {
    1u, 10u, 100u, 1000u, 10000u, 100000u,
    1000000u, 10000000u, 100000000u, 1000000000u
};

static void bn_mul_pow10(bn *a, int n)
{
    while (n >= 9) { bn_mul_small(a, 1000000000u); n -= 9; }
    if (n > 0) bn_mul_small(a, pow10_u32[n]);
}

static int bn_cmp(const bn *a, const bn *b)
{
    int i;
    if (a->n != b->n)
        return a->n > b->n ? 1 : -1;
    for (i = a->n - 1; i >= 0; i--)
        if (a->v[i] != b->v[i])
            return a->v[i] > b->v[i] ? 1 : -1;
    return 0;
}

static void bn_add(bn *d, const bn *a, const bn *b)
{
    uint64_t carry = 0;
    int i, n = a->n > b->n ? a->n : b->n;

    for (i = 0; i < n; i++) {
        uint64_t t = carry;
        t += (i < a->n) ? a->v[i] : 0;
        t += (i < b->n) ? b->v[i] : 0;
        d->v[i] = (uint32_t)t;
        carry = t >> 32;
    }
    if (carry && n < BN_LIMBS)
        d->v[n++] = (uint32_t)carry;
    d->n = n;
    for (i = n; i < BN_LIMBS; i++)
        d->v[i] = 0;
}

static void bn_sub(bn *a, const bn *b)
{
    int64_t borrow = 0;
    int i;

    for (i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->v[i] - borrow - ((i < b->n) ? b->v[i] : 0);
        if (t < 0) { t += 0x100000000LL; borrow = 1; } else borrow = 0;
        a->v[i] = (uint32_t)t;
    }
    while (a->n > 0 && a->v[a->n - 1] == 0)
        a->n--;
}

/* ================================================================== */
/* double -> decimal digits                                            */
/* ================================================================== */

/* Shortest digit string that round-trips. Writes to `out` (>= 24 bytes),
 * returns the digit count, sets *ptout so that value == 0.DIGITS * 10^pt. */
static int shortest_digits(double v, char *out, int *ptout)
{
    uint64_t b = dbits(v);
    int be = (int)((b >> 52) & 0x7FF);
    uint64_t mant = b & 0xFFFFFFFFFFFFFULL;
    uint64_t f;
    int e, boundary, even, pt, guard, nd = 0;
    bn R, S, MP, MM, T;

    if (be == 0) { f = mant; e = -1074; }
    else { f = mant | (1ULL << 52); e = be - 1075; }
    even = (f & 1) == 0;
    boundary = (mant == 0 && be > 1);

    if (e >= 0) {
        if (!boundary) {
            bn_set(&R, f); bn_shl(&R, e + 1);
            bn_set(&S, 2);
            bn_set(&MP, 1); bn_shl(&MP, e);
            bn_set(&MM, 1); bn_shl(&MM, e);
        } else {
            bn_set(&R, f); bn_shl(&R, e + 2);
            bn_set(&S, 4);
            bn_set(&MP, 1); bn_shl(&MP, e + 1);
            bn_set(&MM, 1); bn_shl(&MM, e);
        }
    } else {
        if (!boundary) {
            bn_set(&R, f); bn_shl(&R, 1);
            bn_set(&S, 1); bn_shl(&S, 1 - e);
            bn_set(&MP, 1);
            bn_set(&MM, 1);
        } else {
            bn_set(&R, f); bn_shl(&R, 2);
            bn_set(&S, 1); bn_shl(&S, 2 - e);
            bn_set(&MP, 2);
            bn_set(&MM, 1);
        }
    }

    /* Estimate ceil(log10(v)) from the position of the top set bit; the
     * fix-up loops below correct an error of one either way. Using the bit
     * length rather than a fixed 53 is what keeps denormals right. */
    {
        int fb = 0;
        uint64_t tf = f;
        double lg;
        while (tf) { fb++; tf >>= 1; }
        lg = (double)(e + fb - 1) * 0.30102999566398120 + 0.1505;
        pt = (int)js_ceil(lg);
    }
    if (pt >= 0) bn_mul_pow10(&S, pt);
    else { bn_mul_pow10(&R, -pt); bn_mul_pow10(&MP, -pt); bn_mul_pow10(&MM, -pt); }

    for (guard = 0; guard < 6; guard++) {
        int c;
        bn_add(&T, &R, &MP);
        c = bn_cmp(&T, &S);
        if (even ? (c >= 0) : (c > 0)) { bn_mul_small(&S, 10); pt++; }
        else break;
    }
    for (guard = 0; guard < 6; guard++) {
        int c;
        bn_add(&T, &R, &MP);
        bn_mul_small(&T, 10);
        c = bn_cmp(&T, &S);
        if (even ? (c < 0) : (c <= 0)) {
            bn_mul_small(&R, 10); bn_mul_small(&MP, 10); bn_mul_small(&MM, 10);
            pt--;
        } else break;
    }

    for (;;) {
        int dg = 0, low, high, c;
        bn_mul_small(&R, 10);
        bn_mul_small(&MP, 10);
        bn_mul_small(&MM, 10);
        while (bn_cmp(&R, &S) >= 0) { bn_sub(&R, &S); dg++; }
        c = bn_cmp(&R, &MM);
        low = even ? (c <= 0) : (c < 0);
        bn_add(&T, &R, &MP);
        c = bn_cmp(&T, &S);
        high = even ? (c >= 0) : (c > 0);
        if (!low && !high) {
            out[nd++] = (char)('0' + dg);
            if (nd >= 20) break;
            continue;
        }
        if (low && !high)
            out[nd++] = (char)('0' + dg);
        else if (high && !low)
            out[nd++] = (char)('0' + dg + 1);
        else {
            T = R;
            bn_mul_small(&T, 2);
            c = bn_cmp(&T, &S);
            out[nd++] = (char)('0' + dg + (c > 0 ? 1 : 0));
        }
        break;
    }

    /* propagate any 9+1 carry */
    {
        int i = nd - 1;
        while (i >= 0 && out[i] > '9') {
            out[i] = '0';
            if (i == 0) {
                memmove(out + 1, out, (unsigned long)nd);
                out[0] = '1';
                nd++;
                pt++;
                break;
            }
            out[i - 1]++;
            i--;
        }
    }
    while (nd > 1 && out[nd - 1] == '0')
        nd--;
    *ptout = pt;
    return nd;
}

/* Exactly `count` digits (rounded half-up at that position), for toFixed,
 * toPrecision and toExponential. Returns the digit count actually written
 * (count, or count+1 if a carry lengthened it), sets *ptout. */
static int exact_digits(double v, int count, char *out, int *ptout)
{
    uint64_t b = dbits(v);
    int be = (int)((b >> 52) & 0x7FF);
    uint64_t mant = b & 0xFFFFFFFFFFFFFULL;
    uint64_t f;
    int e, pt, guard, nd = 0, i;
    bn R, S, T;

    if (be == 0) { f = mant; e = -1074; }
    else { f = mant | (1ULL << 52); e = be - 1075; }

    bn_set(&R, f);
    bn_set(&S, 1);
    if (e >= 0) bn_shl(&R, e);
    else bn_shl(&S, -e);

    {
        int fb = 0;
        uint64_t tf = f;
        double lg;
        while (tf) { fb++; tf >>= 1; }
        lg = (double)(e + fb - 1) * 0.30102999566398120 + 0.1505;
        pt = (int)js_ceil(lg);
    }
    if (pt >= 0) bn_mul_pow10(&S, pt);
    else bn_mul_pow10(&R, -pt);

    for (guard = 0; guard < 6; guard++) {
        if (bn_cmp(&R, &S) >= 0) { bn_mul_small(&S, 10); pt++; }
        else break;
    }
    for (guard = 0; guard < 6; guard++) {
        T = R;
        bn_mul_small(&T, 10);
        if (bn_cmp(&T, &S) < 0) { bn_mul_small(&R, 10); pt--; }
        else break;
    }

    if (count < 0)
        count = 0;
    if (count > 40)
        count = 40;
    for (i = 0; i < count; i++) {
        int dg = 0;
        bn_mul_small(&R, 10);
        while (bn_cmp(&R, &S) >= 0) { bn_sub(&R, &S); dg++; }
        out[nd++] = (char)('0' + dg);
    }
    /* round half up on the remaining tail */
    T = R;
    bn_mul_small(&T, 2);
    if (bn_cmp(&T, &S) >= 0) {
        int j = nd - 1;
        for (;;) {
            if (j < 0) {
                memmove(out + 1, out, (unsigned long)nd);
                out[0] = '1';
                nd++;
                pt++;
                break;
            }
            if (out[j] != '9') { out[j]++; break; }
            out[j] = '0';
            j--;
        }
    }
    if (nd == 0) { out[0] = '0'; nd = 1; pt = 1; }
    *ptout = pt;
    return nd;
}

/* ES5 9.8.1 */
void js_dtoa(double d, char *buf)
{
    char digits[32];
    int nd, pt, i, n = 0;

    if (js_isnan(d)) { memcpy(buf, "NaN", 4); return; }
    if (d == 0) { memcpy(buf, "0", 2); return; }
    if (d < 0) { buf[n++] = '-'; d = -d; }
    if (js_isinf(d)) { memcpy(buf + n, "Infinity", 9); return; }

    nd = shortest_digits(d, digits, &pt);

    if (pt >= nd && pt <= 21) {
        for (i = 0; i < nd; i++) buf[n++] = digits[i];
        for (i = nd; i < pt; i++) buf[n++] = '0';
    } else if (pt > 0 && pt <= 21) {
        for (i = 0; i < pt; i++) buf[n++] = digits[i];
        buf[n++] = '.';
        for (i = pt; i < nd; i++) buf[n++] = digits[i];
    } else if (pt <= 0 && pt > -6) {
        buf[n++] = '0';
        buf[n++] = '.';
        for (i = 0; i < -pt; i++) buf[n++] = '0';
        for (i = 0; i < nd; i++) buf[n++] = digits[i];
    } else {
        int ex = pt - 1;
        buf[n++] = digits[0];
        if (nd > 1) {
            buf[n++] = '.';
            for (i = 1; i < nd; i++) buf[n++] = digits[i];
        }
        buf[n++] = 'e';
        buf[n++] = ex < 0 ? '-' : '+';
        if (ex < 0) ex = -ex;
        {
            char t[8];
            int m = 0;
            if (ex == 0) t[m++] = '0';
            while (ex) { t[m++] = (char)('0' + ex % 10); ex /= 10; }
            while (m) buf[n++] = t[--m];
        }
    }
    buf[n] = 0;
}

void js_dtoa_fixed(double d, int frac, char *buf, unsigned long bufsz)
{
    char digits[48];
    int nd, pt, i, n = 0, count;

    if (js_isnan(d) || js_isinf(d) || js_fabs(d) >= 1e21) {
        js_dtoa(d, buf);
        (void)bufsz;
        return;
    }
    if (d < 0) { buf[n++] = '-'; d = -d; }
    if (d == 0) {
        buf[n++] = '0';
        if (frac > 0) { buf[n++] = '.'; for (i = 0; i < frac; i++) buf[n++] = '0'; }
        buf[n] = 0;
        return;
    }
    nd = shortest_digits(d, digits, &pt);
    count = pt + frac;
    if (count < 0) {
        buf[n++] = '0';
        if (frac > 0) { buf[n++] = '.'; for (i = 0; i < frac; i++) buf[n++] = '0'; }
        buf[n] = 0;
        return;
    }
    nd = exact_digits(d, count, digits, &pt);
    if (pt <= 0) {
        buf[n++] = '0';
        if (frac > 0) {
            buf[n++] = '.';
            for (i = 0; i < -pt && i < frac; i++) buf[n++] = '0';
            for (i = 0; i < nd && (int)(n) < (int)bufsz - 2; i++) buf[n++] = digits[i];
            while (n > 0 && (int)n < 2 + frac) buf[n++] = '0';
        }
    } else {
        for (i = 0; i < pt; i++) buf[n++] = (i < nd) ? digits[i] : '0';
        if (frac > 0) {
            int k = 0;
            buf[n++] = '.';
            for (i = pt; i < nd && k < frac; i++, k++) buf[n++] = digits[i];
            while (k++ < frac) buf[n++] = '0';
        }
    }
    buf[n] = 0;
}

void js_dtoa_exponential(double d, int frac, char *buf, unsigned long bufsz)
{
    char digits[48];
    int nd, pt, i, n = 0, ex;

    (void)bufsz;
    if (js_isnan(d) || js_isinf(d)) { js_dtoa(d, buf); return; }
    if (d < 0) { buf[n++] = '-'; d = -d; }
    if (d == 0) {
        nd = 1; digits[0] = '0'; pt = 1;
        while (nd <= frac) digits[nd++] = '0';
    } else if (frac < 0) {
        nd = shortest_digits(d, digits, &pt);
    } else {
        nd = exact_digits(d, frac + 1, digits, &pt);
        if (nd > frac + 1) nd = frac + 1;
    }
    ex = (d == 0) ? 0 : pt - 1;
    buf[n++] = digits[0];
    if (nd > 1) {
        buf[n++] = '.';
        for (i = 1; i < nd; i++) buf[n++] = digits[i];
    }
    buf[n++] = 'e';
    buf[n++] = ex < 0 ? '-' : '+';
    if (ex < 0) ex = -ex;
    {
        char t[8];
        int m = 0;
        if (ex == 0) t[m++] = '0';
        while (ex) { t[m++] = (char)('0' + ex % 10); ex /= 10; }
        while (m) buf[n++] = t[--m];
    }
    buf[n] = 0;
}

void js_dtoa_precision(double d, int prec, char *buf, unsigned long bufsz)
{
    char digits[48];
    int nd, pt, i, n = 0;

    if (js_isnan(d) || js_isinf(d)) { js_dtoa(d, buf); return; }
    if (d < 0) { buf[n++] = '-'; d = -d; }
    if (d == 0) {
        buf[n++] = '0';
        if (prec > 1) { buf[n++] = '.'; for (i = 1; i < prec; i++) buf[n++] = '0'; }
        buf[n] = 0;
        return;
    }
    nd = exact_digits(d, prec, digits, &pt);
    if (nd > prec) nd = prec;
    if (pt - 1 < -6 || pt - 1 >= prec) {
        js_dtoa_exponential(d, prec - 1, buf + (n ? 1 : 0), bufsz);
        return;
    }
    if (pt <= 0) {
        buf[n++] = '0';
        buf[n++] = '.';
        for (i = 0; i < -pt; i++) buf[n++] = '0';
        for (i = 0; i < nd; i++) buf[n++] = digits[i];
    } else {
        for (i = 0; i < pt && i < nd; i++) buf[n++] = digits[i];
        for (i = nd; i < pt; i++) buf[n++] = '0';
        if (pt < nd) {
            buf[n++] = '.';
            for (i = pt; i < nd; i++) buf[n++] = digits[i];
        }
    }
    buf[n] = 0;
}

void js_dtoa_radix(double d, int radix, char *buf, unsigned long bufsz)
{
    static const char dig[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char tmp[80];
    int n = 0, m = 0, i;
    double ip, fp;

    if (radix == 10 || js_isnan(d) || js_isinf(d)) { js_dtoa(d, buf); return; }
    if (d == 0) { memcpy(buf, "0", 2); return; }
    if (d < 0) { buf[n++] = '-'; d = -d; }
    ip = js_floor(d);
    fp = d - ip;
    if (ip == 0) tmp[m++] = '0';
    while (ip >= 1 && m < (int)sizeof(tmp)) {
        double q = js_floor(ip / radix);
        int r = (int)(ip - q * radix);
        tmp[m++] = dig[r];
        ip = q;
    }
    while (m && n < (int)bufsz - 2)
        buf[n++] = tmp[--m];
    if (fp > 0 && n < (int)bufsz - 3) {
        buf[n++] = '.';
        for (i = 0; i < 52 && fp > 0 && n < (int)bufsz - 2; i++) {
            int r;
            fp *= radix;
            r = (int)js_floor(fp);
            if (r >= radix) r = radix - 1;
            buf[n++] = dig[r];
            fp -= r;
        }
    }
    buf[n] = 0;
}

/* ================================================================== */
/* decimal -> double                                                   */
/* ================================================================== */

static long double pow10l(int e)
{
    /* 10^k is exact in the 64-bit x87 significand up to k = 27. */
    static const long double t[28] = {
        1e0L, 1e1L, 1e2L, 1e3L, 1e4L, 1e5L, 1e6L, 1e7L, 1e8L, 1e9L,
        1e10L, 1e11L, 1e12L, 1e13L, 1e14L, 1e15L, 1e16L, 1e17L, 1e18L,
        1e19L, 1e20L, 1e21L, 1e22L, 1e23L, 1e24L, 1e25L, 1e26L, 1e27L
    };
    long double r = 1.0L;
    int neg = e < 0;

    if (neg) e = -e;
    while (e > 27) { r *= t[27]; e -= 27; }
    r *= t[e];
    return neg ? 1.0L / r : r;
}

/* Split a positive finite double into f * 2^e with f an integer. */
static void split_double(double d, uint64_t *f, int *e)
{
    uint64_t b = dbits(d);
    int be = (int)((b >> 52) & 0x7FF);

    if (be == 0) { *f = b & 0xFFFFFFFFFFFFFULL; *e = -1074; }
    else { *f = (b & 0xFFFFFFFFFFFFFULL) | (1ULL << 52); *e = be - 1075; }
}

/* Exact three-way comparison of the decimal m * 10^de against the double d
 * (which must be positive and finite). Both sides are scaled to integers,
 * so there is no rounding anywhere in the comparison. */
static int cmp_dec_double(uint64_t m, int de, double d)
{
    bn A, B;
    uint64_t fd;
    int ed;

    if (d == 0)
        return m ? 1 : 0;
    split_double(d, &fd, &ed);
    bn_set(&A, m);
    bn_set(&B, fd);
    if (de > 0) bn_mul_pow10(&A, de);
    else if (de < 0) bn_mul_pow10(&B, -de);
    if (ed > 0) bn_shl(&B, ed);
    else if (ed < 0) bn_shl(&A, -ed);
    return bn_cmp(&A, &B);
}

static double next_double(double d, int up)
{
    uint64_t b = dbits(d);

    if (d == 0)
        return up ? dfrom(1) : -dfrom(1);
    if (up) b++;
    else b--;
    return dfrom(b);
}

/* Correctly rounded m * 10^de, starting from an approximation and walking
 * to the neighbouring double that is genuinely closest. The approximation
 * is already within an ulp or two, so this converges in a couple of steps. */
static double refine_double(uint64_t m, int de, double d)
{
    bn A, B, U, T;
    uint64_t fd;
    int ed, i, c;

    if (m == 0)
        return 0.0;
    for (i = 0; i < 8; i++) {
        c = cmp_dec_double(m, de, d);
        if (c == 0)
            return d;
        if (c > 0) {
            double up = next_double(d, 1);
            if (js_isinf(up))
                return up;
            if (cmp_dec_double(m, de, up) >= 0) { d = up; continue; }
            break;                       /* d < N < nextup(d) */
        }
        d = next_double(d, 0);
        if (d < 0) return 0.0;
    }
    if (js_isinf(d) || d == 0) {
        /* Both neighbours are at the representable edge; nothing to pick. */
        if (d == 0 && cmp_dec_double(m, de, next_double(0, 1)) > 0)
            return next_double(0, 1);
        return d;
    }

    /* Choose between d and nextup(d) by comparing the exact distances. */
    split_double(d, &fd, &ed);
    bn_set(&A, m);
    bn_set(&B, fd);
    bn_set(&U, 1);
    if (de > 0) {
        bn_mul_pow10(&A, de);
    } else if (de < 0) {
        bn_mul_pow10(&B, -de);
        bn_mul_pow10(&U, -de);
    }
    if (ed > 0) {
        bn_shl(&B, ed);
        bn_shl(&U, ed);
    } else if (ed < 0) {
        bn_shl(&A, -ed);
    }
    {
        bn lo = A, hi;
        bn_sub(&lo, &B);                 /* N - d          */
        bn_add(&T, &B, &U);              /* the next double */
        hi = T;
        bn_sub(&hi, &A);                 /* nextup(d) - N   */
        c = bn_cmp(&lo, &hi);
        if (c > 0)
            return next_double(d, 1);
        if (c == 0 && (fd & 1))
            return next_double(d, 1);    /* ties go to even */
        return d;
    }
}

static double make_double(int neg, uint64_t mant, int ndig, int dexp, int over)
{
    double r;

    if (mant == 0)
        return neg ? -0.0 : 0.0;
    if (ndig <= 15 && dexp >= -22 && dexp <= 22 && !over) {
        /* Both operands are exact here, so one IEEE operation is already
         * correctly rounded. */
        double m = (double)mant;
        r = dexp >= 0 ? m * (double)pow10l(dexp) : m / (double)pow10l(-dexp);
        return neg ? -r : r;
    }
    if (dexp > 400) return neg ? js_inf(1) : js_inf(0);
    if (dexp < -400) return neg ? -0.0 : 0.0;
    r = (double)((long double)mant * pow10l(dexp));
    if (!js_isinf(r))
        r = refine_double(mant, dexp, r);
    return neg ? -r : r;
}

double js_strtod(const char *s, unsigned long n, unsigned long *end)
{
    unsigned long i = 0;
    int neg = 0, ndig = 0, seen = 0, over = 0;
    int dexp = 0;
    uint64_t mant = 0;

    if (i < n && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
    if (i + 8 <= n && memcmp(s + i, "Infinity", 8) == 0) {
        if (end) *end = i + 8;
        return neg ? js_inf(1) : js_inf(0);
    }
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        seen = 1;
        if (mant < 1000000000000000000ULL) {
            mant = mant * 10 + (uint64_t)(s[i] - '0');
            if (mant) ndig++;
        } else {
            dexp++;
            over = 1;
        }
        i++;
    }
    if (i < n && s[i] == '.') {
        unsigned long j = i + 1;
        while (j < n && s[j] >= '0' && s[j] <= '9') {
            seen = 1;
            if (mant < 1000000000000000000ULL) {
                mant = mant * 10 + (uint64_t)(s[j] - '0');
                if (mant) ndig++;
                dexp--;
            } else {
                over = 1;
            }
            j++;
        }
        i = j;
    }
    if (!seen) { if (end) *end = 0; return js_nan(); }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        unsigned long j = i + 1;
        int eneg = 0, ev = 0, any = 0;
        if (j < n && (s[j] == '+' || s[j] == '-')) { eneg = s[j] == '-'; j++; }
        while (j < n && s[j] >= '0' && s[j] <= '9') {
            if (ev < 100000) ev = ev * 10 + (s[j] - '0');
            any = 1;
            j++;
        }
        if (any) { dexp += eneg ? -ev : ev; i = j; }
    }
    if (end) *end = i;
    return make_double(neg, mant, ndig, dexp, over);
}

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f';
}

double js_strtonum(const char *s, unsigned long n)
{
    unsigned long a = 0, b = n, used = 0;
    int neg = 0;

    while (a < b && (is_ws(s[a]) || (unsigned char)s[a] == 0xA0)) a++;
    while (b > a && (is_ws(s[b - 1]) || (unsigned char)s[b - 1] == 0xA0)) b--;
    if (a == b)
        return 0.0;
    if (s[a] == '+' || s[a] == '-') { neg = s[a] == '-'; a++; }
    if (b - a > 2 && s[a] == '0' && (s[a + 1] == 'x' || s[a + 1] == 'X')) {
        double v = 0;
        unsigned long i;
        if (neg) return js_nan();          /* ES5: no sign on HexIntegerLiteral */
        for (i = a + 2; i < b; i++) {
            int d;
            char c = s[i];
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return js_nan();
            v = v * 16 + d;
        }
        return v;
    }
    if (neg) a--;
    {
        double v = js_strtod(s + a, b - a, &used);
        if (used != b - a)
            return js_nan();
        return v;
    }
}

/* ================================================================== */
/* The ES5 abstract operations                                         */
/* ================================================================== */

const char *js_typeof(js_value v)
{
    switch (v.type) {
    case JS_UNDEFINED: return "undefined";
    case JS_NULL:      return "object";
    case JS_BOOL:      return "boolean";
    case JS_NUMBER:    return "number";
    case JS_STRING:    return "string";
    default:
        return v.u.obj->cls == JS_CLASS_FUNCTION ? "function" : "object";
    }
}

int js_to_boolean(js_value v)
{
    switch (v.type) {
    case JS_UNDEFINED:
    case JS_NULL:   return 0;
    case JS_BOOL:   return v.u.b;
    case JS_NUMBER: return !(v.u.num == 0 || js_isnan(v.u.num));
    case JS_STRING: return v.u.str->len != 0;
    default:        return 1;
    }
}

int js_to_primitive(js_ctx *ctx, js_value v, int hint, js_value *out)
{
    js_string *order[2];
    int i;

    if (v.type != JS_OBJECT) { *out = v; return JS_OK; }
    if (hint == JS_HINT_NONE)
        hint = (v.u.obj->cls == JS_CLASS_DATE) ? JS_HINT_STRING : JS_HINT_NUMBER;
    if (hint == JS_HINT_STRING) { order[0] = ctx->s_toString; order[1] = ctx->s_valueOf; }
    else { order[0] = ctx->s_valueOf; order[1] = ctx->s_toString; }

    for (i = 0; i < 2; i++) {
        js_value fn, r;
        if (js_obj_get(ctx, v.u.obj, order[i], v, &fn) != JS_OK)
            return JS_THROW;
        if (!js_is_function(fn))
            continue;
        if (js_call(ctx, fn, v, 0, 0, &r) != JS_OK)
            return JS_THROW;
        if (r.type != JS_OBJECT) { *out = r; return JS_OK; }
    }
    *out = js_undefined();
    return js_throw_error(ctx, JS_ERR_TYPE, "cannot convert object to primitive");
}

int js_to_number(js_ctx *ctx, js_value v, double *out)
{
    switch (v.type) {
    case JS_UNDEFINED: *out = js_nan(); return JS_OK;
    case JS_NULL:      *out = 0; return JS_OK;
    case JS_BOOL:      *out = v.u.b ? 1 : 0; return JS_OK;
    case JS_NUMBER:    *out = v.u.num; return JS_OK;
    case JS_STRING:    *out = js_strtonum(v.u.str->data, v.u.str->len); return JS_OK;
    default: {
        js_value p;
        if (js_to_primitive(ctx, v, JS_HINT_NUMBER, &p) != JS_OK) { *out = js_nan(); return JS_THROW; }
        return js_to_number(ctx, p, out);
    }
    }
}

int js_to_string(js_ctx *ctx, js_value v, js_value *out)
{
    char buf[48];

    switch (v.type) {
    case JS_UNDEFINED: *out = js_string_value(ctx->s_undefined); return JS_OK;
    case JS_NULL:      *out = js_string_value(ctx->s_null); return JS_OK;
    case JS_BOOL:      *out = js_string_value(v.u.b ? ctx->s_true : ctx->s_false); return JS_OK;
    case JS_STRING:    *out = v; return JS_OK;
    case JS_NUMBER:
        js_dtoa(v.u.num, buf);
        *out = js_mkcstring(ctx, buf);
        return ctx->fatal ? JS_THROW : JS_OK;
    default: {
        js_value p;
        if (js_to_primitive(ctx, v, JS_HINT_STRING, &p) != JS_OK) {
            *out = js_string_value(ctx->s_empty);
            return JS_THROW;
        }
        return js_to_string(ctx, p, out);
    }
    }
}

const char *js_to_cstring(js_ctx *ctx, js_value v)
{
    js_value s;
    if (js_to_string(ctx, v, &s) != JS_OK || s.type != JS_STRING)
        return 0;
    return s.u.str->data;
}

int js_to_integer(js_ctx *ctx, js_value v, double *out)
{
    double d;
    if (js_to_number(ctx, v, &d) != JS_OK) { *out = 0; return JS_THROW; }
    if (js_isnan(d)) { *out = 0; return JS_OK; }
    if (d == 0 || js_isinf(d)) { *out = d; return JS_OK; }
    *out = js_trunc(d);
    return JS_OK;
}

static uint32_t d_to_u32(double d)
{
    uint64_t b, m;
    int e;

    if (js_isnan(d) || js_isinf(d) || d == 0)
        return 0;
    b = dbits(d);
    e = (int)((b >> 52) & 0x7FF);
    if (e < 1023)
        return 0;                       /* |d| < 1 */
    m = (b & 0xFFFFFFFFFFFFFULL) | (1ULL << 52);
    e -= 1075;                          /* d = +-m * 2^e */
    if (e >= 32)
        return 0;                       /* m * 2^e is a multiple of 2^32 */
    if (e >= 0)
        m <<= e;
    else if (-e >= 64)
        m = 0;
    else
        m >>= -e;
    {
        uint32_t r = (uint32_t)m;
        return (b >> 63) ? (uint32_t)(0u - r) : r;
    }
}

int js_to_int32(js_ctx *ctx, js_value v, int32_t *out)
{
    double d;
    if (js_to_number(ctx, v, &d) != JS_OK) { *out = 0; return JS_THROW; }
    *out = (int32_t)d_to_u32(d);
    return JS_OK;
}

int js_to_uint32(js_ctx *ctx, js_value v, uint32_t *out)
{
    double d;
    if (js_to_number(ctx, v, &d) != JS_OK) { *out = 0; return JS_THROW; }
    *out = d_to_u32(d);
    return JS_OK;
}

int js_to_object(js_ctx *ctx, js_value v, js_value *out)
{
    if (v.type == JS_OBJECT) { *out = v; return JS_OK; }
    if (v.type == JS_UNDEFINED || v.type == JS_NULL) {
        *out = js_undefined();
        return js_throw_error(ctx, JS_ERR_TYPE, "cannot convert %s to object",
                              v.type == JS_NULL ? "null" : "undefined");
    }
    return js_wrap_primitive(ctx, v, out);
}

int js_strict_equals(js_value a, js_value b)
{
    if (a.type != b.type)
        return 0;
    switch (a.type) {
    case JS_UNDEFINED:
    case JS_NULL:   return 1;
    case JS_BOOL:   return a.u.b == b.u.b;
    case JS_NUMBER:
        if (js_isnan(a.u.num) || js_isnan(b.u.num))
            return 0;
        return a.u.num == b.u.num;
    case JS_STRING: return js_str_eq(a.u.str, b.u.str);
    default:        return a.u.obj == b.u.obj;
    }
}

int js_loose_equals(js_ctx *ctx, js_value a, js_value b, int *out)
{
    int i;

    *out = 0;
    for (i = 0; i < 8; i++) {
        if (a.type == b.type) { *out = js_strict_equals(a, b); return JS_OK; }
        if ((a.type == JS_NULL && b.type == JS_UNDEFINED) ||
            (a.type == JS_UNDEFINED && b.type == JS_NULL)) { *out = 1; return JS_OK; }
        if (a.type == JS_NULL || a.type == JS_UNDEFINED ||
            b.type == JS_NULL || b.type == JS_UNDEFINED) { *out = 0; return JS_OK; }
        if (a.type == JS_NUMBER && b.type == JS_STRING) {
            double d;
            if (js_to_number(ctx, b, &d) != JS_OK) return JS_THROW;
            b = js_number(d);
            continue;
        }
        if (a.type == JS_STRING && b.type == JS_NUMBER) {
            double d;
            if (js_to_number(ctx, a, &d) != JS_OK) return JS_THROW;
            a = js_number(d);
            continue;
        }
        if (a.type == JS_BOOL) {
            double d;
            if (js_to_number(ctx, a, &d) != JS_OK) return JS_THROW;
            a = js_number(d);
            continue;
        }
        if (b.type == JS_BOOL) {
            double d;
            if (js_to_number(ctx, b, &d) != JS_OK) return JS_THROW;
            b = js_number(d);
            continue;
        }
        if (b.type == JS_OBJECT) {
            js_value p;
            if (js_to_primitive(ctx, b, JS_HINT_NONE, &p) != JS_OK) return JS_THROW;
            b = p;
            continue;
        }
        if (a.type == JS_OBJECT) {
            js_value p;
            if (js_to_primitive(ctx, a, JS_HINT_NONE, &p) != JS_OK) return JS_THROW;
            a = p;
            continue;
        }
        return JS_OK;
    }
    return JS_OK;
}

/* ================================================================== */
/* Context lifecycle                                                   */
/* ================================================================== */

void js_config_default(js_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->max_heap = 16UL * 1024 * 1024;
    cfg->max_steps = 20000000UL;
    cfg->max_call_depth = 32;
    cfg->max_depth = 160;
    cfg->max_parse_depth = 64;
    cfg->max_string = 4UL * 1024 * 1024;
}

js_ctx *js_new(const js_config *cfg)
{
    js_ctx *ctx = (js_ctx *)malloc(sizeof(js_ctx));

    if (!ctx)
        return 0;
    memset(ctx, 0, sizeof(*ctx));
    if (cfg)
        ctx->cfg = *cfg;
    else
        js_config_default(&ctx->cfg);
    if (ctx->cfg.max_heap < 262144UL)
        ctx->cfg.max_heap = 262144UL;
    if (ctx->cfg.max_string == 0 || ctx->cfg.max_string > ctx->cfg.max_heap)
        ctx->cfg.max_string = ctx->cfg.max_heap / 2;
    if (ctx->cfg.max_call_depth <= 0) ctx->cfg.max_call_depth = 32;
    if (ctx->cfg.max_depth <= 0) ctx->cfg.max_depth = 160;
    if (ctx->cfg.max_parse_depth <= 0) ctx->cfg.max_parse_depth = 64;
    ctx->exception = js_undefined();

    if (!js_init_builtins(ctx)) {
        js_free(ctx);
        return 0;
    }
    return ctx;
}

void js_free(js_ctx *ctx)
{
    struct js_chunk *c;

    if (!ctx)
        return;
    js_run_host_finalizers(ctx);
    c = ctx->chunks;
    while (c) {
        struct js_chunk *n = c->next;
        free(c);
        c = n;
    }
    free(ctx);
}
