#pragma once

/* libjs -- a tree-walking interpreter for a subset of ECMAScript 5.
 *
 * SCOPE. This is not a production JavaScript engine and does not pretend to
 * be one. There is no JIT, no garbage collector, no event loop, no promises,
 * no generators, no `let`/`const`, no modules, and no strict mode. What it
 * does implement is the ES5 core language -- the full expression grammar,
 * the full statement set, closures, prototypes, exceptions, automatic
 * semicolon insertion, the ES5 abstract coercion operations, and the common
 * builtins -- to the level needed to run the small scripts that ordinary
 * pages use to toggle a menu, validate a form or rewrite some text.
 *
 * The exact set of deviations is listed at the bottom of this header. Read
 * it before assuming something works.
 *
 * MEMORY MODEL. Allocation is region-based: every string, object, property
 * table and AST node lives in the context and is released in one shot by
 * js_free(). Nothing is collected while a script runs. A configurable heap
 * cap bounds the damage a hostile script can do; exceeding it aborts the
 * script with a fatal (uncatchable) error. This is a deliberate trade of
 * long-run efficiency for the absence of use-after-free and cycle bugs. The
 * intended use is one context per document, or one per script evaluation.
 *
 * SAFETY. Five caller-configurable limits bound a hostile page:
 *   cfg.max_heap        total bytes the script may allocate
 *   cfg.max_steps       interpreter steps before the script is killed
 *   cfg.max_call_depth  nested JS function calls
 *   cfg.max_depth       total interpreter recursion depth (C stack guard)
 *   cfg.max_parse_depth parser recursion depth
 * Heap and step exhaustion are FATAL: they cannot be caught by try/catch,
 * so `try { while (1) {} } catch (e) {}` still terminates. Call-depth,
 * interpreter-depth and parse-depth overflow raise ordinary catchable
 * errors. Regular expression matching has its own step budget and depth
 * cap, so catastrophic backtracking throws instead of hanging.
 *
 * STACK. This interpreter uses the C stack for the JS stack, and the
 * KestrelOS user stack is 16 pages (64 KiB, USER_STACK_PAGES in
 * abi/kestrel_abi.h) with unmapped pages below it. Measured cost is about
 * 1.3 KiB per nested JS call and 430 bytes per level of expression
 * nesting; the shipped defaults hold the worst adversarial case measured
 * to 32 KiB. If the embedder gives libjs a larger stack, raise
 * max_call_depth, max_depth and max_parse_depth together.
 */

#include <stdint.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Values                                                              */
/* ------------------------------------------------------------------ */

enum js_type {
    JS_UNDEFINED = 0,
    JS_NULL      = 1,
    JS_BOOL      = 2,
    JS_NUMBER    = 3,
    JS_STRING    = 4,
    JS_OBJECT    = 5
};

typedef struct js_ctx    js_ctx;
typedef struct js_string js_string;
typedef struct js_object js_object;
typedef struct js_env    js_env;
typedef struct js_node   js_node;
typedef struct js_func   js_func;
typedef struct js_regexp js_regexp;

typedef struct js_value {
    uint32_t type;              /* enum js_type */
    uint32_t pad;
    union {
        int        b;
        double     num;
        js_string *str;
        js_object *obj;
    } u;
} js_value;

/* Return codes from anything that can throw. */
#define JS_OK    0
#define JS_THROW 1

/* ------------------------------------------------------------------ */
/* Embedding interface                                                 */
/* ------------------------------------------------------------------ */

/* A native function. `ret` must be filled on JS_OK. On JS_THROW the
 * exception has already been set with js_throw()/js_throw_error(). */
typedef int (*js_native)(js_ctx *ctx, js_value this_val,
                         int argc, js_value *argv, js_value *ret);

/* Called once per host object at js_free() time, so the embedder can drop
 * whatever `host` pointed at. Registered per host tag. */
typedef void (*js_host_free)(void *user, void *host);

typedef struct js_config {
    unsigned long max_heap;       /* bytes; default 16 MiB */
    unsigned long max_steps;      /* default 20,000,000; 0 = unlimited */
    int  max_call_depth;          /* default 24  */
    int  max_depth;               /* default 140 */
    int  max_parse_depth;         /* default 40  */
    unsigned long max_string;     /* longest single string; default 4 MiB */
    void (*print)(void *user, const char *text);   /* console.log sink */
    double (*now_ms)(void *user);                  /* ms since Unix epoch */
    void *user;
} js_config;

void    js_config_default(js_config *cfg);

js_ctx *js_new(const js_config *cfg);   /* NULL if the initial heap fails */
void    js_free(js_ctx *ctx);

/* Compile and run `src` (NUL-terminated, `name` used in error messages).
 * Returns JS_OK with the completion value in *result, or JS_THROW with the
 * thrown value in *result. */
int js_eval(js_ctx *ctx, const char *src, const char *name, js_value *result);

/* True once a fatal limit (heap/steps) has fired. The context can still be
 * inspected but must not be used to run more script. */
int js_fatal(js_ctx *ctx);

/* Reset the step counter, e.g. between two independent event handlers. */
void js_reset_steps(js_ctx *ctx);

/* ---- constructing values ---- */
js_value js_undefined(void);
js_value js_null(void);
js_value js_bool(int b);
js_value js_number(double d);
js_value js_object_value(js_object *o);
js_value js_string_value(js_string *s);
/* Copies `n` bytes. Returns undefined (and raises fatal) if the heap is out. */
js_value js_mkstring(js_ctx *ctx, const char *s, unsigned long n);
js_value js_mkcstring(js_ctx *ctx, const char *s);

/* ---- inspecting values ---- */
static inline int js_is_undefined(js_value v) { return v.type == JS_UNDEFINED; }
static inline int js_is_null(js_value v)      { return v.type == JS_NULL; }
static inline int js_is_bool(js_value v)      { return v.type == JS_BOOL; }
static inline int js_is_number(js_value v)    { return v.type == JS_NUMBER; }
static inline int js_is_string(js_value v)    { return v.type == JS_STRING; }
static inline int js_is_object(js_value v)    { return v.type == JS_OBJECT; }
int  js_is_function(js_value v);
int  js_is_array(js_value v);

/* Bytes of a string value; `len` may be NULL. NULL for non-strings. */
const char *js_string_bytes(js_value v, unsigned long *len);

/* ---- coercions (the ES5 abstract operations) ---- */
int    js_to_boolean(js_value v);                                   /* ToBoolean  */
int    js_to_number(js_ctx *ctx, js_value v, double *out);           /* ToNumber   */
int    js_to_string(js_ctx *ctx, js_value v, js_value *out);         /* ToString   */
int    js_to_object(js_ctx *ctx, js_value v, js_value *out);         /* ToObject   */
int    js_to_int32(js_ctx *ctx, js_value v, int32_t *out);           /* ToInt32    */
int    js_to_uint32(js_ctx *ctx, js_value v, uint32_t *out);         /* ToUint32   */
int    js_to_integer(js_ctx *ctx, js_value v, double *out);          /* ToInteger  */
/* Convenience: ToString then hand back the NUL-terminated bytes. Returns
 * NULL and leaves an exception pending on failure. */
const char *js_to_cstring(js_ctx *ctx, js_value v);

/* ---- objects ---- */
js_object *js_new_object(js_ctx *ctx);              /* proto = Object.prototype */
js_object *js_new_array(js_ctx *ctx);
js_object *js_new_object_proto(js_ctx *ctx, js_object *proto);

int js_get(js_ctx *ctx, js_value obj, const char *name, js_value *out);
int js_get_value(js_ctx *ctx, js_value obj, js_value key, js_value *out);
int js_set(js_ctx *ctx, js_value obj, const char *name, js_value v);
int js_set_value(js_ctx *ctx, js_value obj, js_value key, js_value v);
int js_has(js_ctx *ctx, js_value obj, const char *name);
int js_delete(js_ctx *ctx, js_value obj, const char *name);

/* Non-enumerable, writable, configurable -- what builtins want. */
int js_define(js_ctx *ctx, js_object *o, const char *name, js_value v);
/* Enumerable data property -- what plain data wants. */
int js_define_enum(js_ctx *ctx, js_object *o, const char *name, js_value v);

/* Array helpers. */
unsigned long js_array_length(js_object *a);
int           js_array_push(js_ctx *ctx, js_object *a, js_value v);
int           js_array_get(js_ctx *ctx, js_object *a, unsigned long i, js_value *out);

/* The global object, as a value. */
js_value js_global(js_ctx *ctx);

/* ---- native functions and accessors ---- */
/* Create a callable object. `name`/`nargs` populate .name and .length. */
js_object *js_new_native(js_ctx *ctx, js_native fn, const char *name, int nargs);
/* Shorthand: attach a native method to `obj` (non-enumerable, as ES5 does). */
int js_define_native(js_ctx *ctx, js_object *obj, const char *name,
                     js_native fn, int nargs);
/* Define an accessor property backed by native code. Either may be NULL. */
int js_define_accessor(js_ctx *ctx, js_object *obj, const char *name,
                       js_native getter, js_native setter, int enumerable);

/* ---- opaque host pointers ---- */
/* An object carrying an embedder pointer. `tag` lets the embedder check the
 * kind before casting. `proto` may be NULL for Object.prototype. */
js_object *js_new_host(js_ctx *ctx, void *host, uint32_t tag, js_object *proto);
/* Returns the host pointer if `v` is a host object with this tag, else NULL. */
void      *js_host_ptr(js_value v, uint32_t tag);
/* Register a destructor run over every live host object of `tag` in js_free(). */
int        js_host_finalizer(js_ctx *ctx, uint32_t tag, js_host_free fn);

/* ---- calling ---- */
int js_call(js_ctx *ctx, js_value fn, js_value this_val,
            int argc, js_value *argv, js_value *ret);
int js_construct(js_ctx *ctx, js_value fn, int argc, js_value *argv,
                 js_value *ret);

/* ---- throwing ---- */
enum js_errkind {
    JS_ERR_ERROR = 0, JS_ERR_TYPE, JS_ERR_RANGE, JS_ERR_SYNTAX,
    JS_ERR_REFERENCE, JS_ERR_EVAL, JS_ERR_URI, JS_ERR__COUNT
};
int js_throw(js_ctx *ctx, js_value v);
int js_throw_error(js_ctx *ctx, int kind, const char *fmt, ...);
/* The pending exception (undefined if none). */
js_value js_exception(js_ctx *ctx);

/* Render any value the way an uncaught-error report would: Error objects
 * become "TypeError: message", everything else goes through ToString. The
 * result points into the context and stays valid until js_free(). */
const char *js_error_text(js_ctx *ctx, js_value v);

/* ------------------------------------------------------------------ */
/* Known deviations from ES5                                           */
/* ------------------------------------------------------------------ */
/*
 * 1.  Strings are byte strings, not UTF-16. `.length`, charAt/charCodeAt
 *     and indices count BYTES. Source text and \uXXXX escapes are stored
 *     as UTF-8, so a non-ASCII character has length 2 or 3. fromCharCode
 *     emits one byte below 256 and UTF-8 above. ASCII behaves exactly as
 *     ES5 requires; anything else does not.
 * 2.  No garbage collection (see MEMORY MODEL above).
 * 3.  No strict mode; `"use strict"` parses and is ignored.
 * 4.  No `let`, `const`, arrow functions, classes, template literals,
 *     destructuring, spread, generators, promises, Symbol, Proxy, or
 *     typed arrays. Those are ES6+ and out of scope.
 * 5.  Property attributes exist (enumerable/writable/configurable) and are
 *     honoured, but Object.defineProperty/getOwnPropertyDescriptor are the
 *     only ES5 meta-API implemented; seal/freeze/preventExtensions are
 *     present, getOwnPropertyNames is present, the rest are not.
 * 6.  Array holes are not distinguished from `undefined` elements.
 * 7.  The `arguments` object is a snapshot: it is array-like and has
 *     .length and .callee, but it is NOT aliased to the named parameters
 *     the way non-strict ES5 requires.
 * 8.  eval() is not implemented (it throws). Function(...) as a
 *     constructor-from-source is not implemented either.
 * 9.  Regular expressions are a backtracking matcher over a common subset:
 *     literals, ., character classes with ranges/negation/escapes, the
 *     anchors ^ $ \b \B, groups (capturing and (?:...)), alternation, the
 *     quantifiers * + ? {n} {n,} {n,m} in greedy and lazy form, and
 *     backreferences. Lookahead ((?=) (?!)) is supported; lookbehind,
 *     named groups, sticky/unicode flags and \p classes are not. Every
 *     match runs under a step budget and throws if it is exhausted, so
 *     catastrophic backtracking cannot hang the caller.
 * 10. Date implements construction from now/ms/parts/ISO strings and the
 *     getters, getTime, setTime and toISOString; local time IS UTC and
 *     getTimezoneOffset() is always 0, because the kernel has no timezone
 *     database. Date parsing accepts ISO 8601 and little else. There are
 *     no setFullYear/setMonth/... mutators beyond setTime.
 * 11. The lexer decides `/` is a regex or a division from the previous
 *     token. This is the standard heuristic and it is wrong for a handful
 *     of pathological inputs (an object literal immediately divided, for
 *     instance).
 * 12. `__proto__` is not implemented; use Object.getPrototypeOf and
 *     Object.create. `arguments` is not aliased to the parameters.
 * 13. Verified against Node on 514 programs covering the whole subset:
 *     510 produce byte-identical output. The four that differ are the
 *     scope cuts above -- the ES2016 `**` operator, the ES6 method
 *     String.prototype.repeat, `__proto__`, and local-time-is-UTC.
 */

/* ================================================================== */
/* Internals. Shared between the libjs translation units only; an      */
/* embedder should not define JS_INTERNAL.                             */
/* ================================================================== */
#ifdef JS_INTERNAL

/* ---- tokens ---- */
enum js_token {
    TK_EOF = 0, TK_IDENT, TK_NUM, TK_STR, TK_REGEX,
    TK_LBRACE, TK_RBRACE, TK_LPAREN, TK_RPAREN, TK_LBRACKET, TK_RBRACKET,
    TK_DOT, TK_SEMI, TK_COMMA,
    TK_LT, TK_GT, TK_LE, TK_GE, TK_EQ, TK_NE, TK_SEQ, TK_SNE,
    TK_ADD, TK_SUB, TK_MUL, TK_DIV, TK_MOD,
    TK_INC, TK_DEC, TK_SHL, TK_SHR, TK_USHR,
    TK_BAND, TK_BOR, TK_BXOR, TK_NOT, TK_BNOT, TK_ANDAND, TK_OROR,
    TK_QUESTION, TK_COLON, TK_ASSIGN,
    TK_ADD_A, TK_SUB_A, TK_MUL_A, TK_DIV_A, TK_MOD_A,
    TK_SHL_A, TK_SHR_A, TK_USHR_A, TK_BAND_A, TK_BOR_A, TK_BXOR_A,
    /* keywords -- must stay contiguous and match kw_table[] in lex.c */
    TK_BREAK, TK_CASE, TK_CATCH, TK_CONTINUE, TK_DEBUGGER, TK_DEFAULT,
    TK_DELETE, TK_DO, TK_ELSE, TK_FALSE, TK_FINALLY, TK_FOR, TK_FUNCTION,
    TK_IF, TK_IN, TK_INSTANCEOF, TK_NEW, TK_NULL_KW, TK_RETURN, TK_SWITCH,
    TK_THIS, TK_THROW, TK_TRUE, TK_TRY, TK_TYPEOF, TK_VAR, TK_VOID,
    TK_WHILE, TK_WITH,
    TK__COUNT
};

typedef struct js_lexer {
    js_ctx      *ctx;
    const char  *src;
    unsigned long len;
    unsigned long pos;          /* offset of the next byte to read       */
    int          line;

    int          tok;           /* current token                         */
    int          nl_before;     /* a line terminator preceded it (ASI)   */
    int          tok_line;
    js_string   *text;          /* ident name / string value / regex src */
    js_string   *flags;         /* regex flags                           */
    double       num;

    int          prev_tok;      /* for the regex-vs-division heuristic   */
    int          paren_kind[64];/* 0 = grouping, 1 = if/while/for header */
    int          paren_sp;
    int          last_paren;    /* kind of the most recently closed paren  */
    int          error;         /* set once a lex error has been thrown  */
} js_lexer;

void js_lex_init(js_lexer *lx, js_ctx *ctx, const char *src);
int  js_lex_next(js_lexer *lx);          /* JS_OK / JS_THROW */
const char *js_token_name(int tok);

/* ---- AST ---- */
enum js_nodetype {
    N_NUM, N_STR, N_REGEX, N_IDENT, N_THIS, N_NULL, N_BOOL,
    N_ARRAY, N_OBJECT, N_PROP, N_FUNC,
    N_CALL, N_NEW, N_MEMBER, N_INDEX,
    N_PREFIX, N_POSTFIX, N_UNARY, N_BINARY, N_LOGICAL, N_ASSIGN,
    N_COND, N_SEQ,
    N_PROGRAM, N_BLOCK, N_VAR, N_VARDECL, N_EMPTY, N_EXPRSTMT,
    N_IF, N_DO, N_WHILE, N_FOR, N_FORIN,
    N_CONTINUE, N_BREAK, N_RETURN, N_WITH, N_SWITCH, N_CASE, N_LABEL,
    N_THROW, N_TRY, N_FUNCDECL, N_DEBUGGER
};

struct js_node {
    uint8_t   type;
    uint8_t   op;          /* token for binary/unary/assign; kind for N_PROP */
    uint16_t  flags;
    uint32_t  line;
    js_string *str;        /* identifier, string literal, label, prop name  */
    js_string *str2;       /* regex flags                                   */
    double     num;
    js_node   *a, *b, *c, *d;
    js_node   *next;       /* sibling within a list                         */
};

/* Compiled function body, shared by every closure over it. */
struct js_func {
    js_string *name;
    js_node   *params;      /* N_IDENT list  */
    js_node   *body;        /* statement list */
    js_node   *vars;        /* hoisted N_IDENT list (var + params)         */
    js_node   *funcs;       /* hoisted N_FUNCDECL list                     */
    int        nparams;
    js_env    *closure;
    js_native  native;
    int        ctor_kind;   /* JS_CTOR_* -- how `new f()` behaves          */

    /* Function.prototype.bind state; NULL target means "not bound". */
    js_object *bound_target;
    js_value   bound_this;
    js_value  *bound_args;
    int        nbound;
};

#define JS_CTOR_NORMAL 0    /* ordinary function: allocate, call, use this  */
#define JS_CTOR_NATIVE 1    /* native decides; `this` arrives as undefined  */
#define JS_CTOR_NONE   2    /* not a constructor: `new` throws TypeError    */

js_node *js_parse(js_ctx *ctx, const char *src, const char *name);

/* ---- strings ---- */
struct js_string {
    uint32_t len;
    uint32_t hash;
    js_string *inext;       /* intern chain */
    char      data[1];      /* NUL terminated */
};

js_string *js_str_new(js_ctx *ctx, const char *s, unsigned long n);
js_string *js_str_newz(js_ctx *ctx, const char *s);
js_string *js_str_cat(js_ctx *ctx, js_string *a, js_string *b);
js_string *js_str_intern(js_ctx *ctx, const char *s, unsigned long n);
int        js_str_eq(js_string *a, js_string *b);

/* ---- properties and objects ---- */
#define JS_P_ENUM     0x01
#define JS_P_WRITE    0x02
#define JS_P_CONFIG   0x04
#define JS_P_ACCESSOR 0x08
#define JS_P_DEAD     0x10   /* deleted; slot kept so ordering is stable */
#define JS_P_DEFAULT  (JS_P_ENUM | JS_P_WRITE | JS_P_CONFIG)
#define JS_P_HIDDEN   (JS_P_WRITE | JS_P_CONFIG)

typedef struct js_prop {
    js_string *key;
    js_value   value;      /* data value, or the getter when ACCESSOR */
    js_value   setter;
    uint32_t   flags;
    uint32_t   hnext;      /* hash chain, index+1, 0 = end */
} js_prop;

enum js_class {
    JS_CLASS_OBJECT = 0, JS_CLASS_ARRAY, JS_CLASS_FUNCTION, JS_CLASS_ERROR,
    JS_CLASS_DATE, JS_CLASS_REGEXP, JS_CLASS_STRING, JS_CLASS_NUMBER,
    JS_CLASS_BOOLEAN, JS_CLASS_ARGUMENTS, JS_CLASS_MATH, JS_CLASS_JSON,
    JS_CLASS_HOST
};

struct js_object {
    uint8_t    cls;
    uint8_t    extensible;
    uint8_t    sparse;      /* array with indices outside the dense run */
    js_object *proto;

    js_prop   *props;
    uint32_t   nprops, cprops;
    uint32_t  *buckets;
    uint32_t   nbuckets;

    js_value  *elems;       /* dense array storage */
    uint32_t   elen, ecap;

    js_func   *fn;          /* JS_CLASS_FUNCTION */
    js_value   prim;        /* wrapper primitive / Date time value */
    js_regexp *re;          /* JS_CLASS_REGEXP */
    void      *host;        /* embedder pointer */
    uint32_t   host_tag;

    js_object *next;        /* every object, for js_free() */
};

js_object *js_obj_alloc(js_ctx *ctx, int cls, js_object *proto);
js_prop   *js_own_prop(js_object *o, js_string *key);
js_prop   *js_add_prop(js_ctx *ctx, js_object *o, js_string *key, uint32_t flags);
int        js_obj_get(js_ctx *ctx, js_object *o, js_string *key,
                      js_value this_val, js_value *out);
int        js_obj_put(js_ctx *ctx, js_object *o, js_string *key, js_value v);
int        js_obj_delete(js_ctx *ctx, js_object *o, js_string *key);
int        js_obj_has(js_ctx *ctx, js_object *o, js_string *key);
int        js_obj_has_own(js_ctx *ctx, js_object *o, js_string *key);
/* [[Get]] on any value, including the primitive receivers that make
 * "abc".length and (5).toFixed(2) work without allocating a wrapper. */
int        js_wrap_get(js_ctx *ctx, js_value base, js_string *key, js_value *out);

/* Array index for a key, or -1. Only canonical decimal forms qualify. */
long js_array_index(js_string *key);

/* ---- environments ---- */
struct js_env {
    js_env    *parent;
    js_object *vars;
    js_value   this_val;
    int        is_with;
    js_env    *next;        /* every env, for js_free() */
};

js_env *js_env_new(js_ctx *ctx, js_env *parent, js_object *vars);

/* ---- regexp ---- */
struct js_regexp {
    js_string *source;
    js_string *flags;
    int        ignore_case;
    int        global;
    int        multiline;
    int        ngroups;     /* including group 0 */
    void      *prog;        /* compiled node array (builtin.c) */
    uint32_t   nprog;
};

/* Compile; returns NULL with an exception pending. */
js_regexp *js_regexp_compile(js_ctx *ctx, js_string *src, js_string *flags);
/* Match at or after `start`. Returns 1 on match (caps filled with byte
 * offsets, -1 for unmatched groups), 0 on no match, -1 on error/throw. */
int js_regexp_exec_raw(js_ctx *ctx, js_regexp *re, const char *s,
                       long len, long start, long *caps);

/* ---- labels ---- */
/* The label set attached to the statement currently being entered. Pushed
 * on the C stack by N_LABEL, captured and cleared by loops and switches. */
typedef struct js_label {
    js_string       *name;
    struct js_label *next;
} js_label;

/* ---- the context ---- */
#define JS_NPROTO 24
#define JS_ARGSTACK 4096        /* js_value slots shared by all call frames */
#define JS_MAXARGS  255

struct js_ctx {
    js_config cfg;

    /* region allocator */
    struct js_chunk *chunks;
    char         *cur;
    unsigned long cur_left;
    unsigned long heap_used;

    js_string   **intern;
    uint32_t      intern_n, intern_cap;

    js_object    *objects;      /* every object ever allocated */
    js_env       *envs;

    js_object    *global;
    js_env       *genv;

    js_object    *proto[JS_NPROTO];
    js_object    *ctor_object;
    js_object    *ctor_array;
    js_object    *ctor_regexp;

    js_value      exception;
    int           has_exception;
    int           fatal;        /* heap/step exhaustion: uncatchable */
    const char   *fatal_msg;

    unsigned long steps;
    int           depth;
    int           call_depth;

    js_object    *new_target;   /* set while a native runs under `new` */
    js_label     *labels;       /* label set for the statement being entered */
    js_value     *argstack;
    uint32_t      argsp;

    const char   *script_name;

    /* per-tag host finalizers */
    struct { uint32_t tag; js_host_free fn; } hostfin[8];
    int           nhostfin;

    /* scratch used by js_error_text / js_to_cstring */
    char          msgbuf[256];

    /* interned strings used everywhere */
    js_string *s_empty, *s_length, *s_prototype, *s_constructor, *s_proto__;
    js_string *s_name, *s_message, *s_arguments, *s_callee, *s_value,
              *s_writable, *s_enumerable, *s_configurable, *s_get, *s_set,
              *s_index, *s_input, *s_lastIndex, *s_source, *s_toString,
              *s_valueOf, *s_undefined, *s_null, *s_true, *s_false,
              *s_number, *s_string, *s_boolean, *s_object, *s_function;
};

/* proto[] slots */
enum {
    P_OBJECT = 0, P_FUNCTION, P_ARRAY, P_STRING, P_NUMBER, P_BOOLEAN,
    P_ERROR, P_TYPEERROR, P_RANGEERROR, P_SYNTAXERROR, P_REFERENCEERROR,
    P_EVALERROR, P_URIERROR, P_DATE, P_REGEXP
};

/* ---- growable byte buffer (malloc-backed, charged to the heap cap) ---- */
typedef struct js_sbuf {
    js_ctx       *ctx;
    char         *p;
    unsigned long n, cap;
    int           err;
} js_sbuf;

void js_sb_init(js_sbuf *b, js_ctx *ctx);
int  js_sb_put(js_sbuf *b, const char *s, unsigned long n);
int  js_sb_putc(js_sbuf *b, char c);
int  js_sb_puts(js_sbuf *b, const char *s);
void js_sb_free(js_sbuf *b);
/* Turn the buffer into a string value and release it. */
int  js_sb_finish(js_sbuf *b, js_value *out);

/* ---- allocation ---- */
void *js_alloc(js_ctx *ctx, unsigned long n);          /* zeroed, or NULL */
void *js_alloc_raw(js_ctx *ctx, unsigned long n);      /* not zeroed */
int   js_oom(js_ctx *ctx);                             /* raise fatal OOM */
int   js_step(js_ctx *ctx);                            /* charge one step */

/* ---- number <-> string ---- */
/* Shortest round-tripping ES5 ToString(number); buf needs 40 bytes. */
void   js_dtoa(double d, char *buf);
/* `digits` significant digits, ES5 Number.prototype.toFixed/toPrecision. */
void   js_dtoa_fixed(double d, int frac, char *buf, unsigned long bufsz);
void   js_dtoa_precision(double d, int prec, char *buf, unsigned long bufsz);
void   js_dtoa_exponential(double d, int frac, char *buf, unsigned long bufsz);
void   js_dtoa_radix(double d, int radix, char *buf, unsigned long bufsz);
/* ES5 ToNumber(string): full string with surrounding whitespace stripped,
 * NaN if anything is left over. */
double js_strtonum(const char *s, unsigned long n);
/* Prefix parse used by parseFloat; *end receives the stop offset. */
double js_strtod(const char *s, unsigned long n, unsigned long *end);

/* ---- math kernels (no libm here) ---- */
int    js_isnan(double d);
int    js_isinf(double d);
double js_nan(void);
double js_inf(int negative);
double js_floor(double d);
double js_ceil(double d);
double js_trunc(double d);
double js_round(double d);       /* ES5 Math.round: floor(x + 0.5) */
double js_fabs(double d);
double js_sqrt(double d);
double js_fmod(double a, double b);
double js_exp(double d);
double js_log(double d);
double js_pow(double x, double y);
double js_sin(double d);
double js_cos(double d);
double js_tan(double d);
double js_asin(double d);
double js_acos(double d);
double js_atan(double d);
double js_atan2(double y, double x);

/* ---- equality and comparison ---- */
int js_strict_equals(js_value a, js_value b);
int js_loose_equals(js_ctx *ctx, js_value a, js_value b, int *out);
int js_to_primitive(js_ctx *ctx, js_value v, int hint, js_value *out);
#define JS_HINT_NONE   0
#define JS_HINT_NUMBER 1
#define JS_HINT_STRING 2

const char *js_typeof(js_value v);

/* ---- interpreter entry points used by builtin.c ---- */
int js_call_func(js_ctx *ctx, js_object *f, js_value this_val,
                 int argc, js_value *argv, js_value *ret);
js_object *js_make_function(js_ctx *ctx, js_func *fn, js_env *env);

/* ---- builtins ---- */
int  js_init_builtins(js_ctx *ctx);
void js_run_host_finalizers(js_ctx *ctx);
js_object *js_new_error(js_ctx *ctx, int kind, const char *msg);
/* Number/String/Boolean wrapper for a primitive receiver. */
int  js_wrap_primitive(js_ctx *ctx, js_value v, js_value *out);

#endif /* JS_INTERNAL */
