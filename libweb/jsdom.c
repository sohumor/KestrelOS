/* jsdom.c - DOM/window bindings for Kestrel's small JavaScript engine. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsdom.h"
#include "css.h"
#include "url.h"

#define JSDOM_TAG 0x4a444f4dU
#define JSDOM_CLASS_MAX 4096
#define JSDOM_STYLE_MAX 8192
#define JSDOM_FETCH_MAX 32

enum host_kind {
    HOST_NODE = 1,
    HOST_DOCUMENT,
    HOST_LOCATION,
    HOST_CLASSLIST,
    HOST_STYLE,
    HOST_RESPONSE,
    HOST_HEADERS
};

struct response_state {
    int refs;
    int status;
    char *status_text;
    char *url;
    char *content_type;
    char *body;
    unsigned long body_len;
    int body_used;
};

struct jsdom {
    struct dom_document *doc;
    js_ctx *ctx;
    js_object *node_proto;
    js_object *doc_proto;
    js_object *location_proto;
    js_object *class_proto;
    js_object *style_proto;
    js_object *response_proto;
    js_object *headers_proto;
    char *url;
    char *base_url;
    char *navigation;
    int dirty;
    void (*print)(void *user, const char *text);
    const char *(*cookie_get)(void *user);
    int (*cookie_set)(void *user, const char *value);
    int (*fetch)(void *user, const char *url, const char *method,
                 const void *body, unsigned long body_len,
                 struct jsdom_fetch_response *out,
                 char *err, unsigned long errsz);
    void *print_user;
    struct {
        js_value fn;
        int active;
    } timers[64];
    struct {
        js_value promise;
        char *url;
        char *method;
        char *body;
        unsigned long body_len;
        int active;
    } fetches[JSDOM_FETCH_MAX];
};

struct hostref {
    struct jsdom *j;
    struct dom_node *node;
    int kind;
    void *data;
};

static int ci(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int ci_eq_n(const char *a, unsigned long an,
                   const char *b, unsigned long bn)
{
    unsigned long i;

    if (an != bn)
        return 0;
    for (i = 0; i < an; i++)
        if (ci((unsigned char)a[i]) != ci((unsigned char)b[i]))
            return 0;
    return 1;
}

static char *dup_n(const char *s, unsigned long n)
{
    char *p = (char *)malloc(n + 1);

    if (!p)
        return 0;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static char *dup_z(const char *s)
{
    return s ? dup_n(s, strlen(s)) : 0;
}

static struct hostref *host(js_value v, int kind)
{
    struct hostref *h = (struct hostref *)js_host_ptr(v, JSDOM_TAG);

    return h && (!kind || h->kind == kind) ? h : 0;
}

static struct jsdom *binding(js_ctx *c)
{
    js_value v;
    struct hostref *h;

    if (js_get(c, js_global(c), "__kestrelDocument", &v) != JS_OK)
        return 0;
    h = host(v, HOST_DOCUMENT);
    return h ? h->j : 0;
}

static void response_release(struct response_state *s)
{
    if (!s || --s->refs > 0)
        return;
    free(s->status_text);
    free(s->url);
    free(s->content_type);
    free(s->body);
    free(s);
}

static void host_free(void *user, void *ptr)
{
    struct hostref *h = (struct hostref *)ptr;

    (void)user;
    if (h && (h->kind == HOST_RESPONSE || h->kind == HOST_HEADERS))
        response_release((struct response_state *)h->data);
    free(h);
}

static js_object *new_host_data(struct jsdom *j, struct dom_node *n, int kind,
                                void *data, js_object *proto)
{
    struct hostref *h;
    js_object *o;

    h = (struct hostref *)calloc(1, sizeof(*h));
    if (!h)
        return 0;
    h->j = j;
    h->node = n;
    h->kind = kind;
    h->data = data;
    o = js_new_host(j->ctx, h, JSDOM_TAG, proto);
    if (!o)
        free(h);
    return o;
}

static js_object *new_host(struct jsdom *j, struct dom_node *n, int kind,
                           js_object *proto)
{
    return new_host_data(j, n, kind, 0, proto);
}

static js_value wrap_node(struct jsdom *j, struct dom_node *n)
{
    js_object *o;

    if (!n)
        return js_null();
    if (n->user)
        return js_object_value((js_object *)n->user);
    o = new_host(j, n, HOST_NODE, j->node_proto);
    if (!o)
        return js_undefined();
    n->user = o;
    return js_object_value(o);
}

static int string_arg(js_ctx *c, int argc, js_value *argv, int i,
                      const char **out)
{
    if (i >= argc) {
        *out = "undefined";
        return JS_OK;
    }
    *out = js_to_cstring(c, argv[i]);
    return *out ? JS_OK : JS_THROW;
}

static int set_text(struct jsdom *j, struct dom_node *n, const char *s)
{
    struct dom_node *c, *next, *t;

    if (!n || (n->type != DOM_ELEMENT && n->type != DOM_DOCUMENT))
        return 0;
    for (c = n->first_child; c; c = next) {
        next = c->next_sibling;
        dom_remove_child(n, c);
    }
    if (*s) {
        t = dom_create_text(j->doc, s, strlen(s));
        if (!t || !dom_append_child(n, t))
            return 0;
    }
    j->dirty = 1;
    return 1;
}

static struct dom_node *clone_subtree(struct jsdom *j,
                                      const struct dom_node *src, int depth)
{
    struct dom_node *out, *c;
    unsigned int i;

    if (!src || depth > 64)
        return 0;
    if (src->type == DOM_TEXT)
        return dom_create_text(j->doc, src->text, src->text_len);
    if (src->type == DOM_COMMENT)
        return dom_create_comment(j->doc, src->text, src->text_len);
    if (src->type != DOM_ELEMENT)
        return 0;
    out = dom_create_element(j->doc, src->tag, (long)strlen(src->tag));
    if (!out)
        return 0;
    for (i = 0; i < dom_attr_count(src); i++) {
        const struct dom_attr *a = dom_attr_at(src, i);

        if (!a || !dom_set_attr_n(out, a->name, -1, a->value, a->len))
            return 0;
    }
    for (c = src->first_child; c; c = c->next_sibling) {
        struct dom_node *copy = clone_subtree(j, c, depth + 1);

        if (!copy || !dom_append_child(out, copy))
            return 0;
    }
    return out;
}

static int set_inner_html(struct jsdom *j, struct dom_node *target,
                          const char *html)
{
    static const char prefix[] = "<!doctype html><html><body>";
    static const char suffix[] = "</body></html>";
    struct dom_document *tmp;
    struct dom_node *c, *next;
    unsigned long n = strlen(html);
    char *wrapped;

    if (n > DOM_MAX_INPUT - sizeof(prefix) - sizeof(suffix))
        return 0;
    wrapped = (char *)malloc(sizeof(prefix) - 1 + n + sizeof(suffix));
    if (!wrapped)
        return 0;
    memcpy(wrapped, prefix, sizeof(prefix) - 1);
    memcpy(wrapped + sizeof(prefix) - 1, html, n);
    memcpy(wrapped + sizeof(prefix) - 1 + n, suffix, sizeof(suffix));
    tmp = html_parse_document(wrapped,
                              sizeof(prefix) - 1 + n + sizeof(suffix) - 1);
    free(wrapped);
    if (!tmp || tmp->oom) {
        dom_document_free(tmp);
        return 0;
    }
    for (c = target->first_child; c; c = next) {
        next = c->next_sibling;
        dom_remove_child(target, c);
    }
    for (c = tmp->body->first_child; c; c = c->next_sibling) {
        struct dom_node *copy = clone_subtree(j, c, 0);

        if (!copy || !dom_append_child(target, copy)) {
            dom_document_free(tmp);
            return 0;
        }
    }
    dom_document_free(tmp);
    j->dirty = 1;
    return 1;
}

static char *children_html(const struct dom_node *n, unsigned long *out_len)
{
    const struct dom_node *c;
    char *out = 0;
    unsigned long used = 0, cap = 0;

    for (c = n ? n->first_child : 0; c; c = c->next_sibling) {
        unsigned long part_len = 0;
        char *part = dom_serialize(c, &part_len);
        char *grown;

        if (!part || part_len > DOM_MAX_INPUT - used) {
            free(part);
            free(out);
            return 0;
        }
        if (used + part_len + 1 > cap) {
            cap = used + part_len + 1;
            grown = (char *)realloc(out, cap);
            if (!grown) {
                free(part);
                free(out);
                return 0;
            }
            out = grown;
        }
        memcpy(out + used, part, part_len);
        used += part_len;
        free(part);
    }
    if (!out) {
        out = dup_z("");
        cap = 1;
    }
    if (out)
        out[used] = 0;
    if (out_len)
        *out_len = used;
    return out;
}

/* ------------------------------------------------------------------ */
/* Selector subset: tag, #id, .class, [attr], [attr=value], compounds,
 * and descendant chains.  It covers the selectors most small scripts use
 * without pretending to be the CSS selector parser. */

static int name_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':';
}

static int match_token(struct dom_node *n, const char *s, unsigned long len)
{
    unsigned long p = 0, start;

    if (!n || n->type != DOM_ELEMENT || !len)
        return 0;
    if (s[p] == '*')
        p++;
    else if (s[p] != '#' && s[p] != '.' && s[p] != '[') {
        start = p;
        while (p < len && name_char((unsigned char)s[p]))
            p++;
        if (p == start || !n->tag ||
            !ci_eq_n(n->tag, strlen(n->tag), s + start, p - start))
            return 0;
    }
    while (p < len) {
        if (s[p] == '#') {
            const char *id;
            p++;
            start = p;
            while (p < len && name_char((unsigned char)s[p]))
                p++;
            id = dom_get_attr(n, "id");
            if (p == start || !id ||
                strlen(id) != p - start ||
                memcmp(id, s + start, p - start) != 0)
                return 0;
        } else if (s[p] == '.') {
            p++;
            start = p;
            while (p < len && name_char((unsigned char)s[p]))
                p++;
            if (p == start || !dom_has_class_n(n, s + start,
                                                (long)(p - start)))
                return 0;
        } else if (s[p] == '[') {
            const char *v;
            unsigned long ns, ne, vs = 0, ve = 0;
            int have_value = 0;

            p++;
            while (p < len && (s[p] == ' ' || s[p] == '\t')) p++;
            ns = p;
            while (p < len && name_char((unsigned char)s[p])) p++;
            ne = p;
            while (p < len && (s[p] == ' ' || s[p] == '\t')) p++;
            if (p < len && s[p] == '=') {
                int quote = 0;
                have_value = 1;
                p++;
                while (p < len && (s[p] == ' ' || s[p] == '\t')) p++;
                if (p < len && (s[p] == '\'' || s[p] == '"'))
                    quote = s[p++];
                vs = p;
                if (quote) {
                    while (p < len && s[p] != quote) p++;
                    ve = p;
                    if (p < len) p++;
                } else {
                    while (p < len && s[p] != ']' &&
                           s[p] != ' ' && s[p] != '\t') p++;
                    ve = p;
                }
                while (p < len && (s[p] == ' ' || s[p] == '\t')) p++;
            }
            if (p >= len || s[p++] != ']' || ne == ns)
                return 0;
            {
                char name[129];
                unsigned long nn = ne - ns;

                if (nn >= sizeof(name))
                    return 0;
                memcpy(name, s + ns, nn);
                name[nn] = 0;
                v = dom_get_attr(n, name);
            }
            if (!v || (have_value &&
                       (strlen(v) != ve - vs ||
                        memcmp(v, s + vs, ve - vs) != 0)))
                return 0;
        } else {
            return 0;
        }
    }
    return 1;
}

static int selector_match(struct dom_node *n, const char *sel)
{
    const char *end = sel + strlen(sel);
    int first = 1;

    while (end > sel && (end[-1] == ' ' || end[-1] == '\t' ||
                         end[-1] == '\r' || end[-1] == '\n'))
        end--;
    while (end > sel) {
        const char *start = end;

        while (start > sel && start[-1] != ' ' && start[-1] != '\t' &&
               start[-1] != '\r' && start[-1] != '\n')
            start--;
        if (first) {
            if (!match_token(n, start, (unsigned long)(end - start)))
                return 0;
            first = 0;
        } else {
            do {
                n = dom_parent_element(n);
            } while (n && !match_token(n, start,
                                       (unsigned long)(end - start)));
            if (!n)
                return 0;
        }
        while (start > sel && (start[-1] == ' ' || start[-1] == '\t' ||
                               start[-1] == '\r' || start[-1] == '\n'))
            start--;
        end = start;
    }
    return !first;
}

static struct dom_node *query_first(struct dom_node *root, const char *sel)
{
    struct dom_node *n;

    if (!root || !sel || !*sel)
        return 0;
    n = root->first_child;
    while (n) {
        if (n->type == DOM_ELEMENT && selector_match(n, sel))
            return n;
        n = dom_next_within(n, root);
    }
    return 0;
}

static int query_all(js_ctx *c, struct jsdom *j, struct dom_node *root,
                     const char *sel, js_value *ret)
{
    js_object *a = js_new_array(c);
    struct dom_node *n;

    if (!a)
        return JS_THROW;
    for (n = root ? root->first_child : 0; n; n = dom_next_within(n, root)) {
        if (n->type == DOM_ELEMENT && selector_match(n, sel) &&
            js_array_push(c, a, wrap_node(j, n)) != JS_OK)
            return JS_THROW;
    }
    *ret = js_object_value(a);
    return JS_OK;
}

/* ------------------------------------------------------------------ */
/* Element properties and methods. */

static int node_text_get(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    char *s;
    unsigned long n;
    (void)ac; (void)av;

    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal Node receiver");
    if (h->node->type == DOM_TEXT) {
        *r = js_mkstring(c, h->node->text, h->node->text_len);
        return JS_OK;
    }
    s = dom_text_content(h->node, &n);
    if (!s) return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    *r = js_mkstring(c, s, n);
    free(s);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int node_text_set(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const char *s;

    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal Node receiver");
    if (string_arg(c, ac, av, 0, &s) != JS_OK) return JS_THROW;
    if (!set_text(h->j, h->node, s))
        return js_throw_error(c, JS_ERR_ERROR, "cannot set textContent");
    *r = js_undefined();
    return JS_OK;
}

static int node_html_get(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    char *html;
    unsigned long len;
    (void)ac; (void)av;

    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    html = children_html(h->node, &len);
    if (!html)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    *r = js_mkstring(c, html, len);
    free(html);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int node_html_set(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const char *html;

    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &html) != JS_OK)
        return JS_THROW;
    if (!set_inner_html(h->j, h->node, html))
        return js_throw_error(c, JS_ERR_ERROR, "cannot set innerHTML");
    *r = js_undefined();
    return JS_OK;
}

static int node_outer_get(js_ctx *c, js_value t, int ac, js_value *av,
                          js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    char *html;
    unsigned long len;
    (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Node receiver");
    html = dom_serialize(h->node, &len);
    if (!html)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    *r = js_mkstring(c, html, len);
    free(html);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int attr_get_named(js_ctx *c, js_value t, const char *name, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const char *v;

    if (!h || h->node->type != DOM_ELEMENT) {
        *r = js_undefined();
        return JS_OK;
    }
    v = dom_get_attr(h->node, name);
    *r = js_mkcstring(c, v ? v : "");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int attr_set_named(js_ctx *c, js_value t, int ac, js_value *av,
                          const char *name, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const char *v;

    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &v) != JS_OK) return JS_THROW;
    if (!dom_set_attr(h->node, name, v))
        return js_throw_error(c, JS_ERR_ERROR, "cannot set attribute");
    h->j->dirty = 1;
    *r = js_undefined();
    return JS_OK;
}

#define ATTR_ACCESSORS(stem, attr)                                           \
static int stem##_get(js_ctx *c, js_value t, int ac, js_value *av,           \
                      js_value *r)                                            \
{ (void)ac; (void)av; return attr_get_named(c, t, attr, r); }                \
static int stem##_set(js_ctx *c, js_value t, int ac, js_value *av,           \
                      js_value *r)                                            \
{ return attr_set_named(c, t, ac, av, attr, r); }

ATTR_ACCESSORS(node_id, "id")
ATTR_ACCESSORS(node_class, "class")
ATTR_ACCESSORS(node_value, "value")
ATTR_ACCESSORS(node_name, "name")

static int url_attr_get(js_ctx *c, js_value t, const char *name, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const char *v;
    char out[URL_MAX];

    if (!h || h->node->type != DOM_ELEMENT) {
        *r = js_undefined();
        return JS_OK;
    }
    v = dom_get_attr(h->node, name);
    if (!v) v = "";
    if (*v && url_resolve_str(h->j->base_url, v, out, sizeof(out)) == URL_OK)
        v = out;
    *r = js_mkcstring(c, v);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

#define URL_ACCESSORS(stem, attr)                                            \
static int stem##_get(js_ctx *c, js_value t, int ac, js_value *av,           \
                      js_value *r)                                            \
{ (void)ac; (void)av; return url_attr_get(c, t, attr, r); }                  \
static int stem##_set(js_ctx *c, js_value t, int ac, js_value *av,           \
                      js_value *r)                                            \
{ return attr_set_named(c, t, ac, av, attr, r); }

URL_ACCESSORS(node_href, "href")
URL_ACCESSORS(node_src, "src")
URL_ACCESSORS(node_action, "action")

static int bool_attr_get(js_ctx *c, js_value t, const char *name, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    (void)c;
    *r = js_bool(h && h->node->type == DOM_ELEMENT &&
                 dom_has_attr(h->node, name));
    return JS_OK;
}

static int bool_attr_set(js_ctx *c, js_value t, int ac, js_value *av,
                         const char *name, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    int on;

    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    on = ac > 0 && js_to_boolean(av[0]);
    if (on) {
        if (!dom_set_attr(h->node, name, ""))
            return js_throw_error(c, JS_ERR_ERROR, "cannot set attribute");
    } else {
        dom_remove_attr(h->node, name);
    }
    h->j->dirty = 1;
    *r = js_undefined();
    return JS_OK;
}

#define BOOL_ACCESSORS(stem, attr)                                           \
static int stem##_get(js_ctx *c, js_value t, int ac, js_value *av,           \
                      js_value *r)                                            \
{ (void)ac; (void)av; return bool_attr_get(c, t, attr, r); }                 \
static int stem##_set(js_ctx *c, js_value t, int ac, js_value *av,           \
                      js_value *r)                                            \
{ return bool_attr_set(c, t, ac, av, attr, r); }

BOOL_ACCESSORS(node_checked, "checked")
BOOL_ACCESSORS(node_disabled, "disabled")

static int node_tag_get(js_ctx *c, js_value t, int ac, js_value *av,
                        js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    char name[130];
    unsigned long i, n;
    (void)ac; (void)av;

    if (!h || h->node->type != DOM_ELEMENT) {
        *r = js_undefined();
        return JS_OK;
    }
    n = strlen(h->node->tag);
    if (n >= sizeof(name)) n = sizeof(name) - 1;
    for (i = 0; i < n; i++) {
        int ch = (unsigned char)h->node->tag[i];
        name[i] = (char)(ch >= 'a' && ch <= 'z' ? ch - ('a' - 'A') : ch);
    }
    name[n] = 0;
    *r = js_mkcstring(c, name);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int node_type_get(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    (void)c; (void)ac; (void)av;
    *r = js_number(!h ? 0 : h->node->type == DOM_ELEMENT ? 1 :
                   h->node->type == DOM_TEXT ? 3 :
                   h->node->type == DOM_DOCUMENT ? 9 : 8);
    return JS_OK;
}

static int relative_get(js_ctx *c, js_value t, int which, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    struct dom_node *n = 0;
    if (h) {
        if (which == 0) n = h->node->parent;
        if (which == 1) n = h->node->first_child;
        if (which == 2) n = h->node->last_child;
        if (which == 3) n = h->node->next_sibling;
        if (which == 4) n = h->node->prev_sibling;
    }
    *r = h ? wrap_node(h->j, n) : js_null();
    return js_fatal(c) ? JS_THROW : JS_OK;
}

#define REL_GETTER(name, which)                                              \
static int name(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)    \
{ (void)ac; (void)av; return relative_get(c, t, which, r); }

REL_GETTER(node_parent_get, 0)
REL_GETTER(node_first_get, 1)
REL_GETTER(node_last_get, 2)
REL_GETTER(node_next_get, 3)
REL_GETTER(node_prev_get, 4)

static int node_children_get(js_ctx *c, js_value t, int ac, js_value *av,
                             js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    js_object *a;
    struct dom_node *n;
    (void)ac; (void)av;

    if (!h) { *r = js_null(); return JS_OK; }
    a = js_new_array(c);
    if (!a) return JS_THROW;
    for (n = h->node->first_child; n; n = n->next_sibling)
        if (n->type == DOM_ELEMENT &&
            js_array_push(c, a, wrap_node(h->j, n)) != JS_OK)
            return JS_THROW;
    *r = js_object_value(a);
    return JS_OK;
}

static int node_get_attr(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const char *name, *v;
    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &name) != JS_OK) return JS_THROW;
    v = dom_get_attr(h->node, name);
    *r = v ? js_mkcstring(c, v) : js_null();
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int node_set_attr(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const char *name, *value;
    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &name) != JS_OK ||
        string_arg(c, ac, av, 1, &value) != JS_OK) return JS_THROW;
    if (!dom_set_attr(h->node, name, value))
        return js_throw_error(c, JS_ERR_ERROR, "cannot set attribute");
    h->j->dirty = 1;
    *r = js_undefined();
    return JS_OK;
}

static int node_remove_attr(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const char *name;
    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &name) != JS_OK) return JS_THROW;
    dom_remove_attr(h->node, name);
    h->j->dirty = 1;
    *r = js_undefined();
    return JS_OK;
}

static int node_has_attr(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const char *name;
    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &name) != JS_OK) return JS_THROW;
    *r = js_bool(dom_has_attr(h->node, name));
    return JS_OK;
}

static int node_append(js_ctx *c, js_value t, int ac, js_value *av,
                       js_value *r)
{
    struct hostref *p = host(t, HOST_NODE);
    struct hostref *ch = ac ? host(av[0], HOST_NODE) : 0;
    if (!p || !ch || p->j != ch->j)
        return js_throw_error(c, JS_ERR_TYPE, "appendChild requires a Node");
    if (!dom_append_child(p->node, ch->node))
        return js_throw_error(c, JS_ERR_ERROR, "cannot append child");
    p->j->dirty = 1;
    *r = av[0];
    return JS_OK;
}

static int node_remove(js_ctx *c, js_value t, int ac, js_value *av,
                       js_value *r)
{
    struct hostref *p = host(t, HOST_NODE);
    struct hostref *ch = ac ? host(av[0], HOST_NODE) : 0;
    if (!p || !ch || ch->node->parent != p->node)
        return js_throw_error(c, JS_ERR_ERROR, "node is not a child");
    dom_remove_child(p->node, ch->node);
    p->j->dirty = 1;
    *r = av[0];
    return JS_OK;
}

static int node_query(js_ctx *c, js_value t, int ac, js_value *av,
                      js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const char *sel;
    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &sel) != JS_OK) return JS_THROW;
    *r = wrap_node(h->j, query_first(h->node, sel));
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int node_query_all(js_ctx *c, js_value t, int ac, js_value *av,
                          js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const char *sel;
    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &sel) != JS_OK) return JS_THROW;
    return query_all(c, h->j, h->node, sel, r);
}

/* ------------------------------------------------------------------ */
/* classList and style */

static int token_present(const char *list, const char *tok)
{
    unsigned long tn = strlen(tok);
    const char *p = list;
    while (*p) {
        const char *s;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        if ((unsigned long)(p - s) == tn && !memcmp(s, tok, tn))
            return 1;
    }
    return 0;
}

static int class_change(struct hostref *h, const char *tok, int mode, int *now)
{
    const char *old;
    char *out;
    unsigned long used = 0, cap;
    const char *p;
    int had;

    if (!*tok || strlen(tok) > 128 || strchr(tok, ' ') || strchr(tok, '\t'))
        return 0;
    old = dom_get_attr(h->node, "class");
    if (!old) old = "";
    had = token_present(old, tok);
    if (mode == 2) mode = had ? 0 : 1;
    if ((mode == 1 && had) || (mode == 0 && !had)) {
        if (now) *now = had;
        return 1;
    }
    cap = strlen(old) + strlen(tok) + 2;
    if (cap > JSDOM_CLASS_MAX)
        return 0;
    out = (char *)malloc(cap);
    if (!out) return 0;
    for (p = old; *p;) {
        const char *s;
        unsigned long n;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        n = (unsigned long)(p - s);
        if (!n || (n == strlen(tok) && !memcmp(s, tok, n)))
            continue;
        if (used) out[used++] = ' ';
        memcpy(out + used, s, n);
        used += n;
    }
    if (mode == 1) {
        if (used) out[used++] = ' ';
        memcpy(out + used, tok, strlen(tok));
        used += strlen(tok);
    }
    out[used] = 0;
    if (!dom_set_attr(h->node, "class", out)) {
        free(out);
        return 0;
    }
    free(out);
    h->j->dirty = 1;
    if (now) *now = mode == 1;
    return 1;
}

static int class_method(js_ctx *c, js_value t, int ac, js_value *av,
                        js_value *r, int mode)
{
    struct hostref *h = host(t, HOST_CLASSLIST);
    const char *tok;
    int now = 0;
    if (!h || string_arg(c, ac, av, 0, &tok) != JS_OK)
        return h ? JS_THROW :
            js_throw_error(c, JS_ERR_TYPE, "illegal DOMTokenList receiver");
    if (!class_change(h, tok, mode, &now))
        return js_throw_error(c, JS_ERR_ERROR, "invalid class token");
    *r = mode == 2 ? js_bool(now) : js_undefined();
    return JS_OK;
}

static int class_add(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)
{ return class_method(c, t, ac, av, r, 1); }
static int class_remove(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)
{ return class_method(c, t, ac, av, r, 0); }
static int class_toggle(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)
{ return class_method(c, t, ac, av, r, 2); }
static int class_contains(js_ctx *c, js_value t, int ac, js_value *av,
                          js_value *r)
{
    struct hostref *h = host(t, HOST_CLASSLIST);
    const char *tok, *v;
    if (!h || string_arg(c, ac, av, 0, &tok) != JS_OK)
        return h ? JS_THROW :
            js_throw_error(c, JS_ERR_TYPE, "illegal DOMTokenList receiver");
    v = dom_get_attr(h->node, "class");
    *r = js_bool(v && token_present(v, tok));
    return JS_OK;
}

static int node_classlist_get(js_ctx *c, js_value t, int ac, js_value *av,
                              js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    js_object *o;
    (void)c; (void)ac; (void)av;
    if (!h) { *r = js_undefined(); return JS_OK; }
    o = new_host(h->j, h->node, HOST_CLASSLIST, h->j->class_proto);
    if (!o) return JS_THROW;
    *r = js_object_value(o);
    return JS_OK;
}

static int style_css_get(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = host(t, HOST_STYLE);
    const char *v;
    (void)ac; (void)av;
    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal style receiver");
    v = dom_get_attr(h->node, "style");
    *r = js_mkcstring(c, v ? v : "");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int style_css_set(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = host(t, HOST_STYLE);
    const char *v;
    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal style receiver");
    if (string_arg(c, ac, av, 0, &v) != JS_OK) return JS_THROW;
    if (strlen(v) >= JSDOM_STYLE_MAX ||
        !dom_set_attr(h->node, "style", v))
        return js_throw_error(c, JS_ERR_ERROR, "style is too large");
    h->j->dirty = 1;
    *r = js_undefined();
    return JS_OK;
}

static int style_prop_set(js_ctx *c, js_value t, int ac, js_value *av,
                          js_value *r, const char *prop)
{
    struct hostref *h = host(t, HOST_STYLE);
    const char *old, *v;
    char *out;
    unsigned long n;
    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal style receiver");
    if (string_arg(c, ac, av, 0, &v) != JS_OK) return JS_THROW;
    old = dom_get_attr(h->node, "style");
    if (!old) old = "";
    n = strlen(old) + strlen(prop) + strlen(v) + 4;
    if (n >= JSDOM_STYLE_MAX)
        return js_throw_error(c, JS_ERR_RANGE, "style is too large");
    out = (char *)malloc(n + 1);
    if (!out) return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    snprintf(out, n + 1, "%s%s%s:%s;", old,
             *old && old[strlen(old) - 1] != ';' ? ";" : "", prop, v);
    if (!dom_set_attr(h->node, "style", out)) {
        free(out);
        return js_throw_error(c, JS_ERR_ERROR, "cannot set style");
    }
    free(out);
    h->j->dirty = 1;
    *r = js_undefined();
    return JS_OK;
}

static int style_prop_get(js_ctx *c, js_value t, const char *prop, js_value *r)
{
    struct hostref *h = host(t, HOST_STYLE);
    const char *s, *best = 0, *p;
    unsigned long pn = strlen(prop), n = 0;
    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal style receiver");
    s = dom_get_attr(h->node, "style");
    if (!s) s = "";
    for (p = s; *p;) {
        const char *name, *val, *end;
        while (*p == ' ' || *p == '\t' || *p == ';') p++;
        name = p;
        while (*p && *p != ':' && *p != ';') p++;
        if (*p != ':') { while (*p && *p != ';') p++; continue; }
        end = p++;
        while (end > name && (end[-1] == ' ' || end[-1] == '\t')) end--;
        while (*p == ' ' || *p == '\t') p++;
        val = p;
        while (*p && *p != ';') p++;
        if (ci_eq_n(name, (unsigned long)(end - name), prop, pn)) {
            best = val;
            n = (unsigned long)(p - val);
            while (n && (best[n - 1] == ' ' || best[n - 1] == '\t')) n--;
        }
    }
    *r = best ? js_mkstring(c, best, n) : js_mkcstring(c, "");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

#define STYLE_ACCESSORS(stem, prop)                                          \
static int stem##_get(js_ctx *c, js_value t, int ac, js_value *av,           \
                      js_value *r)                                            \
{ (void)ac; (void)av; return style_prop_get(c, t, prop, r); }                \
static int stem##_set(js_ctx *c, js_value t, int ac, js_value *av,           \
                      js_value *r)                                            \
{ return style_prop_set(c, t, ac, av, r, prop); }

STYLE_ACCESSORS(style_display, "display")
STYLE_ACCESSORS(style_color, "color")
STYLE_ACCESSORS(style_bg, "background-color")
STYLE_ACCESSORS(style_width, "width")
STYLE_ACCESSORS(style_height, "height")
STYLE_ACCESSORS(style_margin, "margin")
STYLE_ACCESSORS(style_padding, "padding")
STYLE_ACCESSORS(style_font_size, "font-size")
STYLE_ACCESSORS(style_font_weight, "font-weight")
STYLE_ACCESSORS(style_text_align, "text-align")
STYLE_ACCESSORS(style_visibility, "visibility")

static int node_style_get(js_ctx *c, js_value t, int ac, js_value *av,
                          js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    js_object *o;
    (void)c; (void)ac; (void)av;
    if (!h) { *r = js_undefined(); return JS_OK; }
    o = new_host(h->j, h->node, HOST_STYLE, h->j->style_proto);
    if (!o) return JS_THROW;
    *r = js_object_value(o);
    return JS_OK;
}

/* ------------------------------------------------------------------ */
/* Events */

static int event_prevent(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    (void)ac; (void)av;
    if (!js_is_object(t))
        return js_throw_error(c, JS_ERR_TYPE, "illegal Event receiver");
    if (js_set(c, t, "defaultPrevented", js_bool(1)) != JS_OK)
        return JS_THROW;
    *r = js_undefined();
    return JS_OK;
}

static int node_add_listener(js_ctx *c, js_value t, int ac, js_value *av,
                             js_value *r)
{
    struct hostref *h = host(t, 0);
    const char *type;
    char key[96];
    js_value list;
    js_object *a;

    if (!h || (h->kind != HOST_NODE && h->kind != HOST_DOCUMENT))
        return js_throw_error(c, JS_ERR_TYPE, "illegal EventTarget");
    if (string_arg(c, ac, av, 0, &type) != JS_OK) return JS_THROW;
    if (strlen(type) > 64 || ac < 2 || !js_is_function(av[1]))
        return js_throw_error(c, JS_ERR_TYPE, "invalid event listener");
    snprintf(key, sizeof(key), "__listeners_%s", type);
    if (js_get(c, t, key, &list) != JS_OK || !js_is_array(list)) {
        a = js_new_array(c);
        if (!a || js_define(c, t.u.obj, key, js_object_value(a)) != JS_OK)
            return JS_THROW;
    } else {
        a = list.u.obj;
    }
    if (js_array_push(c, a, av[1]) != JS_OK) return JS_THROW;
    *r = js_undefined();
    return JS_OK;
}

static int global_add_listener(js_ctx *c, js_value t, int ac, js_value *av,
                               js_value *r)
{
    const char *type;
    char key[96];
    js_value list;
    js_object *a;

    (void)t;
    if (string_arg(c, ac, av, 0, &type) != JS_OK) return JS_THROW;
    if (strlen(type) > 64 || ac < 2 || !js_is_function(av[1]))
        return js_throw_error(c, JS_ERR_TYPE, "invalid event listener");
    snprintf(key, sizeof(key), "__listeners_%s", type);
    if (js_get(c, js_global(c), key, &list) != JS_OK ||
        !js_is_array(list)) {
        a = js_new_array(c);
        if (!a || js_define(c, js_global(c).u.obj, key,
                            js_object_value(a)) != JS_OK)
            return JS_THROW;
    } else {
        a = list.u.obj;
    }
    if (js_array_push(c, a, av[1]) != JS_OK) return JS_THROW;
    *r = js_undefined();
    return JS_OK;
}

static int node_click(js_ctx *c, js_value t, int ac, js_value *av,
                      js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    char err[160];
    (void)ac; (void)av;
    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    jsdom_dispatch(h->j, h->node, "click", err, sizeof(err));
    *r = js_undefined();
    return JS_OK;
}

/* ------------------------------------------------------------------ */
/* Document */

static int doc_by_id(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    const char *id;
    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal Document receiver");
    if (string_arg(c, ac, av, 0, &id) != JS_OK) return JS_THROW;
    *r = wrap_node(h->j, dom_get_element_by_id(h->j->doc, id));
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int doc_query(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    const char *sel;
    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal Document receiver");
    if (string_arg(c, ac, av, 0, &sel) != JS_OK) return JS_THROW;
    *r = wrap_node(h->j, query_first(h->j->doc->root, sel));
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int doc_query_all(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    const char *sel;
    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal Document receiver");
    if (string_arg(c, ac, av, 0, &sel) != JS_OK) return JS_THROW;
    return query_all(c, h->j, h->j->doc->root, sel, r);
}

static int doc_create_element(js_ctx *c, js_value t, int ac, js_value *av,
                              js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    const char *name;
    struct dom_node *n;
    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal Document receiver");
    if (string_arg(c, ac, av, 0, &name) != JS_OK) return JS_THROW;
    n = dom_create_element(h->j->doc, name, (long)strlen(name));
    if (!n) return js_throw_error(c, JS_ERR_ERROR, "cannot create element");
    *r = wrap_node(h->j, n);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int doc_create_text(js_ctx *c, js_value t, int ac, js_value *av,
                           js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    const char *text;
    struct dom_node *n;
    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal Document receiver");
    if (string_arg(c, ac, av, 0, &text) != JS_OK) return JS_THROW;
    n = dom_create_text(h->j->doc, text, strlen(text));
    if (!n) return js_throw_error(c, JS_ERR_ERROR, "cannot create text node");
    *r = wrap_node(h->j, n);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int doc_root_get(js_ctx *c, js_value t, int ac, js_value *av,
                        js_value *r, int which)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    struct dom_node *n = 0;
    (void)ac; (void)av;
    if (h) {
        if (which == 0) n = h->j->doc->html;
        if (which == 1) n = h->j->doc->head;
        if (which == 2) n = h->j->doc->body;
    }
    *r = h ? wrap_node(h->j, n) : js_null();
    return js_fatal(c) ? JS_THROW : JS_OK;
}

#define DOC_ROOT_GETTER(name, which)                                         \
static int name(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)    \
{ return doc_root_get(c, t, ac, av, r, which); }
DOC_ROOT_GETTER(doc_html_get, 0)
DOC_ROOT_GETTER(doc_head_get, 1)
DOC_ROOT_GETTER(doc_body_get, 2)

static int doc_title_get(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    (void)ac; (void)av;
    *r = js_mkcstring(c, h && h->j->doc->title ? h->j->doc->title : "");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int doc_title_set(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    struct dom_node *title;
    const char *v;
    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal Document receiver");
    if (string_arg(c, ac, av, 0, &v) != JS_OK) return JS_THROW;
    title = dom_find_tag(h->j->doc->head, "title");
    if (!title) {
        title = dom_create_element(h->j->doc, "title", 5);
        if (!title || !dom_append_child(h->j->doc->head, title))
            return js_throw_error(c, JS_ERR_ERROR, "cannot create title");
    }
    if (!set_text(h->j, title, v))
        return js_throw_error(c, JS_ERR_ERROR, "cannot set title");
    h->j->doc->title = title->first_child ? title->first_child->text : "";
    h->j->dirty = 1;
    *r = js_undefined();
    return JS_OK;
}

static int doc_url_get(js_ctx *c, js_value t, int ac, js_value *av,
                       js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    (void)ac; (void)av;
    *r = js_mkcstring(c, h && h->j->url ? h->j->url : "");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int doc_ready_get(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    (void)t; (void)ac; (void)av;
    *r = js_mkcstring(c, "complete");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int doc_cookie_get(js_ctx *c, js_value t, int ac, js_value *av,
                          js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    const char *v = "";
    (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Document receiver");
    if (h->j->cookie_get)
        v = h->j->cookie_get(h->j->print_user);
    *r = js_mkcstring(c, v ? v : "");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int doc_cookie_set(js_ctx *c, js_value t, int ac, js_value *av,
                          js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    const char *v;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Document receiver");
    if (string_arg(c, ac, av, 0, &v) != JS_OK)
        return JS_THROW;
    if (h->j->cookie_set && h->j->cookie_set(h->j->print_user, v) < 0)
        return js_throw_error(c, JS_ERR_ERROR, "cannot set cookie");
    *r = js_undefined();
    return JS_OK;
}

/* ------------------------------------------------------------------ */
/* location/window */

static int queue_navigation(struct jsdom *j, const char *ref)
{
    char out[URL_MAX];
    char *copy;
    if (url_resolve_str(j->base_url, ref, out, sizeof(out)) != URL_OK)
        return 0;
    copy = dup_z(out);
    if (!copy) return 0;
    free(j->navigation);
    j->navigation = copy;
    return 1;
}

static int location_href_get(js_ctx *c, js_value t, int ac, js_value *av,
                             js_value *r)
{
    struct hostref *h = host(t, HOST_LOCATION);
    (void)ac; (void)av;
    *r = js_mkcstring(c, h && h->j->url ? h->j->url : "");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int location_href_set(js_ctx *c, js_value t, int ac, js_value *av,
                             js_value *r)
{
    struct hostref *h = host(t, HOST_LOCATION);
    const char *v;
    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal Location receiver");
    if (string_arg(c, ac, av, 0, &v) != JS_OK) return JS_THROW;
    if (!queue_navigation(h->j, v))
        return js_throw_error(c, JS_ERR_ERROR, "invalid navigation URL");
    *r = js_undefined();
    return JS_OK;
}

static int location_assign(js_ctx *c, js_value t, int ac, js_value *av,
                           js_value *r)
{
    return location_href_set(c, t, ac, av, r);
}

static int global_alert(js_ctx *c, js_value t, int ac, js_value *av,
                        js_value *r)
{
    struct jsdom *j = binding(c);
    const char *s = "";
    (void)t;
    if (ac && string_arg(c, ac, av, 0, &s) != JS_OK) return JS_THROW;
    if (j && j->print) j->print(j->print_user, s);
    *r = js_undefined();
    return JS_OK;
}

static int global_set_timeout(js_ctx *c, js_value t, int ac, js_value *av,
                              js_value *r)
{
    struct jsdom *j = binding(c);
    int i;
    (void)t;

    if (!j || ac < 1 || !js_is_function(av[0]))
        return js_throw_error(c, JS_ERR_TYPE,
                              "setTimeout requires a function");
    for (i = 0; i < (int)(sizeof(j->timers) / sizeof(j->timers[0])); i++)
        if (!j->timers[i].active) {
            j->timers[i].fn = av[0];
            j->timers[i].active = 1;
            *r = js_number((double)(i + 1));
            return JS_OK;
        }
    return js_throw_error(c, JS_ERR_RANGE, "too many pending timers");
}

static int global_clear_timeout(js_ctx *c, js_value t, int ac, js_value *av,
                                js_value *r)
{
    struct jsdom *j = binding(c);
    double d = 0;
    int id;
    (void)t;

    if (ac && js_to_number(c, av[0], &d) != JS_OK)
        return JS_THROW;
    id = (int)d;
    if (j && id > 0 && id <= (int)(sizeof(j->timers) / sizeof(j->timers[0])))
        j->timers[id - 1].active = 0;
    *r = js_undefined();
    return JS_OK;
}

static int global_animation_frame(js_ctx *c, js_value t, int ac,
                                  js_value *av, js_value *r)
{
    return global_set_timeout(c, t, ac, av, r);
}

static int global_performance_now(js_ctx *c, js_value t, int ac,
                                  js_value *av, js_value *r)
{
    (void)c; (void)t; (void)ac; (void)av;
    *r = js_number(0);
    return JS_OK;
}

#define CSS_NAME_ENTRY(n, s) s,
static const char *const display_names[] = {
    CSS_DISPLAY_LIST(CSS_NAME_ENTRY)
};
static const char *const position_names[] = {
    CSS_POSITION_LIST(CSS_NAME_ENTRY)
};
static const char *const float_names[] = {
    CSS_FLOAT_LIST(CSS_NAME_ENTRY)
};
static const char *const clear_names[] = {
    CSS_CLEAR_LIST(CSS_NAME_ENTRY)
};
static const char *const align_names[] = {
    CSS_TEXTALIGN_LIST(CSS_NAME_ENTRY)
};
static const char *const visibility_names[] = {
    CSS_VISIBILITY_LIST(CSS_NAME_ENTRY)
};
#undef CSS_NAME_ENTRY

static int computed_put(js_ctx *c, js_value obj, const char *name,
                        const char *value)
{
    return js_set(c, obj, name, js_mkcstring(c, value));
}

static void computed_len(char *out, unsigned long outsz, struct css_len v)
{
    if (v.type == CSS_LEN_PX)
        snprintf(out, outsz, "%dpx", v.v);
    else if (v.type == CSS_LEN_PCT)
        snprintf(out, outsz, "%d.%03d%%", v.v / 1000,
                 v.v < 0 ? -(v.v % 1000) : v.v % 1000);
    else if (v.type == CSS_LEN_NUMBER)
        snprintf(out, outsz, "%d.%03d", v.v / 1000,
                 v.v < 0 ? -(v.v % 1000) : v.v % 1000);
    else if (v.type == CSS_LEN_NONE)
        snprintf(out, outsz, "none");
    else if (v.type == CSS_LEN_NORMAL)
        snprintf(out, outsz, "normal");
    else
        snprintf(out, outsz, "auto");
}

static int global_computed_style(js_ctx *c, js_value t, int ac,
                                 js_value *av, js_value *r)
{
    struct hostref *h = ac ? host(av[0], HOST_NODE) : 0;
    const struct computed_style *s;
    js_object *o;
    js_value ov;
    char buf[64];
    (void)t;

    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE,
                              "getComputedStyle requires an Element");
    o = js_new_object(c);
    if (!o)
        return JS_THROW;
    ov = js_object_value(o);
    s = (const struct computed_style *)h->node->style;
    if (!s) {
        *r = ov;
        return JS_OK;
    }
#define ENUM_PUT(prop, value, names) do {                                    \
    unsigned _v = (unsigned)(value);                                         \
    if (_v < sizeof(names) / sizeof((names)[0]) &&                            \
        computed_put(c, ov, prop, (names)[_v]) != JS_OK) return JS_THROW;     \
} while (0)
    ENUM_PUT("display", s->display, display_names);
    ENUM_PUT("position", s->position, position_names);
    ENUM_PUT("float", s->css_float, float_names);
    ENUM_PUT("clear", s->clear, clear_names);
    ENUM_PUT("textAlign", s->text_align, align_names);
    ENUM_PUT("visibility", s->visibility, visibility_names);
#undef ENUM_PUT
    snprintf(buf, sizeof(buf), "rgb(%u, %u, %u)",
             CSS_COLOR_R(s->color), CSS_COLOR_G(s->color),
             CSS_COLOR_B(s->color));
    if (computed_put(c, ov, "color", buf) != JS_OK) return JS_THROW;
    if (!CSS_COLOR_A(s->background_color)) {
        snprintf(buf, sizeof(buf), "transparent");
    } else {
        snprintf(buf, sizeof(buf), "rgba(%u, %u, %u, %u)",
                 CSS_COLOR_R(s->background_color),
                 CSS_COLOR_G(s->background_color),
                 CSS_COLOR_B(s->background_color),
                 CSS_COLOR_A(s->background_color));
    }
    if (computed_put(c, ov, "backgroundColor", buf) != JS_OK)
        return JS_THROW;
    snprintf(buf, sizeof(buf), "%dpx", s->font_size);
    if (computed_put(c, ov, "fontSize", buf) != JS_OK) return JS_THROW;
    snprintf(buf, sizeof(buf), "%u", (unsigned)s->font_weight);
    if (computed_put(c, ov, "fontWeight", buf) != JS_OK) return JS_THROW;
    computed_len(buf, sizeof(buf), s->width);
    if (computed_put(c, ov, "width", buf) != JS_OK) return JS_THROW;
    computed_len(buf, sizeof(buf), s->height);
    if (computed_put(c, ov, "height", buf) != JS_OK) return JS_THROW;
    computed_len(buf, sizeof(buf), s->line_height);
    if (computed_put(c, ov, "lineHeight", buf) != JS_OK) return JS_THROW;
    *r = ov;
    return JS_OK;
}

static int js_b64_value(int ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static int global_atob(js_ctx *c, js_value t, int ac, js_value *av,
                       js_value *r)
{
    const char *s;
    unsigned long n, i, used = 0;
    char *out;
    int bits = -8;
    unsigned val = 0;
    (void)t;

    if (string_arg(c, ac, av, 0, &s) != JS_OK)
        return JS_THROW;
    n = strlen(s);
    out = (char *)malloc(n * 3 / 4 + 2);
    if (!out)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    for (i = 0; i < n; i++) {
        int d;
        if (s[i] == '=') break;
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')
            continue;
        d = js_b64_value((unsigned char)s[i]);
        if (d < 0) {
            free(out);
            return js_throw_error(c, JS_ERR_ERROR,
                                  "invalid base64 input");
        }
        val = (val << 6) | (unsigned)d;
        bits += 6;
        if (bits >= 0) {
            out[used++] = (char)((val >> bits) & 0xff);
            bits -= 8;
        }
    }
    *r = js_mkstring(c, out, used);
    free(out);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int global_btoa(js_ctx *c, js_value t, int ac, js_value *av,
                       js_value *r)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *s;
    unsigned long n, i, used = 0;
    char *out;
    (void)t;

    if (string_arg(c, ac, av, 0, &s) != JS_OK)
        return JS_THROW;
    n = strlen(s);
    if (n > (~0UL - 4) / 4 * 3)
        return js_throw_error(c, JS_ERR_RANGE, "input is too large");
    out = (char *)malloc(((n + 2) / 3) * 4 + 1);
    if (!out)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    for (i = 0; i < n; i += 3) {
        unsigned a = (unsigned char)s[i];
        unsigned b = i + 1 < n ? (unsigned char)s[i + 1] : 0;
        unsigned d = i + 2 < n ? (unsigned char)s[i + 2] : 0;

        out[used++] = tab[a >> 2];
        out[used++] = tab[((a & 3) << 4) | (b >> 4)];
        out[used++] = i + 1 < n ? tab[((b & 15) << 2) | (d >> 6)] : '=';
        out[used++] = i + 2 < n ? tab[d & 63] : '=';
    }
    *r = js_mkstring(c, out, used);
    free(out);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

/* ------------------------------------------------------------------ */
/* Promise-backed fetch and Response                                  */

static struct response_state *response_state_of(js_value v, int kind)
{
    struct hostref *h = host(v, kind);

    return h ? (struct response_state *)h->data : 0;
}

static int resolved_promise(js_ctx *c, js_value value, int rejected,
                            js_value *out)
{
    *out = js_promise_new(c);
    if (!js_is_promise(*out))
        return JS_THROW;
    return rejected ? js_promise_reject(c, *out, value)
                    : js_promise_resolve(c, *out, value);
}

static int response_status_get(js_ctx *c, js_value t, int ac, js_value *av,
                               js_value *r)
{
    struct response_state *s = response_state_of(t, HOST_RESPONSE);
    (void)c; (void)ac; (void)av;
    if (!s)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Response receiver");
    *r = js_number((double)s->status);
    return JS_OK;
}

static int response_ok_get(js_ctx *c, js_value t, int ac, js_value *av,
                           js_value *r)
{
    struct response_state *s = response_state_of(t, HOST_RESPONSE);
    (void)c; (void)ac; (void)av;
    if (!s)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Response receiver");
    *r = js_bool(s->status >= 200 && s->status < 300);
    return JS_OK;
}

static int response_string_get(js_ctx *c, js_value t, int ac, js_value *av,
                               js_value *r, int field)
{
    struct response_state *s = response_state_of(t, HOST_RESPONSE);
    const char *text;
    (void)ac; (void)av;

    if (!s)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Response receiver");
    text = field == 0 ? s->status_text : s->url;
    *r = js_mkcstring(c, text ? text : "");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int response_status_text_get(js_ctx *c, js_value t, int ac,
                                    js_value *av, js_value *r)
{
    return response_string_get(c, t, ac, av, r, 0);
}

static int response_url_get(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    return response_string_get(c, t, ac, av, r, 1);
}

static int response_body_used_get(js_ctx *c, js_value t, int ac,
                                  js_value *av, js_value *r)
{
    struct response_state *s = response_state_of(t, HOST_RESPONSE);
    (void)ac; (void)av;
    if (!s)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Response receiver");
    *r = js_bool(s->body_used);
    return JS_OK;
}

static int headers_get(js_ctx *c, js_value t, int ac, js_value *av,
                       js_value *r)
{
    struct response_state *s = response_state_of(t, HOST_HEADERS);
    const char *name;

    if (!s)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Headers receiver");
    if (string_arg(c, ac, av, 0, &name) != JS_OK)
        return JS_THROW;
    if (ci_eq_n(name, strlen(name), "content-type", 12)) {
        *r = s->content_type
            ? js_mkcstring(c, s->content_type) : js_null();
        return js_fatal(c) ? JS_THROW : JS_OK;
    }
    *r = js_null();
    return JS_OK;
}

static int response_headers_get(js_ctx *c, js_value t, int ac, js_value *av,
                                js_value *r)
{
    struct hostref *rh = host(t, HOST_RESPONSE);
    struct response_state *s;
    js_object *o;
    (void)ac; (void)av;

    if (!rh)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Response receiver");
    s = (struct response_state *)rh->data;
    s->refs++;
    o = new_host_data(rh->j, 0, HOST_HEADERS, s, rh->j->headers_proto);
    if (!o) {
        response_release(s);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    *r = js_object_value(o);
    return JS_OK;
}

static int response_text(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct response_state *s = response_state_of(t, HOST_RESPONSE);
    js_value text;
    (void)ac; (void)av;

    if (!s)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Response receiver");
    if (s->body_used)
        return js_throw_error(c, JS_ERR_TYPE,
                              "Response body has already been consumed");
    s->body_used = 1;
    text = js_mkstring(c, s->body ? s->body : "", s->body_len);
    if (js_fatal(c))
        return JS_THROW;
    return resolved_promise(c, text, 0, r);
}

static int response_json(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct response_state *s = response_state_of(t, HOST_RESPONSE);
    js_value json, parse, body, parsed, reason;
    (void)ac; (void)av;

    if (!s)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Response receiver");
    if (s->body_used)
        return js_throw_error(c, JS_ERR_TYPE,
                              "Response body has already been consumed");
    s->body_used = 1;
    body = js_mkstring(c, s->body ? s->body : "", s->body_len);
    if (js_fatal(c) ||
        js_get(c, js_global(c), "JSON", &json) != JS_OK ||
        js_get(c, json, "parse", &parse) != JS_OK)
        return JS_THROW;
    if (js_call(c, parse, json, 1, &body, &parsed) == JS_OK)
        return resolved_promise(c, parsed, 0, r);
    reason = js_exception(c);
    if (js_fatal(c))
        return JS_THROW;
    js_clear_exception(c);
    return resolved_promise(c, reason, 1, r);
}

static int response_arraybuffer(js_ctx *c, js_value t, int ac,
                                js_value *av, js_value *r)
{
    struct response_state *s = response_state_of(t, HOST_RESPONSE);
    js_value buffer;
    (void)ac; (void)av;

    if (!s)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Response receiver");
    if (s->body_used)
        return js_throw_error(c, JS_ERR_TYPE,
                              "Response body has already been consumed");
    s->body_used = 1;
    buffer = js_arraybuffer_new(c, s->body, s->body_len);
    if (!js_is_arraybuffer(buffer))
        return JS_THROW;
    return resolved_promise(c, buffer, 0, r);
}

static void fetch_response_clear(struct jsdom_fetch_response *f)
{
    if (!f) return;
    free(f->status_text);
    free(f->url);
    free(f->content_type);
    free(f->body);
    memset(f, 0, sizeof(*f));
}

static js_value make_response(struct jsdom *j,
                              struct jsdom_fetch_response *f)
{
    struct response_state *s;
    js_object *o;

    s = (struct response_state *)calloc(1, sizeof(*s));
    if (!s)
        return js_undefined();
    s->refs = 1;
    s->status = f->status;
    s->status_text = f->status_text;
    s->url = f->url;
    s->content_type = f->content_type;
    s->body = f->body;
    s->body_len = f->body_len;
    memset(f, 0, sizeof(*f));
    o = new_host_data(j, 0, HOST_RESPONSE, s, j->response_proto);
    if (!o) {
        response_release(s);
        return js_undefined();
    }
    return js_object_value(o);
}

static int global_fetch(js_ctx *c, js_value t, int ac, js_value *av,
                        js_value *r)
{
    struct jsdom *j = binding(c);
    const char *input;
    char absolute[URL_MAX];
    char *method = 0, *body = 0;
    unsigned long body_len = 0;
    int slot = -1, i;
    js_value promise;
    (void)t;

    if (!j || string_arg(c, ac, av, 0, &input) != JS_OK)
        return JS_THROW;
    if (url_resolve_str(j->base_url, input, absolute, sizeof(absolute)) !=
        URL_OK)
        return js_throw_error(c, JS_ERR_TYPE, "invalid fetch URL");
    method = dup_z("GET");
    if (!method)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    if (ac > 1 && js_is_object(av[1])) {
        js_value v, sv;

        if (js_get(c, av[1], "method", &v) != JS_OK) {
            free(method);
            return JS_THROW;
        }
        if (!js_is_undefined(v)) {
            const char *s;
            unsigned long n;

            if (js_to_string(c, v, &sv) != JS_OK) {
                free(method);
                return JS_THROW;
            }
            s = js_string_bytes(sv, &n);
            if (n == 0 || n > 15) {
                free(method);
                return js_throw_error(c, JS_ERR_TYPE,
                                      "invalid fetch method");
            }
            free(method);
            method = dup_n(s, n);
            if (!method)
                return js_throw_error(c, JS_ERR_ERROR, "out of memory");
            for (i = 0; method[i]; i++)
                if (method[i] >= 'a' && method[i] <= 'z')
                    method[i] = (char)(method[i] - ('a' - 'A'));
        }
        if (js_get(c, av[1], "body", &v) != JS_OK) {
            free(method);
            return JS_THROW;
        }
        if (!js_is_undefined(v) && !js_is_null(v)) {
            const char *s;

            if (js_to_string(c, v, &sv) != JS_OK) {
                free(method);
                return JS_THROW;
            }
            s = js_string_bytes(sv, &body_len);
            body = dup_n(s, body_len);
            if (!body) {
                free(method);
                return js_throw_error(c, JS_ERR_ERROR, "out of memory");
            }
        }
    }
    for (i = 0; i < JSDOM_FETCH_MAX; i++)
        if (!j->fetches[i].active) {
            slot = i;
            break;
        }
    if (slot < 0) {
        free(body);
        free(method);
        return js_throw_error(c, JS_ERR_RANGE,
                              "too many pending fetch requests");
    }
    promise = js_promise_new(c);
    if (!js_is_promise(promise)) {
        free(body);
        free(method);
        return JS_THROW;
    }
    j->fetches[slot].url = dup_z(absolute);
    if (!j->fetches[slot].url) {
        free(body);
        free(method);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    j->fetches[slot].promise = promise;
    j->fetches[slot].method = method;
    j->fetches[slot].body = body;
    j->fetches[slot].body_len = body_len;
    j->fetches[slot].active = 1;
    *r = promise;
    return JS_OK;
}

/* ------------------------------------------------------------------ */
/* Construction and public operations */

static void js_print(void *user, const char *text)
{
    struct jsdom *j = (struct jsdom *)user;
    if (j->print) j->print(j->print_user, text);
}

static int define_node_proto(struct jsdom *j)
{
    js_ctx *c = j->ctx;
    js_object *p = j->node_proto;

#define ACC(name, get, set) if (js_define_accessor(c, p, name, get, set, 1) != JS_OK) return 0
#define FUN(name, fn, n) if (js_define_native(c, p, name, fn, n) != JS_OK) return 0
    ACC("nodeType", node_type_get, 0);
    ACC("tagName", node_tag_get, 0);
    ACC("nodeName", node_tag_get, 0);
    ACC("textContent", node_text_get, node_text_set);
    ACC("innerText", node_text_get, node_text_set);
    ACC("innerHTML", node_html_get, node_html_set);
    ACC("outerHTML", node_outer_get, 0);
    ACC("id", node_id_get, node_id_set);
    ACC("className", node_class_get, node_class_set);
    ACC("value", node_value_get, node_value_set);
    ACC("name", node_name_get, node_name_set);
    ACC("href", node_href_get, node_href_set);
    ACC("src", node_src_get, node_src_set);
    ACC("action", node_action_get, node_action_set);
    ACC("checked", node_checked_get, node_checked_set);
    ACC("disabled", node_disabled_get, node_disabled_set);
    ACC("parentNode", node_parent_get, 0);
    ACC("parentElement", node_parent_get, 0);
    ACC("firstChild", node_first_get, 0);
    ACC("lastChild", node_last_get, 0);
    ACC("nextSibling", node_next_get, 0);
    ACC("previousSibling", node_prev_get, 0);
    ACC("children", node_children_get, 0);
    ACC("classList", node_classlist_get, 0);
    ACC("style", node_style_get, 0);
    FUN("getAttribute", node_get_attr, 1);
    FUN("setAttribute", node_set_attr, 2);
    FUN("removeAttribute", node_remove_attr, 1);
    FUN("hasAttribute", node_has_attr, 1);
    FUN("appendChild", node_append, 1);
    FUN("removeChild", node_remove, 1);
    FUN("querySelector", node_query, 1);
    FUN("querySelectorAll", node_query_all, 1);
    FUN("addEventListener", node_add_listener, 2);
    FUN("click", node_click, 0);
#undef FUN
#undef ACC
    return 1;
}

static int define_other_protos(struct jsdom *j)
{
    js_ctx *c = j->ctx;

#define ACC(p, name, get, set) if (js_define_accessor(c, p, name, get, set, 1) != JS_OK) return 0
#define FUN(p, name, fn, n) if (js_define_native(c, p, name, fn, n) != JS_OK) return 0
    FUN(j->doc_proto, "getElementById", doc_by_id, 1);
    FUN(j->doc_proto, "querySelector", doc_query, 1);
    FUN(j->doc_proto, "querySelectorAll", doc_query_all, 1);
    FUN(j->doc_proto, "getElementsByTagName", doc_query_all, 1);
    FUN(j->doc_proto, "createElement", doc_create_element, 1);
    FUN(j->doc_proto, "createTextNode", doc_create_text, 1);
    FUN(j->doc_proto, "addEventListener", node_add_listener, 2);
    ACC(j->doc_proto, "documentElement", doc_html_get, 0);
    ACC(j->doc_proto, "head", doc_head_get, 0);
    ACC(j->doc_proto, "body", doc_body_get, 0);
    ACC(j->doc_proto, "title", doc_title_get, doc_title_set);
    ACC(j->doc_proto, "URL", doc_url_get, 0);
    ACC(j->doc_proto, "readyState", doc_ready_get, 0);
    ACC(j->doc_proto, "cookie", doc_cookie_get, doc_cookie_set);

    ACC(j->location_proto, "href", location_href_get, location_href_set);
    FUN(j->location_proto, "assign", location_assign, 1);
    FUN(j->location_proto, "replace", location_assign, 1);

    FUN(j->class_proto, "add", class_add, 1);
    FUN(j->class_proto, "remove", class_remove, 1);
    FUN(j->class_proto, "toggle", class_toggle, 1);
    FUN(j->class_proto, "contains", class_contains, 1);

    ACC(j->style_proto, "cssText", style_css_get, style_css_set);
    ACC(j->style_proto, "display", style_display_get, style_display_set);
    ACC(j->style_proto, "color", style_color_get, style_color_set);
    ACC(j->style_proto, "backgroundColor", style_bg_get, style_bg_set);
    ACC(j->style_proto, "width", style_width_get, style_width_set);
    ACC(j->style_proto, "height", style_height_get, style_height_set);
    ACC(j->style_proto, "margin", style_margin_get, style_margin_set);
    ACC(j->style_proto, "padding", style_padding_get, style_padding_set);
    ACC(j->style_proto, "fontSize", style_font_size_get, style_font_size_set);
    ACC(j->style_proto, "fontWeight", style_font_weight_get, style_font_weight_set);
    ACC(j->style_proto, "textAlign", style_text_align_get, style_text_align_set);
    ACC(j->style_proto, "visibility", style_visibility_get, style_visibility_set);

    ACC(j->response_proto, "status", response_status_get, 0);
    ACC(j->response_proto, "statusText", response_status_text_get, 0);
    ACC(j->response_proto, "ok", response_ok_get, 0);
    ACC(j->response_proto, "url", response_url_get, 0);
    ACC(j->response_proto, "headers", response_headers_get, 0);
    ACC(j->response_proto, "bodyUsed", response_body_used_get, 0);
    FUN(j->response_proto, "text", response_text, 0);
    FUN(j->response_proto, "json", response_json, 0);
    FUN(j->response_proto, "arrayBuffer", response_arraybuffer, 0);
    FUN(j->headers_proto, "get", headers_get, 1);
#undef FUN
#undef ACC
    return 1;
}

struct jsdom *jsdom_new(struct dom_document *doc,
                        const struct jsdom_config *cfg)
{
    struct jsdom *j;
    js_config jc;
    js_object *docobj, *locobj, *navobj, *screenobj, *perfobj;
    js_value global;

    if (!doc)
        return 0;
    j = (struct jsdom *)calloc(1, sizeof(*j));
    if (!j)
        return 0;
    j->doc = doc;
    j->url = dup_z(cfg && cfg->url ? cfg->url : "about:blank");
    j->base_url = dup_z(cfg && cfg->base_url ? cfg->base_url :
                        (cfg && cfg->url ? cfg->url : "about:blank"));
    j->print = cfg ? cfg->print : 0;
    j->cookie_get = cfg ? cfg->cookie_get : 0;
    j->cookie_set = cfg ? cfg->cookie_set : 0;
    j->fetch = cfg ? cfg->fetch : 0;
    j->print_user = cfg ? cfg->user : 0;
    if (!j->url || !j->base_url) {
        free(j->base_url);
        free(j->url);
        free(j);
        return 0;
    }

    js_config_default(&jc);
    jc.user = j;
    jc.print = js_print;
    if (cfg && cfg->max_heap) jc.max_heap = cfg->max_heap;
    if (cfg && cfg->max_steps) jc.max_steps = cfg->max_steps;
    j->ctx = js_new(&jc);
    if (!j->ctx) {
        free(j->base_url);
        free(j->url);
        free(j);
        return 0;
    }
    if (js_host_finalizer(j->ctx, JSDOM_TAG, host_free) != 0)
        goto fail;
    j->node_proto = js_new_object(j->ctx);
    j->doc_proto = js_new_object(j->ctx);
    j->location_proto = js_new_object(j->ctx);
    j->class_proto = js_new_object(j->ctx);
    j->style_proto = js_new_object(j->ctx);
    j->response_proto = js_new_object(j->ctx);
    j->headers_proto = js_new_object(j->ctx);
    if (!j->node_proto || !j->doc_proto || !j->location_proto ||
        !j->class_proto || !j->style_proto || !j->response_proto ||
        !j->headers_proto ||
        !define_node_proto(j) || !define_other_protos(j))
        goto fail;

    docobj = new_host(j, 0, HOST_DOCUMENT, j->doc_proto);
    locobj = new_host(j, 0, HOST_LOCATION, j->location_proto);
    navobj = js_new_object(j->ctx);
    screenobj = js_new_object(j->ctx);
    perfobj = js_new_object(j->ctx);
    if (!docobj || !locobj || !navobj || !screenobj || !perfobj)
        goto fail;
    if (js_set(j->ctx, js_object_value(navobj), "userAgent",
               js_mkcstring(j->ctx,
                            "Mozilla/5.0 (X11; KestrelOS) "
                            "KestrelBrowser/1.0")) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "platform",
               js_mkcstring(j->ctx, "KestrelOS x86_64")) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "language",
               js_mkcstring(j->ctx, "en-US")) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "cookieEnabled",
               js_bool(1)) != JS_OK ||
        js_set(j->ctx, js_object_value(screenobj), "width",
               js_number(900)) != JS_OK ||
        js_set(j->ctx, js_object_value(screenobj), "height",
               js_number(620)) != JS_OK ||
        js_define_native(j->ctx, perfobj, "now",
                         global_performance_now, 0) != JS_OK)
        goto fail;
    global = js_global(j->ctx);
    if (js_define(j->ctx, global.u.obj, "__kestrelDocument",
                  js_object_value(docobj)) != JS_OK ||
        js_set(j->ctx, global, "document", js_object_value(docobj)) != JS_OK ||
        js_set(j->ctx, global, "location", js_object_value(locobj)) != JS_OK ||
        js_set(j->ctx, js_object_value(docobj), "location",
               js_object_value(locobj)) != JS_OK ||
        js_set(j->ctx, global, "window", global) != JS_OK ||
        js_set(j->ctx, global, "self", global) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "alert", global_alert, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "addEventListener",
                         global_add_listener, 2) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "setTimeout",
                         global_set_timeout, 2) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "setInterval",
                         global_set_timeout, 2) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "clearTimeout",
                         global_clear_timeout, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "clearInterval",
                         global_clear_timeout, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "requestAnimationFrame",
                         global_animation_frame, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "getComputedStyle",
                         global_computed_style, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "atob",
                         global_atob, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "btoa",
                         global_btoa, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "fetch",
                         global_fetch, 2) != JS_OK ||
        js_set(j->ctx, global, "navigator", js_object_value(navobj)) != JS_OK ||
        js_set(j->ctx, global, "screen", js_object_value(screenobj)) != JS_OK ||
        js_set(j->ctx, global, "performance", js_object_value(perfobj)) != JS_OK)
        goto fail;
    return j;

fail:
    jsdom_free(j);
    return 0;
}

void jsdom_free(struct jsdom *j)
{
    struct dom_node *n;
    int i;
    if (!j) return;
    if (j->doc && j->doc->root)
        for (n = j->doc->root; n; n = dom_next(n))
            n->user = 0;
    js_free(j->ctx);
    for (i = 0; i < JSDOM_FETCH_MAX; i++) {
        free(j->fetches[i].url);
        free(j->fetches[i].method);
        free(j->fetches[i].body);
    }
    free(j->navigation);
    free(j->base_url);
    free(j->url);
    free(j);
}

int jsdom_eval(struct jsdom *j, const char *source, const char *name,
               char *err, unsigned long errsz)
{
    js_value r;
    int rc;
    const char *msg;

    if (!j || !source)
        return -1;
    js_reset_steps(j->ctx);
    rc = js_eval(j->ctx, source, name ? name : "script", &r);
    if (rc == JS_OK)
        return 0;
    msg = js_error_text(j->ctx, r);
    if (err && errsz)
        snprintf(err, errsz, "%s", msg ? msg : "JavaScript exception");
    return -1;
}

static int dispatch_listeners(struct jsdom *j, js_value tv, js_value event,
                              const char *type, char *err,
                              unsigned long errsz)
{
    js_value listeners, fn, result;
    char key[96];
    unsigned long i, count;

    snprintf(key, sizeof(key), "__listeners_%s", type);
    if (js_get(j->ctx, tv, key, &listeners) != JS_OK ||
        !js_is_array(listeners))
        return 1;
    count = js_array_length(listeners.u.obj);
    for (i = 0; i < count; i++) {
        if (js_array_get(j->ctx, listeners.u.obj, i, &fn) == JS_OK &&
            js_is_function(fn)) {
            js_reset_steps(j->ctx);
            if (js_call(j->ctx, fn, tv, 1, &event, &result) != JS_OK) {
                const char *msg = js_error_text(j->ctx,
                                                js_exception(j->ctx));
                if (err && errsz) snprintf(err, errsz, "%s", msg);
                return 0;
            }
            if (js_is_bool(result) && !result.u.b)
                js_set(j->ctx, event, "defaultPrevented", js_bool(1));
        }
    }
    return 1;
}

int jsdom_dispatch(struct jsdom *j, struct dom_node *target,
                   const char *type, char *err, unsigned long errsz)
{
    js_value tv, event, handler, result;
    js_object *eo;
    char attr[80];
    int allowed = 1;

    if (!j || !target || !type || strlen(type) > 64)
        return 1;
    tv = wrap_node(j, target);
    if (!js_is_object(tv))
        return 1;
    eo = js_new_object(j->ctx);
    if (!eo)
        return 1;
    event = js_object_value(eo);
    js_set(j->ctx, event, "type", js_mkcstring(j->ctx, type));
    js_set(j->ctx, event, "target", tv);
    js_set(j->ctx, event, "currentTarget", tv);
    js_set(j->ctx, event, "defaultPrevented", js_bool(0));
    js_define_native(j->ctx, eo, "preventDefault", event_prevent, 0);
    js_define_native(j->ctx, eo, "stopPropagation", event_prevent, 0);

    dispatch_listeners(j, tv, event, type, err, errsz);
    snprintf(attr, sizeof(attr), "on%s", type);
    if (js_get(j->ctx, tv, attr, &handler) == JS_OK &&
        js_is_function(handler)) {
        js_reset_steps(j->ctx);
        if (js_call(j->ctx, handler, tv, 1, &event, &result) == JS_OK &&
            js_is_bool(result) && !result.u.b)
            allowed = 0;
    } else {
        const char *inline_code = target->type == DOM_ELEMENT
            ? dom_get_attr(target, attr) : 0;
        if (inline_code && *inline_code) {
            unsigned long n = strlen(inline_code) + 80;
            char *src = (char *)malloc(n);
            if (src) {
                js_set(j->ctx, js_global(j->ctx), "__kestrelTarget", tv);
                js_set(j->ctx, js_global(j->ctx), "event", event);
                snprintf(src, n, "(function(event){%s}).call(__kestrelTarget,event)",
                         inline_code);
                js_reset_steps(j->ctx);
                if (js_eval(j->ctx, src, "inline event handler", &result) ==
                    JS_OK && js_is_bool(result) && !result.u.b)
                    allowed = 0;
                else if (js_exception(j->ctx).type != JS_UNDEFINED &&
                         err && errsz)
                    snprintf(err, errsz, "%s",
                             js_error_text(j->ctx, js_exception(j->ctx)));
                free(src);
            }
        }
    }
    if (js_get(j->ctx, event, "defaultPrevented", &result) == JS_OK &&
        js_to_boolean(result))
        allowed = 0;
    return allowed;
}

int jsdom_dispatch_document(struct jsdom *j, const char *type,
                            char *err, unsigned long errsz)
{
    js_value doc, event, result;
    js_object *eo;

    if (!j || !type)
        return 1;
    if (js_get(j->ctx, js_global(j->ctx), "__kestrelDocument", &doc) !=
        JS_OK || !js_is_object(doc))
        return 1;
    eo = js_new_object(j->ctx);
    if (!eo)
        return 1;
    event = js_object_value(eo);
    js_set(j->ctx, event, "type", js_mkcstring(j->ctx, type));
    js_set(j->ctx, event, "target", doc);
    js_set(j->ctx, event, "currentTarget", doc);
    js_set(j->ctx, event, "defaultPrevented", js_bool(0));
    js_define_native(j->ctx, eo, "preventDefault", event_prevent, 0);
    dispatch_listeners(j, doc, event, type, err, errsz);
    dispatch_listeners(j, js_global(j->ctx), event, type, err, errsz);
    if (js_get(j->ctx, event, "defaultPrevented", &result) == JS_OK)
        return !js_to_boolean(result);
    return 1;
}

static int pump_fetch(struct jsdom *j, int i)
{
    struct jsdom_fetch_response f;
    char message[192];
    js_value value;
    int rc;

    memset(&f, 0, sizeof(f));
    message[0] = 0;
    if (!j->fetch) {
        rc = -1;
        snprintf(message, sizeof(message), "fetch is unavailable");
    } else {
        rc = j->fetch(j->print_user, j->fetches[i].url,
                      j->fetches[i].method,
                      j->fetches[i].body, j->fetches[i].body_len,
                      &f, message, sizeof(message));
    }
    if (rc == 0) {
        value = make_response(j, &f);
        if (js_is_undefined(value)) {
            fetch_response_clear(&f);
            value = js_mkcstring(j->ctx, "out of memory creating Response");
            rc = js_promise_reject(j->ctx, j->fetches[i].promise, value);
        } else {
            rc = js_promise_resolve(j->ctx, j->fetches[i].promise, value);
        }
    } else {
        fetch_response_clear(&f);
        value = js_mkcstring(j->ctx,
                             message[0] ? message : "network request failed");
        rc = js_promise_reject(j->ctx, j->fetches[i].promise, value);
    }
    free(j->fetches[i].url);
    free(j->fetches[i].method);
    free(j->fetches[i].body);
    memset(&j->fetches[i], 0, sizeof(j->fetches[i]));
    return rc == JS_OK ? 1 : -1;
}

int jsdom_pump(struct jsdom *j, char *err, unsigned long errsz)
{
    int i, ran = 0, jobs;

    if (!j)
        return 0;
    jobs = js_run_jobs(j->ctx, 256);
    if (jobs < 0)
        goto fail;
    ran += jobs;
    for (i = 0; i < (int)(sizeof(j->timers) / sizeof(j->timers[0])); i++) {
        js_value fn, result;

        if (!j->timers[i].active)
            continue;
        fn = j->timers[i].fn;
        j->timers[i].active = 0;
        js_reset_steps(j->ctx);
        if (js_call(j->ctx, fn, js_global(j->ctx), 0, 0, &result) != JS_OK) {
            if (err && errsz)
                snprintf(err, errsz, "%s",
                         js_error_text(j->ctx, js_exception(j->ctx)));
            return -1;
        }
        ran++;
    }
    for (i = 0; i < JSDOM_FETCH_MAX; i++)
        if (j->fetches[i].active) {
            int fetched = pump_fetch(j, i);

            if (fetched < 0)
                goto fail;
            ran += fetched;
        }
    jobs = js_run_jobs(j->ctx, 256);
    if (jobs < 0)
        goto fail;
    ran += jobs;
    return ran;

fail:
    if (err && errsz)
        snprintf(err, errsz, "%s",
                 js_error_text(j->ctx, js_exception(j->ctx)));
    return -1;
}

int jsdom_dirty(const struct jsdom *j)
{
    return j ? j->dirty : 0;
}

void jsdom_clear_dirty(struct jsdom *j)
{
    if (j) j->dirty = 0;
}

const char *jsdom_pending_navigation(const struct jsdom *j)
{
    return j ? j->navigation : 0;
}

void jsdom_clear_navigation(struct jsdom *j)
{
    if (!j) return;
    free(j->navigation);
    j->navigation = 0;
}
