/* html.c - the HTML5 tokenizer and tree builder. See libweb/dom.h.
 *
 * TOKENIZER
 *   A flat state machine over the byte stream, following the HTML5
 *   tokenizer states: data, tag open/name, the four attribute-value
 *   forms, self-closing tags, comments (including the bogus-comment
 *   path taken by "<!x>", "<?xml ...>" and "</ >"), DOCTYPE, CDATA, and
 *   the RCDATA / RAWTEXT / script-data families with their end-tag
 *   matching. Character references are decoded in data, in RCDATA and
 *   in attribute values, named and numeric, with the legacy
 *   missing-semicolon behaviour and the attribute-context suppression
 *   rule.
 *
 *   Deliberate deviations, all in the direction of "what browsers
 *   actually do with the pages this browser will see". They are
 *   deviations from the spec's letter, not accidents:
 *     - <noscript> content is parsed as markup rather than swallowed as
 *       raw text. There is no script engine, so its content is exactly
 *       what should be displayed.
 *     - A self-closing slash on an UNKNOWN element is honoured. HTML5
 *       ignores it; honouring it stops "<my-widget/>" near the top of a
 *       document from nesting the rest of the page inside itself. On
 *       known elements the slash is ignored, as the spec requires.
 *     - <template> content is parsed inline rather than into a separate
 *       document fragment.
 *     - A DOCTYPE's public and system identifiers are parsed and
 *       discarded; only the name is kept, and quirks mode is not
 *       modelled.
 *     - The adoption agency algorithm is approximated: a nested <a>
 *       closes the outer one, and an end tag for a formatting element
 *       simply pops to it.
 *
 * TREE BUILDER
 *   Not the full insertion-mode machine, but the parts real pages need:
 *   implied <html>/<head>/<body>, head-vs-body element routing,
 *   auto-closing of <p>, <li>, <dt>, <dd>, <tr>, <td>, <th>, <option>
 *   and <optgroup>, implicit <tbody>, void elements, approximated table
 *   foster parenting, and scope-aware end-tag matching (default scope,
 *   and the wider table scope for table end tags) so an unmatched
 *   </div> is ignored instead of unwinding the document.
 *
 * NOTHING HERE RECURSES. The open-element stack is an explicit heap
 * array, capped at DOM_MAX_OPEN, and the document depth cap is checked
 * BEFORE a node is created, so markup nested a million deep costs
 * bounded memory, no nodes past the cap, and a constant amount of C
 * stack. That matters: the user stack is 16 pages.
 */

#include <stdlib.h>
#include <string.h>

#include "dom.h"

/* ------------------------------------------------------------------ *
 * Small helpers
 * ------------------------------------------------------------------ */

static int h_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int h_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int h_alpha(int c)
{
    c = h_lower(c);
    return c >= 'a' && c <= 'z';
}

static int h_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int h_alnum(int c)
{
    return h_alpha(c) || h_digit(c);
}

/* ------------------------------------------------------------------ *
 * Growable byte buffers
 *
 * Five of them exist for the whole parse and are reused, so a document
 * with a hundred thousand tags performs a handful of reallocations
 * rather than a hundred thousand malloc/free pairs.
 * ------------------------------------------------------------------ */

struct buf {
    char *p;
    unsigned long len, cap;
};

static int buf_add(struct buf *b, const char *s, unsigned long n,
                   unsigned long max, int *oom)
{
    if (b->len >= max)
        return 0;
    if (n > max - b->len)
        n = max - b->len;
    if (b->len + n + 1 > b->cap) {
        unsigned long ncap = b->cap ? b->cap : 128;
        char *np;
        while (ncap < b->len + n + 1) {
            if (ncap > (~0UL) / 2) {
                *oom = 1;
                return 0;
            }
            ncap *= 2;
        }
        np = (char *)realloc(b->p, ncap);
        if (!np) {
            *oom = 1;
            return 0;
        }
        b->p = np;
        b->cap = ncap;
    }
    if (n)
        memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
    return 1;
}

static void buf_free(struct buf *b)
{
    free(b->p);
    b->p = 0;
    b->len = 0;
    b->cap = 0;
}

/* ------------------------------------------------------------------ *
 * Tokenizer states
 * ------------------------------------------------------------------ */

enum {
    S_DATA = 0, S_TAG_OPEN, S_END_TAG_OPEN, S_TAG_NAME,
    S_BEFORE_ATTR_NAME, S_ATTR_NAME, S_AFTER_ATTR_NAME,
    S_BEFORE_ATTR_VALUE, S_ATTR_VALUE_DQ, S_ATTR_VALUE_SQ,
    S_ATTR_VALUE_UQ, S_AFTER_ATTR_VALUE_Q, S_SELF_CLOSING,
    S_BOGUS_COMMENT, S_MARKUP_DECL,
    S_COMMENT_START, S_COMMENT_START_DASH, S_COMMENT,
    S_COMMENT_END_DASH, S_COMMENT_END, S_COMMENT_END_BANG,
    S_DOCTYPE, S_BEFORE_DOCTYPE_NAME, S_DOCTYPE_NAME, S_BOGUS_DOCTYPE,
    S_CDATA, S_CDATA_BRACKET, S_CDATA_END,
    S_RCDATA, S_RCDATA_LT, S_RCDATA_END_OPEN, S_RCDATA_END_NAME,
    S_RAWTEXT, S_RAWTEXT_LT, S_RAWTEXT_END_OPEN, S_RAWTEXT_END_NAME,
    S_SCRIPT, S_SCRIPT_LT, S_SCRIPT_END_OPEN, S_SCRIPT_END_NAME,
    S_SCRIPT_ESC_START, S_SCRIPT_ESC_START_DASH,
    S_SCRIPT_ESC, S_SCRIPT_ESC_DASH, S_SCRIPT_ESC_DASH_DASH,
    S_SCRIPT_ESC_LT, S_SCRIPT_ESC_END_OPEN, S_SCRIPT_ESC_END_NAME,
    S_SCRIPT_DESC_START, S_SCRIPT_DESC, S_SCRIPT_DESC_DASH,
    S_SCRIPT_DESC_DASH_DASH, S_SCRIPT_DESC_LT, S_SCRIPT_DESC_END
};

/* Where the insertion point is, in the coarse sense the tree builder
 * needs. The full HTML5 machine has twenty-three insertion modes; this
 * has three, plus per-tag rules. */
enum { PH_INITIAL = 0, PH_HEAD, PH_BODY };

struct tokattr {
    unsigned long noff, nlen;
    unsigned long voff, vlen;
};

struct open_ent {
    struct dom_node *node;      /* 0 when the depth cap kept it out    */
    const char *tag;            /* interned, so == compares names      */
    int tag_id;
    struct dom_node *prev_cur;  /* insertion point before this push    */
};

struct parser {
    const char *src;
    unsigned long len, pos;

    struct dom_document *doc;
    struct html_entity_index ents;

    int state;
    int oom;

    struct buf text;    /* pending character data                      */
    struct buf cdata;   /* comment / DOCTYPE name                       */
    struct buf name;    /* current tag name, lowercased                 */
    struct buf abuf;    /* packed attribute names and values            */
    struct buf tmp;     /* end-tag candidate in the raw-text states     */

    struct tokattr *attrs;
    unsigned long nattr, cattr;
    int attr_open, attr_named;

    int is_end_tag;
    int self_closing;

    struct buf last_start;      /* name of the last start tag emitted  */

    struct open_ent *open;
    unsigned long nopen;
    struct dom_node *cur;

    int phase;
    int flushing;               /* guards tb_text_flush re-entry       */
    int skip_lf;                /* swallow one newline after <pre>     */
    unsigned long max_text;
};

/* ------------------------------------------------------------------ *
 * Attribute accumulation
 * ------------------------------------------------------------------ */

#define ABUF_MAX (1024UL * 1024UL)

static void attr_close(struct parser *p)
{
    struct tokattr *a;

    if (!p->attr_open)
        return;
    a = &p->attrs[p->nattr - 1];
    if (!p->attr_named) {
        buf_add(&p->abuf, "", 1, ABUF_MAX, &p->oom);
        a->nlen = p->abuf.len - 1 - a->noff;
        a->voff = p->abuf.len;
        a->vlen = 0;
        p->attr_named = 1;
    }
    buf_add(&p->abuf, "", 1, ABUF_MAX, &p->oom);
    a->vlen = p->abuf.len - 1 - a->voff;
    p->attr_open = 0;
}

static void attr_name_done(struct parser *p)
{
    struct tokattr *a;

    if (!p->attr_open || p->attr_named)
        return;
    a = &p->attrs[p->nattr - 1];
    buf_add(&p->abuf, "", 1, ABUF_MAX, &p->oom);
    a->nlen = p->abuf.len - 1 - a->noff;
    a->voff = p->abuf.len;
    a->vlen = 0;
    p->attr_named = 1;
}

static void attr_start(struct parser *p)
{
    struct tokattr *a;

    attr_close(p);
    if (p->nattr >= DOM_MAX_ATTRS) {
        p->doc->truncated |= DOM_TRUNC_ATTRS;
        /* Keep parsing the tag - the bytes still have to be consumed -
         * but stop recording. attr_open stays 0 so the append helpers
         * below become no-ops. */
        return;
    }
    if (p->nattr == p->cattr) {
        unsigned long ncap = p->cattr ? p->cattr * 2 : 8;
        struct tokattr *na = (struct tokattr *)realloc(p->attrs,
                                                       ncap * sizeof *na);
        if (!na) {
            p->oom = 1;
            return;
        }
        p->attrs = na;
        p->cattr = ncap;
    }
    a = &p->attrs[p->nattr++];
    a->noff = p->abuf.len;
    a->nlen = 0;
    a->voff = 0;
    a->vlen = 0;
    p->attr_open = 1;
    p->attr_named = 0;
}

static void attr_putc(struct parser *p, int c)
{
    char ch = (char)c;

    if (!p->attr_open)
        return;
    buf_add(&p->abuf, &ch, 1, ABUF_MAX, &p->oom);
}

static void attr_put(struct parser *p, const char *s, unsigned long n)
{
    if (!p->attr_open)
        return;
    buf_add(&p->abuf, s, n, ABUF_MAX, &p->oom);
}

/* ------------------------------------------------------------------ *
 * Character references
 * ------------------------------------------------------------------ */

static void tb_chars(struct parser *p, const char *s, unsigned long n);

/* Consume the reference that starts at p->pos, which points just past
 * the '&'. Emits the replacement (or a literal '&') as character data,
 * or into the attribute buffer when `in_attr` - in which case the
 * legacy no-semicolon form is suppressed before '=' and alphanumerics.
 *
 * Decoded characters go through tb_chars() rather than straight into
 * the text buffer so that every rule keyed on "the next character
 * token" - the newline <pre> swallows, for instance - sees them. */
static void ref_consume(struct parser *p, int in_attr)
{
    unsigned long avail = p->len - p->pos;
    char utf[4];
    int n;

    if (avail == 0) {
        if (in_attr)
            attr_putc(p, '&');
        else
            tb_chars(p, "&", 1);
        return;
    }

    if (p->src[p->pos] == '#') {
        unsigned long i = p->pos + 1;
        int hex = 0;
        unsigned long cp = 0, start;

        if (i < p->len && (p->src[i] == 'x' || p->src[i] == 'X')) {
            hex = 1;
            i++;
        }
        start = i;
        while (i < p->len) {
            int c = h_lower((unsigned char)p->src[i]);
            int v;
            if (h_digit(c))
                v = c - '0';
            else if (hex && c >= 'a' && c <= 'f')
                v = c - 'a' + 10;
            else
                break;
            if (cp <= 0x10FFFFUL)
                cp = cp * (hex ? 16UL : 10UL) + (unsigned long)v;
            if (cp > 0x10FFFFUL)
                cp = 0x110000UL;    /* clamp; fixup turns it into FFFD */
            i++;
        }
        if (i == start) {
            /* "&#" or "&#x" with no digits: literal text. */
            if (in_attr)
                attr_putc(p, '&');
            else
                tb_chars(p, "&", 1);
            return;
        }
        if (i < p->len && p->src[i] == ';')
            i++;                    /* a missing ';' is tolerated */
        p->pos = i;
        n = dom_utf8_encode(html_numeric_fixup(cp), utf);
        if (in_attr)
            attr_put(p, utf, (unsigned long)n);
        else
            tb_chars(p, utf, (unsigned long)n);
        return;
    }

    {
        int used = 0, semi = 0;
        unsigned long cp = 0;

        if (html_entity_match(&p->ents, p->src + p->pos, avail, &used, &semi,
                              &cp)) {
            if (!semi && in_attr) {
                unsigned long after = p->pos + (unsigned long)used;
                if (after < p->len &&
                    (p->src[after] == '=' ||
                     h_alnum((unsigned char)p->src[after]))) {
                    attr_putc(p, '&');
                    return;
                }
            }
            p->pos += (unsigned long)used;
            n = dom_utf8_encode(cp, utf);
            if (in_attr)
                attr_put(p, utf, (unsigned long)n);
            else
                tb_chars(p, utf, (unsigned long)n);
            return;
        }
    }

    if (in_attr)
        attr_putc(p, '&');
    else
        tb_chars(p, "&", 1);
}

/* ------------------------------------------------------------------ *
 * Open-element stack
 * ------------------------------------------------------------------ */

static struct dom_node *cur_node(struct parser *p)
{
    return p->cur ? p->cur : p->doc->root;
}

static int cur_tag(struct parser *p)
{
    return p->nopen ? p->open[p->nopen - 1].tag_id : HTAG_UNKNOWN;
}

static int push_open(struct parser *p, struct dom_node *node,
                     const char *tag, int tag_id)
{
    struct open_ent *e;

    if (p->nopen >= DOM_MAX_OPEN) {
        p->doc->truncated |= DOM_TRUNC_OPEN;
        return 0;
    }
    e = &p->open[p->nopen++];
    e->node = node;
    e->tag = tag;
    e->tag_id = tag_id;
    e->prev_cur = p->cur;
    if (node)
        p->cur = node;
    return 1;
}

static void tb_text_flush(struct parser *p);

static void pop_open(struct parser *p)
{
    struct dom_node *node;

    if (p->nopen == 0)
        return;
    node = p->open[p->nopen - 1].node;
    /* An element the depth cap kept out of the tree never became the
     * insertion point, so popping it changes nothing and the character
     * data on either side of it is one run, not two. */
    if (node) {
        tb_text_flush(p);
        /* Flushing real text while <head> is open opens <body>, which
         * pops <head> on the way. If that already removed the entry
         * this call was about to pop, the work is done. */
        if (p->nopen == 0 || p->open[p->nopen - 1].node != node)
            return;
    }
    p->nopen--;
    p->cur = p->open[p->nopen].prev_cur;
}

static void pop_to(struct parser *p, unsigned long n)
{
    while (p->nopen > n) {
        unsigned long before = p->nopen;
        pop_open(p);
        /* Flushing text can open <body>, which restructures the stack
         * underneath this loop. When that happens the entry this call
         * set out to remove is already gone and the indices no longer
         * mean what they meant; stop rather than pop something else. */
        if (p->nopen == before)
            break;
    }
}

/* Topmost index whose tag_id is in `set`, or -1. */
static long find_open(struct parser *p, const int *set, int nset)
{
    long i;
    int k;

    for (i = (long)p->nopen - 1; i >= 0; i--) {
        for (k = 0; k < nset; k++) {
            if (p->open[i].tag_id == set[k])
                return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ *
 * Insertion
 * ------------------------------------------------------------------ */

static int is_table_context(int tag_id)
{
    return tag_id == HTAG_TABLE || tag_id == HTAG_TBODY ||
           tag_id == HTAG_TFOOT || tag_id == HTAG_THEAD ||
           tag_id == HTAG_TR;
}

/* Table foster parenting, approximated: content that has no business
 * inside a table is moved out in front of it rather than dropped or
 * left to corrupt the row structure. */
static int foster_target(struct parser *p, struct dom_node **parent,
                         struct dom_node **before)
{
    static const int tbl[1] = { HTAG_TABLE };
    long i;

    if (!is_table_context(cur_tag(p)))
        return 0;
    i = find_open(p, tbl, 1);
    if (i < 0)
        return 0;
    if (!p->open[i].node || !p->open[i].node->parent)
        return 0;
    *parent = p->open[i].node->parent;
    *before = p->open[i].node;
    return 1;
}

/* Is there room under the insertion point for another element AND a
 * text child inside it? */
static int depth_ok(struct parser *p)
{
    return cur_node(p)->depth + 1 < dom_limit_depth(p->doc);
}

static int place(struct parser *p, struct dom_node *n, int allow_foster)
{
    struct dom_node *parent = 0, *before = 0;

    if (!n)
        return 0;
    if (allow_foster && foster_target(p, &parent, &before)) {
        /* Moving an ELEMENT out of a table leaves the open-element
         * stack describing a nesting the tree no longer has: the
         * element is a sibling of the table, but the table still sits
         * between them on the stack, so a later scope-limited search
         * stops where a re-parse of the serialized tree would not.
         * Browsers have the same hole. Record it rather than pretend. */
        if (n->type == DOM_ELEMENT)
            p->doc->repairs |= DOM_REPAIR_FOSTER;
        return dom_insert_before(parent, n, before);
    }
    return dom_append_child(cur_node(p), n);
}

/* ------------------------------------------------------------------ *
 * Text nodes
 * ------------------------------------------------------------------ */

static int all_space(const char *s, unsigned long n)
{
    unsigned long i;

    for (i = 0; i < n; i++) {
        if (!h_space((unsigned char)s[i]))
            return 0;
    }
    return 1;
}

static void ensure_body(struct parser *p);

static void tb_text_flush(struct parser *p)
{
    struct dom_node *t;
    int ctag;
    int ws, loose;

    /* ensure_body() below can pop <head>, and popping calls back in
     * here. The guard makes the re-entry a no-op; the outer call still
     * owns the buffer. */
    if (p->text.len == 0 || p->flushing)
        return;
    ctag = cur_tag(p);
    ws = all_space(p->text.p, p->text.len);
    loose = (p->nopen == 0 || ctag == HTAG_HTML || ctag == HTAG_HEAD);

    /* Whitespace between </head> and <body>, or loose in <html>, has no
     * effect and would otherwise break the serialize/re-parse round
     * trip, because re-parsing drops it. */
    if (ws && p->phase < PH_BODY && loose) {
        p->text.len = 0;
        if (p->text.p)
            p->text.p[0] = 0;
        return;
    }

    p->flushing = 1;
    /* Real text outside any element opens <body> the way a start tag
     * would; text is never a child of the document node, and character
     * data in <head> ends the head. */
    if (!ws && loose && p->phase < PH_BODY)
        ensure_body(p);
    p->flushing = 0;
    ctag = cur_tag(p);

    if (ctag == HTAG_STYLE)
        dom_collect_style(p->doc, p->text.p, p->text.len);
    else if (ctag == HTAG_SCRIPT)
        dom_collect_script(p->doc, p->text.p, p->text.len);
    else if (ctag == HTAG_TITLE)
        dom_set_title(p->doc, p->text.p, p->text.len);

    /* Work out where the text lands, then look at what is already
     * there. Whitespace-only text inside a table belongs to the table;
     * real text gets fostered out in front of it. */
    {
        struct dom_node *parent = cur_node(p), *before = 0, *prev;

        if (!ws)
            foster_target(p, &parent, &before);
        prev = before ? before->prev_sibling
                      : (parent ? parent->last_child : 0);

        /* HTML5 never produces two adjacent text nodes, and the
         * serializer's round-trip guarantee depends on it: "a" then
         * "b" would be written as "ab" and re-parse as one node. The
         * size guard keeps a pathological document from turning the
         * merge into quadratic copying. */
        if (prev && prev->type == DOM_TEXT &&
            prev->text_len < 1024UL * 1024UL) {
            dom_text_append(prev, p->text.p, p->text.len);
        } else {
            t = dom_create_text(p->doc, p->text.p, p->text.len);
            if (t) {
                if (before)
                    dom_insert_before(parent, t, before);
                else
                    dom_append_child(parent, t);
            }
        }
    }
    p->text.len = 0;
    if (p->text.p)
        p->text.p[0] = 0;
}

static void tb_chars(struct parser *p, const char *s, unsigned long n)
{
    if (n == 0)
        return;
    /* A newline immediately after <pre>, <listing> or <textarea> is
     * swallowed: authors write "<pre>\ncode", and the blank first line
     * is not what they meant. The serializer writes the newline back so
     * the round trip still holds. */
    if (p->skip_lf) {
        p->skip_lf = 0;
        if (s[0] == '\n') {
            s++;
            n--;
            if (n == 0)
                return;
        }
    }
    if (p->text.len + n > p->max_text)
        p->doc->truncated |= DOM_TRUNC_TEXT;
    buf_add(&p->text, s, n, p->max_text, &p->oom);
}

/* ------------------------------------------------------------------ *
 * Document skeleton
 * ------------------------------------------------------------------ */

static struct dom_node *make_el(struct parser *p, const char *tag,
                                unsigned long n, int *in_tree,
                                int allow_foster)
{
    struct dom_node *el = dom_create_element(p->doc, tag, (long)n);

    if (!el) {
        *in_tree = 0;
        return 0;
    }
    *in_tree = place(p, el, allow_foster);
    if (el->tag_id == HTAG_SVG)
        dom_set_namespace(el, DOM_NS_SVG_URI, -1);
    else if (el->parent && el->parent->type == DOM_ELEMENT &&
             el->parent->namespace_id == DOM_NS_SVG &&
             strcmp(el->parent->tag, "foreignobject"))
        dom_set_namespace(el, DOM_NS_SVG_URI, -1);
    else if (!strcmp(el->tag, "math") ||
             (el->parent && el->parent->type == DOM_ELEMENT &&
              el->parent->namespace_id == DOM_NS_MATHML))
        dom_set_namespace(el, DOM_NS_MATHML_URI, -1);
    return el;
}

static void ensure_html(struct parser *p)
{
    struct dom_node *el;
    int in_tree = 0;

    if (p->doc->html)
        return;
    el = make_el(p, "html", 4, &in_tree, 0);
    if (!el)
        return;
    p->doc->html = el;
    push_open(p, in_tree ? el : 0, el->tag, HTAG_HTML);
    if (p->phase < PH_HEAD)
        p->phase = PH_HEAD;
}

static const int k_head[1] = { HTAG_HEAD };

static int head_is_open(struct parser *p)
{
    return find_open(p, k_head, 1) >= 0;
}

static void ensure_head(struct parser *p)
{
    struct dom_node *el;
    int in_tree = 0;

    ensure_html(p);
    if (p->doc->head)
        return;
    el = make_el(p, "head", 4, &in_tree, 0);
    if (!el)
        return;
    p->doc->head = el;
    push_open(p, in_tree ? el : 0, el->tag, HTAG_HEAD);
}

/* Pop everything up to and including <head>, if it is still open. */
static void close_head(struct parser *p)
{
    long i = find_open(p, k_head, 1);

    if (i >= 0)
        pop_to(p, (unsigned long)i);
}

static void ensure_body(struct parser *p)
{
    struct dom_node *el;
    int in_tree = 0;

    if (p->doc->body) {
        p->phase = PH_BODY;
        return;
    }
    ensure_html(p);
    /* Every document has a <head>, even an empty one: consumers should
     * never have to test for it, and it keeps the round trip stable. */
    ensure_head(p);
    close_head(p);
    el = make_el(p, "body", 4, &in_tree, 0);
    if (!el)
        return;
    p->doc->body = el;
    push_open(p, in_tree ? el : 0, el->tag, HTAG_BODY);
    p->phase = PH_BODY;
}

/* ------------------------------------------------------------------ *
 * Auto-closing
 * ------------------------------------------------------------------ */

static int is_scope_boundary(int tag_id)
{
    return (dom_tag_flags(tag_id) & DTF_SCOPE) != 0 || tag_id == HTAG_BUTTON;
}

/* Close an open <p>, but only inside the current container. */
static void close_p(struct parser *p)
{
    long i;

    for (i = (long)p->nopen - 1; i >= 0; i--) {
        if (p->open[i].tag_id == HTAG_P) {
            pop_to(p, (unsigned long)i);
            return;
        }
        if (is_scope_boundary(p->open[i].tag_id))
            return;
    }
}

static void close_list_item(struct parser *p)
{
    long i;

    for (i = (long)p->nopen - 1; i >= 0; i--) {
        int t = p->open[i].tag_id;
        if (t == HTAG_LI) {
            pop_to(p, (unsigned long)i);
            return;
        }
        if (t == HTAG_UL || t == HTAG_OL || t == HTAG_MENU ||
            t == HTAG_DIR || is_scope_boundary(t))
            return;
    }
}

static void close_dt_dd(struct parser *p)
{
    long i;

    for (i = (long)p->nopen - 1; i >= 0; i--) {
        int t = p->open[i].tag_id;
        if (t == HTAG_DT || t == HTAG_DD) {
            pop_to(p, (unsigned long)i);
            return;
        }
        if (t == HTAG_DL || is_scope_boundary(t))
            return;
    }
}

static void close_to_set(struct parser *p, const int *set, int nset)
{
    long i = find_open(p, set, nset);

    if (i >= 0)
        pop_to(p, (unsigned long)(i + 1));
}

static const int k_table[1]   = { HTAG_TABLE };
static const int k_section[4] = { HTAG_TABLE, HTAG_TBODY, HTAG_THEAD,
                                  HTAG_TFOOT };
static const int k_row[5]     = { HTAG_TR, HTAG_TBODY, HTAG_THEAD,
                                  HTAG_TFOOT, HTAG_TABLE };

/* ------------------------------------------------------------------ *
 * Start tags
 * ------------------------------------------------------------------ */

/* Duplicate attributes: the first occurrence wins, which is also what
 * makes merging a second <html> or <body> tag's attributes correct. */
static void apply_attrs(struct parser *p, struct dom_node *el)
{
    unsigned long i;

    if (!el)
        return;
    for (i = 0; i < p->nattr; i++) {
        struct tokattr *a = &p->attrs[i];
        const char *nm = p->abuf.p + a->noff;
        const char *vl = p->abuf.p + a->voff;
        if (a->nlen == 0)
            continue;
        if (dom_get_attr_n(el, nm, (long)a->nlen))
            continue;
        dom_set_attr_n(el, nm, (long)a->nlen, vl, a->vlen);
    }
}

static void tb_start_tag(struct parser *p)
{
    const char *nm = p->name.p ? p->name.p : "";
    unsigned long nl = p->name.len;
    int tag_id = dom_tag_lookup(nm, (long)nl);
    unsigned int flags = dom_tag_flags(tag_id);
    struct dom_node *el;
    int in_tree = 0;

    if (nl == 0)
        return;
    p->skip_lf = 0;

    /* ---- structural elements ----
     *
     * These paths can produce no node at all, so they must not flush
     * pending text: doing so would split one run of character data
     * into two adjacent text nodes. */
    if (tag_id == HTAG_HTML) {
        if (!p->doc->html)
            tb_text_flush(p);
        ensure_html(p);
        apply_attrs(p, p->doc->html);
        return;
    }
    if (tag_id == HTAG_HEAD) {
        if (p->doc->head)
            return;
        tb_text_flush(p);
        ensure_head(p);
        apply_attrs(p, p->doc->head);
        return;
    }
    if (tag_id == HTAG_BODY) {
        if (p->doc->body) {
            apply_attrs(p, p->doc->body);
            p->phase = PH_BODY;
            return;
        }
        tb_text_flush(p);
        ensure_body(p);
        apply_attrs(p, p->doc->body);
        return;
    }

    /* ---- the depth cap ----
     *
     * Checked before anything is created, so a document nested a
     * million elements deep costs no nodes, no arena and no split text
     * runs: the subtree is flattened into its deepest permitted
     * ancestor and its content is preserved.
     *
     * The test is >= rather than > so that a text child of the deepest
     * element still fits: DOM_MAX_DEPTH bounds the depth of every node,
     * text nodes included, which puts the deepest element one level
     * above it. */
    if (!depth_ok(p)) {
        p->doc->truncated |= DOM_TRUNC_DEPTH;
        if (!(flags & DTF_VOID) &&
            !(p->self_closing && tag_id == HTAG_UNKNOWN)) {
            const char *iname = dom_intern_name(p->doc, nm, (long)nl);
            if (!iname) {
                p->oom = 1;
                return;
            }
            push_open(p, 0, iname, tag_id);
            if (tag_id == HTAG_PRE || tag_id == HTAG_LISTING ||
                tag_id == HTAG_TEXTAREA)
                p->skip_lf = 1;
            if (flags & DTF_RCDATA)
                p->state = S_RCDATA;
            else if (flags & DTF_RAWTEXT)
                p->state = (tag_id == HTAG_SCRIPT) ? S_SCRIPT : S_RAWTEXT;
        }
        return;
    }

    tb_text_flush(p);
    /* ---- head vs body routing ----
     *
     * Metadata content goes into <head> while the head is still the
     * place things go; once </head> has been seen, or any body content
     * has arrived, everything lands in <body>. <noscript> keeps its
     * markup children because there is no script engine, so its content
     * is exactly what should be displayed. Framesets are treated as
     * ordinary body elements so their <noframes> fallback renders. */
    if ((flags & DTF_HEAD) && p->phase < PH_BODY &&
        (!p->doc->head || head_is_open(p)))
        ensure_head(p);
    else
        ensure_body(p);

    /* ---- implied end tags ---- */
    if (flags & DTF_BLOCK)
        close_p(p);

    switch (tag_id) {
    case HTAG_LI:
        close_list_item(p);
        break;
    case HTAG_DT:
    case HTAG_DD:
        close_dt_dd(p);
        break;
    case HTAG_TBODY:
    case HTAG_TFOOT:
    case HTAG_THEAD:
    case HTAG_CAPTION:
    case HTAG_COLGROUP:
        close_to_set(p, k_table, 1);
        break;
    case HTAG_TR:
        close_to_set(p, k_section, 4);
        if (cur_tag(p) == HTAG_TABLE) {
            struct dom_node *tb = make_el(p, "tbody", 5, &in_tree, 0);
            if (tb)
                push_open(p, in_tree ? tb : 0, tb->tag, HTAG_TBODY);
        }
        break;
    case HTAG_TD:
    case HTAG_TH:
        close_to_set(p, k_row, 5);
        if (cur_tag(p) == HTAG_TABLE) {
            struct dom_node *tb = make_el(p, "tbody", 5, &in_tree, 0);
            if (tb)
                push_open(p, in_tree ? tb : 0, tb->tag, HTAG_TBODY);
        }
        if (cur_tag(p) == HTAG_TBODY || cur_tag(p) == HTAG_THEAD ||
            cur_tag(p) == HTAG_TFOOT) {
            struct dom_node *tr = make_el(p, "tr", 2, &in_tree, 0);
            if (tr)
                push_open(p, in_tree ? tr : 0, tr->tag, HTAG_TR);
        }
        break;
    case HTAG_OPTION:
        if (cur_tag(p) == HTAG_OPTION)
            pop_open(p);
        break;
    case HTAG_OPTGROUP:
        if (cur_tag(p) == HTAG_OPTION)
            pop_open(p);
        if (cur_tag(p) == HTAG_OPTGROUP)
            pop_open(p);
        break;
    case HTAG_A: {
        /* The adoption agency algorithm, approximated: a nested <a>
         * closes the outer one rather than producing a link inside a
         * link, which no renderer can express anyway. */
        long i;
        for (i = (long)p->nopen - 1; i >= 0; i--) {
            if (p->open[i].tag_id == HTAG_A) {
                pop_to(p, (unsigned long)i);
                break;
            }
            if (dom_tag_flags(p->open[i].tag_id) & DTF_SPECIAL)
                break;
        }
        break;
    }
    default:
        break;
    }
    if ((flags & DTF_HEADING) && (dom_tag_flags(cur_tag(p)) & DTF_HEADING))
        pop_open(p);

    /* ---- insert ---- */
    el = make_el(p, nm, nl, &in_tree,
                 !(flags & DTF_TABLESEC) && tag_id != HTAG_SCRIPT &&
                 tag_id != HTAG_STYLE && tag_id != HTAG_TEMPLATE);
    if (!el) {
        if (p->doc->oom)
            p->oom = 1;
        return;
    }
    apply_attrs(p, el);
    el->self_closed = (unsigned char)(p->self_closing ? 1 : 0);

    if (flags & DTF_VOID)
        return;
    /* HTML5 ignores the slash on known elements; honouring it on
     * unknown ones keeps "<x-foo/>" from swallowing the document. */
    if (p->self_closing && tag_id == HTAG_UNKNOWN)
        return;

    push_open(p, in_tree ? el : 0, el->tag, tag_id);

    if (tag_id == HTAG_PRE || tag_id == HTAG_LISTING ||
        tag_id == HTAG_TEXTAREA)
        p->skip_lf = 1;
    if (flags & DTF_RCDATA)
        p->state = S_RCDATA;
    else if (flags & DTF_RAWTEXT)
        p->state = (tag_id == HTAG_SCRIPT) ? S_SCRIPT : S_RAWTEXT;
}

/* ------------------------------------------------------------------ *
 * End tags
 * ------------------------------------------------------------------ */

/* Table end tags are matched in "table scope", where only <html>,
 * <table> and <template> stop the search. */
static int table_scope_tag(int t)
{
    return t == HTAG_TABLE || t == HTAG_TBODY || t == HTAG_THEAD ||
           t == HTAG_TFOOT || t == HTAG_TR || t == HTAG_TD ||
           t == HTAG_TH || t == HTAG_CAPTION || t == HTAG_COLGROUP;
}

static void tb_end_tag(struct parser *p)
{
    const char *nm = p->name.p ? p->name.p : "";
    unsigned long nl = p->name.len;
    int tag_id = dom_tag_lookup(nm, (long)nl);
    long i;
    int tbl;

    if (nl == 0)
        return;
    p->skip_lf = 0;

    /* </br> is a start tag in disguise; every browser does this. */
    if (tag_id == HTAG_BR) {
        int in_tree = 0;
        tb_text_flush(p);
        ensure_body(p);
        if (depth_ok(p))
            make_el(p, "br", 2, &in_tree, 1);
        else
            p->doc->truncated |= DOM_TRUNC_DEPTH;
        return;
    }
    /* </body> and </html> only mean "stop"; the EOF handler unwinds. */
    if (tag_id == HTAG_BODY || tag_id == HTAG_HTML)
        return;

    /* Scope-aware matching, which is what keeps a stray </div> from
     * unwinding the document: search down for the element, but stop at
     * a scope boundary and ignore the tag if one is hit first. Table
     * end tags use the wider "table scope" so that </table> can close
     * an open cell, exactly as the spec has it. */
    tbl = table_scope_tag(tag_id);
    for (i = (long)p->nopen - 1; i >= 0; i--) {
        int oid = p->open[i].tag_id;
        int same;

        if (oid != HTAG_UNKNOWN || tag_id != HTAG_UNKNOWN)
            same = (oid == tag_id) && oid != HTAG_UNKNOWN;
        else
            same = (strlen(p->open[i].tag) == nl &&
                    memcmp(p->open[i].tag, nm, nl) == 0);
        if (same) {
            pop_to(p, (unsigned long)i);
            return;
        }
        if (tbl) {
            if (oid == HTAG_HTML || oid == HTAG_TABLE ||
                oid == HTAG_TEMPLATE)
                return;
        } else if (dom_tag_flags(oid) & DTF_SCOPE) {
            return;
        }
    }
}

/* ------------------------------------------------------------------ *
 * Comments, DOCTYPE, EOF
 * ------------------------------------------------------------------ */

static void tb_comment(struct parser *p)
{
    struct dom_node *c;

    p->skip_lf = 0;
    tb_text_flush(p);
    c = dom_create_comment(p->doc, p->cdata.p ? p->cdata.p : "",
                           p->cdata.len);
    place(p, c, 1);
    p->cdata.len = 0;
    if (p->cdata.p)
        p->cdata.p[0] = 0;
}

static void tb_doctype(struct parser *p)
{
    struct dom_node *c;

    /* Only a DOCTYPE that arrives before anything else is meaningful;
     * a stray one later creates no node, so it must not flush - doing
     * so would split one run of character data in two. Pending
     * whitespace does not count as "something else": the flush drops
     * it. */
    if (!p->doc->html && p->nopen == 0 &&
        (p->text.len == 0 || all_space(p->text.p, p->text.len))) {
        tb_text_flush(p);
        c = dom_create_doctype(p->doc, p->cdata.len ? p->cdata.p : "html",
                               p->cdata.len ? p->cdata.len : 4);
        place(p, c, 0);
    }
    p->cdata.len = 0;
    if (p->cdata.p)
        p->cdata.p[0] = 0;
}

static void tb_eof(struct parser *p)
{
    tb_text_flush(p);
    /* A document with no elements at all still gets a skeleton, so
     * consumers never have to test for a missing <html> or <body>.
     * This runs before unwinding, because <html> is at the bottom of
     * the open stack and <body> has to be inserted under it. */
    if (!p->doc->html)
        ensure_html(p);
    if (!p->doc->body)
        ensure_body(p);
    while (p->nopen > 0)
        pop_open(p);
}

/* ------------------------------------------------------------------ *
 * Token plumbing
 * ------------------------------------------------------------------ */

static void tok_reset(struct parser *p)
{
    p->name.len = 0;
    if (p->name.p)
        p->name.p[0] = 0;
    p->abuf.len = 0;
    if (p->abuf.p)
        p->abuf.p[0] = 0;
    p->nattr = 0;
    p->attr_open = 0;
    p->attr_named = 0;
    p->self_closing = 0;
    p->is_end_tag = 0;
}

static void emit_tag(struct parser *p)
{
    attr_close(p);
    if (p->is_end_tag) {
        tb_end_tag(p);
    } else {
        p->last_start.len = 0;
        if (p->last_start.p)
            p->last_start.p[0] = 0;
        buf_add(&p->last_start, p->name.p ? p->name.p : "", p->name.len,
                256, &p->oom);
        tb_start_tag(p);
    }
    tok_reset(p);
}

static void name_putc(struct parser *p, int c)
{
    char ch = (char)h_lower(c);

    buf_add(&p->name, &ch, 1, 4096, &p->oom);
}

static void cdata_putc(struct parser *p, int c)
{
    char ch = (char)c;

    buf_add(&p->cdata, &ch, 1, DOM_MAX_TEXT, &p->oom);
}

static void cdata_put(struct parser *p, const char *s, unsigned long n)
{
    buf_add(&p->cdata, s, n, DOM_MAX_TEXT, &p->oom);
}

static void tmp_reset(struct parser *p)
{
    p->tmp.len = 0;
    if (p->tmp.p)
        p->tmp.p[0] = 0;
}

static void tmp_putc(struct parser *p, int c)
{
    char ch = (char)c;

    buf_add(&p->tmp, &ch, 1, 4096, &p->oom);
}

/* Does the temporary buffer name the element the raw-text mode was
 * entered for? */
static int tmp_matches_last(struct parser *p)
{
    unsigned long i;

    if (p->tmp.len != p->last_start.len || p->tmp.len == 0)
        return 0;
    for (i = 0; i < p->tmp.len; i++) {
        if (h_lower((unsigned char)p->tmp.p[i]) !=
            h_lower((unsigned char)p->last_start.p[i]))
            return 0;
    }
    return 1;
}

/* Give up on a raw-text end tag: the "</" and whatever was buffered
 * were ordinary text after all. */
static void raw_bail(struct parser *p, int with_slash, int with_tmp)
{
    tb_chars(p, "<", 1);
    if (with_slash)
        tb_chars(p, "/", 1);
    if (with_tmp && p->tmp.len)
        tb_chars(p, p->tmp.p, p->tmp.len);
}

/* ------------------------------------------------------------------ *
 * The tokenizer
 * ------------------------------------------------------------------ */

static int looks_like(const struct parser *p, unsigned long at,
                      const char *lit, int ci)
{
    unsigned long n = strlen(lit), i;

    if (at + n > p->len)
        return 0;
    for (i = 0; i < n; i++) {
        int a = (unsigned char)p->src[at + i];
        int b = (unsigned char)lit[i];
        if (ci) {
            a = h_lower(a);
            b = h_lower(b);
        }
        if (a != b)
            return 0;
    }
    return 1;
}

static void handle_eof(struct parser *p)
{
    switch (p->state) {
    case S_BOGUS_COMMENT:
    case S_COMMENT_START:
    case S_COMMENT_START_DASH:
    case S_COMMENT:
    case S_COMMENT_END_DASH:
    case S_COMMENT_END:
    case S_COMMENT_END_BANG:
        tb_comment(p);
        break;
    case S_DOCTYPE:
    case S_BEFORE_DOCTYPE_NAME:
    case S_DOCTYPE_NAME:
    case S_BOGUS_DOCTYPE:
        tb_doctype(p);
        break;
    case S_CDATA:
    case S_CDATA_BRACKET:
    case S_CDATA_END:
        break;                      /* content already went to text */
    case S_RCDATA_LT:
    case S_RAWTEXT_LT:
    case S_SCRIPT_LT:
    case S_SCRIPT_ESC_LT:
        raw_bail(p, 0, 0);
        break;
    case S_RCDATA_END_OPEN:
    case S_RAWTEXT_END_OPEN:
    case S_SCRIPT_END_OPEN:
    case S_SCRIPT_ESC_END_OPEN:
        raw_bail(p, 1, 0);
        break;
    case S_RCDATA_END_NAME:
    case S_RAWTEXT_END_NAME:
    case S_SCRIPT_END_NAME:
    case S_SCRIPT_ESC_END_NAME:
        raw_bail(p, 1, 1);
        break;
    default:
        /* An unterminated tag at EOF is dropped, exactly as the spec
         * says: the bytes never became a token. */
        break;
    }
    tb_eof(p);
}

static void tokenize(struct parser *p)
{
    while (p->pos < p->len && !p->oom && !p->doc->oom) {
        int c = (unsigned char)p->src[p->pos];

        switch (p->state) {

        /* ---- data ---- */
        case S_DATA:
            if (c == '&') {
                p->pos++;
                ref_consume(p, 0);
            } else if (c == '<') {
                p->pos++;
                p->state = S_TAG_OPEN;
            } else {
                unsigned long start = p->pos;
                while (p->pos < p->len && p->src[p->pos] != '&' &&
                       p->src[p->pos] != '<')
                    p->pos++;
                tb_chars(p, p->src + start, p->pos - start);
            }
            break;

        case S_TAG_OPEN:
            if (c == '!') {
                p->pos++;
                p->state = S_MARKUP_DECL;
            } else if (c == '/') {
                p->pos++;
                p->state = S_END_TAG_OPEN;
            } else if (h_alpha(c)) {
                tok_reset(p);
                p->state = S_TAG_NAME;
            } else if (c == '?') {
                p->pos++;
                p->cdata.len = 0;
                p->state = S_BOGUS_COMMENT;
            } else {
                tb_chars(p, "<", 1);
                p->state = S_DATA;
            }
            break;

        case S_END_TAG_OPEN:
            if (h_alpha(c)) {
                tok_reset(p);
                p->is_end_tag = 1;
                p->state = S_TAG_NAME;
            } else if (c == '>') {
                p->pos++;
                p->state = S_DATA;      /* "</>" is simply discarded */
            } else {
                p->cdata.len = 0;
                p->state = S_BOGUS_COMMENT;
            }
            break;

        case S_TAG_NAME:
            if (h_space(c)) {
                p->pos++;
                p->state = S_BEFORE_ATTR_NAME;
            } else if (c == '/') {
                p->pos++;
                p->state = S_SELF_CLOSING;
            } else if (c == '>') {
                p->pos++;
                p->state = S_DATA;
                emit_tag(p);
            } else {
                name_putc(p, c);
                p->pos++;
            }
            break;

        case S_BEFORE_ATTR_NAME:
            if (h_space(c)) {
                p->pos++;
            } else if (c == '/') {
                p->pos++;
                p->state = S_SELF_CLOSING;
            } else if (c == '>') {
                p->pos++;
                p->state = S_DATA;
                emit_tag(p);
            } else if (c == '=') {
                /* "<a =b>": the '=' is part of the name, per spec. */
                attr_start(p);
                attr_putc(p, c);
                p->pos++;
                p->state = S_ATTR_NAME;
            } else {
                attr_start(p);
                p->state = S_ATTR_NAME;
            }
            break;

        case S_ATTR_NAME:
            if (h_space(c)) {
                attr_name_done(p);
                p->pos++;
                p->state = S_AFTER_ATTR_NAME;
            } else if (c == '/') {
                attr_name_done(p);
                p->pos++;
                p->state = S_SELF_CLOSING;
            } else if (c == '=') {
                attr_name_done(p);
                p->pos++;
                p->state = S_BEFORE_ATTR_VALUE;
            } else if (c == '>') {
                attr_name_done(p);
                p->pos++;
                p->state = S_DATA;
                emit_tag(p);
            } else {
                attr_putc(p, h_lower(c));
                p->pos++;
            }
            break;

        case S_AFTER_ATTR_NAME:
            if (h_space(c)) {
                p->pos++;
            } else if (c == '/') {
                p->pos++;
                p->state = S_SELF_CLOSING;
            } else if (c == '=') {
                p->pos++;
                p->state = S_BEFORE_ATTR_VALUE;
            } else if (c == '>') {
                p->pos++;
                p->state = S_DATA;
                emit_tag(p);
            } else {
                attr_start(p);
                p->state = S_ATTR_NAME;
            }
            break;

        case S_BEFORE_ATTR_VALUE:
            if (h_space(c)) {
                p->pos++;
            } else if (c == '"') {
                p->pos++;
                p->state = S_ATTR_VALUE_DQ;
            } else if (c == '\'') {
                p->pos++;
                p->state = S_ATTR_VALUE_SQ;
            } else if (c == '>') {
                p->pos++;               /* "<a href=>" - empty value */
                p->state = S_DATA;
                emit_tag(p);
            } else {
                p->state = S_ATTR_VALUE_UQ;
            }
            break;

        case S_ATTR_VALUE_DQ:
        case S_ATTR_VALUE_SQ: {
            int q = (p->state == S_ATTR_VALUE_DQ) ? '"' : '\'';
            if (c == q) {
                p->pos++;
                p->state = S_AFTER_ATTR_VALUE_Q;
            } else if (c == '&') {
                p->pos++;
                ref_consume(p, 1);
            } else {
                unsigned long start = p->pos;
                while (p->pos < p->len && p->src[p->pos] != q &&
                       p->src[p->pos] != '&')
                    p->pos++;
                attr_put(p, p->src + start, p->pos - start);
            }
            break;
        }

        case S_ATTR_VALUE_UQ:
            if (h_space(c)) {
                p->pos++;
                p->state = S_BEFORE_ATTR_NAME;
            } else if (c == '&') {
                p->pos++;
                ref_consume(p, 1);
            } else if (c == '>') {
                p->pos++;
                p->state = S_DATA;
                emit_tag(p);
            } else {
                attr_putc(p, c);
                p->pos++;
            }
            break;

        case S_AFTER_ATTR_VALUE_Q:
            if (h_space(c)) {
                p->pos++;
                p->state = S_BEFORE_ATTR_NAME;
            } else if (c == '/') {
                p->pos++;
                p->state = S_SELF_CLOSING;
            } else if (c == '>') {
                p->pos++;
                p->state = S_DATA;
                emit_tag(p);
            } else {
                p->state = S_BEFORE_ATTR_NAME;
            }
            break;

        case S_SELF_CLOSING:
            if (c == '>') {
                p->self_closing = 1;
                p->pos++;
                p->state = S_DATA;
                emit_tag(p);
            } else {
                p->state = S_BEFORE_ATTR_NAME;
            }
            break;

        /* ---- markup declarations ---- */
        case S_MARKUP_DECL:
            if (looks_like(p, p->pos, "--", 0)) {
                p->pos += 2;
                p->cdata.len = 0;
                if (p->cdata.p)
                    p->cdata.p[0] = 0;
                p->state = S_COMMENT_START;
            } else if (looks_like(p, p->pos, "doctype", 1)) {
                p->pos += 7;
                p->cdata.len = 0;
                if (p->cdata.p)
                    p->cdata.p[0] = 0;
                p->state = S_DOCTYPE;
            } else if (looks_like(p, p->pos, "[CDATA[", 0)) {
                p->pos += 7;
                p->state = S_CDATA;
            } else {
                p->cdata.len = 0;
                if (p->cdata.p)
                    p->cdata.p[0] = 0;
                p->state = S_BOGUS_COMMENT;
            }
            break;

        case S_BOGUS_COMMENT:
            if (c == '>') {
                p->pos++;
                p->state = S_DATA;
                tb_comment(p);
            } else {
                unsigned long start = p->pos;
                while (p->pos < p->len && p->src[p->pos] != '>')
                    p->pos++;
                cdata_put(p, p->src + start, p->pos - start);
            }
            break;

        case S_COMMENT_START:
            if (c == '-') {
                p->pos++;
                p->state = S_COMMENT_START_DASH;
            } else if (c == '>') {
                p->pos++;               /* "<!-->" is an empty comment */
                p->state = S_DATA;
                tb_comment(p);
            } else {
                p->state = S_COMMENT;
            }
            break;

        case S_COMMENT_START_DASH:
            if (c == '-') {
                p->pos++;
                p->state = S_COMMENT_END;
            } else if (c == '>') {
                p->pos++;               /* "<!--->" */
                p->state = S_DATA;
                tb_comment(p);
            } else {
                cdata_putc(p, '-');
                p->state = S_COMMENT;
            }
            break;

        case S_COMMENT:
            if (c == '-') {
                p->pos++;
                p->state = S_COMMENT_END_DASH;
            } else {
                unsigned long start = p->pos;
                while (p->pos < p->len && p->src[p->pos] != '-')
                    p->pos++;
                cdata_put(p, p->src + start, p->pos - start);
            }
            break;

        case S_COMMENT_END_DASH:
            if (c == '-') {
                p->pos++;
                p->state = S_COMMENT_END;
            } else {
                cdata_putc(p, '-');
                p->state = S_COMMENT;
            }
            break;

        case S_COMMENT_END:
            if (c == '>') {
                p->pos++;
                p->state = S_DATA;
                tb_comment(p);
            } else if (c == '!') {
                p->pos++;
                p->state = S_COMMENT_END_BANG;
            } else if (c == '-') {
                /* "a---->": the extra dashes are content. */
                cdata_putc(p, '-');
                p->pos++;
            } else {
                cdata_put(p, "--", 2);
                p->state = S_COMMENT;
            }
            break;

        case S_COMMENT_END_BANG:
            if (c == '-') {
                cdata_put(p, "--!", 3);
                p->pos++;
                p->state = S_COMMENT_END_DASH;
            } else if (c == '>') {
                p->pos++;
                p->state = S_DATA;
                tb_comment(p);
            } else {
                cdata_put(p, "--!", 3);
                p->state = S_COMMENT;
            }
            break;

        /* ---- DOCTYPE ---- */
        case S_DOCTYPE:
            if (h_space(c)) {
                p->pos++;
                p->state = S_BEFORE_DOCTYPE_NAME;
            } else if (c == '>') {
                p->pos++;
                p->state = S_DATA;
                tb_doctype(p);
            } else {
                p->state = S_BEFORE_DOCTYPE_NAME;
            }
            break;

        case S_BEFORE_DOCTYPE_NAME:
            if (h_space(c)) {
                p->pos++;
            } else if (c == '>') {
                p->pos++;
                p->state = S_DATA;
                tb_doctype(p);
            } else {
                p->state = S_DOCTYPE_NAME;
            }
            break;

        case S_DOCTYPE_NAME:
            if (h_space(c)) {
                p->pos++;
                p->state = S_BOGUS_DOCTYPE;   /* public/system ids: skip */
            } else if (c == '>') {
                p->pos++;
                p->state = S_DATA;
                tb_doctype(p);
            } else {
                cdata_putc(p, h_lower(c));
                p->pos++;
            }
            break;

        case S_BOGUS_DOCTYPE:
            if (c == '>') {
                p->pos++;
                p->state = S_DATA;
                tb_doctype(p);
            } else {
                while (p->pos < p->len && p->src[p->pos] != '>')
                    p->pos++;
            }
            break;

        /* ---- CDATA: outside foreign content it is a comment, but the
         * useful thing to do with the text is show it ---- */
        case S_CDATA:
            if (c == ']') {
                p->pos++;
                p->state = S_CDATA_BRACKET;
            } else {
                unsigned long start = p->pos;
                while (p->pos < p->len && p->src[p->pos] != ']')
                    p->pos++;
                tb_chars(p, p->src + start, p->pos - start);
            }
            break;

        case S_CDATA_BRACKET:
            if (c == ']') {
                p->pos++;
                p->state = S_CDATA_END;
            } else {
                tb_chars(p, "]", 1);
                p->state = S_CDATA;
            }
            break;

        case S_CDATA_END:
            if (c == '>') {
                p->pos++;
                p->state = S_DATA;
            } else if (c == ']') {
                tb_chars(p, "]", 1);
                p->pos++;
            } else {
                tb_chars(p, "]]", 2);
                p->state = S_CDATA;
            }
            break;

        /* ---- RCDATA (title, textarea) ---- */
        case S_RCDATA:
            if (c == '&') {
                p->pos++;
                ref_consume(p, 0);
            } else if (c == '<') {
                p->pos++;
                p->state = S_RCDATA_LT;
            } else {
                unsigned long start = p->pos;
                while (p->pos < p->len && p->src[p->pos] != '&' &&
                       p->src[p->pos] != '<')
                    p->pos++;
                tb_chars(p, p->src + start, p->pos - start);
            }
            break;

        case S_RCDATA_LT:
            if (c == '/') {
                p->pos++;
                tmp_reset(p);
                p->state = S_RCDATA_END_OPEN;
            } else {
                raw_bail(p, 0, 0);
                p->state = S_RCDATA;
            }
            break;

        case S_RCDATA_END_OPEN:
            if (h_alpha(c)) {
                p->state = S_RCDATA_END_NAME;
            } else {
                raw_bail(p, 1, 0);
                p->state = S_RCDATA;
            }
            break;

        case S_RCDATA_END_NAME:
            if ((h_space(c) || c == '/' || c == '>') && tmp_matches_last(p)) {
                tok_reset(p);
                p->is_end_tag = 1;
                buf_add(&p->name, p->tmp.p, p->tmp.len, 4096, &p->oom);
                p->state = S_DATA;
                if (c == '>') {
                    p->pos++;
                    emit_tag(p);
                } else if (c == '/') {
                    p->pos++;
                    p->state = S_SELF_CLOSING;
                } else {
                    p->pos++;
                    p->state = S_BEFORE_ATTR_NAME;
                }
            } else if (h_alnum(c) || c == '-' || c == '_') {
                tmp_putc(p, c);
                p->pos++;
            } else {
                raw_bail(p, 1, 1);
                p->state = S_RCDATA;
            }
            break;

        /* ---- RAWTEXT (style, iframe, noframes, xmp) ---- */
        case S_RAWTEXT:
            if (c == '<') {
                p->pos++;
                p->state = S_RAWTEXT_LT;
            } else {
                unsigned long start = p->pos;
                while (p->pos < p->len && p->src[p->pos] != '<')
                    p->pos++;
                tb_chars(p, p->src + start, p->pos - start);
            }
            break;

        case S_RAWTEXT_LT:
            if (c == '/') {
                p->pos++;
                tmp_reset(p);
                p->state = S_RAWTEXT_END_OPEN;
            } else {
                raw_bail(p, 0, 0);
                p->state = S_RAWTEXT;
            }
            break;

        case S_RAWTEXT_END_OPEN:
            if (h_alpha(c)) {
                p->state = S_RAWTEXT_END_NAME;
            } else {
                raw_bail(p, 1, 0);
                p->state = S_RAWTEXT;
            }
            break;

        case S_RAWTEXT_END_NAME:
            if ((h_space(c) || c == '/' || c == '>') && tmp_matches_last(p)) {
                tok_reset(p);
                p->is_end_tag = 1;
                buf_add(&p->name, p->tmp.p, p->tmp.len, 4096, &p->oom);
                p->state = S_DATA;
                if (c == '>') {
                    p->pos++;
                    emit_tag(p);
                } else if (c == '/') {
                    p->pos++;
                    p->state = S_SELF_CLOSING;
                } else {
                    p->pos++;
                    p->state = S_BEFORE_ATTR_NAME;
                }
            } else if (h_alnum(c) || c == '-' || c == '_') {
                tmp_putc(p, c);
                p->pos++;
            } else {
                raw_bail(p, 1, 1);
                p->state = S_RAWTEXT;
            }
            break;

        /* ---- script data, with the escaped and double-escaped states
         * that make "<!-- <script> -->" inside a script behave ---- */
        case S_SCRIPT:
            if (c == '<') {
                p->pos++;
                p->state = S_SCRIPT_LT;
            } else {
                unsigned long start = p->pos;
                while (p->pos < p->len && p->src[p->pos] != '<')
                    p->pos++;
                tb_chars(p, p->src + start, p->pos - start);
            }
            break;

        case S_SCRIPT_LT:
            if (c == '/') {
                p->pos++;
                tmp_reset(p);
                p->state = S_SCRIPT_END_OPEN;
            } else if (c == '!') {
                tb_chars(p, "<!", 2);
                p->pos++;
                p->state = S_SCRIPT_ESC_START;
            } else {
                raw_bail(p, 0, 0);
                p->state = S_SCRIPT;
            }
            break;

        case S_SCRIPT_END_OPEN:
            if (h_alpha(c)) {
                p->state = S_SCRIPT_END_NAME;
            } else {
                raw_bail(p, 1, 0);
                p->state = S_SCRIPT;
            }
            break;

        case S_SCRIPT_END_NAME:
            if ((h_space(c) || c == '/' || c == '>') && tmp_matches_last(p)) {
                tok_reset(p);
                p->is_end_tag = 1;
                buf_add(&p->name, p->tmp.p, p->tmp.len, 4096, &p->oom);
                p->state = S_DATA;
                if (c == '>') {
                    p->pos++;
                    emit_tag(p);
                } else if (c == '/') {
                    p->pos++;
                    p->state = S_SELF_CLOSING;
                } else {
                    p->pos++;
                    p->state = S_BEFORE_ATTR_NAME;
                }
            } else if (h_alnum(c) || c == '-' || c == '_') {
                tmp_putc(p, c);
                p->pos++;
            } else {
                raw_bail(p, 1, 1);
                p->state = S_SCRIPT;
            }
            break;

        case S_SCRIPT_ESC_START:
            if (c == '-') {
                tb_chars(p, "-", 1);
                p->pos++;
                p->state = S_SCRIPT_ESC_START_DASH;
            } else {
                p->state = S_SCRIPT;
            }
            break;

        case S_SCRIPT_ESC_START_DASH:
            if (c == '-') {
                tb_chars(p, "-", 1);
                p->pos++;
                p->state = S_SCRIPT_ESC_DASH_DASH;
            } else {
                p->state = S_SCRIPT;
            }
            break;

        case S_SCRIPT_ESC:
            if (c == '-') {
                tb_chars(p, "-", 1);
                p->pos++;
                p->state = S_SCRIPT_ESC_DASH;
            } else if (c == '<') {
                p->pos++;
                p->state = S_SCRIPT_ESC_LT;
            } else {
                unsigned long start = p->pos;
                while (p->pos < p->len && p->src[p->pos] != '<' &&
                       p->src[p->pos] != '-')
                    p->pos++;
                tb_chars(p, p->src + start, p->pos - start);
            }
            break;

        case S_SCRIPT_ESC_DASH:
            if (c == '-') {
                tb_chars(p, "-", 1);
                p->pos++;
                p->state = S_SCRIPT_ESC_DASH_DASH;
            } else if (c == '<') {
                p->pos++;
                p->state = S_SCRIPT_ESC_LT;
            } else {
                tb_chars(p, p->src + p->pos, 1);
                p->pos++;
                p->state = S_SCRIPT_ESC;
            }
            break;

        case S_SCRIPT_ESC_DASH_DASH:
            if (c == '-') {
                tb_chars(p, "-", 1);
                p->pos++;
            } else if (c == '<') {
                p->pos++;
                p->state = S_SCRIPT_ESC_LT;
            } else if (c == '>') {
                tb_chars(p, ">", 1);
                p->pos++;
                p->state = S_SCRIPT;
            } else {
                tb_chars(p, p->src + p->pos, 1);
                p->pos++;
                p->state = S_SCRIPT_ESC;
            }
            break;

        case S_SCRIPT_ESC_LT:
            if (c == '/') {
                p->pos++;
                tmp_reset(p);
                p->state = S_SCRIPT_ESC_END_OPEN;
            } else if (h_alpha(c)) {
                tmp_reset(p);
                tb_chars(p, "<", 1);
                p->state = S_SCRIPT_DESC_START;
            } else {
                raw_bail(p, 0, 0);
                p->state = S_SCRIPT_ESC;
            }
            break;

        case S_SCRIPT_ESC_END_OPEN:
            if (h_alpha(c)) {
                p->state = S_SCRIPT_ESC_END_NAME;
            } else {
                raw_bail(p, 1, 0);
                p->state = S_SCRIPT_ESC;
            }
            break;

        case S_SCRIPT_ESC_END_NAME:
            if ((h_space(c) || c == '/' || c == '>') && tmp_matches_last(p)) {
                tok_reset(p);
                p->is_end_tag = 1;
                buf_add(&p->name, p->tmp.p, p->tmp.len, 4096, &p->oom);
                p->state = S_DATA;
                if (c == '>') {
                    p->pos++;
                    emit_tag(p);
                } else if (c == '/') {
                    p->pos++;
                    p->state = S_SELF_CLOSING;
                } else {
                    p->pos++;
                    p->state = S_BEFORE_ATTR_NAME;
                }
            } else if (h_alnum(c) || c == '-' || c == '_') {
                tmp_putc(p, c);
                p->pos++;
            } else {
                raw_bail(p, 1, 1);
                p->state = S_SCRIPT_ESC;
            }
            break;

        case S_SCRIPT_DESC_START:
            if (h_space(c) || c == '/' || c == '>') {
                tb_chars(p, p->src + p->pos, 1);
                p->pos++;
                p->state = (p->tmp.len == 6 &&
                            memcmp(p->tmp.p, "script", 6) == 0)
                           ? S_SCRIPT_DESC : S_SCRIPT_ESC;
            } else if (h_alpha(c)) {
                tmp_putc(p, h_lower(c));
                tb_chars(p, p->src + p->pos, 1);
                p->pos++;
            } else {
                p->state = S_SCRIPT_ESC;
            }
            break;

        case S_SCRIPT_DESC:
            if (c == '-') {
                tb_chars(p, "-", 1);
                p->pos++;
                p->state = S_SCRIPT_DESC_DASH;
            } else if (c == '<') {
                tb_chars(p, "<", 1);
                p->pos++;
                p->state = S_SCRIPT_DESC_LT;
            } else {
                unsigned long start = p->pos;
                while (p->pos < p->len && p->src[p->pos] != '<' &&
                       p->src[p->pos] != '-')
                    p->pos++;
                tb_chars(p, p->src + start, p->pos - start);
            }
            break;

        case S_SCRIPT_DESC_DASH:
            if (c == '-') {
                tb_chars(p, "-", 1);
                p->pos++;
                p->state = S_SCRIPT_DESC_DASH_DASH;
            } else if (c == '<') {
                tb_chars(p, "<", 1);
                p->pos++;
                p->state = S_SCRIPT_DESC_LT;
            } else {
                tb_chars(p, p->src + p->pos, 1);
                p->pos++;
                p->state = S_SCRIPT_DESC;
            }
            break;

        case S_SCRIPT_DESC_DASH_DASH:
            if (c == '-') {
                tb_chars(p, "-", 1);
                p->pos++;
            } else if (c == '<') {
                tb_chars(p, "<", 1);
                p->pos++;
                p->state = S_SCRIPT_DESC_LT;
            } else if (c == '>') {
                tb_chars(p, ">", 1);
                p->pos++;
                p->state = S_SCRIPT;
            } else {
                tb_chars(p, p->src + p->pos, 1);
                p->pos++;
                p->state = S_SCRIPT_DESC;
            }
            break;

        case S_SCRIPT_DESC_LT:
            if (c == '/') {
                tmp_reset(p);
                tb_chars(p, "/", 1);
                p->pos++;
                p->state = S_SCRIPT_DESC_END;
            } else {
                p->state = S_SCRIPT_DESC;
            }
            break;

        case S_SCRIPT_DESC_END:
            if (h_space(c) || c == '/' || c == '>') {
                tb_chars(p, p->src + p->pos, 1);
                p->pos++;
                p->state = (p->tmp.len == 6 &&
                            memcmp(p->tmp.p, "script", 6) == 0)
                           ? S_SCRIPT_ESC : S_SCRIPT_DESC;
            } else if (h_alpha(c)) {
                tmp_putc(p, h_lower(c));
                tb_chars(p, p->src + p->pos, 1);
                p->pos++;
            } else {
                p->state = S_SCRIPT_DESC;
            }
            break;

        default:
            /* Unreachable, but a corrupted state must still make
             * progress rather than spin. */
            p->pos++;
            p->state = S_DATA;
            break;
        }
    }

    handle_eof(p);
}

/* ------------------------------------------------------------------ *
 * Entry points
 * ------------------------------------------------------------------ */

struct dom_document *html_parse_document_limits(const char *src,
                                                unsigned long len,
                                                const struct dom_limits *lim)
{
    struct parser p;
    struct dom_document *d;

    d = dom_document_new_limits(lim);
    if (!d)
        return 0;

    memset(&p, 0, sizeof p);
    p.doc = d;
    p.src = src ? src : "";
    p.len = src ? len : 0;
    p.state = S_DATA;
    p.phase = PH_INITIAL;
    p.max_text = DOM_MAX_TEXT;
    if (p.len > dom_limit_input(d)) {
        p.len = dom_limit_input(d);
        d->truncated |= DOM_TRUNC_INPUT;
    }

    p.open = (struct open_ent *)malloc(DOM_MAX_OPEN * sizeof *p.open);
    if (!p.open) {
        d->oom = 1;
        return d;
    }
    html_entity_index_init(&p.ents);

    tokenize(&p);

    if (p.oom)
        d->oom = 1;

    free(p.open);
    free(p.attrs);
    buf_free(&p.text);
    buf_free(&p.cdata);
    buf_free(&p.name);
    buf_free(&p.abuf);
    buf_free(&p.tmp);
    buf_free(&p.last_start);
    return d;
}

struct dom_document *html_parse_document(const char *src, unsigned long len)
{
    return html_parse_document_limits(src, len, 0);
}
