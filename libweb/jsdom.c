/* jsdom.c - DOM/window bindings for Kestrel's small JavaScript engine. */

#if defined(JS_HOST) && defined(_WIN32)
#define _CRT_RAND_S
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef JS_HOST
#ifndef _WIN32
#include <sys/random.h>
#endif
#else
#include <kestrel.h>
#endif
#include "jsdom.h"
#include "css.h"
#include "storage.h"
#include "url.h"

#define JSDOM_TAG 0x4a444f4dU
#define JSDOM_CLASS_MAX 4096
#define JSDOM_STYLE_MAX 8192
#define JSDOM_FETCH_MAX 32
#define JSDOM_OBJECT_URL_MAX 32
#define JSDOM_EVENT_LISTENER_MAX 64
#define JSDOM_HEADERS_MAX 64
#define JSDOM_HEADER_NAME_MAX 63
#define JSDOM_HEADER_VALUE_MAX 4096
#define JSDOM_RESPONSE_BODY_MAX (4UL * 1024UL * 1024UL)
#define JSDOM_REQUEST_HEADERS_MAX 8192
#define JSDOM_HISTORY_MAX 64
#define JSDOM_TIMER_ARGS_MAX 16
#define JSDOM_MUTATION_OBSERVERS_MAX 16
#define JSDOM_MUTATION_RECORDS_MAX 64
#define JSDOM_MUTATION_FILTER_MAX 16
#define JSDOM_MUTATION_ATTRIBUTE_MAX 128
#define JSDOM_MUTATION_TARGETS_MAX 16
#define JSDOM_CANVAS_MAX_DIM 2048
#define JSDOM_CANVAS_MAX_PIXELS (4UL * 1024UL * 1024UL)
#define JSDOM_CANVAS_STACK_MAX 16
#define JSDOM_CANVAS_PATH_MAX 16384
#define JSDOM_OBSERVERS_MAX 16
#define JSDOM_OBSERVER_TARGETS_MAX 32
#define JSDOM_CLIPBOARD_MAX (1024UL * 1024UL)

/* Explicit task states make cancellation and re-entrancy observable instead
 * of overloading one "active" bit.  The browser remains single-threaded:
 * callbacks only advance at jsdom_pump() event-loop checkpoints. */
enum task_state {
    TASK_FREE = 0,
    TASK_QUEUED,
    TASK_RUNNING,
    TASK_SETTLING
};

enum host_kind {
    HOST_NODE = 1,
    HOST_DOCUMENT,
    HOST_LOCATION,
    HOST_CLASSLIST,
    HOST_STYLE,
    HOST_RESPONSE,
    HOST_HEADERS,
    HOST_STORAGE,
    HOST_URL,
    HOST_EVENT_TARGET,
    HOST_REQUEST,
    HOST_FILE,
    HOST_FILE_READER,
    HOST_WEB_ITERATOR,
    HOST_MEDIA_QUERY,
    HOST_HISTORY,
    HOST_DATASET_PROPERTY,
    HOST_MUTATION_OBSERVER,
    HOST_ATTR,
    HOST_NAMED_NODE_MAP,
    HOST_NODE_ITERATOR,
    HOST_TREE_WALKER,
    HOST_RANGE,
    HOST_SELECTION,
    HOST_COMPUTED_STYLE,
    HOST_CANVAS_2D,
    HOST_IMAGE_DATA,
    HOST_CANVAS_GRADIENT,
    HOST_DOM_PARSER,
    HOST_RESIZE_OBSERVER,
    HOST_INTERSECTION_OBSERVER
};

enum response_kind {
    RESPONSE_DEFAULT = 0,
    RESPONSE_BASIC,
    RESPONSE_ERROR
};

struct response_state {
    int refs;
    int status;
    char *status_text;
    char *url;
    struct headers_state *headers;
    char *body;
    unsigned long body_len;
    int body_used;
    int kind;
    int redirected;
};

struct header_entry {
    char *name;
    char *value;
};

struct headers_state {
    int refs;
    unsigned int count;
    struct header_entry entries[JSDOM_HEADERS_MAX];
};

struct request_state {
    char *url;
    char *method;
    char *body;
    unsigned long body_len;
    struct headers_state *headers;
    js_value signal;
    int body_used;
    char cache[20];
    char credentials[16];
    char destination[16];
    char integrity[513];
    char mode[16];
    char redirect[12];
    char referrer[URL_MAX];
    char referrer_policy[32];
    int keepalive;
};

struct file_state {
    char *data;
    unsigned long length;
    char type[128];
    char *name;
    double last_modified;
};

struct file_reader_state {
    int ready_state;
    int aborted;
    unsigned int generation;
    unsigned long total;
    js_value result;
    js_value error;
    js_value pending_result;
};

enum web_iterator_kind {
    ITER_HEADERS_ENTRIES = 1,
    ITER_HEADERS_KEYS,
    ITER_HEADERS_VALUES
};

struct web_iterator_state {
    int kind;
    unsigned int index;
    js_value collection;
};

struct history_entry {
    char *url;
    js_value state;
};

struct history_state {
    unsigned int count;
    unsigned int index;
    char scroll_restoration[7];
    struct history_entry entries[JSDOM_HISTORY_MAX];
};

struct url_state {
    struct url parsed;
    js_value search_params;
    char params_snapshot[URL_QUERY_MAX];
};

struct mutation_registration {
    struct dom_node *target;
    char attribute_filter[JSDOM_MUTATION_FILTER_MAX]
                         [JSDOM_MUTATION_ATTRIBUTE_MAX + 1];
    unsigned int attribute_filter_count;
    unsigned char child_list;
    unsigned char attributes;
    unsigned char character_data;
    unsigned char subtree;
    unsigned char attribute_old_value;
    unsigned char character_data_old_value;
    unsigned char has_attribute_filter;
};

struct mutation_observer_state {
    js_value callback;
    js_value self;
    js_value records[JSDOM_MUTATION_RECORDS_MAX];
    struct mutation_registration registrations[JSDOM_MUTATION_TARGETS_MAX];
    unsigned int record_count;
    unsigned int registration_count;
};

struct traversal_state {
    struct dom_node *root;
    struct dom_node *current;
    js_value filter;
    unsigned long what_to_show;
    unsigned char before_reference;
};

struct range_state {
    struct dom_node *start_container;
    struct dom_node *end_container;
    unsigned long start_offset;
    unsigned long end_offset;
};

struct selection_state {
    js_value range;
    unsigned char has_range;
};

struct attr_ref {
    char *namespace_uri;
    char *local_name;
};

struct doc_ref {
    struct dom_document *doc;
    char content_type[40];
};

struct canvas_pt {
    double x;
    double y;
    unsigned char kind; /* 0 = move, 1 = line, 2 = close */
};

#define JSDOM_GRADIENT_LINEAR 1
#define JSDOM_GRADIENT_RADIAL 2

struct canvas_grad_stop {
    double offset;
    uint32_t color;
};

struct canvas_gradient {
    int refs;
    int kind;
    double x0, y0, r0;
    double x1, y1, r1;
    struct canvas_grad_stop *stops;
    unsigned int stop_count;
    unsigned int stop_cap;
};

struct canvas_2d_saved {
    double m[6]; /* a, b, c, d, e, f */
    uint32_t fill_color;
    uint32_t stroke_color;
    char fill_str[64];
    char stroke_str[64];
    struct canvas_gradient *fill_grad;
    struct canvas_gradient *stroke_grad;
    js_value fill_grad_val;
    js_value stroke_grad_val;
    double line_width;
    double global_alpha;
    char font[128];
    double font_px;
    unsigned char text_align;    /* 0 left/start, 1 center, 2 right/end */
    unsigned char text_baseline; /* 0 alphabetic, 1 top, 2 middle, 3 bottom */
};

struct canvas_2d_state {
    struct canvas_2d_saved cur;
    struct canvas_2d_saved stack[JSDOM_CANVAS_STACK_MAX];
    int sp;
    struct canvas_pt *path;
    unsigned int path_n;
    unsigned int path_cap;
    js_value canvas_obj;
};

struct observer_target {
    struct dom_node *node;
    float last_w;
    float last_h;
    unsigned char pending;
};

struct resize_observer_state {
    js_value callback;
    js_value self;
    struct observer_target targets[JSDOM_OBSERVER_TARGETS_MAX];
    unsigned int target_count;
};

struct intersection_observer_state {
    js_value callback;
    js_value self;
    struct dom_node *root;
    struct observer_target targets[JSDOM_OBSERVER_TARGETS_MAX];
    unsigned int target_count;
    js_value pending_records[JSDOM_MUTATION_RECORDS_MAX];
    unsigned int pending_count;
};

struct jsdom {
    struct dom_document *doc;
    js_ctx *ctx;
    js_object *node_proto;
    js_object *character_data_proto;
    js_object *text_proto;
    js_object *comment_proto;
    js_object *element_proto;
    js_object *html_element_proto;
    js_object *canvas_element_proto;
    js_object *template_element_proto;
    js_object *svg_element_proto;
    js_object *fragment_proto;
    js_object *doc_proto;
    js_object *html_document_proto;
    js_object *location_proto;
    js_object *class_proto;
    js_object *style_proto;
    js_object *computed_style_proto;
    js_object *canvas_2d_proto;
    js_object *image_data_proto;
    js_object *canvas_gradient_proto;
    js_object *dom_parser_proto;
    js_object *resize_observer_proto;
    js_object *intersection_observer_proto;
    js_object *response_proto;
    js_object *headers_proto;
    js_object *storage_proto;
    js_object *url_proto;
    js_object *event_target_proto;
    js_object *event_proto;
    js_object *custom_event_proto;
    js_object *ui_event_proto;
    js_object *mouse_event_proto;
    js_object *keyboard_event_proto;
    js_object *focus_event_proto;
    js_object *input_event_proto;
    js_object *pointer_event_proto;
    js_object *request_proto;
    js_object *file_proto;
    js_object *file_reader_proto;
    js_object *web_iterator_proto;
    js_object *media_query_proto;
    js_object *history_proto;
    js_object *mutation_observer_proto;
    js_object *mutation_record_proto;
    js_object *dom_exception_proto;
    js_object *attr_proto;
    js_object *named_node_map_proto;
    js_object *node_iterator_proto;
    js_object *tree_walker_proto;
    js_object *range_proto;
    js_object *selection_proto;
    js_value selection;
    char *url;
    char *base_url;
    char origin[URL_MAX];
    char *navigation;
    int dirty;
    void (*print)(void *user, const char *text);
    const char *(*cookie_get)(void *user);
    int (*cookie_set)(void *user, const char *value);
    int (*fetch)(void *user, const char *url, const char *method,
                 const void *body, unsigned long body_len,
                 const char *content_type, const char *accept,
                 const char *extra_headers,
                 const char *mode, const char *credentials,
                 const char *redirect,
                 struct jsdom_fetch_response *out,
                 char *err, unsigned long errsz);
    int (*get_box_rect)(void *user, struct dom_node *node,
                        float *x, float *y, float *w, float *h);
    void (*scroll_cb)(void *user, double x, double y);
    int (*clipboard_write)(void *user, const char *text, unsigned long len);
    char *(*clipboard_read)(void *user, unsigned long *len_out);
    void *print_user;
    struct web_storage *local_storage;
    struct web_storage *session_storage;
    int owns_local_storage;
    int owns_session_storage;
    struct timer_task {
        js_value fn;
        js_value args[JSDOM_TIMER_ARGS_MAX];
        unsigned long due_turn;
        unsigned int interval_turns;
        js_value abort_signal;
        unsigned char state;
        unsigned char repeats;
        unsigned char argc;
        unsigned char callback_kind; /* 0 timer, 1 animation, 2 idle */
    } timers[64];
    struct fetch_task {
        js_value promise;
        char *url;
        char *method;
        char *body;
        unsigned long body_len;
        char *content_type;
        char *accept;
        char *extra_headers;
        char mode[16];
        char credentials[16];
        char redirect[12];
        js_value abort_signal;
        unsigned char state;
    } fetches[JSDOM_FETCH_MAX];
    struct file_reader_task {
        js_value reader;
        unsigned int generation;
        unsigned char state;
    } file_readers[16];
    struct object_url_entry {
        char *url;
        char *data;
        unsigned long length;
        char type[128];
    } object_urls[JSDOM_OBJECT_URL_MAX];
    struct mutation_observer_state
        *mutation_observers[JSDOM_MUTATION_OBSERVERS_MAX];
    struct resize_observer_state
        *resize_observers[JSDOM_OBSERVERS_MAX];
    struct intersection_observer_state
        *intersection_observers[JSDOM_OBSERVERS_MAX];
    struct dom_node **dataset_nodes;
    unsigned int dataset_node_count;
    unsigned int dataset_node_cap;
    struct dom_document **parsed_docs;
    unsigned int parsed_doc_count;
    unsigned int parsed_doc_cap;
    struct dom_memory_credit parsed_doc_credit;
    struct css_stylesheet *author_sheet;
    const struct css_stylesheet *external_sheet;
    uint64_t initial_style_hash;
    int style_sheet_dirty;
    double scroll_x;
    double scroll_y;
    char *clipboard_text;
    unsigned long clipboard_len;
    unsigned long object_url_counter;
    unsigned long event_turn;
    unsigned int viewport_width;
    unsigned int viewport_height;
    int base_follows_url;
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

static void headers_release(struct headers_state *headers)
{
    unsigned int i;

    if (!headers || --headers->refs > 0)
        return;
    for (i = 0; i < headers->count; i++) {
        free(headers->entries[i].name);
        free(headers->entries[i].value);
    }
    free(headers);
}

static void response_release(struct response_state *s)
{
    if (!s || --s->refs > 0)
        return;
    free(s->status_text);
    free(s->url);
    headers_release(s->headers);
    free(s->body);
    free(s);
}

static void request_release(struct request_state *request)
{
    if (!request)
        return;
    free(request->url);
    free(request->method);
    free(request->body);
    headers_release(request->headers);
    free(request);
}

static void file_release(struct file_state *file)
{
    if (!file)
        return;
    free(file->data);
    free(file->name);
    free(file);
}

static void history_release(struct history_state *history)
{
    unsigned int i;

    if (!history)
        return;
    for (i = 0; i < history->count; i++)
        free(history->entries[i].url);
    free(history);
}

static struct canvas_gradient *canvas_grad_retain(struct canvas_gradient *g)
{
    if (g)
        g->refs++;
    return g;
}

static void canvas_grad_release(struct canvas_gradient *g)
{
    if (!g || --g->refs > 0)
        return;
    free(g->stops);
    free(g);
}

static void canvas_2d_saved_release(struct canvas_2d_saved *s)
{
    if (!s)
        return;
    canvas_grad_release(s->fill_grad);
    canvas_grad_release(s->stroke_grad);
    s->fill_grad = 0;
    s->stroke_grad = 0;
    s->fill_grad_val = js_undefined();
    s->stroke_grad_val = js_undefined();
}

static void canvas_2d_saved_copy(struct canvas_2d_saved *dst,
                                 const struct canvas_2d_saved *src)
{
    canvas_grad_retain(src->fill_grad);
    canvas_grad_retain(src->stroke_grad);
    canvas_2d_saved_release(dst);
    *dst = *src;
}

static void canvas_2d_release_grads(struct canvas_2d_state *st)
{
    int i;

    if (!st)
        return;
    canvas_2d_saved_release(&st->cur);
    for (i = 0; i < st->sp; i++)
        canvas_2d_saved_release(&st->stack[i]);
}

static void free_node_canvas(struct dom_node *n)
{
    if (!n || !n->canvas)
        return;
    free(n->canvas->px);
    free(n->canvas->alpha);
    free(n->canvas);
    n->canvas = 0;
}

static void free_subtree_canvases(struct dom_node *root)
{
    struct dom_node *n;

    if (!root)
        return;
    for (n = root; n; n = dom_next_within(n, root))
        free_node_canvas(n);
}

static void mark_dom_dirty(struct jsdom *j)
{
    if (!j)
        return;
    j->dirty = 1;
    j->style_sheet_dirty = 1;
}

static struct dom_document *doc_of(const struct hostref *h)
{
    if (!h)
        return 0;
    if (h->kind == HOST_DOCUMENT && h->data)
        return ((const struct doc_ref *)h->data)->doc;
    return h->j ? h->j->doc : 0;
}

static struct dom_node *template_content_node(struct jsdom *j,
                                              struct dom_node *tpl);

static void host_free(void *user, void *ptr)
{
    struct hostref *h = (struct hostref *)ptr;

    (void)user;
    if (h && h->kind == HOST_NODE) {
        if (h->node)
            free_node_canvas(h->node);
    }
    else if (h && h->kind == HOST_DOCUMENT) {
        free(h->data);
    }
    else if (h && h->kind == HOST_CANVAS_2D) {
        struct canvas_2d_state *st = (struct canvas_2d_state *)h->data;

        if (st) {
            canvas_2d_release_grads(st);
            free(st->path);
        }
        free(st);
    }
    else if (h && h->kind == HOST_CANVAS_GRADIENT)
        canvas_grad_release((struct canvas_gradient *)h->data);
    else if (h && h->kind == HOST_RESPONSE)
        response_release((struct response_state *)h->data);
    else if (h && h->kind == HOST_HEADERS)
        headers_release((struct headers_state *)h->data);
    else if (h && h->kind == HOST_URL)
        free(h->data);
    else if (h && h->kind == HOST_REQUEST)
        request_release((struct request_state *)h->data);
    else if (h && h->kind == HOST_FILE)
        file_release((struct file_state *)h->data);
    else if (h && h->kind == HOST_FILE_READER)
        free(h->data);
    else if (h && h->kind == HOST_WEB_ITERATOR)
        free(h->data);
    else if (h && h->kind == HOST_HISTORY)
        history_release((struct history_state *)h->data);
    else if (h && h->kind == HOST_DATASET_PROPERTY)
        free(h->data);
    else if (h && h->kind == HOST_ATTR) {
        struct attr_ref *attribute = (struct attr_ref *)h->data;

        if (attribute) {
            free(attribute->namespace_uri);
            free(attribute->local_name);
        }
        free(attribute);
    }
    else if (h && (h->kind == HOST_NODE_ITERATOR ||
                   h->kind == HOST_TREE_WALKER))
        free(h->data);
    else if (h && (h->kind == HOST_RANGE ||
                   h->kind == HOST_SELECTION))
        free(h->data);
    else if (h && h->kind == HOST_MUTATION_OBSERVER) {
        struct mutation_observer_state *state =
            (struct mutation_observer_state *)h->data;
        unsigned int i;

        if (h->j)
            for (i = 0; i < JSDOM_MUTATION_OBSERVERS_MAX; i++)
                if (h->j->mutation_observers[i] == state)
                    h->j->mutation_observers[i] = 0;
        free(state);
    }
    else if (h && h->kind == HOST_RESIZE_OBSERVER) {
        struct resize_observer_state *state =
            (struct resize_observer_state *)h->data;
        unsigned int i;

        if (h->j)
            for (i = 0; i < JSDOM_OBSERVERS_MAX; i++)
                if (h->j->resize_observers[i] == state)
                    h->j->resize_observers[i] = 0;
        free(state);
    }
    else if (h && h->kind == HOST_INTERSECTION_OBSERVER) {
        struct intersection_observer_state *state =
            (struct intersection_observer_state *)h->data;
        unsigned int i;

        if (h->j)
            for (i = 0; i < JSDOM_OBSERVERS_MAX; i++)
                if (h->j->intersection_observers[i] == state)
                    h->j->intersection_observers[i] = 0;
        free(state);
    }
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
    o = new_host(j, n, HOST_NODE,
                 n->type == DOM_ELEMENT
                 ? (n->namespace_id == DOM_NS_SVG
                    ? j->svg_element_proto :
                    n->namespace_id == DOM_NS_HTML
                    ? (n->tag_id == HTAG_CANVAS && j->canvas_element_proto
                       ? j->canvas_element_proto :
                       n->tag_id == HTAG_TEMPLATE && j->template_element_proto
                       ? j->template_element_proto :
                       j->html_element_proto)
                    : j->element_proto) :
                 n->type == DOM_FRAGMENT ? j->fragment_proto :
                 n->type == DOM_TEXT ? j->text_proto :
                 n->type == DOM_COMMENT ? j->comment_proto :
                 j->node_proto);
    if (!o)
        return js_undefined();
    n->user = o;
    if (n->type == DOM_ELEMENT && n->tag_id == HTAG_TEMPLATE &&
        n->namespace_id == DOM_NS_HTML)
        (void)template_content_node(j, n);
    return js_object_value(o);
}

enum mutation_kind {
    MUTATION_ATTRIBUTES = 0,
    MUTATION_CHILD_LIST,
    MUTATION_CHARACTER_DATA
};

static const struct mutation_registration *mutation_registration_for(
    const struct mutation_observer_state *state,
    const struct dom_node *target, enum mutation_kind kind,
    const char *attribute)
{
    unsigned int pass, index;

    if (!state || !target)
        return 0;
    for (pass = 0; pass < 2; pass++) {
        for (index = 0; index < state->registration_count; index++) {
            const struct mutation_registration *registration =
                &state->registrations[index];
            const struct dom_node *node;
            int matches = registration->target == target;
            int wanted;

            if (!matches && pass == 1 && registration->subtree)
                for (node = target->parent; node; node = node->parent)
                    if (node == registration->target) {
                        matches = 1;
                        break;
                    }
            if (!matches || (pass == 0) !=
                            (registration->target == target))
                continue;
            wanted = kind == MUTATION_ATTRIBUTES
                ? registration->attributes :
                kind == MUTATION_CHILD_LIST
                ? registration->child_list :
                registration->character_data;
            if (!wanted)
                continue;
            if (kind == MUTATION_ATTRIBUTES &&
                registration->has_attribute_filter) {
                unsigned int filter_index;

                for (filter_index = 0;
                     filter_index < registration->attribute_filter_count;
                     filter_index++)
                    if (!strcmp(
                            registration->attribute_filter[filter_index],
                            attribute ? attribute : ""))
                        break;
                if (filter_index ==
                    registration->attribute_filter_count)
                    continue;
            }
            return registration;
        }
    }
    return 0;
}

static int mutation_queue(struct jsdom *j, enum mutation_kind kind,
                          struct dom_node *target, const char *attribute,
                          const char *attribute_namespace,
                          const char *old_value,
                          struct dom_node *added, struct dom_node *removed,
                          struct dom_node *previous, struct dom_node *next)
{
    unsigned int i;

    for (i = 0; i < JSDOM_MUTATION_OBSERVERS_MAX; i++) {
        struct mutation_observer_state *state = j->mutation_observers[i];
        const struct mutation_registration *registration;
        js_object *record, *added_nodes, *removed_nodes;
        js_value record_value, target_value, old_value_js = js_null();

        registration = mutation_registration_for(state, target,
                                                 kind, attribute);
        if (!registration ||
            state->record_count >= JSDOM_MUTATION_RECORDS_MAX)
            continue;
        if (old_value &&
            ((kind == MUTATION_ATTRIBUTES &&
              registration->attribute_old_value) ||
             (kind == MUTATION_CHARACTER_DATA &&
              registration->character_data_old_value)))
            old_value_js = js_mkcstring(j->ctx, old_value);
        record = js_new_object_proto(j->ctx, j->mutation_record_proto);
        added_nodes = js_new_array(j->ctx);
        removed_nodes = js_new_array(j->ctx);
        if (!record || !added_nodes || !removed_nodes)
            return JS_THROW;
        if (added &&
            js_array_push(j->ctx, added_nodes, wrap_node(j, added)) != JS_OK)
            return JS_THROW;
        if (removed &&
            js_array_push(j->ctx, removed_nodes, wrap_node(j, removed)) !=
                JS_OK)
            return JS_THROW;
        target_value = target == j->doc->root
            ? js_undefined() : wrap_node(j, target);
        if (target == j->doc->root &&
            js_get(j->ctx, js_global(j->ctx), "document",
                   &target_value) != JS_OK)
            return JS_THROW;
        record_value = js_object_value(record);
        if (js_set(j->ctx, record_value, "type",
                   js_mkcstring(j->ctx,
                                kind == MUTATION_ATTRIBUTES ? "attributes" :
                                kind == MUTATION_CHILD_LIST ? "childList" :
                                "characterData")) != JS_OK ||
            js_set(j->ctx, record_value, "target", target_value) != JS_OK ||
            js_set(j->ctx, record_value, "addedNodes",
                   js_object_value(added_nodes)) != JS_OK ||
            js_set(j->ctx, record_value, "removedNodes",
                   js_object_value(removed_nodes)) != JS_OK ||
            js_set(j->ctx, record_value, "previousSibling",
                   previous ? wrap_node(j, previous) : js_null()) != JS_OK ||
            js_set(j->ctx, record_value, "nextSibling",
                   next ? wrap_node(j, next) : js_null()) != JS_OK ||
            js_set(j->ctx, record_value, "attributeName",
                   attribute ? js_mkcstring(j->ctx, attribute) :
                   js_null()) != JS_OK ||
            js_set(j->ctx, record_value, "attributeNamespace",
                   attribute_namespace
                       ? js_mkcstring(j->ctx, attribute_namespace)
                       : js_null()) != JS_OK ||
            js_set(j->ctx, record_value, "oldValue",
                   old_value_js) != JS_OK)
            return JS_THROW;
        state->records[state->record_count++] = record_value;
    }
    return JS_OK;
}

static int mutation_attribute(struct jsdom *j, struct dom_node *target,
                              const char *name, const char *old_value)
{
    const char *canonical = target && target->doc
        ? dom_intern_name(target->doc, name, -1) : name;

    return mutation_queue(j, MUTATION_ATTRIBUTES, target,
                          canonical ? canonical : name, 0, old_value,
                          0, 0, 0, 0);
}

static int mutation_attribute_ns(struct jsdom *j, struct dom_node *target,
                                 const char *namespace_uri,
                                 const char *local_name,
                                 const char *old_value)
{
    return mutation_queue(j, MUTATION_ATTRIBUTES, target, local_name,
                          namespace_uri, old_value, 0, 0, 0, 0);
}

static int mutation_character_data(struct jsdom *j, struct dom_node *target,
                                   const char *old_value)
{
    return mutation_queue(j, MUTATION_CHARACTER_DATA, target, 0, 0, old_value,
                          0, 0, 0, 0);
}

static int mutation_child(struct jsdom *j, struct dom_node *target,
                          struct dom_node *added, struct dom_node *removed,
                          struct dom_node *previous, struct dom_node *next)
{
    return mutation_queue(j, MUTATION_CHILD_LIST, target, 0, 0, 0,
                          added, removed, previous, next);
}

#define JSDOM_MUTATION_NODES_MAX 128

struct mutation_node_snapshot {
    struct dom_node *node;
    struct dom_node *parent;
    struct dom_node *previous;
    struct dom_node *next;
};

static int mutation_snapshot_nodes(struct dom_node *node,
                                   struct mutation_node_snapshot *snapshot,
                                   unsigned int *count)
{
    struct dom_node *child;

    *count = 0;
    if (!node)
        return 0;
    if (node->type != DOM_FRAGMENT) {
        snapshot[0].node = node;
        snapshot[0].parent = node->parent;
        snapshot[0].previous = node->prev_sibling;
        snapshot[0].next = node->next_sibling;
        *count = 1;
        return 1;
    }
    for (child = node->first_child; child; child = child->next_sibling) {
        if (*count >= JSDOM_MUTATION_NODES_MAX)
            return 0;
        snapshot[*count].node = child;
        snapshot[*count].parent = child->parent;
        snapshot[*count].previous = child->prev_sibling;
        snapshot[*count].next = child->next_sibling;
        (*count)++;
    }
    return 1;
}

static int mutation_queue_moved_nodes(
    struct jsdom *j, struct dom_node *new_parent,
    const struct mutation_node_snapshot *snapshot, unsigned int count)
{
    unsigned int i;

    for (i = 0; i < count; i++) {
        if (snapshot[i].parent &&
            mutation_child(j, snapshot[i].parent, 0, snapshot[i].node,
                           snapshot[i].previous,
                           snapshot[i].next) != JS_OK)
            return JS_THROW;
        if (mutation_child(j, new_parent, snapshot[i].node, 0,
                           snapshot[i].node->prev_sibling,
                           snapshot[i].node->next_sibling) != JS_OK)
            return JS_THROW;
    }
    return JS_OK;
}

static void mutation_rebind_target(struct jsdom *j,
                                   struct dom_node *old_node,
                                   struct dom_node *new_node)
{
    unsigned int i, registration;

    for (i = 0; i < JSDOM_MUTATION_OBSERVERS_MAX; i++) {
        struct mutation_observer_state *state = j->mutation_observers[i];

        if (!state)
            continue;
        for (registration = 0;
             registration < state->registration_count;
             registration++)
            if (state->registrations[registration].target == old_node)
                state->registrations[registration].target = new_node;
    }
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
    struct dom_document *d;

    if (!n || (n->type != DOM_ELEMENT && n->type != DOM_DOCUMENT &&
               n->type != DOM_FRAGMENT))
        return 0;
    d = n->doc ? n->doc : j->doc;
    for (c = n->first_child; c; c = next) {
        struct dom_node *previous = c->prev_sibling;

        next = c->next_sibling;
        dom_remove_child(n, c);
        if (mutation_child(j, n, 0, c, previous, next) != JS_OK)
            return 0;
    }
    if (*s) {
        t = dom_create_text(d, s, strlen(s));
        if (!t || !dom_append_child(n, t))
            return 0;
        if (mutation_child(j, n, t, 0, t->prev_sibling, 0) != JS_OK)
            return 0;
    }
    mark_dom_dirty(j);
    return 1;
}

static struct dom_node *clone_subtree_doc(struct jsdom *j,
                                          struct dom_document *dst_doc,
                                          const struct dom_node *src,
                                          int depth)
{
    struct dom_node *out, *c;
    unsigned int i;

    if (!src || !dst_doc || depth > 64)
        return 0;
    if (src->type == DOM_TEXT)
        return dom_create_text(dst_doc, src->text, src->text_len);
    if (src->type == DOM_COMMENT)
        return dom_create_comment(dst_doc, src->text, src->text_len);
    if (src->type == DOM_FRAGMENT)
        out = dom_create_fragment(dst_doc);
    else if (src->type == DOM_ELEMENT)
        out = dom_create_element(dst_doc, src->tag, (long)strlen(src->tag));
    else
        return 0;
    if (!out)
        return 0;
    if (src->type == DOM_ELEMENT &&
        !dom_set_namespace(out, src->namespace_uri, -1))
        return 0;
    for (i = 0; src->type == DOM_ELEMENT &&
                i < dom_attr_count(src); i++) {
        const struct dom_attr *a = dom_attr_at(src, i);

        if (!a ||
            (a->namespace_uri
                ? !dom_set_attr_ns(out, a->namespace_uri,
                                   a->name, a->value)
                : !dom_set_attr_n(out, a->name, -1,
                                  a->value, a->len)))
            return 0;
    }
    for (c = src->first_child; c; c = c->next_sibling) {
        struct dom_node *copy = clone_subtree_doc(j, dst_doc, c, depth + 1);

        if (!copy || !dom_append_child(out, copy))
            return 0;
    }
    if (src->type == DOM_ELEMENT && src->tag_id == HTAG_TEMPLATE &&
        src->namespace_id == DOM_NS_HTML && src->user && j) {
        js_value cv = js_undefined();

        if (js_get(j->ctx, js_object_value((js_object *)src->user),
                   "__templateContent", &cv) == JS_OK &&
            js_is_object(cv)) {
            struct hostref *ch = host(cv, HOST_NODE);

            if (ch && ch->node && ch->node->first_child) {
                struct dom_node *out_frag = template_content_node(j, out);

                if (out_frag) {
                    for (c = ch->node->first_child; c; c = c->next_sibling) {
                        struct dom_node *copy =
                            clone_subtree_doc(j, dst_doc, c, depth + 1);

                        if (!copy || !dom_append_child(out_frag, copy))
                            return 0;
                    }
                }
            }
        }
    }
    return out;
}

static struct dom_node *clone_subtree(struct jsdom *j,
                                      const struct dom_node *src, int depth)
{
    return clone_subtree_doc(j, j->doc, src, depth);
}

static struct dom_node *template_content_node(struct jsdom *j,
                                              struct dom_node *tpl)
{
    js_value tpl_val, content_val;
    struct hostref *ch;
    struct dom_node *frag, *c, *next;

    if (!j || !tpl || tpl->type != DOM_ELEMENT ||
        tpl->tag_id != HTAG_TEMPLATE)
        return 0;
    if (!tpl->user) {
        tpl_val = wrap_node(j, tpl);
        if (!js_is_object(tpl_val))
            return 0;
    } else {
        tpl_val = js_object_value((js_object *)tpl->user);
    }
    if (js_get(j->ctx, tpl_val, "__templateContent", &content_val) == JS_OK &&
        js_is_object(content_val)) {
        ch = host(content_val, HOST_NODE);
        if (ch && ch->node)
            return ch->node;
    }
    frag = dom_create_fragment(tpl->doc ? tpl->doc : j->doc);
    if (!frag)
        return 0;
    for (c = tpl->first_child; c; c = next) {
        next = c->next_sibling;
        dom_remove_child(tpl, c);
        if (!dom_append_child(frag, c))
            return 0;
    }
    content_val = wrap_node(j, frag);
    if (!js_is_object(content_val) ||
        js_define(j->ctx, (js_object *)tpl->user, "__templateContent",
                  content_val) != JS_OK)
        return 0;
    return frag;
}

static void template_adopt_subtree(struct jsdom *j, struct dom_node *root)
{
    struct dom_node *n;

    if (!j || !root)
        return;
    for (n = root->first_child; n; n = dom_next_within(n, root)) {
        if (n->type == DOM_ELEMENT && n->tag_id == HTAG_TEMPLATE &&
            n->namespace_id == DOM_NS_HTML) {
            struct dom_node *frag = template_content_node(j, n);

            if (frag)
                template_adopt_subtree(j, frag);
        }
    }
}

static int set_inner_html_ctx(struct jsdom *j, struct dom_node *target,
                              struct dom_node *ctx_node, const char *html)
{
    char prefix[80];
    char suffix[64];
    struct dom_document *tmp, *dst_doc;
    struct dom_node *container = 0, *c, *next;
    unsigned long n = strlen(html), plen, slen;
    char *wrapped;
    int depth_extract = 0;

    if (!target || !html)
        return 0;
    dst_doc = target->doc ? target->doc : j->doc;
    strcpy(prefix, "<!doctype html><html><body>");
    strcpy(suffix, "</body></html>");
    if (ctx_node && ctx_node->type == DOM_ELEMENT) {
        if (ctx_node->namespace_id == DOM_NS_SVG &&
            strcmp(ctx_node->tag, "foreignobject") != 0) {
            strcpy(prefix, "<!doctype html><html><body><svg>");
            strcpy(suffix, "</svg></body></html>");
            depth_extract = 1;
        } else if (ctx_node->tag_id == HTAG_TEMPLATE) {
            strcpy(prefix, "<!doctype html><html><body><template>");
            strcpy(suffix, "</template></body></html>");
            depth_extract = 1;
        } else if (ctx_node->tag_id == HTAG_TABLE) {
            strcpy(prefix, "<!doctype html><html><body><table>");
            strcpy(suffix, "</table></body></html>");
            depth_extract = 1;
        } else if (ctx_node->tag_id == HTAG_TBODY ||
                   ctx_node->tag_id == HTAG_THEAD ||
                   ctx_node->tag_id == HTAG_TFOOT) {
            snprintf(prefix, sizeof(prefix),
                     "<!doctype html><html><body><table><%s>",
                     ctx_node->tag);
            snprintf(suffix, sizeof(suffix),
                     "</%s></table></body></html>", ctx_node->tag);
            depth_extract = 2;
        } else if (ctx_node->tag_id == HTAG_TR) {
            strcpy(prefix, "<!doctype html><html><body><table><tbody><tr>");
            strcpy(suffix, "</tr></tbody></table></body></html>");
            depth_extract = 3;
        } else if (ctx_node->tag_id == HTAG_SELECT) {
            strcpy(prefix, "<!doctype html><html><body><select>");
            strcpy(suffix, "</select></body></html>");
            depth_extract = 1;
        } else if (ctx_node->tag_id == HTAG_HTML) {
            strcpy(prefix, "<!doctype html><html>");
            strcpy(suffix, "</html>");
            depth_extract = -1;
        }
    }
    plen = strlen(prefix);
    slen = strlen(suffix);
    if (n > DOM_MAX_INPUT - plen - slen - 1)
        return 0;
    wrapped = (char *)malloc(plen + n + slen + 1);
    if (!wrapped)
        return 0;
    memcpy(wrapped, prefix, plen);
    memcpy(wrapped + plen, html, n);
    memcpy(wrapped + plen + n, suffix, slen + 1);
    tmp = html_parse_document(wrapped, plen + n + slen);
    free(wrapped);
    if (!tmp || tmp->oom) {
        dom_document_free(tmp);
        return 0;
    }
    if (depth_extract < 0) {
        container = tmp->html;
    } else {
        container = tmp->body;
        while (container && depth_extract-- > 0)
            container = container->first_child;
    }
    for (c = target->first_child; c; c = next) {
        struct dom_node *previous = c->prev_sibling;

        next = c->next_sibling;
        dom_remove_child(target, c);
        if (mutation_child(j, target, 0, c, previous, next) != JS_OK) {
            dom_document_free(tmp);
            return 0;
        }
    }
    for (c = container ? container->first_child : 0; c; c = c->next_sibling) {
        struct dom_node *copy = clone_subtree_doc(j, dst_doc, c, 0);

        if (!copy || !dom_append_child(target, copy)) {
            dom_document_free(tmp);
            return 0;
        }
        if (mutation_child(j, target, copy, 0,
                           copy->prev_sibling, 0) != JS_OK) {
            dom_document_free(tmp);
            return 0;
        }
    }
    dom_document_free(tmp);
    template_adopt_subtree(j, target);
    mark_dom_dirty(j);
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
/* Selector engine backed by libweb/css.c */

static int throw_dom_exception(js_ctx *c, const char *name,
                               const char *message);
static void dataset_sync_all(struct jsdom *j);

static const char *jsdom_css_tag(void *e)
{
    const struct dom_node *n = (const struct dom_node *)e;
    return (n && n->tag) ? n->tag : "";
}

static const char *jsdom_css_attr(void *e, const char *name)
{
    return dom_get_attr((const struct dom_node *)e, name);
}

static void *jsdom_css_parent(void *e)
{
    return dom_parent_element((const struct dom_node *)e);
}

static void *jsdom_css_prev(void *e)
{
    struct dom_node *n = ((const struct dom_node *)e)->prev_sibling;

    while (n && n->type != DOM_ELEMENT)
        n = n->prev_sibling;
    return n;
}

static void *jsdom_css_next(void *e)
{
    return dom_next_element_sibling((const struct dom_node *)e);
}

static void *jsdom_css_first_child(void *e)
{
    return dom_first_element_child((const struct dom_node *)e);
}

static const char *jsdom_css_id(void *e)
{
    return dom_get_attr((const struct dom_node *)e, "id");
}

static int jsdom_css_has_class(void *e, const char *cls)
{
    return dom_has_class((const struct dom_node *)e, cls);
}

static int jsdom_css_is_empty(void *e)
{
    return ((const struct dom_node *)e)->first_child == 0;
}

static unsigned jsdom_css_state(void *e)
{
    const struct dom_node *n = (const struct dom_node *)e;
    unsigned st = 0;

    if (dom_has_attr(n, "checked") || dom_has_attr(n, "selected"))
        st |= CSS_STATE_CHECKED;
    if (dom_has_attr(n, "disabled"))
        st |= CSS_STATE_DISABLED;
    return st;
}

static const struct css_elem_ops jsdom_css_ops = {
    jsdom_css_tag, jsdom_css_attr, jsdom_css_parent, jsdom_css_prev,
    jsdom_css_next, jsdom_css_first_child, jsdom_css_id, jsdom_css_has_class,
    jsdom_css_state, jsdom_css_is_empty
};

static int css_ws_char(int ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f';
}

static int parse_dom_selector(js_ctx *c, const char *sel,
                              struct css_stylesheet **out_ss)
{
    static const char tail[] = "{display:block}";
    unsigned long i = 0, len;
    int quote = 0, in_comment = 0, bracket = 0, paren = 0;
    int saw_non_ws = 0, expected_sels = 1;
    char *wrapped;
    struct css_stylesheet *ss;

    *out_ss = 0;
    if (!sel)
        return throw_dom_exception(c, "SyntaxError", "invalid selector");
    len = strlen(sel);
    if (len == 0 || len > 65536UL)
        return throw_dom_exception(c, "SyntaxError", "invalid selector");
    while (i < len) {
        int ch = (unsigned char)sel[i];

        if (in_comment) {
            if (ch == '*' && i + 1 < len && sel[i + 1] == '/') {
                in_comment = 0;
                i += 2;
            } else {
                i++;
            }
            continue;
        }
        if (quote) {
            if (ch == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (ch == '\n' || ch == '\r' || ch == '\f')
                return throw_dom_exception(c, "SyntaxError",
                                           "unterminated selector string");
            if (ch == quote)
                quote = 0;
            i++;
            continue;
        }
        if (ch == '/' && i + 1 < len && sel[i + 1] == '*') {
            in_comment = 1;
            i += 2;
            continue;
        }
        if (ch == '\\') {
            if (i + 1 >= len)
                return throw_dom_exception(c, "SyntaxError",
                                           "trailing escape in selector");
            saw_non_ws = 1;
            i += 2;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            saw_non_ws = 1;
            i++;
            continue;
        }
        if (ch == '{' || ch == '}' || ch == ';' || ch == '@')
            return throw_dom_exception(c, "SyntaxError", "invalid selector");
        if (ch == '[')
            bracket++;
        else if (ch == ']') {
            if (--bracket < 0)
                return throw_dom_exception(c, "SyntaxError",
                                           "unbalanced selector brackets");
        } else if (ch == '(')
            paren++;
        else if (ch == ')') {
            if (--paren < 0)
                return throw_dom_exception(c, "SyntaxError",
                                           "unbalanced selector parentheses");
        } else if (ch == ',' && bracket == 0 && paren == 0) {
            if (!saw_non_ws)
                return throw_dom_exception(c, "SyntaxError",
                                           "empty selector in list");
            expected_sels++;
            saw_non_ws = 0;
            i++;
            continue;
        }
        if (!css_ws_char(ch))
            saw_non_ws = 1;
        i++;
    }
    if (quote || in_comment || bracket != 0 || paren != 0 || !saw_non_ws)
        return throw_dom_exception(c, "SyntaxError", "invalid selector");
    wrapped = (char *)malloc(len + sizeof(tail));
    if (!wrapped)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    memcpy(wrapped, sel, len);
    memcpy(wrapped + len, tail, sizeof(tail));
    ss = css_parse(wrapped, len + sizeof(tail) - 1, CSS_ORIGIN_AUTHOR, 0);
    free(wrapped);
    if (!ss || css_truncated(ss) || css_rule_count(ss) != expected_sels) {
        css_free(ss);
        return throw_dom_exception(c, "SyntaxError", "invalid selector");
    }
    *out_ss = ss;
    return JS_OK;
}

static int selector_matches_ss(const struct css_stylesheet *ss,
                               struct dom_node *n)
{
    int i, count;

    if (!ss || !n || n->type != DOM_ELEMENT)
        return 0;
    count = css_rule_count(ss);
    for (i = 0; i < count; i++) {
        const struct css_rule *r = css_rule_at(ss, i);

        if (r && css_selector_matches(&r->sel, n, &jsdom_css_ops))
            return 1;
    }
    return 0;
}

static int query_first(js_ctx *c, struct jsdom *j, struct dom_node *root,
                       const char *sel, struct dom_node **out)
{
    struct css_stylesheet *ss;
    struct dom_node *n;

    *out = 0;
    if (parse_dom_selector(c, sel, &ss) != JS_OK)
        return JS_THROW;
    dataset_sync_all(j);
    n = root ? root->first_child : 0;
    while (n) {
        if (n->type == DOM_ELEMENT && selector_matches_ss(ss, n)) {
            *out = n;
            break;
        }
        n = dom_next_within(n, root);
    }
    css_free(ss);
    return JS_OK;
}

static int query_all(js_ctx *c, struct jsdom *j, struct dom_node *root,
                     const char *sel, js_value *ret)
{
    struct css_stylesheet *ss;
    js_object *a;
    struct dom_node *n;

    if (parse_dom_selector(c, sel, &ss) != JS_OK)
        return JS_THROW;
    a = js_new_array(c);
    if (!a) {
        css_free(ss);
        return JS_THROW;
    }
    dataset_sync_all(j);
    for (n = root ? root->first_child : 0; n; n = dom_next_within(n, root)) {
        if (n->type == DOM_ELEMENT && selector_matches_ss(ss, n) &&
            js_array_push(c, a, wrap_node(j, n)) != JS_OK) {
            css_free(ss);
            return JS_THROW;
        }
    }
    css_free(ss);
    *ret = js_object_value(a);
    return JS_OK;
}

static struct hostref *node_or_document(js_value value);
static struct dom_node *receiver_node(struct hostref *h);

static int node_get_by_tag(js_ctx *c, js_value t,
                           int ac, js_value *av, js_value *r)
{
    struct hostref *h = node_or_document(t);
    struct dom_node *root = receiver_node(h), *node;
    const char *tag;
    unsigned long tag_len;
    int all;
    js_object *result;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Node receiver");
    if (string_arg(c, ac, av, 0, &tag) != JS_OK)
        return JS_THROW;
    all = !strcmp(tag, "*");
    tag_len = strlen(tag);
    result = js_new_array(c);
    if (!result)
        return JS_THROW;
    for (node = root ? root->first_child : 0;
         node; node = dom_next_within(node, root)) {
        if (node->type != DOM_ELEMENT)
            continue;
        if (!all && (!node->tag ||
                     !ci_eq_n(node->tag, strlen(node->tag), tag, tag_len)))
            continue;
        if (js_array_push(c, result, wrap_node(h->j, node)) != JS_OK)
            return JS_THROW;
    }
    *r = js_object_value(result);
    return JS_OK;
}

static int class_names_match(const struct dom_node *node,
                             const char *names)
{
    const char *cursor = names;
    int found = 0;

    while (*cursor) {
        const char *start;

        while (*cursor == ' ' || *cursor == '\t' ||
               *cursor == '\r' || *cursor == '\n' ||
               *cursor == '\f')
            cursor++;
        if (!*cursor)
            break;
        start = cursor;
        while (*cursor && *cursor != ' ' && *cursor != '\t' &&
               *cursor != '\r' && *cursor != '\n' &&
               *cursor != '\f')
            cursor++;
        if (!dom_has_class_n(node, start, (long)(cursor - start)))
            return 0;
        found = 1;
    }
    return found;
}

static int node_get_by_class(js_ctx *c, js_value t,
                             int ac, js_value *av, js_value *r)
{
    struct hostref *h = node_or_document(t);
    struct dom_node *root = receiver_node(h), *node;
    const char *names;
    js_object *result;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Node receiver");
    if (string_arg(c, ac, av, 0, &names) != JS_OK)
        return JS_THROW;
    result = js_new_array(c);
    if (!result)
        return JS_THROW;
    for (node = root ? root->first_child : 0;
         node; node = dom_next_within(node, root))
        if (node->type == DOM_ELEMENT &&
            class_names_match(node, names) &&
            js_array_push(c, result, wrap_node(h->j, node)) != JS_OK)
            return JS_THROW;
    *r = js_object_value(result);
    return JS_OK;
}

static int node_get_by_tag_ns(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    struct hostref *h = node_or_document(t);
    struct dom_node *root = receiver_node(h), *node;
    const char *namespace_uri = 0, *local_name;
    int any_namespace = 0;
    js_object *result;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Node receiver");
    if (ac < 2 || string_arg(c, ac, av, 1, &local_name) != JS_OK)
        return ac < 2
            ? js_throw_error(c, JS_ERR_TYPE,
                             "getElementsByTagNameNS requires two arguments")
            : JS_THROW;
    if (!js_is_null(av[0]) && !js_is_undefined(av[0])) {
        if (string_arg(c, ac, av, 0, &namespace_uri) != JS_OK)
            return JS_THROW;
        if (!strcmp(namespace_uri, "*"))
            any_namespace = 1;
        else if (!*namespace_uri)
            namespace_uri = 0;
    }
    result = js_new_array(c);
    if (!result)
        return JS_THROW;
    for (node = root ? root->first_child : 0;
         node; node = dom_next_within(node, root)) {
        const char *node_local;

        if (node->type != DOM_ELEMENT)
            continue;
        node_local = strchr(node->tag, ':');
        node_local = node_local ? node_local + 1 : node->tag;
        if ((!any_namespace &&
             (!!namespace_uri != !!node->namespace_uri ||
              (namespace_uri &&
               strcmp(namespace_uri, node->namespace_uri)))) ||
            (strcmp(local_name, "*") &&
             strcmp(local_name, node_local)))
            continue;
        if (js_array_push(c, result, wrap_node(h->j, node)) != JS_OK)
            return JS_THROW;
    }
    *r = js_object_value(result);
    return JS_OK;
}

static void dataset_sync_element(struct jsdom *j, struct dom_node *node);
static void dataset_on_attr_removed(struct jsdom *j, struct dom_node *node,
                                    const char *attr_name);
static void classlist_sync_node(struct jsdom *j, struct dom_node *node);
static void canvas_sync_attr_resize(struct jsdom *j, struct dom_node *node);

static int doc_get_by_name(js_ctx *c, js_value t,
                           int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    struct dom_document *d;
    struct dom_node *node;
    const char *name;
    js_object *result;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Document receiver");
    if (string_arg(c, ac, av, 0, &name) != JS_OK)
        return JS_THROW;
    d = doc_of(h);
    result = js_new_array(c);
    if (!result)
        return JS_THROW;
    for (node = d->root->first_child; node;
         node = dom_next_within(node, d->root)) {
        const char *value = dom_get_attr(node, "name");

        if (node->type == DOM_ELEMENT && value &&
            !strcmp(value, name) &&
            js_array_push(c, result, wrap_node(h->j, node)) != JS_OK)
            return JS_THROW;
    }
    *r = js_object_value(result);
    return JS_OK;
}

static int node_get_attribute_names(js_ctx *c, js_value t,
                                    int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    js_object *result;
    unsigned int index;
    (void)ac; (void)av;

    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Element receiver");
    dataset_sync_element(h->j, h->node);
    result = js_new_array(c);
    if (!result)
        return JS_THROW;
    for (index = 0; index < dom_attr_count(h->node); index++) {
        const struct dom_attr *attribute = dom_attr_at(h->node, index);

        if (attribute &&
            js_array_push(c, result,
                          js_mkcstring(c, attribute->name)) != JS_OK)
            return JS_THROW;
    }
    *r = js_object_value(result);
    return JS_OK;
}

static int node_has_attributes(js_ctx *c, js_value t,
                               int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    (void)c; (void)ac; (void)av;

    *r = js_bool(h && h->node->type == DOM_ELEMENT &&
                 dom_attr_count(h->node) != 0);
    return JS_OK;
}

static const struct dom_attr *find_dom_attr(const struct dom_node *node,
                                            const char *name)
{
    unsigned int index;

    for (index = 0; index < dom_attr_count(node); index++) {
        const struct dom_attr *attribute = dom_attr_at(node, index);

        if (attribute && ci_eq_n(attribute->name, strlen(attribute->name),
                                 name, strlen(name)))
            return attribute;
    }
    return 0;
}

static const struct dom_attr *find_dom_attr_ns(const struct dom_node *node,
                                               const char *namespace_uri,
                                               const char *local_name)
{
    unsigned int index;

    for (index = 0; index < dom_attr_count(node); index++) {
        const struct dom_attr *attribute = dom_attr_at(node, index);
        const char *local = attribute && attribute->local_name
            ? attribute->local_name
            : attribute ? attribute->name : 0;
        int same_namespace = attribute &&
            ((!namespace_uri && !attribute->namespace_uri) ||
             (namespace_uri && attribute->namespace_uri &&
              !strcmp(namespace_uri, attribute->namespace_uri)));

        if (same_namespace && local && !strcmp(local, local_name))
            return attribute;
    }
    return 0;
}

static const struct dom_attr *attr_from_host(const struct hostref *h)
{
    const struct attr_ref *reference;

    if (!h || h->kind != HOST_ATTR)
        return 0;
    reference = (const struct attr_ref *)h->data;
    return reference
        ? find_dom_attr_ns(h->node, reference->namespace_uri,
                           reference->local_name)
        : 0;
}

static js_value wrap_attr(struct jsdom *j, struct dom_node *owner,
                          const struct dom_attr *attribute)
{
    struct attr_ref *reference;
    js_object *object;

    if (!attribute)
        return js_undefined();
    reference = (struct attr_ref *)calloc(1, sizeof *reference);
    if (!reference)
        return js_undefined();
    reference->namespace_uri = attribute->namespace_uri
        ? dup_z(attribute->namespace_uri) : 0;
    reference->local_name = dup_z(attribute->local_name
                                  ? attribute->local_name
                                  : attribute->name);
    if (!reference->local_name ||
        (attribute->namespace_uri && !reference->namespace_uri)) {
        free(reference->namespace_uri);
        free(reference->local_name);
        free(reference);
        return js_undefined();
    }
    object = new_host_data(j, owner, HOST_ATTR, reference, j->attr_proto);
    if (!object) {
        free(reference->namespace_uri);
        free(reference->local_name);
        free(reference);
        return js_undefined();
    }
    return js_object_value(object);
}

static int attr_string_field_get(js_ctx *c, js_value t, js_value *r,
                                 int field)
{
    struct hostref *h = host(t, HOST_ATTR);
    const struct dom_attr *attribute;
    const char *value;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Attr receiver");
    attribute = attr_from_host(h);
    if (!attribute) {
        *r = field >= 2 ? js_null() : js_mkcstring(c, "");
        return js_fatal(c) ? JS_THROW : JS_OK;
    }
    value = field == 0 ? attribute->name :
            field == 1 ? (attribute->local_name
                           ? attribute->local_name : attribute->name) :
            field == 2 ? attribute->namespace_uri : attribute->prefix;
    *r = value ? js_mkcstring(c, value) : js_null();
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int attr_name_get(js_ctx *c, js_value t,
                         int ac, js_value *av, js_value *r)
{
    (void)ac; (void)av;
    return attr_string_field_get(c, t, r, 0);
}

static int attr_local_name_get(js_ctx *c, js_value t,
                               int ac, js_value *av, js_value *r)
{
    (void)ac; (void)av;
    return attr_string_field_get(c, t, r, 1);
}

static int attr_namespace_get(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    (void)ac; (void)av;
    return attr_string_field_get(c, t, r, 2);
}

static int attr_prefix_get(js_ctx *c, js_value t,
                           int ac, js_value *av, js_value *r)
{
    (void)ac; (void)av;
    return attr_string_field_get(c, t, r, 3);
}

static int attr_value_get(js_ctx *c, js_value t,
                          int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_ATTR);
    const struct dom_attr *attribute;
    (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Attr receiver");
    attribute = attr_from_host(h);
    *r = js_mkcstring(c, attribute ? attribute->value : "");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int attr_value_set(js_ctx *c, js_value t,
                          int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_ATTR);
    const struct dom_attr *attribute;
    const char *value, *old;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Attr receiver");
    if (string_arg(c, ac, av, 0, &value) != JS_OK)
        return JS_THROW;
    attribute = attr_from_host(h);
    if (!attribute)
        return js_throw_error(c, JS_ERR_ERROR,
                              "attribute is no longer attached");
    old = attribute->value;
    if (attribute->namespace_uri
            ? !dom_set_attr_ns(h->node, attribute->namespace_uri,
                               attribute->name, value)
            : !dom_set_attr(h->node, attribute->name, value))
        return js_throw_error(c, JS_ERR_ERROR,
                              "cannot set attribute");
    if (attribute->namespace_uri
            ? mutation_attribute_ns(h->j, h->node,
                                    attribute->namespace_uri,
                                    attribute->local_name, old)
            : mutation_attribute(h->j, h->node,
                                 attribute->name, old) != JS_OK)
        return JS_THROW;
    if (!attribute->namespace_uri) {
        if (ci_eq_n(attribute->name, strlen(attribute->name), "class", 5))
            classlist_sync_node(h->j, h->node);
        if (h->node->tag_id == HTAG_CANVAS && h->node->canvas &&
            (ci_eq_n(attribute->name, strlen(attribute->name), "width", 5) ||
             ci_eq_n(attribute->name, strlen(attribute->name), "height", 6)))
            canvas_sync_attr_resize(h->j, h->node);
    }
    mark_dom_dirty(h->j);
    *r = js_undefined();
    return JS_OK;
}

static int attr_owner_get(js_ctx *c, js_value t,
                          int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_ATTR);
    (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Attr receiver");
    *r = wrap_node(h->j, h->node);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int attr_owner_document_get(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_ATTR);
    (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Attr receiver");
    return js_get(c, js_global(c), "document", r);
}

static int attr_type_get(js_ctx *c, js_value t,
                         int ac, js_value *av, js_value *r)
{
    (void)c; (void)t; (void)ac; (void)av;
    *r = js_number(2);
    return JS_OK;
}

static int attr_specified_get(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_ATTR);
    (void)c; (void)ac; (void)av;
    *r = js_bool(h != 0);
    return JS_OK;
}

static int named_node_map_length_get(js_ctx *c, js_value t,
                                     int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NAMED_NODE_MAP);
    (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal NamedNodeMap receiver");
    *r = js_number(dom_attr_count(h->node));
    return JS_OK;
}

static int named_node_map_item(js_ctx *c, js_value t,
                               int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NAMED_NODE_MAP);
    const struct dom_attr *attribute;
    double requested;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal NamedNodeMap receiver");
    if (js_to_integer(c, ac ? av[0] : js_undefined(),
                      &requested) != JS_OK)
        return JS_THROW;
    if (requested < 0 || requested >= dom_attr_count(h->node)) {
        *r = js_null();
        return JS_OK;
    }
    attribute = dom_attr_at(h->node, (unsigned int)requested);
    *r = attribute
        ? wrap_attr(h->j, h->node, attribute) : js_null();
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int named_node_map_get(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NAMED_NODE_MAP);
    const struct dom_attr *attribute;
    const char *name;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal NamedNodeMap receiver");
    if (string_arg(c, ac, av, 0, &name) != JS_OK)
        return JS_THROW;
    attribute = find_dom_attr(h->node, name);
    *r = attribute
        ? wrap_attr(h->j, h->node, attribute) : js_null();
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int named_node_map_get_ns(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NAMED_NODE_MAP);
    const struct dom_attr *attribute;
    const char *namespace_uri = 0, *local_name;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal NamedNodeMap receiver");
    if (ac < 2 || string_arg(c, ac, av, 1, &local_name) != JS_OK)
        return ac < 2 ? (*r = js_null(), JS_OK) : JS_THROW;
    if (!js_is_null(av[0]) && !js_is_undefined(av[0])) {
        if (string_arg(c, ac, av, 0, &namespace_uri) != JS_OK)
            return JS_THROW;
        if (!*namespace_uri)
            namespace_uri = 0;
    }
    attribute = find_dom_attr_ns(h->node, namespace_uri, local_name);
    *r = attribute ? wrap_attr(h->j, h->node, attribute) : js_null();
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int node_attributes_get(js_ctx *c, js_value t,
                               int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    js_object *map;
    js_value map_value;
    unsigned int index;
    (void)ac; (void)av;

    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Element receiver");
    dataset_sync_element(h->j, h->node);
    map = new_host(h->j, h->node, HOST_NAMED_NODE_MAP,
                   h->j->named_node_map_proto);
    if (!map)
        return JS_THROW;
    map_value = js_object_value(map);
    for (index = 0; index < dom_attr_count(h->node); index++) {
        const struct dom_attr *attribute = dom_attr_at(h->node, index);
        char key[24];
        js_value attribute_value;

        if (!attribute)
            continue;
        attribute_value = wrap_attr(h->j, h->node, attribute);
        if (js_is_undefined(attribute_value))
            return JS_THROW;
        snprintf(key, sizeof(key), "%u", index);
        if (js_set(c, map_value, key, attribute_value) != JS_OK)
            return JS_THROW;
        if (strcmp(attribute->name, "length") &&
            strcmp(attribute->name, "item") &&
            strcmp(attribute->name, "getNamedItem") &&
            strcmp(attribute->name, "getNamedItemNS") &&
            js_set(c, map_value, attribute->name,
                   attribute_value) != JS_OK)
            return JS_THROW;
    }
    *r = map_value;
    return JS_OK;
}

static int node_get_attribute_node(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const struct dom_attr *attribute;
    const char *name;

    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &name) != JS_OK)
        return JS_THROW;
    attribute = find_dom_attr(h->node, name);
    *r = attribute
        ? wrap_attr(h->j, h->node, attribute) : js_null();
    return js_fatal(c) ? JS_THROW : JS_OK;
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
    if (h->node->type == DOM_TEXT || h->node->type == DOM_COMMENT) {
        *r = js_mkstring(c, h->node->text, h->node->text_len);
        return JS_OK;
    }
    s = dom_text_content(h->node, &n);
    if (!s) return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    *r = js_mkstring(c, s, n);
    free(s);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int node_node_value_set(js_ctx *c, js_value t, int ac,
                               js_value *av, js_value *r);

static int node_text_set(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const char *s;

    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal Node receiver");
    if (string_arg(c, ac, av, 0, &s) != JS_OK) return JS_THROW;
    if (h->node->type == DOM_TEXT || h->node->type == DOM_COMMENT)
        return node_node_value_set(c, t, ac, av, r);
    if (!set_text(h->j, h->node, s))
        return js_throw_error(c, JS_ERR_ERROR, "cannot set textContent");
    *r = js_undefined();
    return JS_OK;
}

static int node_html_get(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    struct dom_node *src;
    char *html;
    unsigned long len;
    (void)ac; (void)av;

    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    dataset_sync_all(h->j);
    src = (h->node->tag_id == HTAG_TEMPLATE &&
           h->node->namespace_id == DOM_NS_HTML)
        ? template_content_node(h->j, h->node) : h->node;
    html = children_html(src, &len);
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
    struct dom_node *target;
    const char *html;

    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &html) != JS_OK)
        return JS_THROW;
    target = (h->node->tag_id == HTAG_TEMPLATE &&
              h->node->namespace_id == DOM_NS_HTML)
        ? template_content_node(h->j, h->node) : h->node;
    if (!target || !set_inner_html_ctx(h->j, target, h->node, html))
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
    dataset_sync_all(h->j);
    html = dom_serialize(h->node, &len);
    if (!html)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    if (h->node->type == DOM_ELEMENT && h->node->tag_id == HTAG_TEMPLATE &&
        h->node->namespace_id == DOM_NS_HTML) {
        struct dom_node *frag = template_content_node(h->j, h->node);
        unsigned long ilen = 0;
        char *inner = frag ? children_html(frag, &ilen) : 0;

        if (inner && ilen) {
            const char *close_tag = "</template>";
            unsigned long clen = 11;
            if (len >= clen && !memcmp(html + len - clen, close_tag, clen)) {
                char *merged = (char *)malloc(len + ilen + 1);
                if (merged) {
                    memcpy(merged, html, len - clen);
                    memcpy(merged + len - clen, inner, ilen);
                    memcpy(merged + len - clen + ilen, close_tag, clen);
                    merged[len + ilen] = 0;
                    free(html);
                    html = merged;
                    len += ilen;
                }
            }
        }
        free(inner);
    }
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
    const char *v, *old;

    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &v) != JS_OK) return JS_THROW;
    old = dom_get_attr(h->node, name);
    if (!dom_set_attr(h->node, name, v))
        return js_throw_error(c, JS_ERR_ERROR, "cannot set attribute");
    if (mutation_attribute(h->j, h->node, name, old) != JS_OK)
        return JS_THROW;
    if (!strcmp(name, "class"))
        classlist_sync_node(h->j, h->node);
    mark_dom_dirty(h->j);
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
    const char *old;
    int on;

    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    old = dom_get_attr(h->node, name);
    on = ac > 0 && js_to_boolean(av[0]);
    if (on) {
        if (!dom_set_attr(h->node, name, ""))
            return js_throw_error(c, JS_ERR_ERROR, "cannot set attribute");
    } else {
        dom_remove_attr(h->node, name);
    }
    if (!!old != !!on &&
        mutation_attribute(h->j, h->node, name, old) != JS_OK)
        return JS_THROW;
    mark_dom_dirty(h->j);
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
        name[i] = (char)(h->node->namespace_id == DOM_NS_HTML &&
                         ch >= 'a' && ch <= 'z'
                         ? ch - ('a' - 'A') : ch);
    }
    name[n] = 0;
    *r = js_mkcstring(c, name);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static struct hostref *node_or_document(js_value value)
{
    struct hostref *h = host(value, HOST_NODE);
    return h ? h : host(value, HOST_DOCUMENT);
}

static int node_dom_name_get(js_ctx *c, js_value t, int ac, js_value *av,
                             js_value *r)
{
    struct hostref *h = node_or_document(t);
    (void)ac; (void)av;

    if (!h) {
        *r = js_null();
        return JS_OK;
    }
    if (h->kind == HOST_DOCUMENT) {
        *r = js_mkcstring(c, "#document");
        return js_fatal(c) ? JS_THROW : JS_OK;
    }
    if (h->node->type == DOM_ELEMENT)
        return node_tag_get(c, t, 0, 0, r);
    if (h->node->type == DOM_TEXT)
        *r = js_mkcstring(c, "#text");
    else if (h->node->type == DOM_COMMENT)
        *r = js_mkcstring(c, "#comment");
    else if (h->node->type == DOM_DOCTYPE)
        *r = js_mkcstring(c, h->node->text ? h->node->text : "html");
    else if (h->node->type == DOM_FRAGMENT)
        *r = js_mkcstring(c, "#document-fragment");
    else
        *r = js_mkcstring(c, "#document");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int node_local_name_get(js_ctx *c, js_value t,
                               int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const char *local;
    (void)ac; (void)av;

    local = h && h->node->type == DOM_ELEMENT
        ? strchr(h->node->tag, ':') : 0;
    *r = h && h->node->type == DOM_ELEMENT
        ? js_mkcstring(c, local ? local + 1 : h->node->tag) : js_null();
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int node_prefix_get(js_ctx *c, js_value t,
                           int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const char *colon;
    (void)ac; (void)av;

    colon = h && h->node->type == DOM_ELEMENT
        ? strchr(h->node->tag, ':') : 0;
    *r = colon
        ? js_mkstring(c, h->node->tag,
                      (unsigned long)(colon - h->node->tag))
        : js_null();
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int node_namespace_get(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    (void)ac; (void)av;

    if (!h || h->node->type != DOM_ELEMENT ||
        !h->node->namespace_uri)
        *r = js_null();
    else
        *r = js_mkcstring(c, h->node->namespace_uri);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static void dataset_sync_element(struct jsdom *j, struct dom_node *node);
static void classlist_sync_node(struct jsdom *j, struct dom_node *node);
static void canvas_sync_attr_resize(struct jsdom *j, struct dom_node *node);

static int node_owner_document_get(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    struct hostref *h = node_or_document(t);
    (void)ac; (void)av;

    if (!h || h->kind == HOST_DOCUMENT) {
        *r = js_null();
        return JS_OK;
    }
    if (h->node && h->node->doc && h->node->doc != h->j->doc &&
        h->node->doc->root && h->node->doc->root->user) {
        *r = js_object_value((js_object *)h->node->doc->root->user);
        return JS_OK;
    }
    if (js_get(c, js_global(c), "document", r) != JS_OK)
        return JS_THROW;
    return JS_OK;
}

static int node_node_value_get(js_ctx *c, js_value t, int ac, js_value *av,
                               js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    (void)ac; (void)av;

    if (h && (h->node->type == DOM_TEXT ||
              h->node->type == DOM_COMMENT)) {
        *r = js_mkstring(c, h->node->text, h->node->text_len);
        return js_fatal(c) ? JS_THROW : JS_OK;
    }
    *r = js_null();
    return JS_OK;
}

static int node_node_value_set(js_ctx *c, js_value t, int ac, js_value *av,
                               js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    struct dom_node *replacement, *parent, *old_node;
    struct dom_document *d;
    const char *value, *old_value;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Node receiver");
    if (h->node->type != DOM_TEXT && h->node->type != DOM_COMMENT) {
        *r = js_undefined();
        return JS_OK;
    }
    if (string_arg(c, ac, av, 0, &value) != JS_OK)
        return JS_THROW;
    d = h->node->doc ? h->node->doc : h->j->doc;
    replacement = h->node->type == DOM_TEXT
        ? dom_create_text(d, value, strlen(value))
        : dom_create_comment(d, value, strlen(value));
    if (!replacement)
        return js_throw_error(c, JS_ERR_ERROR, "cannot set nodeValue");
    old_node = h->node;
    old_value = old_node->text;
    parent = old_node->parent;
    if (parent) {
        if (!dom_insert_before(parent, replacement, old_node))
            return js_throw_error(c, JS_ERR_ERROR, "cannot set nodeValue");
    }
    if (mutation_character_data(h->j, old_node, old_value) != JS_OK) {
        if (parent)
            dom_remove_child(parent, replacement);
        return JS_THROW;
    }
    if (parent)
        dom_remove_child(parent, old_node);
    mutation_rebind_target(h->j, old_node, replacement);
    old_node->user = 0;
    h->node = replacement;
    replacement->user = t.u.obj;
    mark_dom_dirty(h->j);
    *r = js_undefined();
    return JS_OK;
}

static int node_type_get(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = node_or_document(t);
    (void)c; (void)ac; (void)av;
    *r = js_number(!h ? 0 : h->kind == HOST_DOCUMENT ? 9 :
                   h->node->type == DOM_ELEMENT ? 1 :
                   h->node->type == DOM_TEXT ? 3 :
                   h->node->type == DOM_COMMENT ? 8 :
                   h->node->type == DOM_DOCTYPE ? 10 :
                   h->node->type == DOM_FRAGMENT ? 11 : 9);
    return JS_OK;
}

static struct dom_node *receiver_node(struct hostref *h);

static int relative_get(js_ctx *c, js_value t, int which, js_value *r)
{
    struct hostref *h = node_or_document(t);
    struct dom_node *base = receiver_node(h);
    struct dom_node *n = 0;
    if (base) {
        if (which == 0 && h->kind != HOST_DOCUMENT) n = base->parent;
        if (which == 1) n = base->first_child;
        if (which == 2) n = base->last_child;
        if (which == 3 && h->kind != HOST_DOCUMENT) n = base->next_sibling;
        if (which == 4 && h->kind != HOST_DOCUMENT) n = base->prev_sibling;
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

static int element_relative_get(js_ctx *c, js_value t, int which,
                                js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    struct dom_node *n = 0, *cursor;

    if (!h) {
        *r = js_null();
        return JS_OK;
    }
    if (which == 0)
        n = dom_first_element_child(h->node);
    else if (which == 1) {
        for (cursor = h->node->last_child; cursor;
             cursor = cursor->prev_sibling)
            if (cursor->type == DOM_ELEMENT) {
                n = cursor;
                break;
            }
    } else if (which == 2)
        n = dom_next_element_sibling(h->node);
    else if (which == 3) {
        for (cursor = h->node->prev_sibling; cursor;
             cursor = cursor->prev_sibling)
            if (cursor->type == DOM_ELEMENT) {
                n = cursor;
                break;
            }
    }
    *r = wrap_node(h->j, n);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

#define ELEMENT_REL_GETTER(name, which)                                      \
static int name(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)    \
{ (void)ac; (void)av; return element_relative_get(c, t, which, r); }
ELEMENT_REL_GETTER(node_first_element_get, 0)
ELEMENT_REL_GETTER(node_last_element_get, 1)
ELEMENT_REL_GETTER(node_next_element_get, 2)
ELEMENT_REL_GETTER(node_prev_element_get, 3)
#undef ELEMENT_REL_GETTER

static int node_child_element_count_get(js_ctx *c, js_value t,
                                        int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    (void)c; (void)ac; (void)av;

    *r = js_number(h ? dom_element_child_count(h->node) : 0);
    return JS_OK;
}

static int node_is_connected_get(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    struct dom_node *n;
    (void)c; (void)ac; (void)av;

    for (n = h ? h->node : 0; n && n->parent; n = n->parent)
        ;
    *r = js_bool(h && n && (n == h->j->doc->root || n->type == DOM_DOCUMENT));
    return JS_OK;
}

static int node_children_get(js_ctx *c, js_value t, int ac, js_value *av,
                             js_value *r)
{
    struct hostref *h = node_or_document(t);
    js_object *a;
    struct dom_node *parent = receiver_node(h), *n;
    (void)ac; (void)av;

    if (!h) { *r = js_null(); return JS_OK; }
    a = js_new_array(c);
    if (!a) return JS_THROW;
    for (n = parent ? parent->first_child : 0; n; n = n->next_sibling)
        if (n->type == DOM_ELEMENT &&
            js_array_push(c, a, wrap_node(h->j, n)) != JS_OK)
            return JS_THROW;
    *r = js_object_value(a);
    return JS_OK;
}

static struct dom_node *receiver_node(struct hostref *h)
{
    struct dom_document *d;

    if (!h)
        return 0;
    if (h->kind == HOST_DOCUMENT) {
        d = doc_of(h);
        return d ? d->root : 0;
    }
    return h->node;
}

static int node_child_nodes_get(js_ctx *c, js_value t, int ac, js_value *av,
                                js_value *r)
{
    struct hostref *h = node_or_document(t);
    struct dom_node *parent = receiver_node(h), *node;
    js_object *nodes;
    (void)ac; (void)av;

    if (!h) {
        *r = js_null();
        return JS_OK;
    }
    nodes = js_new_array(c);
    if (!nodes)
        return JS_THROW;
    for (node = parent ? parent->first_child : 0;
         node; node = node->next_sibling)
        if (js_array_push(c, nodes, wrap_node(h->j, node)) != JS_OK)
            return JS_THROW;
    *r = js_object_value(nodes);
    return JS_OK;
}

static int node_has_child_nodes(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    struct hostref *h = node_or_document(t);
    struct dom_node *node = receiver_node(h);
    (void)c; (void)ac; (void)av;

    *r = js_bool(node && node->first_child);
    return JS_OK;
}

static int node_get_root(js_ctx *c, js_value t,
                         int ac, js_value *av, js_value *r)
{
    struct hostref *h = node_or_document(t);
    struct dom_node *node = receiver_node(h);
    (void)ac; (void)av;

    if (!h || !node) {
        *r = js_null();
        return JS_OK;
    }
    while (node->parent)
        node = node->parent;
    if (node == h->j->doc->root)
        return js_get(c, js_global(c), "document", r);
    if (node->type == DOM_DOCUMENT && node->user) {
        *r = js_object_value((js_object *)node->user);
        return JS_OK;
    }
    *r = wrap_node(h->j, node);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int node_get_attr(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const char *name, *v;
    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &name) != JS_OK) return JS_THROW;
    if (strlen(name) > 5 && ci_eq_n(name, 5, "data-", 5))
        dataset_sync_element(h->j, h->node);
    v = dom_get_attr(h->node, name);
    *r = v ? js_mkcstring(c, v) : js_null();
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int node_set_attr(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const char *name, *value, *old;
    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &name) != JS_OK ||
        string_arg(c, ac, av, 1, &value) != JS_OK) return JS_THROW;
    if (strlen(name) > 5 && ci_eq_n(name, 5, "data-", 5))
        dataset_sync_element(h->j, h->node);
    old = dom_get_attr(h->node, name);
    if (!dom_set_attr(h->node, name, value))
        return js_throw_error(c, JS_ERR_ERROR, "cannot set attribute");
    if (mutation_attribute(h->j, h->node, name, old) != JS_OK)
        return JS_THROW;
    if (ci_eq_n(name, strlen(name), "class", 5))
        classlist_sync_node(h->j, h->node);
    if (h->node->tag_id == HTAG_CANVAS && h->node->canvas &&
        (ci_eq_n(name, strlen(name), "width", 5) ||
         ci_eq_n(name, strlen(name), "height", 6)))
        canvas_sync_attr_resize(h->j, h->node);
    mark_dom_dirty(h->j);
    *r = js_undefined();
    return JS_OK;
}

static int node_remove_attr(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const char *name, *old;
    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &name) != JS_OK) return JS_THROW;
    if (strlen(name) > 5 && ci_eq_n(name, 5, "data-", 5))
        dataset_sync_element(h->j, h->node);
    old = dom_get_attr(h->node, name);
    dom_remove_attr(h->node, name);
    if (strlen(name) > 5 && ci_eq_n(name, 5, "data-", 5))
        dataset_on_attr_removed(h->j, h->node, name);
    if (old && mutation_attribute(h->j, h->node, name, old) != JS_OK)
        return JS_THROW;
    if (ci_eq_n(name, strlen(name), "class", 5))
        classlist_sync_node(h->j, h->node);
    if (h->node->tag_id == HTAG_CANVAS && h->node->canvas &&
        (ci_eq_n(name, strlen(name), "width", 5) ||
         ci_eq_n(name, strlen(name), "height", 6)))
        canvas_sync_attr_resize(h->j, h->node);
    mark_dom_dirty(h->j);
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
    if (strlen(name) > 5 && ci_eq_n(name, 5, "data-", 5))
        dataset_sync_element(h->j, h->node);
    *r = js_bool(dom_has_attr(h->node, name));
    return JS_OK;
}

static int node_toggle_attr(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const char *name;
    int present, enabled;

    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &name) != JS_OK)
        return JS_THROW;
    if (strlen(name) > 5 && ci_eq_n(name, 5, "data-", 5))
        dataset_sync_element(h->j, h->node);
    present = dom_has_attr(h->node, name);
    {
        const char *old = dom_get_attr(h->node, name);
    enabled = ac > 1 ? js_to_boolean(av[1]) : !present;
    if (enabled) {
        if (!present && !dom_set_attr(h->node, name, ""))
            return js_throw_error(c, JS_ERR_ERROR,
                                  "cannot set attribute");
    } else if (present) {
        dom_remove_attr(h->node, name);
        if (strlen(name) > 5 && ci_eq_n(name, 5, "data-", 5))
            dataset_on_attr_removed(h->j, h->node, name);
    }
    if (present != enabled &&
        mutation_attribute(h->j, h->node, name, old) != JS_OK)
        return JS_THROW;
    }
    if (ci_eq_n(name, strlen(name), "class", 5))
        classlist_sync_node(h->j, h->node);
    mark_dom_dirty(h->j);
    *r = js_bool(enabled);
    return JS_OK;
}

static int node_append(js_ctx *c, js_value t, int ac, js_value *av,
                       js_value *r)
{
    struct hostref *p = host(t, HOST_NODE);
    struct hostref *ch = ac ? host(av[0], HOST_NODE) : 0;
    struct mutation_node_snapshot snapshot[JSDOM_MUTATION_NODES_MAX];
    unsigned int count;

    if (!p || !ch || p->j != ch->j)
        return js_throw_error(c, JS_ERR_TYPE, "appendChild requires a Node");
    if (!mutation_snapshot_nodes(ch->node, snapshot, &count))
        return js_throw_error(c, JS_ERR_RANGE,
                              "fragment has too many children");
    if (!dom_append_child(p->node, ch->node))
        return js_throw_error(c, JS_ERR_ERROR, "cannot append child");
    if (mutation_queue_moved_nodes(p->j, p->node,
                                   snapshot, count) != JS_OK)
        return JS_THROW;
    p->j->dirty = 1;
    *r = av[0];
    return JS_OK;
}

static int node_remove(js_ctx *c, js_value t, int ac, js_value *av,
                       js_value *r)
{
    struct hostref *p = host(t, HOST_NODE);
    struct hostref *ch = ac ? host(av[0], HOST_NODE) : 0;
    struct dom_node *previous, *next;

    if (!p || !ch || ch->node->parent != p->node)
        return js_throw_error(c, JS_ERR_ERROR, "node is not a child");
    previous = ch->node->prev_sibling;
    next = ch->node->next_sibling;
    dom_remove_child(p->node, ch->node);
    if (mutation_child(p->j, p->node, 0, ch->node,
                       previous, next) != JS_OK)
        return JS_THROW;
    p->j->dirty = 1;
    *r = av[0];
    return JS_OK;
}

static int node_insert_before(js_ctx *c, js_value t, int ac, js_value *av,
                              js_value *r)
{
    struct hostref *parent = host(t, HOST_NODE);
    struct hostref *child = ac ? host(av[0], HOST_NODE) : 0;
    struct hostref *reference =
        ac > 1 && !js_is_null(av[1]) ? host(av[1], HOST_NODE) : 0;
    struct mutation_node_snapshot snapshot[JSDOM_MUTATION_NODES_MAX];
    unsigned int count;

    if (!parent || !child || parent->j != child->j ||
        (ac > 1 && !js_is_null(av[1]) &&
         (!reference || reference->j != parent->j)))
        return js_throw_error(c, JS_ERR_TYPE,
                              "insertBefore requires Nodes");
    if (reference && child->node == reference->node) {
        *r = av[0];
        return JS_OK;
    }
    if (!mutation_snapshot_nodes(child->node, snapshot, &count))
        return js_throw_error(c, JS_ERR_RANGE,
                              "fragment has too many children");
    if (!dom_insert_before(parent->node, child->node,
                           reference ? reference->node : 0))
        return js_throw_error(c, JS_ERR_ERROR,
                              "cannot insert child");
    if (mutation_queue_moved_nodes(parent->j, parent->node,
                                   snapshot, count) != JS_OK)
        return JS_THROW;
    parent->j->dirty = 1;
    *r = av[0];
    return JS_OK;
}

static int node_replace_child(js_ctx *c, js_value t, int ac, js_value *av,
                              js_value *r)
{
    struct hostref *parent = host(t, HOST_NODE);
    struct hostref *child = ac ? host(av[0], HOST_NODE) : 0;
    struct hostref *old = ac > 1 ? host(av[1], HOST_NODE) : 0;
    struct mutation_node_snapshot snapshot[JSDOM_MUTATION_NODES_MAX];
    struct dom_node *previous, *next;
    unsigned int count;

    if (!parent || !child || !old || parent->j != child->j ||
        parent->j != old->j)
        return js_throw_error(c, JS_ERR_TYPE,
                              "replaceChild requires Nodes");
    if (old->node->parent != parent->node)
        return js_throw_error(c, JS_ERR_ERROR,
                              "node is not a child");
    if (child->node != old->node) {
        if (!mutation_snapshot_nodes(child->node, snapshot, &count))
            return js_throw_error(c, JS_ERR_RANGE,
                                  "fragment has too many children");
        previous = old->node->prev_sibling;
        next = old->node->next_sibling;
        if (!dom_insert_before(parent->node, child->node, old->node))
            return js_throw_error(c, JS_ERR_ERROR,
                                  "cannot insert replacement");
        dom_remove_child(parent->node, old->node);
        if (mutation_queue_moved_nodes(parent->j, parent->node,
                                       snapshot, count) != JS_OK)
            return JS_THROW;
        if (mutation_child(parent->j, parent->node, 0, old->node,
                           previous, next) != JS_OK)
            return JS_THROW;
        parent->j->dirty = 1;
    }
    *r = av[1];
    return JS_OK;
}

static int collect_nodes_or_text(js_ctx *c, struct hostref *owner,
                                 int ac, js_value *av,
                                 struct dom_node ***out)
{
    struct dom_node **nodes;
    int i;

    *out = 0;
    if (ac < 0 || ac > 128)
        return js_throw_error(c, JS_ERR_RANGE,
                              "too many nodes for one mutation");
    if (!ac)
        return JS_OK;
    nodes = (struct dom_node **)calloc((unsigned long)ac, sizeof(*nodes));
    if (!nodes)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    for (i = 0; i < ac; i++) {
        struct hostref *node = host(av[i], HOST_NODE);

        if (node) {
            if (node->j != owner->j) {
                free(nodes);
                return js_throw_error(c, JS_ERR_ERROR,
                                      "Node belongs to another document");
            }
            nodes[i] = node->node;
        } else {
            js_value text;
            const char *bytes;
            unsigned long length;

            if (js_to_string(c, av[i], &text) != JS_OK) {
                free(nodes);
                return JS_THROW;
            }
            bytes = js_string_bytes(text, &length);
            nodes[i] = dom_create_text(owner->j->doc, bytes, length);
            if (!nodes[i]) {
                free(nodes);
                return js_throw_error(c, JS_ERR_ERROR,
                                      "cannot create text node");
            }
        }
    }
    *out = nodes;
    return JS_OK;
}

static int node_parent_mutation(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r,
                                int mode)
{
    struct hostref *owner = host(t, HOST_NODE);
    struct dom_node **nodes = 0;
    struct dom_node *reference, *child, *next;
    struct mutation_node_snapshot snapshot[JSDOM_MUTATION_NODES_MAX];
    unsigned int count;
    int i;

    if (!owner || (owner->node->type != DOM_ELEMENT &&
                   owner->node->type != DOM_DOCUMENT &&
                   owner->node->type != DOM_FRAGMENT))
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal ParentNode receiver");
    if (collect_nodes_or_text(c, owner, ac, av, &nodes) != JS_OK)
        return JS_THROW;
    reference = mode == 1 ? owner->node->first_child : 0;
    if (mode == 2) {
        for (child = owner->node->first_child; child; child = next) {
            struct dom_node *previous = child->prev_sibling;

            next = child->next_sibling;
            dom_remove_child(owner->node, child);
            if (mutation_child(owner->j, owner->node, 0, child,
                               previous, next) != JS_OK) {
                free(nodes);
                return JS_THROW;
            }
        }
    }
    for (i = 0; i < ac; i++) {
        int ok;

        if (mode == 1 && nodes[i] == reference)
            continue;
        if (!mutation_snapshot_nodes(nodes[i], snapshot, &count)) {
            free(nodes);
            return js_throw_error(c, JS_ERR_RANGE,
                                  "fragment has too many children");
        }
        ok = mode == 1
            ? dom_insert_before(owner->node, nodes[i], reference)
            : dom_append_child(owner->node, nodes[i]);
        if (!ok) {
            free(nodes);
            return js_throw_error(c, JS_ERR_ERROR,
                                  "cannot insert node");
        }
        if (mutation_queue_moved_nodes(owner->j, owner->node,
                                       snapshot, count) != JS_OK) {
            free(nodes);
            return JS_THROW;
        }
    }
    free(nodes);
    owner->j->dirty = 1;
    *r = js_undefined();
    return JS_OK;
}

static int node_append_many(js_ctx *c, js_value t,
                            int ac, js_value *av, js_value *r)
{
    return node_parent_mutation(c, t, ac, av, r, 0);
}

static int node_prepend_many(js_ctx *c, js_value t,
                             int ac, js_value *av, js_value *r)
{
    return node_parent_mutation(c, t, ac, av, r, 1);
}

static int node_replace_children(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    return node_parent_mutation(c, t, ac, av, r, 2);
}

static int node_self_remove(js_ctx *c, js_value t,
                            int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    (void)c; (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal ChildNode receiver");
    if (h->node->parent) {
        struct dom_node *parent = h->node->parent;
        struct dom_node *previous = h->node->prev_sibling;
        struct dom_node *next = h->node->next_sibling;

        dom_remove_child(parent, h->node);
        if (mutation_child(h->j, parent, 0, h->node,
                           previous, next) != JS_OK)
            return JS_THROW;
        h->j->dirty = 1;
    }
    *r = js_undefined();
    return JS_OK;
}

static int node_sibling_mutation(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r,
                                 int mode)
{
    struct hostref *owner = host(t, HOST_NODE);
    struct dom_node **nodes = 0;
    struct dom_node *parent, *reference;
    struct mutation_node_snapshot snapshot[JSDOM_MUTATION_NODES_MAX];
    unsigned int count;
    int i, kept_self = 0;

    if (!owner)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal ChildNode receiver");
    parent = owner->node->parent;
    if (!parent) {
        *r = js_undefined();
        return JS_OK;
    }
    if (collect_nodes_or_text(c, owner, ac, av, &nodes) != JS_OK)
        return JS_THROW;
    reference = mode == 0 ? owner->node : owner->node->next_sibling;
    for (i = 0; i < ac; i++) {
        if (nodes[i] == owner->node)
            kept_self = 1;
        if (nodes[i] == reference) {
            reference = reference->next_sibling;
            continue;
        }
        if (!mutation_snapshot_nodes(nodes[i], snapshot, &count)) {
            free(nodes);
            return js_throw_error(c, JS_ERR_RANGE,
                                  "fragment has too many children");
        }
        if (!dom_insert_before(parent, nodes[i], reference)) {
            free(nodes);
            return js_throw_error(c, JS_ERR_ERROR,
                                  "cannot insert sibling");
        }
        if (mutation_queue_moved_nodes(owner->j, parent,
                                       snapshot, count) != JS_OK) {
            free(nodes);
            return JS_THROW;
        }
    }
    if (mode == 2 && !kept_self) {
        struct dom_node *previous = owner->node->prev_sibling;
        struct dom_node *next = owner->node->next_sibling;

        dom_remove_child(parent, owner->node);
        if (mutation_child(owner->j, parent, 0, owner->node,
                           previous, next) != JS_OK) {
            free(nodes);
            return JS_THROW;
        }
    }
    free(nodes);
    owner->j->dirty = 1;
    *r = js_undefined();
    return JS_OK;
}

static int node_before(js_ctx *c, js_value t,
                       int ac, js_value *av, js_value *r)
{
    return node_sibling_mutation(c, t, ac, av, r, 0);
}

static int node_after(js_ctx *c, js_value t,
                      int ac, js_value *av, js_value *r)
{
    return node_sibling_mutation(c, t, ac, av, r, 1);
}

static int node_replace_with(js_ctx *c, js_value t,
                             int ac, js_value *av, js_value *r)
{
    return node_sibling_mutation(c, t, ac, av, r, 2);
}

enum adjacent_position {
    ADJ_BEFORE_BEGIN = 0,
    ADJ_AFTER_BEGIN,
    ADJ_BEFORE_END,
    ADJ_AFTER_END
};

static int adjacent_site(js_ctx *c, struct hostref *owner,
                         const char *position, struct dom_node **parent,
                         struct dom_node **reference)
{
    int where;

    if (ci_eq_n(position, strlen(position),
                "beforebegin", 11))
        where = ADJ_BEFORE_BEGIN;
    else if (ci_eq_n(position, strlen(position),
                     "afterbegin", 10))
        where = ADJ_AFTER_BEGIN;
    else if (ci_eq_n(position, strlen(position),
                     "beforeend", 9))
        where = ADJ_BEFORE_END;
    else if (ci_eq_n(position, strlen(position),
                     "afterend", 8))
        where = ADJ_AFTER_END;
    else
        return js_throw_error(c, JS_ERR_ERROR,
                              "invalid adjacent position");
    if (where == ADJ_BEFORE_BEGIN || where == ADJ_AFTER_END) {
        *parent = owner->node->parent;
        if (!*parent) {
            *reference = 0;
            return 0;
        }
        *reference = where == ADJ_BEFORE_BEGIN
            ? owner->node : owner->node->next_sibling;
    } else {
        *parent = owner->node;
        *reference = where == ADJ_AFTER_BEGIN
            ? owner->node->first_child : 0;
    }
    return 1;
}

static int node_insert_adjacent_element(js_ctx *c, js_value t,
                                        int ac, js_value *av, js_value *r)
{
    struct hostref *owner = host(t, HOST_NODE);
    struct hostref *element = ac > 1 ? host(av[1], HOST_NODE) : 0;
    struct mutation_node_snapshot snapshot[JSDOM_MUTATION_NODES_MAX];
    struct dom_node *parent, *reference;
    const char *position;
    unsigned int count;
    int site;

    if (!owner || owner->node->type != DOM_ELEMENT ||
        !element || element->node->type != DOM_ELEMENT ||
        owner->j != element->j)
        return js_throw_error(c, JS_ERR_TYPE,
                              "insertAdjacentElement requires an Element");
    if (string_arg(c, ac, av, 0, &position) != JS_OK)
        return JS_THROW;
    site = adjacent_site(c, owner, position, &parent, &reference);
    if (site < 0)
        return JS_THROW;
    if (!site) {
        *r = js_null();
        return JS_OK;
    }
    if (element->node == reference)
        reference = reference->next_sibling;
    if (!mutation_snapshot_nodes(element->node, snapshot, &count))
        return js_throw_error(c, JS_ERR_RANGE,
                              "element mutation is too large");
    if (!dom_insert_before(parent, element->node, reference))
        return js_throw_error(c, JS_ERR_ERROR,
                              "cannot insert adjacent element");
    if (mutation_queue_moved_nodes(owner->j, parent,
                                   snapshot, count) != JS_OK)
        return JS_THROW;
    owner->j->dirty = 1;
    *r = av[1];
    return JS_OK;
}

static int node_insert_adjacent_text(js_ctx *c, js_value t,
                                     int ac, js_value *av, js_value *r)
{
    struct hostref *owner = host(t, HOST_NODE);
    struct dom_node *parent, *reference, *text_node;
    js_value text;
    const char *position, *bytes;
    unsigned long length;
    int site;

    if (!owner || owner->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &position) != JS_OK ||
        js_to_string(c, ac > 1 ? av[1] : js_undefined(), &text) != JS_OK)
        return JS_THROW;
    site = adjacent_site(c, owner, position, &parent, &reference);
    if (site < 0)
        return JS_THROW;
    if (!site) {
        *r = js_undefined();
        return JS_OK;
    }
    bytes = js_string_bytes(text, &length);
    text_node = dom_create_text(owner->j->doc, bytes, length);
    if (!text_node ||
        !dom_insert_before(parent, text_node, reference))
        return js_throw_error(c, JS_ERR_ERROR,
                              "cannot insert adjacent text");
    if (mutation_child(owner->j, parent, text_node, 0,
                       text_node->prev_sibling,
                       text_node->next_sibling) != JS_OK)
        return JS_THROW;
    owner->j->dirty = 1;
    *r = js_undefined();
    return JS_OK;
}

static int node_insert_adjacent_html(js_ctx *c, js_value t,
                                     int ac, js_value *av, js_value *r)
{
    struct hostref *owner = host(t, HOST_NODE);
    struct dom_node *parent, *reference, *container, *node;
    const char *position, *html;
    int site;

    if (!owner || owner->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &position) != JS_OK ||
        string_arg(c, ac, av, 1, &html) != JS_OK)
        return JS_THROW;
    site = adjacent_site(c, owner, position, &parent, &reference);
    if (site < 0)
        return JS_THROW;
    if (!site) {
        *r = js_undefined();
        return JS_OK;
    }
    {
        struct dom_document *dst_doc = (parent && parent->doc) ? parent->doc :
            (owner->node->doc ? owner->node->doc : owner->j->doc);
        struct dom_node *ctx_node = (parent == owner->node) ? owner->node :
            (parent && parent->type == DOM_ELEMENT ? parent : 0);

        container = dom_create_element(dst_doc, "div", 3);
        if (!container ||
            !set_inner_html_ctx(owner->j, container, ctx_node, html))
            return js_throw_error(c, JS_ERR_ERROR,
                                  "cannot parse adjacent HTML");
    }
    while ((node = container->first_child) != 0) {
        struct mutation_node_snapshot snapshot[JSDOM_MUTATION_NODES_MAX];
        unsigned int count;

        if (!mutation_snapshot_nodes(node, snapshot, &count))
            return js_throw_error(c, JS_ERR_RANGE,
                                  "adjacent HTML is too large");
        if (!dom_insert_before(parent, node, reference))
            return js_throw_error(c, JS_ERR_ERROR,
                                  "cannot insert adjacent HTML");
        if (mutation_queue_moved_nodes(owner->j, parent,
                                       snapshot, count) != JS_OK)
            return JS_THROW;
    }
    mark_dom_dirty(owner->j);
    *r = js_undefined();
    return JS_OK;
}

static int node_outer_set(js_ctx *c, js_value t,
                          int ac, js_value *av, js_value *r)
{
    struct hostref *owner = host(t, HOST_NODE);
    struct dom_node *parent, *previous, *next;
    js_value args[2], ignored;

    if (!owner || owner->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Element receiver");
    if (!owner->node->parent) {
        *r = js_undefined();
        return JS_OK;
    }
    parent = owner->node->parent;
    previous = owner->node->prev_sibling;
    next = owner->node->next_sibling;
    args[0] = js_mkcstring(c, "beforebegin");
    args[1] = ac ? av[0] : js_undefined();
    if (node_insert_adjacent_html(c, t, 2, args, &ignored) != JS_OK)
        return JS_THROW;
    dom_remove_child(parent, owner->node);
    if (mutation_child(owner->j, parent, 0, owner->node,
                       previous, next) != JS_OK)
        return JS_THROW;
    mark_dom_dirty(owner->j);
    *r = js_undefined();
    return JS_OK;
}

static int node_clone(js_ctx *c, js_value t,
                      int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    struct dom_document *dst_doc;
    struct dom_node *copy;
    int deep = ac && js_to_boolean(av[0]);

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Node receiver");
    dst_doc = h->node->doc ? h->node->doc : h->j->doc;
    if (h->node->type == DOM_TEXT)
        copy = dom_create_text(dst_doc, h->node->text,
                               h->node->text_len);
    else if (h->node->type == DOM_COMMENT)
        copy = dom_create_comment(dst_doc, h->node->text,
                                  h->node->text_len);
    else if (deep)
        copy = clone_subtree_doc(h->j, dst_doc, h->node, 0);
    else if (h->node->type == DOM_FRAGMENT)
        copy = dom_create_fragment(dst_doc);
    else if (h->node->type == DOM_ELEMENT) {
        unsigned int i;

        copy = dom_create_element(dst_doc, h->node->tag, -1);
        if (copy) {
            if (!dom_set_namespace(copy, h->node->namespace_uri, -1))
                copy = 0;
        }
        if (copy) {
            for (i = 0; i < dom_attr_count(h->node); i++) {
                const struct dom_attr *attribute =
                    dom_attr_at(h->node, i);
                if (!attribute ||
                    (attribute->namespace_uri
                        ? !dom_set_attr_ns(copy,
                                           attribute->namespace_uri,
                                           attribute->name,
                                           attribute->value)
                        : !dom_set_attr_n(copy, attribute->name, -1,
                                          attribute->value,
                                          attribute->len))) {
                    copy = 0;
                    break;
                }
            }
        }
    } else {
        copy = 0;
    }
    if (!copy)
        return js_throw_error(c, JS_ERR_ERROR, "cannot clone node");
    *r = wrap_node(h->j, copy);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int node_contains(js_ctx *c, js_value t,
                         int ac, js_value *av, js_value *r)
{
    struct hostref *h = node_or_document(t);
    struct hostref *candidate =
        ac && !js_is_null(av[0]) ? node_or_document(av[0]) : 0;
    struct dom_node *base = receiver_node(h);
    struct dom_node *candidate_node = receiver_node(candidate);
    struct dom_node *cursor;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Node receiver");
    if (!candidate || candidate->j != h->j) {
        *r = js_bool(0);
        return JS_OK;
    }
    for (cursor = candidate_node; cursor && cursor != base;
         cursor = cursor->parent)
        ;
    *r = js_bool(cursor == base);
    return JS_OK;
}

static int node_is_same(js_ctx *c, js_value t,
                        int ac, js_value *av, js_value *r)
{
    struct hostref *left = node_or_document(t);
    struct hostref *right = ac ? node_or_document(av[0]) : 0;

    if (!left)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Node receiver");
    *r = js_bool(right && left->j == right->j &&
                 receiver_node(left) == receiver_node(right));
    return JS_OK;
}

static int dom_nodes_equal(const struct dom_node *left,
                           const struct dom_node *right,
                           unsigned int depth)
{
    const struct dom_node *left_child, *right_child;
    unsigned int index;

    if (left == right)
        return 1;
    if (!left || !right || depth > 128 ||
        left->type != right->type ||
        left->text_len != right->text_len ||
        (left->text_len &&
         memcmp(left->text, right->text, left->text_len)))
        return 0;
    if (left->type == DOM_ELEMENT) {
        if (strcmp(left->tag, right->tag) ||
            (!!left->namespace_uri != !!right->namespace_uri) ||
            (left->namespace_uri &&
             strcmp(left->namespace_uri, right->namespace_uri)) ||
            dom_attr_count(left) != dom_attr_count(right))
            return 0;
        for (index = 0; index < dom_attr_count(left); index++) {
            const struct dom_attr *attribute = dom_attr_at(left, index);
            const char *right_value = attribute
                ? dom_get_attr_ns(right, attribute->namespace_uri,
                                  attribute->local_name
                                      ? attribute->local_name
                                      : attribute->name)
                : 0;

            if (!attribute || !right_value ||
                strlen(right_value) != attribute->len ||
                memcmp(right_value, attribute->value, attribute->len))
                return 0;
        }
    }
    left_child = left->first_child;
    right_child = right->first_child;
    while (left_child && right_child) {
        if (!dom_nodes_equal(left_child, right_child, depth + 1))
            return 0;
        left_child = left_child->next_sibling;
        right_child = right_child->next_sibling;
    }
    return !left_child && !right_child;
}

static int node_is_equal(js_ctx *c, js_value t,
                         int ac, js_value *av, js_value *r)
{
    struct hostref *left = node_or_document(t);
    struct hostref *right = ac ? node_or_document(av[0]) : 0;

    if (!left)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Node receiver");
    *r = js_bool(right &&
                 dom_nodes_equal(receiver_node(left),
                                 receiver_node(right), 0));
    return JS_OK;
}

static int node_compare_position(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct hostref *left = node_or_document(t);
    struct hostref *right = ac ? node_or_document(av[0]) : 0;
    struct dom_node *left_node = receiver_node(left);
    struct dom_node *right_node = receiver_node(right);
    struct dom_node *left_path[129], *right_path[129], *cursor;
    unsigned int left_depth = 0, right_depth = 0, shared = 0;
    unsigned int result;

    if (!left || !right)
        return js_throw_error(c, JS_ERR_TYPE,
                              "compareDocumentPosition requires a Node");
    if (left_node == right_node) {
        *r = js_number(0);
        return JS_OK;
    }
    for (cursor = left_node; cursor && left_depth < 129;
         cursor = cursor->parent)
        left_path[left_depth++] = cursor;
    for (cursor = right_node; cursor && right_depth < 129;
         cursor = cursor->parent)
        right_path[right_depth++] = cursor;
    if (!left_depth || !right_depth ||
        left_path[left_depth - 1] != right_path[right_depth - 1]) {
        result = 1 | 32;
        result |= left_node->index < right_node->index ? 4 : 2;
        *r = js_number(result);
        return JS_OK;
    }
    while (shared < left_depth && shared < right_depth &&
           left_path[left_depth - 1 - shared] ==
           right_path[right_depth - 1 - shared])
        shared++;
    if (shared == left_depth) {
        *r = js_number(4 | 16);
        return JS_OK;
    }
    if (shared == right_depth) {
        *r = js_number(2 | 8);
        return JS_OK;
    }
    {
        struct dom_node *left_branch =
            left_path[left_depth - shared - 1];
        struct dom_node *right_branch =
            right_path[right_depth - shared - 1];
        struct dom_node *parent =
            left_path[left_depth - shared];

        for (cursor = parent->first_child; cursor;
             cursor = cursor->next_sibling)
            if (cursor == left_branch || cursor == right_branch)
                break;
        result = cursor == left_branch ? 4 : 2;
    }
    *r = js_number(result);
    return JS_OK;
}

static int normalize_subtree(struct jsdom *j, struct dom_node *parent,
                             unsigned int depth)
{
    struct dom_node *node, *next;

    if (!parent || depth > 128)
        return 0;
    for (node = parent->first_child; node; node = next) {
        next = node->next_sibling;
        if (node->type == DOM_TEXT) {
            if (!node->text_len) {
                struct dom_node *previous = node->prev_sibling;

                dom_remove_child(parent, node);
                if (mutation_child(j, parent, 0, node,
                                   previous, next) != JS_OK)
                    return 0;
                continue;
            }
            while (next && next->type == DOM_TEXT) {
                struct dom_node *removed = next;
                struct dom_node *after = removed->next_sibling;
                const char *old_value = node->text;

                if (!dom_text_append(node, removed->text,
                                     removed->text_len))
                    return 0;
                if (mutation_character_data(j, node, old_value) != JS_OK)
                    return 0;
                dom_remove_child(parent, removed);
                if (mutation_child(j, parent, 0, removed,
                                   node, after) != JS_OK)
                    return 0;
                next = after;
            }
        } else if ((node->type == DOM_ELEMENT ||
                    node->type == DOM_FRAGMENT) &&
                   !normalize_subtree(j, node, depth + 1)) {
            return 0;
        }
    }
    return 1;
}

static int node_normalize(js_ctx *c, js_value t,
                          int ac, js_value *av, js_value *r)
{
    struct hostref *h = node_or_document(t);
    (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Node receiver");
    if (!normalize_subtree(h->j, receiver_node(h), 0))
        return js_throw_error(c, JS_ERR_ERROR,
                              "cannot normalize node");
    h->j->dirty = 1;
    *r = js_undefined();
    return JS_OK;
}

static int node_matches(js_ctx *c, js_value t,
                        int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    struct css_stylesheet *ss;
    const char *selector;
    int matched;

    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &selector) != JS_OK)
        return JS_THROW;
    if (parse_dom_selector(c, selector, &ss) != JS_OK)
        return JS_THROW;
    dataset_sync_all(h->j);
    matched = selector_matches_ss(ss, h->node);
    css_free(ss);
    *r = js_bool(matched);
    return JS_OK;
}

static int node_closest(js_ctx *c, js_value t,
                        int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    struct css_stylesheet *ss;
    struct dom_node *node;
    const char *selector;

    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &selector) != JS_OK)
        return JS_THROW;
    if (parse_dom_selector(c, selector, &ss) != JS_OK)
        return JS_THROW;
    dataset_sync_all(h->j);
    for (node = h->node; node; node = dom_parent_element(node))
        if (selector_matches_ss(ss, node))
            break;
    css_free(ss);
    *r = wrap_node(h->j, node);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int node_query(js_ctx *c, js_value t, int ac, js_value *av,
                      js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    struct dom_node *match = 0;
    const char *sel;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    if (string_arg(c, ac, av, 0, &sel) != JS_OK)
        return JS_THROW;
    if (query_first(c, h->j, h->node, sel, &match) != JS_OK)
        return JS_THROW;
    *r = wrap_node(h->j, match);
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
/* dataset, classList and style */

static char *dataset_key(const char *attribute)
{
    const char *source;
    char *key;
    unsigned long used = 0, length;

    if (!attribute || strncmp(attribute, "data-", 5) != 0 ||
        !attribute[5])
        return 0;
    source = attribute + 5;
    length = strlen(source);
    key = (char *)malloc(length + 1);
    if (!key)
        return 0;
    while (*source) {
        if (*source == '-' && source[1] >= 'a' && source[1] <= 'z') {
            key[used++] = (char)(source[1] - ('a' - 'A'));
            source += 2;
        } else {
            key[used++] = *source++;
        }
    }
    key[used] = 0;
    return key;
}

static char *dataset_attr_name(const char *key)
{
    unsigned long len, out_len = 5, i;
    char *out;

    if (!key || !*key)
        return 0;
    len = strlen(key);
    for (i = 0; i < len; i++) {
        if (key[i] == '-' && key[i + 1] >= 'a' && key[i + 1] <= 'z')
            return 0;
        out_len += (key[i] >= 'A' && key[i] <= 'Z') ? 2u : 1u;
    }
    out = (char *)malloc(out_len + 1);
    if (!out)
        return 0;
    memcpy(out, "data-", 5);
    out_len = 5;
    for (i = 0; i < len; i++) {
        if (key[i] >= 'A' && key[i] <= 'Z') {
            out[out_len++] = '-';
            out[out_len++] = (char)(key[i] + ('a' - 'A'));
        } else {
            out[out_len++] = key[i];
        }
    }
    out[out_len] = 0;
    return out;
}

static void dataset_track_node(struct jsdom *j, struct dom_node *node)
{
    unsigned int i, ncap;
    struct dom_node **narr;

    if (!j || !node)
        return;
    for (i = 0; i < j->dataset_node_count; i++) {
        if (j->dataset_nodes[i] == node)
            return;
    }
    if (j->dataset_node_count >= j->dataset_node_cap) {
        ncap = j->dataset_node_cap ? j->dataset_node_cap * 2 : 16;
        narr = (struct dom_node **)realloc(j->dataset_nodes,
                                           ncap * sizeof(*narr));
        if (!narr)
            return;
        j->dataset_nodes = narr;
        j->dataset_node_cap = ncap;
    }
    j->dataset_nodes[j->dataset_node_count++] = node;
}

static int dataset_property_get(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    struct hostref *property = host(t, HOST_DATASET_PROPERTY);
    const char *value;
    (void)ac; (void)av;

    if (!property)
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid DOMStringMap property");
    value = dom_get_attr(property->node, (const char *)property->data);
    *r = value ? js_mkcstring(c, value) : js_undefined();
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int dataset_property_set(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    struct hostref *property = host(t, HOST_DATASET_PROPERTY);
    const char *value, *old;

    if (!property)
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid DOMStringMap property");
    if (string_arg(c, ac, av, 0, &value) != JS_OK)
        return JS_THROW;
    old = dom_get_attr(property->node, (const char *)property->data);
    if (!dom_set_attr(property->node, (const char *)property->data, value))
        return js_throw_error(c, JS_ERR_ERROR,
                              "cannot set data attribute");
    if (mutation_attribute(property->j, property->node,
                           (const char *)property->data, old) != JS_OK)
        return JS_THROW;
    mark_dom_dirty(property->j);
    *r = js_undefined();
    return JS_OK;
}

static int bind_native_to(js_ctx *c, js_native native,
                          const char *name, int nargs,
                          js_value receiver, js_value *bound)
{
    js_object *function = js_new_native(c, native, name, nargs);
    js_value binder;

    if (!function ||
        js_get(c, js_object_value(function), "bind", &binder) != JS_OK)
        return JS_THROW;
    return js_call(c, binder, js_object_value(function),
                   1, &receiver, bound);
}

static int dataset_bind_key(js_ctx *c, struct hostref *element,
                            js_object *dataset, js_object *bound,
                            const char *key, const char *attr_name)
{
    char *attribute_copy;
    js_object *holder;
    js_value holder_value, getter, setter;

    attribute_copy = dup_z(attr_name);
    if (!attribute_copy)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    holder = new_host_data(element->j, element->node,
                           HOST_DATASET_PROPERTY, attribute_copy, 0);
    if (!holder) {
        free(attribute_copy);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    holder_value = js_object_value(holder);
    if (bind_native_to(c, dataset_property_get, key, 0,
                       holder_value, &getter) != JS_OK ||
        bind_native_to(c, dataset_property_set, key, 1,
                       holder_value, &setter) != JS_OK ||
        js_define_accessor_value(c, dataset, key,
                                 getter, setter, 1) != JS_OK)
        return JS_THROW;
    if (bound &&
        js_define_enum(c, bound, key, js_bool(1)) != JS_OK)
        return JS_THROW;
    return JS_OK;
}

static int object_own_keys(js_ctx *c, js_value target, js_value *out_arr)
{
    js_value obj_ctor, keys_fn;

    if (js_get(c, js_global(c), "Object", &obj_ctor) != JS_OK ||
        js_get(c, obj_ctor, "keys", &keys_fn) != JS_OK ||
        js_call(c, keys_fn, obj_ctor, 1, &target, out_arr) != JS_OK ||
        !js_is_object(*out_arr))
        return 0;
    return 1;
}

static void dataset_on_attr_removed(struct jsdom *j, struct dom_node *node,
                                    const char *attr_name)
{
    js_ctx *c;
    js_value el_val, dataset, bound_val;
    char *key;

    if (!j || !node || !node->user || !attr_name ||
        strncmp(attr_name, "data-", 5) != 0)
        return;
    key = dataset_key(attr_name);
    if (!key)
        return;
    c = j->ctx;
    el_val = js_object_value((js_object *)node->user);
    if (js_get(c, el_val, "__dataset", &dataset) == JS_OK &&
        js_is_object(dataset)) {
        (void)js_delete(c, dataset, key);
        if (js_get(c, dataset, "__bound", &bound_val) == JS_OK &&
            js_is_object(bound_val))
            (void)js_delete(c, bound_val, key);
    }
    free(key);
}

static void dataset_sync_element(struct jsdom *j, struct dom_node *node)
{
    js_ctx *c;
    js_value el_val, dataset, bound_val, keys_arr;
    struct hostref *el_host;
    unsigned long len, i;

    if (!j || !node || !node->user || node->type != DOM_ELEMENT)
        return;
    c = j->ctx;
    el_val = js_object_value((js_object *)node->user);
    el_host = host(el_val, HOST_NODE);
    if (!el_host)
        return;
    if (js_get(c, el_val, "__dataset", &dataset) != JS_OK ||
        !js_is_object(dataset))
        return;
    if (js_get(c, dataset, "__bound", &bound_val) != JS_OK ||
        !js_is_object(bound_val))
        return;
    if (object_own_keys(c, bound_val, &keys_arr)) {
        len = js_array_length(keys_arr.u.obj);
        for (i = 0; i < len; i++) {
            js_value kval;
            const char *key, *old_attr;
            char *attr_name;

            if (js_array_get(c, keys_arr.u.obj, i, &kval) != JS_OK)
                continue;
            key = js_to_cstring(c, kval);
            if (!key || js_has_own(c, dataset, key))
                continue;
            attr_name = dataset_attr_name(key);
            if (attr_name) {
                old_attr = dom_get_attr(node, attr_name);
                if (old_attr) {
                    char *old_copy = dup_z(old_attr);
                    if (dom_remove_attr(node, attr_name)) {
                        (void)mutation_attribute(j, node, attr_name,
                                                 old_copy);
                        mark_dom_dirty(j);
                    }
                    free(old_copy);
                }
                free(attr_name);
            }
            (void)js_delete(c, bound_val, key);
        }
    }
    if (object_own_keys(c, dataset, &keys_arr)) {
        len = js_array_length(keys_arr.u.obj);
        for (i = 0; i < len; i++) {
            js_value kval, vval, str_val;
            const char *key, *sval, *old_attr;
            char *attr_name;

            if (js_array_get(c, keys_arr.u.obj, i, &kval) != JS_OK)
                continue;
            key = js_to_cstring(c, kval);
            if (!key || js_has_own(c, bound_val, key))
                continue;
            attr_name = dataset_attr_name(key);
            if (!attr_name)
                continue;
            if (js_get(c, dataset, key, &vval) != JS_OK ||
                js_to_string(c, vval, &str_val) != JS_OK) {
                free(attr_name);
                continue;
            }
            sval = js_to_cstring(c, str_val);
            if (!sval)
                sval = "";
            old_attr = dom_get_attr(node, attr_name);
            if (dom_set_attr(node, attr_name, sval)) {
                (void)mutation_attribute(j, node, attr_name, old_attr);
                mark_dom_dirty(j);
                (void)js_delete(c, dataset, key);
                (void)dataset_bind_key(c, el_host, dataset.u.obj,
                                       bound_val.u.obj, key, attr_name);
            }
            free(attr_name);
        }
    }
}

static void dataset_sync_all(struct jsdom *j)
{
    unsigned int i;

    if (!j)
        return;
    for (i = 0; i < j->dataset_node_count; i++)
        dataset_sync_element(j, j->dataset_nodes[i]);
}

static int dataset_refresh(js_ctx *c, struct hostref *element,
                           js_object *dataset)
{
    js_value bound_val = js_undefined(), keys_arr;
    js_object *bound_obj = 0;
    unsigned int i;

    dataset_sync_element(element->j, element->node);
    if (js_get(c, js_object_value(dataset), "__bound", &bound_val) != JS_OK)
        return JS_THROW;
    if (js_is_object(bound_val)) {
        bound_obj = bound_val.u.obj;
        if (object_own_keys(c, bound_val, &keys_arr)) {
            unsigned long len = js_array_length(keys_arr.u.obj), k;

            for (k = 0; k < len; k++) {
                js_value kval;
                const char *key;
                char *attr_name;

                if (js_array_get(c, keys_arr.u.obj, k, &kval) != JS_OK)
                    continue;
                key = js_to_cstring(c, kval);
                if (!key)
                    continue;
                attr_name = dataset_attr_name(key);
                if (!attr_name || !dom_get_attr(element->node, attr_name)) {
                    (void)js_delete(c, js_object_value(dataset), key);
                    (void)js_delete(c, bound_val, key);
                }
                free(attr_name);
            }
        }
    }
    for (i = 0; i < dom_attr_count(element->node); i++) {
        const struct dom_attr *attribute = dom_attr_at(element->node, i);
        char *key = dataset_key(attribute ? attribute->name : 0);

        if (!key)
            continue;
        if (js_has_own(c, js_object_value(dataset), key)) {
            free(key);
            continue;
        }
        if (dataset_bind_key(c, element, dataset, bound_obj,
                             key, attribute->name) != JS_OK) {
            free(key);
            return JS_THROW;
        }
        free(key);
    }
    return JS_OK;
}

static int node_dataset_get(js_ctx *c, js_value t,
                            int ac, js_value *av, js_value *r)
{
    struct hostref *element = host(t, HOST_NODE);
    js_value dataset;
    (void)ac; (void)av;

    if (!element || element->node->type != DOM_ELEMENT) {
        *r = js_undefined();
        return JS_OK;
    }
    if (js_get(c, t, "__dataset", &dataset) != JS_OK)
        return JS_THROW;
    if (dataset.type == JS_UNDEFINED) {
        js_object *object = js_new_object(c);
        js_object *bound = js_new_object(c);

        if (!object || !bound)
            return JS_THROW;
        dataset = js_object_value(object);
        if (js_define(c, object, "__bound", js_object_value(bound)) != JS_OK ||
            js_define(c, t.u.obj, "__dataset", dataset) != JS_OK)
            return JS_THROW;
        dataset_track_node(element->j, element->node);
    }
    if (dataset_refresh(c, element, dataset.u.obj) != JS_OK)
        return JS_THROW;
    *r = dataset;
    return JS_OK;
}

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

static int classlist_sync(js_ctx *c, struct dom_node *node,
                          js_object *list_obj)
{
    const char *value;
    js_value synced_val;
    unsigned int old_len = 0, count = 0, i;
    char key[16];

    if (!c || !node || !list_obj)
        return JS_OK;
    if (js_get(c, js_object_value(list_obj), "__syncedLen",
               &synced_val) == JS_OK &&
        js_is_number(synced_val) && synced_val.u.num > 0)
        old_len = (unsigned int)synced_val.u.num;
    value = dom_get_attr(node, "class");
    while (value && *value) {
        const char *start;
        js_value tok_str;

        while (*value == ' ' || *value == '\t' ||
               *value == '\r' || *value == '\n')
            value++;
        if (!*value)
            break;
        start = value;
        while (*value && *value != ' ' && *value != '\t' &&
               *value != '\r' && *value != '\n')
            value++;
        snprintf(key, sizeof(key), "%u", count);
        tok_str = js_mkstring(c, start, (unsigned long)(value - start));
        if (js_fatal(c))
            return JS_THROW;
        if (count < old_len) {
            if (js_set(c, js_object_value(list_obj), key, tok_str) != JS_OK)
                return JS_THROW;
        } else {
            if (js_define_enum(c, list_obj, key, tok_str) != JS_OK)
                return JS_THROW;
        }
        count++;
    }
    for (i = count; i < old_len; i++) {
        snprintf(key, sizeof(key), "%u", i);
        (void)js_delete(c, js_object_value(list_obj), key);
    }
    if (js_set(c, js_object_value(list_obj), "__syncedLen",
               js_number((double)count)) != JS_OK)
        return JS_THROW;
    return JS_OK;
}

static void classlist_sync_node(struct jsdom *j, struct dom_node *node)
{
    js_value list;

    if (!j || !node || !node->user)
        return;
    if (js_get(j->ctx, js_object_value((js_object *)node->user),
               "__classList", &list) != JS_OK)
        return;
    if (host(list, HOST_CLASSLIST))
        (void)classlist_sync(j->ctx, node, list.u.obj);
}

static int class_change(struct hostref *h, const char *tok, int mode, int *now)
{
    const char *old, *old_attr;
    char *out;
    unsigned long used = 0, cap;
    const char *p;
    int had;

    if (!*tok || strlen(tok) > 128 || strchr(tok, ' ') || strchr(tok, '\t') ||
        strchr(tok, '\r') || strchr(tok, '\n'))
        return 0;
    old_attr = dom_get_attr(h->node, "class");
    old = old_attr;
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
    if (mutation_attribute(h->j, h->node, "class", old_attr) != JS_OK)
        return 0;
    classlist_sync_node(h->j, h->node);
    mark_dom_dirty(h->j);
    if (now) *now = mode == 1;
    return 1;
}

static int class_method(js_ctx *c, js_value t, int ac, js_value *av,
                        js_value *r, int mode)
{
    struct hostref *h = host(t, HOST_CLASSLIST);
    const char *token;
    int i;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal DOMTokenList receiver");
    for (i = 0; i < ac; i++) {
        if (string_arg(c, ac, av, i, &token) != JS_OK)
            return JS_THROW;
        if (!*token || strlen(token) > 128 ||
            strchr(token, ' ') || strchr(token, '\t') ||
            strchr(token, '\r') || strchr(token, '\n'))
            return js_throw_error(c, JS_ERR_ERROR,
                                  "invalid class token");
    }
    for (i = 0; i < ac; i++) {
        if (string_arg(c, ac, av, i, &token) != JS_OK)
            return JS_THROW;
        if (!class_change(h, token, mode, 0))
            return js_throw_error(c, JS_ERR_ERROR,
                                  "cannot update class token");
    }
    *r = js_undefined();
    return JS_OK;
}

static int class_add(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)
{ return class_method(c, t, ac, av, r, 1); }
static int class_remove(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)
{ return class_method(c, t, ac, av, r, 0); }
static int class_toggle(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_CLASSLIST);
    const char *token;
    int now = 0;
    int mode;

    if (!h || string_arg(c, ac, av, 0, &token) != JS_OK)
        return h ? JS_THROW :
            js_throw_error(c, JS_ERR_TYPE,
                           "illegal DOMTokenList receiver");
    mode = ac > 1 ? (js_to_boolean(av[1]) ? 1 : 0) : 2;
    if (!class_change(h, token, mode, &now))
        return js_throw_error(c, JS_ERR_ERROR, "invalid class token");
    *r = js_bool(now);
    return JS_OK;
}
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

static int class_value_get(js_ctx *c, js_value t,
                           int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_CLASSLIST);
    const char *value;
    (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal DOMTokenList receiver");
    value = dom_get_attr(h->node, "class");
    *r = js_mkcstring(c, value ? value : "");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int class_value_set(js_ctx *c, js_value t,
                           int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_CLASSLIST);
    const char *value, *old;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal DOMTokenList receiver");
    if (string_arg(c, ac, av, 0, &value) != JS_OK)
        return JS_THROW;
    old = dom_get_attr(h->node, "class");
    if (strlen(value) >= JSDOM_CLASS_MAX ||
        !dom_set_attr(h->node, "class", value))
        return js_throw_error(c, JS_ERR_RANGE,
                              "class value is too large");
    if (mutation_attribute(h->j, h->node, "class", old) != JS_OK)
        return JS_THROW;
    classlist_sync_node(h->j, h->node);
    mark_dom_dirty(h->j);
    *r = js_undefined();
    return JS_OK;
}

static unsigned int class_token_count(const char *value)
{
    unsigned int count = 0;

    while (value && *value) {
        while (*value == ' ' || *value == '\t' ||
               *value == '\r' || *value == '\n')
            value++;
        if (!*value)
            break;
        count++;
        while (*value && *value != ' ' && *value != '\t' &&
               *value != '\r' && *value != '\n')
            value++;
    }
    return count;
}

static int class_length_get(js_ctx *c, js_value t,
                            int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_CLASSLIST);
    const char *value;
    (void)c; (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal DOMTokenList receiver");
    value = dom_get_attr(h->node, "class");
    *r = js_number(class_token_count(value ? value : ""));
    return JS_OK;
}

static int class_item(js_ctx *c, js_value t,
                      int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_CLASSLIST);
    const char *value, *start;
    double requested;
    unsigned int index = 0;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal DOMTokenList receiver");
    if (js_to_integer(c, ac ? av[0] : js_undefined(), &requested) != JS_OK)
        return JS_THROW;
    value = dom_get_attr(h->node, "class");
    if (!value)
        value = "";
    while (*value) {
        while (*value == ' ' || *value == '\t' ||
               *value == '\r' || *value == '\n')
            value++;
        if (!*value)
            break;
        start = value;
        while (*value && *value != ' ' && *value != '\t' &&
               *value != '\r' && *value != '\n')
            value++;
        if (requested == (double)index) {
            *r = js_mkstring(c, start,
                             (unsigned long)(value - start));
            return js_fatal(c) ? JS_THROW : JS_OK;
        }
        index++;
    }
    *r = js_null();
    return JS_OK;
}

static int class_replace(js_ctx *c, js_value t,
                         int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_CLASSLIST);
    const char *old_token, *new_token, *value;

    if (!h ||
        string_arg(c, ac, av, 0, &old_token) != JS_OK ||
        string_arg(c, ac, av, 1, &new_token) != JS_OK)
        return h ? JS_THROW :
            js_throw_error(c, JS_ERR_TYPE,
                           "illegal DOMTokenList receiver");
    if (!*old_token || !*new_token ||
        strlen(old_token) > 128 || strlen(new_token) > 128 ||
        strchr(old_token, ' ') || strchr(new_token, ' ') ||
        strchr(old_token, '\t') || strchr(new_token, '\t') ||
        strchr(old_token, '\r') || strchr(new_token, '\r') ||
        strchr(old_token, '\n') || strchr(new_token, '\n'))
        return js_throw_error(c, JS_ERR_ERROR, "invalid class token");
    value = dom_get_attr(h->node, "class");
    if (!value || !token_present(value, old_token)) {
        *r = js_bool(0);
        return JS_OK;
    }
    if (strcmp(old_token, new_token)) {
        if (!class_change(h, old_token, 0, 0) ||
            !class_change(h, new_token, 1, 0))
            return js_throw_error(c, JS_ERR_ERROR,
                                  "cannot replace class token");
    }
    *r = js_bool(1);
    return JS_OK;
}

static int class_to_string(js_ctx *c, js_value t,
                           int ac, js_value *av, js_value *r)
{
    return class_value_get(c, t, ac, av, r);
}

static int node_classlist_get(js_ctx *c, js_value t, int ac, js_value *av,
                              js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    js_value list;
    (void)ac; (void)av;

    if (!h || h->node->type != DOM_ELEMENT) {
        *r = js_undefined();
        return JS_OK;
    }
    if (js_get(c, t, "__classList", &list) != JS_OK)
        return JS_THROW;
    if (list.type == JS_UNDEFINED) {
        js_object *o = new_host(h->j, h->node, HOST_CLASSLIST, h->j->class_proto);

        if (!o)
            return JS_THROW;
        if (js_define(c, o, "__syncedLen", js_number(0)) != JS_OK)
            return JS_THROW;
        list = js_object_value(o);
        if (js_define(c, t.u.obj, "__classList", list) != JS_OK)
            return JS_THROW;
    }
    if (classlist_sync(c, h->node, list.u.obj) != JS_OK)
        return JS_THROW;
    *r = list;
    return JS_OK;
}

static int style_value_arg(js_ctx *c, int ac, js_value *av, int idx,
                           const char **out)
{
    if (idx >= ac || js_is_null(av[idx])) {
        *out = "";
        return JS_OK;
    }
    return string_arg(c, ac, av, idx, out);
}

static int style_prop_name_eq(const char *a, unsigned long an, const char *prop)
{
    unsigned long pn = strlen(prop);

    if (an != pn)
        return 0;
    if (pn >= 2 && prop[0] == '-' && prop[1] == '-')
        return memcmp(a, prop, pn) == 0;
    return ci_eq_n(a, an, prop, pn);
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
    const char *v, *old;
    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal style receiver");
    if (style_value_arg(c, ac, av, 0, &v) != JS_OK) return JS_THROW;
    old = dom_get_attr(h->node, "style");
    if (strlen(v) >= JSDOM_STYLE_MAX ||
        !dom_set_attr(h->node, "style", v))
        return js_throw_error(c, JS_ERR_ERROR, "style is too large");
    if (mutation_attribute(h->j, h->node, "style", old) != JS_OK)
        return JS_THROW;
    mark_dom_dirty(h->j);
    *r = js_undefined();
    return JS_OK;
}

static int style_next_declaration(const char **cursor,
                                  const char **name,
                                  unsigned long *name_length,
                                  const char **value,
                                  unsigned long *value_length,
                                  int *important)
{
    const char *p = *cursor;
    const char *name_start, *name_end, *value_start, *value_end;
    const char *word_end, *word_start, *bang;
    int quote, paren;

again:
    while (*p == ' ' || *p == '\t' || *p == '\r' ||
           *p == '\n' || *p == ';')
        p++;
    if (!*p) {
        *cursor = p;
        return 0;
    }
    name_start = p;
    while (*p && *p != ':' && *p != ';')
        p++;
    name_end = p;
    while (name_end > name_start &&
           (name_end[-1] == ' ' || name_end[-1] == '\t' ||
            name_end[-1] == '\r' || name_end[-1] == '\n'))
        name_end--;
    while (name_start < name_end &&
           (*name_start == ' ' || *name_start == '\t' ||
            *name_start == '\r' || *name_start == '\n'))
        name_start++;
    if (*p != ':') {
        while (*p && *p != ';')
            p++;
        p = *p ? p + 1 : p;
        *cursor = p;
        goto again;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    value_start = p;
    quote = 0;
    paren = 0;
    while (*p) {
        char ch = *p;
        if (quote) {
            if (ch == '\\' && p[1]) {
                p += 2;
                continue;
            }
            if (ch == quote)
                quote = 0;
            p++;
            continue;
        }
        if (ch == '"' || ch == '\'')
            quote = ch;
        else if (ch == '(')
            paren++;
        else if (ch == ')' && paren > 0)
            paren--;
        else if (ch == ';' && paren == 0)
            break;
        p++;
    }
    value_end = p;
    while (value_end > value_start &&
           (value_end[-1] == ' ' || value_end[-1] == '\t' ||
            value_end[-1] == '\r' || value_end[-1] == '\n'))
        value_end--;
    *important = 0;
    word_end = value_end;
    word_start = word_end;
    while (word_start > value_start &&
           ((word_start[-1] >= 'A' && word_start[-1] <= 'Z') ||
            (word_start[-1] >= 'a' && word_start[-1] <= 'z')))
        word_start--;
    if (ci_eq_n(word_start, (unsigned long)(word_end - word_start),
                "important", 9)) {
        bang = word_start;
        while (bang > value_start &&
               (bang[-1] == ' ' || bang[-1] == '\t' ||
                bang[-1] == '\r' || bang[-1] == '\n'))
            bang--;
        if (bang > value_start && bang[-1] == '!') {
            value_end = bang - 1;
            while (value_end > value_start &&
                   (value_end[-1] == ' ' || value_end[-1] == '\t' ||
                    value_end[-1] == '\r' || value_end[-1] == '\n'))
                value_end--;
            *important = 1;
        }
    }
    *cursor = *p ? p + 1 : p;
    *name = name_start;
    *name_length = (unsigned long)(name_end - name_start);
    *value = value_start;
    *value_length = (unsigned long)(value_end - value_start);
    if (!*name_length)
        goto again;
    return 1;
}

static char *style_property_copy(const char *css, const char *property,
                                 int *priority)
{
    const char *cursor = css ? css : "";
    const char *name, *value;
    unsigned long name_length, value_length;
    char *found = 0;
    int important;

    if (priority)
        *priority = 0;
    while (style_next_declaration(&cursor, &name, &name_length,
                                  &value, &value_length, &important)) {
        if (!style_prop_name_eq(name, name_length, property))
            continue;
        free(found);
        found = dup_n(value, value_length);
        if (priority)
            *priority = important;
    }
    return found;
}

static int style_append_bytes(char *output, unsigned long *used,
                              const char *bytes, unsigned long length)
{
    if (length > JSDOM_STYLE_MAX - 1 - *used)
        return 0;
    memcpy(output + *used, bytes, length);
    *used += length;
    output[*used] = 0;
    return 1;
}

static int style_rewrite_property(struct hostref *h, const char *property,
                                  const char *replacement, int important)
{
    const char *css = dom_get_attr(h->node, "style");
    const char *cursor = css ? css : "";
    const char *name, *value;
    unsigned long name_length, value_length, used = 0;
    char *output;
    int old_important;

    output = (char *)malloc(JSDOM_STYLE_MAX);
    if (!output)
        return 0;
    output[0] = 0;
    while (style_next_declaration(&cursor, &name, &name_length,
                                  &value, &value_length,
                                  &old_important)) {
        if (style_prop_name_eq(name, name_length, property))
            continue;
        if (!style_append_bytes(output, &used, name, name_length) ||
            !style_append_bytes(output, &used, ":", 1) ||
            !style_append_bytes(output, &used, value, value_length) ||
            (old_important &&
             !style_append_bytes(output, &used, " !important", 11)) ||
            !style_append_bytes(output, &used, ";", 1)) {
            free(output);
            return 0;
        }
    }
    if (replacement && *replacement) {
        if (!style_append_bytes(output, &used, property, strlen(property)) ||
            !style_append_bytes(output, &used, ":", 1) ||
            !style_append_bytes(output, &used,
                                replacement, strlen(replacement)) ||
            (important &&
             !style_append_bytes(output, &used, " !important", 11)) ||
            !style_append_bytes(output, &used, ";", 1)) {
            free(output);
            return 0;
        }
    }
    if (!dom_set_attr(h->node, "style", output)) {
        free(output);
        return 0;
    }
    free(output);
    if (mutation_attribute(h->j, h->node, "style", css) != JS_OK)
        return 0;
    mark_dom_dirty(h->j);
    return 1;
}

static int computed_style_prop_get(js_ctx *c, js_value t, const char *prop,
                                   js_value *r);

static int style_prop_set(js_ctx *c, js_value t, int ac, js_value *av,
                          js_value *r, const char *prop)
{
    struct hostref *h = host(t, HOST_STYLE);
    const char *v;

    if (!h) {
        if (host(t, HOST_COMPUTED_STYLE))
            return throw_dom_exception(c, "NoModificationAllowedError",
                                       "computed style is read-only");
        return js_throw_error(c, JS_ERR_TYPE, "illegal style receiver");
    }
    if (style_value_arg(c, ac, av, 0, &v) != JS_OK) return JS_THROW;
    if (!style_rewrite_property(h, prop, v, 0))
        return js_throw_error(c, JS_ERR_ERROR, "cannot set style");
    *r = js_undefined();
    return JS_OK;
}

static int style_prop_get(js_ctx *c, js_value t, const char *prop, js_value *r)
{
    struct hostref *h = host(t, HOST_STYLE);
    const char *css;
    char *value;

    if (!h) {
        if (host(t, HOST_COMPUTED_STYLE))
            return computed_style_prop_get(c, t, prop, r);
        return js_throw_error(c, JS_ERR_TYPE, "illegal style receiver");
    }
    css = dom_get_attr(h->node, "style");
    value = style_property_copy(css, prop, 0);
    *r = js_mkcstring(c, value ? value : "");
    free(value);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int style_get_property_value(js_ctx *c, js_value t,
                                    int ac, js_value *av, js_value *r)
{
    const char *property;

    if (string_arg(c, ac, av, 0, &property) != JS_OK)
        return JS_THROW;
    return style_prop_get(c, t, property, r);
}

static int style_set_property(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_STYLE);
    const char *property, *value, *priority = "";

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal style receiver");
    if (string_arg(c, ac, av, 0, &property) != JS_OK ||
        style_value_arg(c, ac, av, 1, &value) != JS_OK ||
        (ac > 2 && !js_is_null(av[2]) &&
         string_arg(c, ac, av, 2, &priority) != JS_OK))
        return JS_THROW;
    if (!*property || strlen(property) > 128 ||
        strchr(property, ':') || strchr(property, ';'))
        return js_throw_error(c, JS_ERR_ERROR,
                              "invalid CSS property name");
    if (*priority && !ci_eq_n(priority, strlen(priority),
                              "important", 9)) {
        *r = js_undefined();
        return JS_OK;
    }
    if (!style_rewrite_property(h, property, value, *priority != 0))
        return js_throw_error(c, JS_ERR_RANGE,
                              "style declaration is too large");
    *r = js_undefined();
    return JS_OK;
}

static int style_remove_property(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_STYLE);
    const char *property, *css;
    char *old;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal style receiver");
    if (string_arg(c, ac, av, 0, &property) != JS_OK)
        return JS_THROW;
    css = dom_get_attr(h->node, "style");
    old = style_property_copy(css, property, 0);
    if (!style_rewrite_property(h, property, 0, 0)) {
        free(old);
        return js_throw_error(c, JS_ERR_ERROR,
                              "cannot remove style property");
    }
    *r = js_mkcstring(c, old ? old : "");
    free(old);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int style_get_property_priority(js_ctx *c, js_value t,
                                       int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_STYLE);
    const char *property, *css;
    char *value;
    int important = 0;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal style receiver");
    if (string_arg(c, ac, av, 0, &property) != JS_OK)
        return JS_THROW;
    css = dom_get_attr(h->node, "style");
    value = style_property_copy(css, property, &important);
    *r = js_mkcstring(c, value && important ? "important" : "");
    free(value);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int style_length_get(js_ctx *c, js_value t,
                            int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_STYLE);
    const char *cursor, *name, *value;
    unsigned long name_length, value_length, count = 0;
    int important;
    (void)c; (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal style receiver");
    cursor = dom_get_attr(h->node, "style");
    if (!cursor)
        cursor = "";
    while (style_next_declaration(&cursor, &name, &name_length,
                                  &value, &value_length, &important))
        count++;
    *r = js_number(count);
    return JS_OK;
}

static int style_item(js_ctx *c, js_value t,
                      int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_STYLE);
    const char *cursor, *name, *value;
    unsigned long name_length, value_length, index = 0;
    double requested;
    int important;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal style receiver");
    if (js_to_integer(c, ac ? av[0] : js_undefined(), &requested) != JS_OK)
        return JS_THROW;
    cursor = dom_get_attr(h->node, "style");
    if (!cursor)
        cursor = "";
    while (style_next_declaration(&cursor, &name, &name_length,
                                  &value, &value_length, &important)) {
        if (requested == (double)index) {
            *r = js_mkstring(c, name, name_length);
            return js_fatal(c) ? JS_THROW : JS_OK;
        }
        index++;
    }
    *r = js_mkcstring(c, "");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

#define JSDOM_CSS_PROPS(X) \
    X(display,               "display",               "display") \
    X(position,              "position",              "position") \
    X(css_float,             "cssFloat",              "float") \
    X(float_prop,            "float",                 "float") \
    X(clear,                 "clear",                 "clear") \
    X(box_sizing,            "boxSizing",             "box-sizing") \
    X(width,                 "width",                 "width") \
    X(height,                "height",                "height") \
    X(min_width,             "minWidth",              "min-width") \
    X(max_width,             "maxWidth",              "max-width") \
    X(min_height,            "minHeight",             "min-height") \
    X(max_height,            "maxHeight",             "max-height") \
    X(top,                   "top",                   "top") \
    X(right,                 "right",                 "right") \
    X(bottom,                "bottom",                "bottom") \
    X(left,                  "left",                  "left") \
    X(margin,                "margin",                "margin") \
    X(margin_top,            "marginTop",             "margin-top") \
    X(margin_right,          "marginRight",           "margin-right") \
    X(margin_bottom,         "marginBottom",          "margin-bottom") \
    X(margin_left,           "marginLeft",            "margin-left") \
    X(padding,               "padding",               "padding") \
    X(padding_top,           "paddingTop",            "padding-top") \
    X(padding_right,         "paddingRight",          "padding-right") \
    X(padding_bottom,        "paddingBottom",         "padding-bottom") \
    X(padding_left,          "paddingLeft",           "padding-left") \
    X(border,                "border",                "border") \
    X(border_width,          "borderWidth",           "border-width") \
    X(border_style,          "borderStyle",           "border-style") \
    X(border_color,          "borderColor",           "border-color") \
    X(border_top,            "borderTop",             "border-top") \
    X(border_right,          "borderRight",           "border-right") \
    X(border_bottom,         "borderBottom",          "border-bottom") \
    X(border_left,           "borderLeft",            "border-left") \
    X(border_top_width,      "borderTopWidth",        "border-top-width") \
    X(border_right_width,    "borderRightWidth",      "border-right-width") \
    X(border_bottom_width,   "borderBottomWidth",     "border-bottom-width") \
    X(border_left_width,     "borderLeftWidth",       "border-left-width") \
    X(border_top_style,      "borderTopStyle",        "border-top-style") \
    X(border_right_style,    "borderRightStyle",      "border-right-style") \
    X(border_bottom_style,   "borderBottomStyle",     "border-bottom-style") \
    X(border_left_style,     "borderLeftStyle",       "border-left-style") \
    X(border_top_color,      "borderTopColor",        "border-top-color") \
    X(border_right_color,    "borderRightColor",      "border-right-color") \
    X(border_bottom_color,   "borderBottomColor",     "border-bottom-color") \
    X(border_left_color,     "borderLeftColor",       "border-left-color") \
    X(border_radius,         "borderRadius",          "border-radius") \
    X(border_tl_radius,      "borderTopLeftRadius",   "border-top-left-radius") \
    X(border_tr_radius,      "borderTopRightRadius",  "border-top-right-radius") \
    X(border_br_radius,      "borderBottomRightRadius","border-bottom-right-radius") \
    X(border_bl_radius,      "borderBottomLeftRadius","border-bottom-left-radius") \
    X(font,                  "font",                  "font") \
    X(font_family,           "fontFamily",            "font-family") \
    X(font_size,             "fontSize",              "font-size") \
    X(font_weight,           "fontWeight",            "font-weight") \
    X(font_style,            "fontStyle",             "font-style") \
    X(color,                 "color",                 "color") \
    X(color_scheme,          "colorScheme",           "color-scheme") \
    X(opacity,               "opacity",               "opacity") \
    X(background,            "background",            "background") \
    X(background_color,      "backgroundColor",       "background-color") \
    X(background_image,      "backgroundImage",       "background-image") \
    X(box_shadow,            "boxShadow",             "box-shadow") \
    X(text_align,            "textAlign",             "text-align") \
    X(text_decoration,       "textDecoration",        "text-decoration") \
    X(text_indent,           "textIndent",            "text-indent") \
    X(text_transform,        "textTransform",         "text-transform") \
    X(text_overflow,         "textOverflow",          "text-overflow") \
    X(letter_spacing,        "letterSpacing",         "letter-spacing") \
    X(word_spacing,          "wordSpacing",           "word-spacing") \
    X(word_break,            "wordBreak",             "word-break") \
    X(table_layout,          "tableLayout",           "table-layout") \
    X(vertical_align,        "verticalAlign",         "vertical-align") \
    X(line_height,           "lineHeight",            "line-height") \
    X(list_style,            "listStyle",             "list-style") \
    X(list_style_type,       "listStyleType",         "list-style-type") \
    X(list_style_position,   "listStylePosition",     "list-style-position") \
    X(white_space,           "whiteSpace",            "white-space") \
    X(overflow,              "overflow",              "overflow") \
    X(overflow_x,            "overflowX",             "overflow-x") \
    X(overflow_y,            "overflowY",             "overflow-y") \
    X(visibility,            "visibility",            "visibility") \
    X(border_collapse,       "borderCollapse",        "border-collapse") \
    X(border_spacing,        "borderSpacing",         "border-spacing") \
    X(z_index,               "zIndex",                "z-index") \
    X(flex,                  "flex",                  "flex") \
    X(flex_flow,             "flexFlow",              "flex-flow") \
    X(flex_direction,        "flexDirection",         "flex-direction") \
    X(flex_wrap,             "flexWrap",              "flex-wrap") \
    X(justify_content,       "justifyContent",        "justify-content") \
    X(align_items,           "alignItems",            "align-items") \
    X(align_content,         "alignContent",          "align-content") \
    X(align_self,            "alignSelf",             "align-self") \
    X(order,                 "order",                 "order") \
    X(flex_grow,             "flexGrow",              "flex-grow") \
    X(flex_shrink,           "flexShrink",            "flex-shrink") \
    X(flex_basis,            "flexBasis",             "flex-basis") \
    X(gap,                   "gap",                   "gap") \
    X(row_gap,               "rowGap",                "row-gap") \
    X(column_gap,            "columnGap",             "column-gap") \
    X(grid_template_columns, "gridTemplateColumns",   "grid-template-columns") \
    X(grid_template_rows,    "gridTemplateRows",      "grid-template-rows") \
    X(grid_column,           "gridColumn",            "grid-column") \
    X(grid_column_start,     "gridColumnStart",       "grid-column-start") \
    X(grid_column_end,       "gridColumnEnd",         "grid-column-end") \
    X(grid_row,              "gridRow",               "grid-row") \
    X(grid_row_start,        "gridRowStart",          "grid-row-start") \
    X(grid_row_end,          "gridRowEnd",            "grid-row-end") \
    X(grid_area,             "gridArea",              "grid-area") \
    X(transform,             "transform",             "transform") \
    X(transform_origin,      "transformOrigin",       "transform-origin") \
    X(transition,            "transition",            "transition") \
    X(animation,             "animation",             "animation") \
    X(pointer_events,        "pointerEvents",         "pointer-events") \
    X(cursor,                "cursor",                "cursor") \
    X(user_select,           "userSelect",            "user-select") \
    X(outline,               "outline",               "outline") \
    X(outline_width,         "outlineWidth",          "outline-width") \
    X(outline_style,         "outlineStyle",          "outline-style") \
    X(outline_color,         "outlineColor",          "outline-color") \
    X(content,               "content",               "content") \
    X(fill,                  "fill",                  "fill") \
    X(stroke,                "stroke",                "stroke") \
    X(stroke_width,          "strokeWidth",           "stroke-width")

#define STYLE_ACCESSORS(id, camel, prop)                                     \
static int style_##id##_get(js_ctx *c, js_value t, int ac, js_value *av,     \
                            js_value *r)                                     \
{ (void)ac; (void)av; return style_prop_get(c, t, prop, r); }                \
static int style_##id##_set(js_ctx *c, js_value t, int ac, js_value *av,     \
                            js_value *r)                                     \
{ return style_prop_set(c, t, ac, av, r, prop); }

JSDOM_CSS_PROPS(STYLE_ACCESSORS)

static int register_css_decl_accessors(js_ctx *c, js_object *proto)
{
#define REG_STYLE_ACC(id, camel, prop)                                       \
    if (js_define_accessor(c, proto, camel,                                  \
                           style_##id##_get, style_##id##_set, 1) != JS_OK)  \
        return 0;                                                            \
    if (strcmp(camel, prop) != 0 && strcmp(camel, "cssFloat") != 0 &&        \
        js_define_accessor(c, proto, prop,                                   \
                           style_##id##_get, style_##id##_set, 1) != JS_OK)  \
        return 0;
    JSDOM_CSS_PROPS(REG_STYLE_ACC)
#undef REG_STYLE_ACC
    return 1;
}

static int node_style_get(js_ctx *c, js_value t, int ac, js_value *av,
                          js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    js_value st;
    (void)ac; (void)av;

    if (!h || h->node->type != DOM_ELEMENT) {
        *r = js_undefined();
        return JS_OK;
    }
    if (js_get(c, t, "__style", &st) != JS_OK)
        return JS_THROW;
    if (st.type == JS_UNDEFINED) {
        js_object *o = new_host(h->j, h->node, HOST_STYLE, h->j->style_proto);

        if (!o)
            return JS_THROW;
        st = js_object_value(o);
        if (js_define(c, t.u.obj, "__style", st) != JS_OK)
            return JS_THROW;
    }
    *r = st;
    return JS_OK;
}

/* ------------------------------------------------------------------ */
/* Events */

static int dispatch_event_value(struct jsdom *j, js_value target,
                                js_value event, struct dom_node *node,
                                char *err, unsigned long errsz);

static int event_option_bool(js_ctx *c, js_value options, const char *name,
                             int *out)
{
    js_value value;

    *out = 0;
    if (!js_is_object(options))
        return JS_OK;
    if (js_get(c, options, name, &value) != JS_OK)
        return JS_THROW;
    *out = js_to_boolean(value);
    return JS_OK;
}

static int event_make(struct jsdom *j, js_object *prototype,
                      const char *type, js_value options,
                      js_value detail, int custom, js_value *out)
{
    js_object *object;
    js_value event;
    int bubbles, cancelable, composed;

    if (event_option_bool(j->ctx, options, "bubbles", &bubbles) != JS_OK ||
        event_option_bool(j->ctx, options, "cancelable", &cancelable) !=
            JS_OK ||
        event_option_bool(j->ctx, options, "composed", &composed) != JS_OK)
        return JS_THROW;
    object = js_new_object_proto(j->ctx, prototype);
    if (!object)
        return JS_THROW;
    event = js_object_value(object);
    if (js_set(j->ctx, event, "type",
               js_mkcstring(j->ctx, type ? type : "")) != JS_OK ||
        js_set(j->ctx, event, "bubbles", js_bool(bubbles)) != JS_OK ||
        js_set(j->ctx, event, "cancelable", js_bool(cancelable)) != JS_OK ||
        js_set(j->ctx, event, "composed", js_bool(composed)) != JS_OK ||
        js_set(j->ctx, event, "defaultPrevented", js_bool(0)) != JS_OK ||
        js_set(j->ctx, event, "target", js_null()) != JS_OK ||
        js_set(j->ctx, event, "currentTarget", js_null()) != JS_OK ||
        js_set(j->ctx, event, "eventPhase", js_number(0)) != JS_OK ||
        js_set(j->ctx, event, "timeStamp",
               js_number((double)(j->event_turn * 200UL))) != JS_OK ||
        js_set(j->ctx, event, "isTrusted", js_bool(0)) != JS_OK ||
        js_define(j->ctx, object, "__dispatching", js_bool(0)) != JS_OK ||
        js_define(j->ctx, object, "__stopped", js_bool(0)) != JS_OK ||
        js_define(j->ctx, object, "__immediateStopped", js_bool(0)) != JS_OK ||
        js_define(j->ctx, object, "__inPassiveListener", js_bool(0)) != JS_OK ||
        js_define(j->ctx, object, "__path", js_undefined()) != JS_OK ||
        (custom && js_set(j->ctx, event, "detail", detail) != JS_OK))
        return JS_THROW;
    *out = event;
    return JS_OK;
}

static int event_constructor(js_ctx *c, js_value t,
                             int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    js_value type_value;
    const char *type;
    (void)t;

    if (!j || !js_is_constructing(c))
        return js_throw_error(c, JS_ERR_TYPE,
                              "Event must be constructed with new");
    if (ac < 1 || js_to_string(c, av[0], &type_value) != JS_OK)
        return ac < 1
            ? js_throw_error(c, JS_ERR_TYPE, "Event requires a type")
            : JS_THROW;
    type = js_string_bytes(type_value, 0);
    return event_make(j, j->event_proto, type,
                      ac > 1 ? av[1] : js_undefined(),
                      js_undefined(), 0, r);
}

static int custom_event_constructor(js_ctx *c, js_value t,
                                    int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    js_value type_value, detail = js_null(), options;
    const char *type;
    (void)t;

    if (!j || !js_is_constructing(c))
        return js_throw_error(c, JS_ERR_TYPE,
                              "CustomEvent must be constructed with new");
    if (ac < 1 || js_to_string(c, av[0], &type_value) != JS_OK)
        return ac < 1
            ? js_throw_error(c, JS_ERR_TYPE, "CustomEvent requires a type")
            : JS_THROW;
    options = ac > 1 ? av[1] : js_undefined();
    if (js_is_object(options) &&
        js_get(c, options, "detail", &detail) != JS_OK)
        return JS_THROW;
    if (js_is_undefined(detail))
        detail = js_null();
    type = js_string_bytes(type_value, 0);
    return event_make(j, j->custom_event_proto, type, options,
                      detail, 1, r);
}

enum derived_event_kind {
    DERIVED_UI_EVENT = 1,
    DERIVED_MOUSE_EVENT,
    DERIVED_KEYBOARD_EVENT,
    DERIVED_FOCUS_EVENT,
    DERIVED_INPUT_EVENT,
    DERIVED_POINTER_EVENT
};

static int event_option_value(js_ctx *c, js_value options,
                              const char *name, js_value *out)
{
    if (!js_is_object(options)) {
        *out = js_undefined();
        return JS_OK;
    }
    return js_get(c, options, name, out);
}

static int event_set_number_option(js_ctx *c, js_value event,
                                   js_value options, const char *name,
                                   double fallback)
{
    js_value value;
    double number = fallback;

    if (event_option_value(c, options, name, &value) != JS_OK)
        return JS_THROW;
    if (!js_is_undefined(value) &&
        js_to_number(c, value, &number) != JS_OK)
        return JS_THROW;
    return js_set(c, event, name, js_number(number));
}

static int event_set_bool_option(js_ctx *c, js_value event,
                                 js_value options, const char *name)
{
    int value;

    if (event_option_bool(c, options, name, &value) != JS_OK)
        return JS_THROW;
    return js_set(c, event, name, js_bool(value));
}

static int event_set_string_option(js_ctx *c, js_value event,
                                   js_value options, const char *name,
                                   const char *fallback)
{
    js_value value, string;

    if (event_option_value(c, options, name, &value) != JS_OK)
        return JS_THROW;
    if (js_is_undefined(value))
        string = js_mkcstring(c, fallback);
    else if (js_to_string(c, value, &string) != JS_OK)
        return JS_THROW;
    return js_set(c, event, name, string);
}

static int event_set_object_option(js_ctx *c, js_value event,
                                   js_value options, const char *name)
{
    js_value value;

    if (event_option_value(c, options, name, &value) != JS_OK)
        return JS_THROW;
    if (js_is_undefined(value))
        value = js_null();
    return js_set(c, event, name, value);
}

static int derived_event_make(struct jsdom *j, js_object *prototype,
                              const char *type, js_value options,
                              int kind, js_value *out)
{
    js_value event;
    int mouse_like = kind == DERIVED_MOUSE_EVENT ||
                     kind == DERIVED_POINTER_EVENT;

    if (event_make(j, prototype, type, options,
                   js_undefined(), 0, &event) != JS_OK ||
        event_set_object_option(j->ctx, event, options, "view") != JS_OK ||
        event_set_number_option(j->ctx, event, options,
                                "detail", 0) != JS_OK)
        return JS_THROW;
    if (mouse_like) {
        if (event_set_number_option(j->ctx, event, options,
                                    "screenX", 0) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "screenY", 0) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "clientX", 0) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "clientY", 0) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "pageX", 0) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "pageY", 0) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "offsetX", 0) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "offsetY", 0) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "movementX", 0) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "movementY", 0) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "button", 0) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "buttons", 0) != JS_OK ||
            event_set_bool_option(j->ctx, event, options,
                                  "ctrlKey") != JS_OK ||
            event_set_bool_option(j->ctx, event, options,
                                  "shiftKey") != JS_OK ||
            event_set_bool_option(j->ctx, event, options,
                                  "altKey") != JS_OK ||
            event_set_bool_option(j->ctx, event, options,
                                  "metaKey") != JS_OK ||
            event_set_object_option(j->ctx, event, options,
                                    "relatedTarget") != JS_OK)
            return JS_THROW;
    } else if (kind == DERIVED_KEYBOARD_EVENT) {
        if (event_set_string_option(j->ctx, event, options,
                                    "key", "") != JS_OK ||
            event_set_string_option(j->ctx, event, options,
                                    "code", "") != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "location", 0) != JS_OK ||
            event_set_bool_option(j->ctx, event, options,
                                  "ctrlKey") != JS_OK ||
            event_set_bool_option(j->ctx, event, options,
                                  "shiftKey") != JS_OK ||
            event_set_bool_option(j->ctx, event, options,
                                  "altKey") != JS_OK ||
            event_set_bool_option(j->ctx, event, options,
                                  "metaKey") != JS_OK ||
            event_set_bool_option(j->ctx, event, options,
                                  "repeat") != JS_OK ||
            event_set_bool_option(j->ctx, event, options,
                                  "isComposing") != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "charCode", 0) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "keyCode", 0) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "which", 0) != JS_OK)
            return JS_THROW;
    } else if (kind == DERIVED_FOCUS_EVENT) {
        if (event_set_object_option(j->ctx, event, options,
                                    "relatedTarget") != JS_OK)
            return JS_THROW;
    } else if (kind == DERIVED_INPUT_EVENT) {
        js_value data;

        if (event_option_value(j->ctx, options, "data", &data) != JS_OK)
            return JS_THROW;
        if (js_is_undefined(data))
            data = js_null();
        else if (!js_is_null(data) && !js_is_string(data) &&
                 js_to_string(j->ctx, data, &data) != JS_OK)
            return JS_THROW;
        if (js_set(j->ctx, event, "data", data) != JS_OK ||
            event_set_string_option(j->ctx, event, options,
                                    "inputType", "") != JS_OK ||
            event_set_bool_option(j->ctx, event, options,
                                  "isComposing") != JS_OK ||
            event_set_object_option(j->ctx, event, options,
                                    "dataTransfer") != JS_OK)
            return JS_THROW;
    }
    if (kind == DERIVED_POINTER_EVENT) {
        if (event_set_number_option(j->ctx, event, options,
                                    "pointerId", 0) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "width", 1) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "height", 1) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "pressure", 0) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "tangentialPressure", 0) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "tiltX", 0) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "tiltY", 0) != JS_OK ||
            event_set_number_option(j->ctx, event, options,
                                    "twist", 0) != JS_OK ||
            event_set_string_option(j->ctx, event, options,
                                    "pointerType", "") != JS_OK ||
            event_set_bool_option(j->ctx, event, options,
                                  "isPrimary") != JS_OK)
            return JS_THROW;
    }
    *out = event;
    return JS_OK;
}

static int derived_event_constructor(js_ctx *c, int ac, js_value *av,
                                     js_value *r, int kind,
                                     js_object *prototype,
                                     const char *interface_name)
{
    struct jsdom *j = binding(c);
    js_value type_value;
    const char *type;

    if (!j || !js_is_constructing(c))
        return js_throw_error(c, JS_ERR_TYPE, "%s requires new",
                              interface_name);
    if (ac < 1 || js_to_string(c, av[0], &type_value) != JS_OK)
        return ac < 1
            ? js_throw_error(c, JS_ERR_TYPE, "%s requires a type",
                             interface_name)
            : JS_THROW;
    type = js_string_bytes(type_value, 0);
    return derived_event_make(j, prototype, type,
                              ac > 1 ? av[1] : js_undefined(),
                              kind, r);
}

#define DERIVED_EVENT_CONSTRUCTOR(name, kind, field, label)                 \
static int name(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)    \
{                                                                            \
    struct jsdom *j = binding(c);                                             \
    (void)t;                                                                  \
    return !j ? js_throw_error(c, JS_ERR_TYPE, "missing browser realm")       \
              : derived_event_constructor(c, ac, av, r, kind,                 \
                                          j->field, label);                   \
}
DERIVED_EVENT_CONSTRUCTOR(ui_event_constructor, DERIVED_UI_EVENT,
                          ui_event_proto, "UIEvent")
DERIVED_EVENT_CONSTRUCTOR(mouse_event_constructor, DERIVED_MOUSE_EVENT,
                          mouse_event_proto, "MouseEvent")
DERIVED_EVENT_CONSTRUCTOR(keyboard_event_constructor, DERIVED_KEYBOARD_EVENT,
                          keyboard_event_proto, "KeyboardEvent")
DERIVED_EVENT_CONSTRUCTOR(focus_event_constructor, DERIVED_FOCUS_EVENT,
                          focus_event_proto, "FocusEvent")
DERIVED_EVENT_CONSTRUCTOR(input_event_constructor, DERIVED_INPUT_EVENT,
                          input_event_proto, "InputEvent")
DERIVED_EVENT_CONSTRUCTOR(pointer_event_constructor, DERIVED_POINTER_EVENT,
                          pointer_event_proto, "PointerEvent")
#undef DERIVED_EVENT_CONSTRUCTOR

static int event_get_modifier_state(js_ctx *c, js_value t,
                                    int ac, js_value *av, js_value *r)
{
    const char *modifier;
    const char *property = 0;
    js_value value;

    if (!js_is_object(t))
        return js_throw_error(c, JS_ERR_TYPE, "illegal event receiver");
    if (string_arg(c, ac, av, 0, &modifier) != JS_OK)
        return JS_THROW;
    if (!strcmp(modifier, "Alt") || !strcmp(modifier, "AltGraph"))
        property = "altKey";
    else if (!strcmp(modifier, "Control"))
        property = "ctrlKey";
    else if (!strcmp(modifier, "Meta"))
        property = "metaKey";
    else if (!strcmp(modifier, "Shift"))
        property = "shiftKey";
    if (!property || js_get(c, t, property, &value) != JS_OK)
        *r = js_bool(0);
    else
        *r = js_bool(js_to_boolean(value));
    return JS_OK;
}

static int event_target_constructor(js_ctx *c, js_value t,
                                    int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    js_object *object;
    (void)t; (void)ac; (void)av;

    if (!j || !js_is_constructing(c))
        return js_throw_error(c, JS_ERR_TYPE,
                              "EventTarget must be constructed with new");
    object = new_host(j, 0, HOST_EVENT_TARGET, j->event_target_proto);
    if (!object)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    *r = js_object_value(object);
    return JS_OK;
}

static int event_prevent(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    js_value cancelable, passive;
    (void)ac; (void)av;
    if (!js_is_object(t))
        return js_throw_error(c, JS_ERR_TYPE, "illegal Event receiver");
    if (js_get(c, t, "cancelable", &cancelable) != JS_OK ||
        js_get(c, t, "__inPassiveListener", &passive) != JS_OK)
        return JS_THROW;
    if (js_to_boolean(cancelable) && !js_to_boolean(passive) &&
        js_set(c, t, "defaultPrevented", js_bool(1)) != JS_OK)
        return JS_THROW;
    *r = js_undefined();
    return JS_OK;
}

static int event_cancel_bubble_get(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    (void)ac; (void)av;
    if (!js_is_object(t))
        return js_throw_error(c, JS_ERR_TYPE, "illegal Event receiver");
    return js_get(c, t, "__stopped", r);
}

static int event_cancel_bubble_set(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    if (!js_is_object(t))
        return js_throw_error(c, JS_ERR_TYPE, "illegal Event receiver");
    if (ac > 0 && js_to_boolean(av[0]) &&
        js_set(c, t, "__stopped", js_bool(1)) != JS_OK)
        return JS_THROW;
    *r = js_undefined();
    return JS_OK;
}

static int event_return_value_get(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    js_value prevented;
    (void)ac; (void)av;

    if (!js_is_object(t))
        return js_throw_error(c, JS_ERR_TYPE, "illegal Event receiver");
    if (js_get(c, t, "defaultPrevented", &prevented) != JS_OK)
        return JS_THROW;
    *r = js_bool(!js_to_boolean(prevented));
    return JS_OK;
}

static int event_return_value_set(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    if (!js_is_object(t))
        return js_throw_error(c, JS_ERR_TYPE, "illegal Event receiver");
    if (ac > 0 && !js_to_boolean(av[0]))
        return event_prevent(c, t, 0, 0, r);
    *r = js_undefined();
    return JS_OK;
}

static int event_src_element_get(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    (void)ac; (void)av;
    if (!js_is_object(t))
        return js_throw_error(c, JS_ERR_TYPE, "illegal Event receiver");
    return js_get(c, t, "target", r);
}

static int event_composed_path(js_ctx *c, js_value t,
                               int ac, js_value *av, js_value *r)
{
    js_value stored, item;
    js_object *copy;
    unsigned long index, count;
    (void)ac; (void)av;

    if (!js_is_object(t))
        return js_throw_error(c, JS_ERR_TYPE, "illegal Event receiver");
    copy = js_new_array(c);
    if (!copy)
        return JS_THROW;
    if (js_get(c, t, "__path", &stored) != JS_OK)
        return JS_THROW;
    if (js_is_array(stored)) {
        count = js_array_length(stored.u.obj);
        for (index = 0; index < count; index++) {
            if (js_array_get(c, stored.u.obj, index, &item) != JS_OK ||
                js_array_push(c, copy, item) != JS_OK)
                return JS_THROW;
        }
    }
    *r = js_object_value(copy);
    return JS_OK;
}

static int event_stop(js_ctx *c, js_value t, int ac, js_value *av,
                      js_value *r)
{
    (void)ac; (void)av;
    if (!js_is_object(t))
        return js_throw_error(c, JS_ERR_TYPE, "illegal Event receiver");
    if (js_set(c, t, "__stopped", js_bool(1)) != JS_OK)
        return JS_THROW;
    *r = js_undefined();
    return JS_OK;
}

static int event_stop_immediate(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    (void)ac; (void)av;
    if (!js_is_object(t))
        return js_throw_error(c, JS_ERR_TYPE, "illegal Event receiver");
    if (js_set(c, t, "__stopped", js_bool(1)) != JS_OK ||
        js_set(c, t, "__immediateStopped", js_bool(1)) != JS_OK)
        return JS_THROW;
    *r = js_undefined();
    return JS_OK;
}

static int event_callback(js_ctx *c, js_value callback,
                          js_value *callable, js_value *receiver)
{
    if (js_is_function(callback)) {
        *callable = callback;
        *receiver = js_undefined();
        return 1;
    }
    if (!js_is_object(callback) ||
        js_get(c, callback, "handleEvent", callable) != JS_OK ||
        !js_is_function(*callable))
        return 0;
    *receiver = callback;
    return 1;
}

static int listener_capture_option(js_ctx *c, js_value options,
                                   int *capture)
{
    *capture = 0;
    if (js_is_bool(options)) {
        *capture = js_to_boolean(options);
        return JS_OK;
    }
    return event_option_bool(c, options, "capture", capture);
}

static int listener_options(js_ctx *c, js_value options,
                            int *capture, int *once, int *passive,
                            js_value *signal)
{
    if (listener_capture_option(c, options, capture) != JS_OK ||
        event_option_bool(c, options, "once", once) != JS_OK ||
        event_option_bool(c, options, "passive", passive) != JS_OK)
        return JS_THROW;
    *signal = js_undefined();
    if (js_is_object(options) &&
        js_get(c, options, "signal", signal) != JS_OK)
        return JS_THROW;
    if (!js_is_undefined(*signal) && !js_is_null(*signal) &&
        !js_is_abortsignal(*signal))
        return js_throw_error(c, JS_ERR_TYPE,
                              "listener signal must be an AbortSignal");
    return JS_OK;
}

static int listener_entry_fields(js_ctx *c, js_value entry,
                                 js_value *callback, int *capture)
{
    js_value capture_value;

    if (js_is_function(entry)) {
        *callback = entry;
        *capture = 0;
        return JS_OK;
    }
    if (!js_is_object(entry) ||
        js_get(c, entry, "callback", callback) != JS_OK ||
        js_get(c, entry, "capture", &capture_value) != JS_OK)
        return JS_THROW;
    *capture = js_to_boolean(capture_value);
    return JS_OK;
}

static int event_listener_add(js_ctx *c, js_value target,
                              int ac, js_value *av, js_value *r)
{
    const char *type;
    char key[96];
    js_value list, callback, receiver, signal, entry_value;
    js_object *array, *entry;
    unsigned long i, count, hole = (unsigned long)-1;
    int capture, once, passive;

    if (string_arg(c, ac, av, 0, &type) != JS_OK)
        return JS_THROW;
    if (strlen(type) > 64)
        return js_throw_error(c, JS_ERR_RANGE,
                              "event type is too long");
    if (ac < 2 || js_is_null(av[1]) || js_is_undefined(av[1])) {
        *r = js_undefined();
        return JS_OK;
    }
    if (!event_callback(c, av[1], &callback, &receiver))
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid event listener");
    (void)callback;
    (void)receiver;
    if (listener_options(c, ac > 2 ? av[2] : js_undefined(),
                         &capture, &once, &passive, &signal) != JS_OK)
        return JS_THROW;
    if (js_abortsignal_aborted(signal)) {
        *r = js_undefined();
        return JS_OK;
    }
    snprintf(key, sizeof(key), "__listeners_%s", type);
    if (js_get(c, target, key, &list) != JS_OK || !js_is_array(list)) {
        array = js_new_array(c);
        if (!array || js_define(c, target.u.obj, key,
                                js_object_value(array)) != JS_OK)
            return JS_THROW;
        list = js_object_value(array);
    } else {
        array = list.u.obj;
    }
    count = js_array_length(array);
    for (i = 0; i < count; i++) {
        js_value existing, existing_callback;
        int existing_capture;

        if (js_array_get(c, array, i, &existing) != JS_OK)
            return JS_THROW;
        if (js_is_undefined(existing)) {
            if (hole == (unsigned long)-1)
                hole = i;
            continue;
        }
        if (listener_entry_fields(c, existing, &existing_callback,
                                  &existing_capture) != JS_OK)
            return JS_THROW;
        if (js_is_object(existing_callback) &&
            js_is_object(av[1]) &&
            existing_callback.u.obj == av[1].u.obj &&
            existing_capture == capture) {
            *r = js_undefined();
            return JS_OK;
        }
    }
    if (hole == (unsigned long)-1 && count >= JSDOM_EVENT_LISTENER_MAX)
        return js_throw_error(c, JS_ERR_RANGE,
                              "too many listeners for event type");
    entry = js_new_object(c);
    if (!entry)
        return JS_THROW;
    entry_value = js_object_value(entry);
    if (js_define(c, entry, "callback", av[1]) != JS_OK ||
        js_define(c, entry, "capture", js_bool(capture)) != JS_OK ||
        js_define(c, entry, "once", js_bool(once)) != JS_OK ||
        js_define(c, entry, "passive", js_bool(passive)) != JS_OK ||
        js_define(c, entry, "signal", signal) != JS_OK)
        return JS_THROW;
    if (hole != (unsigned long)-1) {
        if (js_set_value(c, list, js_number((double)hole),
                         entry_value) != JS_OK)
            return JS_THROW;
    } else if (js_array_push(c, array, entry_value) != JS_OK) {
        return JS_THROW;
    }
    *r = js_undefined();
    return JS_OK;
}

static int event_listener_remove(js_ctx *c, js_value target,
                                 int ac, js_value *av, js_value *r)
{
    const char *type;
    char key[96];
    js_value list;
    unsigned long i, count;
    int capture;

    if (string_arg(c, ac, av, 0, &type) != JS_OK)
        return JS_THROW;
    if (strlen(type) > 64 || ac < 2 || !js_is_object(av[1]) ||
        listener_capture_option(c, ac > 2 ? av[2] : js_undefined(),
                                &capture) != JS_OK) {
        *r = js_undefined();
        return JS_OK;
    }
    snprintf(key, sizeof(key), "__listeners_%s", type);
    if (js_get(c, target, key, &list) != JS_OK || !js_is_array(list)) {
        *r = js_undefined();
        return JS_OK;
    }
    count = js_array_length(list.u.obj);
    for (i = 0; i < count; i++) {
        js_value entry, callback;
        int entry_capture;

        if (js_array_get(c, list.u.obj, i, &entry) != JS_OK)
            return JS_THROW;
        if (js_is_undefined(entry))
            continue;
        if (listener_entry_fields(c, entry, &callback,
                                  &entry_capture) != JS_OK)
            return JS_THROW;
        if (js_is_object(callback) &&
            callback.u.obj == av[1].u.obj &&
            entry_capture == capture) {
            if (js_set_value(c, list, js_number((double)i),
                             js_undefined()) != JS_OK)
                return JS_THROW;
            break;
        }
    }
    *r = js_undefined();
    return JS_OK;
}

static int node_add_listener(js_ctx *c, js_value t, int ac, js_value *av,
                             js_value *r)
{
    struct hostref *h = host(t, 0);

    if (!h || (h->kind != HOST_NODE && h->kind != HOST_DOCUMENT &&
               h->kind != HOST_EVENT_TARGET &&
               h->kind != HOST_FILE_READER &&
               h->kind != HOST_MEDIA_QUERY))
        return js_throw_error(c, JS_ERR_TYPE, "illegal EventTarget");
    return event_listener_add(c, t, ac, av, r);
}

static int event_remove_listener(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, 0);

    if (!h || (h->kind != HOST_NODE && h->kind != HOST_DOCUMENT &&
               h->kind != HOST_EVENT_TARGET &&
               h->kind != HOST_FILE_READER &&
               h->kind != HOST_MEDIA_QUERY))
        return js_throw_error(c, JS_ERR_TYPE, "illegal EventTarget");
    return event_listener_remove(c, t, ac, av, r);
}

static int event_target_dispatch(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, 0);
    char err[160];
    int allowed;

    if (!h || (h->kind != HOST_NODE && h->kind != HOST_DOCUMENT &&
               h->kind != HOST_EVENT_TARGET &&
               h->kind != HOST_FILE_READER &&
               h->kind != HOST_MEDIA_QUERY))
        return js_throw_error(c, JS_ERR_TYPE, "illegal EventTarget");
    if (ac < 1 || !js_is_object(av[0]))
        return js_throw_error(c, JS_ERR_TYPE,
                              "dispatchEvent requires an Event");
    err[0] = 0;
    allowed = dispatch_event_value(h->j, t, av[0], h->node,
                                   err, sizeof(err));
    if (allowed < 0)
        return JS_THROW;
    *r = js_bool(allowed);
    return JS_OK;
}

static int media_query_add_listener(js_ctx *c, js_value t,
                                    int ac, js_value *av, js_value *r)
{
    js_value args[2];

    if (!host(t, HOST_MEDIA_QUERY))
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid MediaQueryList receiver");
    if (ac < 1 || js_is_null(av[0]) || js_is_undefined(av[0])) {
        *r = js_undefined();
        return JS_OK;
    }
    args[0] = js_mkcstring(c, "change");
    args[1] = av[0];
    return node_add_listener(c, t, 2, args, r);
}

static int media_query_remove_listener(js_ctx *c, js_value t,
                                       int ac, js_value *av, js_value *r)
{
    js_value args[2];

    if (!host(t, HOST_MEDIA_QUERY))
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid MediaQueryList receiver");
    if (ac < 1 || js_is_null(av[0]) || js_is_undefined(av[0])) {
        *r = js_undefined();
        return JS_OK;
    }
    args[0] = js_mkcstring(c, "change");
    args[1] = av[0];
    return event_remove_listener(c, t, 2, args, r);
}

static int global_match_media(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    struct css_media media;
    js_object *object;
    js_value text;
    const char *query;
    unsigned long length;
    int matches;
    (void)t;

    if (!j)
        return js_throw_error(c, JS_ERR_TYPE, "missing browser realm");
    if (ac < 1)
        return js_throw_error(c, JS_ERR_TYPE,
                              "matchMedia requires a query");
    if (js_to_string(c, av[0], &text) != JS_OK)
        return JS_THROW;
    query = js_string_bytes(text, &length);
    if (length > 4096)
        return js_throw_error(c, JS_ERR_RANGE,
                              "media query is too long");
    memset(&media, 0, sizeof(media));
    media.width = (int)j->viewport_width;
    media.height = (int)j->viewport_height;
    media.dpi = 96;
    media.screen = 1;
    matches = css_media_query_matches(query, length, &media);
    object = new_host(j, 0, HOST_MEDIA_QUERY, j->media_query_proto);
    if (!object)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    if (js_set(c, js_object_value(object), "matches",
               js_bool(matches)) != JS_OK ||
        js_set(c, js_object_value(object), "media",
               js_mkstring(c, query, length)) != JS_OK ||
        js_set(c, js_object_value(object), "onchange",
               js_null()) != JS_OK)
        return JS_THROW;
    *r = js_object_value(object);
    return JS_OK;
}

static int global_add_listener(js_ctx *c, js_value t, int ac, js_value *av,
                               js_value *r)
{
    (void)t;
    return event_listener_add(c, js_global(c), ac, av, r);
}

static int global_remove_listener(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    (void)t;
    return event_listener_remove(c, js_global(c), ac, av, r);
}

static int global_dispatch_event(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    char err[160];
    int allowed;
    (void)t;

    if (!j || ac < 1 || !js_is_object(av[0]))
        return js_throw_error(c, JS_ERR_TYPE,
                              "dispatchEvent requires an Event");
    err[0] = 0;
    allowed = dispatch_event_value(j, js_global(c), av[0], 0,
                                   err, sizeof(err));
    if (allowed < 0)
        return JS_THROW;
    *r = js_bool(allowed);
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
/* MutationObserver */

static struct mutation_observer_state *mutation_observer(js_value value)
{
    struct hostref *h = host(value, HOST_MUTATION_OBSERVER);
    return h ? (struct mutation_observer_state *)h->data : 0;
}

static int mutation_observer_constructor(js_ctx *c, js_value t,
                                         int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    struct mutation_observer_state *state;
    js_object *object;
    int slot;
    (void)t;

    if (!js_is_constructing(c))
        return js_throw_error(c, JS_ERR_TYPE,
                              "MutationObserver requires new");
    if (!j || ac < 1 || !js_is_function(av[0]))
        return js_throw_error(c, JS_ERR_TYPE,
                              "MutationObserver requires a callback");
    for (slot = 0; slot < JSDOM_MUTATION_OBSERVERS_MAX; slot++)
        if (!j->mutation_observers[slot])
            break;
    if (slot == JSDOM_MUTATION_OBSERVERS_MAX)
        return js_throw_error(c, JS_ERR_RANGE,
                              "too many MutationObservers");
    state = (struct mutation_observer_state *)calloc(1, sizeof(*state));
    if (!state)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    state->callback = av[0];
    object = new_host_data(j, 0, HOST_MUTATION_OBSERVER,
                           state, j->mutation_observer_proto);
    if (!object) {
        free(state);
        return JS_THROW;
    }
    state->self = js_object_value(object);
    j->mutation_observers[slot] = state;
    *r = state->self;
    return JS_OK;
}

static int mutation_observer_observe(js_ctx *c, js_value t,
                                     int ac, js_value *av, js_value *r)
{
    struct mutation_observer_state *state = mutation_observer(t);
    struct mutation_registration registration;
    struct hostref *target = ac > 0 ? node_or_document(av[0]) : 0;
    struct dom_node *target_node;
    js_value options = ac > 1 ? av[1] : js_undefined();
    js_value filter;
    unsigned int filter_count = 0, filter_index, registration_index;
    int child_list, attributes, character_data, subtree;
    int attribute_old_value, character_data_old_value;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid MutationObserver receiver");
    if (!target)
        return js_throw_error(c, JS_ERR_TYPE,
                              "MutationObserver target must be a Node");
    if (!js_is_object(options))
        return js_throw_error(c, JS_ERR_TYPE,
                              "MutationObserver options are required");
    memset(&registration, 0, sizeof(registration));
    if (event_option_bool(c, options, "childList", &child_list) != JS_OK ||
        event_option_bool(c, options, "attributes", &attributes) != JS_OK ||
        event_option_bool(c, options, "characterData",
                          &character_data) != JS_OK ||
        event_option_bool(c, options, "subtree", &subtree) != JS_OK ||
        event_option_bool(c, options, "attributeOldValue",
                          &attribute_old_value) != JS_OK ||
        event_option_bool(c, options, "characterDataOldValue",
                          &character_data_old_value) != JS_OK)
        return JS_THROW;
    if (js_get(c, options, "attributeFilter", &filter) != JS_OK)
        return JS_THROW;
    if (!js_is_undefined(filter)) {
        if (!js_is_array(filter))
            return js_throw_error(c, JS_ERR_TYPE,
                                  "attributeFilter must be an Array");
        filter_count = (unsigned int)js_array_length(filter.u.obj);
        if (filter_count > JSDOM_MUTATION_FILTER_MAX)
            return js_throw_error(c, JS_ERR_RANGE,
                                  "attributeFilter is too large");
        for (filter_index = 0; filter_index < filter_count; filter_index++) {
            js_value name_value, name_string;
            const char *name;
            unsigned long name_length, name_index;

            if (js_array_get(c, filter.u.obj, filter_index,
                             &name_value) != JS_OK ||
                js_to_string(c, name_value, &name_string) != JS_OK)
                return JS_THROW;
            name = js_string_bytes(name_string, &name_length);
            if (name_length > JSDOM_MUTATION_ATTRIBUTE_MAX)
                return js_throw_error(c, JS_ERR_RANGE,
                                      "attributeFilter name is too large");
            for (name_index = 0; name_index < name_length; name_index++) {
                int ch = (unsigned char)name[name_index];

                registration.attribute_filter[filter_index][name_index] =
                    (char)(ch >= 'A' && ch <= 'Z'
                           ? ch + ('a' - 'A') : ch);
            }
            registration.attribute_filter[filter_index][name_length] = 0;
        }
        attributes = 1;
    }
    if (attribute_old_value)
        attributes = 1;
    if (character_data_old_value)
        character_data = 1;
    if (!child_list && !attributes && !character_data)
        return js_throw_error(c, JS_ERR_TYPE,
                              "MutationObserver observes no mutation type");
    target_node = receiver_node(target);
    for (registration_index = 0;
         registration_index < state->registration_count;
         registration_index++)
        if (state->registrations[registration_index].target == target_node)
            break;
    if (registration_index == state->registration_count &&
        state->registration_count >= JSDOM_MUTATION_TARGETS_MAX)
        return js_throw_error(c, JS_ERR_RANGE,
                              "MutationObserver has too many targets");
    registration.target = target_node;
    registration.child_list = (unsigned char)child_list;
    registration.attributes = (unsigned char)attributes;
    registration.character_data = (unsigned char)character_data;
    registration.subtree = (unsigned char)subtree;
    registration.attribute_old_value = (unsigned char)attribute_old_value;
    registration.character_data_old_value =
        (unsigned char)character_data_old_value;
    registration.has_attribute_filter =
        (unsigned char)!js_is_undefined(filter);
    registration.attribute_filter_count = filter_count;
    state->registrations[registration_index] = registration;
    if (registration_index == state->registration_count)
        state->registration_count++;
    *r = js_undefined();
    return JS_OK;
}

static int mutation_observer_disconnect(js_ctx *c, js_value t,
                                        int ac, js_value *av, js_value *r)
{
    struct mutation_observer_state *state = mutation_observer(t);
    unsigned int i;
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid MutationObserver receiver");
    memset(state->registrations, 0, sizeof(state->registrations));
    state->registration_count = 0;
    for (i = 0; i < state->record_count; i++)
        state->records[i] = js_undefined();
    state->record_count = 0;
    *r = js_undefined();
    return JS_OK;
}

static int mutation_take_records(js_ctx *c,
                                 struct mutation_observer_state *state,
                                 js_value *r)
{
    js_object *records = js_new_array(c);
    unsigned int i;

    if (!records)
        return JS_THROW;
    for (i = 0; i < state->record_count; i++) {
        if (js_array_push(c, records, state->records[i]) != JS_OK)
            return JS_THROW;
        state->records[i] = js_undefined();
    }
    state->record_count = 0;
    *r = js_object_value(records);
    return JS_OK;
}

static int mutation_observer_take_records(js_ctx *c, js_value t,
                                          int ac, js_value *av, js_value *r)
{
    struct mutation_observer_state *state = mutation_observer(t);
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid MutationObserver receiver");
    return mutation_take_records(c, state, r);
}

/* ------------------------------------------------------------------ */
/* DOM traversal */

static int throw_dom_exception(js_ctx *c, const char *name,
                               const char *message);

static unsigned int traversal_node_type(const struct dom_node *node)
{
    if (!node)
        return 0;
    return node->type == DOM_ELEMENT ? 1 :
           node->type == DOM_TEXT ? 3 :
           node->type == DOM_COMMENT ? 8 :
           node->type == DOM_DOCUMENT ? 9 :
           node->type == DOM_DOCTYPE ? 10 :
           node->type == DOM_FRAGMENT ? 11 : 0;
}

static struct traversal_state *traversal_state_of(js_value value,
                                                  int kind)
{
    struct hostref *h = host(value, kind);
    return h ? (struct traversal_state *)h->data : 0;
}

static int traversal_accept(js_ctx *c, struct jsdom *j,
                            struct traversal_state *state,
                            struct dom_node *node, int *decision)
{
    unsigned int type = traversal_node_type(node);
    js_value callback, argument, result;
    double number;

    *decision = 3;
    if (!type || !(state->what_to_show & (1UL << (type - 1))))
        return JS_OK;
    if (js_is_null(state->filter) ||
        js_is_undefined(state->filter)) {
        *decision = 1;
        return JS_OK;
    }
    if (js_is_function(state->filter))
        callback = state->filter;
    else {
        if (!js_is_object(state->filter) ||
            js_get(c, state->filter, "acceptNode", &callback) != JS_OK ||
            !js_is_function(callback))
            return js_throw_error(c, JS_ERR_TYPE,
                                  "NodeFilter requires acceptNode");
    }
    argument = wrap_node(j, node);
    if (js_fatal(c) ||
        js_call(c, callback, state->filter,
                1, &argument, &result) != JS_OK ||
        js_to_integer(c, result, &number) != JS_OK)
        return JS_THROW;
    *decision = number == 2 ? 2 : number == 3 ? 3 : 1;
    return JS_OK;
}

static struct dom_node *traversal_previous(struct dom_node *node,
                                           struct dom_node *root)
{
    if (!node || node == root)
        return 0;
    if (node->prev_sibling) {
        node = node->prev_sibling;
        while (node->last_child)
            node = node->last_child;
        return node;
    }
    return node->parent;
}

static struct dom_node *traversal_after_subtree(struct dom_node *node,
                                                struct dom_node *root)
{
    while (node && node != root) {
        if (node->next_sibling)
            return node->next_sibling;
        node = node->parent;
    }
    return 0;
}

static int traversal_root_get(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE_ITERATOR);
    struct traversal_state *state;
    (void)ac; (void)av;

    if (!h)
        h = host(t, HOST_TREE_WALKER);
    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal traversal receiver");
    state = (struct traversal_state *)h->data;
    *r = wrap_node(h->j, state->root);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int traversal_show_get(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    struct traversal_state *state =
        traversal_state_of(t, HOST_NODE_ITERATOR);
    (void)c; (void)ac; (void)av;

    if (!state)
        state = traversal_state_of(t, HOST_TREE_WALKER);
    if (!state)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal traversal receiver");
    *r = js_number(state->what_to_show);
    return JS_OK;
}

static int traversal_filter_get(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    struct traversal_state *state =
        traversal_state_of(t, HOST_NODE_ITERATOR);
    (void)c; (void)ac; (void)av;

    if (!state)
        state = traversal_state_of(t, HOST_TREE_WALKER);
    if (!state)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal traversal receiver");
    *r = state->filter;
    return JS_OK;
}

static int node_iterator_reference_get(js_ctx *c, js_value t,
                                       int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE_ITERATOR);
    struct traversal_state *state;
    (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal NodeIterator receiver");
    state = (struct traversal_state *)h->data;
    *r = wrap_node(h->j, state->current);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int node_iterator_before_get(js_ctx *c, js_value t,
                                    int ac, js_value *av, js_value *r)
{
    struct traversal_state *state =
        traversal_state_of(t, HOST_NODE_ITERATOR);
    (void)c; (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal NodeIterator receiver");
    *r = js_bool(state->before_reference);
    return JS_OK;
}

static int node_iterator_move(js_ctx *c, js_value t,
                              js_value *r, int direction)
{
    struct hostref *h = host(t, HOST_NODE_ITERATOR);
    struct traversal_state *state;
    struct dom_node *node;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal NodeIterator receiver");
    state = (struct traversal_state *)h->data;
    node = direction > 0
        ? (state->before_reference ? state->current :
           dom_next_within(state->current, state->root))
        : (!state->before_reference ? state->current :
           traversal_previous(state->current, state->root));
    while (node) {
        int decision;

        if (traversal_accept(c, h->j, state, node,
                             &decision) != JS_OK)
            return JS_THROW;
        if (decision == 1) {
            state->current = node;
            state->before_reference = direction < 0;
            *r = wrap_node(h->j, node);
            return js_fatal(c) ? JS_THROW : JS_OK;
        }
        node = direction > 0
            ? dom_next_within(node, state->root)
            : traversal_previous(node, state->root);
    }
    *r = js_null();
    return JS_OK;
}

static int node_iterator_next(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    (void)ac; (void)av;
    return node_iterator_move(c, t, r, 1);
}

static int node_iterator_previous(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    (void)ac; (void)av;
    return node_iterator_move(c, t, r, -1);
}

static int node_iterator_detach(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    (void)c; (void)t; (void)ac; (void)av;
    *r = js_undefined();
    return JS_OK;
}

static int tree_walker_current_get(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_TREE_WALKER);
    struct traversal_state *state;
    (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal TreeWalker receiver");
    state = (struct traversal_state *)h->data;
    *r = wrap_node(h->j, state->current);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int tree_walker_current_set(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_TREE_WALKER);
    struct hostref *node = ac ? node_or_document(av[0]) : 0;
    struct traversal_state *state;
    struct dom_node *candidate, *cursor;

    if (!h || !node || h->j != node->j)
        return js_throw_error(c, JS_ERR_TYPE,
                              "currentNode must be a Node");
    state = (struct traversal_state *)h->data;
    candidate = receiver_node(node);
    for (cursor = candidate; cursor && cursor != state->root;
         cursor = cursor->parent)
        ;
    if (!cursor)
        return throw_dom_exception(c, "NotFoundError",
                                   "currentNode is outside the root");
    state->current = candidate;
    *r = js_undefined();
    return JS_OK;
}

static int tree_walker_next(js_ctx *c, js_value t,
                            int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_TREE_WALKER);
    struct traversal_state *state;
    struct dom_node *node;
    (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal TreeWalker receiver");
    state = (struct traversal_state *)h->data;
    node = dom_next_within(state->current, state->root);
    while (node) {
        int decision;

        if (traversal_accept(c, h->j, state, node,
                             &decision) != JS_OK)
            return JS_THROW;
        if (decision == 1) {
            state->current = node;
            *r = wrap_node(h->j, node);
            return js_fatal(c) ? JS_THROW : JS_OK;
        }
        node = decision == 2
            ? traversal_after_subtree(node, state->root)
            : dom_next_within(node, state->root);
    }
    *r = js_null();
    return JS_OK;
}

static int tree_walker_previous(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_TREE_WALKER);
    struct traversal_state *state;
    struct dom_node *node;
    (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal TreeWalker receiver");
    state = (struct traversal_state *)h->data;
    for (node = traversal_previous(state->current, state->root);
         node; node = traversal_previous(node, state->root)) {
        int decision;

        if (traversal_accept(c, h->j, state, node,
                             &decision) != JS_OK)
            return JS_THROW;
        if (decision == 1) {
            state->current = node;
            *r = wrap_node(h->j, node);
            return js_fatal(c) ? JS_THROW : JS_OK;
        }
    }
    *r = js_null();
    return JS_OK;
}

static int tree_walker_parent(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_TREE_WALKER);
    struct traversal_state *state;
    struct dom_node *node;
    (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal TreeWalker receiver");
    state = (struct traversal_state *)h->data;
    for (node = state->current->parent; node; node = node->parent) {
        int decision;

        if (traversal_accept(c, h->j, state, node,
                             &decision) != JS_OK)
            return JS_THROW;
        if (decision == 1) {
            state->current = node;
            *r = wrap_node(h->j, node);
            return js_fatal(c) ? JS_THROW : JS_OK;
        }
        if (node == state->root)
            break;
    }
    *r = js_null();
    return JS_OK;
}

static int tree_walker_child(js_ctx *c, js_value t,
                             int ac, js_value *av, js_value *r,
                             int last)
{
    struct hostref *h = host(t, HOST_TREE_WALKER);
    struct traversal_state *state;
    struct dom_node *limit, *node;
    (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal TreeWalker receiver");
    state = (struct traversal_state *)h->data;
    limit = state->current;
    node = last ? limit->last_child : limit->first_child;
    while (node) {
        int decision;

        if (traversal_accept(c, h->j, state, node,
                             &decision) != JS_OK)
            return JS_THROW;
        if (decision == 1) {
            state->current = node;
            *r = wrap_node(h->j, node);
            return js_fatal(c) ? JS_THROW : JS_OK;
        }
        if (decision != 2 && node->first_child)
            node = last ? node->last_child : node->first_child;
        else
            node = last ? node->prev_sibling : node->next_sibling;
    }
    *r = js_null();
    return JS_OK;
}

static int tree_walker_first_child(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    return tree_walker_child(c, t, ac, av, r, 0);
}

static int tree_walker_last_child(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    return tree_walker_child(c, t, ac, av, r, 1);
}

static int tree_walker_sibling(js_ctx *c, js_value t,
                               int ac, js_value *av, js_value *r,
                               int previous)
{
    struct hostref *h = host(t, HOST_TREE_WALKER);
    struct traversal_state *state;
    struct dom_node *node;
    (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal TreeWalker receiver");
    state = (struct traversal_state *)h->data;
    for (node = previous ? state->current->prev_sibling :
                           state->current->next_sibling;
         node; node = previous ? node->prev_sibling :
                                 node->next_sibling) {
        int decision;

        if (traversal_accept(c, h->j, state, node,
                             &decision) != JS_OK)
            return JS_THROW;
        if (decision == 1) {
            state->current = node;
            *r = wrap_node(h->j, node);
            return js_fatal(c) ? JS_THROW : JS_OK;
        }
    }
    *r = js_null();
    return JS_OK;
}

static int tree_walker_previous_sibling(js_ctx *c, js_value t,
                                        int ac, js_value *av, js_value *r)
{
    return tree_walker_sibling(c, t, ac, av, r, 1);
}

static int tree_walker_next_sibling(js_ctx *c, js_value t,
                                    int ac, js_value *av, js_value *r)
{
    return tree_walker_sibling(c, t, ac, av, r, 0);
}

static int doc_create_traversal(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r,
                                int kind)
{
    struct hostref *document = host(t, HOST_DOCUMENT);
    struct hostref *root = ac ? node_or_document(av[0]) : 0;
    struct traversal_state *state;
    js_object *object;
    double what = 4294967295.0;

    if (!document || !root || document->j != root->j)
        return js_throw_error(c, JS_ERR_TYPE,
                              "traversal root must be a Node");
    if (ac > 1 && !js_is_undefined(av[1]) &&
        js_to_integer(c, av[1], &what) != JS_OK)
        return JS_THROW;
    if (what < 0)
        what = 4294967295.0;
    state = (struct traversal_state *)calloc(1, sizeof(*state));
    if (!state)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    state->root = receiver_node(root);
    state->current = state->root;
    state->what_to_show = (unsigned long)what;
    state->filter = ac > 2 ? av[2] : js_null();
    state->before_reference = kind == HOST_NODE_ITERATOR;
    object = new_host_data(document->j, 0, kind, state,
                           kind == HOST_NODE_ITERATOR
                           ? document->j->node_iterator_proto
                           : document->j->tree_walker_proto);
    if (!object) {
        free(state);
        return JS_THROW;
    }
    *r = js_object_value(object);
    return JS_OK;
}

static int doc_create_node_iterator(js_ctx *c, js_value t,
                                    int ac, js_value *av, js_value *r)
{
    return doc_create_traversal(c, t, ac, av, r, HOST_NODE_ITERATOR);
}

static int doc_create_tree_walker(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    return doc_create_traversal(c, t, ac, av, r, HOST_TREE_WALKER);
}

/* ------------------------------------------------------------------ */
/* Range and Selection */

static struct range_state *range_state_of(js_value value,
                                          struct hostref **owner)
{
    struct hostref *h = host(value, HOST_RANGE);

    if (owner)
        *owner = h;
    return h ? (struct range_state *)h->data : 0;
}

static unsigned long range_node_length(const struct dom_node *node)
{
    const struct dom_node *child;
    unsigned long count = 0;

    if (!node)
        return 0;
    if (node->type == DOM_TEXT || node->type == DOM_COMMENT)
        return node->text_len;
    for (child = node->first_child; child; child = child->next_sibling)
        count++;
    return count;
}

static struct dom_node *range_child_at(struct dom_node *node,
                                       unsigned long offset)
{
    struct dom_node *child;

    for (child = node ? node->first_child : 0;
         child && offset; child = child->next_sibling)
        offset--;
    return child;
}

static unsigned long range_child_index(const struct dom_node *node)
{
    const struct dom_node *cursor;
    unsigned long index = 0;

    for (cursor = node ? node->prev_sibling : 0;
         cursor; cursor = cursor->prev_sibling)
        index++;
    return index;
}

static int range_is_ancestor(const struct dom_node *ancestor,
                             const struct dom_node *node)
{
    for (; node; node = node->parent)
        if (node == ancestor)
            return 1;
    return 0;
}

static const struct dom_node *range_child_below(
    const struct dom_node *ancestor, const struct dom_node *node)
{
    const struct dom_node *child = node;

    while (child && child->parent != ancestor)
        child = child->parent;
    return child;
}

/* -1 when A precedes B, 0 when equal, 1 when A follows B. */
static int range_compare_points(const struct dom_node *a,
                                unsigned long a_offset,
                                const struct dom_node *b,
                                unsigned long b_offset)
{
    const struct dom_node *a_root = a, *b_root = b;
    const struct dom_node *a_child, *b_child, *cursor;

    if (a == b)
        return a_offset < b_offset ? -1 :
               a_offset > b_offset ? 1 : 0;
    while (a_root && a_root->parent)
        a_root = a_root->parent;
    while (b_root && b_root->parent)
        b_root = b_root->parent;
    if (a_root != b_root)
        return 0;
    if (range_is_ancestor(a, b)) {
        b_child = range_child_below(a, b);
        return b_child && range_child_index(b_child) < a_offset ? 1 : -1;
    }
    if (range_is_ancestor(b, a)) {
        a_child = range_child_below(b, a);
        return a_child && range_child_index(a_child) < b_offset ? -1 : 1;
    }
    a_child = a;
    b_child = b;
    while (a_child->parent != b_child->parent) {
        if (a_child->parent && range_is_ancestor(a_child->parent, b_child))
            b_child = range_child_below(a_child->parent, b_child);
        else
            a_child = a_child->parent;
    }
    for (cursor = a_child; cursor; cursor = cursor->next_sibling)
        if (cursor == b_child)
            return -1;
    return 1;
}

static js_value range_wrap_node(struct jsdom *j, struct dom_node *node)
{
    js_value document;

    if (node == j->doc->root &&
        js_get(j->ctx, js_global(j->ctx), "document", &document) == JS_OK)
        return document;
    return wrap_node(j, node);
}

static int range_boundary_arg(js_ctx *c, int ac, js_value *av,
                              struct dom_node **node,
                              unsigned long *offset)
{
    struct hostref *h;
    double requested;
    unsigned long length;

    if (ac < 2 || !(h = node_or_document(av[0])))
        return js_throw_error(c, JS_ERR_TYPE,
                              "Range boundary requires a Node and offset");
    if (js_to_integer(c, av[1], &requested) != JS_OK)
        return JS_THROW;
    *node = receiver_node(h);
    length = range_node_length(*node);
    if (requested < 0 || requested > (double)length)
        return throw_dom_exception(c, "IndexSizeError",
                                   "Range offset is outside the node");
    *offset = (unsigned long)requested;
    return JS_OK;
}

static int range_boundary_get(js_ctx *c, js_value t,
                              int which, js_value *r)
{
    struct hostref *h;
    struct range_state *state = range_state_of(t, &h);

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Range receiver");
    if (which == 0 || which == 2)
        *r = range_wrap_node(h->j, which == 0
                            ? state->start_container
                            : state->end_container);
    else
        *r = js_number(which == 1
                       ? state->start_offset : state->end_offset);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

#define RANGE_BOUNDARY_GETTER(name, which)                                  \
static int name(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)    \
{ (void)ac; (void)av; return range_boundary_get(c, t, which, r); }
RANGE_BOUNDARY_GETTER(range_start_container_get, 0)
RANGE_BOUNDARY_GETTER(range_start_offset_get, 1)
RANGE_BOUNDARY_GETTER(range_end_container_get, 2)
RANGE_BOUNDARY_GETTER(range_end_offset_get, 3)
#undef RANGE_BOUNDARY_GETTER

static int range_collapsed_get(js_ctx *c, js_value t,
                               int ac, js_value *av, js_value *r)
{
    struct range_state *state = range_state_of(t, 0);
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Range receiver");
    *r = js_bool(state->start_container == state->end_container &&
                 state->start_offset == state->end_offset);
    return JS_OK;
}

static struct dom_node *range_common_ancestor(struct range_state *state)
{
    struct dom_node *left, *right;

    for (left = state->start_container; left; left = left->parent)
        for (right = state->end_container; right; right = right->parent)
            if (left == right)
                return left;
    return 0;
}

static int range_common_get(js_ctx *c, js_value t,
                            int ac, js_value *av, js_value *r)
{
    struct hostref *h;
    struct range_state *state = range_state_of(t, &h);
    struct dom_node *common;
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Range receiver");
    common = range_common_ancestor(state);
    *r = common ? range_wrap_node(h->j, common) : js_null();
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int range_set_boundary(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r,
                              int start)
{
    struct range_state *state = range_state_of(t, 0);
    struct dom_node *node;
    unsigned long offset;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Range receiver");
    if (range_boundary_arg(c, ac, av, &node, &offset) != JS_OK)
        return JS_THROW;
    if (start) {
        state->start_container = node;
        state->start_offset = offset;
        if (range_compare_points(node, offset,
                                 state->end_container,
                                 state->end_offset) > 0) {
            state->end_container = node;
            state->end_offset = offset;
        }
    } else {
        state->end_container = node;
        state->end_offset = offset;
        if (range_compare_points(state->start_container,
                                 state->start_offset,
                                 node, offset) > 0) {
            state->start_container = node;
            state->start_offset = offset;
        }
    }
    *r = js_undefined();
    return JS_OK;
}

static int range_set_start(js_ctx *c, js_value t,
                           int ac, js_value *av, js_value *r)
{
    return range_set_boundary(c, t, ac, av, r, 1);
}

static int range_set_end(js_ctx *c, js_value t,
                         int ac, js_value *av, js_value *r)
{
    return range_set_boundary(c, t, ac, av, r, 0);
}

static int range_set_relative(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r,
                              int start, int after)
{
    struct hostref *node_host = ac ? node_or_document(av[0]) : 0;
    struct dom_node *node = receiver_node(node_host);
    js_value arguments[2];

    if (!node_host || !node || !node->parent)
        return throw_dom_exception(c, "InvalidNodeTypeError",
                                   "Range node has no parent");
    arguments[0] = range_wrap_node(node_host->j, node->parent);
    arguments[1] = js_number(range_child_index(node) + (after ? 1 : 0));
    return range_set_boundary(c, t, 2, arguments, r, start);
}

#define RANGE_RELATIVE(name, start, after)                                  \
static int name(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)    \
{ return range_set_relative(c, t, ac, av, r, start, after); }
RANGE_RELATIVE(range_set_start_before, 1, 0)
RANGE_RELATIVE(range_set_start_after, 1, 1)
RANGE_RELATIVE(range_set_end_before, 0, 0)
RANGE_RELATIVE(range_set_end_after, 0, 1)
#undef RANGE_RELATIVE

static int range_select_node(js_ctx *c, js_value t,
                             int ac, js_value *av, js_value *r)
{
    struct range_state *state = range_state_of(t, 0);
    struct hostref *node_host = ac ? node_or_document(av[0]) : 0;
    struct dom_node *node = receiver_node(node_host);
    unsigned long index;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Range receiver");
    if (!node || !node->parent)
        return throw_dom_exception(c, "InvalidNodeTypeError",
                                   "Range node has no parent");
    index = range_child_index(node);
    state->start_container = node->parent;
    state->end_container = node->parent;
    state->start_offset = index;
    state->end_offset = index + 1;
    *r = js_undefined();
    return JS_OK;
}

static int range_select_contents(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct range_state *state = range_state_of(t, 0);
    struct hostref *node_host = ac ? node_or_document(av[0]) : 0;
    struct dom_node *node = receiver_node(node_host);

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Range receiver");
    if (!node)
        return js_throw_error(c, JS_ERR_TYPE,
                              "selectNodeContents requires a Node");
    state->start_container = node;
    state->end_container = node;
    state->start_offset = 0;
    state->end_offset = range_node_length(node);
    *r = js_undefined();
    return JS_OK;
}

static int range_collapse(js_ctx *c, js_value t,
                          int ac, js_value *av, js_value *r)
{
    struct range_state *state = range_state_of(t, 0);
    int to_start = ac > 0 && js_to_boolean(av[0]);

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Range receiver");
    if (to_start) {
        state->end_container = state->start_container;
        state->end_offset = state->start_offset;
    } else {
        state->start_container = state->end_container;
        state->start_offset = state->end_offset;
    }
    *r = js_undefined();
    return JS_OK;
}

static js_value range_new(struct jsdom *j, struct range_state *source)
{
    struct range_state *state =
        (struct range_state *)calloc(1, sizeof(*state));
    js_object *object;

    if (!state)
        return js_undefined();
    if (source)
        *state = *source;
    else
        state->start_container = state->end_container = j->doc->root;
    object = new_host_data(j, 0, HOST_RANGE, state, j->range_proto);
    if (!object) {
        free(state);
        return js_undefined();
    }
    return js_object_value(object);
}

static int range_clone(js_ctx *c, js_value t,
                       int ac, js_value *av, js_value *r)
{
    struct hostref *h;
    struct range_state *state = range_state_of(t, &h);
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Range receiver");
    *r = range_new(h->j, state);
    return js_is_object(*r) ? JS_OK :
        js_throw_error(c, JS_ERR_ERROR, "out of memory");
}

static int range_constructor(js_ctx *c, js_value t,
                             int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    (void)t; (void)ac; (void)av;

    if (!j || !js_is_constructing(c))
        return js_throw_error(c, JS_ERR_TYPE, "Range requires new");
    *r = range_new(j, 0);
    return js_is_object(*r) ? JS_OK :
        js_throw_error(c, JS_ERR_ERROR, "out of memory");
}

static int doc_create_range(js_ctx *c, js_value t,
                            int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Document receiver");
    *r = range_new(h->j, 0);
    return js_is_object(*r) ? JS_OK :
        js_throw_error(c, JS_ERR_ERROR, "out of memory");
}

static int range_to_string(js_ctx *c, js_value t,
                           int ac, js_value *av, js_value *r)
{
    struct range_state *state = range_state_of(t, 0);
    struct dom_node *root, *node;
    char *output = 0;
    unsigned long used = 0, capacity = 0;
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Range receiver");
    if (state->start_container == state->end_container &&
        (state->start_container->type == DOM_TEXT ||
         state->start_container->type == DOM_COMMENT)) {
        *r = js_mkstring(c,
            state->start_container->text + state->start_offset,
            state->end_offset - state->start_offset);
        return js_fatal(c) ? JS_THROW : JS_OK;
    }
    root = range_common_ancestor(state);
    for (node = root; node; node = dom_next_within(node, root)) {
        unsigned long begin = 0, end, length;
        char *grown;

        if (node->type != DOM_TEXT)
            continue;
        end = node->text_len;
        if (range_compare_points(node, end,
                                 state->start_container,
                                 state->start_offset) <= 0 ||
            range_compare_points(node, 0,
                                 state->end_container,
                                 state->end_offset) >= 0)
            continue;
        if (node == state->start_container)
            begin = state->start_offset;
        if (node == state->end_container)
            end = state->end_offset;
        length = end > begin ? end - begin : 0;
        if (!length)
            continue;
        if (used + length > DOM_MAX_TEXT) {
            free(output);
            return js_throw_error(c, JS_ERR_RANGE,
                                  "Range text is too large");
        }
        if (used + length + 1 > capacity) {
            capacity = used + length + 1;
            grown = (char *)realloc(output, capacity);
            if (!grown) {
                free(output);
                return js_throw_error(c, JS_ERR_ERROR, "out of memory");
            }
            output = grown;
        }
        memcpy(output + used, node->text + begin, length);
        used += length;
    }
    if (!output) {
        *r = js_mkcstring(c, "");
    } else {
        output[used] = 0;
        *r = js_mkstring(c, output, used);
        free(output);
    }
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int range_create_contextual_fragment(js_ctx *c, js_value t,
                                            int ac, js_value *av,
                                            js_value *r)
{
    struct hostref *h;
    struct range_state *state = range_state_of(t, &h);
    struct dom_node *container, *fragment, *child, *ctx_node;
    struct dom_document *dst_doc;
    const char *markup;
    int dirty, ss_dirty;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Range receiver");
    if (string_arg(c, ac, av, 0, &markup) != JS_OK)
        return JS_THROW;
    ctx_node = state->start_container;
    dst_doc = (ctx_node && ctx_node->doc) ? ctx_node->doc : h->j->doc;
    while (ctx_node && ctx_node->type != DOM_ELEMENT)
        ctx_node = ctx_node->parent;
    container = dom_create_element(dst_doc, "div", 3);
    fragment = dom_create_fragment(dst_doc);
    dirty = h->j->dirty;
    ss_dirty = h->j->style_sheet_dirty;
    if (!container || !fragment ||
        !set_inner_html_ctx(h->j, container, ctx_node, markup))
        return js_throw_error(c, JS_ERR_ERROR,
                              "cannot parse contextual fragment");
    while ((child = container->first_child) != 0) {
        dom_remove_child(container, child);
        if (!dom_append_child(fragment, child))
            return js_throw_error(c, JS_ERR_ERROR,
                                  "cannot build contextual fragment");
    }
    h->j->dirty = dirty;
    h->j->style_sheet_dirty = ss_dirty;
    *r = wrap_node(h->j, fragment);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int range_clone_contents(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    struct hostref *h;
    struct range_state *state = range_state_of(t, &h);
    struct dom_node *fragment, *node;
    unsigned long index;
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Range receiver");
    fragment = dom_create_fragment(h->j->doc);
    if (!fragment)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    if (state->start_container == state->end_container &&
        (state->start_container->type == DOM_TEXT ||
         state->start_container->type == DOM_COMMENT)) {
        node = dom_create_text(
            h->j->doc,
            state->start_container->text + state->start_offset,
            state->end_offset - state->start_offset);
        if (node && !dom_append_child(fragment, node))
            node = 0;
        if (!node && state->start_offset != state->end_offset)
            return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    } else if (state->start_container == state->end_container) {
        for (index = state->start_offset;
             index < state->end_offset; index++) {
            struct dom_node *child =
                range_child_at(state->start_container, index);
            struct dom_node *copy = child
                ? clone_subtree(h->j, child, 0) : 0;

            if (!copy || !dom_append_child(fragment, copy))
                return js_throw_error(c, JS_ERR_ERROR,
                                      "cannot clone Range contents");
        }
    } else {
        js_value text;
        const char *bytes;
        unsigned long length;

        if (range_to_string(c, t, 0, 0, &text) != JS_OK)
            return JS_THROW;
        bytes = js_string_bytes(text, &length);
        node = length ? dom_create_text(h->j->doc, bytes, length) : 0;
        if (node && !dom_append_child(fragment, node))
            return js_throw_error(c, JS_ERR_ERROR,
                                  "cannot clone Range contents");
    }
    *r = wrap_node(h->j, fragment);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int range_delete_contents(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct hostref *h;
    struct range_state *state = range_state_of(t, &h);
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Range receiver");
    if (state->start_container == state->end_container &&
        (state->start_container->type == DOM_TEXT ||
         state->start_container->type == DOM_COMMENT)) {
        struct dom_node *old = state->start_container;
        unsigned long prefix = state->start_offset;
        unsigned long suffix = old->text_len - state->end_offset;
        char *text = (char *)malloc(prefix + suffix + 1);
        js_value node_value, argument, ignored;
        struct hostref *node_host;

        if (!text)
            return js_throw_error(c, JS_ERR_ERROR, "out of memory");
        memcpy(text, old->text, prefix);
        memcpy(text + prefix, old->text + state->end_offset, suffix);
        text[prefix + suffix] = 0;
        node_value = wrap_node(h->j, old);
        argument = js_mkstring(c, text, prefix + suffix);
        free(text);
        if (js_fatal(c) ||
            node_node_value_set(c, node_value, 1,
                                &argument, &ignored) != JS_OK)
            return JS_THROW;
        node_host = host(node_value, HOST_NODE);
        state->start_container = state->end_container =
            node_host ? node_host->node : old;
        state->end_offset = state->start_offset;
    } else if (state->start_container == state->end_container) {
        struct dom_node *container = state->start_container;
        unsigned long count = state->end_offset - state->start_offset;

        while (count--) {
            struct dom_node *child =
                range_child_at(container, state->start_offset);
            struct dom_node *previous, *next;

            if (!child)
                break;
            previous = child->prev_sibling;
            next = child->next_sibling;
            dom_remove_child(container, child);
            if (mutation_child(h->j, container, 0, child,
                               previous, next) != JS_OK)
                return JS_THROW;
        }
        state->end_offset = state->start_offset;
        h->j->dirty = 1;
    } else {
        return throw_dom_exception(c, "NotSupportedError",
                                   "complex Range deletion is not supported");
    }
    *r = js_undefined();
    return JS_OK;
}

static int range_extract_contents(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    js_value fragment, ignored;
    (void)ac; (void)av;

    if (range_clone_contents(c, t, 0, 0, &fragment) != JS_OK ||
        range_delete_contents(c, t, 0, 0, &ignored) != JS_OK)
        return JS_THROW;
    *r = fragment;
    return JS_OK;
}

static int range_insert_node(js_ctx *c, js_value t,
                             int ac, js_value *av, js_value *r)
{
    struct range_state *state = range_state_of(t, 0);
    struct hostref *node_host = ac ? host(av[0], HOST_NODE) : 0;
    struct dom_node *reference;
    js_value container, arguments[2], ignored;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Range receiver");
    if (!node_host)
        return js_throw_error(c, JS_ERR_TYPE,
                              "insertNode requires a Node");
    if (state->start_container->type == DOM_TEXT ||
        state->start_container->type == DOM_COMMENT ||
        state->start_container->type == DOM_DOCUMENT)
        return throw_dom_exception(c, "NotSupportedError",
                                   "this Range insertion point is unsupported");
    reference = range_child_at(state->start_container,
                               state->start_offset);
    container = wrap_node(node_host->j, state->start_container);
    arguments[0] = av[0];
    arguments[1] = reference ? wrap_node(node_host->j, reference) : js_null();
    if (node_insert_before(c, container, 2, arguments, &ignored) != JS_OK)
        return JS_THROW;
    *r = js_undefined();
    return JS_OK;
}

static int range_compare_point_method(js_ctx *c, js_value t,
                                      int ac, js_value *av, js_value *r,
                                      int boolean_result)
{
    struct range_state *state = range_state_of(t, 0);
    struct dom_node *node;
    unsigned long offset;
    int before, after;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Range receiver");
    if (range_boundary_arg(c, ac, av, &node, &offset) != JS_OK)
        return JS_THROW;
    before = range_compare_points(node, offset,
                                  state->start_container,
                                  state->start_offset) < 0;
    after = range_compare_points(node, offset,
                                 state->end_container,
                                 state->end_offset) > 0;
    *r = boolean_result ? js_bool(!before && !after)
                        : js_number(before ? -1 : after ? 1 : 0);
    return JS_OK;
}

static int range_compare_point(js_ctx *c, js_value t,
                               int ac, js_value *av, js_value *r)
{
    return range_compare_point_method(c, t, ac, av, r, 0);
}

static int range_is_point_in(js_ctx *c, js_value t,
                             int ac, js_value *av, js_value *r)
{
    return range_compare_point_method(c, t, ac, av, r, 1);
}

static int range_compare_boundaries(js_ctx *c, js_value t,
                                    int ac, js_value *av, js_value *r)
{
    struct range_state *left = range_state_of(t, 0);
    struct range_state *right =
        ac > 1 ? range_state_of(av[1], 0) : 0;
    const struct dom_node *left_node, *right_node;
    unsigned long left_offset, right_offset;
    double how;

    if (!left)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Range receiver");
    if (ac < 2 || !right || js_to_integer(c, av[0], &how) != JS_OK)
        return ac < 2 || !right
            ? js_throw_error(c, JS_ERR_TYPE,
                             "compareBoundaryPoints requires a Range")
            : JS_THROW;
    if (how == 0) {
        left_node = left->start_container;
        left_offset = left->start_offset;
        right_node = right->start_container;
        right_offset = right->start_offset;
    } else if (how == 1) {
        left_node = left->end_container;
        left_offset = left->end_offset;
        right_node = right->start_container;
        right_offset = right->start_offset;
    } else if (how == 2) {
        left_node = left->end_container;
        left_offset = left->end_offset;
        right_node = right->end_container;
        right_offset = right->end_offset;
    } else if (how == 3) {
        left_node = left->start_container;
        left_offset = left->start_offset;
        right_node = right->end_container;
        right_offset = right->end_offset;
    } else {
        return throw_dom_exception(c, "NotSupportedError",
                                   "invalid Range comparison mode");
    }
    *r = js_number(range_compare_points(left_node, left_offset,
                                        right_node, right_offset));
    return JS_OK;
}

static int range_intersects_node(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct range_state *range = range_state_of(t, 0);
    struct hostref *node_host = ac ? node_or_document(av[0]) : 0;
    struct dom_node *node = receiver_node(node_host);
    unsigned long index;

    if (!range)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Range receiver");
    if (!node)
        return js_throw_error(c, JS_ERR_TYPE,
                              "intersectsNode requires a Node");
    if (!node->parent) {
        *r = js_bool(range_is_ancestor(node, range->start_container) ||
                     range_is_ancestor(node, range->end_container));
        return JS_OK;
    }
    index = range_child_index(node);
    *r = js_bool(
        range_compare_points(node->parent, index + 1,
                             range->start_container,
                             range->start_offset) > 0 &&
        range_compare_points(node->parent, index,
                             range->end_container,
                             range->end_offset) < 0);
    return JS_OK;
}

static int range_surround_contents(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    struct hostref *new_parent =
        ac ? host(av[0], HOST_NODE) : 0;
    js_value fragment, ignored, append_argument;

    if (!range_state_of(t, 0))
        return js_throw_error(c, JS_ERR_TYPE, "illegal Range receiver");
    if (!new_parent || new_parent->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE,
                              "surroundContents requires an Element");
    if (range_extract_contents(c, t, 0, 0, &fragment) != JS_OK ||
        range_insert_node(c, t, 1, av, &ignored) != JS_OK)
        return JS_THROW;
    append_argument = fragment;
    if (node_append(c, av[0], 1, &append_argument, &ignored) != JS_OK ||
        range_select_node(c, t, 1, av, &ignored) != JS_OK)
        return JS_THROW;
    *r = js_undefined();
    return JS_OK;
}

static int range_detach(js_ctx *c, js_value t,
                        int ac, js_value *av, js_value *r)
{
    (void)c; (void)t; (void)ac; (void)av;
    *r = js_undefined();
    return JS_OK;
}

static struct selection_state *selection_state_of(js_value value,
                                                  struct hostref **owner)
{
    struct hostref *h = host(value, HOST_SELECTION);

    if (owner)
        *owner = h;
    return h ? (struct selection_state *)h->data : 0;
}

static int global_get_selection(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    (void)t; (void)ac; (void)av;

    if (!j)
        return js_throw_error(c, JS_ERR_TYPE, "missing browser realm");
    *r = j->selection;
    return JS_OK;
}

static int selection_range_count_get(js_ctx *c, js_value t,
                                     int ac, js_value *av, js_value *r)
{
    struct selection_state *state = selection_state_of(t, 0);
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Selection receiver");
    *r = js_number(state->has_range ? 1 : 0);
    return JS_OK;
}

static int selection_boundary_get(js_ctx *c, js_value t,
                                  int which, js_value *r)
{
    struct hostref *h;
    struct selection_state *selection = selection_state_of(t, &h);
    struct range_state *range;

    if (!selection)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Selection receiver");
    range = selection->has_range ? range_state_of(selection->range, 0) : 0;
    if (!range) {
        *r = (which == 0 || which == 2) ? js_null() : js_number(0);
    } else if (which == 0 || which == 2) {
        *r = range_wrap_node(h->j, which == 0
                            ? range->start_container
                            : range->end_container);
    } else {
        *r = js_number(which == 1
                       ? range->start_offset : range->end_offset);
    }
    return js_fatal(c) ? JS_THROW : JS_OK;
}

#define SELECTION_BOUNDARY_GETTER(name, which)                              \
static int name(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)    \
{ (void)ac; (void)av; return selection_boundary_get(c, t, which, r); }
SELECTION_BOUNDARY_GETTER(selection_anchor_node_get, 0)
SELECTION_BOUNDARY_GETTER(selection_anchor_offset_get, 1)
SELECTION_BOUNDARY_GETTER(selection_focus_node_get, 2)
SELECTION_BOUNDARY_GETTER(selection_focus_offset_get, 3)
#undef SELECTION_BOUNDARY_GETTER

static int selection_collapsed_get(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    struct selection_state *selection = selection_state_of(t, 0);
    struct range_state *range;
    (void)ac; (void)av;

    if (!selection)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Selection receiver");
    range = selection->has_range ? range_state_of(selection->range, 0) : 0;
    *r = js_bool(!range ||
                 (range->start_container == range->end_container &&
                  range->start_offset == range->end_offset));
    return JS_OK;
}

static int selection_type_get(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    struct selection_state *selection = selection_state_of(t, 0);
    struct range_state *range;
    const char *type;
    (void)ac; (void)av;

    if (!selection)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Selection receiver");
    range = selection->has_range ? range_state_of(selection->range, 0) : 0;
    type = !range ? "None" :
        range->start_container == range->end_container &&
        range->start_offset == range->end_offset ? "Caret" : "Range";
    *r = js_mkcstring(c, type);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int selection_direction_get(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    if (!selection_state_of(t, 0))
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Selection receiver");
    (void)ac; (void)av;
    *r = js_mkcstring(c, "forward");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int selection_get_range_at(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    struct selection_state *state = selection_state_of(t, 0);
    double index;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Selection receiver");
    if (js_to_integer(c, ac ? av[0] : js_undefined(), &index) != JS_OK)
        return JS_THROW;
    if (!state->has_range || index != 0)
        return throw_dom_exception(c, "IndexSizeError",
                                   "Selection range index is invalid");
    *r = state->range;
    return JS_OK;
}

static int selection_add_range(js_ctx *c, js_value t,
                               int ac, js_value *av, js_value *r)
{
    struct selection_state *state = selection_state_of(t, 0);

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Selection receiver");
    if (ac < 1 || !range_state_of(av[0], 0))
        return js_throw_error(c, JS_ERR_TYPE,
                              "addRange requires a Range");
    state->range = av[0];
    state->has_range = 1;
    *r = js_undefined();
    return JS_OK;
}

static int selection_remove_all(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    struct selection_state *state = selection_state_of(t, 0);
    (void)c; (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Selection receiver");
    state->range = js_undefined();
    state->has_range = 0;
    *r = js_undefined();
    return JS_OK;
}

static int selection_collapse(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    struct hostref *h;
    struct selection_state *selection = selection_state_of(t, &h);
    struct range_state *range;
    struct dom_node *node;
    unsigned long offset;

    if (!selection)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Selection receiver");
    if (ac && js_is_null(av[0]))
        return selection_remove_all(c, t, ac, av, r);
    if (range_boundary_arg(c, ac, av, &node, &offset) != JS_OK)
        return JS_THROW;
    if (!selection->has_range) {
        selection->range = range_new(h->j, 0);
        if (!js_is_object(selection->range))
            return js_throw_error(c, JS_ERR_ERROR, "out of memory");
        selection->has_range = 1;
    }
    range = range_state_of(selection->range, 0);
    range->start_container = range->end_container = node;
    range->start_offset = range->end_offset = offset;
    *r = js_undefined();
    return JS_OK;
}

static int selection_select_all(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    struct hostref *h;
    struct selection_state *selection = selection_state_of(t, &h);
    struct hostref *node_host = ac ? node_or_document(av[0]) : 0;
    struct dom_node *node = receiver_node(node_host);
    struct range_state *range;

    if (!selection)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Selection receiver");
    if (!node)
        return js_throw_error(c, JS_ERR_TYPE,
                              "selectAllChildren requires a Node");
    if (!selection->has_range) {
        selection->range = range_new(h->j, 0);
        if (!js_is_object(selection->range))
            return js_throw_error(c, JS_ERR_ERROR, "out of memory");
        selection->has_range = 1;
    }
    range = range_state_of(selection->range, 0);
    range->start_container = range->end_container = node;
    range->start_offset = 0;
    range->end_offset = range_node_length(node);
    *r = js_undefined();
    return JS_OK;
}

static int selection_to_string(js_ctx *c, js_value t,
                               int ac, js_value *av, js_value *r)
{
    struct selection_state *state = selection_state_of(t, 0);

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Selection receiver");
    if (!state->has_range) {
        *r = js_mkcstring(c, "");
        return js_fatal(c) ? JS_THROW : JS_OK;
    }
    return range_to_string(c, state->range, ac, av, r);
}

static int selection_delete_from_document(js_ctx *c, js_value t,
                                          int ac, js_value *av, js_value *r)
{
    struct selection_state *state = selection_state_of(t, 0);

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Selection receiver");
    if (!state->has_range) {
        *r = js_undefined();
        return JS_OK;
    }
    return range_delete_contents(c, state->range, ac, av, r);
}

static int selection_remove_range(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    struct selection_state *state = selection_state_of(t, 0);

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Selection receiver");
    if (ac < 1 || !range_state_of(av[0], 0))
        return js_throw_error(c, JS_ERR_TYPE,
                              "removeRange requires a Range");
    if (state->has_range &&
        state->range.u.obj == av[0].u.obj) {
        state->range = js_undefined();
        state->has_range = 0;
    }
    *r = js_undefined();
    return JS_OK;
}

static int selection_collapse_edge(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r,
                                   int start)
{
    struct selection_state *selection = selection_state_of(t, 0);
    struct range_state *range;
    (void)ac; (void)av;

    if (!selection)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Selection receiver");
    if (!selection->has_range)
        return throw_dom_exception(c, "InvalidStateError",
                                   "Selection has no Range");
    range = range_state_of(selection->range, 0);
    if (start) {
        range->end_container = range->start_container;
        range->end_offset = range->start_offset;
    } else {
        range->start_container = range->end_container;
        range->start_offset = range->end_offset;
    }
    *r = js_undefined();
    return JS_OK;
}

static int selection_collapse_to_start(js_ctx *c, js_value t,
                                       int ac, js_value *av, js_value *r)
{
    return selection_collapse_edge(c, t, ac, av, r, 1);
}

static int selection_collapse_to_end(js_ctx *c, js_value t,
                                     int ac, js_value *av, js_value *r)
{
    return selection_collapse_edge(c, t, ac, av, r, 0);
}

static int selection_contains_node(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    struct selection_state *selection = selection_state_of(t, 0);
    struct range_state *range;
    struct hostref *node_host = ac ? node_or_document(av[0]) : 0;
    struct dom_node *node = receiver_node(node_host);
    unsigned long index;
    int partial = ac > 1 && js_to_boolean(av[1]);
    int start_cmp, end_cmp;

    if (!selection)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Selection receiver");
    if (!node)
        return js_throw_error(c, JS_ERR_TYPE,
                              "containsNode requires a Node");
    range = selection->has_range ? range_state_of(selection->range, 0) : 0;
    if (!range || !node->parent) {
        *r = js_bool(0);
        return JS_OK;
    }
    index = range_child_index(node);
    start_cmp = range_compare_points(
        node->parent, index, range->start_container, range->start_offset);
    end_cmp = range_compare_points(
        node->parent, index + 1, range->end_container, range->end_offset);
    *r = js_bool(partial
        ? range_compare_points(node->parent, index + 1,
                               range->start_container,
                               range->start_offset) > 0 &&
          range_compare_points(node->parent, index,
                               range->end_container,
                               range->end_offset) < 0
        : start_cmp >= 0 && end_cmp <= 0);
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
    *r = wrap_node(h->j, dom_get_element_by_id(doc_of(h), id));
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int doc_query(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    struct dom_node *match = 0;
    const char *sel;
    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal Document receiver");
    if (string_arg(c, ac, av, 0, &sel) != JS_OK) return JS_THROW;
    if (query_first(c, h->j, doc_of(h)->root, sel, &match) != JS_OK)
        return JS_THROW;
    *r = wrap_node(h->j, match);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int doc_query_all(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    const char *sel;
    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal Document receiver");
    if (string_arg(c, ac, av, 0, &sel) != JS_OK) return JS_THROW;
    return query_all(c, h->j, doc_of(h)->root, sel, r);
}

static int doc_create_element(js_ctx *c, js_value t, int ac, js_value *av,
                              js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    const char *name;
    struct dom_node *n;
    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal Document receiver");
    if (string_arg(c, ac, av, 0, &name) != JS_OK) return JS_THROW;
    n = dom_create_element(doc_of(h), name, (long)strlen(name));
    if (!n) return js_throw_error(c, JS_ERR_ERROR, "cannot create element");
    *r = wrap_node(h->j, n);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int doc_create_element_ns(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    const char *namespace_uri = 0, *name;
    const char *colon;
    struct dom_node *node;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Document receiver");
    if (ac < 2 || string_arg(c, ac, av, 1, &name) != JS_OK)
        return ac < 2
            ? js_throw_error(c, JS_ERR_TYPE,
                             "createElementNS requires a qualified name")
            : JS_THROW;
    if (!js_is_null(av[0]) && !js_is_undefined(av[0])) {
        if (string_arg(c, ac, av, 0, &namespace_uri) != JS_OK)
            return JS_THROW;
        if (!*namespace_uri)
            namespace_uri = 0;
    }
    colon = strchr(name, ':');
    if (!*name || (colon && (colon == name || !colon[1] ||
                             strchr(colon + 1, ':'))) ||
        (colon && !namespace_uri))
        return throw_dom_exception(c, "NamespaceError",
                                   "invalid qualified element name");
    node = dom_create_element(doc_of(h), name, (long)strlen(name));
    if (!node || !dom_set_namespace(node, namespace_uri, -1))
        return js_throw_error(c, JS_ERR_ERROR,
                              "cannot create namespaced element");
    *r = wrap_node(h->j, node);
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
    n = dom_create_text(doc_of(h), text, strlen(text));
    if (!n) return js_throw_error(c, JS_ERR_ERROR, "cannot create text node");
    *r = wrap_node(h->j, n);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int character_data_construct(js_ctx *c, int ac, js_value *av,
                                    js_value *r, int type)
{
    struct jsdom *j = binding(c);
    struct dom_node *node;
    const char *text = "";

    if (!js_is_constructing(c))
        return js_throw_error(c, JS_ERR_TYPE,
                              "%s requires new",
                              type == DOM_TEXT ? "Text" : "Comment");
    if (!j)
        return js_throw_error(c, JS_ERR_ERROR,
                              "DOM binding is unavailable");
    if (ac > 0 && string_arg(c, ac, av, 0, &text) != JS_OK)
        return JS_THROW;
    node = type == DOM_TEXT
        ? dom_create_text(j->doc, text, strlen(text))
        : dom_create_comment(j->doc, text, strlen(text));
    if (!node)
        return js_throw_error(c, JS_ERR_ERROR,
                              "cannot create character data");
    *r = wrap_node(j, node);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int text_constructor(js_ctx *c, js_value t,
                            int ac, js_value *av, js_value *r)
{
    (void)t;
    return character_data_construct(c, ac, av, r, DOM_TEXT);
}

static int comment_constructor(js_ctx *c, js_value t,
                               int ac, js_value *av, js_value *r)
{
    (void)t;
    return character_data_construct(c, ac, av, r, DOM_COMMENT);
}

static int doc_create_comment(js_ctx *c, js_value t, int ac, js_value *av,
                              js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    const char *text;
    struct dom_node *node;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Document receiver");
    if (string_arg(c, ac, av, 0, &text) != JS_OK)
        return JS_THROW;
    node = dom_create_comment(doc_of(h), text, strlen(text));
    if (!node)
        return js_throw_error(c, JS_ERR_ERROR,
                              "cannot create comment");
    *r = wrap_node(h->j, node);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int doc_create_fragment(js_ctx *c, js_value t,
                               int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    struct dom_node *node;
    (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal Document receiver");
    node = dom_create_fragment(doc_of(h));
    if (!node)
        return js_throw_error(c, JS_ERR_ERROR,
                              "cannot create document fragment");
    *r = wrap_node(h->j, node);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int doc_root_get(js_ctx *c, js_value t, int ac, js_value *av,
                        js_value *r, int which)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    struct dom_document *d;
    struct dom_node *n = 0;
    (void)ac; (void)av;
    if (h) {
        d = doc_of(h);
        if (which == 0) {
            n = d->html;
            if (!n && d->root) {
                for (n = d->root->first_child; n && n->type != DOM_ELEMENT;
                     n = n->next_sibling)
                    ;
            }
        }
        if (which == 1) n = d->head;
        if (which == 2) n = d->body;
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
    struct dom_document *d = doc_of(h);
    (void)ac; (void)av;
    *r = js_mkcstring(c, h && d && d->title ? d->title : "");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int doc_title_set(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    struct dom_document *d;
    struct dom_node *title;
    const char *v;
    if (!h) return js_throw_error(c, JS_ERR_TYPE, "illegal Document receiver");
    if (string_arg(c, ac, av, 0, &v) != JS_OK) return JS_THROW;
    d = doc_of(h);
    if (!d->head) {
        *r = js_undefined();
        return JS_OK;
    }
    title = dom_find_tag(d->head, "title");
    if (!title) {
        title = dom_create_element(d, "title", 5);
        if (!title || !dom_append_child(d->head, title))
            return js_throw_error(c, JS_ERR_ERROR, "cannot create title");
    }
    if (!set_text(h->j, title, v))
        return js_throw_error(c, JS_ERR_ERROR, "cannot set title");
    d->title = title->first_child ? title->first_child->text : "";
    mark_dom_dirty(h->j);
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

static int doc_base_url_get(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    (void)ac; (void)av;
    *r = js_mkcstring(c, h && h->j->base_url ? h->j->base_url : "");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int doc_referrer_get(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    (void)t; (void)ac; (void)av;
    *r = js_mkcstring(c, "");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int doc_charset_get(js_ctx *c, js_value t, int ac, js_value *av,
                           js_value *r)
{
    (void)t; (void)ac; (void)av;
    *r = js_mkcstring(c, "UTF-8");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int doc_content_type_get(js_ctx *c, js_value t, int ac, js_value *av,
                                js_value *r)
{
    struct hostref *h = host(t, HOST_DOCUMENT);
    const char *ct = "text/html";
    (void)ac; (void)av;
    if (h && h->data && ((const struct doc_ref *)h->data)->content_type[0])
        ct = ((const struct doc_ref *)h->data)->content_type;
    *r = js_mkcstring(c, ct);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int doc_compat_mode_get(js_ctx *c, js_value t, int ac, js_value *av,
                               js_value *r)
{
    (void)t; (void)ac; (void)av;
    *r = js_mkcstring(c, "CSS1Compat");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int doc_hidden_get(js_ctx *c, js_value t, int ac, js_value *av,
                          js_value *r)
{
    (void)c; (void)t; (void)ac; (void)av;
    *r = js_bool(0);
    return JS_OK;
}

static int doc_visibility_get(js_ctx *c, js_value t, int ac, js_value *av,
                              js_value *r)
{
    (void)t; (void)ac; (void)av;
    *r = js_mkcstring(c, "visible");
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

enum location_component {
    LOCATION_HREF = 0,
    LOCATION_ORIGIN,
    LOCATION_PROTOCOL,
    LOCATION_HOST,
    LOCATION_HOSTNAME,
    LOCATION_PORT,
    LOCATION_PATHNAME,
    LOCATION_SEARCH,
    LOCATION_HASH
};

static int valid_scheme(const char *s, unsigned long length);

static int location_component_get(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r,
                                  int component)
{
    struct hostref *h = host(t, HOST_LOCATION);
    struct url parsed;
    char output[URL_MAX];
    char host_name[URL_HOST_MAX + 3];
    (void)ac; (void)av;

    if (!h || !h->j->url ||
        url_parse(h->j->url, &parsed) != URL_OK)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Location receiver");
    output[0] = 0;
    if (parsed.is_ipv6)
        snprintf(host_name, sizeof(host_name), "[%s]", parsed.host);
    else
        snprintf(host_name, sizeof(host_name), "%s", parsed.host);
    switch (component) {
    case LOCATION_HREF:
        snprintf(output, sizeof(output), "%s", h->j->url);
        break;
    case LOCATION_ORIGIN:
        if (!strcmp(parsed.scheme, "file") || !parsed.has_authority)
            snprintf(output, sizeof(output), "null");
        else if (url_origin(&parsed, output, sizeof(output)) != URL_OK)
            return js_throw_error(c, JS_ERR_RANGE,
                                  "Location origin is too long");
        break;
    case LOCATION_PROTOCOL:
        snprintf(output, sizeof(output), "%s:", parsed.scheme);
        break;
    case LOCATION_HOST:
        if (parsed.port >= 0)
            snprintf(output, sizeof(output), "%s:%d",
                     host_name, parsed.port);
        else
            snprintf(output, sizeof(output), "%s", host_name);
        break;
    case LOCATION_HOSTNAME:
        snprintf(output, sizeof(output), "%s", host_name);
        break;
    case LOCATION_PORT:
        if (parsed.port >= 0)
            snprintf(output, sizeof(output), "%d", parsed.port);
        break;
    case LOCATION_PATHNAME:
        snprintf(output, sizeof(output), "%s", parsed.path);
        break;
    case LOCATION_SEARCH:
        if (parsed.has_query)
            snprintf(output, sizeof(output), "?%s", parsed.query);
        break;
    case LOCATION_HASH:
        if (parsed.has_fragment)
            snprintf(output, sizeof(output), "#%s", parsed.fragment);
        break;
    default:
        return js_throw_error(c, JS_ERR_TYPE,
                              "unknown Location component");
    }
    *r = js_mkcstring(c, output);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int location_component_set(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r,
                                  int component)
{
    struct hostref *h = host(t, HOST_LOCATION);
    struct url parsed, candidate;
    js_value value;
    const char *bytes;
    unsigned long length, i, port = 0;
    char encoded[URL_PATH_MAX];
    char serialized[URL_MAX], candidate_text[URL_MAX];
    long written;
    int prefix, changed = 0, n;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Location receiver");
    if (js_to_string(c, ac ? av[0] : js_undefined(), &value) != JS_OK)
        return JS_THROW;
    bytes = js_string_bytes(value, &length);
    if (component == LOCATION_HREF) {
        if (!queue_navigation(h->j, bytes))
            return js_throw_error(c, JS_ERR_TYPE,
                                  "invalid navigation URL");
        *r = js_undefined();
        return JS_OK;
    }
    if (url_parse(h->j->url, &parsed) != URL_OK)
        return js_throw_error(c, JS_ERR_TYPE, "invalid current URL");
    switch (component) {
    case LOCATION_PROTOCOL:
        if (length && bytes[length - 1] == ':')
            length--;
        if (length < sizeof(parsed.scheme) &&
            valid_scheme(bytes, length)) {
            for (i = 0; i < length; i++)
                parsed.scheme[i] = (char)ci((unsigned char)bytes[i]);
            parsed.scheme[length] = 0;
            changed = 1;
        }
        break;
    case LOCATION_HOST:
    case LOCATION_HOSTNAME:
        n = snprintf(candidate_text, sizeof(candidate_text), "%s://%.*s/",
                     parsed.scheme, (int)length, bytes);
        if (n >= 0 && (unsigned long)n < sizeof(candidate_text) &&
            url_parse(candidate_text, &candidate) == URL_OK &&
            candidate.has_authority && !candidate.has_userinfo &&
            candidate.host[0]) {
            memcpy(parsed.host, candidate.host, sizeof(parsed.host));
            parsed.is_ipv6 = candidate.is_ipv6;
            parsed.has_authority = 1;
            if (component == LOCATION_HOST)
                parsed.port = candidate.port;
            changed = 1;
        }
        break;
    case LOCATION_PORT:
        if (!length) {
            parsed.port = -1;
            changed = 1;
            break;
        }
        for (i = 0; i < length && bytes[i] >= '0' && bytes[i] <= '9'; i++)
            port = port * 10 + (unsigned long)(bytes[i] - '0');
        if (i == length && port <= 65535) {
            parsed.port = (int)port;
            changed = 1;
        }
        break;
    case LOCATION_PATHNAME:
        prefix = parsed.has_authority && (!length || bytes[0] != '/');
        written = url_pct_encode(bytes, length, URL_COMP_PATH,
                                 encoded + prefix,
                                 sizeof(encoded) - (unsigned long)prefix);
        if (written >= 0) {
            if (prefix) {
                encoded[0] = '/';
                written++;
            }
            memcpy(parsed.path, encoded, (unsigned long)written + 1);
            changed = 1;
        }
        break;
    case LOCATION_SEARCH:
        if (!length) {
            parsed.query[0] = 0;
            parsed.has_query = 0;
            changed = 1;
        } else {
            if (bytes[0] == '?') {
                bytes++;
                length--;
            }
            written = url_pct_encode(bytes, length, URL_COMP_QUERY,
                                     parsed.query, sizeof(parsed.query));
            if (written >= 0) {
                parsed.has_query = 1;
                changed = 1;
            }
        }
        break;
    case LOCATION_HASH:
        if (!length) {
            parsed.fragment[0] = 0;
            parsed.has_fragment = 0;
            changed = 1;
        } else {
            if (bytes[0] == '#') {
                bytes++;
                length--;
            }
            written = url_pct_encode(bytes, length, URL_COMP_FRAGMENT,
                                     parsed.fragment,
                                     sizeof(parsed.fragment));
            if (written >= 0) {
                parsed.has_fragment = 1;
                changed = 1;
            }
        }
        break;
    default:
        break;
    }
    if (changed) {
        url_normalize(&parsed, URL_N_ALL);
        if (url_serialize(&parsed, serialized, sizeof(serialized)) != URL_OK ||
            !queue_navigation(h->j, serialized))
            return js_throw_error(c, JS_ERR_RANGE,
                                  "Location value is too long");
    }
    *r = js_undefined();
    return JS_OK;
}

static int location_href_get(js_ctx *c, js_value t, int ac, js_value *av,
                             js_value *r)
{
    return location_component_get(c, t, ac, av, r, LOCATION_HREF);
}

static int location_href_set(js_ctx *c, js_value t, int ac, js_value *av,
                             js_value *r)
{
    return location_component_set(c, t, ac, av, r, LOCATION_HREF);
}

#define LOCATION_ACCESSOR(name, component)                                   \
static int name##_get(js_ctx *c, js_value t, int ac, js_value *av,           \
                      js_value *r)                                            \
{ return location_component_get(c, t, ac, av, r, component); }               \
static int name##_set(js_ctx *c, js_value t, int ac, js_value *av,           \
                      js_value *r)                                            \
{ return location_component_set(c, t, ac, av, r, component); }
LOCATION_ACCESSOR(location_protocol, LOCATION_PROTOCOL)
LOCATION_ACCESSOR(location_host, LOCATION_HOST)
LOCATION_ACCESSOR(location_hostname, LOCATION_HOSTNAME)
LOCATION_ACCESSOR(location_port, LOCATION_PORT)
LOCATION_ACCESSOR(location_pathname, LOCATION_PATHNAME)
LOCATION_ACCESSOR(location_search, LOCATION_SEARCH)
LOCATION_ACCESSOR(location_hash, LOCATION_HASH)
#undef LOCATION_ACCESSOR

static int location_origin_get(js_ctx *c, js_value t, int ac, js_value *av,
                               js_value *r)
{
    return location_component_get(c, t, ac, av, r, LOCATION_ORIGIN);
}

static int location_assign(js_ctx *c, js_value t, int ac, js_value *av,
                           js_value *r)
{
    return location_href_set(c, t, ac, av, r);
}

static int location_reload(js_ctx *c, js_value t, int ac, js_value *av,
                           js_value *r)
{
    struct hostref *h = host(t, HOST_LOCATION);
    (void)ac; (void)av;
    if (!h || !queue_navigation(h->j, h->j->url))
        return js_throw_error(c, JS_ERR_ERROR,
                              "cannot reload current URL");
    *r = js_undefined();
    return JS_OK;
}

static int location_to_string(js_ctx *c, js_value t, int ac,
                              js_value *av, js_value *r)
{
    return location_href_get(c, t, ac, av, r);
}

/* ------------------------------------------------------------------ */
/* Same-document History API.                                         */

static struct history_state *history_of(js_value value,
                                        struct jsdom **owner)
{
    struct hostref *h = host(value, HOST_HISTORY);

    if (owner)
        *owner = h ? h->j : 0;
    return h ? (struct history_state *)h->data : 0;
}

static int history_set_document_url(struct jsdom *j, const char *url)
{
    char *url_copy = dup_z(url);
    char *base_copy = 0;

    if (!url_copy)
        return 0;
    if (j->base_follows_url) {
        base_copy = dup_z(url);
        if (!base_copy) {
            free(url_copy);
            return 0;
        }
    }
    free(j->url);
    j->url = url_copy;
    if (j->base_follows_url) {
        free(j->base_url);
        j->base_url = base_copy;
    }
    return 1;
}

static int history_resolve_url(js_ctx *c, struct jsdom *j,
                               int ac, js_value *av, char *output)
{
    struct url current, resolved;
    js_value value;
    const char *reference;

    if (ac < 3 || js_is_undefined(av[2])) {
        if (strlen(j->url) >= URL_MAX)
            return js_throw_error(c, JS_ERR_RANGE,
                                  "history URL is too long");
        strcpy(output, j->url);
        return JS_OK;
    }
    if (js_to_string(c, av[2], &value) != JS_OK)
        return JS_THROW;
    reference = js_string_bytes(value, 0);
    if (url_resolve_str(j->base_url, reference,
                        output, URL_MAX) != URL_OK ||
        url_parse(j->url, &current) != URL_OK ||
        url_parse(output, &resolved) != URL_OK)
        return js_throw_error(c, JS_ERR_TYPE, "invalid history URL");
    url_normalize(&resolved, URL_N_ALL);
    if (!url_same_origin(&current, &resolved))
        return js_throw_error(c, JS_ERR_ERROR,
                              "history URL must be same-origin");
    if (url_serialize(&resolved, output, URL_MAX) != URL_OK)
        return js_throw_error(c, JS_ERR_RANGE,
                              "history URL is too long");
    return JS_OK;
}

static int history_length_get(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    struct history_state *history = history_of(t, 0);
    (void)c; (void)ac; (void)av;

    if (!history)
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid History receiver");
    *r = js_number(history->count);
    return JS_OK;
}

static int history_state_get(js_ctx *c, js_value t,
                             int ac, js_value *av, js_value *r)
{
    struct history_state *history = history_of(t, 0);
    (void)ac; (void)av;

    if (!history)
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid History receiver");
    *r = history->entries[history->index].state;
    return JS_OK;
}

static int history_scroll_get(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    struct history_state *history = history_of(t, 0);
    (void)ac; (void)av;

    if (!history)
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid History receiver");
    *r = js_mkcstring(c, history->scroll_restoration);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int history_scroll_set(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    struct history_state *history = history_of(t, 0);
    js_value value;
    const char *text;

    if (!history)
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid History receiver");
    if (js_to_string(c, ac ? av[0] : js_undefined(), &value) != JS_OK)
        return JS_THROW;
    text = js_string_bytes(value, 0);
    if (strcmp(text, "auto") && strcmp(text, "manual"))
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid scrollRestoration value");
    strcpy(history->scroll_restoration, text);
    *r = js_undefined();
    return JS_OK;
}

static int history_change_state(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r,
                                int replace)
{
    struct jsdom *j;
    struct history_state *history = history_of(t, &j);
    char url[URL_MAX];
    char *entry_url;
    unsigned int i;
    js_value state = ac ? av[0] : js_undefined();

    if (!history || !j)
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid History receiver");
    if (history_resolve_url(c, j, ac, av, url) != JS_OK)
        return JS_THROW;
    entry_url = dup_z(url);
    if (!entry_url || !history_set_document_url(j, url)) {
        free(entry_url);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    if (replace) {
        free(history->entries[history->index].url);
        history->entries[history->index].url = entry_url;
        history->entries[history->index].state = state;
    } else {
        for (i = history->index + 1; i < history->count; i++) {
            free(history->entries[i].url);
            history->entries[i].url = 0;
            history->entries[i].state = js_undefined();
        }
        history->count = history->index + 1;
        if (history->count == JSDOM_HISTORY_MAX) {
            free(history->entries[0].url);
            memmove(&history->entries[0], &history->entries[1],
                    sizeof(history->entries[0]) *
                    (JSDOM_HISTORY_MAX - 1));
            history->count--;
            history->index--;
        }
        history->index++;
        history->entries[history->index].url = entry_url;
        history->entries[history->index].state = state;
        history->count = history->index + 1;
    }
    *r = js_undefined();
    return JS_OK;
}

static int history_push_state(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    return history_change_state(c, t, ac, av, r, 0);
}

static int history_replace_state(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    return history_change_state(c, t, ac, av, r, 1);
}

static int history_dispatch_popstate(struct jsdom *j, js_value state)
{
    js_value event;
    char err[160];

    if (event_make(j, j->event_proto, "popstate", js_undefined(),
                   js_undefined(), 0, &event) != JS_OK ||
        js_set(j->ctx, event, "state", state) != JS_OK)
        return JS_THROW;
    err[0] = 0;
    return dispatch_event_value(j, js_global(j->ctx), event, 0,
                                err, sizeof(err)) < 0
        ? JS_THROW : JS_OK;
}

static int history_go(js_ctx *c, js_value t,
                      int ac, js_value *av, js_value *r)
{
    struct jsdom *j;
    struct history_state *history = history_of(t, &j);
    double delta = 0;
    double target;
    unsigned int next;

    if (!history || !j)
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid History receiver");
    if (ac && js_to_integer(c, av[0], &delta) != JS_OK)
        return JS_THROW;
    if (delta == 0) {
        if (!queue_navigation(j, j->url))
            return js_throw_error(c, JS_ERR_ERROR,
                                  "cannot reload history entry");
        *r = js_undefined();
        return JS_OK;
    }
    target = (double)history->index + delta;
    if (target < 0 || target >= (double)history->count) {
        *r = js_undefined();
        return JS_OK;
    }
    next = (unsigned int)target;
    if (!history_set_document_url(j, history->entries[next].url))
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    history->index = next;
    if (history_dispatch_popstate(j, history->entries[next].state) != JS_OK)
        return JS_THROW;
    *r = js_undefined();
    return JS_OK;
}

static int history_back(js_ctx *c, js_value t,
                        int ac, js_value *av, js_value *r)
{
    js_value delta = js_number(-1);
    (void)ac; (void)av;
    return history_go(c, t, 1, &delta, r);
}

static int history_forward(js_ctx *c, js_value t,
                           int ac, js_value *av, js_value *r)
{
    js_value delta = js_number(1);
    (void)ac; (void)av;
    return history_go(c, t, 1, &delta, r);
}

/* ------------------------------------------------------------------ */
/* URL - composed over libweb's bounded parser and libjs URLSearchParams. */

static struct url_state *url_object_state(js_value value)
{
    struct hostref *h = host(value, HOST_URL);

    return h ? (struct url_state *)h->data : 0;
}

static int url_snapshot_params(js_ctx *c, struct url_state *state)
{
    js_value serialized;
    const char *bytes;
    unsigned long length;

    if (js_urlsearchparams_string(c, state->search_params,
                                  &serialized) != JS_OK)
        return JS_THROW;
    bytes = js_string_bytes(serialized, &length);
    if (!bytes || length >= sizeof(state->params_snapshot))
        return js_throw_error(c, JS_ERR_RANGE,
                              "URL query exceeds the bounded URL size");
    memcpy(state->params_snapshot, bytes, length);
    state->params_snapshot[length] = 0;
    return JS_OK;
}

static int url_sync_params(js_ctx *c, struct url_state *state)
{
    js_value serialized;
    const char *bytes;
    unsigned long length, old_length;

    if (js_urlsearchparams_string(c, state->search_params,
                                  &serialized) != JS_OK)
        return JS_THROW;
    bytes = js_string_bytes(serialized, &length);
    old_length = strlen(state->params_snapshot);
    if (length == old_length &&
        (!length || !memcmp(bytes, state->params_snapshot, length)))
        return JS_OK;
    if (!bytes || length >= sizeof(state->parsed.query))
        return js_throw_error(c, JS_ERR_RANGE,
                              "URL query exceeds the bounded URL size");
    memcpy(state->parsed.query, bytes, length);
    state->parsed.query[length] = 0;
    state->parsed.has_query = length != 0;
    memcpy(state->params_snapshot, bytes, length + 1);
    return JS_OK;
}

static int url_state_init(js_ctx *c, struct url_state *state,
                          const struct url *parsed)
{
    state->parsed = *parsed;
    state->search_params = js_urlsearchparams_new(
        c, parsed->query, strlen(parsed->query));
    if (!js_is_object(state->search_params))
        return JS_THROW;
    return url_snapshot_params(c, state);
}

static int url_state_replace(js_ctx *c, struct url_state *state,
                             const struct url *parsed)
{
    if (js_urlsearchparams_replace(c, state->search_params,
                                   parsed->query,
                                   strlen(parsed->query)) != JS_OK)
        return JS_THROW;
    state->parsed = *parsed;
    return url_snapshot_params(c, state);
}

static int url_serialize_state(js_ctx *c, struct url_state *state,
                               char *output, unsigned long output_size)
{
    if (url_sync_params(c, state) != JS_OK)
        return JS_THROW;
    if (url_serialize(&state->parsed, output, output_size) != URL_OK)
        return js_throw_error(c, JS_ERR_RANGE, "URL is too long");
    return JS_OK;
}

static int url_parse_input(js_ctx *c, int ac, js_value *av,
                           struct url *parsed)
{
    js_value input_string, base_string;
    const char *input, *base;
    char *input_copy;
    char absolute[URL_MAX];
    int rc;

    if (ac < 1)
        return js_throw_error(c, JS_ERR_TYPE, "URL requires an input");
    if (js_to_string(c, av[0], &input_string) != JS_OK)
        return JS_THROW;
    input = js_string_bytes(input_string, 0);
    input_copy = dup_z(input);
    if (!input_copy)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    if (ac > 1 && !js_is_undefined(av[1])) {
        if (js_to_string(c, av[1], &base_string) != JS_OK) {
            free(input_copy);
            return JS_THROW;
        }
        base = js_string_bytes(base_string, 0);
        rc = url_resolve_str(base, input_copy, absolute, sizeof(absolute));
        free(input_copy);
        if (rc != URL_OK)
            return js_throw_error(c, JS_ERR_TYPE, "invalid URL or base");
        rc = url_parse(absolute, parsed);
    } else {
        rc = url_parse(input_copy, parsed);
        free(input_copy);
    }
    if (rc != URL_OK || !parsed->has_scheme)
        return js_throw_error(c, JS_ERR_TYPE, "invalid absolute URL");
    url_normalize(parsed, URL_N_ALL);
    return JS_OK;
}

static int url_constructor(js_ctx *c, js_value t, int ac, js_value *av,
                           js_value *r)
{
    struct jsdom *j = binding(c);
    struct url parsed;
    struct url_state *state;
    js_object *object;
    (void)t;

    if (!j || !js_is_constructing(c))
        return js_throw_error(c, JS_ERR_TYPE,
                              "URL must be constructed with new");
    if (url_parse_input(c, ac, av, &parsed) != JS_OK)
        return JS_THROW;
    state = (struct url_state *)calloc(1, sizeof(*state));
    if (!state)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    if (url_state_init(c, state, &parsed) != JS_OK) {
        free(state);
        return JS_THROW;
    }
    object = new_host_data(j, 0, HOST_URL, state, j->url_proto);
    if (!object) {
        free(state);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    *r = js_object_value(object);
    return JS_OK;
}

static int url_href_get(js_ctx *c, js_value t, int ac, js_value *av,
                        js_value *r)
{
    struct url_state *state = url_object_state(t);
    char serialized[URL_MAX];
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    if (url_serialize_state(c, state, serialized, sizeof(serialized)) != JS_OK)
        return JS_THROW;
    *r = js_mkcstring(c, serialized);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int url_href_set(js_ctx *c, js_value t, int ac, js_value *av,
                        js_value *r)
{
    struct url_state *state = url_object_state(t);
    js_value input_string;
    const char *input;
    char current[URL_MAX], resolved[URL_MAX];
    struct url parsed;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    if (ac < 1 || js_to_string(c, av[0], &input_string) != JS_OK)
        return JS_THROW;
    input = js_string_bytes(input_string, 0);
    if (url_serialize_state(c, state, current, sizeof(current)) != JS_OK)
        return JS_THROW;
    if (url_resolve_str(current, input, resolved, sizeof(resolved)) != URL_OK ||
        url_parse(resolved, &parsed) != URL_OK || !parsed.has_scheme)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL");
    url_normalize(&parsed, URL_N_ALL);
    if (url_state_replace(c, state, &parsed) != JS_OK)
        return JS_THROW;
    *r = js_undefined();
    return JS_OK;
}

static int url_origin_get(js_ctx *c, js_value t, int ac, js_value *av,
                          js_value *r)
{
    struct url_state *state = url_object_state(t);
    char origin[URL_MAX];
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    if (!strcmp(state->parsed.scheme, "file") ||
        !state->parsed.has_authority) {
        *r = js_mkcstring(c, "null");
    } else if (url_origin(&state->parsed, origin, sizeof(origin)) == URL_OK) {
        *r = js_mkcstring(c, origin);
    } else {
        return js_throw_error(c, JS_ERR_RANGE, "URL origin is too long");
    }
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int url_protocol_get(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    struct url_state *state = url_object_state(t);
    char protocol[URL_SCHEME_MAX + 2];
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    snprintf(protocol, sizeof(protocol), "%s:", state->parsed.scheme);
    *r = js_mkcstring(c, protocol);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int valid_scheme(const char *s, unsigned long length)
{
    unsigned long i;

    if (!length || !((s[0] >= 'A' && s[0] <= 'Z') ||
                     (s[0] >= 'a' && s[0] <= 'z')))
        return 0;
    for (i = 1; i < length; i++)
        if (!((s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= 'a' && s[i] <= 'z') ||
              (s[i] >= '0' && s[i] <= '9') ||
              s[i] == '+' || s[i] == '-' || s[i] == '.'))
            return 0;
    return 1;
}

static int url_protocol_set(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    struct url_state *state = url_object_state(t);
    js_value value;
    const char *bytes;
    unsigned long length, i;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    if (js_to_string(c, ac ? av[0] : js_undefined(), &value) != JS_OK)
        return JS_THROW;
    bytes = js_string_bytes(value, &length);
    if (length && bytes[length - 1] == ':')
        length--;
    if (length < sizeof(state->parsed.scheme) &&
        valid_scheme(bytes, length)) {
        for (i = 0; i < length; i++)
            state->parsed.scheme[i] =
                (char)(bytes[i] >= 'A' && bytes[i] <= 'Z'
                    ? bytes[i] + ('a' - 'A') : bytes[i]);
        state->parsed.scheme[length] = 0;
    }
    *r = js_undefined();
    return JS_OK;
}

static void userinfo_parts(const struct url *url, const char **password,
                           unsigned long *username_length)
{
    const char *colon = strchr(url->userinfo, ':');

    *username_length = colon ? (unsigned long)(colon - url->userinfo)
                             : strlen(url->userinfo);
    *password = colon ? colon + 1 : "";
}

static int url_username_get(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    struct url_state *state = url_object_state(t);
    const char *password;
    unsigned long username_length;
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    userinfo_parts(&state->parsed, &password, &username_length);
    (void)password;
    *r = js_mkstring(c, state->parsed.userinfo, username_length);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int url_password_get(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    struct url_state *state = url_object_state(t);
    const char *password;
    unsigned long username_length;
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    userinfo_parts(&state->parsed, &password, &username_length);
    *r = js_mkcstring(c, password);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int url_userinfo_set(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r, int password_part)
{
    struct url_state *state = url_object_state(t);
    js_value value;
    const char *bytes, *old_password;
    unsigned long length, username_length;
    char encoded[URL_USERINFO_MAX], combined[URL_USERINFO_MAX];
    long encoded_length;
    int written;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    if (js_to_string(c, ac ? av[0] : js_undefined(), &value) != JS_OK)
        return JS_THROW;
    bytes = js_string_bytes(value, &length);
    encoded_length = url_pct_encode(bytes, length, URL_COMP_HOST,
                                    encoded, sizeof(encoded));
    if (encoded_length < 0) {
        *r = js_undefined();
        return JS_OK;
    }
    userinfo_parts(&state->parsed, &old_password, &username_length);
    if (password_part)
        written = snprintf(combined, sizeof(combined), "%.*s:%s",
                           (int)username_length, state->parsed.userinfo,
                           encoded);
    else if (*old_password)
        written = snprintf(combined, sizeof(combined), "%s:%s",
                           encoded, old_password);
    else
        written = snprintf(combined, sizeof(combined), "%s", encoded);
    if (written >= 0 && (unsigned long)written < sizeof(combined)) {
        memcpy(state->parsed.userinfo, combined, (unsigned long)written + 1);
        state->parsed.has_userinfo = written != 0;
    }
    *r = js_undefined();
    return JS_OK;
}

static int url_username_set(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    return url_userinfo_set(c, t, ac, av, r, 0);
}

static int url_password_set(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    return url_userinfo_set(c, t, ac, av, r, 1);
}

static int url_hostname_string(js_ctx *c, struct url_state *state,
                               js_value *r)
{
    char hostname[URL_HOST_MAX + 3];

    if (state->parsed.is_ipv6)
        snprintf(hostname, sizeof(hostname), "[%s]", state->parsed.host);
    else
        snprintf(hostname, sizeof(hostname), "%s", state->parsed.host);
    *r = js_mkcstring(c, hostname);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int url_host_get(js_ctx *c, js_value t, int ac, js_value *av,
                        js_value *r)
{
    struct url_state *state = url_object_state(t);
    char host_name[URL_HOST_MAX + 3], output[URL_HOST_MAX + 16];
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    if (state->parsed.is_ipv6)
        snprintf(host_name, sizeof(host_name), "[%s]", state->parsed.host);
    else
        snprintf(host_name, sizeof(host_name), "%s", state->parsed.host);
    if (state->parsed.port >= 0)
        snprintf(output, sizeof(output), "%s:%d",
                 host_name, state->parsed.port);
    else
        snprintf(output, sizeof(output), "%s", host_name);
    *r = js_mkcstring(c, output);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int url_hostname_get(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    struct url_state *state = url_object_state(t);
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    return url_hostname_string(c, state, r);
}

static int url_host_set_common(js_ctx *c, js_value t, int ac, js_value *av,
                               js_value *r, int keep_port)
{
    struct url_state *state = url_object_state(t);
    js_value value;
    const char *input;
    char candidate[URL_MAX];
    struct url parsed;
    int written;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    if (js_to_string(c, ac ? av[0] : js_undefined(), &value) != JS_OK)
        return JS_THROW;
    input = js_string_bytes(value, 0);
    written = snprintf(candidate, sizeof(candidate), "%s://%s/",
                       state->parsed.scheme, input);
    if (written >= 0 && (unsigned long)written < sizeof(candidate) &&
        url_parse(candidate, &parsed) == URL_OK &&
        parsed.has_authority && !parsed.has_userinfo && parsed.host[0]) {
        if (keep_port)
            parsed.port = state->parsed.port;
        memcpy(state->parsed.host, parsed.host, sizeof(parsed.host));
        state->parsed.is_ipv6 = parsed.is_ipv6;
        state->parsed.port = parsed.port;
        state->parsed.has_authority = 1;
    }
    *r = js_undefined();
    return JS_OK;
}

static int url_host_set(js_ctx *c, js_value t, int ac, js_value *av,
                        js_value *r)
{
    return url_host_set_common(c, t, ac, av, r, 0);
}

static int url_hostname_set(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    return url_host_set_common(c, t, ac, av, r, 1);
}

static int url_port_get(js_ctx *c, js_value t, int ac, js_value *av,
                        js_value *r)
{
    struct url_state *state = url_object_state(t);
    char port[16];
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    if (state->parsed.port < 0)
        port[0] = 0;
    else
        snprintf(port, sizeof(port), "%d", state->parsed.port);
    *r = js_mkcstring(c, port);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int url_port_set(js_ctx *c, js_value t, int ac, js_value *av,
                        js_value *r)
{
    struct url_state *state = url_object_state(t);
    js_value value;
    const char *bytes;
    unsigned long length, i;
    unsigned long port = 0;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    if (js_to_string(c, ac ? av[0] : js_undefined(), &value) != JS_OK)
        return JS_THROW;
    bytes = js_string_bytes(value, &length);
    if (!length) {
        state->parsed.port = -1;
    } else {
        for (i = 0; i < length && bytes[i] >= '0' && bytes[i] <= '9'; i++)
            port = port * 10 + (unsigned long)(bytes[i] - '0');
        if (i == length && port <= 65535)
            state->parsed.port =
                port == (unsigned long)url_default_port(state->parsed.scheme)
                    ? -1 : (int)port;
    }
    *r = js_undefined();
    return JS_OK;
}

static int url_pathname_get(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    struct url_state *state = url_object_state(t);
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    *r = js_mkcstring(c, state->parsed.path);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int url_pathname_set(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    struct url_state *state = url_object_state(t);
    js_value value;
    const char *bytes;
    unsigned long length;
    char path[URL_PATH_MAX];
    long written;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    if (js_to_string(c, ac ? av[0] : js_undefined(), &value) != JS_OK)
        return JS_THROW;
    bytes = js_string_bytes(value, &length);
    written = url_pct_encode(bytes, length, URL_COMP_PATH,
                             path + (state->parsed.has_authority &&
                                     (!length || bytes[0] != '/')),
                             sizeof(path) -
                             (state->parsed.has_authority &&
                              (!length || bytes[0] != '/')));
    if (written >= 0) {
        if (state->parsed.has_authority && (!length || bytes[0] != '/')) {
            path[0] = '/';
            written++;
        }
        memcpy(state->parsed.path, path, (unsigned long)written + 1);
        url_normalize(&state->parsed, URL_N_DOTS);
    }
    *r = js_undefined();
    return JS_OK;
}

static int url_search_get(js_ctx *c, js_value t, int ac, js_value *av,
                          js_value *r)
{
    struct url_state *state = url_object_state(t);
    char search[URL_QUERY_MAX + 2];
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    if (url_sync_params(c, state) != JS_OK)
        return JS_THROW;
    if (state->parsed.has_query)
        snprintf(search, sizeof(search), "?%s", state->parsed.query);
    else
        search[0] = 0;
    *r = js_mkcstring(c, search);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int url_search_set(js_ctx *c, js_value t, int ac, js_value *av,
                          js_value *r)
{
    struct url_state *state = url_object_state(t);
    js_value value;
    const char *bytes;
    unsigned long length;
    const char *query;
    char encoded[URL_QUERY_MAX];
    long written;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    if (js_to_string(c, ac ? av[0] : js_undefined(), &value) != JS_OK)
        return JS_THROW;
    bytes = js_string_bytes(value, &length);
    if (!length) {
        encoded[0] = 0;
        state->parsed.has_query = 0;
    } else {
        query = bytes[0] == '?' ? bytes + 1 : bytes;
        length -= bytes[0] == '?';
        written = url_pct_encode(query, length, URL_COMP_QUERY,
                                 encoded, sizeof(encoded));
        if (written < 0) {
            *r = js_undefined();
            return JS_OK;
        }
        state->parsed.has_query = 1;
    }
    if (js_urlsearchparams_replace(c, state->search_params,
                                   encoded, strlen(encoded)) != JS_OK)
        return JS_THROW;
    memcpy(state->parsed.query, encoded, strlen(encoded) + 1);
    if (url_snapshot_params(c, state) != JS_OK)
        return JS_THROW;
    *r = js_undefined();
    return JS_OK;
}

static int url_search_params_get(js_ctx *c, js_value t, int ac,
                                 js_value *av, js_value *r)
{
    struct url_state *state = url_object_state(t);
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    *r = state->search_params;
    return JS_OK;
}

static int url_hash_get(js_ctx *c, js_value t, int ac, js_value *av,
                        js_value *r)
{
    struct url_state *state = url_object_state(t);
    char hash[URL_FRAG_MAX + 2];
    (void)ac; (void)av;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    if (state->parsed.has_fragment)
        snprintf(hash, sizeof(hash), "#%s", state->parsed.fragment);
    else
        hash[0] = 0;
    *r = js_mkcstring(c, hash);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int url_hash_set(js_ctx *c, js_value t, int ac, js_value *av,
                        js_value *r)
{
    struct url_state *state = url_object_state(t);
    js_value value;
    const char *bytes, *fragment;
    unsigned long length;
    long written;

    if (!state)
        return js_throw_error(c, JS_ERR_TYPE, "invalid URL receiver");
    if (js_to_string(c, ac ? av[0] : js_undefined(), &value) != JS_OK)
        return JS_THROW;
    bytes = js_string_bytes(value, &length);
    if (!length) {
        state->parsed.fragment[0] = 0;
        state->parsed.has_fragment = 0;
    } else {
        fragment = bytes[0] == '#' ? bytes + 1 : bytes;
        length -= bytes[0] == '#';
        written = url_pct_encode(fragment, length, URL_COMP_FRAGMENT,
                                 state->parsed.fragment,
                                 sizeof(state->parsed.fragment));
        if (written >= 0)
            state->parsed.has_fragment = 1;
    }
    *r = js_undefined();
    return JS_OK;
}

static int url_to_string(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    return url_href_get(c, t, ac, av, r);
}

static int url_can_parse(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct url parsed;
    int rc;
    (void)t;

    if (ac < 1) {
        *r = js_bool(0);
        return JS_OK;
    }
    rc = url_parse_input(c, ac, av, &parsed);
    if (rc == JS_OK) {
        *r = js_bool(1);
        return JS_OK;
    }
    if (js_fatal(c))
        return JS_THROW;
    js_clear_exception(c);
    *r = js_bool(0);
    return JS_OK;
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

static int queue_timer(js_ctx *c, int ac, js_value *av, js_value *r,
                       int repeats)
{
    struct jsdom *j = binding(c);
    double delay = 0;
    unsigned int turns = 1;
    int i;

    if (!j || ac < 1 || !js_is_function(av[0]))
        return js_throw_error(c, JS_ERR_TYPE,
                              "setTimeout requires a function");
    if (ac - 2 > JSDOM_TIMER_ARGS_MAX)
        return js_throw_error(c, JS_ERR_RANGE,
                              "too many timer callback arguments");
    if (ac > 1 && js_to_number(c, av[1], &delay) != JS_OK)
        return JS_THROW;
    if (delay > 0) {
        if (delay > 86400000.0)
            delay = 86400000.0;
        turns = (unsigned int)((delay + 199.0) / 200.0);
        if (!turns)
            turns = 1;
    }
    for (i = 0; i < (int)(sizeof(j->timers) / sizeof(j->timers[0])); i++)
        if (j->timers[i].state == TASK_FREE) {
            int argument;

            memset(&j->timers[i], 0, sizeof(j->timers[i]));
            j->timers[i].fn = av[0];
            j->timers[i].state = TASK_QUEUED;
            j->timers[i].repeats = repeats ? 1 : 0;
            j->timers[i].interval_turns = turns;
            j->timers[i].due_turn = j->event_turn + turns;
            j->timers[i].argc = ac > 2 ? (unsigned char)(ac - 2) : 0;
            for (argument = 0; argument < j->timers[i].argc; argument++)
                j->timers[i].args[argument] = av[argument + 2];
            *r = js_number((double)(i + 1));
            return JS_OK;
        }
    return js_throw_error(c, JS_ERR_RANGE, "too many pending timers");
}

static int global_set_timeout(js_ctx *c, js_value t, int ac, js_value *av,
                              js_value *r)
{
    (void)t;
    return queue_timer(c, ac, av, r, 0);
}

static int global_set_interval(js_ctx *c, js_value t, int ac, js_value *av,
                               js_value *r)
{
    (void)t;
    return queue_timer(c, ac, av, r, 1);
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
    if (j && id > 0 &&
        id <= (int)(sizeof(j->timers) / sizeof(j->timers[0]))) {
        memset(&j->timers[id - 1], 0, sizeof(j->timers[id - 1]));
        j->timers[id - 1].fn = js_undefined();
    }
    *r = js_undefined();
    return JS_OK;
}

static int global_animation_frame(js_ctx *c, js_value t, int ac,
                                  js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    int i;
    (void)t;

    if (!j || ac < 1 || !js_is_function(av[0]))
        return js_throw_error(c, JS_ERR_TYPE,
                              "requestAnimationFrame requires a function");
    for (i = 0; i < (int)(sizeof(j->timers) / sizeof(j->timers[0])); i++)
        if (j->timers[i].state == TASK_FREE) {
            memset(&j->timers[i], 0, sizeof(j->timers[i]));
            j->timers[i].fn = av[0];
            j->timers[i].state = TASK_QUEUED;
            j->timers[i].interval_turns = 1;
            j->timers[i].due_turn = j->event_turn + 1;
            j->timers[i].callback_kind = 1;
            *r = js_number((double)(i + 1));
            return JS_OK;
        }
    return js_throw_error(c, JS_ERR_RANGE,
                          "too many pending animation frames");
}

static int idle_time_remaining(js_ctx *c, js_value t, int ac,
                               js_value *av, js_value *r)
{
    (void)c; (void)t; (void)ac; (void)av;
    *r = js_number(10);
    return JS_OK;
}

static int global_idle_callback(js_ctx *c, js_value t, int ac,
                                js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    int i;
    (void)t;

    if (!j || ac < 1 || !js_is_function(av[0]))
        return js_throw_error(c, JS_ERR_TYPE,
                              "requestIdleCallback requires a function");
    for (i = 0; i < (int)(sizeof(j->timers) / sizeof(j->timers[0])); i++)
        if (j->timers[i].state == TASK_FREE) {
            memset(&j->timers[i], 0, sizeof(j->timers[i]));
            j->timers[i].fn = av[0];
            j->timers[i].state = TASK_QUEUED;
            j->timers[i].interval_turns = 1;
            j->timers[i].due_turn = j->event_turn + 1;
            j->timers[i].callback_kind = 2;
            *r = js_number((double)(i + 1));
            return JS_OK;
        }
    return js_throw_error(c, JS_ERR_RANGE,
                          "too many pending idle callbacks");
}

static int global_queue_microtask(js_ctx *c, js_value t, int ac,
                                  js_value *av, js_value *r)
{
    js_value promise, then, ignored;
    (void)t;

    if (ac < 1 || !js_is_function(av[0]))
        return js_throw_error(c, JS_ERR_TYPE,
                              "queueMicrotask requires a function");
    promise = js_promise_new(c);
    if (!js_is_promise(promise) ||
        js_promise_resolve(c, promise, js_undefined()) != JS_OK ||
        js_get(c, promise, "then", &then) != JS_OK ||
        js_call(c, then, promise, 1, av, &ignored) != JS_OK)
        return JS_THROW;
    *r = js_undefined();
    return JS_OK;
}

static int global_performance_now(js_ctx *c, js_value t, int ac,
                                  js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    (void)t; (void)ac; (void)av;
    *r = js_number(j ? (double)(j->event_turn * 200UL) : 0);
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
static const char *const valign_names[] = {
    CSS_VALIGN_LIST(CSS_NAME_ENTRY)
};
static const char *const whitespace_names[] = {
    CSS_WHITESPACE_LIST(CSS_NAME_ENTRY)
};
static const char *const overflow_names[] = {
    CSS_OVERFLOW_LIST(CSS_NAME_ENTRY)
};
static const char *const visibility_names[] = {
    CSS_VISIBILITY_LIST(CSS_NAME_ENTRY)
};
static const char *const fontstyle_names[] = {
    CSS_FONTSTYLE_LIST(CSS_NAME_ENTRY)
};
static const char *const fontfamily_names[] = {
    CSS_FONTFAMILY_LIST(CSS_NAME_ENTRY)
};
static const char *const borderstyle_names[] = {
    CSS_BORDERSTYLE_LIST(CSS_NAME_ENTRY)
};
static const char *const liststyle_names[] = {
    CSS_LISTSTYLE_LIST(CSS_NAME_ENTRY)
};
static const char *const listpos_names[] = {
    CSS_LISTPOS_LIST(CSS_NAME_ENTRY)
};
static const char *const bordercollapse_names[] = {
    CSS_BORDERCOLLAPSE_LIST(CSS_NAME_ENTRY)
};
static const char *const texttransform_names[] = {
    CSS_TEXTTRANSFORM_LIST(CSS_NAME_ENTRY)
};
static const char *const boxsizing_names[] = {
    CSS_BOXSIZING_LIST(CSS_NAME_ENTRY)
};
static const char *const flexdir_names[] = {
    CSS_FLEXDIR_LIST(CSS_NAME_ENTRY)
};
static const char *const flexwrap_names[] = {
    CSS_FLEXWRAP_LIST(CSS_NAME_ENTRY)
};
static const char *const justify_names[] = {
    CSS_JUSTIFY_LIST(CSS_NAME_ENTRY)
};
static const char *const alignitems_names[] = {
    CSS_ALIGN_LIST(CSS_NAME_ENTRY)
};
static const char *const colorscheme_names[] = {
    CSS_COLORSCHEME_LIST(CSS_NAME_ENTRY)
};
static const char *const fontsizekw_names[] = {
    CSS_FONTSIZEKW_LIST(CSS_NAME_ENTRY)
};
static const char *const fontweightkw_names[] = {
    CSS_FONTWEIGHTKW_LIST(CSS_NAME_ENTRY)
};
#undef CSS_NAME_ENTRY

static uint64_t style_tags_hash(const struct dom_document *doc)
{
    uint64_t h = 14695981039346656037ULL;
    const struct dom_node *n, *ch;
    const char *ty;
    unsigned long k;

    if (!doc || !doc->root)
        return h;
    for (n = doc->root; n; n = dom_next(n)) {
        if (n->type != DOM_ELEMENT || n->tag_id != HTAG_STYLE)
            continue;
        ty = dom_get_attr(n, "type");
        if (ty && *ty && !ci_eq_n(ty, strlen(ty), "text/css", 8))
            continue;
        h = (h ^ 0xFFu) * 1099511628211ULL;
        for (ch = n->first_child; ch; ch = ch->next_sibling) {
            if (ch->type == DOM_TEXT && ch->text && ch->text_len) {
                for (k = 0; k < ch->text_len; k++)
                    h = (h ^ (unsigned char)ch->text[k]) * 1099511628211ULL;
            }
        }
    }
    return h;
}

static struct css_stylesheet *ensure_author_stylesheet(struct jsdom *j)
{
    struct dom_node *n;
    unsigned long cap = 0, used = 0;
    char *buf;
    struct css_media media;

    if (!j || !j->doc || !j->doc->root)
        return 0;
    if (j->external_sheet &&
        style_tags_hash(j->doc) == j->initial_style_hash)
        return 0;
    if (!j->style_sheet_dirty)
        return j->author_sheet;
    css_free(j->author_sheet);
    j->author_sheet = 0;
    j->style_sheet_dirty = 0;
    for (n = j->doc->root; n; n = dom_next(n)) {
        const char *ty;
        struct dom_node *ch;

        if (n->type != DOM_ELEMENT || n->tag_id != HTAG_STYLE)
            continue;
        ty = dom_get_attr(n, "type");
        if (ty && *ty && !ci_eq_n(ty, strlen(ty), "text/css", 8))
            continue;
        for (ch = n->first_child; ch; ch = ch->next_sibling) {
            if (ch->type == DOM_TEXT && ch->text && ch->text_len)
                cap += ch->text_len + 1;
        }
        cap += 1;
    }
    if (!cap || cap > CSS_MAX_SOURCE)
        return 0;
    buf = (char *)malloc(cap + 1);
    if (!buf)
        return 0;
    for (n = j->doc->root; n; n = dom_next(n)) {
        const char *ty;
        struct dom_node *ch;

        if (n->type != DOM_ELEMENT || n->tag_id != HTAG_STYLE)
            continue;
        ty = dom_get_attr(n, "type");
        if (ty && *ty && !ci_eq_n(ty, strlen(ty), "text/css", 8))
            continue;
        for (ch = n->first_child; ch; ch = ch->next_sibling) {
            if (ch->type == DOM_TEXT && ch->text && ch->text_len &&
                used + ch->text_len + 1 <= cap) {
                memcpy(buf + used, ch->text, ch->text_len);
                used += ch->text_len;
            }
        }
        if (used < cap)
            buf[used++] = '\n';
    }
    buf[used] = 0;
    memset(&media, 0, sizeof(media));
    media.width = (int32_t)(j->viewport_width ? j->viewport_width : 900);
    media.height = (int32_t)(j->viewport_height ? j->viewport_height : 620);
    media.dpi = 96;
    media.screen = 1;
    media.scripting = 1;
    if (used)
        j->author_sheet = css_parse(buf, used, CSS_ORIGIN_AUTHOR, &media);
    free(buf);
    return j->author_sheet;
}

static void computed_fmt_thousandths(char *out, unsigned long outsz,
                                     int32_t v, const char *unit)
{
    int neg = 0;
    uint32_t av, whole, frac;

    if (!outsz)
        return;
    if (v < 0) {
        neg = 1;
        av = (uint32_t)(-(int64_t)v);
    } else {
        av = (uint32_t)v;
    }
    whole = av / 1000u;
    frac = av % 1000u;
    if (!frac) {
        snprintf(out, outsz, "%s%u%s", neg ? "-" : "",
                 (unsigned)whole, unit);
    } else {
        char fbuf[8];
        int flen = 3;

        snprintf(fbuf, sizeof(fbuf), "%03u", (unsigned)frac);
        while (flen > 1 && fbuf[flen - 1] == '0')
            fbuf[--flen] = 0;
        snprintf(out, outsz, "%s%u.%s%s", neg ? "-" : "",
                 (unsigned)whole, fbuf, unit);
    }
}

static void computed_len(char *out, unsigned long outsz, struct css_len v)
{
    if (v.type == CSS_LEN_PX)
        snprintf(out, outsz, "%dpx", v.v);
    else if (v.type == CSS_LEN_PCT)
        computed_fmt_thousandths(out, outsz, v.v, "%");
    else if (v.type == CSS_LEN_NUMBER)
        computed_fmt_thousandths(out, outsz, v.v, "");
    else if (v.type == CSS_LEN_NONE)
        snprintf(out, outsz, "none");
    else if (v.type == CSS_LEN_NORMAL)
        snprintf(out, outsz, "normal");
    else
        snprintf(out, outsz, "auto");
}

static void computed_fmt_color(char *out, unsigned long outsz, uint32_t col)
{
    uint32_t a = CSS_COLOR_A(col);
    uint32_t r = CSS_COLOR_R(col);
    uint32_t g = CSS_COLOR_G(col);
    uint32_t b = CSS_COLOR_B(col);

    if (a == 0) {
        snprintf(out, outsz, "rgba(0, 0, 0, 0)");
    } else if (a == 255) {
        snprintf(out, outsz, "rgb(%u, %u, %u)",
                 (unsigned)r, (unsigned)g, (unsigned)b);
    } else {
        char abuf[16];
        uint32_t ath = (a * 1000u + 127u) / 255u;

        computed_fmt_thousandths(abuf, sizeof(abuf), (int32_t)ath, "");
        snprintf(out, outsz, "rgba(%u, %u, %u, %s)",
                 (unsigned)r, (unsigned)g, (unsigned)b, abuf);
    }
}

static const char *computed_kw_str(uint16_t prop_id, int16_t kw)
{
    const char *const *tab = 0;
    unsigned n = 0;
    unsigned idx = (unsigned)kw;

    switch (prop_id) {
    case CSS_PROP_DISPLAY:        tab = display_names; n = sizeof(display_names) / sizeof(display_names[0]); break;
    case CSS_PROP_POSITION:       tab = position_names; n = sizeof(position_names) / sizeof(position_names[0]); break;
    case CSS_PROP_FLOAT:          tab = float_names; n = sizeof(float_names) / sizeof(float_names[0]); break;
    case CSS_PROP_CLEAR:          tab = clear_names; n = sizeof(clear_names) / sizeof(clear_names[0]); break;
    case CSS_PROP_TEXT_ALIGN:     tab = align_names; n = sizeof(align_names) / sizeof(align_names[0]); break;
    case CSS_PROP_VERTICAL_ALIGN: tab = valign_names; n = sizeof(valign_names) / sizeof(valign_names[0]); break;
    case CSS_PROP_WHITE_SPACE:    tab = whitespace_names; n = sizeof(whitespace_names) / sizeof(whitespace_names[0]); break;
    case CSS_PROP_OVERFLOW:       tab = overflow_names; n = sizeof(overflow_names) / sizeof(overflow_names[0]); break;
    case CSS_PROP_VISIBILITY:     tab = visibility_names; n = sizeof(visibility_names) / sizeof(visibility_names[0]); break;
    case CSS_PROP_FONT_STYLE:     tab = fontstyle_names; n = sizeof(fontstyle_names) / sizeof(fontstyle_names[0]); break;
    case CSS_PROP_FONT_FAMILY:    tab = fontfamily_names; n = sizeof(fontfamily_names) / sizeof(fontfamily_names[0]); break;
    case CSS_PROP_BORDER_TOP_STYLE:
    case CSS_PROP_BORDER_RIGHT_STYLE:
    case CSS_PROP_BORDER_BOTTOM_STYLE:
    case CSS_PROP_BORDER_LEFT_STYLE:
        tab = borderstyle_names; n = sizeof(borderstyle_names) / sizeof(borderstyle_names[0]); break;
    case CSS_PROP_LIST_STYLE_TYPE: tab = liststyle_names; n = sizeof(liststyle_names) / sizeof(liststyle_names[0]); break;
    case CSS_PROP_LIST_STYLE_POSITION: tab = listpos_names; n = sizeof(listpos_names) / sizeof(listpos_names[0]); break;
    case CSS_PROP_BORDER_COLLAPSE: tab = bordercollapse_names; n = sizeof(bordercollapse_names) / sizeof(bordercollapse_names[0]); break;
    case CSS_PROP_TEXT_TRANSFORM: tab = texttransform_names; n = sizeof(texttransform_names) / sizeof(texttransform_names[0]); break;
    case CSS_PROP_BOX_SIZING:     tab = boxsizing_names; n = sizeof(boxsizing_names) / sizeof(boxsizing_names[0]); break;
    case CSS_PROP_FLEX_DIRECTION: tab = flexdir_names; n = sizeof(flexdir_names) / sizeof(flexdir_names[0]); break;
    case CSS_PROP_FLEX_WRAP:      tab = flexwrap_names; n = sizeof(flexwrap_names) / sizeof(flexwrap_names[0]); break;
    case CSS_PROP_JUSTIFY_CONTENT: tab = justify_names; n = sizeof(justify_names) / sizeof(justify_names[0]); break;
    case CSS_PROP_ALIGN_ITEMS:    tab = alignitems_names; n = sizeof(alignitems_names) / sizeof(alignitems_names[0]); break;
    case CSS_PROP_COLOR_SCHEME:   tab = colorscheme_names; n = sizeof(colorscheme_names) / sizeof(colorscheme_names[0]); break;
    case CSS_PROP_FONT_SIZE:      tab = fontsizekw_names; n = sizeof(fontsizekw_names) / sizeof(fontsizekw_names[0]); break;
    case CSS_PROP_FONT_WEIGHT:    tab = fontweightkw_names; n = sizeof(fontweightkw_names) / sizeof(fontweightkw_names[0]); break;
    default: break;
    }
    if (tab && idx < n && tab[idx][0] != '\1')
        return tab[idx];
    return "";
}

static const char *computed_ua_display(const struct dom_node *node)
{
    static const char *const none_tags[] = {
        "head", "style", "script", "link", "meta", "title", "base",
        "template", "datalist", "noscript", "param", "source", "track", 0
    };
    static const char *const inline_tags[] = {
        "span", "a", "em", "strong", "b", "i", "u", "s", "small", "big",
        "code", "kbd", "samp", "var", "cite", "q", "abbr", "dfn", "sub",
        "sup", "time", "mark", "bdi", "bdo", "br", "wbr", "label", "img",
        "input", "button", "select", "textarea", "canvas", "svg", "audio",
        "video", "iframe", 0
    };
    const char *tag;
    int i;

    if (!node || !node->tag)
        return "block";
    tag = node->tag;
    if (node->namespace_id == DOM_NS_SVG && strcmp(tag, "svg") != 0)
        return "inline";
    for (i = 0; none_tags[i]; i++)
        if (!strcmp(tag, none_tags[i]))
            return "none";
    if (!strcmp(tag, "li")) return "list-item";
    if (!strcmp(tag, "table")) return "table";
    if (!strcmp(tag, "tr")) return "table-row";
    if (!strcmp(tag, "td") || !strcmp(tag, "th")) return "table-cell";
    if (!strcmp(tag, "tbody")) return "table-row-group";
    if (!strcmp(tag, "thead")) return "table-header-group";
    if (!strcmp(tag, "tfoot")) return "table-footer-group";
    for (i = 0; inline_tags[i]; i++)
        if (!strcmp(tag, inline_tags[i]))
            return "inline";
    return "block";
}

struct computed_cascade_match {
    uint16_t prop_id;
    const char *custom_name;
    int has_winner;
    int best_important;
    uint8_t best_origin;
    uint32_t best_spec;
    uint32_t best_order;
    struct css_value val;
    const char *custom_val;
};

static int computed_beats(const struct computed_cascade_match *m,
                          int imp, uint8_t origin,
                          uint32_t spec, uint32_t order)
{
    if (!m->has_winner)
        return 1;
    if (imp != m->best_important)
        return imp > m->best_important;
    if (origin != m->best_origin)
        return origin > m->best_origin;
    if (spec != m->best_spec)
        return spec > m->best_spec;
    return order >= m->best_order;
}

static void computed_apply_rule(struct computed_cascade_match *m,
                                const struct css_rule *r, uint8_t origin)
{
    int i;

    if (!m || !r)
        return;
    if (m->custom_name) {
        for (i = 0; i < r->ncustom; i++) {
            const struct css_custom_decl *cd = &r->customs[i];
            if (cd->name && !strcmp(cd->name, m->custom_name) &&
                computed_beats(m, cd->important, origin,
                               r->sel.specificity, r->order)) {
                m->has_winner = 1;
                m->best_important = cd->important;
                m->best_origin = origin;
                m->best_spec = r->sel.specificity;
                m->best_order = r->order;
                m->custom_val = cd->value ? cd->value : "";
            }
        }
        return;
    }
    for (i = 0; i < r->ndecl; i++) {
        const struct css_decl *d = &r->decls[i];
        if (d->prop == m->prop_id &&
            computed_beats(m, d->important, origin,
                           r->sel.specificity, r->order)) {
            m->has_winner = 1;
            m->best_important = d->important;
            m->best_origin = origin;
            m->best_spec = r->sel.specificity;
            m->best_order = r->order;
            m->val = d->val;
        }
    }
}

static void computed_rule_cb(void *ctx, const struct css_rule *r)
{
    computed_apply_rule((struct computed_cascade_match *)ctx, r,
                        CSS_ORIGIN_AUTHOR);
}

static void computed_resolve_prop(struct jsdom *j, struct dom_node *node,
                                  const char *prop, char *out,
                                  unsigned long outsz, int depth);

static void computed_expand_var(struct jsdom *j, struct dom_node *node,
                                const char *expr, char *out,
                                unsigned long outsz, int depth)
{
    const char *p, *comma = 0;
    char vname[128], fb[256];
    unsigned long nlen;
    int paren = 0;

    if (!expr || depth > 8) {
        snprintf(out, outsz, "%s", expr ? expr : "");
        return;
    }
    while (*expr == ' ' || *expr == '\t') expr++;
    if (strncmp(expr, "var(", 4) != 0) {
        snprintf(out, outsz, "%s", expr);
        return;
    }
    p = expr + 4;
    while (*p == ' ' || *p == '\t') p++;
    for (nlen = 0; p[nlen] && p[nlen] != ',' && p[nlen] != ')' &&
                   p[nlen] != ' ' && p[nlen] != '\t' &&
                   nlen < sizeof(vname) - 1; nlen++)
        vname[nlen] = p[nlen];
    vname[nlen] = 0;
    if (strncmp(vname, "--", 2) == 0) {
        out[0] = 0;
        computed_resolve_prop(j, node, vname, out, outsz, depth + 1);
        if (out[0])
            return;
    }
    while (p[nlen] && (p[nlen] == ' ' || p[nlen] == '\t'))
        nlen++;
    if (p[nlen] == ',') {
        const char *fstart = p + nlen + 1;
        const char *fend;

        while (*fstart == ' ' || *fstart == '\t') fstart++;
        fend = fstart;
        while (*fend) {
            if (*fend == '(') paren++;
            else if (*fend == ')') {
                if (paren == 0) break;
                paren--;
            }
            fend++;
        }
        while (fend > fstart && (fend[-1] == ' ' || fend[-1] == '\t'))
            fend--;
        nlen = (unsigned long)(fend - fstart);
        if (nlen >= sizeof(fb)) nlen = sizeof(fb) - 1;
        memcpy(fb, fstart, nlen);
        fb[nlen] = 0;
        (void)comma;
        if (strncmp(fb, "var(", 4) == 0)
            computed_expand_var(j, node, fb, out, outsz, depth + 1);
        else
            snprintf(out, outsz, "%s", fb);
        return;
    }
    out[0] = 0;
}

static void computed_fmt_css_value(struct jsdom *j, struct dom_node *node,
                                   uint16_t prop_id, const char *prop,
                                   const struct css_value *v,
                                   char *out, unsigned long outsz, int depth)
{
    static const char *const units[] = {
        "", "px", "em", "rem", "ex", "ch", "pt", "pc", "cm", "mm", "in",
        "vw", "vh"
    };
    const char *u;

    if (!v || !outsz)
        return;
    switch (v->type) {
    case CSS_VAL_KEYWORD:
        if (prop_id == CSS_PROP_FONT_WEIGHT) {
            if (v->kw == CSS_FONTWEIGHTKW_BOLD)
                snprintf(out, outsz, "700");
            else if (v->kw == CSS_FONTWEIGHTKW_NORMAL)
                snprintf(out, outsz, "400");
            else
                snprintf(out, outsz, "%s",
                         computed_kw_str(prop_id, v->kw));
        } else {
            const char *kw = computed_kw_str(prop_id, v->kw);
            if ((prop_id == CSS_PROP_COLOR ||
                 prop_id == CSS_PROP_BACKGROUND_COLOR) &&
                (!kw || !*kw))
                snprintf(out, outsz, "%s",
                         prop_id == CSS_PROP_COLOR ? "rgb(0, 0, 0)" :
                         "rgba(0, 0, 0, 0)");
            else
                snprintf(out, outsz, "%s", kw);
        }
        break;
    case CSS_VAL_COLOR:
        computed_fmt_color(out, outsz, v->color);
        break;
    case CSS_VAL_LENGTH:
        if (v->unit == CSS_UNIT_REM) {
            computed_fmt_thousandths(out, outsz, v->num * 16, "px");
        } else {
            u = v->unit < sizeof(units) / sizeof(units[0]) ? units[v->unit] : "px";
            computed_fmt_thousandths(out, outsz, v->num, u);
        }
        break;
    case CSS_VAL_NUMBER:
        computed_fmt_thousandths(out, outsz, v->num, "");
        break;
    case CSS_VAL_PERCENT:
        computed_fmt_thousandths(out, outsz, v->num, "%");
        break;
    case CSS_VAL_AUTO:
        snprintf(out, outsz, "auto");
        break;
    case CSS_VAL_BITS:
        if (!v->kw) {
            snprintf(out, outsz, "none");
        } else {
            out[0] = 0;
            if (v->kw & CSS_DECOR_UNDERLINE) strcat(out, "underline");
            if (v->kw & CSS_DECOR_OVERLINE) {
                if (out[0]) strcat(out, " ");
                strcat(out, "overline");
            }
            if (v->kw & CSS_DECOR_LINETHRU) {
                if (out[0]) strcat(out, " ");
                strcat(out, "line-through");
            }
            if (v->kw & CSS_DECOR_BLINK) {
                if (out[0]) strcat(out, " ");
                strcat(out, "blink");
            }
        }
        break;
    case CSS_VAL_RAW_EXPR:
        if (v->str && strncmp(v->str, "var(", 4) == 0) {
            char resolved[256];
            uint32_t col;

            computed_expand_var(j, node, v->str, resolved, sizeof(resolved),
                                depth);
            if ((prop_id == CSS_PROP_COLOR ||
                 prop_id == CSS_PROP_BACKGROUND_COLOR ||
                 prop_id == CSS_PROP_BORDER_TOP_COLOR ||
                 prop_id == CSS_PROP_BORDER_RIGHT_COLOR ||
                 prop_id == CSS_PROP_BORDER_BOTTOM_COLOR ||
                 prop_id == CSS_PROP_BORDER_LEFT_COLOR) &&
                resolved[0] && css_parse_color_string(resolved, &col)) {
                computed_fmt_color(out, outsz, col);
            } else {
                snprintf(out, outsz, "%s", resolved);
            }
        } else {
            snprintf(out, outsz, "%s", v->str ? v->str : "");
        }
        break;
    case CSS_VAL_URL:
        if (v->str && strncmp(v->str, "url(", 4) == 0)
            snprintf(out, outsz, "%s", v->str);
        else
            snprintf(out, outsz, "url(\"%s\")", v->str ? v->str : "");
        break;
    case CSS_VAL_STRING:
    case CSS_VAL_IDENT:
        snprintf(out, outsz, "%s", v->str ? v->str : "");
        break;
    default:
        (void)prop;
        out[0] = 0;
        break;
    }
}

static int computed_from_node_style(struct dom_node *node, uint16_t prop_id,
                                    char *out, unsigned long outsz)
{
    const struct computed_style *s =
        node ? (const struct computed_style *)node->style : 0;

    if (!s || !outsz)
        return 0;
    switch (prop_id) {
    case CSS_PROP_DISPLAY:
        if (s->display < sizeof(display_names) / sizeof(display_names[0])) {
            snprintf(out, outsz, "%s", display_names[s->display]);
            return 1;
        }
        break;
    case CSS_PROP_POSITION:
        if (s->position < sizeof(position_names) / sizeof(position_names[0])) {
            snprintf(out, outsz, "%s", position_names[s->position]);
            return 1;
        }
        break;
    case CSS_PROP_FLOAT:
        if (s->css_float < sizeof(float_names) / sizeof(float_names[0])) {
            snprintf(out, outsz, "%s", float_names[s->css_float]);
            return 1;
        }
        break;
    case CSS_PROP_CLEAR:
        if (s->clear < sizeof(clear_names) / sizeof(clear_names[0])) {
            snprintf(out, outsz, "%s", clear_names[s->clear]);
            return 1;
        }
        break;
    case CSS_PROP_TEXT_ALIGN:
        if (s->text_align < sizeof(align_names) / sizeof(align_names[0])) {
            snprintf(out, outsz, "%s", align_names[s->text_align]);
            return 1;
        }
        break;
    case CSS_PROP_VISIBILITY:
        if (s->visibility < sizeof(visibility_names) / sizeof(visibility_names[0])) {
            snprintf(out, outsz, "%s", visibility_names[s->visibility]);
            return 1;
        }
        break;
    case CSS_PROP_FONT_STYLE:
        if (s->font_style < sizeof(fontstyle_names) / sizeof(fontstyle_names[0])) {
            snprintf(out, outsz, "%s", fontstyle_names[s->font_style]);
            return 1;
        }
        break;
    case CSS_PROP_FONT_FAMILY:
        if (s->font_family_name && s->font_family_name[0]) {
            snprintf(out, outsz, "%s", s->font_family_name);
            return 1;
        }
        if (s->font_family < sizeof(fontfamily_names) / sizeof(fontfamily_names[0])) {
            snprintf(out, outsz, "%s", fontfamily_names[s->font_family]);
            return 1;
        }
        break;
    case CSS_PROP_COLOR:
        computed_fmt_color(out, outsz, s->color);
        return 1;
    case CSS_PROP_BACKGROUND_COLOR:
        computed_fmt_color(out, outsz, s->background_color);
        return 1;
    case CSS_PROP_OPACITY:
        computed_fmt_thousandths(out, outsz, (int32_t)s->opacity, "");
        return 1;
    case CSS_PROP_FONT_SIZE:
        snprintf(out, outsz, "%dpx", s->font_size);
        return 1;
    case CSS_PROP_FONT_WEIGHT:
        snprintf(out, outsz, "%u", (unsigned)s->font_weight);
        return 1;
    case CSS_PROP_WIDTH:
        if (s->width.type != CSS_LEN_AUTO) {
            computed_len(out, outsz, s->width);
            return 1;
        }
        break;
    case CSS_PROP_HEIGHT:
        if (s->height.type != CSS_LEN_AUTO) {
            computed_len(out, outsz, s->height);
            return 1;
        }
        break;
    case CSS_PROP_LINE_HEIGHT:
        computed_len(out, outsz, s->line_height);
        return 1;
    case CSS_PROP_Z_INDEX:
        if (s->z_auto)
            snprintf(out, outsz, "auto");
        else
            snprintf(out, outsz, "%d", s->z_index);
        return 1;
    case CSS_PROP_BORDER_TL_RADIUS:
    case CSS_PROP_BORDER_TR_RADIUS:
    case CSS_PROP_BORDER_BR_RADIUS:
    case CSS_PROP_BORDER_BL_RADIUS:
        snprintf(out, outsz, "%dpx",
                 s->border_radius[prop_id - CSS_PROP_BORDER_TL_RADIUS]);
        return 1;
    case CSS_PROP_MARGIN_TOP:
    case CSS_PROP_MARGIN_RIGHT:
    case CSS_PROP_MARGIN_BOTTOM:
    case CSS_PROP_MARGIN_LEFT:
        computed_len(out, outsz, s->margin[prop_id - CSS_PROP_MARGIN_TOP]);
        return 1;
    case CSS_PROP_PADDING_TOP:
    case CSS_PROP_PADDING_RIGHT:
    case CSS_PROP_PADDING_BOTTOM:
    case CSS_PROP_PADDING_LEFT:
        computed_len(out, outsz, s->padding[prop_id - CSS_PROP_PADDING_TOP]);
        return 1;
    default:
        break;
    }
    return 0;
}

static void computed_initial_value(struct jsdom *j, struct dom_node *node,
                                   uint16_t prop_id, const char *prop,
                                   char *out, unsigned long outsz)
{
    if (!outsz)
        return;
    out[0] = 0;
    if (prop && prop[0] == '-' && prop[1] == '-')
        return;
    switch (prop_id) {
    case CSS_PROP_DISPLAY:
        snprintf(out, outsz, "%s", computed_ua_display(node));
        return;
    case CSS_PROP_POSITION:
        snprintf(out, outsz, "static");
        return;
    case CSS_PROP_FLOAT:
    case CSS_PROP_CLEAR:
    case CSS_PROP_TEXT_DECORATION:
    case CSS_PROP_TEXT_TRANSFORM:
    case CSS_PROP_LIST_STYLE_TYPE:
    case CSS_PROP_BORDER_TOP_STYLE:
    case CSS_PROP_BORDER_RIGHT_STYLE:
    case CSS_PROP_BORDER_BOTTOM_STYLE:
    case CSS_PROP_BORDER_LEFT_STYLE:
    case CSS_PROP_MAX_WIDTH:
    case CSS_PROP_MAX_HEIGHT:
    case CSS_PROP_BOX_SHADOW:
    case CSS_PROP_BACKGROUND_IMAGE:
        snprintf(out, outsz, "none");
        return;
    case CSS_PROP_VISIBILITY:
        snprintf(out, outsz, "visible");
        return;
    case CSS_PROP_OVERFLOW:
        snprintf(out, outsz, "visible");
        return;
    case CSS_PROP_COLOR:
        snprintf(out, outsz, "rgb(0, 0, 0)");
        return;
    case CSS_PROP_BACKGROUND_COLOR:
        snprintf(out, outsz, "rgba(0, 0, 0, 0)");
        return;
    case CSS_PROP_OPACITY:
    case CSS_PROP_FLEX_SHRINK:
        snprintf(out, outsz, "1");
        return;
    case CSS_PROP_FLEX_GROW:
        snprintf(out, outsz, "0");
        return;
    case CSS_PROP_FONT_SIZE:
        snprintf(out, outsz, "16px");
        return;
    case CSS_PROP_FONT_WEIGHT:
        if (node && node->tag &&
            (!strcmp(node->tag, "b") || !strcmp(node->tag, "strong") ||
             !strcmp(node->tag, "h1") || !strcmp(node->tag, "h2") ||
             !strcmp(node->tag, "h3") || !strcmp(node->tag, "h4") ||
             !strcmp(node->tag, "h5") || !strcmp(node->tag, "h6") ||
             !strcmp(node->tag, "th")))
            snprintf(out, outsz, "700");
        else
            snprintf(out, outsz, "400");
        return;
    case CSS_PROP_FONT_STYLE:
        if (node && node->tag &&
            (!strcmp(node->tag, "em") || !strcmp(node->tag, "i") ||
             !strcmp(node->tag, "cite")))
            snprintf(out, outsz, "italic");
        else
            snprintf(out, outsz, "normal");
        return;
    case CSS_PROP_FONT_FAMILY:
        snprintf(out, outsz, "sans-serif");
        return;
    case CSS_PROP_TEXT_ALIGN:
        snprintf(out, outsz, "start");
        return;
    case CSS_PROP_LINE_HEIGHT:
    case CSS_PROP_WHITE_SPACE:
    case CSS_PROP_JUSTIFY_CONTENT:
    case CSS_PROP_ALIGN_ITEMS:
    case CSS_PROP_GAP:
    case CSS_PROP_ROW_GAP:
    case CSS_PROP_COLUMN_GAP:
    case CSS_PROP_COLOR_SCHEME:
        snprintf(out, outsz, "normal");
        return;
    case CSS_PROP_BOX_SIZING:
        snprintf(out, outsz, "content-box");
        return;
    case CSS_PROP_FLEX_DIRECTION:
        snprintf(out, outsz, "row");
        return;
    case CSS_PROP_FLEX_WRAP:
        snprintf(out, outsz, "nowrap");
        return;
    case CSS_PROP_BORDER_COLLAPSE:
        snprintf(out, outsz, "separate");
        return;
    case CSS_PROP_LIST_STYLE_POSITION:
        snprintf(out, outsz, "outside");
        return;
    case CSS_PROP_VERTICAL_ALIGN:
        snprintf(out, outsz, "baseline");
        return;
    case CSS_PROP_MARGIN_TOP:
    case CSS_PROP_MARGIN_RIGHT:
    case CSS_PROP_MARGIN_BOTTOM:
    case CSS_PROP_MARGIN_LEFT:
    case CSS_PROP_PADDING_TOP:
    case CSS_PROP_PADDING_RIGHT:
    case CSS_PROP_PADDING_BOTTOM:
    case CSS_PROP_PADDING_LEFT:
    case CSS_PROP_BORDER_TOP_WIDTH:
    case CSS_PROP_BORDER_RIGHT_WIDTH:
    case CSS_PROP_BORDER_BOTTOM_WIDTH:
    case CSS_PROP_BORDER_LEFT_WIDTH:
    case CSS_PROP_BORDER_TL_RADIUS:
    case CSS_PROP_BORDER_TR_RADIUS:
    case CSS_PROP_BORDER_BR_RADIUS:
    case CSS_PROP_BORDER_BL_RADIUS:
    case CSS_PROP_MIN_WIDTH:
    case CSS_PROP_MIN_HEIGHT:
    case CSS_PROP_TEXT_INDENT:
    case CSS_PROP_BORDER_SPACING:
        snprintf(out, outsz, "0px");
        return;
    case CSS_PROP_WIDTH:
    case CSS_PROP_HEIGHT:
        if (j && j->get_box_rect && node) {
            float bx = 0, by = 0, bw = 0, bh = 0;
            if (j->get_box_rect(j->print_user, node, &bx, &by, &bw, &bh)) {
                snprintf(out, outsz, "%dpx",
                         (int)(prop_id == CSS_PROP_WIDTH ? bw : bh));
                return;
            }
        }
        snprintf(out, outsz, "auto");
        return;
    case CSS_PROP_TOP:
    case CSS_PROP_RIGHT:
    case CSS_PROP_BOTTOM:
    case CSS_PROP_LEFT:
    case CSS_PROP_Z_INDEX:
    case CSS_PROP_FLEX_BASIS:
    case CSS_PROP_GRID_COL_START:
    case CSS_PROP_GRID_COL_END:
    case CSS_PROP_GRID_ROW_START:
    case CSS_PROP_GRID_ROW_END:
        snprintf(out, outsz, "auto");
        return;
    default:
        break;
    }
    if (prop) {
        if (!strcmp(prop, "border-radius") || !strcmp(prop, "margin") ||
            !strcmp(prop, "padding"))
            snprintf(out, outsz, "0px");
        else if (!strcmp(prop, "pointer-events"))
            snprintf(out, outsz, "auto");
        else if (!strcmp(prop, "overflow-x") || !strcmp(prop, "overflow-y"))
            snprintf(out, outsz, "visible");
        else if (!strcmp(prop, "transform"))
            snprintf(out, outsz, "none");
        else if (!strcmp(prop, "cursor"))
            snprintf(out, outsz, "auto");
    }
}

static void computed_resolve_prop(struct jsdom *j, struct dom_node *node,
                                  const char *prop, char *out,
                                  unsigned long outsz, int depth)
{
    struct css_stylesheet *author, *inline_ss;
    struct computed_cascade_match m;
    const char *inline_css;
    struct dom_node *parent;
    int is_custom;
    uint16_t prop_id;

    if (!outsz)
        return;
    out[0] = 0;
    if (!j || !node || node->type != DOM_ELEMENT || !prop || !*prop ||
        depth > 64)
        return;
    is_custom = (prop[0] == '-' && prop[1] == '-');
    if (!is_custom) {
        if (!strcmp(prop, "margin") || !strcmp(prop, "padding") ||
            !strcmp(prop, "border-radius")) {
            char a[48], b[48], cv[48], d[48];
            const char *p0 = !strcmp(prop, "margin") ? "margin-top" :
                             !strcmp(prop, "padding") ? "padding-top" :
                             "border-top-left-radius";
            const char *p1 = !strcmp(prop, "margin") ? "margin-right" :
                             !strcmp(prop, "padding") ? "padding-right" :
                             "border-top-right-radius";
            const char *p2 = !strcmp(prop, "margin") ? "margin-bottom" :
                             !strcmp(prop, "padding") ? "padding-bottom" :
                             "border-bottom-right-radius";
            const char *p3 = !strcmp(prop, "margin") ? "margin-left" :
                             !strcmp(prop, "padding") ? "padding-left" :
                             "border-bottom-left-radius";

            computed_resolve_prop(j, node, p0, a, sizeof(a), depth + 1);
            computed_resolve_prop(j, node, p1, b, sizeof(b), depth + 1);
            computed_resolve_prop(j, node, p2, cv, sizeof(cv), depth + 1);
            computed_resolve_prop(j, node, p3, d, sizeof(d), depth + 1);
            if (!strcmp(a, b) && !strcmp(a, cv) && !strcmp(a, d))
                snprintf(out, outsz, "%s", a);
            else if (!strcmp(a, cv) && !strcmp(b, d))
                snprintf(out, outsz, "%s %s", a, b);
            else
                snprintf(out, outsz, "%s %s %s %s", a, b, cv, d);
            return;
        }
        if (!strcmp(prop, "border-width") || !strcmp(prop, "border-style") ||
            !strcmp(prop, "border-color")) {
            char a[64], b[64], cv[64], d[64];
            const char *suf = prop + 6;
            char p0[32], p1[32], p2[32], p3[32];

            snprintf(p0, sizeof(p0), "border-top%s", suf);
            snprintf(p1, sizeof(p1), "border-right%s", suf);
            snprintf(p2, sizeof(p2), "border-bottom%s", suf);
            snprintf(p3, sizeof(p3), "border-left%s", suf);
            computed_resolve_prop(j, node, p0, a, sizeof(a), depth + 1);
            computed_resolve_prop(j, node, p1, b, sizeof(b), depth + 1);
            computed_resolve_prop(j, node, p2, cv, sizeof(cv), depth + 1);
            computed_resolve_prop(j, node, p3, d, sizeof(d), depth + 1);
            if (!strcmp(a, b) && !strcmp(a, cv) && !strcmp(a, d))
                snprintf(out, outsz, "%s", a);
            else
                snprintf(out, outsz, "%s %s %s %s", a, b, cv, d);
            return;
        }
    }
    prop_id = is_custom ? CSS_PROP_NONE : (uint16_t)css_property_id(prop);
    memset(&m, 0, sizeof(m));
    m.prop_id = prop_id;
    m.custom_name = is_custom ? prop : 0;

    if (j->external_sheet && (is_custom || prop_id != CSS_PROP_NONE))
        css_match(j->external_sheet, node, &jsdom_css_ops, computed_rule_cb, &m);
    author = ensure_author_stylesheet(j);
    if (author && (is_custom || prop_id != CSS_PROP_NONE))
        css_match(author, node, &jsdom_css_ops, computed_rule_cb, &m);
    inline_css = dom_get_attr(node, "style");
    if (inline_css && *inline_css && (is_custom || prop_id != CSS_PROP_NONE)) {
        inline_ss = css_parse_style_attr(inline_css);
        if (inline_ss) {
            int i, rc = css_rule_count(inline_ss);
            for (i = 0; i < rc; i++)
                computed_apply_rule(&m, css_rule_at(inline_ss, i),
                                    CSS_ORIGIN_INLINE);
            css_free(inline_ss);
        }
    }
    if (!m.has_winner && !is_custom && prop_id == CSS_PROP_NONE &&
        inline_css && *inline_css) {
        char *raw = style_property_copy(inline_css, prop, 0);
        if (raw) {
            snprintf(out, outsz, "%s", raw);
            free(raw);
            return;
        }
    }
    if (m.has_winner) {
        if (is_custom) {
            if (m.custom_val && strncmp(m.custom_val, "var(", 4) == 0)
                computed_expand_var(j, node, m.custom_val, out, outsz, depth);
            else
                snprintf(out, outsz, "%s", m.custom_val ? m.custom_val : "");
            return;
        }
        if (m.val.type == CSS_VAL_INHERIT ||
            (m.val.type == CSS_VAL_UNSET && css_property_inherited(prop_id))) {
            parent = dom_parent_element(node);
            if (parent)
                computed_resolve_prop(j, parent, prop, out, outsz, depth + 1);
            if (!out[0])
                computed_initial_value(j, node, prop_id, prop, out, outsz);
            return;
        }
        if (m.val.type == CSS_VAL_INITIAL || m.val.type == CSS_VAL_UNSET) {
            computed_initial_value(j, node, prop_id, prop, out, outsz);
            return;
        }
        computed_fmt_css_value(j, node, prop_id, prop, &m.val,
                               out, outsz, depth);
        if (out[0])
            return;
    }
    if (computed_from_node_style(node, prop_id, out, outsz))
        return;
    if (is_custom ||
        (prop_id != CSS_PROP_NONE && css_property_inherited(prop_id))) {
        parent = dom_parent_element(node);
        if (parent) {
            computed_resolve_prop(j, parent, prop, out, outsz, depth + 1);
            if (out[0])
                return;
        }
    }
    computed_initial_value(j, node, prop_id, prop, out, outsz);
}

static int computed_style_prop_get(js_ctx *c, js_value t, const char *prop,
                                   js_value *r)
{
    struct hostref *h = host(t, HOST_COMPUTED_STYLE);
    char buf[256];

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal CSSStyleDeclaration receiver");
    dataset_sync_all(h->j);
    computed_resolve_prop(h->j, h->node, prop, buf, sizeof(buf), 0);
    *r = js_mkcstring(c, buf);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int computed_style_get_property_value(js_ctx *c, js_value t,
                                             int ac, js_value *av, js_value *r)
{
    const char *prop;

    if (string_arg(c, ac, av, 0, &prop) != JS_OK)
        return JS_THROW;
    return computed_style_prop_get(c, t, prop, r);
}

static int computed_style_get_property_priority(js_ctx *c, js_value t,
                                                int ac, js_value *av,
                                                js_value *r)
{
    struct hostref *h = host(t, HOST_COMPUTED_STYLE);
    const char *prop;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal CSSStyleDeclaration receiver");
    if (string_arg(c, ac, av, 0, &prop) != JS_OK)
        return JS_THROW;
    *r = js_mkcstring(c, "");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int computed_style_readonly(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    (void)t; (void)ac; (void)av; (void)r;
    return throw_dom_exception(c, "NoModificationAllowedError",
                               "computed style is read-only");
}

static int computed_style_css_get(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_COMPUTED_STYLE);
    (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal CSSStyleDeclaration receiver");
    *r = js_mkcstring(c, "");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int computed_style_length_get(js_ctx *c, js_value t,
                                     int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_COMPUTED_STYLE);
    (void)c; (void)ac; (void)av;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal CSSStyleDeclaration receiver");
    *r = js_number((double)(CSS_PROP_COUNT - 1));
    return JS_OK;
}

static int computed_style_item(js_ctx *c, js_value t,
                               int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_COMPUTED_STYLE);
    double idx;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal CSSStyleDeclaration receiver");
    if (js_to_integer(c, ac ? av[0] : js_undefined(), &idx) != JS_OK)
        return JS_THROW;
    if (idx < 0 || idx >= (double)(CSS_PROP_COUNT - 1)) {
        *r = js_mkcstring(c, "");
        return js_fatal(c) ? JS_THROW : JS_OK;
    }
    *r = js_mkcstring(c, css_property_name((int)idx + 1));
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int global_computed_style(js_ctx *c, js_value t, int ac,
                                 js_value *av, js_value *r)
{
    struct hostref *h = ac ? host(av[0], HOST_NODE) : 0;
    js_object *o;
    (void)t;

    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE,
                              "getComputedStyle requires an Element");
    o = new_host(h->j, h->node, HOST_COMPUTED_STYLE,
                 h->j->computed_style_proto);
    if (!o)
        return JS_THROW;
    *r = js_object_value(o);
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

static int crypto_fill(unsigned char *bytes, unsigned long length)
{
#if defined(JS_HOST) && defined(_WIN32)
    unsigned long offset = 0;

    while (offset < length) {
        unsigned int val = 0;
        unsigned long take = length - offset;
        if (rand_s(&val) != 0)
            return -1;
        if (take > sizeof(val))
            take = sizeof(val);
        memcpy(bytes + offset, &val, take);
        offset += take;
    }
    return 0;
#else
    unsigned long offset = 0;

    while (offset < length) {
        long count = (long)getrandom(bytes + offset,
                                     length - offset, 0);

        if (count <= 0 || (unsigned long)count > length - offset)
            return -1;
        offset += (unsigned long)count;
    }
    return 0;
#endif
}

static int jsdom_random_bytes(void *user, uint8_t *out, unsigned long len)
{
    (void)user;
    return crypto_fill((unsigned char *)out, len);
}

/* ------------------------------------------------------------------ */
/* AbortController / AbortSignal                                      */

static int make_named_error(js_ctx *c, const char *error_name,
                            const char *error_message, js_value *out)
{
    js_value constructor, message, name;

    if (js_get(c, js_global(c), "Error", &constructor) != JS_OK)
        return JS_THROW;
    message = js_mkcstring(c, error_message);
    if (js_fatal(c) ||
        js_construct(c, constructor, 1, &message, out) != JS_OK)
        return JS_THROW;
    name = js_mkcstring(c, error_name);
    if (js_fatal(c) || js_set(c, *out, "name", name) != JS_OK)
        return JS_THROW;
    return JS_OK;
}

static int make_abort_reason(js_ctx *c, js_value *out)
{
    return make_named_error(c, "AbortError", "The operation was aborted",
                            out);
}

static int make_timeout_reason(js_ctx *c, js_value *out)
{
    return make_named_error(c, "TimeoutError",
                            "The operation timed out", out);
}

static void fetch_task_clear(struct jsdom *j, int index)
{
    free(j->fetches[index].url);
    free(j->fetches[index].method);
    free(j->fetches[index].body);
    free(j->fetches[index].content_type);
    free(j->fetches[index].accept);
    free(j->fetches[index].extra_headers);
    memset(&j->fetches[index], 0, sizeof(j->fetches[index]));
}

static int abort_pending_fetches(struct jsdom *j, js_value signal)
{
    js_value reason = js_abortsignal_reason(signal);
    int i;

    for (i = 0; i < JSDOM_FETCH_MAX; i++) {
        int rc;

        if (j->fetches[i].state != TASK_QUEUED ||
            !js_is_object(j->fetches[i].abort_signal) ||
            j->fetches[i].abort_signal.u.obj != signal.u.obj)
            continue;
        j->fetches[i].state = TASK_SETTLING;
        rc = js_promise_reject(j->ctx, j->fetches[i].promise, reason);
        fetch_task_clear(j, i);
        if (rc != JS_OK)
            return JS_THROW;
    }
    return JS_OK;
}

static void jsdom_on_abort(void *user, js_value signal)
{
    struct jsdom *j = (struct jsdom *)user;

    if (j)
        (void)abort_pending_fetches(j, signal);
}

static js_value jsdom_make_abort_event(void *user, const char *type)
{
    struct jsdom *j = (struct jsdom *)user;
    js_value ev;

    if (!j || event_make(j, j->event_proto, type, js_undefined(),
                         js_undefined(), 0, &ev) != JS_OK)
        return js_undefined();
    return ev;
}

static int abort_signal_timeout_static(js_ctx *c, js_value t,
                                       int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    js_value signal;
    double delay;
    unsigned int turns = 1;
    int slot = -1, i;
    (void)t;

    if (!j || ac < 1 || js_to_number(c, av[0], &delay) != JS_OK)
        return ac < 1
            ? js_throw_error(c, JS_ERR_TYPE,
                             "AbortSignal.timeout requires a delay")
            : JS_THROW;
    if (!(delay >= 0.0) || delay > 86400000.0)
        return js_throw_error(c, JS_ERR_RANGE,
                              "AbortSignal timeout is out of range");
    if (delay > 0.0) {
        turns = (unsigned int)((delay + 199.0) / 200.0);
        if (!turns)
            turns = 1;
    }
    for (i = 0; i < (int)(sizeof(j->timers) / sizeof(j->timers[0])); i++)
        if (j->timers[i].state == TASK_FREE) {
            slot = i;
            break;
        }
    if (slot < 0)
        return js_throw_error(c, JS_ERR_RANGE, "too many pending timers");
    signal = js_abortsignal_new(c);
    if (!js_is_abortsignal(signal))
        return JS_THROW;
    j->timers[slot].fn = js_undefined();
    j->timers[slot].abort_signal = signal;
    j->timers[slot].state = TASK_QUEUED;
    j->timers[slot].repeats = 0;
    j->timers[slot].argc = 0;
    j->timers[slot].interval_turns = 0;
    j->timers[slot].callback_kind = 0;
    j->timers[slot].due_turn = j->event_turn + turns;
    *r = signal;
    return JS_OK;
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

static struct headers_state *headers_state_new(void)
{
    struct headers_state *headers =
        (struct headers_state *)calloc(1, sizeof(*headers));

    if (headers)
        headers->refs = 1;
    return headers;
}

static struct headers_state *headers_state_of(js_value value)
{
    struct hostref *h = host(value, HOST_HEADERS);

    return h ? (struct headers_state *)h->data : 0;
}

static int header_name_normalize(js_ctx *c, const char *input,
                                 char output[JSDOM_HEADER_NAME_MAX + 1])
{
    unsigned long i, length = strlen(input);

    if (!length || length > JSDOM_HEADER_NAME_MAX)
        return js_throw_error(c, JS_ERR_TYPE, "invalid header name");
    for (i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)input[i];
        int token = (ch >= 'a' && ch <= 'z') ||
                    (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') ||
                    ch == '!' || ch == '#' || ch == '$' || ch == '%' ||
                    ch == '&' || ch == '\'' || ch == '*' || ch == '+' ||
                    ch == '-' || ch == '.' || ch == '^' || ch == '_' ||
                    ch == '`' || ch == '|' || ch == '~';

        if (!token)
            return js_throw_error(c, JS_ERR_TYPE, "invalid header name");
        output[i] = (char)ci(ch);
    }
    output[length] = 0;
    return JS_OK;
}

static char *header_value_copy(js_ctx *c, const char *input)
{
    const char *start = input;
    unsigned long length;
    const char *p;

    while (*start == ' ' || *start == '\t')
        start++;
    length = strlen(start);
    while (length && (start[length - 1] == ' ' ||
                      start[length - 1] == '\t'))
        length--;
    if (length > JSDOM_HEADER_VALUE_MAX) {
        js_throw_error(c, JS_ERR_RANGE, "header value is too long");
        return 0;
    }
    for (p = start; p < start + length; p++)
        if (*p == '\r' || *p == '\n') {
            js_throw_error(c, JS_ERR_TYPE, "invalid header value");
            return 0;
        }
    p = dup_n(start, length);
    if (!p)
        js_throw_error(c, JS_ERR_ERROR, "out of memory");
    return (char *)p;
}

static int headers_store(js_ctx *c, struct headers_state *headers,
                         const char *name, const char *value, int replace)
{
    char normalized[JSDOM_HEADER_NAME_MAX + 1];
    char *copy;
    unsigned int i;

    if (header_name_normalize(c, name, normalized) != JS_OK)
        return JS_THROW;
    copy = header_value_copy(c, value);
    if (!copy)
        return JS_THROW;
    for (i = 0; i < headers->count; i++) {
        struct header_entry *entry = &headers->entries[i];

        if (strcmp(entry->name, normalized))
            continue;
        if (replace) {
            free(entry->value);
            entry->value = copy;
            return JS_OK;
        } else {
            unsigned long old_length = strlen(entry->value);
            unsigned long add_length = strlen(copy);
            char *joined;

            if (old_length + add_length + 2 > JSDOM_HEADER_VALUE_MAX) {
                free(copy);
                return js_throw_error(c, JS_ERR_RANGE,
                                      "combined header value is too long");
            }
            joined = (char *)realloc(entry->value,
                                     old_length + add_length + 3);
            if (!joined) {
                free(copy);
                return js_throw_error(c, JS_ERR_ERROR, "out of memory");
            }
            joined[old_length] = ',';
            joined[old_length + 1] = ' ';
            memcpy(joined + old_length + 2, copy, add_length + 1);
            free(copy);
            entry->value = joined;
            return JS_OK;
        }
    }
    if (headers->count >= JSDOM_HEADERS_MAX) {
        free(copy);
        return js_throw_error(c, JS_ERR_RANGE, "too many headers");
    }
    headers->entries[headers->count].name = dup_z(normalized);
    if (!headers->entries[headers->count].name) {
        free(copy);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    headers->entries[headers->count].value = copy;
    headers->count++;
    return JS_OK;
}

static int headers_fill(js_ctx *c, struct headers_state *headers,
                        js_value initializer)
{
    struct headers_state *source = headers_state_of(initializer);
    unsigned long i, count;

    if (js_is_undefined(initializer) || js_is_null(initializer))
        return JS_OK;
    if (source) {
        for (i = 0; i < source->count; i++)
            if (headers_store(c, headers, source->entries[i].name,
                              source->entries[i].value, 0) != JS_OK)
                return JS_THROW;
        return JS_OK;
    }
    if (js_is_array(initializer)) {
        count = js_array_length(initializer.u.obj);
        if (count > JSDOM_HEADERS_MAX)
            return js_throw_error(c, JS_ERR_RANGE, "too many headers");
        for (i = 0; i < count; i++) {
            js_value pair, name_value, field_value;
            const char *name, *value;

            if (js_array_get(c, initializer.u.obj, i, &pair) != JS_OK ||
                !js_is_array(pair) ||
                js_array_length(pair.u.obj) != 2 ||
                js_array_get(c, pair.u.obj, 0, &name_value) != JS_OK ||
                js_array_get(c, pair.u.obj, 1, &field_value) != JS_OK)
                return js_throw_error(c, JS_ERR_TYPE,
                                      "header pair must contain two values");
            name = js_to_cstring(c, name_value);
            if (!name)
                return JS_THROW;
            value = js_to_cstring(c, field_value);
            if (!value ||
                headers_store(c, headers, name, value, 0) != JS_OK)
                return JS_THROW;
        }
        return JS_OK;
    }
    if (js_is_object(initializer)) {
        js_value object_ctor, keys_fn, keys, key, field;

        if (js_get(c, js_global(c), "Object", &object_ctor) != JS_OK ||
            js_get(c, object_ctor, "keys", &keys_fn) != JS_OK ||
            js_call(c, keys_fn, object_ctor, 1, &initializer, &keys) !=
                JS_OK ||
            !js_is_array(keys))
            return JS_THROW;
        count = js_array_length(keys.u.obj);
        if (count > JSDOM_HEADERS_MAX)
            return js_throw_error(c, JS_ERR_RANGE, "too many headers");
        for (i = 0; i < count; i++) {
            const char *name, *value;

            if (js_array_get(c, keys.u.obj, i, &key) != JS_OK ||
                js_get_value(c, initializer, key, &field) != JS_OK)
                return JS_THROW;
            name = js_to_cstring(c, key);
            if (!name)
                return JS_THROW;
            value = js_to_cstring(c, field);
            if (!value ||
                headers_store(c, headers, name, value, 0) != JS_OK)
                return JS_THROW;
        }
        return JS_OK;
    }
    return js_throw_error(c, JS_ERR_TYPE, "invalid Headers initializer");
}

static int headers_constructor(js_ctx *c, js_value t,
                               int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    struct headers_state *headers;
    js_object *object;
    (void)t;

    if (!j || !js_is_constructing(c))
        return js_throw_error(c, JS_ERR_TYPE,
                              "Headers must be constructed with new");
    headers = headers_state_new();
    if (!headers)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    if (headers_fill(c, headers, ac ? av[0] : js_undefined()) != JS_OK) {
        headers_release(headers);
        return JS_THROW;
    }
    object = new_host_data(j, 0, HOST_HEADERS, headers, j->headers_proto);
    if (!object) {
        headers_release(headers);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    *r = js_object_value(object);
    return JS_OK;
}

static int headers_get(js_ctx *c, js_value t, int ac, js_value *av,
                       js_value *r)
{
    struct headers_state *headers = headers_state_of(t);
    char normalized[JSDOM_HEADER_NAME_MAX + 1];
    const char *name;
    unsigned int i;

    if (!headers)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Headers receiver");
    if (string_arg(c, ac, av, 0, &name) != JS_OK ||
        header_name_normalize(c, name, normalized) != JS_OK)
        return JS_THROW;
    for (i = 0; i < headers->count; i++)
        if (!strcmp(headers->entries[i].name, normalized)) {
            *r = js_mkcstring(c, headers->entries[i].value);
            return js_fatal(c) ? JS_THROW : JS_OK;
        }
    *r = js_null();
    return JS_OK;
}

static int headers_has(js_ctx *c, js_value t, int ac, js_value *av,
                       js_value *r)
{
    struct headers_state *headers = headers_state_of(t);
    char normalized[JSDOM_HEADER_NAME_MAX + 1];
    const char *name;
    unsigned int i;

    if (!headers)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Headers receiver");
    if (string_arg(c, ac, av, 0, &name) != JS_OK ||
        header_name_normalize(c, name, normalized) != JS_OK)
        return JS_THROW;
    for (i = 0; i < headers->count; i++)
        if (!strcmp(headers->entries[i].name, normalized)) {
            *r = js_bool(1);
            return JS_OK;
        }
    *r = js_bool(0);
    return JS_OK;
}

static int headers_append_method(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct headers_state *headers = headers_state_of(t);
    const char *name, *value;

    if (!headers)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Headers receiver");
    if (string_arg(c, ac, av, 0, &name) != JS_OK ||
        string_arg(c, ac, av, 1, &value) != JS_OK ||
        headers_store(c, headers, name, value, 0) != JS_OK)
        return JS_THROW;
    *r = js_undefined();
    return JS_OK;
}

static int headers_set_method(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    struct headers_state *headers = headers_state_of(t);
    const char *name, *value;

    if (!headers)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Headers receiver");
    if (string_arg(c, ac, av, 0, &name) != JS_OK ||
        string_arg(c, ac, av, 1, &value) != JS_OK ||
        headers_store(c, headers, name, value, 1) != JS_OK)
        return JS_THROW;
    *r = js_undefined();
    return JS_OK;
}

static int headers_delete_method(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct headers_state *headers = headers_state_of(t);
    char normalized[JSDOM_HEADER_NAME_MAX + 1];
    const char *name;
    unsigned int i;

    if (!headers)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Headers receiver");
    if (string_arg(c, ac, av, 0, &name) != JS_OK ||
        header_name_normalize(c, name, normalized) != JS_OK)
        return JS_THROW;
    for (i = 0; i < headers->count; i++) {
        if (strcmp(headers->entries[i].name, normalized))
            continue;
        free(headers->entries[i].name);
        free(headers->entries[i].value);
        for (; i + 1 < headers->count; i++)
            headers->entries[i] = headers->entries[i + 1];
        headers->count--;
        memset(&headers->entries[headers->count], 0,
               sizeof(headers->entries[headers->count]));
        break;
    }
    *r = js_undefined();
    return JS_OK;
}

static int headers_for_each(js_ctx *c, js_value t,
                            int ac, js_value *av, js_value *r)
{
    struct headers_state *headers = headers_state_of(t);
    unsigned int i;

    if (!headers)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Headers receiver");
    if (ac < 1 || !js_is_function(av[0]))
        return js_throw_error(c, JS_ERR_TYPE,
                              "Headers.forEach requires a callback");
    for (i = 0; i < headers->count; i++) {
        js_value args[3], ignored;

        args[0] = js_mkcstring(c, headers->entries[i].value);
        args[1] = js_mkcstring(c, headers->entries[i].name);
        args[2] = t;
        if (js_fatal(c) ||
            js_call(c, av[0], ac > 1 ? av[1] : js_undefined(),
                    3, args, &ignored) != JS_OK)
            return JS_THROW;
    }
    *r = js_undefined();
    return JS_OK;
}

static int request_header_forbidden(const char *name)
{
    return !strcmp(name, "accept-encoding") ||
           !strcmp(name, "connection") ||
           !strcmp(name, "content-length") ||
           !strcmp(name, "cookie") ||
           !strcmp(name, "cookie2") ||
           !strcmp(name, "host") ||
           !strcmp(name, "keep-alive") ||
           !strcmp(name, "origin") ||
           !strcmp(name, "referer") ||
           !strcmp(name, "te") ||
           !strcmp(name, "trailer") ||
           !strcmp(name, "transfer-encoding") ||
           !strcmp(name, "upgrade") ||
           !strcmp(name, "user-agent") ||
           !strcmp(name, "via") ||
           !strncmp(name, "proxy-", 6) ||
           !strncmp(name, "sec-", 4);
}

static int headers_request_fields(js_ctx *c, struct headers_state *headers,
                                  char **content_type, char **accept,
                                  char **extra_headers)
{
    unsigned long extra_length = 0, used = 0;
    unsigned int i;

    *content_type = 0;
    *accept = 0;
    *extra_headers = 0;
    for (i = 0; i < headers->count; i++) {
        struct header_entry *entry = &headers->entries[i];

        if (request_header_forbidden(entry->name))
            return js_throw_error(c, JS_ERR_TYPE,
                                  "forbidden fetch request header: %s",
                                  entry->name);
        if (!strcmp(entry->name, "content-type")) {
            *content_type = dup_z(entry->value);
            if (!*content_type)
                goto oom;
        } else if (!strcmp(entry->name, "accept")) {
            *accept = dup_z(entry->value);
            if (!*accept)
                goto oom;
        } else {
            unsigned long name_length = strlen(entry->name);
            unsigned long value_length = strlen(entry->value);

            if (extra_length >
                JSDOM_REQUEST_HEADERS_MAX -
                name_length - value_length - 4)
                return js_throw_error(c, JS_ERR_RANGE,
                                      "fetch request headers are too large");
            extra_length += name_length + value_length + 4;
        }
    }
    if (extra_length) {
        *extra_headers = (char *)malloc(extra_length + 1);
        if (!*extra_headers)
            goto oom;
        for (i = 0; i < headers->count; i++) {
            struct header_entry *entry = &headers->entries[i];
            unsigned long name_length, value_length;

            if (!strcmp(entry->name, "content-type") ||
                !strcmp(entry->name, "accept"))
                continue;
            name_length = strlen(entry->name);
            value_length = strlen(entry->value);
            memcpy(*extra_headers + used, entry->name, name_length);
            used += name_length;
            (*extra_headers)[used++] = ':';
            (*extra_headers)[used++] = ' ';
            memcpy(*extra_headers + used, entry->value, value_length);
            used += value_length;
            (*extra_headers)[used++] = '\r';
            (*extra_headers)[used++] = '\n';
        }
        (*extra_headers)[used] = 0;
    }
    return JS_OK;

oom:
    free(*content_type);
    free(*accept);
    free(*extra_headers);
    *content_type = 0;
    *accept = 0;
    *extra_headers = 0;
    return js_throw_error(c, JS_ERR_ERROR, "out of memory");
}

static int headers_contains_name(struct headers_state *headers,
                                 const char *normalized)
{
    unsigned int i;

    for (i = 0; i < headers->count; i++)
        if (!strcmp(headers->entries[i].name, normalized))
            return 1;
    return 0;
}

static struct headers_state *headers_clone_state(
    js_ctx *c, const struct headers_state *source)
{
    struct headers_state *copy = headers_state_new();
    unsigned int i;

    if (!copy) {
        js_throw_error(c, JS_ERR_ERROR, "out of memory");
        return 0;
    }
    for (i = 0; i < source->count; i++)
        if (headers_store(c, copy, source->entries[i].name,
                          source->entries[i].value, 0) != JS_OK) {
            headers_release(copy);
            return 0;
        }
    return copy;
}

static struct file_state *file_state_of(js_value value)
{
    struct hostref *h = host(value, HOST_FILE);

    return h ? (struct file_state *)h->data : 0;
}

static int jsdom_blob_info(js_value v, const void **data, unsigned long *len,
                           const char **type, const char **fname)
{
    struct file_state *f = file_state_of(v);
    const void *p;

    if (f) {
        if (data) *data = f->data ? f->data : "";
        if (len) *len = f->length;
        if (type) *type = f->type;
        if (fname) *fname = f->name;
        return 1;
    }
    p = js_blob_data(v, len, type);
    if (p || js_is_blob(v)) {
        if (data) *data = p ? p : "";
        if (fname) *fname = 0;
        return 1;
    }
    return 0;
}

static int jsdom_file_extract(void *user, js_value v, const void **data,
                              unsigned long *len, const char **type,
                              const char **filename)
{
    struct file_state *f = file_state_of(v);

    (void)user;
    if (!f)
        return 0;
    if (data) *data = f->data ? f->data : "";
    if (len) *len = f->length;
    if (type) *type = f->type;
    if (filename) *filename = f->name;
    return 1;
}

static int file_type_set(js_ctx *c, char dst[128], js_value value)
{
    js_value string_value;
    const char *bytes;
    unsigned long length, i;

    dst[0] = 0;
    if (js_is_undefined(value))
        return JS_OK;
    if (js_to_string(c, value, &string_value) != JS_OK)
        return JS_THROW;
    bytes = js_string_bytes(string_value, &length);
    if (length >= 128)
        return JS_OK;
    for (i = 0; i < length; i++)
        if ((unsigned char)bytes[i] < 0x20 ||
            (unsigned char)bytes[i] > 0x7e)
            return JS_OK;
    for (i = 0; i < length; i++)
        dst[i] = (char)ci((unsigned char)bytes[i]);
    dst[length] = 0;
    return JS_OK;
}

static int file_append_part(js_ctx *c, struct file_state *file,
                            js_value part)
{
    js_value string_value;
    const void *bytes = 0;
    unsigned long length = 0;
    char *grown;

    if (!jsdom_blob_info(part, &bytes, &length, 0, 0)) {
        bytes = js_buffersource_data(part, &length);
        if (!bytes) {
            if (js_to_string(c, part, &string_value) != JS_OK)
                return JS_THROW;
            bytes = js_string_bytes(string_value, &length);
        }
    }
    if (length > JSDOM_RESPONSE_BODY_MAX - file->length)
        return js_throw_error(c, JS_ERR_RANGE, "File is too large");
    if (!length)
        return JS_OK;
    grown = (char *)realloc(file->data, file->length + length);
    if (!grown)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    memcpy(grown + file->length, bytes, length);
    file->data = grown;
    file->length += length;
    return JS_OK;
}

static int file_constructor(js_ctx *c, js_value t,
                            int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    struct file_state *file;
    js_object *object;
    js_value name_value, options, value, date, now;
    const char *name_bytes;
    unsigned long name_length, i, count;
    (void)t;

    if (!j || !js_is_constructing(c))
        return js_throw_error(c, JS_ERR_TYPE,
                              "File must be constructed with new");
    if (ac < 2 || !js_is_array(av[0]))
        return js_throw_error(c, JS_ERR_TYPE,
                              "File requires parts and a name");
    file = (struct file_state *)calloc(1, sizeof(*file));
    if (!file)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    count = js_array_length(av[0].u.obj);
    for (i = 0; i < count; i++) {
        js_value part;

        if (js_array_get(c, av[0].u.obj, i, &part) != JS_OK ||
            file_append_part(c, file, part) != JS_OK)
            goto fail;
    }
    if (js_to_string(c, av[1], &name_value) != JS_OK)
        goto fail;
    name_bytes = js_string_bytes(name_value, &name_length);
    if (name_length > 255) {
        js_throw_error(c, JS_ERR_RANGE, "File name is too long");
        goto fail;
    }
    for (i = 0; i < name_length; i++)
        if (!name_bytes[i]) {
            js_throw_error(c, JS_ERR_TYPE, "File name contains NUL");
            goto fail;
        }
    file->name = dup_n(name_bytes, name_length);
    if (!file->name) {
        js_throw_error(c, JS_ERR_ERROR, "out of memory");
        goto fail;
    }
    if (js_get(c, js_global(c), "Date", &date) != JS_OK ||
        js_get(c, date, "now", &value) != JS_OK ||
        !js_is_function(value) ||
        js_call(c, value, date, 0, 0, &now) != JS_OK ||
        js_to_number(c, now, &file->last_modified) != JS_OK)
        goto fail;
    options = ac > 2 ? av[2] : js_undefined();
    value = js_undefined();
    if (js_is_object(options) &&
        js_get(c, options, "type", &value) != JS_OK)
        goto fail;
    if (file_type_set(c, file->type, value) != JS_OK)
        goto fail;
    if (js_is_object(options)) {
        double number;

        if (js_get(c, options, "lastModified", &value) != JS_OK)
            goto fail;
        if (!js_is_undefined(value)) {
            if (js_to_number(c, value, &number) != JS_OK)
                goto fail;
            file->last_modified = number;
        }
    }
    object = new_host_data(j, 0, HOST_FILE, file, j->file_proto);
    if (!object) {
        js_throw_error(c, JS_ERR_ERROR, "out of memory");
        goto fail;
    }
    *r = js_object_value(object);
    return JS_OK;

fail:
    file_release(file);
    return JS_THROW;
}

static int file_name_get(js_ctx *c, js_value t,
                         int ac, js_value *av, js_value *r)
{
    struct file_state *file = file_state_of(t);
    (void)ac; (void)av;

    if (!file)
        return js_throw_error(c, JS_ERR_TYPE, "invalid File receiver");
    *r = js_mkcstring(c, file->name);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int file_last_modified_get(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    struct file_state *file = file_state_of(t);
    (void)ac; (void)av;

    if (!file)
        return js_throw_error(c, JS_ERR_TYPE, "invalid File receiver");
    *r = js_number(file->last_modified);
    return JS_OK;
}

static int file_relative_path_get(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    struct file_state *file = file_state_of(t);
    (void)ac; (void)av;

    if (!file)
        return js_throw_error(c, JS_ERR_TYPE, "invalid File receiver");
    *r = js_mkcstring(c, "");
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int url_create_object_url(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    const void *blob_data = 0;
    unsigned long blob_len = 0;
    const char *blob_type = 0;
    char generated[URL_MAX + 64];
    char *url, *data;
    int slot;
    (void)t;

    if (!j || !ac ||
        !jsdom_blob_info(av[0], &blob_data, &blob_len, &blob_type, 0))
        return js_throw_error(c, JS_ERR_TYPE,
                              "createObjectURL requires a Blob");
    for (slot = 0; slot < JSDOM_OBJECT_URL_MAX; slot++)
        if (!j->object_urls[slot].url)
            break;
    if (slot == JSDOM_OBJECT_URL_MAX)
        return js_throw_error(c, JS_ERR_RANGE,
                              "too many active object URLs");
    snprintf(generated, sizeof(generated), "blob:%s/%08lx",
             j->origin, ++j->object_url_counter);
    url = dup_z(generated);
    data = dup_n(blob_data ? (const char *)blob_data : "", blob_len);
    if (!url || !data) {
        free(url);
        free(data);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    j->object_urls[slot].url = url;
    j->object_urls[slot].data = data;
    j->object_urls[slot].length = blob_len;
    snprintf(j->object_urls[slot].type, sizeof(j->object_urls[slot].type),
             "%s", blob_type ? blob_type : "");
    *r = js_mkcstring(c, url);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int url_revoke_object_url(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    const char *url;
    int i;
    (void)t;

    if (!j || string_arg(c, ac, av, 0, &url) != JS_OK)
        return JS_THROW;
    for (i = 0; i < JSDOM_OBJECT_URL_MAX; i++)
        if (j->object_urls[i].url &&
            !strcmp(j->object_urls[i].url, url)) {
            free(j->object_urls[i].url);
            free(j->object_urls[i].data);
            memset(&j->object_urls[i], 0,
                   sizeof(j->object_urls[i]));
            break;
        }
    *r = js_undefined();
    return JS_OK;
}

static struct file_reader_state *file_reader_state_of(js_value value)
{
    struct hostref *h = host(value, HOST_FILE_READER);

    return h ? (struct file_reader_state *)h->data : 0;
}

static int file_reader_constructor(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    struct file_reader_state *reader;
    js_object *object;
    (void)t; (void)ac; (void)av;

    if (!j || !js_is_constructing(c))
        return js_throw_error(c, JS_ERR_TYPE,
                              "FileReader must be constructed with new");
    reader = (struct file_reader_state *)calloc(1, sizeof(*reader));
    if (!reader)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    reader->result = js_null();
    reader->error = js_null();
    reader->pending_result = js_null();
    object = new_host_data(j, 0, HOST_FILE_READER, reader,
                           j->file_reader_proto);
    if (!object) {
        free(reader);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    *r = js_object_value(object);
    return JS_OK;
}

static int file_reader_ready_get(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct file_reader_state *reader = file_reader_state_of(t);
    (void)ac; (void)av;

    if (!reader)
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid FileReader receiver");
    *r = js_number((double)reader->ready_state);
    return JS_OK;
}

static int file_reader_result_get(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    struct file_reader_state *reader = file_reader_state_of(t);
    (void)ac; (void)av;

    if (!reader)
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid FileReader receiver");
    *r = reader->result;
    return JS_OK;
}

static int file_reader_error_get(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct file_reader_state *reader = file_reader_state_of(t);
    (void)ac; (void)av;

    if (!reader)
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid FileReader receiver");
    *r = reader->error;
    return JS_OK;
}

static int file_reader_data_url(js_ctx *c, const char *data, unsigned long length,
                                const char *blob_type, js_value *r)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *type = blob_type && blob_type[0]
        ? blob_type : "application/octet-stream";
    unsigned long prefix_length = 5 + strlen(type) + 8;
    unsigned long encoded_length, total, i, write;
    char *output;

    if (length > (~0UL - 2) / 3)
        return js_throw_error(c, JS_ERR_RANGE, "FileReader input is too large");
    encoded_length = ((length + 2) / 3) * 4;
    if (prefix_length > ~0UL - encoded_length)
        return js_throw_error(c, JS_ERR_RANGE, "FileReader input is too large");
    total = prefix_length + encoded_length;
    output = (char *)malloc(total + 1);
    if (!output)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    write = (unsigned long)snprintf(output, prefix_length + 1,
                                    "data:%s;base64,", type);
    for (i = 0; i < length; i += 3) {
        unsigned a = (unsigned char)data[i];
        unsigned b = i + 1 < length ? (unsigned char)data[i + 1] : 0;
        unsigned d = i + 2 < length ? (unsigned char)data[i + 2] : 0;

        output[write++] = table[a >> 2];
        output[write++] = table[((a & 3) << 4) | (b >> 4)];
        output[write++] = i + 1 < length
            ? table[((b & 15) << 2) | (d >> 6)] : '=';
        output[write++] = i + 2 < length
            ? table[d & 63] : '=';
    }
    output[write] = 0;
    *r = js_mkstring(c, output, write);
    free(output);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int file_reader_schedule(js_ctx *c, js_value t,
                                js_value blob_val, int mode,
                                js_value *r)
{
    struct hostref *reader_host = host(t, HOST_FILE_READER);
    struct file_reader_state *reader;
    struct jsdom *j;
    const void *data = 0;
    unsigned long length = 0;
    const char *blob_type = 0;
    js_value result;
    int slot;

    if (!reader_host)
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid FileReader receiver");
    if (!jsdom_blob_info(blob_val, &data, &length, &blob_type, 0))
        return js_throw_error(c, JS_ERR_TYPE,
                              "FileReader requires a Blob");
    reader = (struct file_reader_state *)reader_host->data;
    if (reader->ready_state == 1)
        return js_throw_error(c, JS_ERR_ERROR,
                              "FileReader is already loading");
    if (mode == 1)
        result = js_arraybuffer_new(c, data, length);
    else if (mode == 3) {
        if (file_reader_data_url(c, (const char *)data, length,
                                 blob_type, &result) != JS_OK)
            return JS_THROW;
    } else
        result = js_mkstring(c, data ? (const char *)data : "", length);
    if (js_fatal(c))
        return JS_THROW;
    j = reader_host->j;
    for (slot = 0;
         slot < (int)(sizeof(j->file_readers) /
                      sizeof(j->file_readers[0])); slot++)
        if (j->file_readers[slot].state == TASK_FREE)
            break;
    if (slot == (int)(sizeof(j->file_readers) /
                      sizeof(j->file_readers[0])))
        return js_throw_error(c, JS_ERR_RANGE,
                              "too many pending FileReader operations");
    reader->generation++;
    reader->ready_state = 1;
    reader->aborted = 0;
    reader->total = length;
    reader->result = js_null();
    reader->error = js_null();
    reader->pending_result = result;
    j->file_readers[slot].reader = t;
    j->file_readers[slot].generation = reader->generation;
    j->file_readers[slot].state = TASK_QUEUED;
    *r = js_undefined();
    return JS_OK;
}

static int file_reader_read_text(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    return file_reader_schedule(c, t, ac ? av[0] : js_undefined(), 0, r);
}

static int file_reader_read_arraybuffer(js_ctx *c, js_value t,
                                        int ac, js_value *av, js_value *r)
{
    return file_reader_schedule(c, t, ac ? av[0] : js_undefined(), 1, r);
}

static int file_reader_read_binary(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    return file_reader_schedule(c, t, ac ? av[0] : js_undefined(), 2, r);
}

static int file_reader_read_data_url(js_ctx *c, js_value t,
                                     int ac, js_value *av, js_value *r)
{
    return file_reader_schedule(c, t, ac ? av[0] : js_undefined(), 3, r);
}

static int file_reader_abort(js_ctx *c, js_value t,
                             int ac, js_value *av, js_value *r)
{
    struct file_reader_state *reader = file_reader_state_of(t);
    (void)ac; (void)av;

    if (!reader)
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid FileReader receiver");
    reader->result = js_null();
    if (reader->ready_state == 1) {
        if (make_abort_reason(c, &reader->error) != JS_OK)
            return JS_THROW;
        reader->ready_state = 2;
        reader->aborted = 1;
    } else {
        reader->error = js_null();
    }
    *r = js_undefined();
    return JS_OK;
}

static int file_reader_dispatch(struct jsdom *j, js_value reader,
                                const char *type, unsigned long loaded,
                                unsigned long total,
                                char *err, unsigned long errsz)
{
    js_value event, options;
    js_object *option_object = js_new_object(j->ctx);

    if (!option_object)
        return -1;
    options = js_object_value(option_object);
    if (js_set(j->ctx, options, "bubbles", js_bool(0)) != JS_OK ||
        js_set(j->ctx, options, "cancelable", js_bool(0)) != JS_OK ||
        event_make(j, j->event_proto, type, options,
                   js_undefined(), 0, &event) != JS_OK ||
        js_set(j->ctx, event, "isTrusted", js_bool(1)) != JS_OK ||
        js_set(j->ctx, event, "lengthComputable", js_bool(1)) != JS_OK ||
        js_set(j->ctx, event, "loaded",
               js_number((double)loaded)) != JS_OK ||
        js_set(j->ctx, event, "total",
               js_number((double)total)) != JS_OK)
        return -1;
    return dispatch_event_value(j, reader, event, 0, err, errsz) < 0
        ? -1 : 0;
}

static int web_iterator_result(js_ctx *c, js_value value, int done,
                               js_value *r)
{
    js_object *object = js_new_object(c);

    if (!object ||
        js_set(c, js_object_value(object), "value", value) != JS_OK ||
        js_set(c, js_object_value(object), "done",
               js_bool(done)) != JS_OK)
        return JS_THROW;
    *r = js_object_value(object);
    return JS_OK;
}

static int web_iterator_new(js_ctx *c, js_value collection,
                            int kind, js_value *r)
{
    struct jsdom *j = binding(c);
    struct web_iterator_state *iterator;
    js_object *object;

    if (!j)
        return js_throw_error(c, JS_ERR_ERROR,
                              "iterator binding is unavailable");
    iterator = (struct web_iterator_state *)
        calloc(1, sizeof(*iterator));
    if (!iterator)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    iterator->kind = kind;
    iterator->collection = collection;
    object = new_host_data(j, 0, HOST_WEB_ITERATOR, iterator,
                           j->web_iterator_proto);
    if (!object) {
        free(iterator);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    *r = js_object_value(object);
    return JS_OK;
}

static int web_iterator_next(js_ctx *c, js_value t,
                             int ac, js_value *av, js_value *r)
{
    struct hostref *iterator_host = host(t, HOST_WEB_ITERATOR);
    struct web_iterator_state *iterator;
    struct headers_state *headers;
    struct header_entry *entry;
    js_value value;
    (void)ac; (void)av;

    if (!iterator_host)
        return js_throw_error(c, JS_ERR_TYPE, "invalid iterator receiver");
    iterator = (struct web_iterator_state *)iterator_host->data;
    headers = headers_state_of(iterator->collection);
    if (!headers || iterator->kind < ITER_HEADERS_ENTRIES ||
        iterator->kind > ITER_HEADERS_VALUES)
        return js_throw_error(c, JS_ERR_TYPE,
                              "Headers iterator source is invalid");
    if (iterator->index >= headers->count)
        return web_iterator_result(c, js_undefined(), 1, r);
    entry = &headers->entries[iterator->index++];
    if (iterator->kind == ITER_HEADERS_KEYS)
        value = js_mkcstring(c, entry->name);
    else if (iterator->kind == ITER_HEADERS_VALUES)
        value = js_mkcstring(c, entry->value);
    else {
        js_object *pair = js_new_array(c);

        if (!pair ||
            js_array_push(c, pair,
                          js_mkcstring(c, entry->name)) != JS_OK ||
            js_array_push(c, pair,
                          js_mkcstring(c, entry->value)) != JS_OK)
            return JS_THROW;
        value = js_object_value(pair);
    }
    if (js_fatal(c))
        return JS_THROW;
    return web_iterator_result(c, value, 0, r);
}

static int headers_entries(js_ctx *c, js_value t,
                           int ac, js_value *av, js_value *r)
{
    (void)ac; (void)av;
    if (!headers_state_of(t))
        return js_throw_error(c, JS_ERR_TYPE, "invalid Headers receiver");
    return web_iterator_new(c, t, ITER_HEADERS_ENTRIES, r);
}

static int headers_keys(js_ctx *c, js_value t,
                        int ac, js_value *av, js_value *r)
{
    (void)ac; (void)av;
    if (!headers_state_of(t))
        return js_throw_error(c, JS_ERR_TYPE, "invalid Headers receiver");
    return web_iterator_new(c, t, ITER_HEADERS_KEYS, r);
}

static int headers_values(js_ctx *c, js_value t,
                          int ac, js_value *av, js_value *r)
{
    (void)ac; (void)av;
    if (!headers_state_of(t))
        return js_throw_error(c, JS_ERR_TYPE, "invalid Headers receiver");
    return web_iterator_new(c, t, ITER_HEADERS_VALUES, r);
}

struct form_buffer {
    char *data;
    unsigned long length;
    unsigned long capacity;
};

static int form_buffer_put(js_ctx *c, struct form_buffer *buffer,
                           const void *data, unsigned long length)
{
    char *grown;
    unsigned long capacity;

    if (length > JSDOM_RESPONSE_BODY_MAX - buffer->length)
        return js_throw_error(c, JS_ERR_RANGE,
                              "encoded FormData is too large");
    if (buffer->length + length > buffer->capacity) {
        capacity = buffer->capacity ? buffer->capacity : 256;
        while (capacity < buffer->length + length) {
            if (capacity > JSDOM_RESPONSE_BODY_MAX / 2) {
                capacity = JSDOM_RESPONSE_BODY_MAX;
                break;
            }
            capacity *= 2;
        }
        grown = (char *)realloc(buffer->data, capacity);
        if (!grown)
            return js_throw_error(c, JS_ERR_ERROR, "out of memory");
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    if (length)
        memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    return JS_OK;
}

static int form_buffer_string(js_ctx *c, struct form_buffer *buffer,
                              const char *text)
{
    return form_buffer_put(c, buffer, text, strlen(text));
}

static int form_buffer_quoted(js_ctx *c, struct form_buffer *buffer,
                              const char *text)
{
    for (; *text; text++) {
        const char *escaped = 0;

        if (*text == '"') escaped = "%22";
        else if (*text == '\r') escaped = "%0D";
        else if (*text == '\n') escaped = "%0A";
        if (escaped) {
            if (form_buffer_string(c, buffer, escaped) != JS_OK)
                return JS_THROW;
        } else if (form_buffer_put(c, buffer, text, 1) != JS_OK) {
            return JS_THROW;
        }
    }
    return JS_OK;
}

static int formdata_encode(js_ctx *c, js_value form,
                           char **data, unsigned long *length,
                           char content_type[128])
{
    static unsigned int boundary_counter;
    struct form_buffer buffer;
    char boundary[64];
    unsigned int i, count;

    memset(&buffer, 0, sizeof(buffer));
    snprintf(boundary, sizeof(boundary),
             "----KestrelFormBoundary%08x", ++boundary_counter);
    snprintf(content_type, 128, "multipart/form-data; boundary=%s",
             boundary);
    count = js_formdata_count(form);
    for (i = 0; i < count; i++) {
        const char *name = 0, *filename = 0;
        js_value val = js_undefined();
        const void *bdata = 0;
        unsigned long blen = 0;
        const char *btype = 0, *bfname = 0;
        const char *bytes;
        unsigned long value_length;

        if (!js_formdata_entry(form, i, &name, &val, &filename))
            continue;
        if (form_buffer_string(c, &buffer, "--") != JS_OK ||
            form_buffer_string(c, &buffer, boundary) != JS_OK ||
            form_buffer_string(c, &buffer,
                               "\r\nContent-Disposition: form-data; name=\"")
                != JS_OK ||
            form_buffer_quoted(c, &buffer, name) != JS_OK ||
            form_buffer_string(c, &buffer, "\"") != JS_OK)
            goto fail;
        if (jsdom_blob_info(val, &bdata, &blen, &btype, &bfname)) {
            const char *eff_name = filename ? filename
                                            : (bfname ? bfname : "blob");
            const char *eff_type = (btype && btype[0])
                ? btype : "application/octet-stream";

            if (form_buffer_string(c, &buffer, "; filename=\"") != JS_OK ||
                form_buffer_quoted(c, &buffer, eff_name) != JS_OK ||
                form_buffer_string(c, &buffer, "\"\r\nContent-Type: ") !=
                    JS_OK ||
                form_buffer_string(c, &buffer, eff_type) != JS_OK)
                goto fail;
            bytes = bdata ? (const char *)bdata : "";
            value_length = blen;
        } else {
            bytes = js_string_bytes(val, &value_length);
        }
        if (form_buffer_string(c, &buffer, "\r\n\r\n") != JS_OK ||
            form_buffer_put(c, &buffer, bytes, value_length) != JS_OK ||
            form_buffer_string(c, &buffer, "\r\n") != JS_OK)
            goto fail;
    }
    if (form_buffer_string(c, &buffer, "--") != JS_OK ||
        form_buffer_string(c, &buffer, boundary) != JS_OK ||
        form_buffer_string(c, &buffer, "--\r\n") != JS_OK)
        goto fail;
    *data = buffer.data;
    *length = buffer.length;
    return JS_OK;

fail:
    free(buffer.data);
    return JS_THROW;
}

static int response_build(js_ctx *c, struct jsdom *j,
                          js_value body_value, js_value options,
                          const char *default_content_type, js_value *r)
{
    struct response_state *state;
    struct headers_state *headers;
    js_object *object;
    js_value value, string_value;
    const void *body_bytes = 0;
    const char *status_text = "";
    unsigned long body_length = 0, status_text_length = 0, i;
    char *body = 0, *status_copy = 0, *url_copy = 0;
    char generated_content_type[128];
    int status = 200, body_present, body_encoded = 0;

    headers = headers_state_new();
    if (!headers)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    if (js_is_object(options)) {
        double number;

        if (js_get(c, options, "status", &value) != JS_OK)
            goto fail;
        if (!js_is_undefined(value)) {
            if (js_to_number(c, value, &number) != JS_OK)
                goto fail;
            if (!(number >= 200.0 && number <= 599.0) ||
                number != (double)(int)number) {
                js_throw_error(c, JS_ERR_RANGE,
                               "Response status is out of range");
                goto fail;
            }
            status = (int)number;
        }
        if (js_get(c, options, "statusText", &value) != JS_OK)
            goto fail;
        if (!js_is_undefined(value)) {
            const char *bytes;

            if (js_to_string(c, value, &string_value) != JS_OK)
                goto fail;
            bytes = js_string_bytes(string_value, &status_text_length);
            if (status_text_length > 128) {
                js_throw_error(c, JS_ERR_RANGE,
                               "Response statusText is too long");
                goto fail;
            }
            for (i = 0; i < status_text_length; i++)
                if (!bytes[i] || bytes[i] == '\r' || bytes[i] == '\n') {
                    js_throw_error(c, JS_ERR_TYPE,
                                   "invalid Response statusText");
                    goto fail;
                }
            status_text = bytes;
        }
        if (js_get(c, options, "headers", &value) != JS_OK ||
            headers_fill(c, headers, value) != JS_OK)
            goto fail;
    }
    body_present = !js_is_undefined(body_value) && !js_is_null(body_value);
    if (body_present && (status == 204 || status == 205 || status == 304)) {
        js_throw_error(c, JS_ERR_TYPE,
                       "Response status cannot have a body");
        goto fail;
    }
    if (body_present) {
        const char *btype = 0;

        if (js_is_formdata(body_value)) {
            if (formdata_encode(c, body_value, &body, &body_length,
                                generated_content_type) != JS_OK)
                goto fail;
            body_bytes = body;
            body_encoded = 1;
            if (!default_content_type)
                default_content_type = generated_content_type;
        } else if (js_is_urlsearchparams(body_value)) {
            if (js_urlsearchparams_string(c, body_value,
                                          &string_value) != JS_OK)
                goto fail;
            body_bytes = js_string_bytes(string_value, &body_length);
            if (!default_content_type)
                default_content_type =
                    "application/x-www-form-urlencoded;charset=UTF-8";
        } else if (jsdom_blob_info(body_value, &body_bytes, &body_length,
                                   &btype, 0)) {
            if (!default_content_type && btype && btype[0])
                default_content_type = btype;
        } else if ((body_bytes = js_buffersource_data(body_value,
                                                      &body_length)) != 0) {
        } else {
            if (js_to_string(c, body_value, &string_value) != JS_OK)
                goto fail;
            body_bytes = js_string_bytes(string_value, &body_length);
            if (!default_content_type)
                default_content_type = "text/plain;charset=UTF-8";
        }
        if (body_length > JSDOM_RESPONSE_BODY_MAX) {
            js_throw_error(c, JS_ERR_RANGE, "Response body is too large");
            goto fail;
        }
        if (!body_encoded) {
            body = dup_n(body_bytes ? (const char *)body_bytes : "",
                         body_length);
            if (!body) {
                js_throw_error(c, JS_ERR_ERROR, "out of memory");
                goto fail;
            }
        }
        if (default_content_type &&
            !headers_contains_name(headers, "content-type") &&
            headers_store(c, headers, "content-type",
                          default_content_type, 0) != JS_OK)
            goto fail;
    }
    if (!status_text_length)
        status_text_length = strlen(status_text);
    status_copy = dup_n(status_text, status_text_length);
    url_copy = dup_z("");
    if (!status_copy || !url_copy) {
        js_throw_error(c, JS_ERR_ERROR, "out of memory");
        goto fail;
    }
    state = (struct response_state *)calloc(1, sizeof(*state));
    if (!state) {
        js_throw_error(c, JS_ERR_ERROR, "out of memory");
        goto fail;
    }
    state->refs = 1;
    state->status = status;
    state->status_text = status_copy;
    state->url = url_copy;
    state->headers = headers;
    state->body = body;
    state->body_len = body_length;
    state->kind = RESPONSE_DEFAULT;
    object = new_host_data(j, 0, HOST_RESPONSE, state, j->response_proto);
    if (!object) {
        response_release(state);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    *r = js_object_value(object);
    return JS_OK;

fail:
    free(url_copy);
    free(status_copy);
    free(body);
    headers_release(headers);
    return JS_THROW;
}

static int response_constructor(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    (void)t;

    if (!j || !js_is_constructing(c))
        return js_throw_error(c, JS_ERR_TYPE,
                              "Response must be constructed with new");
    return response_build(c, j, ac ? av[0] : js_undefined(),
                          ac > 1 ? av[1] : js_undefined(), 0, r);
}

static int response_clone(js_ctx *c, js_value t,
                          int ac, js_value *av, js_value *r)
{
    struct hostref *hostref = host(t, HOST_RESPONSE);
    struct response_state *source, *copy;
    struct headers_state *headers;
    js_object *object;
    (void)ac; (void)av;

    if (!hostref)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Response receiver");
    source = (struct response_state *)hostref->data;
    if (source->body_used)
        return js_throw_error(c, JS_ERR_TYPE,
                              "used Response body cannot be cloned");
    headers = headers_clone_state(c, source->headers);
    if (!headers)
        return JS_THROW;
    copy = (struct response_state *)calloc(1, sizeof(*copy));
    if (!copy) {
        headers_release(headers);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    copy->refs = 1;
    copy->status = source->status;
    copy->status_text = dup_z(source->status_text);
    copy->url = dup_z(source->url);
    copy->headers = headers;
    copy->body = source->body ? dup_n(source->body, source->body_len) : 0;
    copy->body_len = source->body_len;
    copy->kind = source->kind;
    copy->redirected = source->redirected;
    if (!copy->status_text || !copy->url ||
        (source->body && !copy->body)) {
        response_release(copy);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    object = new_host_data(hostref->j, 0, HOST_RESPONSE, copy,
                           hostref->j->response_proto);
    if (!object) {
        response_release(copy);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    *r = js_object_value(object);
    return JS_OK;
}

static int response_json_static(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    js_value json, stringify, serialized;
    (void)t;

    if (!j || ac < 1)
        return js_throw_error(c, JS_ERR_TYPE,
                              "Response.json requires a value");
    if (js_get(c, js_global(c), "JSON", &json) != JS_OK ||
        js_get(c, json, "stringify", &stringify) != JS_OK ||
        js_call(c, stringify, json, 1, av, &serialized) != JS_OK)
        return JS_THROW;
    if (!js_is_string(serialized))
        return js_throw_error(c, JS_ERR_TYPE,
                              "value is not JSON serializable");
    return response_build(c, j, serialized,
                          ac > 1 ? av[1] : js_undefined(),
                          "application/json", r);
}

static int response_factory(js_ctx *c, struct jsdom *j, int status,
                            int kind, const char *location, js_value *r)
{
    struct response_state *response;
    struct headers_state *headers;
    js_object *object;

    headers = headers_state_new();
    if (!headers)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    if (location &&
        headers_store(c, headers, "location", location, 0) != JS_OK) {
        headers_release(headers);
        return JS_THROW;
    }
    response = (struct response_state *)calloc(1, sizeof(*response));
    if (!response) {
        headers_release(headers);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    response->refs = 1;
    response->status = status;
    response->status_text = dup_z("");
    response->url = dup_z("");
    response->headers = headers;
    response->kind = kind;
    if (!response->status_text || !response->url) {
        response_release(response);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    object = new_host_data(j, 0, HOST_RESPONSE, response,
                           j->response_proto);
    if (!object) {
        response_release(response);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    *r = js_object_value(object);
    return JS_OK;
}

static int response_redirect_static(js_ctx *c, js_value t,
                                    int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    const char *input;
    char absolute[URL_MAX];
    double status_number = 302;
    int status;
    (void)t;

    if (!j || string_arg(c, ac, av, 0, &input) != JS_OK)
        return JS_THROW;
    if (ac > 1 && js_to_number(c, av[1], &status_number) != JS_OK)
        return JS_THROW;
    if (status_number != 301.0 && status_number != 302.0 &&
        status_number != 303.0 && status_number != 307.0 &&
        status_number != 308.0)
        return js_throw_error(c, JS_ERR_RANGE,
                              "invalid redirect response status");
    if (url_resolve_str(j->base_url, input, absolute,
                        sizeof(absolute)) != URL_OK)
        return js_throw_error(c, JS_ERR_TYPE,
                              "invalid redirect response URL");
    status = (int)status_number;
    return response_factory(c, j, status, RESPONSE_DEFAULT, absolute, r);
}

static int response_error_static(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    (void)t; (void)ac; (void)av;

    if (!j)
        return js_throw_error(c, JS_ERR_ERROR,
                              "Response binding is unavailable");
    return response_factory(c, j, 0, RESPONSE_ERROR, 0, r);
}

static int response_type_get(js_ctx *c, js_value t,
                             int ac, js_value *av, js_value *r)
{
    struct response_state *response =
        response_state_of(t, HOST_RESPONSE);
    const char *type;
    (void)ac; (void)av;
    if (!response)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Response receiver");
    type = response->kind == RESPONSE_ERROR ? "error" :
           response->kind == RESPONSE_BASIC ? "basic" : "default";
    *r = js_mkcstring(c, type);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int response_redirected_get(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    struct response_state *response =
        response_state_of(t, HOST_RESPONSE);
    (void)ac; (void)av;
    if (!response)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Response receiver");
    *r = js_bool(response->redirected);
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
    s->headers->refs++;
    o = new_host_data(rh->j, 0, HOST_HEADERS, s->headers,
                      rh->j->headers_proto);
    if (!o) {
        headers_release(s->headers);
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

static const char *headers_value(struct headers_state *headers,
                                 const char *normalized_name)
{
    unsigned int i;

    if (!headers)
        return 0;
    for (i = 0; i < headers->count; i++)
        if (!strcmp(headers->entries[i].name, normalized_name))
            return headers->entries[i].value;
    return 0;
}

static int mime_type_is(const char *value, const char *expected)
{
    unsigned long i, length = strlen(expected);

    if (!value)
        return 0;
    while (*value == ' ' || *value == '\t')
        value++;
    for (i = 0; i < length; i++)
        if (!value[i] ||
            ci((unsigned char)value[i]) !=
                ci((unsigned char)expected[i]))
            return 0;
    value += length;
    while (*value == ' ' || *value == '\t')
        value++;
    return !*value || *value == ';';
}

static int form_hex_value(unsigned char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    ch = ci(ch);
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    return -1;
}

static int form_url_decode(js_ctx *c, const char *input,
                           unsigned long length, js_value *r,
                           int reject_nul)
{
    char *decoded;
    unsigned long read, write = 0;

    decoded = (char *)malloc(length + 1);
    if (!decoded)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    for (read = 0; read < length; read++) {
        unsigned char ch = (unsigned char)input[read];

        if (ch == '+') {
            ch = ' ';
        } else if (ch == '%' && read + 2 < length) {
            int high = form_hex_value((unsigned char)input[read + 1]);
            int low = form_hex_value((unsigned char)input[read + 2]);

            if (high >= 0 && low >= 0) {
                ch = (unsigned char)((high << 4) | low);
                read += 2;
            }
        }
        if (!ch && reject_nul) {
            free(decoded);
            return js_throw_error(c, JS_ERR_TYPE,
                                  "FormData field name contains NUL");
        }
        decoded[write++] = (char)ch;
    }
    decoded[write] = 0;
    *r = js_mkstring(c, decoded, write);
    free(decoded);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int formdata_from_urlencoded(js_ctx *c, struct jsdom *j,
                                    const char *body,
                                    unsigned long body_length,
                                    js_value *r)
{
    js_value form;
    unsigned long start = 0;
    (void)j;

    form = js_formdata_new(c);
    if (!js_is_formdata(form))
        return JS_THROW;
    while (start < body_length) {
        unsigned long end = start, equals, name_length;
        js_value name, value;

        while (end < body_length && body[end] != '&')
            end++;
        if (end == start) {
            start = end + (end < body_length);
            continue;
        }
        equals = start;
        while (equals < end && body[equals] != '=')
            equals++;
        name_length = equals - start;
        if (form_url_decode(c, body + start, name_length,
                            &name, 1) != JS_OK ||
            form_url_decode(c,
                            equals < end ? body + equals + 1 : "",
                            equals < end ? end - equals - 1 : 0,
                            &value, 0) != JS_OK ||
            js_formdata_append(c, form, name, value,
                               js_undefined()) != JS_OK)
            return JS_THROW;
        start = end + (end < body_length);
    }
    *r = form;
    return JS_OK;
}

static int ascii_name_equal(const char *value, unsigned long length,
                            const char *expected)
{
    unsigned long i;

    if (strlen(expected) != length)
        return 0;
    for (i = 0; i < length; i++)
        if (ci((unsigned char)value[i]) !=
            ci((unsigned char)expected[i]))
            return 0;
    return 1;
}

static int multipart_boundary(js_ctx *c, const char *content_type,
                              char boundary[71],
                              unsigned long *boundary_length)
{
    const char *cursor;

    cursor = strchr(content_type ? content_type : "", ';');
    while (cursor) {
        const char *name, *end;
        unsigned long name_length, length = 0;

        cursor++;
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        name = cursor;
        while (*cursor && *cursor != '=' && *cursor != ';' &&
               *cursor != ' ' && *cursor != '\t')
            cursor++;
        name_length = (unsigned long)(cursor - name);
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        if (*cursor != '=') {
            cursor = strchr(cursor, ';');
            continue;
        }
        cursor++;
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        if (*cursor == '"') {
            cursor++;
            while (*cursor && *cursor != '"') {
                if (*cursor == '\\' && cursor[1])
                    cursor++;
                if (length < 70)
                    boundary[length] = *cursor;
                length++;
                cursor++;
            }
            if (*cursor != '"')
                return js_throw_error(c, JS_ERR_TYPE,
                                      "unterminated multipart boundary");
            cursor++;
        } else {
            end = cursor;
            while (*end && *end != ';')
                end++;
            while (end > cursor &&
                   (end[-1] == ' ' || end[-1] == '\t'))
                end--;
            length = (unsigned long)(end - cursor);
            if (length <= 70)
                memcpy(boundary, cursor, length);
            cursor = end;
        }
        if (ascii_name_equal(name, name_length, "boundary")) {
            unsigned long i;

            if (!length || length > 70)
                return js_throw_error(c, JS_ERR_TYPE,
                                      "invalid multipart boundary length");
            for (i = 0; i < length; i++)
                if ((unsigned char)boundary[i] < 0x21 ||
                    (unsigned char)boundary[i] > 0x7e)
                    return js_throw_error(c, JS_ERR_TYPE,
                                          "invalid multipart boundary");
            boundary[length] = 0;
            *boundary_length = length;
            return JS_OK;
        }
        cursor = strchr(cursor, ';');
    }
    return js_throw_error(c, JS_ERR_TYPE,
                          "multipart boundary is missing");
}

static int multipart_parameter(js_ctx *c, const char *header,
                               const char *wanted, char *output,
                               unsigned long capacity, int *present)
{
    const char *cursor = strchr(header, ';');

    *present = 0;
    while (cursor) {
        const char *name, *value, *end;
        unsigned long name_length, write = 0;

        cursor++;
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        name = cursor;
        while (*cursor && *cursor != '=' && *cursor != ';' &&
               *cursor != ' ' && *cursor != '\t')
            cursor++;
        name_length = (unsigned long)(cursor - name);
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        if (*cursor != '=') {
            cursor = strchr(cursor, ';');
            continue;
        }
        cursor++;
        while (*cursor == ' ' || *cursor == '\t')
            cursor++;
        value = cursor;
        if (*cursor == '"') {
            value = ++cursor;
            while (*cursor && *cursor != '"') {
                char ch = *cursor++;

                if (ch == '\\' && *cursor)
                    ch = *cursor++;
                if (!ch || ch == '\r' || ch == '\n')
                    return js_throw_error(c, JS_ERR_TYPE,
                                          "invalid multipart parameter");
                if (write + 1 < capacity)
                    output[write] = ch;
                write++;
            }
            if (*cursor != '"')
                return js_throw_error(c, JS_ERR_TYPE,
                                      "unterminated multipart parameter");
            cursor++;
        } else {
            end = cursor;
            while (*end && *end != ';')
                end++;
            while (end > cursor &&
                   (end[-1] == ' ' || end[-1] == '\t'))
                end--;
            write = (unsigned long)(end - value);
            if (write + 1 < capacity)
                memcpy(output, value, write);
            cursor = end;
        }
        if (ascii_name_equal(name, name_length, wanted)) {
            if (write >= capacity)
                return js_throw_error(c, JS_ERR_RANGE,
                                      "multipart parameter is too long");
            output[write] = 0;
            *present = 1;
            return JS_OK;
        }
        cursor = strchr(cursor, ';');
    }
    return JS_OK;
}

static int multipart_file(js_ctx *c, const char *data,
                          unsigned long length, const char *name,
                          const char *type, js_value *r)
{
    js_value constructor, args[3], bytes;
    js_object *parts, *options;

    parts = js_new_array(c);
    options = js_new_object(c);
    bytes = js_uint8array_new(c, data, length);
    if (!parts || !options || !js_is_uint8array(bytes) ||
        js_array_push(c, parts, bytes) != JS_OK ||
        js_set(c, js_object_value(options), "type",
               js_mkcstring(c, type ? type : "")) != JS_OK ||
        js_get(c, js_global(c), "File", &constructor) != JS_OK)
        return JS_THROW;
    args[0] = js_object_value(parts);
    args[1] = js_mkcstring(c, name);
    args[2] = js_object_value(options);
    if (js_fatal(c))
        return JS_THROW;
    return js_construct(c, constructor, 3, args, r);
}

static int multipart_header_copy(js_ctx *c,
                                 const char *start, const char *end,
                                 char *output, unsigned long capacity)
{
    unsigned long length, i;

    while (start < end && (*start == ' ' || *start == '\t'))
        start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
        end--;
    length = (unsigned long)(end - start);
    if (length >= capacity)
        return js_throw_error(c, JS_ERR_RANGE,
                              "multipart header is too long");
    for (i = 0; i < length; i++)
        if (!start[i] || start[i] == '\r' || start[i] == '\n')
            return js_throw_error(c, JS_ERR_TYPE,
                                  "invalid multipart header value");
    if (length)
        memcpy(output, start, length);
    output[length] = 0;
    return JS_OK;
}

static int formdata_from_multipart(js_ctx *c, struct jsdom *j,
                                   const char *content_type,
                                   const char *body,
                                   unsigned long body_length,
                                   js_value *r)
{
    js_value form;
    char boundary[71];
    unsigned long boundary_length = 0, cursor = 0;
    (void)j;

    if (multipart_boundary(c, content_type, boundary,
                           &boundary_length) != JS_OK)
        return JS_THROW;
    form = js_formdata_new(c);
    if (!js_is_formdata(form))
        return JS_THROW;
    for (;;) {
        unsigned long headers_start, headers_end, content_start;
        unsigned long delimiter, line;
        char disposition[1024], part_type[128];
        char name[256], filename[256];
        int have_disposition = 0, have_name = 0, have_filename = 0;
        js_value name_value, value;

        if (cursor + 2 + boundary_length > body_length ||
            body[cursor] != '-' || body[cursor + 1] != '-' ||
            memcmp(body + cursor + 2, boundary, boundary_length)) {
            js_throw_error(c, JS_ERR_TYPE,
                           "invalid multipart delimiter");
            goto fail;
        }
        cursor += 2 + boundary_length;
        if (cursor + 2 <= body_length &&
            body[cursor] == '-' && body[cursor + 1] == '-') {
            cursor += 2;
            break;
        }
        if (cursor + 2 > body_length ||
            body[cursor] != '\r' || body[cursor + 1] != '\n') {
            js_throw_error(c, JS_ERR_TYPE,
                           "invalid multipart delimiter ending");
            goto fail;
        }
        cursor += 2;
        headers_start = cursor;
        headers_end = body_length;
        for (line = cursor; line + 3 < body_length; line++)
            if (!memcmp(body + line, "\r\n\r\n", 4)) {
                headers_end = line;
                break;
            }
        if (headers_end == body_length ||
            headers_end - headers_start > JSDOM_REQUEST_HEADERS_MAX) {
            js_throw_error(c, JS_ERR_TYPE,
                           "invalid multipart headers");
            goto fail;
        }
        disposition[0] = 0;
        part_type[0] = 0;
        line = headers_start;
        while (line < headers_end) {
            unsigned long line_end = line, colon;

            while (line_end < headers_end &&
                   !(line_end + 1 < headers_end &&
                     body[line_end] == '\r' &&
                     body[line_end + 1] == '\n'))
                line_end++;
            colon = line;
            while (colon < line_end && body[colon] != ':')
                colon++;
            if (colon == line_end) {
                js_throw_error(c, JS_ERR_TYPE,
                               "invalid multipart header");
                goto fail;
            }
            if (ascii_name_equal(body + line, colon - line,
                                 "content-disposition")) {
                if (multipart_header_copy(c, body + colon + 1,
                                          body + line_end, disposition,
                                          sizeof(disposition)) != JS_OK)
                    goto fail;
                have_disposition = 1;
            } else if (ascii_name_equal(body + line, colon - line,
                                        "content-type")) {
                if (multipart_header_copy(c, body + colon + 1,
                                          body + line_end, part_type,
                                          sizeof(part_type)) != JS_OK)
                    goto fail;
            }
            line = line_end + (line_end < headers_end ? 2 : 0);
        }
        if (!have_disposition ||
            !mime_type_is(disposition, "form-data") ||
            multipart_parameter(c, disposition, "name", name,
                                sizeof(name), &have_name) != JS_OK ||
            multipart_parameter(c, disposition, "filename", filename,
                                sizeof(filename), &have_filename) != JS_OK)
            goto fail;
        if (!have_name) {
            js_throw_error(c, JS_ERR_TYPE,
                           "multipart field name is missing");
            goto fail;
        }
        content_start = headers_end + 4;
        delimiter = body_length;
        for (line = content_start;
             line + 4 + boundary_length <= body_length; line++) {
            unsigned long after;

            if (body[line] != '\r' || body[line + 1] != '\n' ||
                body[line + 2] != '-' || body[line + 3] != '-' ||
                memcmp(body + line + 4, boundary, boundary_length))
                continue;
            after = line + 4 + boundary_length;
            if (after + 2 <= body_length &&
                ((body[after] == '-' && body[after + 1] == '-') ||
                 (body[after] == '\r' && body[after + 1] == '\n'))) {
                delimiter = line;
                break;
            }
        }
        if (delimiter == body_length) {
            js_throw_error(c, JS_ERR_TYPE,
                           "multipart closing delimiter is missing");
            goto fail;
        }
        name_value = js_mkcstring(c, name);
        if (js_fatal(c))
            goto fail;
        if (have_filename) {
            if (multipart_file(c, body + content_start,
                               delimiter - content_start, filename,
                               part_type, &value) != JS_OK)
                goto fail;
        } else {
            value = js_mkstring(c, body + content_start,
                                delimiter - content_start);
            if (js_fatal(c))
                goto fail;
        }
        if (js_formdata_append(c, form, name_value, value,
                               js_undefined()) != JS_OK)
            goto fail;
        cursor = delimiter + 2;
    }
    *r = form;
    return JS_OK;

fail:
    return JS_THROW;
}

static int body_formdata(js_ctx *c, struct jsdom *j,
                         struct headers_state *headers,
                         const char *body, unsigned long body_length,
                         js_value *r)
{
    const char *content_type = headers_value(headers, "content-type");

    if (mime_type_is(content_type,
                     "application/x-www-form-urlencoded"))
        return formdata_from_urlencoded(c, j, body ? body : "",
                                        body_length, r);
    if (mime_type_is(content_type, "multipart/form-data"))
        return formdata_from_multipart(c, j, content_type,
                                       body ? body : "", body_length, r);
    return js_throw_error(c, JS_ERR_TYPE,
                          "body is not form data");
}

static int response_blob(js_ctx *c, js_value t, int ac,
                         js_value *av, js_value *r)
{
    struct hostref *response_host = host(t, HOST_RESPONSE);
    struct response_state *response;
    js_value blob;
    (void)ac; (void)av;

    if (!response_host)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Response receiver");
    response = (struct response_state *)response_host->data;
    if (response->body_used)
        return js_throw_error(c, JS_ERR_TYPE,
                              "Response body has already been consumed");
    response->body_used = 1;
    blob = js_blob_new(c, response->body, response->body_len,
                       headers_value(response->headers, "content-type"));
    if (!js_is_blob(blob))
        return js_fatal(c) ? JS_THROW
                           : js_throw_error(c, JS_ERR_ERROR, "out of memory");
    return resolved_promise(c, blob, 0, r);
}

static int response_bytes(js_ctx *c, js_value t, int ac,
                          js_value *av, js_value *r)
{
    struct response_state *response =
        response_state_of(t, HOST_RESPONSE);
    js_value bytes;
    (void)ac; (void)av;

    if (!response)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Response receiver");
    if (response->body_used)
        return js_throw_error(c, JS_ERR_TYPE,
                              "Response body has already been consumed");
    response->body_used = 1;
    bytes = js_uint8array_new(c, response->body, response->body_len);
    if (!js_is_uint8array(bytes))
        return JS_THROW;
    return resolved_promise(c, bytes, 0, r);
}

static int response_formdata(js_ctx *c, js_value t, int ac,
                             js_value *av, js_value *r)
{
    struct hostref *response_host = host(t, HOST_RESPONSE);
    struct response_state *response;
    js_value form, reason;
    (void)ac; (void)av;

    if (!response_host)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Response receiver");
    response = (struct response_state *)response_host->data;
    if (response->body_used)
        return js_throw_error(c, JS_ERR_TYPE,
                              "Response body has already been consumed");
    response->body_used = 1;
    if (body_formdata(c, response_host->j, response->headers,
                      response->body, response->body_len,
                      &form) == JS_OK)
        return resolved_promise(c, form, 0, r);
    reason = js_exception(c);
    if (js_fatal(c))
        return JS_THROW;
    js_clear_exception(c);
    return resolved_promise(c, reason, 1, r);
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
    struct headers_state *headers;
    js_object *o;

    headers = headers_state_new();
    if (!headers)
        return js_undefined();
    if (f->content_type &&
        headers_store(j->ctx, headers, "content-type",
                      f->content_type, 0) != JS_OK) {
        headers_release(headers);
        return js_undefined();
    }
    s = (struct response_state *)calloc(1, sizeof(*s));
    if (!s) {
        headers_release(headers);
        return js_undefined();
    }
    s->refs = 1;
    s->status = f->status;
    s->status_text = f->status_text;
    s->url = f->url;
    s->headers = headers;
    s->body = f->body;
    s->body_len = f->body_len;
    s->kind = RESPONSE_BASIC;
    s->redirected = f->redirected;
    free(f->content_type);
    f->content_type = 0;
    memset(f, 0, sizeof(*f));
    o = new_host_data(j, 0, HOST_RESPONSE, s, j->response_proto);
    if (!o) {
        response_release(s);
        return js_undefined();
    }
    return js_object_value(o);
}

static struct request_state *request_state_of(js_value value)
{
    struct hostref *h = host(value, HOST_REQUEST);

    return h ? (struct request_state *)h->data : 0;
}

static int request_method_set(js_ctx *c, struct request_state *request,
                              js_value value)
{
    js_value string_value;
    const char *bytes;
    unsigned long length, i;
    char *method;

    if (js_to_string(c, value, &string_value) != JS_OK)
        return JS_THROW;
    bytes = js_string_bytes(string_value, &length);
    if (!length || length > 15)
        return js_throw_error(c, JS_ERR_TYPE, "invalid request method");
    method = dup_n(bytes, length);
    if (!method)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    for (i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)method[i];
        int token = (ch >= 'a' && ch <= 'z') ||
                    (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') ||
                    ch == '!' || ch == '#' || ch == '$' || ch == '%' ||
                    ch == '&' || ch == '\'' || ch == '*' || ch == '+' ||
                    ch == '-' || ch == '.' || ch == '^' || ch == '_' ||
                    ch == '`' || ch == '|' || ch == '~';

        if (!token) {
            free(method);
            return js_throw_error(c, JS_ERR_TYPE,
                                  "invalid request method");
        }
        if (ch >= 'a' && ch <= 'z')
            method[i] = (char)(ch - ('a' - 'A'));
    }
    if (!strcmp(method, "CONNECT") || !strcmp(method, "TRACE") ||
        !strcmp(method, "TRACK")) {
        free(method);
        return js_throw_error(c, JS_ERR_TYPE,
                              "forbidden request method");
    }
    free(request->method);
    request->method = method;
    return JS_OK;
}

static int request_body_set(js_ctx *c, struct request_state *request,
                            js_value value)
{
    js_value string_value;
    const void *bytes = 0;
    const char *blob_type = 0;
    unsigned long length = 0;
    char *copy;
    int is_text = 0, is_blob = 0;

    free(request->body);
    request->body = 0;
    request->body_len = 0;
    if (js_is_undefined(value) || js_is_null(value))
        return JS_OK;
    if (js_is_formdata(value)) {
        char generated_content_type[128];

        if (formdata_encode(c, value, &request->body,
                            &request->body_len,
                            generated_content_type) != JS_OK)
            return JS_THROW;
        if (!headers_contains_name(request->headers, "content-type") &&
            headers_store(c, request->headers, "content-type",
                          generated_content_type, 0) != JS_OK)
            return JS_THROW;
        return JS_OK;
    }
    if (js_is_urlsearchparams(value)) {
        if (js_urlsearchparams_string(c, value, &string_value) != JS_OK)
            return JS_THROW;
        bytes = js_string_bytes(string_value, &length);
    } else if (jsdom_blob_info(value, &bytes, &length, &blob_type, 0)) {
        is_blob = 1;
    } else if ((bytes = js_buffersource_data(value, &length)) != 0) {
    } else {
        if (js_to_string(c, value, &string_value) != JS_OK)
            return JS_THROW;
        bytes = js_string_bytes(string_value, &length);
        is_text = 1;
    }
    if (length > JSDOM_RESPONSE_BODY_MAX)
        return js_throw_error(c, JS_ERR_RANGE, "Request body is too large");
    copy = dup_n(bytes ? (const char *)bytes : "", length);
    if (!copy)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    request->body = copy;
    request->body_len = length;
    if (js_is_urlsearchparams(value) &&
        !headers_contains_name(request->headers, "content-type") &&
        headers_store(c, request->headers, "content-type",
                      "application/x-www-form-urlencoded;charset=UTF-8",
                      0) != JS_OK)
        return JS_THROW;
    if (is_blob && blob_type && blob_type[0] &&
        !headers_contains_name(request->headers, "content-type") &&
        headers_store(c, request->headers, "content-type",
                      blob_type, 0) != JS_OK)
        return JS_THROW;
    if (is_text &&
        !headers_contains_name(request->headers, "content-type") &&
        headers_store(c, request->headers, "content-type",
                      "text/plain;charset=UTF-8", 0) != JS_OK)
        return JS_THROW;
    return JS_OK;
}

static void request_metadata_defaults(struct request_state *request)
{
    strcpy(request->cache, "default");
    strcpy(request->credentials, "same-origin");
    request->destination[0] = 0;
    request->integrity[0] = 0;
    strcpy(request->mode, "cors");
    strcpy(request->redirect, "follow");
    strcpy(request->referrer, "about:client");
    request->referrer_policy[0] = 0;
}

static void request_metadata_copy(struct request_state *request,
                                  const struct request_state *source)
{
    strcpy(request->cache, source->cache);
    strcpy(request->credentials, source->credentials);
    strcpy(request->destination, source->destination);
    strcpy(request->integrity, source->integrity);
    strcpy(request->mode, source->mode);
    strcpy(request->redirect, source->redirect);
    strcpy(request->referrer, source->referrer);
    strcpy(request->referrer_policy, source->referrer_policy);
    request->keepalive = source->keepalive;
}

static int request_enum_option(js_ctx *c, js_value options,
                               const char *name, char *destination,
                               unsigned long capacity,
                               const char *const *allowed,
                               unsigned long allowed_count)
{
    js_value value, string_value;
    const char *bytes;
    unsigned long length, i;

    if (js_get(c, options, name, &value) != JS_OK)
        return JS_THROW;
    if (js_is_undefined(value))
        return JS_OK;
    if (js_to_string(c, value, &string_value) != JS_OK)
        return JS_THROW;
    bytes = js_string_bytes(string_value, &length);
    for (i = 0; i < allowed_count; i++)
        if (strlen(allowed[i]) == length &&
            !memcmp(bytes, allowed[i], length)) {
            if (length >= capacity)
                return js_throw_error(c, JS_ERR_RANGE,
                                      "Request option is too long");
            memcpy(destination, bytes, length);
            destination[length] = 0;
            return JS_OK;
        }
    return js_throw_error(c, JS_ERR_TYPE,
                          "invalid Request option value");
}

static int request_text_option(js_ctx *c, js_value options,
                               const char *name, char *destination,
                               unsigned long capacity)
{
    js_value value, string_value;
    const char *bytes;
    unsigned long length, i;

    if (js_get(c, options, name, &value) != JS_OK)
        return JS_THROW;
    if (js_is_undefined(value))
        return JS_OK;
    if (js_to_string(c, value, &string_value) != JS_OK)
        return JS_THROW;
    bytes = js_string_bytes(string_value, &length);
    if (length >= capacity)
        return js_throw_error(c, JS_ERR_RANGE,
                              "Request option is too long");
    for (i = 0; i < length; i++)
        if (!bytes[i])
            return js_throw_error(c, JS_ERR_TYPE,
                                  "Request option contains NUL");
    memcpy(destination, bytes, length);
    destination[length] = 0;
    return JS_OK;
}

static int request_metadata_options(js_ctx *c, struct jsdom *j,
                                    struct request_state *request,
                                    js_value options)
{
    static const char *const cache_values[] = {
        "default", "no-store", "reload", "no-cache",
        "force-cache", "only-if-cached"
    };
    static const char *const credential_values[] = {
        "omit", "same-origin", "include"
    };
    static const char *const mode_values[] = {
        "same-origin", "cors", "no-cors"
    };
    static const char *const redirect_values[] = {
        "follow", "error", "manual"
    };
    static const char *const policy_values[] = {
        "", "no-referrer", "no-referrer-when-downgrade", "origin",
        "origin-when-cross-origin", "same-origin", "strict-origin",
        "strict-origin-when-cross-origin", "unsafe-url"
    };
    js_value value, string_value;
    const char *referrer;
    char resolved[URL_MAX];

    if (request_enum_option(c, options, "cache", request->cache,
                            sizeof(request->cache), cache_values,
                            sizeof(cache_values) /
                                sizeof(cache_values[0])) != JS_OK ||
        request_enum_option(c, options, "credentials",
                            request->credentials,
                            sizeof(request->credentials), credential_values,
                            sizeof(credential_values) /
                                sizeof(credential_values[0])) != JS_OK ||
        request_enum_option(c, options, "mode", request->mode,
                            sizeof(request->mode), mode_values,
                            sizeof(mode_values) /
                                sizeof(mode_values[0])) != JS_OK ||
        request_enum_option(c, options, "redirect", request->redirect,
                            sizeof(request->redirect), redirect_values,
                            sizeof(redirect_values) /
                                sizeof(redirect_values[0])) != JS_OK ||
        request_enum_option(c, options, "referrerPolicy",
                            request->referrer_policy,
                            sizeof(request->referrer_policy), policy_values,
                            sizeof(policy_values) /
                                sizeof(policy_values[0])) != JS_OK ||
        request_text_option(c, options, "integrity",
                            request->integrity,
                            sizeof(request->integrity)) != JS_OK)
        return JS_THROW;
    if (js_get(c, options, "keepalive", &value) != JS_OK)
        return JS_THROW;
    if (!js_is_undefined(value))
        request->keepalive = js_to_boolean(value);
    if (js_get(c, options, "referrer", &value) != JS_OK)
        return JS_THROW;
    if (!js_is_undefined(value)) {
        if (js_to_string(c, value, &string_value) != JS_OK)
            return JS_THROW;
        referrer = js_string_bytes(string_value, 0);
        if (!*referrer || !strcmp(referrer, "about:client")) {
            if (strlen(referrer) >= sizeof(request->referrer))
                return js_throw_error(c, JS_ERR_RANGE,
                                      "Request referrer is too long");
            strcpy(request->referrer, referrer);
        } else {
            if (url_resolve_str(j->base_url, referrer, resolved,
                                sizeof(resolved)) != URL_OK)
                return js_throw_error(c, JS_ERR_TYPE,
                                      "invalid Request referrer");
            strcpy(request->referrer, resolved);
        }
    }
    if (!strcmp(request->cache, "only-if-cached") &&
        strcmp(request->mode, "same-origin"))
        return js_throw_error(c, JS_ERR_TYPE,
                              "only-if-cached requires same-origin mode");
    return JS_OK;
}

static int request_state_init(js_ctx *c, struct jsdom *j,
                              int ac, js_value *av,
                              struct request_state **out)
{
    struct request_state *source;
    struct request_state *request;
    js_value input_string, value;
    const char *input;
    char absolute[URL_MAX];

    *out = 0;
    if (ac < 1)
        return js_throw_error(c, JS_ERR_TYPE, "Request requires an input");
    request = (struct request_state *)calloc(1, sizeof(*request));
    if (!request)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    request->signal = js_undefined();
    request_metadata_defaults(request);
    source = request_state_of(av[0]);
    if (source) {
        if (source->body_used) {
            js_throw_error(c, JS_ERR_TYPE,
                           "used Request body cannot be copied");
            goto fail;
        }
        request->url = dup_z(source->url);
        request->method = dup_z(source->method);
        request->headers = headers_clone_state(c, source->headers);
        request->body = source->body
            ? dup_n(source->body, source->body_len) : 0;
        request->body_len = source->body_len;
        request->signal = source->signal;
        request_metadata_copy(request, source);
        if (!request->url || !request->method || !request->headers ||
            (source->body && !request->body)) {
            if (!js_fatal(c))
                js_throw_error(c, JS_ERR_ERROR, "out of memory");
            goto fail;
        }
    } else {
        if (js_to_string(c, av[0], &input_string) != JS_OK)
            goto fail;
        input = js_string_bytes(input_string, 0);
        if (url_resolve_str(j->base_url, input, absolute,
                            sizeof(absolute)) != URL_OK) {
            js_throw_error(c, JS_ERR_TYPE, "invalid Request URL");
            goto fail;
        }
        request->url = dup_z(absolute);
        request->method = dup_z("GET");
        request->headers = headers_state_new();
        if (!request->url || !request->method || !request->headers) {
            js_throw_error(c, JS_ERR_ERROR, "out of memory");
            goto fail;
        }
    }
    if (ac > 1 && js_is_object(av[1])) {
        if (request_metadata_options(c, j, request, av[1]) != JS_OK)
            goto fail;
        if (js_get(c, av[1], "method", &value) != JS_OK)
            goto fail;
        if (!js_is_undefined(value) &&
            request_method_set(c, request, value) != JS_OK)
            goto fail;
        if (js_get(c, av[1], "headers", &value) != JS_OK)
            goto fail;
        if (!js_is_undefined(value)) {
            struct headers_state *headers = headers_state_new();

            if (!headers) {
                js_throw_error(c, JS_ERR_ERROR, "out of memory");
                goto fail;
            }
            if (headers_fill(c, headers, value) != JS_OK) {
                headers_release(headers);
                goto fail;
            }
            headers_release(request->headers);
            request->headers = headers;
        }
        if (js_get(c, av[1], "body", &value) != JS_OK)
            goto fail;
        if (!js_is_undefined(value) &&
            request_body_set(c, request, value) != JS_OK)
            goto fail;
        if (js_get(c, av[1], "signal", &value) != JS_OK)
            goto fail;
        if (!js_is_undefined(value) && !js_is_null(value)) {
            if (!js_is_abortsignal(value)) {
                js_throw_error(c, JS_ERR_TYPE,
                               "Request signal is not an AbortSignal");
                goto fail;
            }
            request->signal = value;
        }
    }
    if (!js_is_object(request->signal)) {
        js_value sig = js_abortsignal_new(c);

        if (!js_is_abortsignal(sig))
            goto fail;
        request->signal = sig;
    }
    if (request->body &&
        (!strcmp(request->method, "GET") ||
         !strcmp(request->method, "HEAD"))) {
        js_throw_error(c, JS_ERR_TYPE,
                       "GET and HEAD requests cannot have a body");
        goto fail;
    }
    if (!strcmp(request->mode, "no-cors") &&
        strcmp(request->method, "GET") &&
        strcmp(request->method, "HEAD") &&
        strcmp(request->method, "POST")) {
        js_throw_error(c, JS_ERR_TYPE,
                       "no-cors mode requires a simple method");
        goto fail;
    }
    *out = request;
    return JS_OK;

fail:
    request_release(request);
    return JS_THROW;
}

static int request_constructor(js_ctx *c, js_value t,
                               int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    struct request_state *request;
    js_object *object;
    (void)t;

    if (!j || !js_is_constructing(c))
        return js_throw_error(c, JS_ERR_TYPE,
                              "Request must be constructed with new");
    if (request_state_init(c, j, ac, av, &request) != JS_OK)
        return JS_THROW;
    object = new_host_data(j, 0, HOST_REQUEST, request, j->request_proto);
    if (!object) {
        request_release(request);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    *r = js_object_value(object);
    return JS_OK;
}

static int request_string_get(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r, int field)
{
    struct request_state *request = request_state_of(t);
    const char *text;
    (void)ac; (void)av;

    if (!request)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Request receiver");
    text = field ? request->method : request->url;
    *r = js_mkcstring(c, text);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int request_url_get(js_ctx *c, js_value t,
                           int ac, js_value *av, js_value *r)
{
    return request_string_get(c, t, ac, av, r, 0);
}

static int request_method_get(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    return request_string_get(c, t, ac, av, r, 1);
}

static int request_metadata_get(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r,
                                int field)
{
    struct request_state *request = request_state_of(t);
    const char *value;
    (void)ac; (void)av;

    if (!request)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Request receiver");
    switch (field) {
    case 0: value = request->cache; break;
    case 1: value = request->credentials; break;
    case 2: value = request->destination; break;
    case 3: value = request->integrity; break;
    case 4: value = request->mode; break;
    case 5: value = request->redirect; break;
    case 6: value = request->referrer; break;
    default: value = request->referrer_policy; break;
    }
    *r = js_mkcstring(c, value);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

#define REQUEST_METADATA_GETTER(name, field)                              \
    static int name(js_ctx *c, js_value t, int ac, js_value *av,          \
                    js_value *r)                                           \
    {                                                                      \
        return request_metadata_get(c, t, ac, av, r, field);              \
    }
REQUEST_METADATA_GETTER(request_cache_get, 0)
REQUEST_METADATA_GETTER(request_credentials_get, 1)
REQUEST_METADATA_GETTER(request_destination_get, 2)
REQUEST_METADATA_GETTER(request_integrity_get, 3)
REQUEST_METADATA_GETTER(request_mode_get, 4)
REQUEST_METADATA_GETTER(request_redirect_get, 5)
REQUEST_METADATA_GETTER(request_referrer_get, 6)
REQUEST_METADATA_GETTER(request_referrer_policy_get, 7)
#undef REQUEST_METADATA_GETTER

static int request_keepalive_get(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct request_state *request = request_state_of(t);
    (void)ac; (void)av;

    if (!request)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Request receiver");
    *r = js_bool(request->keepalive);
    return JS_OK;
}

static int request_navigation_get(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    (void)ac; (void)av;
    if (!request_state_of(t))
        return js_throw_error(c, JS_ERR_TYPE, "invalid Request receiver");
    *r = js_bool(0);
    return JS_OK;
}

static int request_headers_get(js_ctx *c, js_value t,
                               int ac, js_value *av, js_value *r)
{
    struct hostref *request_host = host(t, HOST_REQUEST);
    struct request_state *request;
    js_object *object;
    (void)ac; (void)av;

    if (!request_host)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Request receiver");
    request = (struct request_state *)request_host->data;
    request->headers->refs++;
    object = new_host_data(request_host->j, 0, HOST_HEADERS,
                           request->headers,
                           request_host->j->headers_proto);
    if (!object) {
        headers_release(request->headers);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    *r = js_object_value(object);
    return JS_OK;
}

static int request_signal_get(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    struct request_state *request = request_state_of(t);
    (void)ac; (void)av;

    if (!request)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Request receiver");
    *r = request->signal;
    return JS_OK;
}

static int request_body_used_get(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct request_state *request = request_state_of(t);
    (void)ac; (void)av;

    if (!request)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Request receiver");
    *r = js_bool(request->body_used);
    return JS_OK;
}

static int request_clone(js_ctx *c, js_value t,
                         int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    struct request_state *copy;
    js_object *object;
    js_value input = t;
    (void)ac; (void)av;

    if (!request_state_of(t))
        return js_throw_error(c, JS_ERR_TYPE, "invalid Request receiver");
    if (request_state_init(c, j, 1, &input, &copy) != JS_OK)
        return JS_THROW;
    object = new_host_data(j, 0, HOST_REQUEST, copy, j->request_proto);
    if (!object) {
        request_release(copy);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    *r = js_object_value(object);
    return JS_OK;
}

static int request_text(js_ctx *c, js_value t,
                        int ac, js_value *av, js_value *r)
{
    struct request_state *request = request_state_of(t);
    js_value text;
    (void)ac; (void)av;

    if (!request)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Request receiver");
    if (request->body_used)
        return js_throw_error(c, JS_ERR_TYPE,
                              "Request body has already been consumed");
    request->body_used = 1;
    text = js_mkstring(c, request->body ? request->body : "",
                       request->body_len);
    if (js_fatal(c))
        return JS_THROW;
    return resolved_promise(c, text, 0, r);
}

static int request_arraybuffer(js_ctx *c, js_value t,
                               int ac, js_value *av, js_value *r)
{
    struct request_state *request = request_state_of(t);
    js_value buffer;
    (void)ac; (void)av;

    if (!request)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Request receiver");
    if (request->body_used)
        return js_throw_error(c, JS_ERR_TYPE,
                              "Request body has already been consumed");
    request->body_used = 1;
    buffer = js_arraybuffer_new(c, request->body, request->body_len);
    if (!js_is_arraybuffer(buffer))
        return JS_THROW;
    return resolved_promise(c, buffer, 0, r);
}

static int request_json(js_ctx *c, js_value t,
                        int ac, js_value *av, js_value *r)
{
    struct request_state *request = request_state_of(t);
    js_value json, parse, body, parsed, reason;
    (void)ac; (void)av;

    if (!request)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Request receiver");
    if (request->body_used)
        return js_throw_error(c, JS_ERR_TYPE,
                              "Request body has already been consumed");
    request->body_used = 1;
    body = js_mkstring(c, request->body ? request->body : "",
                       request->body_len);
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

static int request_blob(js_ctx *c, js_value t,
                        int ac, js_value *av, js_value *r)
{
    struct hostref *request_host = host(t, HOST_REQUEST);
    struct request_state *request;
    js_value blob;
    (void)ac; (void)av;

    if (!request_host)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Request receiver");
    request = (struct request_state *)request_host->data;
    if (request->body_used)
        return js_throw_error(c, JS_ERR_TYPE,
                              "Request body has already been consumed");
    request->body_used = 1;
    blob = js_blob_new(c, request->body, request->body_len,
                       headers_value(request->headers, "content-type"));
    if (!js_is_blob(blob))
        return js_fatal(c) ? JS_THROW
                           : js_throw_error(c, JS_ERR_ERROR, "out of memory");
    return resolved_promise(c, blob, 0, r);
}

static int request_bytes(js_ctx *c, js_value t,
                         int ac, js_value *av, js_value *r)
{
    struct request_state *request = request_state_of(t);
    js_value bytes;
    (void)ac; (void)av;

    if (!request)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Request receiver");
    if (request->body_used)
        return js_throw_error(c, JS_ERR_TYPE,
                              "Request body has already been consumed");
    request->body_used = 1;
    bytes = js_uint8array_new(c, request->body, request->body_len);
    if (!js_is_uint8array(bytes))
        return JS_THROW;
    return resolved_promise(c, bytes, 0, r);
}

static int request_formdata(js_ctx *c, js_value t,
                            int ac, js_value *av, js_value *r)
{
    struct hostref *request_host = host(t, HOST_REQUEST);
    struct request_state *request;
    js_value form, reason;
    (void)ac; (void)av;

    if (!request_host)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Request receiver");
    request = (struct request_state *)request_host->data;
    if (request->body_used)
        return js_throw_error(c, JS_ERR_TYPE,
                              "Request body has already been consumed");
    request->body_used = 1;
    if (body_formdata(c, request_host->j, request->headers,
                      request->body, request->body_len,
                      &form) == JS_OK)
        return resolved_promise(c, form, 0, r);
    reason = js_exception(c);
    if (js_fatal(c))
        return JS_THROW;
    js_clear_exception(c);
    return resolved_promise(c, reason, 1, r);
}

static int global_fetch(js_ctx *c, js_value t, int ac, js_value *av,
                        js_value *r)
{
    struct jsdom *j = binding(c);
    struct request_state *source_request;
    const char *input;
    char absolute[URL_MAX];
    char *method = 0, *body = 0;
    char *content_type = 0, *accept = 0, *extra_headers = 0;
    const char *body_content_type = 0;
    char generated_body_content_type[128];
    unsigned long body_len = 0;
    struct headers_state *request_headers;
    struct request_state policy;
    js_value abort_signal = js_undefined();
    int slot = -1, i;
    js_value promise;
    (void)t;

    if (!j || ac < 1)
        return js_throw_error(c, JS_ERR_TYPE, "fetch requires an input");
    memset(&policy, 0, sizeof(policy));
    request_metadata_defaults(&policy);
    source_request = request_state_of(av[0]);
    if (source_request) {
        if (source_request->body_used)
            return js_throw_error(c, JS_ERR_TYPE,
                                  "used Request body cannot be fetched");
        if (strlen(source_request->url) >= sizeof(absolute))
            return js_throw_error(c, JS_ERR_RANGE,
                                  "fetch URL is too long");
        strcpy(absolute, source_request->url);
        method = dup_z(source_request->method);
        body = source_request->body
            ? dup_n(source_request->body, source_request->body_len) : 0;
        body_len = source_request->body_len;
        request_headers = headers_clone_state(c, source_request->headers);
        abort_signal = source_request->signal;
        request_metadata_copy(&policy, source_request);
        if (!method || !request_headers ||
            (source_request->body && !body)) {
            headers_release(request_headers);
            free(body);
            free(method);
            if (!js_fatal(c))
                return js_throw_error(c, JS_ERR_ERROR, "out of memory");
            return JS_THROW;
        }
    } else {
        if (string_arg(c, ac, av, 0, &input) != JS_OK)
            return JS_THROW;
        if (url_resolve_str(j->base_url, input, absolute,
                            sizeof(absolute)) != URL_OK)
            return js_throw_error(c, JS_ERR_TYPE, "invalid fetch URL");
        method = dup_z("GET");
        if (!method)
            return js_throw_error(c, JS_ERR_ERROR, "out of memory");
        request_headers = headers_state_new();
        if (!request_headers) {
            free(method);
            return js_throw_error(c, JS_ERR_ERROR, "out of memory");
        }
    }
    if (ac > 1 && js_is_object(av[1])) {
        js_value v, sv;

        if (js_get(c, av[1], "method", &v) != JS_OK) {
            headers_release(request_headers);
            free(method);
            return JS_THROW;
        }
        if (!js_is_undefined(v)) {
            const char *s;
            unsigned long n;

            if (js_to_string(c, v, &sv) != JS_OK) {
                headers_release(request_headers);
                free(method);
                return JS_THROW;
            }
            s = js_string_bytes(sv, &n);
            if (n == 0 || n > 15) {
                headers_release(request_headers);
                free(method);
                return js_throw_error(c, JS_ERR_TYPE,
                                      "invalid fetch method");
            }
            free(method);
            method = dup_n(s, n);
            if (!method) {
                headers_release(request_headers);
                return js_throw_error(c, JS_ERR_ERROR, "out of memory");
            }
            for (i = 0; method[i]; i++)
                if (method[i] >= 'a' && method[i] <= 'z')
                    method[i] = (char)(method[i] - ('a' - 'A'));
        }
        if (js_get(c, av[1], "body", &v) != JS_OK) {
            headers_release(request_headers);
            free(method);
            return JS_THROW;
        }
        if (!js_is_undefined(v)) {
            const void *bytes = 0;
            const char *btype = 0;

            free(body);
            body = 0;
            body_len = 0;
            body_content_type = 0;
            if (js_is_null(v))
                goto body_done;
            if (js_is_formdata(v)) {
                if (formdata_encode(c, v, &body, &body_len,
                                    generated_body_content_type) != JS_OK) {
                    headers_release(request_headers);
                    free(method);
                    return JS_THROW;
                }
                body_content_type = generated_body_content_type;
                goto body_done;
            } else if (js_is_urlsearchparams(v)) {
                if (js_urlsearchparams_string(c, v, &sv) != JS_OK) {
                    headers_release(request_headers);
                    free(method);
                    return JS_THROW;
                }
                bytes = js_string_bytes(sv, &body_len);
                body_content_type =
                    "application/x-www-form-urlencoded;charset=UTF-8";
            } else if (jsdom_blob_info(v, &bytes, &body_len, &btype, 0)) {
                if (btype && btype[0])
                    body_content_type = btype;
            } else if ((bytes = js_buffersource_data(v, &body_len)) != 0) {
            } else if (js_to_string(c, v, &sv) == JS_OK) {
                bytes = js_string_bytes(sv, &body_len);
                body_content_type = "text/plain;charset=UTF-8";
            } else {
                headers_release(request_headers);
                free(method);
                return JS_THROW;
            }
            if (body_len > JSDOM_RESPONSE_BODY_MAX) {
                headers_release(request_headers);
                free(method);
                return js_throw_error(c, JS_ERR_RANGE,
                                      "fetch body is too large");
            }
            body = dup_n(bytes ? (const char *)bytes : "", body_len);
            if (!body) {
                headers_release(request_headers);
                free(method);
                return js_throw_error(c, JS_ERR_ERROR, "out of memory");
            }
        }
body_done:
        if (js_get(c, av[1], "signal", &v) != JS_OK) {
            headers_release(request_headers);
            free(body);
            free(method);
            return JS_THROW;
        }
        if (!js_is_undefined(v) && !js_is_null(v)) {
            if (!js_is_abortsignal(v)) {
                headers_release(request_headers);
                free(body);
                free(method);
                return js_throw_error(c, JS_ERR_TYPE,
                                      "fetch signal is not an AbortSignal");
            }
            abort_signal = v;
        }
        if (js_get(c, av[1], "headers", &v) != JS_OK) {
            headers_release(request_headers);
            free(body);
            free(method);
            return JS_THROW;
        }
        if (!js_is_undefined(v)) {
            struct headers_state *replacement = headers_state_new();

            if (!replacement ||
                headers_fill(c, replacement, v) != JS_OK) {
                headers_release(replacement);
                headers_release(request_headers);
                free(body);
                free(method);
                if (!replacement && !js_fatal(c))
                    return js_throw_error(c, JS_ERR_ERROR,
                                          "out of memory");
                return JS_THROW;
            }
            headers_release(request_headers);
            request_headers = replacement;
        }
    }
    if (ac > 1 && js_is_object(av[1]) &&
        request_metadata_options(c, j, &policy, av[1]) != JS_OK) {
        headers_release(request_headers);
        free(body);
        free(method);
        return JS_THROW;
    }
    if (policy.keepalive && body_len > 65536UL) {
        headers_release(request_headers);
        free(body);
        free(method);
        return js_throw_error(c, JS_ERR_RANGE,
                              "keepalive body exceeds 64 KiB");
    }
    if (!strcmp(policy.mode, "no-cors") &&
        strcmp(method, "GET") && strcmp(method, "HEAD") &&
        strcmp(method, "POST")) {
        headers_release(request_headers);
        free(body);
        free(method);
        return js_throw_error(c, JS_ERR_TYPE,
                              "no-cors mode requires a simple method");
    }
    if (body && (!strcmp(method, "GET") || !strcmp(method, "HEAD"))) {
        headers_release(request_headers);
        free(body);
        free(method);
        return js_throw_error(c, JS_ERR_TYPE,
                              "GET and HEAD requests cannot have a body");
    }
    if (body && body_content_type &&
        !headers_contains_name(request_headers, "content-type") &&
        headers_store(c, request_headers, "content-type",
                      body_content_type, 0) != JS_OK) {
        headers_release(request_headers);
        free(body);
        free(method);
        return JS_THROW;
    }
    if (headers_request_fields(c, request_headers, &content_type,
                               &accept, &extra_headers) != JS_OK) {
        headers_release(request_headers);
        free(content_type);
        free(accept);
        free(extra_headers);
        free(body);
        free(method);
        return JS_THROW;
    }
    headers_release(request_headers);
    if (js_abortsignal_aborted(abort_signal)) {
        promise = js_promise_new(c);
        free(content_type);
        free(accept);
        free(extra_headers);
        free(body);
        free(method);
        if (!js_is_promise(promise))
            return JS_THROW;
        *r = promise;
        return js_promise_reject(c, promise, js_abortsignal_reason(abort_signal));
    }
    for (i = 0; i < JSDOM_FETCH_MAX; i++)
        if (j->fetches[i].state == TASK_FREE) {
            slot = i;
            break;
        }
    if (slot < 0) {
        free(content_type);
        free(accept);
        free(extra_headers);
        free(body);
        free(method);
        return js_throw_error(c, JS_ERR_RANGE,
                              "too many pending fetch requests");
    }
    promise = js_promise_new(c);
    if (!js_is_promise(promise)) {
        free(content_type);
        free(accept);
        free(extra_headers);
        free(body);
        free(method);
        return JS_THROW;
    }
    j->fetches[slot].url = dup_z(absolute);
    if (!j->fetches[slot].url) {
        free(content_type);
        free(accept);
        free(extra_headers);
        free(body);
        free(method);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    j->fetches[slot].promise = promise;
    j->fetches[slot].method = method;
    j->fetches[slot].body = body;
    j->fetches[slot].body_len = body_len;
    j->fetches[slot].content_type = content_type;
    j->fetches[slot].accept = accept;
    j->fetches[slot].extra_headers = extra_headers;
    strcpy(j->fetches[slot].mode, policy.mode);
    strcpy(j->fetches[slot].credentials, policy.credentials);
    strcpy(j->fetches[slot].redirect, policy.redirect);
    j->fetches[slot].abort_signal = abort_signal;
    j->fetches[slot].state = TASK_QUEUED;
    if (source_request && source_request->body)
        source_request->body_used = 1;
    *r = promise;
    return JS_OK;
}

static int navigator_send_beacon(js_ctx *c, js_value t, int ac,
                                 js_value *av, js_value *r)
{
    js_object *options;
    js_value args[2], ignored;
    int status;
    (void)t;

    if (ac < 1)
        return js_throw_error(c, JS_ERR_TYPE,
                              "sendBeacon requires a URL");
    options = js_new_object(c);
    if (!options)
        return JS_THROW;
    if (js_set(c, js_object_value(options), "method",
               js_mkcstring(c, "POST")) != JS_OK ||
        js_set(c, js_object_value(options), "keepalive",
               js_bool(1)) != JS_OK ||
        js_set(c, js_object_value(options), "credentials",
               js_mkcstring(c, "include")) != JS_OK ||
        js_set(c, js_object_value(options), "mode",
               js_mkcstring(c, "no-cors")) != JS_OK)
        return JS_THROW;
    if (ac > 1 &&
        js_set(c, js_object_value(options), "body", av[1]) != JS_OK)
        return JS_THROW;
    args[0] = av[0];
    args[1] = js_object_value(options);
    status = global_fetch(c, js_undefined(), 2, args, &ignored);
    if (status != JS_OK) {
        if (js_fatal(c))
            return JS_THROW;
        js_clear_exception(c);
        *r = js_bool(0);
        return JS_OK;
    }
    *r = js_bool(1);
    return JS_OK;
}

/* ------------------------------------------------------------------ */
/* Web Storage                                                        */

static struct web_storage *storage_of(js_value value, struct jsdom **owner)
{
    struct hostref *h = host(value, HOST_STORAGE);

    if (!h)
        return 0;
    if (owner)
        *owner = h->j;
    return (struct web_storage *)h->data;
}

static int storage_length_get(js_ctx *c, js_value t, int ac, js_value *av,
                              js_value *r)
{
    struct jsdom *j;
    struct web_storage *s = storage_of(t, &j);
    (void)c; (void)ac; (void)av;

    if (!s)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Storage receiver");
    *r = js_number((double)web_storage_length(s, j->origin));
    return JS_OK;
}

static int storage_key_method(js_ctx *c, js_value t, int ac, js_value *av,
                              js_value *r)
{
    struct jsdom *j;
    struct web_storage *s = storage_of(t, &j);
    uint32_t index = 0;
    const char *key;

    if (!s)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Storage receiver");
    if (ac > 0 && js_to_uint32(c, av[0], &index) != JS_OK)
        return JS_THROW;
    key = web_storage_key(s, j->origin, index);
    *r = key ? js_mkcstring(c, key) : js_null();
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int storage_get_item(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    struct jsdom *j;
    struct web_storage *s = storage_of(t, &j);
    const char *key, *value;

    if (!s)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Storage receiver");
    if (string_arg(c, ac, av, 0, &key) != JS_OK)
        return JS_THROW;
    value = web_storage_get(s, j->origin, key);
    *r = value ? js_mkcstring(c, value) : js_null();
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int storage_set_item(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    struct jsdom *j;
    struct web_storage *s = storage_of(t, &j);
    const char *key, *value;
    char *key_copy;
    int rc;

    if (!s)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Storage receiver");
    if (string_arg(c, ac, av, 0, &key) != JS_OK)
        return JS_THROW;
    /* ToString(value) can reuse the interpreter scratch buffer, so preserve
     * the first converted argument before converting the second one. */
    key_copy = dup_z(key);
    if (!key_copy)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    if (string_arg(c, ac, av, 1, &value) != JS_OK) {
        free(key_copy);
        return JS_THROW;
    }
    rc = web_storage_set(s, j->origin, key_copy, value);
    free(key_copy);
    if (rc == WEB_STORAGE_QUOTA || rc == WEB_STORAGE_FULL)
        return js_throw_error(c, JS_ERR_RANGE, "Storage quota exceeded");
    if (rc != WEB_STORAGE_OK)
        return js_throw_error(c, JS_ERR_ERROR, "%s",
                              web_storage_error(rc));
    *r = js_undefined();
    return JS_OK;
}

static int storage_remove_item(js_ctx *c, js_value t, int ac, js_value *av,
                               js_value *r)
{
    struct jsdom *j;
    struct web_storage *s = storage_of(t, &j);
    const char *key;

    if (!s)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Storage receiver");
    if (string_arg(c, ac, av, 0, &key) != JS_OK)
        return JS_THROW;
    web_storage_remove(s, j->origin, key);
    *r = js_undefined();
    return JS_OK;
}

static int storage_clear_method(js_ctx *c, js_value t, int ac, js_value *av,
                                js_value *r)
{
    struct jsdom *j;
    struct web_storage *s = storage_of(t, &j);
    (void)ac; (void)av;

    if (!s)
        return js_throw_error(c, JS_ERR_TYPE, "invalid Storage receiver");
    web_storage_clear(s, j->origin);
    *r = js_undefined();
    return JS_OK;
}

/* ------------------------------------------------------------------ */
/* Construction and public operations */

static void js_print(void *user, const char *text)
{
    struct jsdom *j = (struct jsdom *)user;
    if (j->print) j->print(j->print_user, text);
}

static int dom_exception_code(const char *name)
{
    static const struct {
        const char *name;
        int code;
    } codes[] = {
        { "IndexSizeError", 1 },
        { "HierarchyRequestError", 3 },
        { "WrongDocumentError", 4 },
        { "InvalidCharacterError", 5 },
        { "NoModificationAllowedError", 7 },
        { "NotFoundError", 8 },
        { "NotSupportedError", 9 },
        { "InUseAttributeError", 10 },
        { "InvalidStateError", 11 },
        { "SyntaxError", 12 },
        { "InvalidModificationError", 13 },
        { "NamespaceError", 14 },
        { "InvalidAccessError", 15 },
        { "TypeMismatchError", 17 },
        { "SecurityError", 18 },
        { "NetworkError", 19 },
        { "AbortError", 20 },
        { "URLMismatchError", 21 },
        { "QuotaExceededError", 22 },
        { "TimeoutError", 23 },
        { "InvalidNodeTypeError", 24 },
        { "DataCloneError", 25 }
    };
    unsigned int i;

    for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
        if (!strcmp(name, codes[i].name))
            return codes[i].code;
    return 0;
}

static int dom_exception_make(js_ctx *c, const char *message,
                              const char *name, js_value *out)
{
    struct jsdom *j = binding(c);
    js_object *object;
    js_value value;

    if (!j || !j->dom_exception_proto)
        return JS_THROW;
    object = js_new_object_proto(c, j->dom_exception_proto);
    if (!object)
        return JS_THROW;
    value = js_object_value(object);
    if (js_set(c, value, "message",
               js_mkcstring(c, message ? message : "")) != JS_OK ||
        js_set(c, value, "name",
               js_mkcstring(c, name ? name : "Error")) != JS_OK ||
        js_set(c, value, "code",
               js_number(dom_exception_code(name ? name : "Error"))) !=
            JS_OK)
        return JS_THROW;
    *out = value;
    return JS_OK;
}

static int throw_dom_exception(js_ctx *c, const char *name,
                               const char *message)
{
    js_value exception;

    if (dom_exception_make(c, message, name, &exception) != JS_OK)
        return JS_THROW;
    return js_throw(c, exception);
}

static int dom_exception_constructor(js_ctx *c, js_value t,
                                     int ac, js_value *av, js_value *r)
{
    const char *message = "", *name = "Error";
    (void)t;

    if (!js_is_constructing(c))
        return js_throw_error(c, JS_ERR_TYPE,
                              "DOMException requires new");
    if ((ac > 0 && string_arg(c, ac, av, 0, &message) != JS_OK) ||
        (ac > 1 && string_arg(c, ac, av, 1, &name) != JS_OK))
        return JS_THROW;
    return dom_exception_make(c, message, name, r);
}

static int dom_exception_to_string(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    js_value name_value, message_value, name_string, message_string;
    const char *name, *message;
    unsigned long name_length, message_length;
    char *output;
    (void)ac; (void)av;

    if (!js_is_object(t) ||
        js_get(c, t, "name", &name_value) != JS_OK ||
        js_get(c, t, "message", &message_value) != JS_OK ||
        js_to_string(c, name_value, &name_string) != JS_OK ||
        js_to_string(c, message_value, &message_string) != JS_OK)
        return JS_THROW;
    name = js_string_bytes(name_string, &name_length);
    message = js_string_bytes(message_string, &message_length);
    if (!name_length) {
        *r = message_string;
        return JS_OK;
    }
    if (!message_length) {
        *r = name_string;
        return JS_OK;
    }
    output = (char *)malloc(name_length + message_length + 3);
    if (!output)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    memcpy(output, name, name_length);
    output[name_length] = ':';
    output[name_length + 1] = ' ';
    memcpy(output + name_length + 2, message, message_length);
    output[name_length + message_length + 2] = 0;
    *r = js_mkstring(c, output, name_length + message_length + 2);
    free(output);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

/* ------------------------------------------------------------------ */
/* Self-contained math, color & bitmap helpers (freestanding safe)    */

#define JSDOM_PI                3.14159265358979323846
#define JSDOM_PI2               6.28318530717958647693

static int jsd_isnan(double x) { return x != x; }
static int jsd_isfinite(double x) { return !jsd_isnan(x) && !jsd_isnan(x - x); }
static double jsd_fabs(double x) { return x < 0.0 ? -x : x; }

static double jsd_floor(double x)
{
    long long i;

    if (!jsd_isfinite(x) || jsd_fabs(x) >= 4503599627370496.0)
        return x;
    i = (long long)x;
    if (x < (double)i)
        i--;
    return (double)i;
}

static double jsd_ceil(double x)
{
    return -jsd_floor(-x);
}

static double jsd_sqrt(double x)
{
    double y;
    int scale = 0, i;

    if (x <= 0.0 || !jsd_isfinite(x))
        return x == 0.0 ? 0.0 : 0.0;
    while (x >= 2.0) { x *= 0.25; scale++; }
    while (x < 0.5) { x *= 4.0; scale--; }
    y = 0.5 * (1.0 + x);
    for (i = 0; i < 6; i++)
        y = 0.5 * (y + x / y);
    while (scale > 0) { y *= 2.0; scale--; }
    while (scale < 0) { y *= 0.5; scale++; }
    return y;
}

static double jsd_sin(double x)
{
    double k, x2, num, res, sign = 1.0;

    if (!jsd_isfinite(x))
        return 0.0;
    k = jsd_floor((x + JSDOM_PI) / JSDOM_PI2);
    x -= k * JSDOM_PI2;
    if (x < 0.0) { x = -x; sign = -1.0; }
    if (x > JSDOM_PI * 0.5)
        x = JSDOM_PI - x;
    if (x > JSDOM_PI * 0.25) {
        double c = JSDOM_PI * 0.5 - x;
        double c2 = c * c;
        res = 1.0 - c2 * (0.5 - c2 * (1.0 / 24.0 - c2 * (1.0 / 720.0 - c2 / 40320.0)));
        return sign * res;
    }
    x2 = x * x;
    num = x * x2;
    res = x - num / 6.0;
    num *= x2; res += num / 120.0;
    num *= x2; res -= num / 5040.0;
    num *= x2; res += num / 362880.0;
    num *= x2; res -= num / 39916800.0;
    return sign * res;
}

static double jsd_cos(double x)
{
    return jsd_sin(x + JSDOM_PI * 0.5);
}

static int jsd_parse_double(const char *s, double *out, const char **endp)
{
    int neg = 0, digits = 0;
    double v = 0.0, frac = 0.1;

    if (!s)
        return 0;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    if (*s == '+' || *s == '-') {
        neg = (*s == '-');
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10.0 + (double)(*s - '0');
        s++;
        digits = 1;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            v += (double)(*s - '0') * frac;
            frac *= 0.1;
            s++;
            digits = 1;
        }
    }
    if (!digits)
        return 0;
    if (neg) v = -v;
    *out = v;
    if (endp) *endp = s;
    return 1;
}

static int jsd_clamp_u8(double x)
{
    if (x <= 0.0) return 0;
    if (x >= 255.0) return 255;
    return (int)(x + 0.5);
}

static int jsdom_parse_color(const char *s, uint32_t *out)
{
    char buf[128];
    unsigned long len, start = 0, end;
    const char *p;
    double c[4];
    int pct[4];
    int n = 0;

    if (!s || !out)
        return 0;
    while (s[start] && (unsigned char)s[start] <= ' ')
        start++;
    len = strlen(s + start);
    while (len > 0 && (unsigned char)s[start + len - 1] <= ' ')
        len--;
    if (!len || len >= sizeof(buf))
        return 0;
    memcpy(buf, s + start, len);
    buf[len] = 0;
    if (css_parse_color_string(buf, out))
        return 1;
    (void)end;
    if (ci_eq_n(buf, 4, "rgb(", 4) || ci_eq_n(buf, 5, "rgba(", 5)) {
        p = strchr(buf, '(') + 1;
        while (*p && n < 4) {
            const char *np;

            while (*p == ' ' || *p == '\t' || *p == ',' || *p == '/')
                p++;
            if (*p == ')')
                break;
            if (!jsd_parse_double(p, &c[n], &np))
                return 0;
            pct[n] = (*np == '%');
            if (pct[n]) np++;
            p = np;
            n++;
        }
        while (*p == ' ' || *p == '\t') p++;
        if (*p != ')' || (n != 3 && n != 4))
            return 0;
        {
            uint32_t r = (uint32_t)jsd_clamp_u8(pct[0] ? c[0] * 2.55 : c[0]);
            uint32_t g = (uint32_t)jsd_clamp_u8(pct[1] ? c[1] * 2.55 : c[1]);
            uint32_t b = (uint32_t)jsd_clamp_u8(pct[2] ? c[2] * 2.55 : c[2]);
            uint32_t a = n == 4
                ? (uint32_t)jsd_clamp_u8(pct[3] ? c[3] * 2.55 : c[3] * 255.0)
                : 255u;

            *out = (a << 24) | (r << 16) | (g << 8) | b;
            return 1;
        }
    }
    return 0;
}

static void canvas_fmt_color(uint32_t col, char *out, unsigned long outsz)
{
    uint32_t a = CSS_COLOR_A(col);
    uint32_t r = CSS_COLOR_R(col);
    uint32_t g = CSS_COLOR_G(col);
    uint32_t b = CSS_COLOR_B(col);

    if (a == 255) {
        snprintf(out, outsz, "#%02x%02x%02x",
                 (unsigned)r, (unsigned)g, (unsigned)b);
    } else {
        char abuf[16];
        uint32_t ath = (a * 1000u + 127u) / 255u;

        computed_fmt_thousandths(abuf, sizeof(abuf), (int32_t)ath, "");
        snprintf(out, outsz, "rgba(%u, %u, %u, %s)",
                 (unsigned)r, (unsigned)g, (unsigned)b, abuf);
    }
}

/* 5x7 ASCII glyph table (' ' .. '~', LSB = top row, 5 columns per char) */
static const uint8_t jsdom_font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5f,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7f,0x14,0x7f,0x14},{0x24,0x2a,0x7f,0x2a,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1c,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1c,0x00},{0x14,0x08,0x3e,0x08,0x14},{0x08,0x08,0x3e,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},{0x18,0x14,0x12,0x7f,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e},{0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3e},
    {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a},{0x7f,0x08,0x08,0x08,0x7f},{0x00,0x41,0x7f,0x41,0x00},
    {0x20,0x40,0x41,0x3f,0x01},{0x7f,0x08,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,0x09,0x09,0x09,0x06},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7f,0x01,0x01},{0x3f,0x40,0x40,0x40,0x3f},
    {0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7f,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7f,0x00},{0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40},{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
    {0x7f,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7f},
    {0x38,0x54,0x54,0x54,0x18},{0x08,0x7e,0x09,0x01,0x02},{0x0c,0x52,0x52,0x52,0x3e},
    {0x7f,0x08,0x04,0x04,0x78},{0x00,0x44,0x7d,0x40,0x00},{0x20,0x40,0x44,0x3d,0x00},
    {0x7f,0x10,0x28,0x44,0x00},{0x00,0x41,0x7f,0x40,0x00},{0x7c,0x04,0x18,0x04,0x78},
    {0x7c,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7c,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7c},{0x7c,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3f,0x44,0x40,0x20},{0x3c,0x40,0x40,0x20,0x7c},{0x1c,0x20,0x40,0x20,0x1c},
    {0x3c,0x40,0x30,0x40,0x3c},{0x44,0x28,0x10,0x28,0x44},{0x0c,0x50,0x50,0x50,0x3c},
    {0x44,0x64,0x54,0x4c,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7f,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00},{0x08,0x04,0x08,0x10,0x08}
};

/* ------------------------------------------------------------------ */
/* HTMLTemplateElement                                                */

static int template_content_get(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    struct dom_node *frag;
    (void)ac; (void)av;

    if (!h || h->node->type != DOM_ELEMENT ||
        h->node->tag_id != HTAG_TEMPLATE ||
        h->node->namespace_id != DOM_NS_HTML)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal HTMLTemplateElement receiver");
    frag = template_content_node(h->j, h->node);
    if (!frag)
        return js_throw_error(c, JS_ERR_ERROR,
                              "cannot access template content");
    *r = wrap_node(h->j, frag);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

/* ------------------------------------------------------------------ */
/* Canvas 2D Backing Store, Rasterizer, ImageData & PNG Export        */

static unsigned int canvas_parse_dim_attr(const char *s, unsigned int def)
{
    unsigned long v = 0;
    int digits = 0;

    if (!s || !*s)
        return def;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    if (*s == '+')
        s++;
    while (*s >= '0' && *s <= '9') {
        digits = 1;
        if (v <= JSDOM_CANVAS_MAX_DIM)
            v = v * 10ul + (unsigned long)(*s - '0');
        s++;
    }
    if (!digits)
        return def;
    if (v > JSDOM_CANVAS_MAX_DIM)
        v = JSDOM_CANVAS_MAX_DIM;
    return (unsigned int)v;
}

static void canvas_2d_reset_state(struct canvas_2d_state *st)
{
    if (!st)
        return;
    canvas_2d_release_grads(st);
    st->sp = 0;
    st->path_n = 0;
    memset(&st->cur, 0, sizeof(st->cur));
    st->cur.m[0] = 1.0; st->cur.m[1] = 0.0;
    st->cur.m[2] = 0.0; st->cur.m[3] = 1.0;
    st->cur.m[4] = 0.0; st->cur.m[5] = 0.0;
    st->cur.fill_color = 0xFF000000u;
    st->cur.stroke_color = 0xFF000000u;
    strcpy(st->cur.fill_str, "#000000");
    strcpy(st->cur.stroke_str, "#000000");
    st->cur.fill_grad_val = js_undefined();
    st->cur.stroke_grad_val = js_undefined();
    st->cur.line_width = 1.0;
    st->cur.global_alpha = 1.0;
    strcpy(st->cur.font, "10px sans-serif");
    st->cur.font_px = 10.0;
    st->cur.text_align = 0;
    st->cur.text_baseline = 0;
}

static struct dom_canvas *canvas_ensure_buffer(struct jsdom *j,
                                               struct dom_node *node,
                                               int force_resize,
                                               unsigned int req_w,
                                               unsigned int req_h)
{
    unsigned int w, h;
    struct dom_canvas *cv;

    if (!node || node->type != DOM_ELEMENT || node->tag_id != HTAG_CANVAS)
        return 0;
    if (!force_resize && node->canvas)
        return node->canvas;
    if (!force_resize) {
        w = canvas_parse_dim_attr(dom_get_attr(node, "width"), 300u);
        h = canvas_parse_dim_attr(dom_get_attr(node, "height"), 150u);
    } else {
        w = req_w;
        h = req_h;
    }
    if (w > JSDOM_CANVAS_MAX_DIM) w = JSDOM_CANVAS_MAX_DIM;
    if (h > JSDOM_CANVAS_MAX_DIM) h = JSDOM_CANVAS_MAX_DIM;
    if (w > 0 && h > JSDOM_CANVAS_MAX_PIXELS / w)
        h = JSDOM_CANVAS_MAX_PIXELS / w;
    cv = node->canvas;
    if (!cv) {
        cv = (struct dom_canvas *)calloc(1, sizeof(*cv));
        if (!cv)
            return 0;
        node->canvas = cv;
    } else {
        free(cv->px);
        free(cv->alpha);
        cv->px = 0;
        cv->alpha = 0;
    }
    cv->w = w;
    cv->h = h;
    if (w > 0 && h > 0) {
        unsigned long total = (unsigned long)w * (unsigned long)h;

        cv->px = (uint32_t *)calloc(total, sizeof(uint32_t));
        cv->alpha = (uint8_t *)calloc(total, 1);
        if (!cv->px || !cv->alpha) {
            free(cv->px);
            free(cv->alpha);
            cv->px = 0;
            cv->alpha = 0;
            cv->w = 0;
            cv->h = 0;
            return 0;
        }
    }
    if (force_resize && node->user && j) {
        js_value ctx2d = js_undefined();

        if (js_get(j->ctx, js_object_value((js_object *)node->user),
                   "__ctx2d", &ctx2d) == JS_OK &&
            js_is_object(ctx2d)) {
            struct hostref *ch = host(ctx2d, HOST_CANVAS_2D);

            if (ch && ch->data)
                canvas_2d_reset_state((struct canvas_2d_state *)ch->data);
        }
    }
    mark_dom_dirty(j);
    return cv;
}

static void canvas_sync_attr_resize(struct jsdom *j, struct dom_node *node)
{
    unsigned int w, h;

    if (!node || node->tag_id != HTAG_CANVAS || !node->canvas)
        return;
    w = canvas_parse_dim_attr(dom_get_attr(node, "width"), 300u);
    h = canvas_parse_dim_attr(dom_get_attr(node, "height"), 150u);
    (void)canvas_ensure_buffer(j, node, 1, w, h);
}

static unsigned int canvas_current_dim(struct dom_node *node, int is_height)
{
    if (node && node->canvas)
        return is_height ? node->canvas->h : node->canvas->w;
    return canvas_parse_dim_attr(
        dom_get_attr(node, is_height ? "height" : "width"),
        is_height ? 150u : 300u);
}

static int canvas_width_get(js_ctx *c, js_value t,
                            int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    (void)c; (void)ac; (void)av;

    if (!h || h->node->type != DOM_ELEMENT || h->node->tag_id != HTAG_CANVAS) {
        *r = js_undefined();
        return JS_OK;
    }
    *r = js_number((double)canvas_current_dim(h->node, 0));
    return JS_OK;
}

static int canvas_height_get(js_ctx *c, js_value t,
                             int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    (void)c; (void)ac; (void)av;

    if (!h || h->node->type != DOM_ELEMENT || h->node->tag_id != HTAG_CANVAS) {
        *r = js_undefined();
        return JS_OK;
    }
    *r = js_number((double)canvas_current_dim(h->node, 1));
    return JS_OK;
}

static int canvas_dim_set(js_ctx *c, js_value t, int ac, js_value *av,
                          js_value *r, int is_height)
{
    struct hostref *h = host(t, HOST_NODE);
    double dv;
    unsigned int val, other, new_w, new_h;
    const char *attr_name = is_height ? "height" : "width";
    const char *old;
    char buf[24];

    if (!h || h->node->type != DOM_ELEMENT || h->node->tag_id != HTAG_CANVAS)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal HTMLCanvasElement receiver");
    if (js_to_number(c, ac ? av[0] : js_undefined(), &dv) != JS_OK)
        return JS_THROW;
    if (!jsd_isfinite(dv) || dv < 0.0)
        val = is_height ? 150u : 300u;
    else if (dv > (double)JSDOM_CANVAS_MAX_DIM)
        val = JSDOM_CANVAS_MAX_DIM;
    else
        val = (unsigned int)dv;
    other = canvas_current_dim(h->node, !is_height);
    new_w = is_height ? other : val;
    new_h = is_height ? val : other;
    snprintf(buf, sizeof(buf), "%u", val);
    old = dom_get_attr(h->node, attr_name);
    if (!dom_set_attr(h->node, attr_name, buf))
        return js_throw_error(c, JS_ERR_ERROR, "cannot set canvas dimension");
    if (mutation_attribute(h->j, h->node, attr_name, old) != JS_OK)
        return JS_THROW;
    if (!canvas_ensure_buffer(h->j, h->node, 1, new_w, new_h))
        return js_throw_error(c, JS_ERR_ERROR, "canvas allocation failed");
    *r = js_undefined();
    return JS_OK;
}

static int canvas_width_set(js_ctx *c, js_value t,
                            int ac, js_value *av, js_value *r)
{
    return canvas_dim_set(c, t, ac, av, r, 0);
}

static int canvas_height_set(js_ctx *c, js_value t,
                             int ac, js_value *av, js_value *r)
{
    return canvas_dim_set(c, t, ac, av, r, 1);
}

static int canvas_get_context(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    const char *kind;
    js_value ctx_val = js_undefined();
    struct canvas_2d_state *st;
    js_object *obj;

    if (!h || h->node->type != DOM_ELEMENT || h->node->tag_id != HTAG_CANVAS)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal HTMLCanvasElement receiver");
    if (string_arg(c, ac, av, 0, &kind) != JS_OK)
        return JS_THROW;
    if (strcmp(kind, "2d") != 0) {
        *r = js_null();
        return JS_OK;
    }
    if (!canvas_ensure_buffer(h->j, h->node, 0, 0, 0))
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    if (js_get(c, t, "__ctx2d", &ctx_val) != JS_OK)
        return JS_THROW;
    if (host(ctx_val, HOST_CANVAS_2D)) {
        *r = ctx_val;
        return JS_OK;
    }
    st = (struct canvas_2d_state *)calloc(1, sizeof(*st));
    if (!st)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    canvas_2d_reset_state(st);
    st->canvas_obj = t;
    obj = new_host_data(h->j, h->node, HOST_CANVAS_2D, st, h->j->canvas_2d_proto);
    if (!obj) {
        free(st);
        return JS_THROW;
    }
    ctx_val = js_object_value(obj);
    if (js_define(c, obj, "__canvas", t) != JS_OK ||
        js_define(c, t.u.obj, "__ctx2d", ctx_val) != JS_OK)
        return JS_THROW;
    *r = ctx_val;
    return JS_OK;
}

static uint32_t png_crc32_update(uint32_t crc, const uint8_t *buf, unsigned long len)
{
    unsigned long i;
    int k;

    crc = ~crc;
    for (i = 0; i < len; i++) {
        crc ^= buf[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return ~crc;
}

static void png_put_be32(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)((v >> 24) & 0xFFu);
    dst[1] = (uint8_t)((v >> 16) & 0xFFu);
    dst[2] = (uint8_t)((v >> 8) & 0xFFu);
    dst[3] = (uint8_t)(v & 0xFFu);
}

static void png_write_chunk(uint8_t *out, unsigned long *used,
                            const char tag[4], const uint8_t *data, uint32_t len)
{
    uint32_t crc;

    png_put_be32(out + *used, len);
    memcpy(out + *used + 4, tag, 4);
    if (len && data)
        memcpy(out + *used + 8, data, len);
    crc = png_crc32_update(0u, out + *used + 4, 4ul + len);
    png_put_be32(out + *used + 8 + len, crc);
    *used += 12ul + len;
}

static char *jsd_base64_encode(const uint8_t *src, unsigned long len,
                               unsigned long *out_len)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out;
    unsigned long i, used = 0, need;

    need = ((len + 2ul) / 3ul) * 4ul;
    out = (char *)malloc(need + 1);
    if (!out)
        return 0;
    for (i = 0; i < len; i += 3ul) {
        uint32_t a = src[i];
        uint32_t b = (i + 1 < len) ? src[i + 1] : 0u;
        uint32_t c = (i + 2 < len) ? src[i + 2] : 0u;

        out[used++] = tab[(a >> 2) & 0x3Fu];
        out[used++] = tab[((a & 0x03u) << 4) | (b >> 4)];
        out[used++] = (i + 1 < len) ? tab[((b & 0x0Fu) << 2) | (c >> 6)] : '=';
        out[used++] = (i + 2 < len) ? tab[c & 0x3Fu] : '=';
    }
    out[used] = 0;
    if (out_len)
        *out_len = used;
    return out;
}

static int canvas_to_data_url(js_ctx *c, js_value t,
                              int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    struct dom_canvas *cv;
    unsigned long w, hpx, raw_len, nblocks, zlen, png_cap, png_len = 0;
    unsigned long pos = 0, b64_len = 0, y, x;
    uint8_t *raw, *png, *zbuf;
    uint8_t ihdr[13];
    uint32_t s1 = 1u, s2 = 0u;
    char *b64, *url;

    (void)ac; (void)av;
    if (!h || h->node->type != DOM_ELEMENT || h->node->tag_id != HTAG_CANVAS)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal HTMLCanvasElement receiver");
    cv = canvas_ensure_buffer(h->j, h->node, 0, 0, 0);
    if (!cv)
        return js_throw_error(c, JS_ERR_ERROR, "canvas allocation failed");
    if (cv->w == 0 || cv->h == 0) {
        *r = js_mkcstring(c, "data:,");
        return js_fatal(c) ? JS_THROW : JS_OK;
    }
    w = cv->w;
    hpx = cv->h;
    raw_len = (1ul + w * 4ul) * hpx;
    raw = (uint8_t *)malloc(raw_len);
    if (!raw)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    for (y = 0; y < hpx; y++) {
        raw[pos++] = 0u;
        for (x = 0; x < w; x++) {
            unsigned long idx = y * w + x;
            uint32_t px = cv->px[idx];
            uint8_t al = cv->alpha[idx];

            raw[pos++] = (uint8_t)((px >> 16) & 0xFFu);
            raw[pos++] = (uint8_t)((px >> 8) & 0xFFu);
            raw[pos++] = (uint8_t)(px & 0xFFu);
            raw[pos++] = al;
        }
    }
    for (pos = 0; pos < raw_len; pos++) {
        s1 = (s1 + raw[pos]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    nblocks = (raw_len + 65534ul) / 65535ul;
    zlen = 2ul + nblocks * 5ul + raw_len + 4ul;
    png_cap = 8ul + 25ul + (12ul + zlen) + 12ul;
    zbuf = (uint8_t *)malloc(zlen);
    png = (uint8_t *)malloc(png_cap);
    if (!zbuf || !png) {
        free(zbuf); free(png); free(raw);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    pos = 0;
    zbuf[pos++] = 0x78u;
    zbuf[pos++] = 0x01u;
    {
        unsigned long rem = raw_len, off = 0;
        while (rem > 0) {
            unsigned long blk = rem > 65535ul ? 65535ul : rem;
            uint16_t len16 = (uint16_t)blk;
            uint16_t nlen16 = (uint16_t)(~len16);

            zbuf[pos++] = (rem == blk) ? 0x01u : 0x00u;
            zbuf[pos++] = (uint8_t)(len16 & 0xFFu);
            zbuf[pos++] = (uint8_t)(len16 >> 8);
            zbuf[pos++] = (uint8_t)(nlen16 & 0xFFu);
            zbuf[pos++] = (uint8_t)(nlen16 >> 8);
            memcpy(zbuf + pos, raw + off, blk);
            pos += blk;
            off += blk;
            rem -= blk;
        }
    }
    png_put_be32(zbuf + pos, (s2 << 16) | s1);
    free(raw);

    png[0] = 0x89u; png[1] = 0x50u; png[2] = 0x4Eu; png[3] = 0x47u;
    png[4] = 0x0Du; png[5] = 0x0Au; png[6] = 0x1Au; png[7] = 0x0Au;
    png_len = 8ul;
    png_put_be32(ihdr, (uint32_t)w);
    png_put_be32(ihdr + 4, (uint32_t)hpx);
    ihdr[8] = 8u; ihdr[9] = 6u; ihdr[10] = 0u; ihdr[11] = 0u; ihdr[12] = 0u;
    png_write_chunk(png, &png_len, "IHDR", ihdr, 13u);
    png_write_chunk(png, &png_len, "IDAT", zbuf, (uint32_t)zlen);
    png_write_chunk(png, &png_len, "IEND", 0, 0u);
    free(zbuf);

    b64 = jsd_base64_encode(png, png_len, &b64_len);
    free(png);
    if (!b64)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    url = (char *)malloc(22ul + b64_len + 1ul);
    if (!url) {
        free(b64);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    memcpy(url, "data:image/png;base64,", 22ul);
    memcpy(url + 22ul, b64, b64_len + 1ul);
    free(b64);
    *r = js_mkstring(c, url, 22ul + b64_len);
    free(url);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static struct canvas_2d_state *ctx2d_of(js_value t, struct hostref **out_h,
                                        struct dom_canvas **out_cv)
{
    struct hostref *h = host(t, HOST_CANVAS_2D);

    if (!h || !h->data)
        return 0;
    if (out_h) *out_h = h;
    if (out_cv) *out_cv = canvas_ensure_buffer(h->j, h->node, 0, 0, 0);
    return (struct canvas_2d_state *)h->data;
}

static void canvas_xform(const struct canvas_2d_saved *cur,
                         double x, double y, double *ox, double *oy)
{
    *ox = cur->m[0] * x + cur->m[2] * y + cur->m[4];
    *oy = cur->m[1] * x + cur->m[3] * y + cur->m[5];
}

static uint8_t canvas_effective_alpha(uint32_t col, double global_alpha)
{
    double a = ((double)CSS_COLOR_A(col) / 255.0) * global_alpha;
    return (uint8_t)jsd_clamp_u8(a * 255.0);
}

static void canvas_blend_px(struct dom_canvas *cv, int x, int y,
                            uint8_t sr, uint8_t sg, uint8_t sb, uint8_t sa)
{
    unsigned long idx;
    uint32_t da, inv_da, out_a, rr, gg, bb, dp;

    if (!cv || !cv->px || !cv->alpha || sa == 0)
        return;
    if (x < 0 || y < 0 || x >= cv->w || y >= cv->h)
        return;
    idx = (unsigned long)y * (unsigned long)cv->w + (unsigned long)x;
    if (sa == 255) {
        cv->px[idx] = ((uint32_t)sr << 16) | ((uint32_t)sg << 8) | (uint32_t)sb;
        cv->alpha[idx] = 255;
        return;
    }
    da = cv->alpha[idx];
    if (da == 0) {
        cv->px[idx] = ((uint32_t)sr << 16) | ((uint32_t)sg << 8) | (uint32_t)sb;
        cv->alpha[idx] = sa;
        return;
    }
    dp = cv->px[idx];
    inv_da = 255u - sa;
    out_a = (uint32_t)sa + (da * inv_da + 127u) / 255u;
    if (out_a == 0) {
        cv->px[idx] = 0;
        cv->alpha[idx] = 0;
        return;
    }
    {
        uint32_t dr = (dp >> 16) & 0xFFu;
        uint32_t dg = (dp >> 8) & 0xFFu;
        uint32_t db = dp & 0xFFu;
        uint32_t sw = (uint32_t)sa * 255u;
        uint32_t dw = da * inv_da;
        uint32_t denom = out_a * 255u;

        rr = (sr * sw + dr * dw + denom / 2u) / denom;
        gg = (sg * sw + dg * dw + denom / 2u) / denom;
        bb = (sb * sw + db * dw + denom / 2u) / denom;
        if (rr > 255u) rr = 255u;
        if (gg > 255u) gg = 255u;
        if (bb > 255u) bb = 255u;
        cv->px[idx] = (rr << 16) | (gg << 8) | bb;
        cv->alpha[idx] = (uint8_t)out_a;
    }
}

static void canvas_clear_px(struct dom_canvas *cv, int x, int y)
{
    unsigned long idx;

    if (!cv || !cv->px || !cv->alpha)
        return;
    if (x < 0 || y < 0 || x >= cv->w || y >= cv->h)
        return;
    idx = (unsigned long)y * (unsigned long)cv->w + (unsigned long)x;
    cv->px[idx] = 0;
    cv->alpha[idx] = 0;
}

struct canvas_paint {
    int is_clear;
    uint8_t sr, sg, sb, sa;
    const struct canvas_gradient *grad;
    double inv_m[6];
    int inv_ok;
    double global_alpha;
};

static void canvas_paint_init(struct canvas_paint *p,
                              const struct canvas_2d_saved *cur,
                              int is_stroke, int is_clear)
{
    memset(p, 0, sizeof(*p));
    p->is_clear = is_clear;
    if (is_clear) {
        p->sa = 255u;
        return;
    }
    p->global_alpha = cur->global_alpha;
    p->grad = is_stroke ? cur->stroke_grad : cur->fill_grad;
    if (p->grad) {
        double det = cur->m[0] * cur->m[3] - cur->m[1] * cur->m[2];
        if (jsd_fabs(det) > 1e-12) {
            double inv_det = 1.0 / det;
            p->inv_m[0] =  cur->m[3] * inv_det;
            p->inv_m[1] = -cur->m[1] * inv_det;
            p->inv_m[2] = -cur->m[2] * inv_det;
            p->inv_m[3] =  cur->m[0] * inv_det;
            p->inv_m[4] = (cur->m[2] * cur->m[5] - cur->m[3] * cur->m[4]) * inv_det;
            p->inv_m[5] = (cur->m[1] * cur->m[4] - cur->m[0] * cur->m[5]) * inv_det;
            p->inv_ok = 1;
        }
        p->sa = (p->grad->stop_count > 0 && p->global_alpha > 0.0) ? 255u : 0u;
    } else {
        uint32_t col = is_stroke ? cur->stroke_color : cur->fill_color;
        p->sr = (uint8_t)CSS_COLOR_R(col);
        p->sg = (uint8_t)CSS_COLOR_G(col);
        p->sb = (uint8_t)CSS_COLOR_B(col);
        p->sa = canvas_effective_alpha(col, p->global_alpha);
    }
}

static int canvas_eval_gradient(const struct canvas_gradient *g,
                                double lx, double ly, double global_alpha,
                                uint8_t *sr, uint8_t *sg, uint8_t *sb,
                                uint8_t *sa)
{
    double t = 0.0, u = 0.0;
    uint32_t col0, col1;

    if (!g || !g->stop_count || global_alpha <= 0.0)
        return 0;
    if (g->kind == JSDOM_GRADIENT_LINEAR) {
        double dx = g->x1 - g->x0, dy = g->y1 - g->y0;
        double len2 = dx * dx + dy * dy;

        if (len2 <= 1e-18)
            return 0;
        t = ((lx - g->x0) * dx + (ly - g->y0) * dy) / len2;
    } else {
        double dx = g->x1 - g->x0, dy = g->y1 - g->y0, dr = g->r1 - g->r0;
        double ox = lx - g->x0, oy = ly - g->y0;
        double a, hb, cq;

        if (jsd_fabs(dx) < 1e-12 && jsd_fabs(dy) < 1e-12 &&
            jsd_fabs(dr) < 1e-12)
            return 0;
        a = dx * dx + dy * dy - dr * dr;
        hb = -(ox * dx + oy * dy + g->r0 * dr);
        cq = ox * ox + oy * oy - g->r0 * g->r0;
        if (jsd_fabs(a) < 1e-12) {
            if (jsd_fabs(hb) < 1e-12)
                return 0;
            t = -cq / (2.0 * hb);
            if (g->r0 + t * dr < -1e-9)
                return 0;
        } else {
            double disc = hb * hb - a * cq;
            double sq, t1, t2, tmax, tmin;

            if (disc < 0.0)
                return 0;
            sq = jsd_sqrt(disc);
            t1 = (-hb + sq) / a;
            t2 = (-hb - sq) / a;
            tmax = t1 > t2 ? t1 : t2;
            tmin = t1 > t2 ? t2 : t1;
            if (g->r0 + tmax * dr >= -1e-9)
                t = tmax;
            else if (g->r0 + tmin * dr >= -1e-9)
                t = tmin;
            else
                return 0;
        }
    }
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    if (t <= g->stops[0].offset) {
        col0 = col1 = g->stops[0].color;
    } else if (t >= g->stops[g->stop_count - 1].offset) {
        col0 = col1 = g->stops[g->stop_count - 1].color;
    } else {
        unsigned int idx = 0;

        while (idx + 1 < g->stop_count && g->stops[idx + 1].offset <= t)
            idx++;
        if (idx + 1 >= g->stop_count) {
            col0 = col1 = g->stops[g->stop_count - 1].color;
        } else {
            double o0 = g->stops[idx].offset;
            double o1 = g->stops[idx + 1].offset;
            double span = o1 - o0;

            col0 = g->stops[idx].color;
            col1 = g->stops[idx + 1].color;
            u = span > 1e-12 ? (t - o0) / span : 0.0;
        }
    }
    *sr = (uint8_t)jsd_clamp_u8((double)CSS_COLOR_R(col0) +
                                u * ((double)CSS_COLOR_R(col1) -
                                     (double)CSS_COLOR_R(col0)));
    *sg = (uint8_t)jsd_clamp_u8((double)CSS_COLOR_G(col0) +
                                u * ((double)CSS_COLOR_G(col1) -
                                     (double)CSS_COLOR_G(col0)));
    *sb = (uint8_t)jsd_clamp_u8((double)CSS_COLOR_B(col0) +
                                u * ((double)CSS_COLOR_B(col1) -
                                     (double)CSS_COLOR_B(col0)));
    {
        double a0 = (double)CSS_COLOR_A(col0);
        double a1 = (double)CSS_COLOR_A(col1);
        *sa = (uint8_t)jsd_clamp_u8((a0 + u * (a1 - a0)) * global_alpha);
    }
    return *sa != 0;
}

static void canvas_blend_paint_px(struct dom_canvas *cv, int x, int y,
                                  const struct canvas_paint *p)
{
    uint8_t sr, sg, sb, sa;

    if (p->is_clear) {
        canvas_clear_px(cv, x, y);
        return;
    }
    if (!p->grad) {
        canvas_blend_px(cv, x, y, p->sr, p->sg, p->sb, p->sa);
        return;
    }
    if (!p->inv_ok)
        return;
    {
        double px = (double)x + 0.5, py = (double)y + 0.5;
        double lx = p->inv_m[0] * px + p->inv_m[2] * py + p->inv_m[4];
        double ly = p->inv_m[1] * px + p->inv_m[3] * py + p->inv_m[5];

        if (!canvas_eval_gradient(p->grad, lx, ly, p->global_alpha,
                                  &sr, &sg, &sb, &sa))
            return;
        canvas_blend_px(cv, x, y, sr, sg, sb, sa);
    }
}

static int canvas_rasterize_poly(struct dom_canvas *cv,
                                 const struct canvas_pt *pts, unsigned int npts,
                                 const struct canvas_paint *p)
{
    double miny = 1e9, maxy = -1e9;
    unsigned int i, sp_start = 0, edge_count = 0;
    int y0, y1, py;
    struct { double x0, y0, x1, y1; } *edges;
    double xs[512];

    if (!cv || !cv->w || !cv->h || npts < 3 || (!p->is_clear && p->sa == 0))
        return 0;
    edges = malloc(sizeof(*edges) * npts);
    if (!edges)
        return 0;
    for (i = 0; i < npts; i++) {
        if (pts[i].y < miny) miny = pts[i].y;
        if (pts[i].y > maxy) maxy = pts[i].y;
        if (pts[i].kind == 0) {
            sp_start = i;
        } else {
            edges[edge_count].x0 = pts[i - 1].x;
            edges[edge_count].y0 = pts[i - 1].y;
            edges[edge_count].x1 = pts[i].x;
            edges[edge_count].y1 = pts[i].y;
            edge_count++;
        }
        if (i + 1 == npts || pts[i + 1].kind == 0) {
            if (i > sp_start) {
                edges[edge_count].x0 = pts[i].x;
                edges[edge_count].y0 = pts[i].y;
                edges[edge_count].x1 = pts[sp_start].x;
                edges[edge_count].y1 = pts[sp_start].y;
                edge_count++;
            }
        }
    }
    if (!edge_count) {
        free(edges);
        return 0;
    }
    y0 = (int)jsd_floor(miny);
    y1 = (int)jsd_ceil(maxy);
    if (y0 < 0) y0 = 0;
    if (y1 >= (int)cv->h) y1 = (int)cv->h - 1;
    for (py = y0; py <= y1; py++) {
        double sy = (double)py + 0.5;
        int nx = 0, k, px;

        for (i = 0; i < edge_count; i++) {
            double ey0 = edges[i].y0, ey1 = edges[i].y1;
            if ((ey0 <= sy && ey1 > sy) || (ey1 <= sy && ey0 > sy)) {
                double t = (sy - ey0) / (ey1 - ey0);
                double ix = edges[i].x0 + t * (edges[i].x1 - edges[i].x0);

                if (nx < (int)(sizeof(xs) / sizeof(xs[0])))
                    xs[nx++] = ix;
            }
        }
        for (k = 1; k < nx; k++) {
            double key = xs[k];
            int m = k - 1;
            while (m >= 0 && xs[m] > key) {
                xs[m + 1] = xs[m];
                m--;
            }
            xs[m + 1] = key;
        }
        for (k = 0; k + 1 < nx; k += 2) {
            int x0 = (int)jsd_ceil(xs[k] - 0.5);
            int x1 = (int)jsd_floor(xs[k + 1] - 0.5);

            if (x0 < 0) x0 = 0;
            if (x1 >= (int)cv->w) x1 = (int)cv->w - 1;
            for (px = x0; px <= x1; px++)
                canvas_blend_paint_px(cv, px, py, p);
        }
    }
    free(edges);
    return 1;
}

static void canvas_draw_segment(struct dom_canvas *cv,
                                double x0, double y0, double x1, double y1,
                                double lw, const struct canvas_paint *p)
{
    double half_w, dx, dy, len2, r2;
    int ix0, iy0, ix1, iy1, px, py;

    if (!cv || !cv->w || !cv->h || p->sa == 0)
        return;
    if (lw <= 1.2) {
        int bx0 = (int)jsd_floor(x0), by0 = (int)jsd_floor(y0);
        int bx1 = (int)jsd_floor(x1), by1 = (int)jsd_floor(y1);
        int sx = bx0 < bx1 ? 1 : -1;
        int sy = by0 < by1 ? 1 : -1;
        int adx = bx1 >= bx0 ? bx1 - bx0 : bx0 - bx1;
        int ady = by1 >= by0 ? by1 - by0 : by0 - by1;
        int err = adx - ady, steps = 0;

        while (steps++ < 16384) {
            canvas_blend_paint_px(cv, bx0, by0, p);
            if (bx0 == bx1 && by0 == by1)
                break;
            {
                int e2 = 2 * err;
                if (e2 > -ady) { err -= ady; bx0 += sx; }
                if (e2 < adx) { err += adx; by0 += sy; }
            }
        }
        return;
    }
    half_w = lw * 0.5;
    r2 = half_w * half_w;
    dx = x1 - x0;
    dy = y1 - y0;
    len2 = dx * dx + dy * dy;
    ix0 = (int)jsd_floor((x0 < x1 ? x0 : x1) - half_w - 1.0);
    ix1 = (int)jsd_ceil((x0 > x1 ? x0 : x1) + half_w + 1.0);
    iy0 = (int)jsd_floor((y0 < y1 ? y0 : y1) - half_w - 1.0);
    iy1 = (int)jsd_ceil((y0 > y1 ? y0 : y1) + half_w + 1.0);
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 >= (int)cv->w) ix1 = (int)cv->w - 1;
    if (iy1 >= (int)cv->h) iy1 = (int)cv->h - 1;
    for (py = iy0; py <= iy1; py++) {
        for (px = ix0; px <= ix1; px++) {
            double cx = (double)px + 0.5, cy = (double)py + 0.5;
            double t = len2 > 1e-12 ? ((cx - x0) * dx + (cy - y0) * dy) / len2 : 0.0;
            double projx, projy, d2;

            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            projx = x0 + t * dx;
            projy = y0 + t * dy;
            d2 = (cx - projx) * (cx - projx) + (cy - projy) * (cy - projy);
            if (d2 <= r2)
                canvas_blend_paint_px(cv, px, py, p);
        }
    }
}

static int canvas_pt_add(struct canvas_2d_state *st,
                         double x, double y, unsigned char kind)
{
    if (!st || st->path_n >= 16384u)
        return 0;
    if (st->path_n >= st->path_cap) {
        unsigned int ncap = st->path_cap ? st->path_cap * 2u : 64u;
        struct canvas_pt *narr;

        if (ncap > 16384u) ncap = 16384u;
        narr = (struct canvas_pt *)realloc(st->path, ncap * sizeof(*narr));
        if (!narr)
            return 0;
        st->path = narr;
        st->path_cap = ncap;
    }
    st->path[st->path_n].x = x;
    st->path[st->path_n].y = y;
    st->path[st->path_n].kind = kind;
    st->path_n++;
    return 1;
}

static int ctx2d_num_arg(js_ctx *c, int ac, js_value *av, int idx, double *out)
{
    double v;

    *out = 0.0;
    if (idx >= ac)
        return 0;
    if (js_to_number(c, av[idx], &v) != JS_OK)
        return -1;
    if (!jsd_isfinite(v))
        return 0;
    *out = v;
    return 1;
}

static int ctx2d_canvas_get(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    (void)ac; (void)av;
    if (!ctx2d_of(t, 0, 0))
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal CanvasRenderingContext2D receiver");
    if (js_get(c, t, "__canvas", r) != JS_OK)
        return JS_THROW;
    return JS_OK;
}

static int ctx2d_save(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    (void)ac; (void)av;

    if (!st)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal CanvasRenderingContext2D receiver");
    if (st->sp < JSDOM_CANVAS_STACK_MAX) {
        memset(&st->stack[st->sp], 0, sizeof(st->stack[st->sp]));
        canvas_2d_saved_copy(&st->stack[st->sp], &st->cur);
        st->sp++;
    }
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_restore(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    (void)ac; (void)av;

    if (!st)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal CanvasRenderingContext2D receiver");
    if (st->sp > 0) {
        st->sp--;
        canvas_2d_saved_copy(&st->cur, &st->stack[st->sp]);
        canvas_2d_saved_release(&st->stack[st->sp]);
    }
    *r = js_undefined();
    return JS_OK;
}

static void ctx2d_mul_matrix(struct canvas_2d_saved *cur,
                             double a2, double b2, double c2,
                             double d2, double e2, double f2)
{
    double a1 = cur->m[0], b1 = cur->m[1], c1 = cur->m[2];
    double d1 = cur->m[3], e1 = cur->m[4], f1 = cur->m[5];

    cur->m[0] = a1 * a2 + c1 * b2;
    cur->m[1] = b1 * a2 + d1 * b2;
    cur->m[2] = a1 * c2 + c1 * d2;
    cur->m[3] = b1 * c2 + d1 * d2;
    cur->m[4] = a1 * e2 + c1 * f2 + e1;
    cur->m[5] = b1 * e2 + d1 * f2 + f1;
}

static int ctx2d_translate(js_ctx *c, js_value t, int ac, js_value *av,
                           js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    double x, y;
    int rx, ry;

    if (!st)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal CanvasRenderingContext2D receiver");
    rx = ctx2d_num_arg(c, ac, av, 0, &x);
    ry = ctx2d_num_arg(c, ac, av, 1, &y);
    if (rx < 0 || ry < 0) return JS_THROW;
    if (rx == 1 && ry == 1)
        ctx2d_mul_matrix(&st->cur, 1.0, 0.0, 0.0, 1.0, x, y);
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_scale(js_ctx *c, js_value t, int ac, js_value *av,
                       js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    double sx, sy;
    int rx, ry;

    if (!st)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal CanvasRenderingContext2D receiver");
    rx = ctx2d_num_arg(c, ac, av, 0, &sx);
    ry = ctx2d_num_arg(c, ac, av, 1, &sy);
    if (rx < 0 || ry < 0) return JS_THROW;
    if (rx == 1 && ry == 1)
        ctx2d_mul_matrix(&st->cur, sx, 0.0, 0.0, sy, 0.0, 0.0);
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_rotate(js_ctx *c, js_value t, int ac, js_value *av,
                        js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    double ang;
    int ra;

    if (!st)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal CanvasRenderingContext2D receiver");
    ra = ctx2d_num_arg(c, ac, av, 0, &ang);
    if (ra < 0) return JS_THROW;
    if (ra == 1) {
        double co = jsd_cos(ang), si = jsd_sin(ang);
        ctx2d_mul_matrix(&st->cur, co, si, -si, co, 0.0, 0.0);
    }
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_transform(js_ctx *c, js_value t, int ac, js_value *av,
                           js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    double m[6];
    int i;

    if (!st)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal CanvasRenderingContext2D receiver");
    for (i = 0; i < 6; i++) {
        int rc = ctx2d_num_arg(c, ac, av, i, &m[i]);
        if (rc < 0) return JS_THROW;
        if (rc == 0) { *r = js_undefined(); return JS_OK; }
    }
    ctx2d_mul_matrix(&st->cur, m[0], m[1], m[2], m[3], m[4], m[5]);
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_set_transform(js_ctx *c, js_value t, int ac, js_value *av,
                               js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    double m[6];
    int i;

    if (!st)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal CanvasRenderingContext2D receiver");
    if (ac == 0) {
        st->cur.m[0] = 1.0; st->cur.m[1] = 0.0;
        st->cur.m[2] = 0.0; st->cur.m[3] = 1.0;
        st->cur.m[4] = 0.0; st->cur.m[5] = 0.0;
        *r = js_undefined();
        return JS_OK;
    }
    for (i = 0; i < 6; i++) {
        int rc = ctx2d_num_arg(c, ac, av, i, &m[i]);
        if (rc < 0) return JS_THROW;
        if (rc == 0) { *r = js_undefined(); return JS_OK; }
    }
    for (i = 0; i < 6; i++)
        st->cur.m[i] = m[i];
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_reset_transform(js_ctx *c, js_value t, int ac, js_value *av,
                                 js_value *r)
{
    (void)ac;
    return ctx2d_set_transform(c, t, 0, av, r);
}

static int ctx2d_style_get(js_ctx *c, js_value t, int is_stroke, js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);

    if (!st)
        return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    if (is_stroke ? st->cur.stroke_grad != 0 : st->cur.fill_grad != 0) {
        *r = is_stroke ? st->cur.stroke_grad_val : st->cur.fill_grad_val;
        return JS_OK;
    }
    *r = js_mkcstring(c, is_stroke ? st->cur.stroke_str : st->cur.fill_str);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int ctx2d_style_set(js_ctx *c, js_value t, int is_stroke,
                           int ac, js_value *av, js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    struct hostref *gh;
    const char *s;
    uint32_t col;
    struct canvas_gradient **gslot;
    js_value *vslot;
    uint32_t *cslot;
    char *strslot;
    unsigned long strsz;

    if (!st)
        return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    gslot = is_stroke ? &st->cur.stroke_grad : &st->cur.fill_grad;
    vslot = is_stroke ? &st->cur.stroke_grad_val : &st->cur.fill_grad_val;
    cslot = is_stroke ? &st->cur.stroke_color : &st->cur.fill_color;
    strslot = is_stroke ? st->cur.stroke_str : st->cur.fill_str;
    strsz = is_stroke ? sizeof(st->cur.stroke_str) : sizeof(st->cur.fill_str);
    if (ac > 0 && (gh = host(av[0], HOST_CANVAS_GRADIENT)) != 0) {
        struct canvas_gradient *g = (struct canvas_gradient *)gh->data;
        canvas_grad_retain(g);
        canvas_grad_release(*gslot);
        *gslot = g;
        *vslot = av[0];
        *r = js_undefined();
        return JS_OK;
    }
    if (string_arg(c, ac, av, 0, &s) != JS_OK)
        return JS_THROW;
    if (jsdom_parse_color(s, &col)) {
        canvas_grad_release(*gslot);
        *gslot = 0;
        *vslot = js_undefined();
        *cslot = col;
        canvas_fmt_color(col, strslot, strsz);
    }
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_fill_style_get(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    (void)ac; (void)av;
    return ctx2d_style_get(c, t, 0, r);
}

static int ctx2d_fill_style_set(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    return ctx2d_style_set(c, t, 0, ac, av, r);
}

static int ctx2d_stroke_style_get(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    (void)ac; (void)av;
    return ctx2d_style_get(c, t, 1, r);
}

static int ctx2d_stroke_style_set(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    return ctx2d_style_set(c, t, 1, ac, av, r);
}

static int ctx2d_gradient_new(js_ctx *c, struct jsdom *j, int kind,
                              double x0, double y0, double r0,
                              double x1, double y1, double r1, js_value *r)
{
    struct canvas_gradient *g;
    js_object *obj;

    g = (struct canvas_gradient *)calloc(1, sizeof(*g));
    if (!g)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    g->refs = 1;
    g->kind = kind;
    g->x0 = x0; g->y0 = y0; g->r0 = r0;
    g->x1 = x1; g->y1 = y1; g->r1 = r1;
    obj = new_host_data(j, 0, HOST_CANVAS_GRADIENT, g,
                        j->canvas_gradient_proto);
    if (!obj) {
        canvas_grad_release(g);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    *r = js_object_value(obj);
    return JS_OK;
}

static int ctx2d_create_linear_gradient(js_ctx *c, js_value t,
                                        int ac, js_value *av, js_value *r)
{
    struct hostref *h = 0;
    struct canvas_2d_state *st = ctx2d_of(t, &h, 0);
    double v[4];
    int i;

    if (!st)
        return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    if (ac < 4)
        return js_throw_error(c, JS_ERR_TYPE,
                              "createLinearGradient requires 4 arguments");
    for (i = 0; i < 4; i++) {
        if (js_to_number(c, av[i], &v[i]) != JS_OK)
            return JS_THROW;
        if (!jsd_isfinite(v[i]))
            return js_throw_error(c, JS_ERR_TYPE,
                                  "non-finite gradient coordinate");
    }
    return ctx2d_gradient_new(c, h->j, JSDOM_GRADIENT_LINEAR,
                              v[0], v[1], 0.0, v[2], v[3], 0.0, r);
}

static int ctx2d_create_radial_gradient(js_ctx *c, js_value t,
                                        int ac, js_value *av, js_value *r)
{
    struct hostref *h = 0;
    struct canvas_2d_state *st = ctx2d_of(t, &h, 0);
    double v[6];
    int i;

    if (!st)
        return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    if (ac < 6)
        return js_throw_error(c, JS_ERR_TYPE,
                              "createRadialGradient requires 6 arguments");
    for (i = 0; i < 6; i++) {
        if (js_to_number(c, av[i], &v[i]) != JS_OK)
            return JS_THROW;
        if (!jsd_isfinite(v[i]))
            return js_throw_error(c, JS_ERR_TYPE,
                                  "non-finite gradient coordinate");
    }
    if (v[2] < 0.0 || v[5] < 0.0)
        return throw_dom_exception(c, "IndexSizeError",
                                   "radial gradient radius must be non-negative");
    return ctx2d_gradient_new(c, h->j, JSDOM_GRADIENT_RADIAL,
                              v[0], v[1], v[2], v[3], v[4], v[5], r);
}

static int canvas_gradient_add_color_stop(js_ctx *c, js_value t,
                                          int ac, js_value *av, js_value *r)
{
    struct hostref *gh = host(t, HOST_CANVAS_GRADIENT);
    struct canvas_gradient *g;
    double offset;
    const char *color_str;
    uint32_t col;
    unsigned int pos;

    if (!gh || !gh->data)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal CanvasGradient receiver");
    g = (struct canvas_gradient *)gh->data;
    if (ac < 2)
        return js_throw_error(c, JS_ERR_TYPE,
                              "addColorStop requires offset and color");
    if (js_to_number(c, av[0], &offset) != JS_OK)
        return JS_THROW;
    if (!jsd_isfinite(offset) || offset < 0.0 || offset > 1.0)
        return throw_dom_exception(c, "IndexSizeError",
                                   "color stop offset must be in [0, 1]");
    if (string_arg(c, ac, av, 1, &color_str) != JS_OK)
        return JS_THROW;
    if (!jsdom_parse_color(color_str, &col))
        return throw_dom_exception(c, "SyntaxError",
                                   "invalid color stop value");
    if (g->stop_count >= 256u)
        return js_throw_error(c, JS_ERR_RANGE, "too many gradient color stops");
    if (g->stop_count >= g->stop_cap) {
        unsigned int ncap = g->stop_cap ? g->stop_cap * 2u : 8u;
        struct canvas_grad_stop *ns;

        ns = (struct canvas_grad_stop *)realloc(
            g->stops, (unsigned long)ncap * sizeof(*ns));
        if (!ns)
            return js_throw_error(c, JS_ERR_ERROR, "out of memory");
        g->stops = ns;
        g->stop_cap = ncap;
    }
    pos = g->stop_count;
    while (pos > 0 && g->stops[pos - 1].offset > offset) {
        g->stops[pos] = g->stops[pos - 1];
        pos--;
    }
    g->stops[pos].offset = offset;
    g->stops[pos].color = col;
    g->stop_count++;
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_line_width_get(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    (void)ac; (void)av;
    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    *r = js_number(st->cur.line_width);
    return JS_OK;
}

static int ctx2d_line_width_set(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    double v;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    if (ac > 0) {
        if (js_to_number(c, av[0], &v) != JS_OK) return JS_THROW;
        if (jsd_isfinite(v) && v > 0.0)
            st->cur.line_width = v;
    }
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_global_alpha_get(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    (void)ac; (void)av;
    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    *r = js_number(st->cur.global_alpha);
    return JS_OK;
}

static int ctx2d_global_alpha_set(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    double v;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    if (ac > 0) {
        if (js_to_number(c, av[0], &v) != JS_OK) return JS_THROW;
        if (jsd_isfinite(v) && v >= 0.0 && v <= 1.0)
            st->cur.global_alpha = v;
    }
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_font_get(js_ctx *c, js_value t,
                          int ac, js_value *av, js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    (void)ac; (void)av;
    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    *r = js_mkcstring(c, st->cur.font);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int ctx2d_font_set(js_ctx *c, js_value t,
                          int ac, js_value *av, js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    const char *s, *p;
    double sz = 10.0;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    if (string_arg(c, ac, av, 0, &s) != JS_OK) return JS_THROW;
    if (*s && strlen(s) < sizeof(st->cur.font)) {
        for (p = s; *p; p++) {
            const char *end;
            double v;
            if ((*p >= '0' && *p <= '9') &&
                jsd_parse_double(p, &v, &end) && v > 0.0) {
                if (ci_eq_n(end, 2, "px", 2)) { sz = v; break; }
                if (ci_eq_n(end, 2, "pt", 2)) { sz = v * 4.0 / 3.0; break; }
            }
        }
        snprintf(st->cur.font, sizeof(st->cur.font), "%s", s);
        st->cur.font_px = sz;
    }
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_text_align_get(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    const char *v;
    (void)ac; (void)av;
    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    v = st->cur.text_align == 1 ? "center" :
        st->cur.text_align == 2 ? "right" : "left";
    *r = js_mkcstring(c, v);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int ctx2d_text_align_set(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    const char *s;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    if (string_arg(c, ac, av, 0, &s) != JS_OK) return JS_THROW;
    if (!strcmp(s, "left") || !strcmp(s, "start")) st->cur.text_align = 0;
    else if (!strcmp(s, "center")) st->cur.text_align = 1;
    else if (!strcmp(s, "right") || !strcmp(s, "end")) st->cur.text_align = 2;
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_text_baseline_get(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    static const char *const names[] = {
        "alphabetic", "top", "middle", "bottom"
    };
    (void)ac; (void)av;
    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    *r = js_mkcstring(c, names[st->cur.text_baseline < 4 ? st->cur.text_baseline : 0]);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int ctx2d_text_baseline_set(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    const char *s;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    if (string_arg(c, ac, av, 0, &s) != JS_OK) return JS_THROW;
    if (!strcmp(s, "alphabetic") || !strcmp(s, "ideographic"))
        st->cur.text_baseline = 0;
    else if (!strcmp(s, "top") || !strcmp(s, "hanging"))
        st->cur.text_baseline = 1;
    else if (!strcmp(s, "middle"))
        st->cur.text_baseline = 2;
    else if (!strcmp(s, "bottom"))
        st->cur.text_baseline = 3;
    *r = js_undefined();
    return JS_OK;
}

static void ctx2d_fill_transformed_rect(struct dom_canvas *cv,
                                        const struct canvas_2d_saved *cur,
                                        double x, double y, double w, double h,
                                        const struct canvas_paint *p)
{
    struct canvas_pt pts[4];
    double x0, y0, x1, y1;

    if (!cv || !cv->w || !cv->h || (!p->is_clear && p->sa == 0))
        return;
    if (w < 0.0) { x += w; w = -w; }
    if (h < 0.0) { y += h; h = -h; }
    if (w == 0.0 || h == 0.0)
        return;
    if (jsd_fabs(cur->m[1]) < 1e-9 && jsd_fabs(cur->m[2]) < 1e-9 &&
        cur->m[0] >= 0.0 && cur->m[3] >= 0.0) {
        int ix0, iy0, ix1, iy1, px, py;

        x0 = cur->m[0] * x + cur->m[4];
        y0 = cur->m[3] * y + cur->m[5];
        x1 = cur->m[0] * (x + w) + cur->m[4];
        y1 = cur->m[3] * (y + h) + cur->m[5];
        ix0 = (int)jsd_ceil(x0 - 0.5);
        iy0 = (int)jsd_ceil(y0 - 0.5);
        ix1 = (int)jsd_floor(x1 - 0.5);
        iy1 = (int)jsd_floor(y1 - 0.5);
        if (ix0 < 0) ix0 = 0;
        if (iy0 < 0) iy0 = 0;
        if (ix1 >= (int)cv->w) ix1 = (int)cv->w - 1;
        if (iy1 >= (int)cv->h) iy1 = (int)cv->h - 1;
        for (py = iy0; py <= iy1; py++)
            for (px = ix0; px <= ix1; px++)
                canvas_blend_paint_px(cv, px, py, p);
        return;
    }
    canvas_xform(cur, x, y, &pts[0].x, &pts[0].y); pts[0].kind = 0;
    canvas_xform(cur, x + w, y, &pts[1].x, &pts[1].y); pts[1].kind = 1;
    canvas_xform(cur, x + w, y + h, &pts[2].x, &pts[2].y); pts[2].kind = 1;
    canvas_xform(cur, x, y + h, &pts[3].x, &pts[3].y); pts[3].kind = 1;
    canvas_rasterize_poly(cv, pts, 4, p);
}

static int ctx2d_rect_op(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r, int mode)
{
    struct hostref *h = 0;
    struct dom_canvas *cv = 0;
    struct canvas_2d_state *st = ctx2d_of(t, &h, &cv);
    struct canvas_paint paint;
    double x, y, w, hbox;
    int i, ok = 1;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    {
        double *dst[4] = { &x, &y, &w, &hbox };
        for (i = 0; i < 4; i++) {
            int rc = ctx2d_num_arg(c, ac, av, i, dst[i]);
            if (rc < 0) return JS_THROW;
            if (rc == 0) ok = 0;
        }
    }
    if (!ok || !cv) {
        *r = js_undefined();
        return JS_OK;
    }
    if (mode == 0 || mode == 2) {
        canvas_paint_init(&paint, &st->cur, 0, mode == 2);
        ctx2d_fill_transformed_rect(cv, &st->cur, x, y, w, hbox, &paint);
    } else {
        double p[4][2];

        canvas_paint_init(&paint, &st->cur, 1, 0);
        canvas_xform(&st->cur, x, y, &p[0][0], &p[0][1]);
        canvas_xform(&st->cur, x + w, y, &p[1][0], &p[1][1]);
        canvas_xform(&st->cur, x + w, y + hbox, &p[2][0], &p[2][1]);
        canvas_xform(&st->cur, x, y + hbox, &p[3][0], &p[3][1]);
        for (i = 0; i < 4; i++)
            canvas_draw_segment(cv, p[i][0], p[i][1],
                                p[(i + 1) & 3][0], p[(i + 1) & 3][1],
                                st->cur.line_width, &paint);
    }
    mark_dom_dirty(h->j);
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_fill_rect(js_ctx *c, js_value t, int ac, js_value *av,
                           js_value *r)
{ return ctx2d_rect_op(c, t, ac, av, r, 0); }
static int ctx2d_stroke_rect(js_ctx *c, js_value t, int ac, js_value *av,
                             js_value *r)
{ return ctx2d_rect_op(c, t, ac, av, r, 1); }
static int ctx2d_clear_rect(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{ return ctx2d_rect_op(c, t, ac, av, r, 2); }

static int ctx2d_begin_path(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    (void)ac; (void)av;
    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    st->path_n = 0;
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_close_path(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    unsigned int sp_start;
    (void)ac; (void)av;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    if (st->path_n > 1) {
        sp_start = st->path_n - 1;
        while (sp_start > 0 && st->path[sp_start].kind != 0)
            sp_start--;
        if (st->path_n - 1 > sp_start)
            (void)canvas_pt_add(st, st->path[sp_start].x,
                                st->path[sp_start].y, 2);
    }
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_move_to(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    double x, y, dx, dy;
    int rx, ry;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    rx = ctx2d_num_arg(c, ac, av, 0, &x);
    ry = ctx2d_num_arg(c, ac, av, 1, &y);
    if (rx < 0 || ry < 0) return JS_THROW;
    if (rx == 1 && ry == 1) {
        canvas_xform(&st->cur, x, y, &dx, &dy);
        (void)canvas_pt_add(st, dx, dy, 0);
    }
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_line_to(js_ctx *c, js_value t, int ac, js_value *av,
                         js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    double x, y, dx, dy;
    int rx, ry;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    rx = ctx2d_num_arg(c, ac, av, 0, &x);
    ry = ctx2d_num_arg(c, ac, av, 1, &y);
    if (rx < 0 || ry < 0) return JS_THROW;
    if (rx == 1 && ry == 1) {
        canvas_xform(&st->cur, x, y, &dx, &dy);
        (void)canvas_pt_add(st, dx, dy, st->path_n ? 1 : 0);
    }
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_rect(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    double x, y, w, hbox, dx, dy;
    int rx, ry, rw, rh;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    rx = ctx2d_num_arg(c, ac, av, 0, &x);
    ry = ctx2d_num_arg(c, ac, av, 1, &y);
    rw = ctx2d_num_arg(c, ac, av, 2, &w);
    rh = ctx2d_num_arg(c, ac, av, 3, &hbox);
    if (rx < 0 || ry < 0 || rw < 0 || rh < 0) return JS_THROW;
    if (rx == 1 && ry == 1 && rw == 1 && rh == 1) {
        canvas_xform(&st->cur, x, y, &dx, &dy);
        (void)canvas_pt_add(st, dx, dy, 0);
        canvas_xform(&st->cur, x + w, y, &dx, &dy);
        (void)canvas_pt_add(st, dx, dy, 1);
        canvas_xform(&st->cur, x + w, y + hbox, &dx, &dy);
        (void)canvas_pt_add(st, dx, dy, 1);
        canvas_xform(&st->cur, x, y + hbox, &dx, &dy);
        (void)canvas_pt_add(st, dx, dy, 1);
        canvas_xform(&st->cur, x, y, &dx, &dy);
        (void)canvas_pt_add(st, dx, dy, 2);
    }
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_arc(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    double cx, cy, rad, a0, a1, sweep, scale;
    int rx, ry, rr, r0, r1, ccw = 0, segs, i;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    rx = ctx2d_num_arg(c, ac, av, 0, &cx);
    ry = ctx2d_num_arg(c, ac, av, 1, &cy);
    rr = ctx2d_num_arg(c, ac, av, 2, &rad);
    r0 = ctx2d_num_arg(c, ac, av, 3, &a0);
    r1 = ctx2d_num_arg(c, ac, av, 4, &a1);
    if (rx < 0 || ry < 0 || rr < 0 || r0 < 0 || r1 < 0) return JS_THROW;
    if (rx != 1 || ry != 1 || rr != 1 || r0 != 1 || r1 != 1) {
        *r = js_undefined();
        return JS_OK;
    }
    if (rad < 0.0)
        return throw_dom_exception(c, "IndexSizeError",
                                   "arc radius must be non-negative");
    if (ac > 5)
        ccw = js_to_boolean(av[5]);
    if (ccw) {
        if (a0 - a1 >= JSDOM_PI2) {
            sweep = -JSDOM_PI2;
        } else {
            sweep = a1 - a0;
            while (sweep > 0.0) sweep -= JSDOM_PI2;
            while (sweep <= -JSDOM_PI2) sweep += JSDOM_PI2;
            if (sweep == 0.0 && a0 != a1) sweep = -JSDOM_PI2;
        }
    } else {
        if (a1 - a0 >= JSDOM_PI2) {
            sweep = JSDOM_PI2;
        } else {
            sweep = a1 - a0;
            while (sweep < 0.0) sweep += JSDOM_PI2;
            while (sweep > JSDOM_PI2) sweep -= JSDOM_PI2;
            if (sweep == 0.0 && a0 != a1) sweep = JSDOM_PI2;
        }
    }
    scale = jsd_sqrt(st->cur.m[0] * st->cur.m[0] + st->cur.m[1] * st->cur.m[1]);
    segs = (int)(rad * scale * jsd_fabs(sweep) * 0.5) + 12;
    if (segs < 16) segs = 16;
    if (segs > 128) segs = 128;
    for (i = 0; i <= segs; i++) {
        double a = a0 + sweep * ((double)i / (double)segs);
        double px = cx + rad * jsd_cos(a);
        double py = cy + rad * jsd_sin(a);
        double dx, dy;

        canvas_xform(&st->cur, px, py, &dx, &dy);
        (void)canvas_pt_add(st, dx, dy, st->path_n ? 1 : 0);
    }
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_quadratic_curve_to(js_ctx *c, js_value t,
                                    int ac, js_value *av, js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    double cpx, cpy, x, y, dcx, dcy, dx, dy, p0x, p0y;
    int rc0, rc1, rc2, rc3, i;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    rc0 = ctx2d_num_arg(c, ac, av, 0, &cpx);
    rc1 = ctx2d_num_arg(c, ac, av, 1, &cpy);
    rc2 = ctx2d_num_arg(c, ac, av, 2, &x);
    rc3 = ctx2d_num_arg(c, ac, av, 3, &y);
    if (rc0 < 0 || rc1 < 0 || rc2 < 0 || rc3 < 0) return JS_THROW;
    if (rc0 != 1 || rc1 != 1 || rc2 != 1 || rc3 != 1) {
        *r = js_undefined();
        return JS_OK;
    }
    canvas_xform(&st->cur, cpx, cpy, &dcx, &dcy);
    canvas_xform(&st->cur, x, y, &dx, &dy);
    if (!st->path_n)
        (void)canvas_pt_add(st, dcx, dcy, 0);
    p0x = st->path[st->path_n - 1].x;
    p0y = st->path[st->path_n - 1].y;
    for (i = 1; i <= 24; i++) {
        double u = (double)i / 24.0, inv = 1.0 - u;
        double bx = inv * inv * p0x + 2.0 * inv * u * dcx + u * u * dx;
        double by = inv * inv * p0y + 2.0 * inv * u * dcy + u * u * dy;
        (void)canvas_pt_add(st, bx, by, 1);
    }
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_bezier_curve_to(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    double v[6], dc1x, dc1y, dc2x, dc2y, dx, dy, p0x, p0y;
    int i;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    for (i = 0; i < 6; i++) {
        int rc = ctx2d_num_arg(c, ac, av, i, &v[i]);
        if (rc < 0) return JS_THROW;
        if (rc == 0) { *r = js_undefined(); return JS_OK; }
    }
    canvas_xform(&st->cur, v[0], v[1], &dc1x, &dc1y);
    canvas_xform(&st->cur, v[2], v[3], &dc2x, &dc2y);
    canvas_xform(&st->cur, v[4], v[5], &dx, &dy);
    if (!st->path_n)
        (void)canvas_pt_add(st, dc1x, dc1y, 0);
    p0x = st->path[st->path_n - 1].x;
    p0y = st->path[st->path_n - 1].y;
    for (i = 1; i <= 24; i++) {
        double u = (double)i / 24.0, inv = 1.0 - u;
        double a0 = inv * inv * inv, a1 = 3.0 * inv * inv * u;
        double a2 = 3.0 * inv * u * u, a3 = u * u * u;
        double bx = a0 * p0x + a1 * dc1x + a2 * dc2x + a3 * dx;
        double by = a0 * p0y + a1 * dc1y + a2 * dc2y + a3 * dy;
        (void)canvas_pt_add(st, bx, by, 1);
    }
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_fill(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)
{
    struct hostref *h = 0;
    struct dom_canvas *cv = 0;
    struct canvas_2d_state *st = ctx2d_of(t, &h, &cv);
    struct canvas_paint paint;
    (void)ac; (void)av;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    if (cv && st->path_n >= 3) {
        canvas_paint_init(&paint, &st->cur, 0, 0);
        if (canvas_rasterize_poly(cv, st->path, st->path_n, &paint))
            mark_dom_dirty(h->j);
    }
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_stroke(js_ctx *c, js_value t, int ac, js_value *av,
                        js_value *r)
{
    struct hostref *h = 0;
    struct dom_canvas *cv = 0;
    struct canvas_2d_state *st = ctx2d_of(t, &h, &cv);
    struct canvas_paint paint;
    unsigned int i;
    (void)ac; (void)av;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    if (cv && st->path_n >= 2) {
        canvas_paint_init(&paint, &st->cur, 1, 0);
        for (i = 1; i < st->path_n; i++) {
            if (st->path[i].kind == 0)
                continue;
            canvas_draw_segment(cv, st->path[i - 1].x, st->path[i - 1].y,
                                st->path[i].x, st->path[i].y,
                                st->cur.line_width, &paint);
        }
        mark_dom_dirty(h->j);
    }
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_draw_text_op(js_ctx *c, js_value t, int ac, js_value *av,
                              js_value *r, int is_stroke)
{
    struct hostref *h = 0;
    struct dom_canvas *cv = 0;
    struct canvas_2d_state *st = ctx2d_of(t, &h, &cv);
    struct canvas_paint paint;
    const char *text;
    double x, y, scale, total_w, ox, oy;
    int rx, ry;
    unsigned long len, ci;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    if (ac < 3 || string_arg(c, ac, av, 0, &text) != JS_OK)
        return ac < 3 ? js_throw_error(c, JS_ERR_TYPE, "too few arguments")
                      : JS_THROW;
    rx = ctx2d_num_arg(c, ac, av, 1, &x);
    ry = ctx2d_num_arg(c, ac, av, 2, &y);
    if (rx < 0 || ry < 0) return JS_THROW;
    if (!cv || rx != 1 || ry != 1 || !*text) {
        *r = js_undefined();
        return JS_OK;
    }
    len = strlen(text);
    scale = st->cur.font_px / 7.0;
    if (scale < 1.0) scale = 1.0;
    total_w = (double)len * 6.0 * scale;
    ox = x;
    if (st->cur.text_align == 1) ox -= total_w * 0.5;
    else if (st->cur.text_align == 2) ox -= total_w;
    if (st->cur.text_baseline == 1) oy = y;
    else if (st->cur.text_baseline == 2) oy = y - 3.5 * scale;
    else if (st->cur.text_baseline == 3) oy = y - 7.0 * scale;
    else oy = y - 6.0 * scale;

    canvas_paint_init(&paint, &st->cur, is_stroke, 0);

    for (ci = 0; ci < len; ci++) {
        unsigned char ch = (unsigned char)text[ci];
        int gx, gy;

        if (ch < 0x20 || ch > 0x7E)
            ch = '?';
        for (gx = 0; gx < 5; gx++) {
            uint8_t col_bits = jsdom_font5x7[ch - 0x20][gx];
            for (gy = 0; gy < 7; gy++) {
                if (!(col_bits & (1u << gy)))
                    continue;
                if (is_stroke) {
                    int edge = (gx == 0 || gx == 4 || gy == 0 || gy == 6 ||
                                !(jsdom_font5x7[ch - 0x20][gx - 1] & (1u << gy)) ||
                                !(jsdom_font5x7[ch - 0x20][gx + 1] & (1u << gy)) ||
                                !(col_bits & (1u << (gy - 1))) ||
                                !(col_bits & (1u << (gy + 1))));
                    if (!edge) continue;
                }
                ctx2d_fill_transformed_rect(
                    cv, &st->cur,
                    ox + ((double)ci * 6.0 + (double)gx) * scale,
                    oy + (double)gy * scale,
                    scale, scale, &paint);
            }
        }
    }
    mark_dom_dirty(h->j);
    *r = js_undefined();
    return JS_OK;
}

static int ctx2d_fill_text(js_ctx *c, js_value t, int ac, js_value *av,
                           js_value *r)
{ return ctx2d_draw_text_op(c, t, ac, av, r, 0); }
static int ctx2d_stroke_text(js_ctx *c, js_value t, int ac, js_value *av,
                             js_value *r)
{ return ctx2d_draw_text_op(c, t, ac, av, r, 1); }

static int ctx2d_measure_text(js_ctx *c, js_value t, int ac, js_value *av,
                              js_value *r)
{
    struct canvas_2d_state *st = ctx2d_of(t, 0, 0);
    const char *text;
    double scale;
    js_object *m;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    if (string_arg(c, ac, av, 0, &text) != JS_OK) return JS_THROW;
    scale = st->cur.font_px / 7.0;
    if (scale < 1.0) scale = 1.0;
    m = js_new_object(c);
    if (!m ||
        js_set(c, js_object_value(m), "width",
               js_number((double)strlen(text) * 6.0 * scale)) != JS_OK)
        return JS_THROW;
    *r = js_object_value(m);
    return JS_OK;
}

static int image_data_dims(js_ctx *c, double dw, double dh,
                           unsigned int *ow, unsigned int *oh)
{
    unsigned int w, h;

    if (!jsd_isfinite(dw) || !jsd_isfinite(dh) || dw < 1.0 || dh < 1.0 ||
        dw > (double)JSDOM_CANVAS_MAX_DIM ||
        dh > (double)JSDOM_CANVAS_MAX_DIM)
        return throw_dom_exception(c, "IndexSizeError",
                                   "invalid ImageData dimensions");
    w = (unsigned int)dw;
    h = (unsigned int)dh;
    if (w == 0 || h == 0 || w > JSDOM_CANVAS_MAX_PIXELS / h)
        return throw_dom_exception(c, "IndexSizeError",
                                   "invalid ImageData dimensions");
    *ow = w;
    *oh = h;
    return JS_OK;
}

static int make_image_data_obj_buf(js_ctx *c, struct jsdom *j,
                                   unsigned int w, unsigned int hpx,
                                   const uint8_t *src_rgba, js_value buf_arr,
                                   js_value *out)
{
    js_object *obj;
    js_value u8 = buf_arr;
    unsigned long byte_len;

    if (w == 0 || hpx == 0 || w > JSDOM_CANVAS_MAX_DIM ||
        hpx > JSDOM_CANVAS_MAX_DIM || w > JSDOM_CANVAS_MAX_PIXELS / hpx)
        return throw_dom_exception(c, "IndexSizeError",
                                   "invalid ImageData dimensions");
    if (js_is_undefined(u8)) {
        byte_len = (unsigned long)w * (unsigned long)hpx * 4ul;
        u8 = js_uint8clampedarray_new(c, src_rgba, byte_len);
        if (js_fatal(c) || js_is_undefined(u8))
            return JS_THROW;
    }
    obj = new_host(j, 0, HOST_IMAGE_DATA, j->image_data_proto);
    if (!obj)
        return JS_THROW;
    if (js_define_enum(c, obj, "width", js_number((double)w)) != JS_OK ||
        js_define_enum(c, obj, "height", js_number((double)hpx)) != JS_OK ||
        js_define_enum(c, obj, "data", u8) != JS_OK)
        return JS_THROW;
    *out = js_object_value(obj);
    return JS_OK;
}

static int make_image_data_obj(js_ctx *c, struct jsdom *j,
                               unsigned int w, unsigned int hpx,
                               const uint8_t *src_rgba, js_value *out)
{
    return make_image_data_obj_buf(c, j, w, hpx, src_rgba, js_undefined(), out);
}

static int image_data_constructor(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    double dw, dh;
    unsigned int w = 0, h = 0;
    (void)t;

    if (!j || !js_is_constructing(c))
        return js_throw_error(c, JS_ERR_TYPE,
                              "ImageData must be constructed with new");
    if (ac < 2)
        return js_throw_error(c, JS_ERR_TYPE,
                              "ImageData requires at least 2 arguments");
    if (js_is_object(av[0])) {
        unsigned long dlen = 0, pixels, req_h;

        if (!js_is_uint8clampedarray(av[0]))
            return js_throw_error(c, JS_ERR_TYPE,
                                  "ImageData data must be a Uint8ClampedArray");
        (void)js_uint8clampedarray_data(av[0], &dlen);
        if (dlen == 0 || (dlen % 4ul) != 0)
            return throw_dom_exception(c, "InvalidStateError",
                                       "ImageData data length must be a positive multiple of 4");
        if (js_to_number(c, av[1], &dw) != JS_OK)
            return JS_THROW;
        if (!jsd_isfinite(dw) || dw < 1.0 || dw > (double)JSDOM_CANVAS_MAX_DIM)
            return throw_dom_exception(c, "IndexSizeError",
                                       "invalid ImageData width");
        w = (unsigned int)dw;
        pixels = dlen / 4ul;
        if (w == 0 || (pixels % (unsigned long)w) != 0)
            return throw_dom_exception(c, "IndexSizeError",
                                       "ImageData buffer length does not match width");
        req_h = pixels / (unsigned long)w;
        if (ac >= 3 && !js_is_undefined(av[2]) && !js_is_object(av[2])) {
            if (js_to_number(c, av[2], &dh) != JS_OK)
                return JS_THROW;
            if (!jsd_isfinite(dh) || dh < 1.0 ||
                dh > (double)JSDOM_CANVAS_MAX_DIM ||
                (unsigned long)(unsigned int)dh != req_h)
                return throw_dom_exception(c, "IndexSizeError",
                                           "ImageData height does not match buffer length");
        }
        if (image_data_dims(c, (double)w, (double)req_h, &w, &h) != JS_OK)
            return JS_THROW;
        return make_image_data_obj_buf(c, j, w, h, 0, av[0], r);
    }
    if (js_to_number(c, av[0], &dw) != JS_OK ||
        js_to_number(c, av[1], &dh) != JS_OK)
        return JS_THROW;
    if (image_data_dims(c, dw, dh, &w, &h) != JS_OK)
        return JS_THROW;
    return make_image_data_obj(c, j, w, h, 0, r);
}

static int ctx2d_create_image_data(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    struct hostref *h = 0;
    struct canvas_2d_state *st = ctx2d_of(t, &h, 0);
    double w, hbox;
    unsigned int wu = 0, hu = 0;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    if (ac >= 1 && js_is_object(av[0])) {
        js_value vw, vh;

        if (js_get(c, av[0], "width", &vw) != JS_OK ||
            js_get(c, av[0], "height", &vh) != JS_OK ||
            js_to_number(c, vw, &w) != JS_OK ||
            js_to_number(c, vh, &hbox) != JS_OK)
            return JS_THROW;
    } else {
        if (ac < 2 || js_to_number(c, av[0], &w) != JS_OK ||
            js_to_number(c, av[1], &hbox) != JS_OK)
            return ac < 2
                ? js_throw_error(c, JS_ERR_TYPE,
                                 "createImageData requires dimensions")
                : JS_THROW;
    }
    if (image_data_dims(c, w, hbox, &wu, &hu) != JS_OK)
        return JS_THROW;
    return make_image_data_obj(c, h->j, wu, hu, 0, r);
}

static int ctx2d_get_image_data(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    struct hostref *h = 0;
    struct dom_canvas *cv = 0;
    struct canvas_2d_state *st = ctx2d_of(t, &h, &cv);
    double dsx, dsy, dsw, dsh;
    int sx, sy, sw, sh, iy, ix;
    uint8_t *buf;
    int rc;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    if (ac < 4)
        return js_throw_error(c, JS_ERR_TYPE, "getImageData requires 4 args");
    if (js_to_number(c, av[0], &dsx) != JS_OK ||
        js_to_number(c, av[1], &dsy) != JS_OK ||
        js_to_number(c, av[2], &dsw) != JS_OK ||
        js_to_number(c, av[3], &dsh) != JS_OK)
        return JS_THROW;
    if (!jsd_isfinite(dsw) || !jsd_isfinite(dsh) || dsw == 0.0 || dsh == 0.0)
        return throw_dom_exception(c, "IndexSizeError",
                                   "getImageData width/height must be non-zero");
    if (dsw < 0.0) { dsx += dsw; dsw = -dsw; }
    if (dsh < 0.0) { dsy += dsh; dsh = -dsh; }
    sx = (int)jsd_floor(dsx);
    sy = (int)jsd_floor(dsy);
    sw = (int)dsw;
    sh = (int)dsh;
    if (sw <= 0 || sh <= 0 || (unsigned int)sw > JSDOM_CANVAS_MAX_DIM ||
        (unsigned int)sh > JSDOM_CANVAS_MAX_DIM ||
        (unsigned int)sw > JSDOM_CANVAS_MAX_PIXELS / (unsigned int)sh)
        return throw_dom_exception(c, "IndexSizeError",
                                   "getImageData dimensions out of range");
    buf = (uint8_t *)calloc((unsigned long)sw * (unsigned long)sh, 4ul);
    if (!buf)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    if (cv && cv->px && cv->alpha) {
        for (iy = 0; iy < sh; iy++) {
            int cy = sy + iy;
            if (cy < 0 || cy >= cv->h)
                continue;
            for (ix = 0; ix < sw; ix++) {
                int cx = sx + ix;
                unsigned long si, di;

                if (cx < 0 || cx >= cv->w)
                    continue;
                si = (unsigned long)cy * (unsigned long)cv->w + (unsigned long)cx;
                di = ((unsigned long)iy * (unsigned long)sw + (unsigned long)ix) * 4ul;
                buf[di]     = (uint8_t)((cv->px[si] >> 16) & 0xFFu);
                buf[di + 1] = (uint8_t)((cv->px[si] >> 8) & 0xFFu);
                buf[di + 2] = (uint8_t)(cv->px[si] & 0xFFu);
                buf[di + 3] = cv->alpha[si];
            }
        }
    }
    rc = make_image_data_obj(c, h->j, (unsigned int)sw, (unsigned int)sh, buf, r);
    free(buf);
    return rc;
}

static int ctx2d_put_image_data(js_ctx *c, js_value t,
                                int ac, js_value *av, js_value *r)
{
    struct hostref *h = 0;
    struct dom_canvas *cv = 0;
    struct canvas_2d_state *st = ctx2d_of(t, &h, &cv);
    js_value vw, vh, vdata;
    double dw, dh, ddx, ddy;
    const uint8_t *src;
    unsigned long dlen = 0, need;
    int iw, ih, dx, dy, sx0 = 0, sy0 = 0, sx1, sy1, ix, iy;

    if (!st) return js_throw_error(c, JS_ERR_TYPE, "illegal ctx2d");
    if (ac < 3 || !js_is_object(av[0]))
        return js_throw_error(c, JS_ERR_TYPE,
                              "putImageData requires ImageData and dx, dy");
    if (ac > 3 && ac < 7)
        return js_throw_error(c, JS_ERR_TYPE,
                              "putImageData requires 3 or 7 arguments");
    if (js_get(c, av[0], "width", &vw) != JS_OK ||
        js_get(c, av[0], "height", &vh) != JS_OK ||
        js_get(c, av[0], "data", &vdata) != JS_OK ||
        js_to_number(c, vw, &dw) != JS_OK ||
        js_to_number(c, vh, &dh) != JS_OK ||
        js_to_number(c, av[1], &ddx) != JS_OK ||
        js_to_number(c, av[2], &ddy) != JS_OK)
        return JS_THROW;
    src = (const uint8_t *)js_uint8clampedarray_data(vdata, &dlen);
    if (!src && js_is_uint8array(vdata))
        src = (const uint8_t *)js_uint8array_data(vdata, &dlen);
    if (!jsd_isfinite(dw) || !jsd_isfinite(dh) || dw < 1.0 || dh < 1.0 ||
        dw > (double)JSDOM_CANVAS_MAX_DIM || dh > (double)JSDOM_CANVAS_MAX_DIM)
        return js_throw_error(c, JS_ERR_TYPE, "invalid ImageData");
    iw = (int)dw;
    ih = (int)dh;
    if (!src || iw <= 0 || ih <= 0)
        return js_throw_error(c, JS_ERR_TYPE, "invalid ImageData");
    need = (unsigned long)iw * (unsigned long)ih * 4ul;
    if (dlen < need)
        return js_throw_error(c, JS_ERR_RANGE, "ImageData buffer too small");
    sx1 = iw;
    sy1 = ih;
    if (ac >= 7) {
        double dsx, dsy, dsw, dsh;

        if (js_to_number(c, av[3], &dsx) != JS_OK ||
            js_to_number(c, av[4], &dsy) != JS_OK ||
            js_to_number(c, av[5], &dsw) != JS_OK ||
            js_to_number(c, av[6], &dsh) != JS_OK)
            return JS_THROW;
        if (!jsd_isfinite(ddx) || !jsd_isfinite(ddy) ||
            !jsd_isfinite(dsx) || !jsd_isfinite(dsy) ||
            !jsd_isfinite(dsw) || !jsd_isfinite(dsh))
            return js_throw_error(c, JS_ERR_TYPE, "non-finite putImageData arg");
        if (dsw < 0.0) { dsx += dsw; dsw = -dsw; }
        if (dsh < 0.0) { dsy += dsh; dsh = -dsh; }
        sx0 = (int)jsd_floor(dsx);
        sy0 = (int)jsd_floor(dsy);
        sx1 = sx0 + (int)jsd_floor(dsw);
        sy1 = sy0 + (int)jsd_floor(dsh);
        if (sx0 < 0) sx0 = 0;
        if (sy0 < 0) sy0 = 0;
        if (sx1 > iw) sx1 = iw;
        if (sy1 > ih) sy1 = ih;
    }
    if (cv && cv->px && cv->alpha) {
        dx = (int)jsd_floor(ddx);
        dy = (int)jsd_floor(ddy);
        for (iy = sy0; iy < sy1; iy++) {
            int cy = dy + iy;
            if (cy < 0 || cy >= cv->h)
                continue;
            for (ix = sx0; ix < sx1; ix++) {
                int cx = dx + ix;
                unsigned long si, di;

                if (cx < 0 || cx >= cv->w)
                    continue;
                si = ((unsigned long)iy * (unsigned long)iw + (unsigned long)ix) * 4ul;
                di = (unsigned long)cy * (unsigned long)cv->w + (unsigned long)cx;
                cv->px[di] = ((uint32_t)src[si] << 16) |
                             ((uint32_t)src[si + 1] << 8) |
                             (uint32_t)src[si + 2];
                cv->alpha[di] = src[si + 3];
            }
        }
        mark_dom_dirty(h->j);
    }
    *r = js_undefined();
    return JS_OK;
}

/* ------------------------------------------------------------------ */
/* DOMParser (text/html, image/svg+xml, text/xml)                     */

static void xml_decode_entities(const char *src, unsigned long len,
                                char *out, unsigned long outsz)
{
    unsigned long i = 0, used = 0;

    while (i < len && used + 1 < outsz) {
        if (src[i] == '&') {
            unsigned long j = i + 1;
            while (j < len && src[j] != ';' && j - i < 32 &&
                   src[j] != '<' && src[j] != '&')
                j++;
            if (j < len && src[j] == ';') {
                unsigned long elen = j - (i + 1);
                char ebuf[36];
                unsigned long cp = 0;
                int ok = 0;

                if (elen > 0 && elen < sizeof(ebuf)) {
                    memcpy(ebuf, src + i + 1, elen);
                    ebuf[elen] = 0;
                    if (!strcmp(ebuf, "apos")) { cp = '\''; ok = 1; }
                    else if (ebuf[0] == '#') {
                        const char *np = ebuf + 1;
                        int base = 10;
                        if (np[0] == 'x' || np[0] == 'X') { base = 16; np++; }
                        while (*np) {
                            unsigned d;
                            if (*np >= '0' && *np <= '9') d = (unsigned)(*np - '0');
                            else if (base == 16 && *np >= 'a' && *np <= 'f') d = (unsigned)(*np - 'a' + 10);
                            else if (base == 16 && *np >= 'A' && *np <= 'F') d = (unsigned)(*np - 'A' + 10);
                            else { ok = 0; break; }
                            cp = cp * (unsigned long)base + d;
                            ok = 1;
                            np++;
                        }
                    } else if (html_entity_lookup(ebuf, (long)elen, &cp)) {
                        ok = 1;
                    }
                    if (ok && cp > 0 && cp <= 0x10FFFFul) {
                        char u8[6];
                        int nbytes = dom_utf8_encode(cp, u8);
                        if (nbytes > 0 && used + (unsigned long)nbytes + 1 < outsz) {
                            memcpy(out + used, u8, (unsigned long)nbytes);
                            used += (unsigned long)nbytes;
                        }
                        i = j + 1;
                        continue;
                    }
                }
            }
        }
        out[used++] = src[i++];
    }
    out[used] = 0;
}

static struct dom_document *jsdom_parse_xml_document(const char *src,
                                                     unsigned long len,
                                                     const char *default_ns,
                                                     const struct dom_limits *limits)
{
    struct dom_document *d = dom_document_new_limits(limits);
    struct dom_node *stack[64];
    const char *ns_stack[64];
    int sp = 0;
    unsigned long i = 0;

    if (!d)
        return 0;
    stack[0] = d->root;
    ns_stack[0] = default_ns;
    while (i < len && !(d->truncated & DOM_TRUNC_CREDIT)) {
        if (src[i] == '<') {
            if (i + 4 <= len && !memcmp(src + i, "<!--", 4)) {
                unsigned long start = i + 4, end = start;
                struct dom_node *cm;

                while (end + 3 <= len && memcmp(src + end, "-->", 3) != 0)
                    end++;
                cm = dom_create_comment(d, src + start, end - start);
                if (cm) (void)dom_append_child(stack[sp], cm);
                i = (end + 3 <= len) ? end + 3 : len;
                continue;
            }
            if (i + 9 <= len && !memcmp(src + i, "<![CDATA[", 9)) {
                unsigned long start = i + 9, end = start;
                struct dom_node *tx;

                while (end + 3 <= len && memcmp(src + end, "]]>", 3) != 0)
                    end++;
                tx = dom_create_text(d, src + start, end - start);
                if (tx) (void)dom_append_child(stack[sp], tx);
                i = (end + 3 <= len) ? end + 3 : len;
                continue;
            }
            if (i + 2 <= len && (src[i + 1] == '?' || src[i + 1] == '!')) {
                i += 2;
                while (i < len && src[i] != '>') i++;
                if (i < len) i++;
                continue;
            }
            if (i + 2 <= len && src[i + 1] == '/') {
                unsigned long tstart = i + 2, tend;
                int k;

                while (tstart < len && (src[tstart] == ' ' || src[tstart] == '\t'))
                    tstart++;
                tend = tstart;
                while (tend < len && src[tend] != '>' &&
                       (unsigned char)src[tend] > ' ')
                    tend++;
                while (i < len && src[i] != '>') i++;
                if (i < len) i++;
                for (k = sp; k >= 1; k--) {
                    if (stack[k]->tag &&
                        ci_eq_n(stack[k]->tag, strlen(stack[k]->tag),
                                src + tstart, tend - tstart)) {
                        sp = k - 1;
                        break;
                    }
                }
                continue;
            }
            {
                unsigned long tstart = i + 1, tend = tstart;
                char tagbuf[129], decbuf[2048];
                struct dom_node *el;
                const char *cur_ns = ns_stack[sp];
                int self_close = 0;

                while (tend < len && (unsigned char)src[tend] > ' ' &&
                       src[tend] != '>' && src[tend] != '/')
                    tend++;
                if (tend == tstart) {
                    i++;
                    continue;
                }
                {
                    unsigned long tl = tend - tstart;
                    if (tl >= sizeof(tagbuf)) tl = sizeof(tagbuf) - 1;
                    memcpy(tagbuf, src + tstart, tl);
                    tagbuf[tl] = 0;
                    el = dom_create_element(d, tagbuf, (long)tl);
                    if (el)
                        (void)dom_set_namespace(el, cur_ns, -1);
                }
                i = tend;
                while (i < len && src[i] != '>') {
                    unsigned long astart, aend, vstart, vend;
                    char aname[129];

                    while (i < len && (unsigned char)src[i] <= ' ') i++;
                    if (i < len && src[i] == '/') {
                        self_close = 1;
                        i++;
                        while (i < len && (unsigned char)src[i] <= ' ') i++;
                        break;
                    }
                    if (i >= len || src[i] == '>')
                        break;
                    astart = i;
                    while (i < len && (unsigned char)src[i] > ' ' &&
                           src[i] != '=' && src[i] != '>' && src[i] != '/')
                        i++;
                    aend = i;
                    while (i < len && (unsigned char)src[i] <= ' ') i++;
                    vstart = vend = i;
                    if (i < len && src[i] == '=') {
                        i++;
                        while (i < len && (unsigned char)src[i] <= ' ') i++;
                        if (i < len && (src[i] == '"' || src[i] == '\'')) {
                            char q = src[i++];
                            vstart = i;
                            while (i < len && src[i] != q) i++;
                            vend = i;
                            if (i < len) i++;
                        } else {
                            vstart = i;
                            while (i < len && (unsigned char)src[i] > ' ' &&
                                   src[i] != '>')
                                i++;
                            vend = i;
                        }
                    }
                    if (el && aend > astart && aend - astart < sizeof(aname)) {
                        memcpy(aname, src + astart, aend - astart);
                        aname[aend - astart] = 0;
                        xml_decode_entities(src + vstart, vend - vstart,
                                            decbuf, sizeof(decbuf));
                        if (!strcmp(aname, "xmlns")) {
                            (void)dom_set_namespace(el, decbuf[0] ? decbuf : 0, -1);
                            cur_ns = el->namespace_uri;
                        }
                        (void)dom_set_attr(el, aname, decbuf);
                    }
                }
                if (i < len && src[i] == '>')
                    i++;
                if (el) {
                    (void)dom_append_child(stack[sp], el);
                    if (!self_close && sp + 1 < 64) {
                        sp++;
                        stack[sp] = el;
                        ns_stack[sp] = cur_ns;
                    }
                }
                continue;
            }
        }
        {
            unsigned long tstart = i;
            char *tbuf;
            struct dom_node *tn;

            while (i < len && src[i] != '<') i++;
            tbuf = (char *)malloc((i - tstart) + 1);
            if (tbuf) {
                xml_decode_entities(src + tstart, i - tstart,
                                    tbuf, (i - tstart) + 1);
                tn = dom_create_text(d, tbuf, strlen(tbuf));
                if (tn) (void)dom_append_child(stack[sp], tn);
                free(tbuf);
            }
        }
    }
    return d;
}

static int track_parsed_document(struct jsdom *j, struct dom_document *d)
{
    unsigned int ncap;
    struct dom_document **narr;

    if (!j || !d)
        return 0;
    if (j->parsed_doc_count >= j->parsed_doc_cap) {
        ncap = j->parsed_doc_cap ? j->parsed_doc_cap * 2u : 8u;
        narr = (struct dom_document **)realloc(j->parsed_docs,
                                               ncap * sizeof(*narr));
        if (!narr)
            return 0;
        j->parsed_docs = narr;
        j->parsed_doc_cap = ncap;
    }
    j->parsed_docs[j->parsed_doc_count++] = d;
    return 1;
}

static int dom_parser_constructor(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    js_object *obj;
    (void)t; (void)ac; (void)av;

    if (!j || !js_is_constructing(c))
        return js_throw_error(c, JS_ERR_TYPE,
                              "DOMParser must be constructed with new");
    obj = new_host(j, 0, HOST_DOM_PARSER, j->dom_parser_proto);
    if (!obj)
        return JS_THROW;
    *r = js_object_value(obj);
    return JS_OK;
}

static int dom_parser_parse_from_string(js_ctx *c, js_value t,
                                        int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_DOM_PARSER);
    const char *markup, *mime;
    struct dom_document *doc2 = 0;
    struct doc_ref *ref;
    js_object *proto, *docobj;

    if (!h)
        return js_throw_error(c, JS_ERR_TYPE, "illegal DOMParser receiver");
    if (ac < 2 || string_arg(c, ac, av, 0, &markup) != JS_OK ||
        string_arg(c, ac, av, 1, &mime) != JS_OK)
        return ac < 2
            ? js_throw_error(c, JS_ERR_TYPE,
                             "parseFromString requires string and type")
            : JS_THROW;
    const struct dom_limits limits = { .credit = &h->j->parsed_doc_credit };
    if (!strcmp(mime, "text/html")) {
        doc2 = html_parse_document_limits(markup, strlen(markup), &limits);
        proto = h->j->html_document_proto;
    } else if (!strcmp(mime, "image/svg+xml")) {
        doc2 = jsdom_parse_xml_document(markup, strlen(markup),
                                        DOM_NS_SVG_URI, &limits);
        proto = h->j->doc_proto;
    } else if (!strcmp(mime, "text/xml") ||
               !strcmp(mime, "application/xml")) {
        doc2 = jsdom_parse_xml_document(markup, strlen(markup), 0, &limits);
        proto = h->j->doc_proto;
    } else if (!strcmp(mime, "application/xhtml+xml")) {
        doc2 = jsdom_parse_xml_document(markup, strlen(markup),
                                        DOM_NS_HTML_URI, &limits);
        proto = h->j->doc_proto;
    } else {
        return js_throw_error(c, JS_ERR_TYPE,
                              "unsupported DOMParser MIME type");
    }
    if (!doc2 || (doc2->truncated & DOM_TRUNC_CREDIT)) {
        dom_document_free(doc2);
        return js_throw_error(c, JS_ERR_ERROR, "cannot parse document");
    }
    ref = (struct doc_ref *)calloc(1, sizeof(*ref));
    if (!ref || !track_parsed_document(h->j, doc2)) {
        free(ref);
        dom_document_free(doc2);
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    }
    ref->doc = doc2;
    snprintf(ref->content_type, sizeof(ref->content_type), "%s", mime);
    docobj = new_host_data(h->j, 0, HOST_DOCUMENT, ref, proto);
    if (!docobj) {
        h->j->parsed_doc_count--;
        dom_document_free(doc2);
        free(ref);
        return JS_THROW;
    }
    *r = js_object_value(docobj);
    return JS_OK;
}

/* ------------------------------------------------------------------ */
/* Geometry, Viewport, Scroll, Observers & Clipboard                  */

static int node_is_in_main_dom(const struct jsdom *j,
                               const struct dom_node *node)
{
    const struct dom_node *cur = node;

    if (!j || !j->doc || !j->doc->root)
        return 0;
    while (cur && cur != j->doc->root)
        cur = cur->parent;
    return cur == j->doc->root;
}

static int node_box_rect(struct jsdom *j, struct dom_node *node,
                         float *ox, float *oy, float *ow, float *oh)
{
    const char *ws, *hs;

    *ox = 0.0f; *oy = 0.0f; *ow = 0.0f; *oh = 0.0f;
    if (!j || !node || node->type != DOM_ELEMENT)
        return 0;
    if (j->get_box_rect && j->get_box_rect(j->print_user, node, ox, oy, ow, oh))
        return 1;
    if (node->tag_id == HTAG_CANVAS && node->canvas) {
        *ow = (float)node->canvas->w;
        *oh = (float)node->canvas->h;
        return 1;
    }
    ws = dom_get_attr(node, "width");
    hs = dom_get_attr(node, "height");
    if (ws || hs) {
        double dw = 0.0, dh = 0.0;
        if (ws && jsd_parse_double(ws, &dw, 0) && dw > 0.0) *ow = (float)dw;
        if (hs && jsd_parse_double(hs, &dh, 0) && dh > 0.0) *oh = (float)dh;
        return (*ow > 0.0f || *oh > 0.0f);
    }
    {
        char wbuf[64], hbuf[64];
        double dw = 0.0, dh = 0.0;
        const char *we, *he;

        computed_resolve_prop(j, node, "width", wbuf, sizeof(wbuf), 0);
        computed_resolve_prop(j, node, "height", hbuf, sizeof(hbuf), 0);
        if (jsd_parse_double(wbuf, &dw, &we) && !strcmp(we, "px") && dw > 0.0)
            *ow = (float)dw;
        if (jsd_parse_double(hbuf, &dh, &he) && !strcmp(he, "px") && dh > 0.0)
            *oh = (float)dh;
    }
    return 0;
}

static int make_dom_rect(js_ctx *c, double x, double y, double w, double h,
                         js_value *out)
{
    js_object *rect = js_new_object(c);
    js_value rv;

    if (!rect)
        return JS_THROW;
    rv = js_object_value(rect);
    if (js_set(c, rv, "x", js_number(x)) != JS_OK ||
        js_set(c, rv, "y", js_number(y)) != JS_OK ||
        js_set(c, rv, "width", js_number(w)) != JS_OK ||
        js_set(c, rv, "height", js_number(h)) != JS_OK ||
        js_set(c, rv, "top", js_number(y)) != JS_OK ||
        js_set(c, rv, "left", js_number(x)) != JS_OK ||
        js_set(c, rv, "right", js_number(x + w)) != JS_OK ||
        js_set(c, rv, "bottom", js_number(y + h)) != JS_OK)
        return JS_THROW;
    *out = rv;
    return JS_OK;
}

static int node_get_bounding_client_rect(js_ctx *c, js_value t,
                                         int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    float x, y, w, hb;
    (void)ac; (void)av;

    if (!h || h->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE, "illegal Element receiver");
    (void)node_box_rect(h->j, h->node, &x, &y, &w, &hb);
    return make_dom_rect(c, (double)x, (double)y, (double)w, (double)hb, r);
}

static int node_metric_get(js_ctx *c, js_value t, int ac, js_value *av,
                           js_value *r, int which)
{
    struct hostref *h = host(t, HOST_NODE);
    float x = 0, y = 0, w = 0, hb = 0;
    double val = 0.0;
    (void)c; (void)ac; (void)av;

    if (!h || h->node->type != DOM_ELEMENT) {
        *r = js_undefined();
        return JS_OK;
    }
    if (h->node == h->j->doc->html && (which == 0 || which == 1)) {
        *r = js_number(which == 0 ? (double)h->j->viewport_width
                                  : (double)h->j->viewport_height);
        return JS_OK;
    }
    (void)node_box_rect(h->j, h->node, &x, &y, &w, &hb);
    if (which == 0 || which == 2) val = (double)(int)(w + 0.5f);
    else if (which == 1 || which == 3) val = (double)(int)(hb + 0.5f);
    else if (which == 4) val = (double)(int)(x + 0.5f);
    else if (which == 5) val = (double)(int)(y + 0.5f);
    *r = js_number(val);
    return JS_OK;
}

#define NODE_METRIC_GETTER(fn, which)                                        \
static int fn(js_ctx *c, js_value t, int ac, js_value *av, js_value *r)      \
{ return node_metric_get(c, t, ac, av, r, which); }
NODE_METRIC_GETTER(node_client_width_get, 0)
NODE_METRIC_GETTER(node_client_height_get, 1)
NODE_METRIC_GETTER(node_offset_width_get, 2)
NODE_METRIC_GETTER(node_offset_height_get, 3)
NODE_METRIC_GETTER(node_offset_left_get, 4)
NODE_METRIC_GETTER(node_offset_top_get, 5)
#undef NODE_METRIC_GETTER

static int node_scroll_pos_get(js_ctx *c, js_value t, int ac, js_value *av,
                               js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    (void)c; (void)ac; (void)av;
    *r = h ? js_number(0) : js_undefined();
    return JS_OK;
}

static int node_scroll_pos_set(js_ctx *c, js_value t, int ac, js_value *av,
                               js_value *r)
{
    (void)c; (void)t; (void)ac; (void)av;
    *r = js_undefined();
    return JS_OK;
}

static int window_scroll_x_get(js_ctx *c, js_value t, int ac, js_value *av,
                               js_value *r)
{
    struct jsdom *j = binding(c);
    (void)t; (void)ac; (void)av;
    *r = js_number(j ? j->scroll_x : 0.0);
    return JS_OK;
}

static int window_scroll_y_get(js_ctx *c, js_value t, int ac, js_value *av,
                               js_value *r)
{
    struct jsdom *j = binding(c);
    (void)t; (void)ac; (void)av;
    *r = js_number(j ? j->scroll_y : 0.0);
    return JS_OK;
}

static int window_inner_w_get(js_ctx *c, js_value t, int ac, js_value *av,
                              js_value *r)
{
    struct jsdom *j = binding(c);
    (void)t; (void)ac; (void)av;
    *r = js_number(j ? (double)j->viewport_width : 0.0);
    return JS_OK;
}

static int window_inner_h_get(js_ctx *c, js_value t, int ac, js_value *av,
                              js_value *r)
{
    struct jsdom *j = binding(c);
    (void)t; (void)ac; (void)av;
    *r = js_number(j ? (double)j->viewport_height : 0.0);
    return JS_OK;
}

static int parse_scroll_args(js_ctx *c, int ac, js_value *av,
                             double def_x, double def_y,
                             double *ox, double *oy)
{
    double x = def_x, y = def_y;

    if (ac == 1 && js_is_object(av[0])) {
        js_value lx, ly;
        double d;

        if (js_get(c, av[0], "left", &lx) != JS_OK ||
            js_get(c, av[0], "top", &ly) != JS_OK)
            return JS_THROW;
        if (!js_is_undefined(lx)) {
            if (js_to_number(c, lx, &d) != JS_OK) return JS_THROW;
            if (jsd_isfinite(d)) x = d;
        }
        if (!js_is_undefined(ly)) {
            if (js_to_number(c, ly, &d) != JS_OK) return JS_THROW;
            if (jsd_isfinite(d)) y = d;
        }
    } else if (ac >= 1) {
        double d;
        if (js_to_number(c, av[0], &d) != JS_OK) return JS_THROW;
        if (jsd_isfinite(d)) x = d;
        if (ac >= 2) {
            if (js_to_number(c, av[1], &d) != JS_OK) return JS_THROW;
            if (jsd_isfinite(d)) y = d;
        }
    }
    *ox = x;
    *oy = y;
    return JS_OK;
}

static int window_scroll_apply(js_ctx *c, struct jsdom *j, double x, double y)
{
    js_value ev, global;
    char ebuf[128];

    if (x < 0.0) x = 0.0;
    if (y < 0.0) y = 0.0;
    j->scroll_x = x;
    j->scroll_y = y;
    if (j->scroll_cb)
        j->scroll_cb(j->print_user, x, y);
    if (event_make(j, j->event_proto, "scroll", js_undefined(),
                   js_undefined(), 0, &ev) != JS_OK)
        return JS_THROW;
    global = js_global(c);
    ebuf[0] = 0;
    if (dispatch_event_value(j, global, ev, 0, ebuf, sizeof(ebuf)) < 0)
        return JS_THROW;
    return JS_OK;
}

static int global_scroll_to(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    struct jsdom *j = binding(c);
    double x, y;
    (void)t;

    if (!j) return js_throw_error(c, JS_ERR_ERROR, "DOM binding unavailable");
    if (parse_scroll_args(c, ac, av, j->scroll_x, j->scroll_y, &x, &y) != JS_OK)
        return JS_THROW;
    if (window_scroll_apply(c, j, x, y) != JS_OK)
        return JS_THROW;
    *r = js_undefined();
    return JS_OK;
}

static int global_scroll_by(js_ctx *c, js_value t, int ac, js_value *av,
                            js_value *r)
{
    struct jsdom *j = binding(c);
    double dx, dy;
    (void)t;

    if (!j) return js_throw_error(c, JS_ERR_ERROR, "DOM binding unavailable");
    if (parse_scroll_args(c, ac, av, 0.0, 0.0, &dx, &dy) != JS_OK)
        return JS_THROW;
    if (window_scroll_apply(c, j, j->scroll_x + dx, j->scroll_y + dy) != JS_OK)
        return JS_THROW;
    *r = js_undefined();
    return JS_OK;
}

/* ------------------------------------------------------------------ */
/* ResizeObserver & IntersectionObserver                              */

static int resize_observer_constructor(js_ctx *c, js_value t,
                                       int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    struct resize_observer_state *st;
    js_object *obj;
    unsigned int slot;
    (void)t;

    if (!j || !js_is_constructing(c))
        return js_throw_error(c, JS_ERR_TYPE,
                              "ResizeObserver requires new");
    if (ac < 1 || !js_is_function(av[0]))
        return js_throw_error(c, JS_ERR_TYPE,
                              "ResizeObserver requires a callback");
    for (slot = 0; slot < JSDOM_OBSERVERS_MAX; slot++)
        if (!j->resize_observers[slot]) break;
    if (slot == JSDOM_OBSERVERS_MAX)
        return js_throw_error(c, JS_ERR_RANGE, "too many ResizeObservers");
    st = (struct resize_observer_state *)calloc(1, sizeof(*st));
    if (!st)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    st->callback = av[0];
    obj = new_host_data(j, 0, HOST_RESIZE_OBSERVER, st,
                        j->resize_observer_proto);
    if (!obj) {
        free(st);
        return JS_THROW;
    }
    st->self = js_object_value(obj);
    if (js_define(c, obj, "__callback", av[0]) != JS_OK)
        return JS_THROW;
    j->resize_observers[slot] = st;
    *r = st->self;
    return JS_OK;
}

static int resize_observer_observe(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_RESIZE_OBSERVER);
    struct hostref *target = ac ? host(av[0], HOST_NODE) : 0;
    struct resize_observer_state *st =
        h ? (struct resize_observer_state *)h->data : 0;
    unsigned int i;

    if (!st)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal ResizeObserver receiver");
    if (!target || target->j != h->j || target->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE,
                              "observe requires an Element target");
    for (i = 0; i < st->target_count; i++) {
        if (st->targets[i].node == target->node) {
            st->targets[i].pending = 1;
            *r = js_undefined();
            return JS_OK;
        }
    }
    if (st->target_count >= JSDOM_OBSERVER_TARGETS_MAX)
        return js_throw_error(c, JS_ERR_RANGE,
                              "too many observed targets");
    st->targets[st->target_count].node = target->node;
    st->targets[st->target_count].last_w = -1.0f;
    st->targets[st->target_count].last_h = -1.0f;
    st->targets[st->target_count].pending = 1;
    st->target_count++;
    *r = js_undefined();
    return JS_OK;
}

static int resize_observer_unobserve(js_ctx *c, js_value t,
                                     int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_RESIZE_OBSERVER);
    struct hostref *target = ac ? host(av[0], HOST_NODE) : 0;
    struct resize_observer_state *st =
        h ? (struct resize_observer_state *)h->data : 0;
    unsigned int i;

    if (!st)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal ResizeObserver receiver");
    if (target) {
        for (i = 0; i < st->target_count; i++) {
            if (st->targets[i].node == target->node) {
                memmove(&st->targets[i], &st->targets[i + 1],
                        (st->target_count - i - 1) * sizeof(st->targets[0]));
                st->target_count--;
                break;
            }
        }
    }
    *r = js_undefined();
    return JS_OK;
}

static int resize_observer_disconnect(js_ctx *c, js_value t,
                                      int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_RESIZE_OBSERVER);
    struct resize_observer_state *st =
        h ? (struct resize_observer_state *)h->data : 0;
    (void)ac; (void)av;

    if (!st)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal ResizeObserver receiver");
    st->target_count = 0;
    *r = js_undefined();
    return JS_OK;
}

static int intersection_observer_constructor(js_ctx *c, js_value t,
                                             int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    struct intersection_observer_state *st;
    js_object *obj;
    unsigned int slot;
    (void)t;

    if (!j || !js_is_constructing(c))
        return js_throw_error(c, JS_ERR_TYPE,
                              "IntersectionObserver requires new");
    if (ac < 1 || !js_is_function(av[0]))
        return js_throw_error(c, JS_ERR_TYPE,
                              "IntersectionObserver requires a callback");
    for (slot = 0; slot < JSDOM_OBSERVERS_MAX; slot++)
        if (!j->intersection_observers[slot]) break;
    if (slot == JSDOM_OBSERVERS_MAX)
        return js_throw_error(c, JS_ERR_RANGE,
                              "too many IntersectionObservers");
    st = (struct intersection_observer_state *)calloc(1, sizeof(*st));
    if (!st)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    st->callback = av[0];
    if (ac > 1 && js_is_object(av[1])) {
        js_value rv;
        if (js_get(c, av[1], "root", &rv) == JS_OK) {
            struct hostref *rh = host(rv, HOST_NODE);
            if (rh && rh->j == j && rh->node->type == DOM_ELEMENT)
                st->root = rh->node;
        }
    }
    obj = new_host_data(j, 0, HOST_INTERSECTION_OBSERVER, st,
                        j->intersection_observer_proto);
    if (!obj) {
        free(st);
        return JS_THROW;
    }
    st->self = js_object_value(obj);
    if (js_define(c, obj, "__callback", av[0]) != JS_OK)
        return JS_THROW;
    j->intersection_observers[slot] = st;
    *r = st->self;
    return JS_OK;
}

static int intersection_observer_observe(js_ctx *c, js_value t,
                                         int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_INTERSECTION_OBSERVER);
    struct hostref *target = ac ? host(av[0], HOST_NODE) : 0;
    struct intersection_observer_state *st =
        h ? (struct intersection_observer_state *)h->data : 0;
    unsigned int i;

    if (!st)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal IntersectionObserver receiver");
    if (!target || target->j != h->j || target->node->type != DOM_ELEMENT)
        return js_throw_error(c, JS_ERR_TYPE,
                              "observe requires an Element target");
    for (i = 0; i < st->target_count; i++) {
        if (st->targets[i].node == target->node) {
            st->targets[i].pending = 1;
            *r = js_undefined();
            return JS_OK;
        }
    }
    if (st->target_count >= JSDOM_OBSERVER_TARGETS_MAX)
        return js_throw_error(c, JS_ERR_RANGE,
                              "too many observed targets");
    st->targets[st->target_count].node = target->node;
    st->targets[st->target_count].pending = 1;
    st->target_count++;
    *r = js_undefined();
    return JS_OK;
}

static int intersection_observer_unobserve(js_ctx *c, js_value t,
                                           int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_INTERSECTION_OBSERVER);
    struct hostref *target = ac ? host(av[0], HOST_NODE) : 0;
    struct intersection_observer_state *st =
        h ? (struct intersection_observer_state *)h->data : 0;
    unsigned int i;

    if (!st)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal IntersectionObserver receiver");
    if (target) {
        for (i = 0; i < st->target_count; i++) {
            if (st->targets[i].node == target->node) {
                memmove(&st->targets[i], &st->targets[i + 1],
                        (st->target_count - i - 1) * sizeof(st->targets[0]));
                st->target_count--;
                break;
            }
        }
    }
    *r = js_undefined();
    return JS_OK;
}

static int intersection_observer_disconnect(js_ctx *c, js_value t,
                                            int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_INTERSECTION_OBSERVER);
    struct intersection_observer_state *st =
        h ? (struct intersection_observer_state *)h->data : 0;
    (void)ac; (void)av;

    if (!st)
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal IntersectionObserver receiver");
    st->target_count = 0;
    *r = js_undefined();
    return JS_OK;
}

static int intersection_observer_take_records(js_ctx *c, js_value t,
                                              int ac, js_value *av, js_value *r)
{
    js_object *arr;
    (void)ac; (void)av;
    if (!host(t, HOST_INTERSECTION_OBSERVER))
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal IntersectionObserver receiver");
    arr = js_new_array(c);
    if (!arr) return JS_THROW;
    *r = js_object_value(arr);
    return JS_OK;
}

/* ------------------------------------------------------------------ */
/* navigator.clipboard                                                */

static int navigator_clipboard_write_text(js_ctx *c, js_value t,
                                          int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    const char *text;
    unsigned long len;
    js_value promise;
    char *copy;
    (void)t;

    if (!j)
        return js_throw_error(c, JS_ERR_ERROR, "DOM binding unavailable");
    promise = js_promise_new(c);
    if (!js_is_promise(promise))
        return JS_THROW;
    if (string_arg(c, ac, av, 0, &text) != JS_OK)
        return JS_THROW;
    len = strlen(text);
    copy = (char *)malloc(len + 1);
    if (!copy) {
        (void)js_promise_reject(c, promise, js_mkcstring(c, "out of memory"));
        *r = promise;
        return JS_OK;
    }
    memcpy(copy, text, len + 1);
    free(j->clipboard_text);
    j->clipboard_text = copy;
    j->clipboard_len = len;
    if (j->clipboard_write && j->clipboard_write(j->print_user, copy, len) != 0) {
        (void)js_promise_reject(c, promise,
                                js_mkcstring(c, "clipboard write failed"));
        *r = promise;
        return JS_OK;
    }
    if (js_promise_resolve(c, promise, js_undefined()) != JS_OK)
        return JS_THROW;
    *r = promise;
    return JS_OK;
}

static int navigator_clipboard_read_text(js_ctx *c, js_value t,
                                         int ac, js_value *av, js_value *r)
{
    struct jsdom *j = binding(c);
    js_value promise, str_val;
    (void)t; (void)ac; (void)av;

    if (!j)
        return js_throw_error(c, JS_ERR_ERROR, "DOM binding unavailable");
    promise = js_promise_new(c);
    if (!js_is_promise(promise))
        return JS_THROW;
    if (j->clipboard_read) {
        unsigned long len = 0;
        char *buf = j->clipboard_read(j->print_user, &len);
        if (buf) {
            str_val = js_mkstring(c, buf, len);
            free(buf);
            if (js_fatal(c) || js_promise_resolve(c, promise, str_val) != JS_OK)
                return JS_THROW;
            *r = promise;
            return JS_OK;
        }
    }
    str_val = js_mkstring(c, j->clipboard_text ? j->clipboard_text : "",
                          j->clipboard_text ? j->clipboard_len : 0ul);
    if (js_fatal(c) || js_promise_resolve(c, promise, str_val) != JS_OK)
        return JS_THROW;
    *r = promise;
    return JS_OK;
}

static int define_node_proto(struct jsdom *j)
{
    js_ctx *c = j->ctx;
    js_object *p = j->node_proto;

#define ACC(name, get, set) if (js_define_accessor(c, p, name, get, set, 1) != JS_OK) return 0
#define FUN(name, fn, n) if (js_define_native(c, p, name, fn, n) != JS_OK) return 0
    ACC("nodeType", node_type_get, 0);
    ACC("tagName", node_tag_get, 0);
    ACC("nodeName", node_dom_name_get, 0);
    ACC("localName", node_local_name_get, 0);
    ACC("namespaceURI", node_namespace_get, 0);
    ACC("prefix", node_prefix_get, 0);
    ACC("nodeValue", node_node_value_get, node_node_value_set);
    ACC("ownerDocument", node_owner_document_get, 0);
    ACC("textContent", node_text_get, node_text_set);
    ACC("innerText", node_text_get, node_text_set);
    ACC("innerHTML", node_html_get, node_html_set);
    ACC("outerHTML", node_outer_get, node_outer_set);
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
    ACC("firstElementChild", node_first_element_get, 0);
    ACC("lastElementChild", node_last_element_get, 0);
    ACC("nextElementSibling", node_next_element_get, 0);
    ACC("previousElementSibling", node_prev_element_get, 0);
    ACC("childElementCount", node_child_element_count_get, 0);
    ACC("isConnected", node_is_connected_get, 0);
    ACC("children", node_children_get, 0);
    ACC("childNodes", node_child_nodes_get, 0);
    ACC("attributes", node_attributes_get, 0);
    ACC("dataset", node_dataset_get, 0);
    ACC("classList", node_classlist_get, 0);
    ACC("style", node_style_get, 0);
    ACC("clientWidth", node_client_width_get, 0);
    ACC("clientHeight", node_client_height_get, 0);
    ACC("offsetWidth", node_offset_width_get, 0);
    ACC("offsetHeight", node_offset_height_get, 0);
    ACC("offsetLeft", node_offset_left_get, 0);
    ACC("offsetTop", node_offset_top_get, 0);
    ACC("scrollWidth", node_client_width_get, 0);
    ACC("scrollHeight", node_client_height_get, 0);
    ACC("scrollLeft", node_scroll_pos_get, node_scroll_pos_set);
    ACC("scrollTop", node_scroll_pos_get, node_scroll_pos_set);
    FUN("getBoundingClientRect", node_get_bounding_client_rect, 0);
    FUN("getAttribute", node_get_attr, 1);
    FUN("getAttributeNode", node_get_attribute_node, 1);
    FUN("setAttribute", node_set_attr, 2);
    FUN("removeAttribute", node_remove_attr, 1);
    FUN("hasAttribute", node_has_attr, 1);
    FUN("getAttributeNames", node_get_attribute_names, 0);
    FUN("hasAttributes", node_has_attributes, 0);
    FUN("toggleAttribute", node_toggle_attr, 2);
    FUN("appendChild", node_append, 1);
    FUN("insertBefore", node_insert_before, 2);
    FUN("replaceChild", node_replace_child, 2);
    FUN("removeChild", node_remove, 1);
    FUN("append", node_append_many, 1);
    FUN("prepend", node_prepend_many, 1);
    FUN("replaceChildren", node_replace_children, 1);
    FUN("remove", node_self_remove, 0);
    FUN("before", node_before, 1);
    FUN("after", node_after, 1);
    FUN("replaceWith", node_replace_with, 1);
    FUN("insertAdjacentElement", node_insert_adjacent_element, 2);
    FUN("insertAdjacentText", node_insert_adjacent_text, 2);
    FUN("insertAdjacentHTML", node_insert_adjacent_html, 2);
    FUN("cloneNode", node_clone, 1);
    FUN("contains", node_contains, 1);
    FUN("isSameNode", node_is_same, 1);
    FUN("isEqualNode", node_is_equal, 1);
    FUN("compareDocumentPosition", node_compare_position, 1);
    FUN("normalize", node_normalize, 0);
    FUN("hasChildNodes", node_has_child_nodes, 0);
    FUN("getRootNode", node_get_root, 0);
    FUN("matches", node_matches, 1);
    FUN("closest", node_closest, 1);
    FUN("querySelector", node_query, 1);
    FUN("querySelectorAll", node_query_all, 1);
    FUN("getElementsByTagName", node_get_by_tag, 1);
    FUN("getElementsByTagNameNS", node_get_by_tag_ns, 2);
    FUN("getElementsByClassName", node_get_by_class, 1);
    FUN("addEventListener", node_add_listener, 2);
    FUN("click", node_click, 0);
#undef FUN
#undef ACC
    return 1;
}

static int illegal_dom_constructor(js_ctx *c, js_value t,
                                   int ac, js_value *av, js_value *r)
{
    (void)t; (void)ac; (void)av; (void)r;
    return js_throw_error(c, JS_ERR_TYPE, "Illegal constructor");
}

static int define_node_constants(js_ctx *c, js_object *object)
{
#define NODE_CONSTANT(name, value)                                          \
    if (js_define(c, object, name, js_number(value)) != JS_OK) return 0
    NODE_CONSTANT("ELEMENT_NODE", 1);
    NODE_CONSTANT("ATTRIBUTE_NODE", 2);
    NODE_CONSTANT("TEXT_NODE", 3);
    NODE_CONSTANT("CDATA_SECTION_NODE", 4);
    NODE_CONSTANT("ENTITY_REFERENCE_NODE", 5);
    NODE_CONSTANT("ENTITY_NODE", 6);
    NODE_CONSTANT("PROCESSING_INSTRUCTION_NODE", 7);
    NODE_CONSTANT("COMMENT_NODE", 8);
    NODE_CONSTANT("DOCUMENT_NODE", 9);
    NODE_CONSTANT("DOCUMENT_TYPE_NODE", 10);
    NODE_CONSTANT("DOCUMENT_FRAGMENT_NODE", 11);
    NODE_CONSTANT("NOTATION_NODE", 12);
    NODE_CONSTANT("DOCUMENT_POSITION_DISCONNECTED", 1);
    NODE_CONSTANT("DOCUMENT_POSITION_PRECEDING", 2);
    NODE_CONSTANT("DOCUMENT_POSITION_FOLLOWING", 4);
    NODE_CONSTANT("DOCUMENT_POSITION_CONTAINS", 8);
    NODE_CONSTANT("DOCUMENT_POSITION_CONTAINED_BY", 16);
    NODE_CONSTANT("DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC", 32);
#undef NODE_CONSTANT
    return 1;
}

static int define_dom_exception_constants(js_ctx *c, js_object *object)
{
#define DOM_EXCEPTION_CONSTANT(name, value)                                 \
    if (js_define(c, object, name, js_number(value)) != JS_OK) return 0
    DOM_EXCEPTION_CONSTANT("INDEX_SIZE_ERR", 1);
    DOM_EXCEPTION_CONSTANT("DOMSTRING_SIZE_ERR", 2);
    DOM_EXCEPTION_CONSTANT("HIERARCHY_REQUEST_ERR", 3);
    DOM_EXCEPTION_CONSTANT("WRONG_DOCUMENT_ERR", 4);
    DOM_EXCEPTION_CONSTANT("INVALID_CHARACTER_ERR", 5);
    DOM_EXCEPTION_CONSTANT("NO_DATA_ALLOWED_ERR", 6);
    DOM_EXCEPTION_CONSTANT("NO_MODIFICATION_ALLOWED_ERR", 7);
    DOM_EXCEPTION_CONSTANT("NOT_FOUND_ERR", 8);
    DOM_EXCEPTION_CONSTANT("NOT_SUPPORTED_ERR", 9);
    DOM_EXCEPTION_CONSTANT("INUSE_ATTRIBUTE_ERR", 10);
    DOM_EXCEPTION_CONSTANT("INVALID_STATE_ERR", 11);
    DOM_EXCEPTION_CONSTANT("SYNTAX_ERR", 12);
    DOM_EXCEPTION_CONSTANT("INVALID_MODIFICATION_ERR", 13);
    DOM_EXCEPTION_CONSTANT("NAMESPACE_ERR", 14);
    DOM_EXCEPTION_CONSTANT("INVALID_ACCESS_ERR", 15);
    DOM_EXCEPTION_CONSTANT("VALIDATION_ERR", 16);
    DOM_EXCEPTION_CONSTANT("TYPE_MISMATCH_ERR", 17);
    DOM_EXCEPTION_CONSTANT("SECURITY_ERR", 18);
    DOM_EXCEPTION_CONSTANT("NETWORK_ERR", 19);
    DOM_EXCEPTION_CONSTANT("ABORT_ERR", 20);
    DOM_EXCEPTION_CONSTANT("URL_MISMATCH_ERR", 21);
    DOM_EXCEPTION_CONSTANT("QUOTA_EXCEEDED_ERR", 22);
    DOM_EXCEPTION_CONSTANT("TIMEOUT_ERR", 23);
    DOM_EXCEPTION_CONSTANT("INVALID_NODE_TYPE_ERR", 24);
    DOM_EXCEPTION_CONSTANT("DATA_CLONE_ERR", 25);
#undef DOM_EXCEPTION_CONSTANT
    return 1;
}

static int define_node_filter_constants(js_ctx *c, js_object *object)
{
#define NODE_FILTER_CONSTANT(name, value)                                   \
    if (js_define(c, object, name, js_number(value)) != JS_OK) return 0
    NODE_FILTER_CONSTANT("FILTER_ACCEPT", 1);
    NODE_FILTER_CONSTANT("FILTER_REJECT", 2);
    NODE_FILTER_CONSTANT("FILTER_SKIP", 3);
    NODE_FILTER_CONSTANT("SHOW_ALL", 4294967295.0);
    NODE_FILTER_CONSTANT("SHOW_ELEMENT", 1);
    NODE_FILTER_CONSTANT("SHOW_ATTRIBUTE", 2);
    NODE_FILTER_CONSTANT("SHOW_TEXT", 4);
    NODE_FILTER_CONSTANT("SHOW_CDATA_SECTION", 8);
    NODE_FILTER_CONSTANT("SHOW_ENTITY_REFERENCE", 16);
    NODE_FILTER_CONSTANT("SHOW_ENTITY", 32);
    NODE_FILTER_CONSTANT("SHOW_PROCESSING_INSTRUCTION", 64);
    NODE_FILTER_CONSTANT("SHOW_COMMENT", 128);
    NODE_FILTER_CONSTANT("SHOW_DOCUMENT", 256);
    NODE_FILTER_CONSTANT("SHOW_DOCUMENT_TYPE", 512);
    NODE_FILTER_CONSTANT("SHOW_DOCUMENT_FRAGMENT", 1024);
    NODE_FILTER_CONSTANT("SHOW_NOTATION", 2048);
#undef NODE_FILTER_CONSTANT
    return 1;
}

static int define_event_constants(js_ctx *c, js_object *object)
{
#define EVENT_CONSTANT(name, value)                                         \
    if (js_define(c, object, name, js_number(value)) != JS_OK) return 0
    EVENT_CONSTANT("NONE", 0);
    EVENT_CONSTANT("CAPTURING_PHASE", 1);
    EVENT_CONSTANT("AT_TARGET", 2);
    EVENT_CONSTANT("BUBBLING_PHASE", 3);
#undef EVENT_CONSTANT
    return 1;
}

static int define_keyboard_event_constants(js_ctx *c, js_object *object)
{
#define KEYBOARD_CONSTANT(name, value)                                      \
    if (js_define(c, object, name, js_number(value)) != JS_OK) return 0
    KEYBOARD_CONSTANT("DOM_KEY_LOCATION_STANDARD", 0);
    KEYBOARD_CONSTANT("DOM_KEY_LOCATION_LEFT", 1);
    KEYBOARD_CONSTANT("DOM_KEY_LOCATION_RIGHT", 2);
    KEYBOARD_CONSTANT("DOM_KEY_LOCATION_NUMPAD", 3);
#undef KEYBOARD_CONSTANT
    return 1;
}

static int define_range_constants(js_ctx *c, js_object *object)
{
#define RANGE_CONSTANT(name, value)                                         \
    if (js_define(c, object, name, js_number(value)) != JS_OK) return 0
    RANGE_CONSTANT("START_TO_START", 0);
    RANGE_CONSTANT("START_TO_END", 1);
    RANGE_CONSTANT("END_TO_END", 2);
    RANGE_CONSTANT("END_TO_START", 3);
#undef RANGE_CONSTANT
    return 1;
}

static int character_data_length_get(js_ctx *c, js_value t,
                                     int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    (void)ac; (void)av;

    if (!h || (h->node->type != DOM_TEXT &&
               h->node->type != DOM_COMMENT))
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal CharacterData receiver");
    *r = js_number(h->node->text_len);
    return JS_OK;
}

static int character_data_bounds(js_ctx *c, struct hostref *h,
                                 int ac, js_value *av,
                                 unsigned long *offset,
                                 unsigned long *count)
{
    double offset_number, count_number;

    if (js_to_integer(c, ac > 0 ? av[0] : js_undefined(),
                      &offset_number) != JS_OK ||
        js_to_integer(c, ac > 1 ? av[1] : js_undefined(),
                      &count_number) != JS_OK)
        return JS_THROW;
    if (offset_number < 0 ||
        offset_number > (double)h->node->text_len ||
        count_number < 0)
        return throw_dom_exception(c, "IndexSizeError",
                                   "CharacterData offset is outside the data");
    *offset = (unsigned long)offset_number;
    *count = (unsigned long)count_number;
    if (*count > h->node->text_len - *offset)
        *count = h->node->text_len - *offset;
    return JS_OK;
}

static int character_data_rewrite(js_ctx *c, js_value t,
                                  unsigned long offset,
                                  unsigned long count,
                                  const char *replacement,
                                  unsigned long replacement_length,
                                  js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    unsigned long suffix, output_length;
    char *output;
    js_value argument;
    int result;

    if (!h || (h->node->type != DOM_TEXT &&
               h->node->type != DOM_COMMENT))
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal CharacterData receiver");
    suffix = h->node->text_len - offset - count;
    if (replacement_length > DOM_MAX_TEXT - offset ||
        suffix > DOM_MAX_TEXT - offset - replacement_length)
        return js_throw_error(c, JS_ERR_RANGE,
                              "CharacterData is too large");
    output_length = offset + replacement_length + suffix;
    output = (char *)malloc(output_length + 1);
    if (!output)
        return js_throw_error(c, JS_ERR_ERROR, "out of memory");
    memcpy(output, h->node->text, offset);
    memcpy(output + offset, replacement, replacement_length);
    memcpy(output + offset + replacement_length,
           h->node->text + offset + count, suffix);
    output[output_length] = 0;
    argument = js_mkstring(c, output, output_length);
    free(output);
    if (js_fatal(c))
        return JS_THROW;
    result = node_node_value_set(c, t, 1, &argument, r);
    return result;
}

static int character_data_substring(js_ctx *c, js_value t,
                                    int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    unsigned long offset, count;

    if (!h || (h->node->type != DOM_TEXT &&
               h->node->type != DOM_COMMENT))
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal CharacterData receiver");
    if (character_data_bounds(c, h, ac, av, &offset, &count) != JS_OK)
        return JS_THROW;
    *r = js_mkstring(c, h->node->text + offset, count);
    return js_fatal(c) ? JS_THROW : JS_OK;
}

static int character_data_append(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    struct hostref *h = host(t, HOST_NODE);
    js_value string;
    const char *replacement;
    unsigned long replacement_length;

    if (!h || (h->node->type != DOM_TEXT &&
               h->node->type != DOM_COMMENT))
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal CharacterData receiver");
    if (js_to_string(c, ac ? av[0] : js_undefined(), &string) != JS_OK)
        return JS_THROW;
    replacement = js_string_bytes(string, &replacement_length);
    return character_data_rewrite(c, t, h->node->text_len, 0,
                                  replacement, replacement_length, r);
}

static int character_data_edit(js_ctx *c, js_value t,
                               int ac, js_value *av, js_value *r,
                               int mode)
{
    struct hostref *h = host(t, HOST_NODE);
    unsigned long offset, count;
    js_value string;
    const char *replacement = "";
    unsigned long replacement_length = 0;

    if (!h || (h->node->type != DOM_TEXT &&
               h->node->type != DOM_COMMENT))
        return js_throw_error(c, JS_ERR_TYPE,
                              "illegal CharacterData receiver");
    if (character_data_bounds(c, h, ac, av, &offset, &count) != JS_OK)
        return JS_THROW;
    if (mode != 0) {
        if (js_to_string(c, ac > 2 ? av[2] : js_undefined(),
                         &string) != JS_OK)
            return JS_THROW;
        replacement = js_string_bytes(string, &replacement_length);
    }
    if (mode == 2)
        count = 0;
    return character_data_rewrite(c, t, offset, count,
                                  replacement, replacement_length, r);
}

static int character_data_delete(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    return character_data_edit(c, t, ac, av, r, 0);
}

static int character_data_replace(js_ctx *c, js_value t,
                                  int ac, js_value *av, js_value *r)
{
    return character_data_edit(c, t, ac, av, r, 1);
}

static int character_data_insert(js_ctx *c, js_value t,
                                 int ac, js_value *av, js_value *r)
{
    js_value arguments[3];

    arguments[0] = ac > 0 ? av[0] : js_undefined();
    arguments[1] = js_number(0);
    arguments[2] = ac > 1 ? av[1] : js_undefined();
    return character_data_edit(c, t, 3, arguments, r, 2);
}

static int define_other_protos(struct jsdom *j)
{
    js_ctx *c = j->ctx;

#define ACC(p, name, get, set) if (js_define_accessor(c, p, name, get, set, 1) != JS_OK) return 0
#define FUN(p, name, fn, n) if (js_define_native(c, p, name, fn, n) != JS_OK) return 0
    ACC(j->attr_proto, "name", attr_name_get, 0);
    ACC(j->attr_proto, "nodeName", attr_name_get, 0);
    ACC(j->attr_proto, "localName", attr_local_name_get, 0);
    ACC(j->attr_proto, "value", attr_value_get, attr_value_set);
    ACC(j->attr_proto, "nodeValue", attr_value_get, attr_value_set);
    ACC(j->attr_proto, "textContent", attr_value_get, attr_value_set);
    ACC(j->attr_proto, "ownerElement", attr_owner_get, 0);
    ACC(j->attr_proto, "ownerDocument", attr_owner_document_get, 0);
    ACC(j->attr_proto, "specified", attr_specified_get, 0);
    ACC(j->attr_proto, "nodeType", attr_type_get, 0);
    ACC(j->attr_proto, "namespaceURI", attr_namespace_get, 0);
    ACC(j->attr_proto, "prefix", attr_prefix_get, 0);
    ACC(j->named_node_map_proto, "length",
        named_node_map_length_get, 0);
    FUN(j->named_node_map_proto, "item", named_node_map_item, 1);
    FUN(j->named_node_map_proto, "getNamedItem",
        named_node_map_get, 1);
    FUN(j->named_node_map_proto, "getNamedItemNS",
        named_node_map_get_ns, 2);
    ACC(j->node_iterator_proto, "root", traversal_root_get, 0);
    ACC(j->node_iterator_proto, "whatToShow", traversal_show_get, 0);
    ACC(j->node_iterator_proto, "filter", traversal_filter_get, 0);
    ACC(j->node_iterator_proto, "referenceNode",
        node_iterator_reference_get, 0);
    ACC(j->node_iterator_proto, "pointerBeforeReferenceNode",
        node_iterator_before_get, 0);
    FUN(j->node_iterator_proto, "nextNode",
        node_iterator_next, 0);
    FUN(j->node_iterator_proto, "previousNode",
        node_iterator_previous, 0);
    FUN(j->node_iterator_proto, "detach",
        node_iterator_detach, 0);
    ACC(j->tree_walker_proto, "root", traversal_root_get, 0);
    ACC(j->tree_walker_proto, "whatToShow", traversal_show_get, 0);
    ACC(j->tree_walker_proto, "filter", traversal_filter_get, 0);
    ACC(j->tree_walker_proto, "currentNode",
        tree_walker_current_get, tree_walker_current_set);
    FUN(j->tree_walker_proto, "parentNode",
        tree_walker_parent, 0);
    FUN(j->tree_walker_proto, "firstChild",
        tree_walker_first_child, 0);
    FUN(j->tree_walker_proto, "lastChild",
        tree_walker_last_child, 0);
    FUN(j->tree_walker_proto, "previousSibling",
        tree_walker_previous_sibling, 0);
    FUN(j->tree_walker_proto, "nextSibling",
        tree_walker_next_sibling, 0);
    FUN(j->tree_walker_proto, "previousNode",
        tree_walker_previous, 0);
    FUN(j->tree_walker_proto, "nextNode",
        tree_walker_next, 0);
    ACC(j->range_proto, "startContainer",
        range_start_container_get, 0);
    ACC(j->range_proto, "startOffset",
        range_start_offset_get, 0);
    ACC(j->range_proto, "endContainer",
        range_end_container_get, 0);
    ACC(j->range_proto, "endOffset",
        range_end_offset_get, 0);
    ACC(j->range_proto, "collapsed", range_collapsed_get, 0);
    ACC(j->range_proto, "commonAncestorContainer",
        range_common_get, 0);
    FUN(j->range_proto, "setStart", range_set_start, 2);
    FUN(j->range_proto, "setEnd", range_set_end, 2);
    FUN(j->range_proto, "setStartBefore", range_set_start_before, 1);
    FUN(j->range_proto, "setStartAfter", range_set_start_after, 1);
    FUN(j->range_proto, "setEndBefore", range_set_end_before, 1);
    FUN(j->range_proto, "setEndAfter", range_set_end_after, 1);
    FUN(j->range_proto, "selectNode", range_select_node, 1);
    FUN(j->range_proto, "selectNodeContents",
        range_select_contents, 1);
    FUN(j->range_proto, "collapse", range_collapse, 1);
    FUN(j->range_proto, "cloneRange", range_clone, 0);
    FUN(j->range_proto, "cloneContents", range_clone_contents, 0);
    FUN(j->range_proto, "extractContents",
        range_extract_contents, 0);
    FUN(j->range_proto, "deleteContents",
        range_delete_contents, 0);
    FUN(j->range_proto, "insertNode", range_insert_node, 1);
    FUN(j->range_proto, "createContextualFragment",
        range_create_contextual_fragment, 1);
    FUN(j->range_proto, "comparePoint", range_compare_point, 2);
    FUN(j->range_proto, "isPointInRange", range_is_point_in, 2);
    FUN(j->range_proto, "compareBoundaryPoints",
        range_compare_boundaries, 2);
    FUN(j->range_proto, "intersectsNode",
        range_intersects_node, 1);
    FUN(j->range_proto, "surroundContents",
        range_surround_contents, 1);
    FUN(j->range_proto, "detach", range_detach, 0);
    FUN(j->range_proto, "toString", range_to_string, 0);
    ACC(j->selection_proto, "anchorNode",
        selection_anchor_node_get, 0);
    ACC(j->selection_proto, "anchorOffset",
        selection_anchor_offset_get, 0);
    ACC(j->selection_proto, "focusNode",
        selection_focus_node_get, 0);
    ACC(j->selection_proto, "focusOffset",
        selection_focus_offset_get, 0);
    ACC(j->selection_proto, "isCollapsed",
        selection_collapsed_get, 0);
    ACC(j->selection_proto, "rangeCount",
        selection_range_count_get, 0);
    ACC(j->selection_proto, "type", selection_type_get, 0);
    ACC(j->selection_proto, "direction",
        selection_direction_get, 0);
    FUN(j->selection_proto, "getRangeAt",
        selection_get_range_at, 1);
    FUN(j->selection_proto, "addRange", selection_add_range, 1);
    FUN(j->selection_proto, "removeRange",
        selection_remove_range, 1);
    FUN(j->selection_proto, "removeAllRanges",
        selection_remove_all, 0);
    FUN(j->selection_proto, "empty", selection_remove_all, 0);
    FUN(j->selection_proto, "collapse", selection_collapse, 2);
    FUN(j->selection_proto, "setPosition", selection_collapse, 2);
    FUN(j->selection_proto, "collapseToStart",
        selection_collapse_to_start, 0);
    FUN(j->selection_proto, "collapseToEnd",
        selection_collapse_to_end, 0);
    FUN(j->selection_proto, "selectAllChildren",
        selection_select_all, 1);
    FUN(j->selection_proto, "deleteFromDocument",
        selection_delete_from_document, 0);
    FUN(j->selection_proto, "containsNode",
        selection_contains_node, 2);
    FUN(j->selection_proto, "toString", selection_to_string, 0);
    FUN(j->dom_exception_proto, "toString",
        dom_exception_to_string, 0);
    ACC(j->character_data_proto, "data",
        node_node_value_get, node_node_value_set);
    ACC(j->character_data_proto, "length",
        character_data_length_get, 0);
    FUN(j->character_data_proto, "substringData",
        character_data_substring, 2);
    FUN(j->character_data_proto, "appendData",
        character_data_append, 1);
    FUN(j->character_data_proto, "insertData",
        character_data_insert, 2);
    FUN(j->character_data_proto, "deleteData",
        character_data_delete, 2);
    FUN(j->character_data_proto, "replaceData",
        character_data_replace, 3);
    FUN(j->event_target_proto, "addEventListener", node_add_listener, 2);
    FUN(j->event_target_proto, "removeEventListener",
        event_remove_listener, 2);
    FUN(j->event_target_proto, "dispatchEvent", event_target_dispatch, 1);
    FUN(j->media_query_proto, "addListener",
        media_query_add_listener, 1);
    FUN(j->media_query_proto, "removeListener",
        media_query_remove_listener, 1);
    ACC(j->history_proto, "length", history_length_get, 0);
    ACC(j->history_proto, "state", history_state_get, 0);
    ACC(j->history_proto, "scrollRestoration",
        history_scroll_get, history_scroll_set);
    FUN(j->history_proto, "pushState", history_push_state, 3);
    FUN(j->history_proto, "replaceState", history_replace_state, 3);
    FUN(j->history_proto, "go", history_go, 1);
    FUN(j->history_proto, "back", history_back, 0);
    FUN(j->history_proto, "forward", history_forward, 0);
    FUN(j->mutation_observer_proto, "observe",
        mutation_observer_observe, 2);
    FUN(j->mutation_observer_proto, "disconnect",
        mutation_observer_disconnect, 0);
    FUN(j->mutation_observer_proto, "takeRecords",
        mutation_observer_take_records, 0);
    FUN(j->event_proto, "preventDefault", event_prevent, 0);
    FUN(j->event_proto, "stopPropagation", event_stop, 0);
    FUN(j->event_proto, "stopImmediatePropagation",
        event_stop_immediate, 0);
    FUN(j->event_proto, "composedPath", event_composed_path, 0);
    ACC(j->event_proto, "cancelBubble",
        event_cancel_bubble_get, event_cancel_bubble_set);
    ACC(j->event_proto, "returnValue",
        event_return_value_get, event_return_value_set);
    ACC(j->event_proto, "srcElement", event_src_element_get, 0);
    FUN(j->mouse_event_proto, "getModifierState",
        event_get_modifier_state, 1);
    FUN(j->keyboard_event_proto, "getModifierState",
        event_get_modifier_state, 1);

    FUN(j->doc_proto, "getElementById", doc_by_id, 1);
    FUN(j->doc_proto, "querySelector", doc_query, 1);
    FUN(j->doc_proto, "querySelectorAll", doc_query_all, 1);
    FUN(j->doc_proto, "getElementsByTagName", node_get_by_tag, 1);
    FUN(j->doc_proto, "getElementsByTagNameNS",
        node_get_by_tag_ns, 2);
    FUN(j->doc_proto, "getElementsByClassName", node_get_by_class, 1);
    FUN(j->doc_proto, "getElementsByName", doc_get_by_name, 1);
    FUN(j->doc_proto, "createElement", doc_create_element, 1);
    FUN(j->doc_proto, "createElementNS", doc_create_element_ns, 2);
    FUN(j->doc_proto, "createTextNode", doc_create_text, 1);
    FUN(j->doc_proto, "createComment", doc_create_comment, 1);
    FUN(j->doc_proto, "createDocumentFragment", doc_create_fragment, 0);
    FUN(j->doc_proto, "createNodeIterator",
        doc_create_node_iterator, 3);
    FUN(j->doc_proto, "createTreeWalker",
        doc_create_tree_walker, 3);
    FUN(j->doc_proto, "createRange", doc_create_range, 0);
    FUN(j->doc_proto, "getSelection", global_get_selection, 0);
    FUN(j->doc_proto, "addEventListener", node_add_listener, 2);
    ACC(j->doc_proto, "documentElement", doc_html_get, 0);
    ACC(j->doc_proto, "head", doc_head_get, 0);
    ACC(j->doc_proto, "body", doc_body_get, 0);
    ACC(j->doc_proto, "title", doc_title_get, doc_title_set);
    ACC(j->doc_proto, "URL", doc_url_get, 0);
    ACC(j->doc_proto, "documentURI", doc_url_get, 0);
    ACC(j->doc_proto, "baseURI", doc_base_url_get, 0);
    ACC(j->doc_proto, "referrer", doc_referrer_get, 0);
    ACC(j->doc_proto, "characterSet", doc_charset_get, 0);
    ACC(j->doc_proto, "charset", doc_charset_get, 0);
    ACC(j->doc_proto, "inputEncoding", doc_charset_get, 0);
    ACC(j->doc_proto, "contentType", doc_content_type_get, 0);
    ACC(j->doc_proto, "compatMode", doc_compat_mode_get, 0);
    ACC(j->doc_proto, "hidden", doc_hidden_get, 0);
    ACC(j->doc_proto, "visibilityState", doc_visibility_get, 0);
    ACC(j->doc_proto, "readyState", doc_ready_get, 0);
    ACC(j->doc_proto, "cookie", doc_cookie_get, doc_cookie_set);

    ACC(j->location_proto, "href", location_href_get, location_href_set);
    ACC(j->location_proto, "origin", location_origin_get, 0);
    ACC(j->location_proto, "protocol",
        location_protocol_get, location_protocol_set);
    ACC(j->location_proto, "host", location_host_get, location_host_set);
    ACC(j->location_proto, "hostname",
        location_hostname_get, location_hostname_set);
    ACC(j->location_proto, "port", location_port_get, location_port_set);
    ACC(j->location_proto, "pathname",
        location_pathname_get, location_pathname_set);
    ACC(j->location_proto, "search",
        location_search_get, location_search_set);
    ACC(j->location_proto, "hash", location_hash_get, location_hash_set);
    FUN(j->location_proto, "assign", location_assign, 1);
    FUN(j->location_proto, "replace", location_assign, 1);
    FUN(j->location_proto, "reload", location_reload, 0);
    FUN(j->location_proto, "toString", location_to_string, 0);

    ACC(j->url_proto, "href", url_href_get, url_href_set);
    ACC(j->url_proto, "origin", url_origin_get, 0);
    ACC(j->url_proto, "protocol", url_protocol_get, url_protocol_set);
    ACC(j->url_proto, "username", url_username_get, url_username_set);
    ACC(j->url_proto, "password", url_password_get, url_password_set);
    ACC(j->url_proto, "host", url_host_get, url_host_set);
    ACC(j->url_proto, "hostname", url_hostname_get, url_hostname_set);
    ACC(j->url_proto, "port", url_port_get, url_port_set);
    ACC(j->url_proto, "pathname", url_pathname_get, url_pathname_set);
    ACC(j->url_proto, "search", url_search_get, url_search_set);
    ACC(j->url_proto, "searchParams", url_search_params_get, 0);
    ACC(j->url_proto, "hash", url_hash_get, url_hash_set);
    FUN(j->url_proto, "toString", url_to_string, 0);
    FUN(j->url_proto, "toJSON", url_to_string, 0);

    FUN(j->class_proto, "add", class_add, 1);
    FUN(j->class_proto, "remove", class_remove, 1);
    FUN(j->class_proto, "toggle", class_toggle, 1);
    FUN(j->class_proto, "contains", class_contains, 1);
    FUN(j->class_proto, "replace", class_replace, 2);
    FUN(j->class_proto, "item", class_item, 1);
    FUN(j->class_proto, "toString", class_to_string, 0);
    ACC(j->class_proto, "length", class_length_get, 0);
    ACC(j->class_proto, "value", class_value_get, class_value_set);

    ACC(j->style_proto, "cssText", style_css_get, style_css_set);
    if (!register_css_decl_accessors(c, j->style_proto))
        return 0;
    ACC(j->style_proto, "length", style_length_get, 0);
    FUN(j->style_proto, "getPropertyValue",
        style_get_property_value, 1);
    FUN(j->style_proto, "setProperty", style_set_property, 3);
    FUN(j->style_proto, "removeProperty", style_remove_property, 1);
    FUN(j->style_proto, "getPropertyPriority",
        style_get_property_priority, 1);
    FUN(j->style_proto, "item", style_item, 1);

    ACC(j->computed_style_proto, "cssText", computed_style_css_get,
        computed_style_readonly);
    if (!register_css_decl_accessors(c, j->computed_style_proto))
        return 0;
    ACC(j->computed_style_proto, "length", computed_style_length_get, 0);
    FUN(j->computed_style_proto, "getPropertyValue",
        computed_style_get_property_value, 1);
    FUN(j->computed_style_proto, "setProperty",
        computed_style_readonly, 3);
    FUN(j->computed_style_proto, "removeProperty",
        computed_style_readonly, 1);
    FUN(j->computed_style_proto, "getPropertyPriority",
        computed_style_get_property_priority, 1);
    FUN(j->computed_style_proto, "item", computed_style_item, 1);

    ACC(j->template_element_proto, "content", template_content_get, 0);
    ACC(j->canvas_element_proto, "width",
        canvas_width_get, canvas_width_set);
    ACC(j->canvas_element_proto, "height",
        canvas_height_get, canvas_height_set);
    FUN(j->canvas_element_proto, "getContext", canvas_get_context, 1);
    FUN(j->canvas_element_proto, "toDataURL", canvas_to_data_url, 1);

    ACC(j->canvas_2d_proto, "canvas", ctx2d_canvas_get, 0);
    ACC(j->canvas_2d_proto, "fillStyle",
        ctx2d_fill_style_get, ctx2d_fill_style_set);
    ACC(j->canvas_2d_proto, "strokeStyle",
        ctx2d_stroke_style_get, ctx2d_stroke_style_set);
    ACC(j->canvas_2d_proto, "lineWidth",
        ctx2d_line_width_get, ctx2d_line_width_set);
    ACC(j->canvas_2d_proto, "globalAlpha",
        ctx2d_global_alpha_get, ctx2d_global_alpha_set);
    ACC(j->canvas_2d_proto, "font", ctx2d_font_get, ctx2d_font_set);
    ACC(j->canvas_2d_proto, "textAlign",
        ctx2d_text_align_get, ctx2d_text_align_set);
    ACC(j->canvas_2d_proto, "textBaseline",
        ctx2d_text_baseline_get, ctx2d_text_baseline_set);
    FUN(j->canvas_2d_proto, "save", ctx2d_save, 0);
    FUN(j->canvas_2d_proto, "restore", ctx2d_restore, 0);
    FUN(j->canvas_2d_proto, "translate", ctx2d_translate, 2);
    FUN(j->canvas_2d_proto, "scale", ctx2d_scale, 2);
    FUN(j->canvas_2d_proto, "rotate", ctx2d_rotate, 1);
    FUN(j->canvas_2d_proto, "transform", ctx2d_transform, 6);
    FUN(j->canvas_2d_proto, "setTransform", ctx2d_set_transform, 6);
    FUN(j->canvas_2d_proto, "resetTransform", ctx2d_reset_transform, 0);
    FUN(j->canvas_2d_proto, "fillRect", ctx2d_fill_rect, 4);
    FUN(j->canvas_2d_proto, "strokeRect", ctx2d_stroke_rect, 4);
    FUN(j->canvas_2d_proto, "clearRect", ctx2d_clear_rect, 4);
    FUN(j->canvas_2d_proto, "beginPath", ctx2d_begin_path, 0);
    FUN(j->canvas_2d_proto, "closePath", ctx2d_close_path, 0);
    FUN(j->canvas_2d_proto, "moveTo", ctx2d_move_to, 2);
    FUN(j->canvas_2d_proto, "lineTo", ctx2d_line_to, 2);
    FUN(j->canvas_2d_proto, "rect", ctx2d_rect, 4);
    FUN(j->canvas_2d_proto, "arc", ctx2d_arc, 6);
    FUN(j->canvas_2d_proto, "quadraticCurveTo",
        ctx2d_quadratic_curve_to, 4);
    FUN(j->canvas_2d_proto, "bezierCurveTo", ctx2d_bezier_curve_to, 6);
    FUN(j->canvas_2d_proto, "fill", ctx2d_fill, 0);
    FUN(j->canvas_2d_proto, "stroke", ctx2d_stroke, 0);
    FUN(j->canvas_2d_proto, "fillText", ctx2d_fill_text, 3);
    FUN(j->canvas_2d_proto, "strokeText", ctx2d_stroke_text, 3);
    FUN(j->canvas_2d_proto, "measureText", ctx2d_measure_text, 1);
    FUN(j->canvas_2d_proto, "createImageData", ctx2d_create_image_data, 2);
    FUN(j->canvas_2d_proto, "getImageData", ctx2d_get_image_data, 4);
    FUN(j->canvas_2d_proto, "putImageData", ctx2d_put_image_data, 3);
    FUN(j->canvas_2d_proto, "createLinearGradient",
        ctx2d_create_linear_gradient, 4);
    FUN(j->canvas_2d_proto, "createRadialGradient",
        ctx2d_create_radial_gradient, 6);
    FUN(j->canvas_gradient_proto, "addColorStop",
        canvas_gradient_add_color_stop, 2);

    FUN(j->dom_parser_proto, "parseFromString",
        dom_parser_parse_from_string, 2);

    FUN(j->resize_observer_proto, "observe", resize_observer_observe, 1);
    FUN(j->resize_observer_proto, "unobserve", resize_observer_unobserve, 1);
    FUN(j->resize_observer_proto, "disconnect",
        resize_observer_disconnect, 0);

    FUN(j->intersection_observer_proto, "observe",
        intersection_observer_observe, 1);
    FUN(j->intersection_observer_proto, "unobserve",
        intersection_observer_unobserve, 1);
    FUN(j->intersection_observer_proto, "disconnect",
        intersection_observer_disconnect, 0);
    FUN(j->intersection_observer_proto, "takeRecords",
        intersection_observer_take_records, 0);

    ACC(j->response_proto, "status", response_status_get, 0);
    ACC(j->response_proto, "statusText", response_status_text_get, 0);
    ACC(j->response_proto, "ok", response_ok_get, 0);
    ACC(j->response_proto, "url", response_url_get, 0);
    ACC(j->response_proto, "headers", response_headers_get, 0);
    ACC(j->response_proto, "bodyUsed", response_body_used_get, 0);
    ACC(j->response_proto, "type", response_type_get, 0);
    ACC(j->response_proto, "redirected", response_redirected_get, 0);
    FUN(j->response_proto, "text", response_text, 0);
    FUN(j->response_proto, "json", response_json, 0);
    FUN(j->response_proto, "arrayBuffer", response_arraybuffer, 0);
    FUN(j->response_proto, "blob", response_blob, 0);
    FUN(j->response_proto, "bytes", response_bytes, 0);
    FUN(j->response_proto, "formData", response_formdata, 0);
    FUN(j->response_proto, "clone", response_clone, 0);
    ACC(j->request_proto, "url", request_url_get, 0);
    ACC(j->request_proto, "method", request_method_get, 0);
    ACC(j->request_proto, "cache", request_cache_get, 0);
    ACC(j->request_proto, "credentials", request_credentials_get, 0);
    ACC(j->request_proto, "destination", request_destination_get, 0);
    ACC(j->request_proto, "integrity", request_integrity_get, 0);
    ACC(j->request_proto, "mode", request_mode_get, 0);
    ACC(j->request_proto, "redirect", request_redirect_get, 0);
    ACC(j->request_proto, "referrer", request_referrer_get, 0);
    ACC(j->request_proto, "referrerPolicy",
        request_referrer_policy_get, 0);
    ACC(j->request_proto, "keepalive", request_keepalive_get, 0);
    ACC(j->request_proto, "isHistoryNavigation",
        request_navigation_get, 0);
    ACC(j->request_proto, "isReloadNavigation",
        request_navigation_get, 0);
    ACC(j->request_proto, "headers", request_headers_get, 0);
    ACC(j->request_proto, "signal", request_signal_get, 0);
    ACC(j->request_proto, "bodyUsed", request_body_used_get, 0);
    FUN(j->request_proto, "clone", request_clone, 0);
    FUN(j->request_proto, "text", request_text, 0);
    FUN(j->request_proto, "json", request_json, 0);
    FUN(j->request_proto, "arrayBuffer", request_arraybuffer, 0);
    FUN(j->request_proto, "blob", request_blob, 0);
    FUN(j->request_proto, "bytes", request_bytes, 0);
    FUN(j->request_proto, "formData", request_formdata, 0);
    ACC(j->file_proto, "name", file_name_get, 0);
    ACC(j->file_proto, "lastModified", file_last_modified_get, 0);
    ACC(j->file_proto, "webkitRelativePath", file_relative_path_get, 0);
    ACC(j->file_reader_proto, "readyState", file_reader_ready_get, 0);
    ACC(j->file_reader_proto, "result", file_reader_result_get, 0);
    ACC(j->file_reader_proto, "error", file_reader_error_get, 0);
    FUN(j->file_reader_proto, "readAsText", file_reader_read_text, 1);
    FUN(j->file_reader_proto, "readAsArrayBuffer",
        file_reader_read_arraybuffer, 1);
    FUN(j->file_reader_proto, "readAsBinaryString",
        file_reader_read_binary, 1);
    FUN(j->file_reader_proto, "readAsDataURL",
        file_reader_read_data_url, 1);
    FUN(j->file_reader_proto, "abort", file_reader_abort, 0);
    FUN(j->headers_proto, "get", headers_get, 1);
    FUN(j->headers_proto, "has", headers_has, 1);
    FUN(j->headers_proto, "append", headers_append_method, 2);
    FUN(j->headers_proto, "set", headers_set_method, 2);
    FUN(j->headers_proto, "delete", headers_delete_method, 1);
    FUN(j->headers_proto, "forEach", headers_for_each, 1);
    FUN(j->headers_proto, "entries", headers_entries, 0);
    FUN(j->headers_proto, "keys", headers_keys, 0);
    FUN(j->headers_proto, "values", headers_values, 0);
    FUN(j->web_iterator_proto, "next", web_iterator_next, 0);

    ACC(j->storage_proto, "length", storage_length_get, 0);
    FUN(j->storage_proto, "key", storage_key_method, 1);
    FUN(j->storage_proto, "getItem", storage_get_item, 1);
    FUN(j->storage_proto, "setItem", storage_set_item, 2);
    FUN(j->storage_proto, "removeItem", storage_remove_item, 1);
    FUN(j->storage_proto, "clear", storage_clear_method, 0);
#undef FUN
#undef ACC
    return 1;
}

struct jsdom *jsdom_new(struct dom_document *doc,
                        const struct jsdom_config *cfg)
{
    struct jsdom *j;
    js_config jc;
    js_object *docobj, *locobj, *navobj, *clipboardobj, *screenobj, *perfobj;
    js_object *url_ctor;
    js_object *event_target_ctor, *event_ctor, *custom_event_ctor;
    js_object *ui_event_ctor, *mouse_event_ctor, *keyboard_event_ctor;
    js_object *focus_event_ctor, *input_event_ctor, *pointer_event_ctor;
    js_object *node_ctor, *character_data_ctor, *text_ctor, *comment_ctor;
    js_object *element_ctor, *html_element_ctor, *svg_element_ctor;
    js_object *html_canvas_ctor, *html_template_ctor, *canvas_2d_ctor;
    js_object *image_data_ctor, *canvas_gradient_ctor, *dom_parser_ctor;
    js_object *resize_observer_ctor, *intersection_observer_ctor;
    js_object *fragment_ctor, *document_ctor;
    js_object *html_document_ctor;
    js_object *mutation_observer_ctor, *mutation_record_ctor;
    js_object *dom_exception_ctor;
    js_object *attr_ctor, *named_node_map_ctor;
    js_object *node_iterator_ctor, *tree_walker_ctor, *node_filter;
    js_object *range_ctor, *selection_ctor;
    js_object *headers_ctor, *response_ctor, *request_ctor;
    js_object *file_ctor, *file_reader_ctor;
    js_object *localobj, *sessionobj, *languages;
    js_object *orientation;
    js_object *historyobj;
    js_object *selection_object;
    struct history_state *history;
    struct selection_state *selection_state;
    struct url parsed_url;
    js_value global;

    if (!doc)
        return 0;
    j = (struct jsdom *)calloc(1, sizeof(*j));
    if (!j)
        return 0;
    j->doc = doc;
    j->parsed_doc_credit.remaining = cfg && cfg->max_parsed_document_bytes
        ? cfg->max_parsed_document_bytes : DOM_MAX_ARENA;
    j->external_sheet = cfg ? cfg->stylesheet : 0;
    if (j->external_sheet)
        j->initial_style_hash = style_tags_hash(doc);
    j->style_sheet_dirty = 1;
    j->url = dup_z(cfg && cfg->url ? cfg->url : "about:blank");
    j->base_url = dup_z(cfg && cfg->base_url ? cfg->base_url :
                        (cfg && cfg->url ? cfg->url : "about:blank"));
    j->base_follows_url = !(cfg && cfg->base_url) ||
        (j->url && j->base_url && !strcmp(j->url, j->base_url));
    j->print = cfg ? cfg->print : 0;
    j->cookie_get = cfg ? cfg->cookie_get : 0;
    j->cookie_set = cfg ? cfg->cookie_set : 0;
    j->fetch = cfg ? cfg->fetch : 0;
    j->get_box_rect = cfg ? cfg->get_box_rect : 0;
    j->scroll_cb = cfg ? cfg->scroll_cb : 0;
    j->clipboard_write = cfg ? cfg->clipboard_write : 0;
    j->clipboard_read = cfg ? cfg->clipboard_read : 0;
    j->print_user = cfg ? cfg->user : 0;
    j->local_storage = cfg ? cfg->local_storage : 0;
    j->session_storage = cfg ? cfg->session_storage : 0;
    j->viewport_width = cfg && cfg->viewport_width
        ? cfg->viewport_width : 900;
    j->viewport_height = cfg && cfg->viewport_height
        ? cfg->viewport_height : 620;
    if (!j->local_storage) {
        j->local_storage = web_storage_new(1024UL * 1024UL, 512);
        j->owns_local_storage = 1;
    }
    if (!j->session_storage) {
        j->session_storage = web_storage_new(1024UL * 1024UL, 512);
        j->owns_session_storage = 1;
    }
    if (!j->url || !j->base_url) {
        if (j->owns_session_storage)
            web_storage_free(j->session_storage);
        if (j->owns_local_storage)
            web_storage_free(j->local_storage);
        free(j->base_url);
        free(j->url);
        free(j);
        return 0;
    }
    if (!j->local_storage || !j->session_storage ||
        url_parse(j->url, &parsed_url) != URL_OK ||
        !parsed_url.has_scheme ||
        url_origin(&parsed_url, j->origin, sizeof(j->origin)) != URL_OK)
        goto fail;

    js_config_default(&jc);
    jc.user = j;
    jc.print = js_print;
    jc.random_bytes = jsdom_random_bytes;
    if (cfg && cfg->max_heap) jc.max_heap = cfg->max_heap;
    if (cfg && cfg->max_steps) jc.max_steps = cfg->max_steps;
    j->ctx = js_new(&jc);
    if (!j->ctx) {
        jsdom_free(j);
        return 0;
    }
    if (js_host_finalizer(j->ctx, JSDOM_TAG, host_free) != 0)
        goto fail;
    j->event_target_proto = js_new_object(j->ctx);
    js_abortsignal_set_proto(j->ctx, j->event_target_proto);
    js_set_abort_hooks(j->ctx, jsdom_on_abort, jsdom_make_abort_event, j);
    js_set_blob_extractor(j->ctx, jsdom_file_extract, j);
    j->node_proto = js_new_object_proto(j->ctx, j->event_target_proto);
    j->character_data_proto =
        js_new_object_proto(j->ctx, j->node_proto);
    j->text_proto =
        js_new_object_proto(j->ctx, j->character_data_proto);
    j->comment_proto =
        js_new_object_proto(j->ctx, j->character_data_proto);
    j->element_proto = js_new_object_proto(j->ctx, j->node_proto);
    j->html_element_proto =
        js_new_object_proto(j->ctx, j->element_proto);
    j->canvas_element_proto =
        js_new_object_proto(j->ctx, j->html_element_proto);
    j->template_element_proto =
        js_new_object_proto(j->ctx, j->html_element_proto);
    j->svg_element_proto =
        js_new_object_proto(j->ctx, j->element_proto);
    j->fragment_proto = js_new_object_proto(j->ctx, j->node_proto);
    j->doc_proto = js_new_object_proto(j->ctx, j->node_proto);
    j->html_document_proto =
        js_new_object_proto(j->ctx, j->doc_proto);
    j->location_proto = js_new_object(j->ctx);
    j->class_proto = js_new_object(j->ctx);
    j->style_proto = js_new_object(j->ctx);
    j->computed_style_proto = js_new_object(j->ctx);
    j->canvas_2d_proto = js_new_object(j->ctx);
    j->image_data_proto = js_new_object(j->ctx);
    j->canvas_gradient_proto = js_new_object(j->ctx);
    j->dom_parser_proto = js_new_object(j->ctx);
    j->resize_observer_proto = js_new_object(j->ctx);
    j->intersection_observer_proto = js_new_object(j->ctx);
    j->response_proto = js_new_object(j->ctx);
    j->request_proto = js_new_object(j->ctx);
    j->file_proto = js_new_object_proto(j->ctx, js_blob_proto(j->ctx));
    j->file_reader_proto =
        js_new_object_proto(j->ctx, j->event_target_proto);
    j->web_iterator_proto = js_new_object(j->ctx);
    j->media_query_proto =
        js_new_object_proto(j->ctx, j->event_target_proto);
    j->history_proto = js_new_object(j->ctx);
    j->mutation_observer_proto = js_new_object(j->ctx);
    j->mutation_record_proto = js_new_object(j->ctx);
    j->dom_exception_proto = js_new_object(j->ctx);
    j->attr_proto = js_new_object_proto(j->ctx, j->node_proto);
    j->named_node_map_proto = js_new_object(j->ctx);
    j->node_iterator_proto = js_new_object(j->ctx);
    j->tree_walker_proto = js_new_object(j->ctx);
    j->range_proto = js_new_object(j->ctx);
    j->selection_proto = js_new_object(j->ctx);
    j->headers_proto = js_new_object(j->ctx);
    j->storage_proto = js_new_object(j->ctx);
    j->url_proto = js_new_object(j->ctx);
    j->event_proto = js_new_object(j->ctx);
    j->custom_event_proto = js_new_object_proto(j->ctx, j->event_proto);
    j->ui_event_proto = js_new_object_proto(j->ctx, j->event_proto);
    j->mouse_event_proto =
        js_new_object_proto(j->ctx, j->ui_event_proto);
    j->keyboard_event_proto =
        js_new_object_proto(j->ctx, j->ui_event_proto);
    j->focus_event_proto =
        js_new_object_proto(j->ctx, j->ui_event_proto);
    j->input_event_proto =
        js_new_object_proto(j->ctx, j->ui_event_proto);
    j->pointer_event_proto =
        js_new_object_proto(j->ctx, j->mouse_event_proto);
    if (!j->event_target_proto || !j->node_proto ||
        !j->character_data_proto || !j->text_proto || !j->comment_proto ||
        !j->element_proto || !j->html_element_proto ||
        !j->canvas_element_proto || !j->template_element_proto ||
        !j->svg_element_proto || !j->fragment_proto || !j->doc_proto ||
        !j->html_document_proto ||
        !j->location_proto ||
        !j->class_proto || !j->style_proto || !j->computed_style_proto ||
        !j->canvas_2d_proto || !j->image_data_proto ||
        !j->canvas_gradient_proto || !j->dom_parser_proto ||
        !j->resize_observer_proto || !j->intersection_observer_proto ||
        !j->response_proto ||
        !j->request_proto || !j->file_proto ||
        !j->file_reader_proto ||
        !j->web_iterator_proto || !j->media_query_proto ||
        !j->history_proto || !j->mutation_observer_proto ||
        !j->mutation_record_proto || !j->dom_exception_proto ||
        !j->attr_proto || !j->named_node_map_proto ||
        !j->node_iterator_proto || !j->tree_walker_proto ||
        !j->range_proto || !j->selection_proto ||
        !j->headers_proto ||
        !j->storage_proto || !j->url_proto ||
        !j->event_proto || !j->custom_event_proto || !j->ui_event_proto ||
        !j->mouse_event_proto || !j->keyboard_event_proto ||
        !j->focus_event_proto || !j->input_event_proto ||
        !j->pointer_event_proto ||
        !js_abortsignal_ctor(j->ctx) || !js_blob_proto(j->ctx) ||
        !define_node_proto(j) || !define_other_protos(j))
        goto fail;

    url_ctor = js_new_native_constructor(j->ctx, url_constructor, "URL", 1,
                                         j->url_proto);
    if (!url_ctor ||
        js_define_native(j->ctx, url_ctor, "canParse",
                         url_can_parse, 1) != JS_OK ||
        js_define_native(j->ctx, url_ctor, "createObjectURL",
                         url_create_object_url, 1) != JS_OK ||
        js_define_native(j->ctx, url_ctor, "revokeObjectURL",
                         url_revoke_object_url, 1) != JS_OK ||
        js_define_native(j->ctx, js_abortsignal_ctor(j->ctx), "timeout",
                         abort_signal_timeout_static, 1) != JS_OK)
        goto fail;
    event_target_ctor = js_new_native_constructor(
        j->ctx, event_target_constructor, "EventTarget", 0,
        j->event_target_proto);
    node_ctor = js_new_native_constructor(
        j->ctx, illegal_dom_constructor, "Node", 0, j->node_proto);
    character_data_ctor = js_new_native_constructor(
        j->ctx, illegal_dom_constructor, "CharacterData", 0,
        j->character_data_proto);
    text_ctor = js_new_native_constructor(
        j->ctx, text_constructor, "Text", 0, j->text_proto);
    comment_ctor = js_new_native_constructor(
        j->ctx, comment_constructor, "Comment", 0, j->comment_proto);
    element_ctor = js_new_native_constructor(
        j->ctx, illegal_dom_constructor, "Element", 0, j->element_proto);
    html_element_ctor = js_new_native_constructor(
        j->ctx, illegal_dom_constructor, "HTMLElement", 0,
        j->html_element_proto);
    html_canvas_ctor = js_new_native_constructor(
        j->ctx, illegal_dom_constructor, "HTMLCanvasElement", 0,
        j->canvas_element_proto);
    html_template_ctor = js_new_native_constructor(
        j->ctx, illegal_dom_constructor, "HTMLTemplateElement", 0,
        j->template_element_proto);
    canvas_2d_ctor = js_new_native_constructor(
        j->ctx, illegal_dom_constructor, "CanvasRenderingContext2D", 0,
        j->canvas_2d_proto);
    image_data_ctor = js_new_native_constructor(
        j->ctx, image_data_constructor, "ImageData", 2,
        j->image_data_proto);
    canvas_gradient_ctor = js_new_native_constructor(
        j->ctx, illegal_dom_constructor, "CanvasGradient", 0,
        j->canvas_gradient_proto);
    dom_parser_ctor = js_new_native_constructor(
        j->ctx, dom_parser_constructor, "DOMParser", 0, j->dom_parser_proto);
    resize_observer_ctor = js_new_native_constructor(
        j->ctx, resize_observer_constructor, "ResizeObserver", 1,
        j->resize_observer_proto);
    intersection_observer_ctor = js_new_native_constructor(
        j->ctx, intersection_observer_constructor, "IntersectionObserver", 1,
        j->intersection_observer_proto);
    svg_element_ctor = js_new_native_constructor(
        j->ctx, illegal_dom_constructor, "SVGElement", 0,
        j->svg_element_proto);
    fragment_ctor = js_new_native_constructor(
        j->ctx, illegal_dom_constructor, "DocumentFragment", 0,
        j->fragment_proto);
    document_ctor = js_new_native_constructor(
        j->ctx, illegal_dom_constructor, "Document", 0, j->doc_proto);
    html_document_ctor = js_new_native_constructor(
        j->ctx, illegal_dom_constructor, "HTMLDocument", 0,
        j->html_document_proto);
    mutation_observer_ctor = js_new_native_constructor(
        j->ctx, mutation_observer_constructor, "MutationObserver", 1,
        j->mutation_observer_proto);
    mutation_record_ctor = js_new_native_constructor(
        j->ctx, illegal_dom_constructor, "MutationRecord", 0,
        j->mutation_record_proto);
    dom_exception_ctor = js_new_native_constructor(
        j->ctx, dom_exception_constructor, "DOMException", 0,
        j->dom_exception_proto);
    attr_ctor = js_new_native_constructor(
        j->ctx, illegal_dom_constructor, "Attr", 0,
        j->attr_proto);
    named_node_map_ctor = js_new_native_constructor(
        j->ctx, illegal_dom_constructor, "NamedNodeMap", 0,
        j->named_node_map_proto);
    node_iterator_ctor = js_new_native_constructor(
        j->ctx, illegal_dom_constructor, "NodeIterator", 0,
        j->node_iterator_proto);
    tree_walker_ctor = js_new_native_constructor(
        j->ctx, illegal_dom_constructor, "TreeWalker", 0,
        j->tree_walker_proto);
    range_ctor = js_new_native_constructor(
        j->ctx, range_constructor, "Range", 0, j->range_proto);
    selection_ctor = js_new_native_constructor(
        j->ctx, illegal_dom_constructor, "Selection", 0,
        j->selection_proto);
    node_filter = js_new_object(j->ctx);
    event_ctor = js_new_native_constructor(
        j->ctx, event_constructor, "Event", 1, j->event_proto);
    custom_event_ctor = js_new_native_constructor(
        j->ctx, custom_event_constructor, "CustomEvent", 1,
        j->custom_event_proto);
    ui_event_ctor = js_new_native_constructor(
        j->ctx, ui_event_constructor, "UIEvent", 1,
        j->ui_event_proto);
    mouse_event_ctor = js_new_native_constructor(
        j->ctx, mouse_event_constructor, "MouseEvent", 1,
        j->mouse_event_proto);
    keyboard_event_ctor = js_new_native_constructor(
        j->ctx, keyboard_event_constructor, "KeyboardEvent", 1,
        j->keyboard_event_proto);
    focus_event_ctor = js_new_native_constructor(
        j->ctx, focus_event_constructor, "FocusEvent", 1,
        j->focus_event_proto);
    input_event_ctor = js_new_native_constructor(
        j->ctx, input_event_constructor, "InputEvent", 1,
        j->input_event_proto);
    pointer_event_ctor = js_new_native_constructor(
        j->ctx, pointer_event_constructor, "PointerEvent", 1,
        j->pointer_event_proto);
    if (!event_target_ctor || !node_ctor || !character_data_ctor ||
        !text_ctor || !comment_ctor || !element_ctor || !fragment_ctor ||
        !html_element_ctor || !svg_element_ctor ||
        !html_canvas_ctor || !html_template_ctor || !canvas_2d_ctor ||
        !image_data_ctor || !canvas_gradient_ctor || !dom_parser_ctor ||
        !resize_observer_ctor || !intersection_observer_ctor ||
        !document_ctor || !html_document_ctor ||
        !mutation_observer_ctor || !mutation_record_ctor ||
        !dom_exception_ctor || !attr_ctor || !named_node_map_ctor ||
        !node_iterator_ctor || !tree_walker_ctor || !node_filter ||
        !range_ctor || !selection_ctor ||
        !event_ctor || !custom_event_ctor || !ui_event_ctor ||
        !mouse_event_ctor || !keyboard_event_ctor ||
        !focus_event_ctor || !input_event_ctor || !pointer_event_ctor ||
        !define_node_constants(j->ctx, node_ctor) ||
        !define_node_constants(j->ctx, j->node_proto) ||
        !define_dom_exception_constants(j->ctx, dom_exception_ctor) ||
        !define_dom_exception_constants(j->ctx, j->dom_exception_proto) ||
        !define_node_filter_constants(j->ctx, node_filter) ||
        !define_event_constants(j->ctx, event_ctor) ||
        !define_event_constants(j->ctx, j->event_proto) ||
        !define_keyboard_event_constants(j->ctx, keyboard_event_ctor) ||
        !define_keyboard_event_constants(j->ctx,
                                         j->keyboard_event_proto) ||
        !define_range_constants(j->ctx, range_ctor) ||
        !define_range_constants(j->ctx, j->range_proto))
        goto fail;
    headers_ctor = js_new_native_constructor(
        j->ctx, headers_constructor, "Headers", 1, j->headers_proto);
    response_ctor = js_new_native_constructor(
        j->ctx, response_constructor, "Response", 0, j->response_proto);
    request_ctor = js_new_native_constructor(
        j->ctx, request_constructor, "Request", 1, j->request_proto);
    file_ctor = js_new_native_constructor(
        j->ctx, file_constructor, "File", 2, j->file_proto);
    file_reader_ctor = js_new_native_constructor(
        j->ctx, file_reader_constructor, "FileReader", 0,
        j->file_reader_proto);
    if (!headers_ctor || !response_ctor || !request_ctor ||
        !file_ctor || !file_reader_ctor ||
        js_define_native(j->ctx, response_ctor, "json",
                         response_json_static, 1) != JS_OK ||
        js_define_native(j->ctx, response_ctor, "redirect",
                         response_redirect_static, 1) != JS_OK ||
        js_define_native(j->ctx, response_ctor, "error",
                         response_error_static, 0) != JS_OK ||
        js_set(j->ctx, js_object_value(file_reader_ctor), "EMPTY",
               js_number(0)) != JS_OK ||
        js_set(j->ctx, js_object_value(file_reader_ctor), "LOADING",
               js_number(1)) != JS_OK ||
        js_set(j->ctx, js_object_value(file_reader_ctor), "DONE",
               js_number(2)) != JS_OK ||
        js_set(j->ctx, js_object_value(j->file_reader_proto), "EMPTY",
               js_number(0)) != JS_OK ||
        js_set(j->ctx, js_object_value(j->file_reader_proto), "LOADING",
               js_number(1)) != JS_OK ||
        js_set(j->ctx, js_object_value(j->file_reader_proto), "DONE",
               js_number(2)) != JS_OK)
        goto fail;

    docobj = new_host(j, 0, HOST_DOCUMENT, j->html_document_proto);
    locobj = new_host(j, 0, HOST_LOCATION, j->location_proto);
    selection_state =
        (struct selection_state *)calloc(1, sizeof(*selection_state));
    if (selection_state)
        selection_state->range = js_undefined();
    selection_object = selection_state
        ? new_host_data(j, 0, HOST_SELECTION, selection_state,
                        j->selection_proto) : 0;
    if (!selection_object)
        free(selection_state);
    j->selection = selection_object
        ? js_object_value(selection_object) : js_undefined();
    navobj = js_new_object(j->ctx);
    clipboardobj = js_new_object(j->ctx);
    languages = js_new_array(j->ctx);
    screenobj = js_new_object(j->ctx);
    orientation = js_new_object(j->ctx);
    perfobj = js_new_object(j->ctx);
    localobj = new_host_data(j, 0, HOST_STORAGE, j->local_storage,
                             j->storage_proto);
    sessionobj = new_host_data(j, 0, HOST_STORAGE, j->session_storage,
                               j->storage_proto);
    history = (struct history_state *)calloc(1, sizeof(*history));
    if (history) {
        history->count = 1;
        history->index = 0;
        strcpy(history->scroll_restoration, "auto");
        history->entries[0].url = dup_z(j->url);
        history->entries[0].state = js_null();
        if (!history->entries[0].url) {
            history_release(history);
            history = 0;
        }
    }
    historyobj = history
        ? new_host_data(j, 0, HOST_HISTORY, history, j->history_proto) : 0;
    if (history && !historyobj)
        history_release(history);
    if (!docobj || !locobj || !selection_object ||
        !navobj || !clipboardobj || !languages ||
        !orientation ||
        !screenobj || !perfobj ||
        !localobj || !sessionobj || !historyobj)
        goto fail;
    if (js_array_push(j->ctx, languages,
                      js_mkcstring(j->ctx, "en-US")) != JS_OK ||
        js_array_push(j->ctx, languages,
                      js_mkcstring(j->ctx, "en")) != JS_OK ||
        js_define_native(j->ctx, clipboardobj, "writeText",
                         navigator_clipboard_write_text, 1) != JS_OK ||
        js_define_native(j->ctx, clipboardobj, "readText",
                         navigator_clipboard_read_text, 0) != JS_OK ||
        js_define_native(j->ctx, navobj, "sendBeacon",
                         navigator_send_beacon, 2) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "clipboard",
               js_object_value(clipboardobj)) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "userAgent",
               js_mkcstring(j->ctx,
                            "Mozilla/5.0 (X11; KestrelOS) "
                            "KestrelBrowser/1.0")) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "appCodeName",
               js_mkcstring(j->ctx, "Mozilla")) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "appName",
               js_mkcstring(j->ctx, "Netscape")) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "appVersion",
               js_mkcstring(j->ctx, "5.0 (X11; KestrelOS)")) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "platform",
               js_mkcstring(j->ctx, "KestrelOS x86_64")) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "oscpu",
               js_mkcstring(j->ctx, "KestrelOS x86_64")) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "product",
               js_mkcstring(j->ctx, "Gecko")) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "productSub",
               js_mkcstring(j->ctx, "20030107")) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "vendor",
               js_mkcstring(j->ctx, "")) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "vendorSub",
               js_mkcstring(j->ctx, "")) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "language",
               js_mkcstring(j->ctx, "en-US")) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "languages",
               js_object_value(languages)) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "hardwareConcurrency",
               js_number(cfg && cfg->hardware_concurrency
                         ? cfg->hardware_concurrency : 1)) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "cookieEnabled",
               js_bool(1)) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "onLine",
               js_bool(1)) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "maxTouchPoints",
               js_number(0)) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "webdriver",
               js_bool(0)) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "pdfViewerEnabled",
               js_bool(0)) != JS_OK ||
        js_set(j->ctx, js_object_value(navobj), "doNotTrack",
               js_null()) != JS_OK ||
        js_set(j->ctx, js_object_value(screenobj), "width",
               js_number(j->viewport_width)) != JS_OK ||
        js_set(j->ctx, js_object_value(screenobj), "height",
               js_number(j->viewport_height)) != JS_OK ||
        js_set(j->ctx, js_object_value(screenobj), "availWidth",
               js_number(j->viewport_width)) != JS_OK ||
        js_set(j->ctx, js_object_value(screenobj), "availHeight",
               js_number(j->viewport_height)) != JS_OK ||
        js_set(j->ctx, js_object_value(screenobj), "colorDepth",
               js_number(24)) != JS_OK ||
        js_set(j->ctx, js_object_value(screenobj), "pixelDepth",
               js_number(24)) != JS_OK ||
        js_set(j->ctx, js_object_value(orientation), "type",
               js_mkcstring(j->ctx,
                            j->viewport_width >= j->viewport_height
                            ? "landscape-primary"
                            : "portrait-primary")) != JS_OK ||
        js_set(j->ctx, js_object_value(orientation), "angle",
               js_number(0)) != JS_OK ||
        js_set(j->ctx, js_object_value(screenobj), "orientation",
               js_object_value(orientation)) != JS_OK ||
        js_set(j->ctx, js_object_value(perfobj), "timeOrigin",
               js_number(0)) != JS_OK ||
        js_define_native(j->ctx, perfobj, "now",
                         global_performance_now, 0) != JS_OK)
        goto fail;
    global = js_global(j->ctx);
    {
        js_value obj_ctor, freeze_fn, lang_val = js_object_value(languages), ig;
        if (js_get(j->ctx, global, "Object", &obj_ctor) == JS_OK &&
            js_get(j->ctx, obj_ctor, "freeze", &freeze_fn) == JS_OK)
            (void)js_call(j->ctx, freeze_fn, obj_ctor, 1, &lang_val, &ig);
    }
    if (js_define(j->ctx, global.u.obj, "__kestrelDocument",
                  js_object_value(docobj)) != JS_OK ||
        js_set(j->ctx, global, "document", js_object_value(docobj)) != JS_OK ||
        js_set(j->ctx, global, "location", js_object_value(locobj)) != JS_OK ||
        js_set(j->ctx, js_object_value(docobj), "location",
               js_object_value(locobj)) != JS_OK ||
        js_set(j->ctx, js_object_value(docobj), "defaultView",
               global) != JS_OK ||
        js_set(j->ctx, global, "window", global) != JS_OK ||
        js_set(j->ctx, global, "self", global) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "alert", global_alert, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "addEventListener",
                         global_add_listener, 2) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "removeEventListener",
                         global_remove_listener, 2) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "dispatchEvent",
                         global_dispatch_event, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "setTimeout",
                         global_set_timeout, 2) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "setInterval",
                         global_set_interval, 2) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "clearTimeout",
                         global_clear_timeout, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "clearInterval",
                         global_clear_timeout, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "requestAnimationFrame",
                         global_animation_frame, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "cancelAnimationFrame",
                         global_clear_timeout, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "requestIdleCallback",
                         global_idle_callback, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "cancelIdleCallback",
                         global_clear_timeout, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "queueMicrotask",
                         global_queue_microtask, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "getComputedStyle",
                         global_computed_style, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "atob",
                         global_atob, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "btoa",
                         global_btoa, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "fetch",
                         global_fetch, 2) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "matchMedia",
                         global_match_media, 1) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "getSelection",
                         global_get_selection, 0) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "scrollTo",
                         global_scroll_to, 2) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "scroll",
                         global_scroll_to, 2) != JS_OK ||
        js_define_native(j->ctx, global.u.obj, "scrollBy",
                         global_scroll_by, 2) != JS_OK ||
        js_define_accessor(j->ctx, global.u.obj, "innerWidth",
                           window_inner_w_get, 0, 1) != JS_OK ||
        js_define_accessor(j->ctx, global.u.obj, "innerHeight",
                           window_inner_h_get, 0, 1) != JS_OK ||
        js_define_accessor(j->ctx, global.u.obj, "outerWidth",
                           window_inner_w_get, 0, 1) != JS_OK ||
        js_define_accessor(j->ctx, global.u.obj, "outerHeight",
                           window_inner_h_get, 0, 1) != JS_OK ||
        js_set(j->ctx, global, "devicePixelRatio",
               js_number(1)) != JS_OK ||
        js_define_accessor(j->ctx, global.u.obj, "scrollX",
                           window_scroll_x_get, 0, 1) != JS_OK ||
        js_define_accessor(j->ctx, global.u.obj, "scrollY",
                           window_scroll_y_get, 0, 1) != JS_OK ||
        js_define_accessor(j->ctx, global.u.obj, "pageXOffset",
                           window_scroll_x_get, 0, 1) != JS_OK ||
        js_define_accessor(j->ctx, global.u.obj, "pageYOffset",
                           window_scroll_y_get, 0, 1) != JS_OK ||
        js_set(j->ctx, global, "localStorage",
               js_object_value(localobj)) != JS_OK ||
        js_set(j->ctx, global, "sessionStorage",
               js_object_value(sessionobj)) != JS_OK ||
        js_set(j->ctx, global, "URL",
               js_object_value(url_ctor)) != JS_OK ||
        js_set(j->ctx, global, "EventTarget",
               js_object_value(event_target_ctor)) != JS_OK ||
        js_set(j->ctx, global, "Node",
               js_object_value(node_ctor)) != JS_OK ||
        js_set(j->ctx, global, "CharacterData",
               js_object_value(character_data_ctor)) != JS_OK ||
        js_set(j->ctx, global, "Text",
               js_object_value(text_ctor)) != JS_OK ||
        js_set(j->ctx, global, "Comment",
               js_object_value(comment_ctor)) != JS_OK ||
        js_set(j->ctx, global, "Element",
               js_object_value(element_ctor)) != JS_OK ||
        js_set(j->ctx, global, "HTMLElement",
               js_object_value(html_element_ctor)) != JS_OK ||
        js_set(j->ctx, global, "HTMLCanvasElement",
               js_object_value(html_canvas_ctor)) != JS_OK ||
        js_set(j->ctx, global, "HTMLTemplateElement",
               js_object_value(html_template_ctor)) != JS_OK ||
        js_set(j->ctx, global, "CanvasRenderingContext2D",
               js_object_value(canvas_2d_ctor)) != JS_OK ||
        js_set(j->ctx, global, "ImageData",
               js_object_value(image_data_ctor)) != JS_OK ||
        js_set(j->ctx, global, "CanvasGradient",
               js_object_value(canvas_gradient_ctor)) != JS_OK ||
        js_set(j->ctx, global, "DOMParser",
               js_object_value(dom_parser_ctor)) != JS_OK ||
        js_set(j->ctx, global, "ResizeObserver",
               js_object_value(resize_observer_ctor)) != JS_OK ||
        js_set(j->ctx, global, "IntersectionObserver",
               js_object_value(intersection_observer_ctor)) != JS_OK ||
        js_set(j->ctx, global, "SVGElement",
               js_object_value(svg_element_ctor)) != JS_OK ||
        js_set(j->ctx, global, "DocumentFragment",
               js_object_value(fragment_ctor)) != JS_OK ||
        js_set(j->ctx, global, "Document",
               js_object_value(document_ctor)) != JS_OK ||
        js_set(j->ctx, global, "HTMLDocument",
               js_object_value(html_document_ctor)) != JS_OK ||
        js_set(j->ctx, global, "MutationObserver",
               js_object_value(mutation_observer_ctor)) != JS_OK ||
        js_set(j->ctx, global, "MutationRecord",
               js_object_value(mutation_record_ctor)) != JS_OK ||
        js_set(j->ctx, global, "DOMException",
               js_object_value(dom_exception_ctor)) != JS_OK ||
        js_set(j->ctx, global, "Attr",
               js_object_value(attr_ctor)) != JS_OK ||
        js_set(j->ctx, global, "NamedNodeMap",
               js_object_value(named_node_map_ctor)) != JS_OK ||
        js_set(j->ctx, global, "NodeIterator",
               js_object_value(node_iterator_ctor)) != JS_OK ||
        js_set(j->ctx, global, "TreeWalker",
               js_object_value(tree_walker_ctor)) != JS_OK ||
        js_set(j->ctx, global, "Range",
               js_object_value(range_ctor)) != JS_OK ||
        js_set(j->ctx, global, "Selection",
               js_object_value(selection_ctor)) != JS_OK ||
        js_set(j->ctx, global, "NodeFilter",
               js_object_value(node_filter)) != JS_OK ||
        js_set(j->ctx, global, "Event",
               js_object_value(event_ctor)) != JS_OK ||
        js_set(j->ctx, global, "CustomEvent",
               js_object_value(custom_event_ctor)) != JS_OK ||
        js_set(j->ctx, global, "UIEvent",
               js_object_value(ui_event_ctor)) != JS_OK ||
        js_set(j->ctx, global, "MouseEvent",
               js_object_value(mouse_event_ctor)) != JS_OK ||
        js_set(j->ctx, global, "KeyboardEvent",
               js_object_value(keyboard_event_ctor)) != JS_OK ||
        js_set(j->ctx, global, "FocusEvent",
               js_object_value(focus_event_ctor)) != JS_OK ||
        js_set(j->ctx, global, "InputEvent",
               js_object_value(input_event_ctor)) != JS_OK ||
        js_set(j->ctx, global, "PointerEvent",
               js_object_value(pointer_event_ctor)) != JS_OK ||
        js_set(j->ctx, global, "Headers",
               js_object_value(headers_ctor)) != JS_OK ||
        js_set(j->ctx, global, "Response",
               js_object_value(response_ctor)) != JS_OK ||
        js_set(j->ctx, global, "Request",
               js_object_value(request_ctor)) != JS_OK ||
        js_set(j->ctx, global, "File",
               js_object_value(file_ctor)) != JS_OK ||
        js_set(j->ctx, global, "FileReader",
               js_object_value(file_reader_ctor)) != JS_OK ||
        js_set(j->ctx, global, "navigator", js_object_value(navobj)) != JS_OK ||
        js_set(j->ctx, global, "history",
               js_object_value(historyobj)) != JS_OK ||
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
    unsigned int k;
    int i;
    if (!j) return;
    if (j->doc && j->doc->root)
        for (n = j->doc->root; n; n = dom_next(n))
            n->user = 0;
    for (k = 0; k < j->parsed_doc_count; k++) {
        if (j->parsed_docs[k] && j->parsed_docs[k]->root)
            for (n = j->parsed_docs[k]->root; n; n = dom_next(n))
                n->user = 0;
    }
    js_free(j->ctx);
    if (j->doc && j->doc->root)
        free_subtree_canvases(j->doc->root);
    for (k = 0; k < j->parsed_doc_count; k++) {
        if (j->parsed_docs[k]) {
            free_subtree_canvases(j->parsed_docs[k]->root);
            dom_document_free(j->parsed_docs[k]);
        }
    }
    free(j->parsed_docs);
    free(j->dataset_nodes);
    css_free(j->author_sheet);
    free(j->clipboard_text);
    for (i = 0; i < JSDOM_FETCH_MAX; i++) {
        free(j->fetches[i].url);
        free(j->fetches[i].method);
        free(j->fetches[i].body);
        free(j->fetches[i].content_type);
        free(j->fetches[i].accept);
        free(j->fetches[i].extra_headers);
    }
    for (i = 0; i < JSDOM_OBJECT_URL_MAX; i++) {
        free(j->object_urls[i].url);
        free(j->object_urls[i].data);
    }
    if (j->owns_session_storage)
        web_storage_free(j->session_storage);
    if (j->owns_local_storage)
        web_storage_free(j->local_storage);
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
    dataset_sync_all(j);
    if (rc == JS_OK)
        return 0;
    msg = js_error_text(j->ctx, r);
    if (err && errsz)
        snprintf(err, errsz, "%s", msg ? msg : "JavaScript exception");
    return -1;
}

static int dispatch_listeners(struct jsdom *j, js_value tv, js_value event,
                              const char *type, int capture, char *err,
                              unsigned long errsz)
{
    js_value listeners, entry, callback, fn, receiver, result, stopped;
    char key[96];
    unsigned long i, count;

    snprintf(key, sizeof(key), "__listeners_%s", type);
    if (js_get(j->ctx, tv, key, &listeners) != JS_OK ||
        !js_is_array(listeners))
        return 1;
    count = js_array_length(listeners.u.obj);
    for (i = 0; i < count; i++) {
        js_value once_value = js_bool(0), passive_value = js_bool(0);
        js_value signal = js_undefined();
        int entry_capture;

        if (js_array_get(j->ctx, listeners.u.obj, i, &entry) != JS_OK)
            return 0;
        if (js_is_undefined(entry))
            continue;
        if (listener_entry_fields(j->ctx, entry, &callback,
                                  &entry_capture) != JS_OK)
            return 0;
        if (entry_capture != capture)
            continue;
        if (!js_is_function(entry) &&
            (js_get(j->ctx, entry, "once", &once_value) != JS_OK ||
             js_get(j->ctx, entry, "passive", &passive_value) != JS_OK ||
             js_get(j->ctx, entry, "signal", &signal) != JS_OK))
            return 0;
        if (js_abortsignal_aborted(signal)) {
            if (js_set_value(j->ctx, listeners, js_number((double)i),
                             js_undefined()) != JS_OK)
                return 0;
            continue;
        }
        if (!event_callback(j->ctx, callback, &fn, &receiver))
            continue;
        if (js_to_boolean(once_value) &&
            js_set_value(j->ctx, listeners, js_number((double)i),
                         js_undefined()) != JS_OK)
            return 0;
        if (js_set(j->ctx, event, "__inPassiveListener",
                   js_bool(js_to_boolean(passive_value))) != JS_OK)
            return 0;
        js_reset_steps(j->ctx);
        if (js_call(j->ctx, fn,
                    js_is_function(callback) ? tv : receiver,
                    1, &event, &result) != JS_OK) {
            const char *msg;

            js_set(j->ctx, event, "__inPassiveListener", js_bool(0));
            msg = js_error_text(j->ctx, js_exception(j->ctx));
            if (err && errsz)
                snprintf(err, errsz, "%s", msg);
            return 0;
        }
        if (js_set(j->ctx, event, "__inPassiveListener",
                   js_bool(0)) != JS_OK ||
            js_get(j->ctx, event, "__immediateStopped", &stopped) !=
                JS_OK ||
            js_to_boolean(stopped))
            break;
    }
    return 1;
}

static int dispatch_event_value(struct jsdom *j, js_value target,
                                js_value event, struct dom_node *node,
                                char *err, unsigned long errsz)
{
    js_value type_value, dispatching, bubbles, stopped;
    js_value handler, result, current, path_value;
    js_object *path;
    const char *type;
    struct dom_node *ancestors[DOM_MAX_DEPTH + 1];
    struct dom_node *cursor = node;
    unsigned int ancestor_count = 0, ancestor_index;
    char attr[80];
    int first = 1;
    int reaches_document = 0;

    if (js_get(j->ctx, event, "type", &type_value) != JS_OK ||
        !js_is_string(type_value))
    {
        js_throw_error(j->ctx, JS_ERR_TYPE,
                       "dispatchEvent requires an Event");
        return -1;
    }
    type = js_string_bytes(type_value, 0);
    if (!type || !*type || strlen(type) > 64)
    {
        js_throw_error(j->ctx, JS_ERR_TYPE,
                       "Event type is empty or too long");
        return -1;
    }
    if (js_get(j->ctx, event, "__dispatching", &dispatching) != JS_OK)
        return -1;
    if (js_to_boolean(dispatching)) {
        js_throw_error(j->ctx, JS_ERR_ERROR,
                       "Event is already being dispatched");
        return -1;
    }
    if (js_set(j->ctx, event, "__dispatching", js_bool(1)) != JS_OK ||
        js_set(j->ctx, event, "__stopped", js_bool(0)) != JS_OK ||
        js_set(j->ctx, event, "__immediateStopped", js_bool(0)) != JS_OK ||
        js_set(j->ctx, event, "__inPassiveListener", js_bool(0)) != JS_OK ||
        js_set(j->ctx, event, "target", target) != JS_OK)
        return -1;

    if (node) {
        for (cursor = node->parent;
             cursor && ancestor_count < DOM_MAX_DEPTH + 1;
             cursor = cursor->parent)
            ancestors[ancestor_count++] = cursor;
        reaches_document = node == j->doc->root ||
            (ancestor_count &&
             ancestors[ancestor_count - 1] == j->doc->root);
    }
    path = js_new_array(j->ctx);
    if (!path ||
        js_array_push(j->ctx, path, target) != JS_OK)
        goto fail;
    for (ancestor_index = 0; ancestor_index < ancestor_count;
         ancestor_index++) {
        if (ancestors[ancestor_index] == j->doc->root) {
            if (js_get(j->ctx, js_global(j->ctx),
                       "document", &current) != JS_OK)
                goto fail;
        } else {
            current = wrap_node(j, ancestors[ancestor_index]);
        }
        if (!js_is_object(current) ||
            js_array_push(j->ctx, path, current) != JS_OK)
            goto fail;
    }
    if (reaches_document &&
        js_array_push(j->ctx, path, js_global(j->ctx)) != JS_OK)
        goto fail;
    path_value = js_object_value(path);
    if (js_set(j->ctx, event, "__path", path_value) != JS_OK)
        goto fail;

    if (node) {
        if (reaches_document) {
            current = js_global(j->ctx);
            if (js_set(j->ctx, event, "currentTarget", current) != JS_OK ||
                js_set(j->ctx, event, "eventPhase", js_number(1)) != JS_OK ||
                !dispatch_listeners(j, current, event, type, 1,
                                    err, errsz) ||
                js_get(j->ctx, event, "__stopped", &stopped) != JS_OK)
                goto fail;
            if (js_to_boolean(stopped))
                goto finish;
        }
        for (ancestor_index = ancestor_count; ancestor_index > 0;
             ancestor_index--) {
            if (ancestors[ancestor_index - 1] == j->doc->root) {
                if (js_get(j->ctx, js_global(j->ctx),
                           "document", &current) != JS_OK)
                    goto fail;
            } else {
                current = wrap_node(j, ancestors[ancestor_index - 1]);
            }
            if (!js_is_object(current) ||
                js_set(j->ctx, event, "currentTarget", current) != JS_OK ||
                js_set(j->ctx, event, "eventPhase", js_number(1)) != JS_OK ||
                !dispatch_listeners(j, current, event, type, 1,
                                    err, errsz) ||
                js_get(j->ctx, event, "__stopped", &stopped) != JS_OK)
                goto fail;
            if (js_to_boolean(stopped))
                goto finish;
        }
    }

    cursor = node;
    current = target;
    for (;;) {
        if (js_set(j->ctx, event, "currentTarget", current) != JS_OK ||
            js_set(j->ctx, event, "eventPhase",
                   js_number(first ? 2 : 3)) != JS_OK)
            goto fail;
        if (first &&
            !dispatch_listeners(j, current, event, type, 1, err, errsz))
            goto fail;
        if (js_get(j->ctx, event, "__immediateStopped", &stopped) != JS_OK)
            goto fail;
        if (!js_to_boolean(stopped) &&
            !dispatch_listeners(j, current, event, type, 0, err, errsz))
            goto fail;
        snprintf(attr, sizeof(attr), "on%s", type);
        if (js_get(j->ctx, event, "__immediateStopped", &stopped) != JS_OK)
            goto fail;
        if (!js_to_boolean(stopped) &&
            js_get(j->ctx, current, attr, &handler) == JS_OK &&
            js_is_function(handler)) {
            js_reset_steps(j->ctx);
            if (js_call(j->ctx, handler, current, 1, &event, &result) !=
                    JS_OK)
                goto fail;
            if (js_is_bool(result) && !result.u.b &&
                event_prevent(j->ctx, event, 0, 0, &result) != JS_OK)
                goto fail;
        } else if (first && node && node->type == DOM_ELEMENT) {
            const char *inline_code = dom_get_attr(node, attr);

            if (inline_code && *inline_code) {
                unsigned long length = strlen(inline_code) + 80;
                char *source = (char *)malloc(length);

                if (!source) {
                    js_throw_error(j->ctx, JS_ERR_ERROR, "out of memory");
                    goto fail;
                }
                if (js_set(j->ctx, js_global(j->ctx),
                           "__kestrelTarget", current) != JS_OK ||
                    js_set(j->ctx, js_global(j->ctx), "event", event) !=
                        JS_OK) {
                    free(source);
                    goto fail;
                }
                snprintf(source, length,
                         "(function(event){%s}).call(__kestrelTarget,event)",
                         inline_code);
                js_reset_steps(j->ctx);
                if (js_eval(j->ctx, source,
                            "inline event handler", &result) != JS_OK) {
                    free(source);
                    goto fail;
                }
                free(source);
                if (js_is_bool(result) && !result.u.b &&
                    event_prevent(j->ctx, event, 0, 0, &result) != JS_OK)
                    goto fail;
            }
        }
        if (js_get(j->ctx, event, "__stopped", &stopped) != JS_OK ||
            js_get(j->ctx, event, "bubbles", &bubbles) != JS_OK)
            goto fail;
        if (js_to_boolean(stopped) || !js_to_boolean(bubbles) ||
            !cursor || !cursor->parent)
            break;
        cursor = cursor->parent;
        if (cursor == j->doc->root) {
            if (js_get(j->ctx, js_global(j->ctx),
                       "document", &current) != JS_OK)
                goto fail;
        } else {
            current = wrap_node(j, cursor);
        }
        if (!js_is_object(current))
            goto fail;
        first = 0;
    }
    if (reaches_document && !js_to_boolean(stopped) &&
        js_to_boolean(bubbles)) {
        current = js_global(j->ctx);
        if (js_set(j->ctx, event, "currentTarget", current) != JS_OK ||
            js_set(j->ctx, event, "eventPhase", js_number(3)) != JS_OK ||
            !dispatch_listeners(j, current, event, type, 0, err, errsz) ||
            js_get(j->ctx, event, "__immediateStopped", &stopped) != JS_OK)
            goto fail;
        snprintf(attr, sizeof(attr), "on%s", type);
        if (!js_to_boolean(stopped) &&
            js_get(j->ctx, current, attr, &handler) == JS_OK &&
            js_is_function(handler)) {
            js_reset_steps(j->ctx);
            if (js_call(j->ctx, handler, current, 1, &event, &result) !=
                    JS_OK)
                goto fail;
            if (js_is_bool(result) && !result.u.b &&
                event_prevent(j->ctx, event, 0, 0, &result) != JS_OK)
                goto fail;
        }
    }
finish:
    if (js_set(j->ctx, event, "currentTarget", js_null()) != JS_OK ||
        js_set(j->ctx, event, "eventPhase", js_number(0)) != JS_OK ||
        js_set(j->ctx, event, "__inPassiveListener", js_bool(0)) != JS_OK ||
        js_set(j->ctx, event, "__path", js_undefined()) != JS_OK ||
        js_set(j->ctx, event, "__dispatching", js_bool(0)) != JS_OK ||
        js_get(j->ctx, event, "defaultPrevented", &result) != JS_OK)
        return -1;
    return !js_to_boolean(result);

fail:
    js_set(j->ctx, event, "currentTarget", js_null());
    js_set(j->ctx, event, "eventPhase", js_number(0));
    js_set(j->ctx, event, "__inPassiveListener", js_bool(0));
    js_set(j->ctx, event, "__path", js_undefined());
    js_set(j->ctx, event, "__dispatching", js_bool(0));
    if (err && errsz && js_exception(j->ctx).type != JS_UNDEFINED)
        snprintf(err, errsz, "%s",
                 js_error_text(j->ctx, js_exception(j->ctx)));
    return -1;
}

int jsdom_dispatch(struct jsdom *j, struct dom_node *target,
                   const char *type, char *err, unsigned long errsz)
{
    js_value tv, event, options;
    js_object *option_object;
    int allowed, event_status;

    if (!j || !target || !type || strlen(type) > 64)
        return 1;
    tv = wrap_node(j, target);
    if (!js_is_object(tv))
        return 1;
    option_object = js_new_object(j->ctx);
    if (!option_object)
        return 1;
    options = js_object_value(option_object);
    if (js_set(j->ctx, options, "bubbles", js_bool(1)) != JS_OK ||
        js_set(j->ctx, options, "cancelable", js_bool(1)) != JS_OK)
        return 1;
    if (!strcmp(type, "click") || !strcmp(type, "dblclick") ||
        !strcmp(type, "mousedown") || !strcmp(type, "mouseup") ||
        !strcmp(type, "mousemove") || !strcmp(type, "mouseenter") ||
        !strcmp(type, "mouseleave") || !strcmp(type, "mouseover") ||
        !strcmp(type, "mouseout") || !strcmp(type, "contextmenu") ||
        !strcmp(type, "auxclick"))
        event_status = derived_event_make(
            j, j->mouse_event_proto, type, options,
            DERIVED_MOUSE_EVENT, &event);
    else if (!strcmp(type, "input") || !strcmp(type, "beforeinput"))
        event_status = derived_event_make(
            j, j->input_event_proto, type, options,
            DERIVED_INPUT_EVENT, &event);
    else if (!strcmp(type, "focus") || !strcmp(type, "blur") ||
             !strcmp(type, "focusin") || !strcmp(type, "focusout"))
        event_status = derived_event_make(
            j, j->focus_event_proto, type, options,
            DERIVED_FOCUS_EVENT, &event);
    else if (!strncmp(type, "pointer", 7))
        event_status = derived_event_make(
            j, j->pointer_event_proto, type, options,
            DERIVED_POINTER_EVENT, &event);
    else
        event_status = event_make(
            j, j->event_proto, type, options,
            js_undefined(), 0, &event);
    if (event_status != JS_OK ||
        js_set(j->ctx, event, "isTrusted", js_bool(1)) != JS_OK)
        return 1;
    allowed = dispatch_event_value(j, tv, event, target, err, errsz);
    return allowed < 0 ? 1 : allowed;
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
    dispatch_listeners(j, doc, event, type, 0, err, errsz);
    dispatch_listeners(j, js_global(j->ctx), event, type, 0, err, errsz);
    if (js_get(j->ctx, event, "defaultPrevented", &result) == JS_OK)
        return !js_to_boolean(result);
    return 1;
}

static int object_url_fetch(struct jsdom *j, const char *url,
                            const char *method,
                            struct jsdom_fetch_response *out,
                            char *err, unsigned long errsz)
{
    struct object_url_entry *entry = 0;
    unsigned long length;
    int i;

    if (strncmp(url, "blob:", 5))
        return 0;
    for (i = 0; i < JSDOM_OBJECT_URL_MAX; i++)
        if (j->object_urls[i].url &&
            !strcmp(j->object_urls[i].url, url)) {
            entry = &j->object_urls[i];
            break;
        }
    if (!entry) {
        snprintf(err, errsz, "Blob URL has been revoked or is unknown");
        return -1;
    }
    if (strcmp(method, "GET") && strcmp(method, "HEAD")) {
        snprintf(err, errsz, "Blob URL fetch supports GET and HEAD only");
        return -1;
    }
    length = !strcmp(method, "HEAD") ? 0 : entry->length;
    out->status = 200;
    out->status_text = dup_z("OK");
    out->url = dup_z(url);
    out->content_type = dup_z(entry->type);
    out->body = dup_n(length ? entry->data : "", length);
    out->body_len = length;
    if (!out->status_text || !out->url || !out->content_type ||
        !out->body) {
        fetch_response_clear(out);
        snprintf(err, errsz, "out of memory fetching Blob URL");
        return -1;
    }
    return 1;
}

static int pump_fetch(struct jsdom *j, int i)
{
    struct jsdom_fetch_response f;
    char message[192];
    js_value value;
    int rc;

    if (!j || i < 0 || i >= JSDOM_FETCH_MAX ||
        j->fetches[i].state != TASK_QUEUED)
        return 0;
    if (js_abortsignal_aborted(j->fetches[i].abort_signal)) {
        rc = js_promise_reject(j->ctx, j->fetches[i].promise,
                               js_abortsignal_reason(j->fetches[i].abort_signal));
        fetch_task_clear(j, i);
        return rc == JS_OK ? 1 : -1;
    }
    j->fetches[i].state = TASK_RUNNING;
    memset(&f, 0, sizeof(f));
    message[0] = 0;
    rc = object_url_fetch(j, j->fetches[i].url,
                          j->fetches[i].method, &f,
                          message, sizeof(message));
    if (rc > 0) {
        rc = 0;
    } else if (rc < 0) {
        rc = -1;
    } else if (!j->fetch) {
        rc = -1;
        snprintf(message, sizeof(message), "fetch is unavailable");
    } else {
        rc = j->fetch(j->print_user, j->fetches[i].url,
                      j->fetches[i].method,
                      j->fetches[i].body, j->fetches[i].body_len,
                      j->fetches[i].content_type,
                      j->fetches[i].accept,
                      j->fetches[i].extra_headers,
                      j->fetches[i].mode,
                      j->fetches[i].credentials,
                      j->fetches[i].redirect,
                      &f, message, sizeof(message));
    }
    j->fetches[i].state = TASK_SETTLING;
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
    fetch_task_clear(j, i);
    return rc == JS_OK ? 1 : -1;
}

static int pump_file_reader(struct jsdom *j, int index,
                            char *err, unsigned long errsz)
{
    struct file_reader_task *task;
    struct hostref *reader_host;
    struct file_reader_state *reader;
    js_value reader_value;
    unsigned int generation;

    if (!j || index < 0 ||
        index >= (int)(sizeof(j->file_readers) /
                       sizeof(j->file_readers[0])))
        return 0;
    task = &j->file_readers[index];
    if (task->state != TASK_QUEUED)
        return 0;
    reader_value = task->reader;
    generation = task->generation;
    memset(task, 0, sizeof(*task));
    task->reader = js_undefined();
    reader_host = host(reader_value, HOST_FILE_READER);
    if (!reader_host)
        return 1;
    reader = (struct file_reader_state *)reader_host->data;
    if (reader->generation != generation)
        return 1;
    if (reader->aborted) {
        if (file_reader_dispatch(j, reader_value, "abort", 0,
                                 reader->total, err, errsz) != 0 ||
            file_reader_dispatch(j, reader_value, "loadend", 0,
                                 reader->total, err, errsz) != 0)
            return -1;
        return 1;
    }
    if (reader->ready_state != 1)
        return 1;
    if (file_reader_dispatch(j, reader_value, "loadstart", 0,
                             reader->total, err, errsz) != 0)
        return -1;
    if (reader->generation != generation)
        return 1;
    if (reader->aborted) {
        if (file_reader_dispatch(j, reader_value, "abort", 0,
                                 reader->total, err, errsz) != 0 ||
            file_reader_dispatch(j, reader_value, "loadend", 0,
                                 reader->total, err, errsz) != 0)
            return -1;
        return 1;
    }
    if (file_reader_dispatch(j, reader_value, "progress",
                             reader->total, reader->total,
                             err, errsz) != 0)
        return -1;
    if (reader->generation != generation)
        return 1;
    if (reader->aborted) {
        if (file_reader_dispatch(j, reader_value, "abort", 0,
                                 reader->total, err, errsz) != 0 ||
            file_reader_dispatch(j, reader_value, "loadend", 0,
                                 reader->total, err, errsz) != 0)
            return -1;
        return 1;
    }
    reader->result = reader->pending_result;
    reader->pending_result = js_null();
    reader->ready_state = 2;
    if (file_reader_dispatch(j, reader_value, "load",
                             reader->total, reader->total,
                             err, errsz) != 0 ||
        file_reader_dispatch(j, reader_value, "loadend",
                             reader->total, reader->total,
                             err, errsz) != 0)
        return -1;
    return 1;
}

static int observer_has_pending(const struct jsdom *j)
{
    unsigned int i, k;

    for (i = 0; i < JSDOM_OBSERVERS_MAX; i++) {
        const struct resize_observer_state *ro = j->resize_observers[i];
        const struct intersection_observer_state *io =
            j->intersection_observers[i];

        if (ro) {
            for (k = 0; k < ro->target_count; k++)
                if (ro->targets[k].pending)
                    return 1;
        }
        if (io) {
            for (k = 0; k < io->target_count; k++)
                if (io->targets[k].pending)
                    return 1;
        }
    }
    return 0;
}

static int jsdom_needs_immediate_turn(struct jsdom *j, int jobs)
{
    int i;

    if (jobs >= 256)
        return 1;
    for (i = 0; i < JSDOM_FETCH_MAX; i++)
        if (j->fetches[i].state == TASK_QUEUED)
            return 1;
    for (i = 0;
         i < (int)(sizeof(j->file_readers) /
                   sizeof(j->file_readers[0])); i++)
        if (j->file_readers[i].state == TASK_QUEUED)
            return 1;
    for (i = 0;
         i < (int)(sizeof(j->timers) / sizeof(j->timers[0])); i++)
        if (j->timers[i].state == TASK_QUEUED &&
            !j->timers[i].repeats &&
            j->timers[i].due_turn <= j->event_turn + 1)
            return 1;
    for (i = 0; i < JSDOM_MUTATION_OBSERVERS_MAX; i++)
        if (j->mutation_observers[i] &&
            j->mutation_observers[i]->record_count)
            return 1;
    if (observer_has_pending(j))
        return 1;
    return 0;
}

static int deliver_mutations(struct jsdom *j)
{
    int i, delivered = 0;

    for (i = 0; i < JSDOM_MUTATION_OBSERVERS_MAX; i++) {
        struct mutation_observer_state *state = j->mutation_observers[i];
        js_value args[2], ignored;

        if (!state || !state->registration_count ||
            !state->record_count)
            continue;
        if (mutation_take_records(j->ctx, state, &args[0]) != JS_OK)
            return -1;
        args[1] = state->self;
        js_reset_steps(j->ctx);
        if (js_call(j->ctx, state->callback, state->self,
                    2, args, &ignored) != JS_OK)
            return -1;
        delivered++;
    }
    return delivered;
}

static int deliver_resize_observers(struct jsdom *j)
{
    unsigned int i, k;
    int delivered = 0;

    for (i = 0; i < JSDOM_OBSERVERS_MAX; i++) {
        struct resize_observer_state *st = j->resize_observers[i];
        js_object *entries;
        js_value args[2], ignored;
        unsigned long n_entries = 0;

        if (!st || !st->target_count)
            continue;
        entries = js_new_array(j->ctx);
        if (!entries)
            return -1;
        for (k = 0; k < st->target_count; k++) {
            float x = 0, y = 0, w = 0, h = 0;
            js_object *entry;
            js_value rect_val;

            (void)node_box_rect(j, st->targets[k].node, &x, &y, &w, &h);
            if (!st->targets[k].pending &&
                w == st->targets[k].last_w &&
                h == st->targets[k].last_h)
                continue;
            st->targets[k].pending = 0;
            st->targets[k].last_w = w;
            st->targets[k].last_h = h;
            entry = js_new_object(j->ctx);
            if (!entry ||
                make_dom_rect(j->ctx, 0.0, 0.0, (double)w, (double)h,
                              &rect_val) != JS_OK ||
                js_set(j->ctx, js_object_value(entry), "target",
                       wrap_node(j, st->targets[k].node)) != JS_OK ||
                js_set(j->ctx, js_object_value(entry), "contentRect",
                       rect_val) != JS_OK ||
                js_array_push(j->ctx, entries,
                              js_object_value(entry)) != JS_OK)
                return -1;
            n_entries++;
        }
        if (!n_entries)
            continue;
        args[0] = js_object_value(entries);
        args[1] = st->self;
        js_reset_steps(j->ctx);
        if (js_call(j->ctx, st->callback, st->self,
                    2, args, &ignored) != JS_OK)
            return -1;
        delivered++;
    }
    return delivered;
}

static int deliver_intersection_observers(struct jsdom *j)
{
    unsigned int i, k;
    int delivered = 0;

    for (i = 0; i < JSDOM_OBSERVERS_MAX; i++) {
        struct intersection_observer_state *st = j->intersection_observers[i];
        js_object *entries;
        js_value args[2], ignored, root_bounds;
        double rx0, ry0, rx1, ry1, rw, rh;
        unsigned long n_entries = 0;
        int has_pending = 0;

        if (!st || !st->target_count)
            continue;
        for (k = 0; k < st->target_count; k++)
            if (st->targets[k].pending) { has_pending = 1; break; }
        if (!has_pending)
            continue;
        if (st->root) {
            float fx, fy, fw, fh;
            (void)node_box_rect(j, st->root, &fx, &fy, &fw, &fh);
            rx0 = (double)fx; ry0 = (double)fy;
            rw = (double)fw; rh = (double)fh;
            rx1 = rx0 + rw; ry1 = ry0 + rh;
        } else {
            rx0 = j->scroll_x;
            ry0 = j->scroll_y;
            rw = (double)j->viewport_width;
            rh = (double)j->viewport_height;
            rx1 = rx0 + rw;
            ry1 = ry0 + rh;
        }
        if (make_dom_rect(j->ctx, rx0, ry0, rw, rh, &root_bounds) != JS_OK)
            return -1;
        entries = js_new_array(j->ctx);
        if (!entries)
            return -1;
        for (k = 0; k < st->target_count; k++) {
            float fx = 0, fy = 0, fw = 0, fh = 0;
            double bx, by, bw, bh, ix0, iy0, ix1, iy1, iw = 0, ih = 0;
            double ratio = 0.0;
            int connected, has_box, is_int = 0;
            js_object *entry;
            js_value brect, irect;

            if (!st->targets[k].pending)
                continue;
            st->targets[k].pending = 0;
            connected = node_is_in_main_dom(j, st->targets[k].node);
            has_box = node_box_rect(j, st->targets[k].node, &fx, &fy, &fw, &fh);
            bx = (double)fx; by = (double)fy;
            bw = (double)fw; bh = (double)fh;
            if (connected) {
                if (!has_box && bw == 0.0 && bh == 0.0) {
                    is_int = 1;
                    ratio = 1.0;
                    ix0 = bx; iy0 = by;
                } else {
                    ix0 = bx > rx0 ? bx : rx0;
                    iy0 = by > ry0 ? by : ry0;
                    ix1 = (bx + bw) < rx1 ? (bx + bw) : rx1;
                    iy1 = (by + bh) < ry1 ? (by + bh) : ry1;
                    if (ix1 > ix0 && iy1 > iy0) {
                        iw = ix1 - ix0;
                        ih = iy1 - iy0;
                        is_int = 1;
                        ratio = (bw * bh > 0.0)
                            ? (iw * ih) / (bw * bh) : 1.0;
                        if (ratio > 1.0) ratio = 1.0;
                    } else if ((bw == 0.0 || bh == 0.0) &&
                               bx >= rx0 && bx <= rx1 &&
                               by >= ry0 && by <= ry1) {
                        is_int = 1;
                        ratio = 1.0;
                        ix0 = bx; iy0 = by;
                    } else {
                        ix0 = 0.0; iy0 = 0.0;
                    }
                }
            } else {
                ix0 = 0.0; iy0 = 0.0;
            }
            entry = js_new_object(j->ctx);
            if (!entry ||
                make_dom_rect(j->ctx, bx, by, bw, bh, &brect) != JS_OK ||
                make_dom_rect(j->ctx, ix0, iy0, iw, ih, &irect) != JS_OK ||
                js_set(j->ctx, js_object_value(entry), "target",
                       wrap_node(j, st->targets[k].node)) != JS_OK ||
                js_set(j->ctx, js_object_value(entry), "isIntersecting",
                       js_bool(is_int)) != JS_OK ||
                js_set(j->ctx, js_object_value(entry), "intersectionRatio",
                       js_number(ratio)) != JS_OK ||
                js_set(j->ctx, js_object_value(entry), "boundingClientRect",
                       brect) != JS_OK ||
                js_set(j->ctx, js_object_value(entry), "intersectionRect",
                       irect) != JS_OK ||
                js_set(j->ctx, js_object_value(entry), "rootBounds",
                       root_bounds) != JS_OK ||
                js_set(j->ctx, js_object_value(entry), "time",
                       js_number((double)(j->event_turn * 200UL))) != JS_OK ||
                js_array_push(j->ctx, entries,
                              js_object_value(entry)) != JS_OK)
                return -1;
            n_entries++;
        }
        if (!n_entries)
            continue;
        args[0] = js_object_value(entries);
        args[1] = st->self;
        js_reset_steps(j->ctx);
        if (js_call(j->ctx, st->callback, st->self,
                    2, args, &ignored) != JS_OK)
            return -1;
        delivered++;
    }
    return delivered;
}

int jsdom_pump(struct jsdom *j, char *err, unsigned long errsz)
{
    int i, ran = 0, jobs, mutations, obs, pass;

    if (err && errsz)
        err[0] = 0;
    if (!j)
        return 0;
    for (pass = 0; pass < 64; pass++) {
    j->event_turn++;
    jobs = js_run_jobs(j->ctx, 256);
    if (jobs < 0)
        goto fail;
    ran += jobs;
    mutations = deliver_mutations(j);
    if (mutations < 0)
        goto fail;
    ran += mutations;
    obs = deliver_resize_observers(j);
    if (obs < 0) goto fail;
    ran += obs;
    obs = deliver_intersection_observers(j);
    if (obs < 0) goto fail;
    ran += obs;
    for (i = 0; i < (int)(sizeof(j->timers) / sizeof(j->timers[0])); i++) {
        js_value fn, result, special_arg;
        js_value *callback_args;
        int callback_argc;

        struct timer_task *task = &j->timers[i];

        if (task->state != TASK_QUEUED ||
            task->due_turn > j->event_turn)
            continue;
        if (js_is_abortsignal(task->abort_signal)) {
            js_value signal = task->abort_signal;
            js_value reason;

            task->state = TASK_RUNNING;
            if (!js_abortsignal_aborted(signal)) {
                if (make_timeout_reason(j->ctx, &reason) != JS_OK ||
                    js_abortsignal_trigger(j->ctx, signal, reason, 1) != JS_OK)
                    goto fail;
            }
            memset(task, 0, sizeof(*task));
            task->fn = js_undefined();
            ran++;
            continue;
        }
        fn = task->fn;
        task->state = TASK_RUNNING;
        callback_args = task->args;
        callback_argc = task->argc;
        if (task->callback_kind == 1) {
            special_arg = js_number((double)(j->event_turn * 200UL));
            callback_args = &special_arg;
            callback_argc = 1;
        } else if (task->callback_kind == 2) {
            js_object *deadline = js_new_object(j->ctx);

            if (!deadline ||
                js_set(j->ctx, js_object_value(deadline), "didTimeout",
                       js_bool(0)) != JS_OK ||
                js_define_native(j->ctx, deadline, "timeRemaining",
                                 idle_time_remaining, 0) != JS_OK)
                goto fail;
            special_arg = js_object_value(deadline);
            callback_args = &special_arg;
            callback_argc = 1;
        }
        js_reset_steps(j->ctx);
        if (js_call(j->ctx, fn, js_global(j->ctx),
                    callback_argc, callback_args, &result) != JS_OK) {
            if (err && errsz)
                snprintf(err, errsz, "%s",
                         js_error_text(j->ctx, js_exception(j->ctx)));
            return -1;
        }
        /* clearInterval() may have changed RUNNING to FREE from inside the
         * callback. Only an untouched repeating task is re-queued. */
        if (task->state == TASK_RUNNING && task->repeats) {
            task->state = TASK_QUEUED;
            task->due_turn = j->event_turn + task->interval_turns;
        } else if (task->state == TASK_RUNNING) {
            memset(task, 0, sizeof(*task));
            task->fn = js_undefined();
        }
        ran++;
    }
    for (i = 0; i < JSDOM_FETCH_MAX; i++)
        if (j->fetches[i].state == TASK_QUEUED) {
            int fetched = pump_fetch(j, i);

            if (fetched < 0)
                goto fail;
            ran += fetched;
        }
    for (i = 0;
         i < (int)(sizeof(j->file_readers) /
                   sizeof(j->file_readers[0])); i++)
        if (j->file_readers[i].state == TASK_QUEUED) {
            int read = pump_file_reader(j, i, err, errsz);

            if (read < 0)
                goto fail;
            ran += read;
        }
    mutations = deliver_mutations(j);
    if (mutations < 0)
        goto fail;
    ran += mutations;
    obs = deliver_resize_observers(j);
    if (obs < 0) goto fail;
    ran += obs;
    obs = deliver_intersection_observers(j);
    if (obs < 0) goto fail;
    ran += obs;
    jobs = js_run_jobs(j->ctx, 256);
    if (jobs < 0)
        goto fail;
    ran += jobs;
    dataset_sync_all(j);
    if (!jsdom_needs_immediate_turn(j, jobs))
        break;
    }
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

const char *jsdom_document_url(const struct jsdom *j)
{
    return j ? j->url : 0;
}

double jsdom_scroll_x(const struct jsdom *j)
{
    return j ? j->scroll_x : 0.0;
}

double jsdom_scroll_y(const struct jsdom *j)
{
    return j ? j->scroll_y : 0.0;
}

void jsdom_set_scroll(struct jsdom *j, double x, double y)
{
    if (!j) return;
    j->scroll_x = x < 0.0 ? 0.0 : x;
    j->scroll_y = y < 0.0 ? 0.0 : y;
}

void jsdom_notify_resize(struct jsdom *j, unsigned int vw, unsigned int vh,
                         char *err, unsigned long errsz)
{
    unsigned int i, k;
    js_value ev, global;
    char ebuf[160];

    if (!j)
        return;
    if (vw) j->viewport_width = vw;
    if (vh) j->viewport_height = vh;
    j->style_sheet_dirty = 1;
    for (i = 0; i < JSDOM_OBSERVERS_MAX; i++) {
        if (j->resize_observers[i]) {
            for (k = 0; k < j->resize_observers[i]->target_count; k++)
                j->resize_observers[i]->targets[k].pending = 1;
        }
        if (j->intersection_observers[i]) {
            for (k = 0; k < j->intersection_observers[i]->target_count; k++)
                j->intersection_observers[i]->targets[k].pending = 1;
        }
    }
    if (event_make(j, j->event_proto, "resize", js_undefined(),
                   js_undefined(), 0, &ev) == JS_OK) {
        global = js_global(j->ctx);
        ebuf[0] = 0;
        if (dispatch_event_value(j, global, ev, 0,
                                 err && errsz ? err : ebuf,
                                 err && errsz ? errsz : sizeof(ebuf)) < 0)
            return;
    }
    (void)jsdom_pump(j, err, errsz);
}
