/* dom.h - a real HTML5 document object model for the Kestrel browser.
 *
 * This replaces the flat box list of apps/html.h with a tree. Everything
 * above it - CSS selector matching, layout, painting and the JavaScript
 * bindings - reads this structure, so it is deliberately a contract and
 * not an implementation detail.
 *
 * Portability: this file and its .c files include only <stdlib.h> and
 * <string.h> and use plain 0 rather than NULL, so the same source builds
 * against the host libc (tools/test_dom.c) and against the KestrelOS
 * freestanding libc. No floating point anywhere. No global mutable
 * state: every table that needs an index builds it into the document.
 *
 *   html_parse_document()   bytes -> struct dom_document
 *
 * The parser is an HTML5 tokenizer (data, RCDATA, RAWTEXT, script data
 * with its escaped states, comments including the bogus-comment path,
 * DOCTYPE and CDATA) driving a tree builder that implements the parts of
 * the insertion-mode machinery real pages depend on: implied
 * <html>/<head>/<body>, auto-closing of <p>/<li>/<dt>/<dd>/<tr>/<td>/
 * <th>/<option>, implicit <tbody>, void elements, approximated table
 * foster parenting, and scope-aware end-tag matching so a stray </div>
 * cannot unwind the document.
 *
 * MEMORY MODEL
 *   Every node, string and attribute array belongs to the document and
 *   is carved out of chunked arenas that are never reallocated, so node
 *   pointers are stable for the lifetime of the document. Freeing a
 *   document frees the chunks - there is no recursive tree walk, and
 *   therefore no way for a pathological document to blow the 16-page
 *   (64 KiB) user stack on teardown. The parser, the serializer and
 *   every traversal helper in this library are iterative for the same
 *   reason.
 *
 * LIMITS
 *   Malformed input is bounded, never fatal. Every cap below has a bit
 *   in dom_document.truncated so a caller can tell the user that what
 *   they are looking at is incomplete. Past a cap the parser keeps
 *   going with degraded structure; it does not abort and does not leak.
 *
 *   Measured cost: a realistic 2 MiB page produces 252,000 nodes and
 *   the document owns 37 MiB, about 18 bytes per byte of source. A
 *   typical 200 KiB page therefore costs a few megabytes.
 *   dom_memory_used() reports the real figure for a given document.
 *
 * TEXT ENCODING
 *   Text and attribute values are stored as UTF-8. Character references
 *   are decoded to UTF-8, including the legacy Windows-1252 remapping of
 *   numeric references in 0x80..0x9F. Bytes that are not valid UTF-8 are
 *   passed through unchanged. Because the framebuffer fonts are ASCII,
 *   dom_fold_ascii() is provided to render UTF-8 down to ASCII
 *   lookalikes at paint time - the DOM itself stays lossless.
 */

#pragma once

/* ------------------------------------------------------------------ *
 * Limits
 * ------------------------------------------------------------------ */

#define DOM_MAX_INPUT    (8UL * 1024UL * 1024UL) /* bytes of source     */
#define DOM_MAX_NODES    300000                  /* nodes per document  */
#define DOM_MAX_DEPTH    256                     /* tree depth          */
#define DOM_MAX_OPEN     1024                    /* open-element stack  */
#define DOM_MAX_ARENA    (64UL * 1024UL * 1024UL)/* total owned bytes   */
#define DOM_MAX_ATTRS    256                     /* attributes/element  */
#define DOM_MAX_TEXT     (4UL * 1024UL * 1024UL) /* one text node       */
#define DOM_MAX_COLLECT  (2UL * 1024UL * 1024UL) /* <style>/<script>    */

/* dom_document.truncated bits */
#define DOM_TRUNC_INPUT  0x01u  /* source longer than the input cap     */
#define DOM_TRUNC_NODES  0x02u  /* node cap hit; later nodes dropped    */
#define DOM_TRUNC_DEPTH  0x04u  /* depth cap hit; subtree flattened     */
#define DOM_TRUNC_MEMORY 0x08u  /* arena cap hit                        */
#define DOM_TRUNC_ATTRS  0x10u  /* an element had too many attributes   */
#define DOM_TRUNC_TEXT   0x20u  /* a text node was clipped              */
#define DOM_TRUNC_COLLECT 0x40u /* collected style/script text clipped  */
#define DOM_TRUNC_OPEN   0x80u  /* open-element stack cap hit           */

/* dom_document.repairs bits: structural repairs the parser had to make.
 * Not errors - the tree is fine - but a caller may want to say so, and
 * they mark the one case where serialize/re-parse is not guaranteed to
 * reproduce the tree exactly. */
#define DOM_REPAIR_FOSTER 0x01u /* an element was moved out of a table */

/* ------------------------------------------------------------------ *
 * Node types
 * ------------------------------------------------------------------ */

#define DOM_DOCUMENT 0
#define DOM_ELEMENT  1
#define DOM_TEXT     2
#define DOM_COMMENT  3
#define DOM_DOCTYPE  4

/* ------------------------------------------------------------------ *
 * Tag identifiers
 *
 * Every element carries a small integer id so consumers can switch()
 * instead of strcmp(). Elements this table does not know get
 * HTAG_UNKNOWN and keep their name and their children.
 * ------------------------------------------------------------------ */

enum {
    HTAG_UNKNOWN = 0,
    HTAG_A, HTAG_ABBR, HTAG_ADDRESS, HTAG_APPLET, HTAG_AREA, HTAG_ARTICLE,
    HTAG_ASIDE, HTAG_AUDIO, HTAG_B, HTAG_BASE, HTAG_BASEFONT, HTAG_BDI,
    HTAG_BDO, HTAG_BIG, HTAG_BLOCKQUOTE, HTAG_BODY, HTAG_BR, HTAG_BUTTON,
    HTAG_CANVAS, HTAG_CAPTION, HTAG_CENTER, HTAG_CITE, HTAG_CODE, HTAG_COL,
    HTAG_COLGROUP, HTAG_DATA, HTAG_DATALIST, HTAG_DD, HTAG_DEL,
    HTAG_DETAILS, HTAG_DFN, HTAG_DIALOG, HTAG_DIR, HTAG_DIV, HTAG_DL,
    HTAG_DT, HTAG_EM, HTAG_EMBED, HTAG_FIELDSET, HTAG_FIGCAPTION,
    HTAG_FIGURE, HTAG_FONT, HTAG_FOOTER, HTAG_FORM, HTAG_FRAME,
    HTAG_FRAMESET, HTAG_H1, HTAG_H2, HTAG_H3, HTAG_H4, HTAG_H5, HTAG_H6,
    HTAG_HEAD, HTAG_HEADER, HTAG_HGROUP, HTAG_HR, HTAG_HTML, HTAG_I,
    HTAG_IFRAME, HTAG_IMG, HTAG_INPUT, HTAG_INS, HTAG_KBD, HTAG_KEYGEN,
    HTAG_LABEL, HTAG_LEGEND, HTAG_LI, HTAG_LINK, HTAG_LISTING, HTAG_MAIN,
    HTAG_MAP, HTAG_MARK, HTAG_MARQUEE, HTAG_MENU, HTAG_META, HTAG_METER,
    HTAG_NAV, HTAG_NOBR, HTAG_NOFRAMES, HTAG_NOSCRIPT, HTAG_OBJECT,
    HTAG_OL, HTAG_OPTGROUP, HTAG_OPTION, HTAG_OUTPUT, HTAG_P, HTAG_PARAM,
    HTAG_PICTURE, HTAG_PRE, HTAG_PROGRESS, HTAG_Q, HTAG_RP, HTAG_RT,
    HTAG_RUBY, HTAG_S, HTAG_SAMP, HTAG_SCRIPT, HTAG_SECTION, HTAG_SELECT,
    HTAG_SLOT, HTAG_SMALL, HTAG_SOURCE, HTAG_SPAN, HTAG_STRIKE,
    HTAG_STRONG, HTAG_STYLE, HTAG_SUB, HTAG_SUMMARY, HTAG_SUP, HTAG_SVG,
    HTAG_TABLE, HTAG_TBODY, HTAG_TD, HTAG_TEMPLATE, HTAG_TEXTAREA,
    HTAG_TFOOT, HTAG_TH, HTAG_THEAD, HTAG_TIME, HTAG_TITLE, HTAG_TR,
    HTAG_TRACK, HTAG_TT, HTAG_U, HTAG_UL, HTAG_VAR, HTAG_VIDEO, HTAG_WBR,
    HTAG_XMP,
    HTAG__COUNT
};

/* Tag flags, from dom_tag_flags(). */
#define DTF_VOID     0x0001u /* no end tag, never has children          */
#define DTF_RAWTEXT  0x0002u /* content is raw text (style, script)     */
#define DTF_RCDATA   0x0004u /* raw text but entities decode (title)    */
#define DTF_BLOCK    0x0008u /* default CSS display is block-ish        */
#define DTF_SPECIAL  0x0010u /* HTML5 "special": stops end-tag search   */
#define DTF_FORMAT   0x0020u /* formatting element (b, i, a, font, ...) */
#define DTF_HEAD     0x0040u /* metadata content, belongs in <head>     */
#define DTF_TABLESEC 0x0080u /* table structure element                 */
#define DTF_HEADING  0x0100u /* h1..h6                                  */
#define DTF_SCOPE    0x0200u /* scope boundary (table, td, th, ...)     */

/* ------------------------------------------------------------------ *
 * Structures
 * ------------------------------------------------------------------ */

struct dom_document;

struct dom_attr {
    const char *name;   /* interned, ASCII-lowercased, NUL-terminated */
    const char *value;  /* NUL-terminated UTF-8, owned by the document */
    unsigned long len;  /* strlen(value), precomputed                  */
};

struct dom_node {
    unsigned char type;         /* DOM_*                                */
    unsigned char self_closed;  /* source said <x/>                     */
    unsigned short nattr;
    unsigned short cattr;       /* private: attribute array capacity    */
    int tag_id;                 /* HTAG_*, elements only                */
    unsigned int depth;         /* 0 for the document node              */
    unsigned int index;         /* serial in document order at parse    */
    const char *tag;            /* interned lowercase name, or 0        */
    char *text;                 /* text/comment data, doctype name      */
    unsigned long text_len;
    struct dom_attr *attr;
    struct dom_node *parent;
    struct dom_node *first_child;
    struct dom_node *last_child;
    struct dom_node *prev_sibling;
    struct dom_node *next_sibling;
    struct dom_document *doc;

    /* Reserved for the layers above. The DOM never reads or frees
     * these; whoever sets them owns them. */
    void *style;                /* computed style (the CSS agent)       */
    void *user;                 /* anything else (layout, JS wrappers)  */
};

struct dom_document {
    struct dom_node *root;      /* the DOM_DOCUMENT node, never 0       */
    struct dom_node *html;      /* <html>, created implicitly if absent */
    struct dom_node *head;
    struct dom_node *body;

    const char *title;          /* <title> text, "" when absent         */

    /* Concatenated contents of every <style> and every <script>, in
     * document order, each element's text followed by a '\n'. The CSS
     * and JS agents consume these. Always NUL-terminated. */
    char *style_text;
    unsigned long style_len;
    char *script_text;
    unsigned long script_len;

    unsigned int nnodes;
    unsigned int truncated;     /* DOM_TRUNC_* bits                     */
    unsigned int repairs;       /* DOM_REPAIR_* bits                    */
    int oom;                    /* an allocation failed during parsing  */

    /* private */
    void *priv;
};

/* Caps for one parse. A zero field means "use the compiled-in
 * default"; this exists so tests can drive the limit paths cheaply. */
struct dom_limits {
    unsigned long max_input;
    unsigned long max_nodes;
    unsigned long max_depth;
    unsigned long max_arena;
};

/* ------------------------------------------------------------------ *
 * Parsing
 * ------------------------------------------------------------------ */

/* Parse len bytes of HTML into a fresh document. Returns 0 only when
 * out of memory; every other failure mode is reported through
 * dom_document.truncated and still yields a usable tree. */
struct dom_document *html_parse_document(const char *src, unsigned long len);
struct dom_document *html_parse_document_limits(const char *src,
                                                unsigned long len,
                                                const struct dom_limits *lim);

/* ------------------------------------------------------------------ *
 * Document lifetime and manual construction
 * ------------------------------------------------------------------ */

struct dom_document *dom_document_new(void);
struct dom_document *dom_document_new_limits(const struct dom_limits *lim);
void dom_document_free(struct dom_document *d);

/* Node factories. `taglen` < 0 means "NUL-terminated". Tag names are
 * ASCII-lowercased and interned. Return 0 on OOM or when a cap is hit
 * (the document's truncated/oom fields say which). */
struct dom_node *dom_create_element(struct dom_document *d,
                                    const char *tag, long taglen);
struct dom_node *dom_create_text(struct dom_document *d,
                                 const char *s, unsigned long n);
struct dom_node *dom_create_comment(struct dom_document *d,
                                    const char *s, unsigned long n);

/* Tree editing. Return 1 on success, 0 when the move is illegal (a
 * cycle, a null argument, or a depth-cap violation). */
int dom_append_child(struct dom_node *parent, struct dom_node *child);
int dom_insert_before(struct dom_node *parent, struct dom_node *child,
                      struct dom_node *ref);
void dom_remove_child(struct dom_node *parent, struct dom_node *child);

/* ------------------------------------------------------------------ *
 * Attributes
 * ------------------------------------------------------------------ */

/* Case-insensitive lookup; returns 0 when absent, "" for a valueless
 * attribute. `namelen` < 0 means NUL-terminated. */
const char *dom_get_attr(const struct dom_node *n, const char *name);
const char *dom_get_attr_n(const struct dom_node *n,
                           const char *name, long namelen);
int dom_has_attr(const struct dom_node *n, const char *name);

/* Sets (or replaces) an attribute. Returns 1 on success. */
int dom_set_attr(struct dom_node *n, const char *name, const char *value);
int dom_set_attr_n(struct dom_node *n, const char *name, long namelen,
                   const char *value, unsigned long vallen);
int dom_remove_attr(struct dom_node *n, const char *name);

unsigned int dom_attr_count(const struct dom_node *n);
const struct dom_attr *dom_attr_at(const struct dom_node *n, unsigned int i);

/* ------------------------------------------------------------------ *
 * Traversal
 * ------------------------------------------------------------------ */

/* Next node in document order over the whole tree, or 0 at the end. */
struct dom_node *dom_next(const struct dom_node *n);

/* Next node in document order without leaving `root`'s subtree. */
struct dom_node *dom_next_within(const struct dom_node *n,
                                 const struct dom_node *root);

/* Same, but does not descend into `n`'s children. */
struct dom_node *dom_next_skip(const struct dom_node *n,
                               const struct dom_node *root);

struct dom_node *dom_first_element_child(const struct dom_node *n);
struct dom_node *dom_next_element_sibling(const struct dom_node *n);
struct dom_node *dom_parent_element(const struct dom_node *n);

/* Index of `n` among its parent's element children, counting from 1;
 * 0 when it has no parent. Selector matching needs this. */
unsigned int dom_element_index(const struct dom_node *n);
unsigned int dom_element_child_count(const struct dom_node *n);

/* ------------------------------------------------------------------ *
 * Queries
 * ------------------------------------------------------------------ */

/* O(1) average. Returns the first element in document order whose id
 * attribute equals `id` exactly, or 0. Stays correct when an id is
 * changed or removed after parsing, at the cost of one document walk
 * the first time a stale entry is consulted. */
struct dom_node *dom_get_element_by_id(const struct dom_document *d,
                                       const char *id);

/* First element with this tag, searching `root`'s subtree in document
 * order. `root` may be the document node. */
struct dom_node *dom_find_tag(struct dom_node *root, const char *tag);

/* Whitespace-separated membership test on the class attribute. */
int dom_has_class(const struct dom_node *n, const char *cls);
int dom_has_class_n(const struct dom_node *n, const char *cls, long clslen);

/* Case-insensitive tag test. Cheaper: compare node->tag_id. */
int dom_tag_is(const struct dom_node *n, const char *tag);

/* Concatenated text of the subtree. dom_text_content() returns a
 * malloc'd NUL-terminated string the caller frees, or 0 on OOM; the
 * _into form writes at most cap bytes (always NUL-terminating when cap
 * > 0) and returns the length it wanted to write. */
char *dom_text_content(const struct dom_node *n, unsigned long *out_len);
unsigned long dom_text_content_into(const struct dom_node *n,
                                    char *buf, unsigned long cap);

/* ------------------------------------------------------------------ *
 * Serialization
 * ------------------------------------------------------------------ */

/* Serialize the subtree rooted at `n` (children only when `n` is the
 * document node) back to HTML. Round-trips: re-parsing the output
 * yields an identical tree. Caller frees with free(). */
char *dom_serialize(const struct dom_node *n, unsigned long *out_len);

/* ------------------------------------------------------------------ *
 * Tag metadata
 * ------------------------------------------------------------------ */

int dom_tag_lookup(const char *name, long len);   /* HTAG_*            */
unsigned int dom_tag_flags(int tag_id);           /* DTF_*             */
const char *dom_tag_name(int tag_id);             /* canonical name    */
int dom_tag_is_void(int tag_id);
int dom_heading_level(int tag_id);                /* 1..6, else 0      */

/* ------------------------------------------------------------------ *
 * Text helpers (entities.c)
 * ------------------------------------------------------------------ */

/* Fold UTF-8 to the ASCII the bitmap fonts can actually draw: accented
 * letters lose their accents, typographic punctuation becomes its ASCII
 * lookalike, everything else becomes '?'. Writes at most cap bytes,
 * NUL-terminating when cap > 0, and returns the length it wanted. */
unsigned long dom_fold_ascii(const char *utf8, unsigned long n,
                             char *out, unsigned long cap);

/* Decode one UTF-8 sequence. Stores the code point and returns the
 * number of bytes consumed (always >= 1; an invalid sequence yields
 * the raw byte and 1). */
unsigned long dom_utf8_decode(const char *s, unsigned long n,
                              unsigned long *cp);

/* Encode a code point as UTF-8 into out (>= 4 bytes). Returns length. */
int dom_utf8_encode(unsigned long cp, char *out);

/* Look one named character reference up by exact name (no '&', no
 * ';'). Returns 1 and stores the code point, or 0. */
int html_entity_lookup(const char *name, long len, unsigned long *cp);

/* Number of named references in the table; for tests. */
int html_entity_count(void);
const char *html_entity_name(int i, unsigned long *cp, int *legacy);

/* ---- named character reference matching ----
 *
 * The tokenizer needs longest-prefix matching, so the table gets a hash
 * index. The index is built into the caller's storage rather than a
 * file-scope cache: no global mutable state, and a parse is
 * self-contained. Building it costs one pass over the table.
 */

#define HTML_ENT_SLOTS   1024
#define HTML_ENT_MAXNAME 12     /* longest name, plus room for ';'      */

struct html_entity_index {
    unsigned short slot[HTML_ENT_SLOTS];
};

void html_entity_index_init(struct html_entity_index *ix);

/* Match the longest named reference at `s` (which points just PAST the
 * '&'). On success returns 1, stores the code point, the number of
 * bytes consumed from `s` in *used, and whether the match was
 * terminated by ';' in *semi. A match with *semi == 0 is a legacy
 * no-semicolon match, which callers must suppress inside attribute
 * values when the next byte is '=' or alphanumeric. */
int html_entity_match(const struct html_entity_index *ix,
                      const char *s, unsigned long avail,
                      int *used, int *semi, unsigned long *cp);

/* Apply the HTML5 numeric-reference fixups: NUL and out-of-range become
 * U+FFFD, surrogates become U+FFFD, and 0x80..0x9F map through the
 * Windows-1252 table. */
unsigned long html_numeric_fixup(unsigned long cp);

/* ------------------------------------------------------------------ *
 * Shared with the parser
 *
 * Public because html.c is a separate translation unit, not because
 * application code should reach for them.
 * ------------------------------------------------------------------ */

struct dom_node *dom_create_doctype(struct dom_document *d,
                                    const char *s, unsigned long n);
int dom_collect_style(struct dom_document *d, const char *s, unsigned long n);
int dom_collect_script(struct dom_document *d, const char *s, unsigned long n);
int dom_set_title(struct dom_document *d, const char *s, unsigned long n);

/* The caps this document was created with. */
unsigned long dom_limit_input(const struct dom_document *d);
unsigned long dom_limit_depth(const struct dom_document *d);

/* Every byte the document owns: nodes, strings, attribute arrays, the
 * intern table, the id index and the collected style/script text. The
 * browser shows this; the caps above bound it. */
unsigned long dom_memory_used(const struct dom_document *d);

/* Intern a name without creating a node. */
const char *dom_intern_name(struct dom_document *d, const char *s, long n);

/* Extend a text node. The tree builder keeps character data in a single
 * node per run; the serializer's round-trip guarantee depends on there
 * never being two adjacent text siblings. */
int dom_text_append(struct dom_node *t, const char *s, unsigned long n);
