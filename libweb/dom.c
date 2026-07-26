/* dom.c - the document object model: storage, tree editing, indexes,
 * traversal and serialization. See libweb/dom.h.
 *
 * Storage is chunked arenas that are never reallocated, so every node
 * pointer handed out stays valid until the document is freed. Freeing
 * walks a list of chunks, not the tree, so teardown cannot recurse and
 * cannot overflow the 64 KiB user stack no matter how the document is
 * shaped. Every traversal in this file is iterative for the same
 * reason.
 */

#include <stdlib.h>
#include <string.h>

#include "dom.h"

/* ------------------------------------------------------------------ *
 * ASCII helpers
 * ------------------------------------------------------------------ */

static int d_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int d_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int d_ieq(const char *a, const char *b, unsigned long n)
{
    unsigned long i;

    for (i = 0; i < n; i++) {
        if (d_lower((unsigned char)a[i]) != d_lower((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

static unsigned long d_hash(const char *s, unsigned long n)
{
    unsigned long h = 2166136261UL;
    unsigned long i;

    for (i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619UL;
        h &= 0xffffffffUL;
    }
    return h;
}

/* ------------------------------------------------------------------ *
 * Tag table
 *
 * Designated initializers keep this in sync with the enum by
 * construction: adding a tag in the middle of the enum cannot silently
 * shift every name by one.
 * ------------------------------------------------------------------ */

struct tag_row {
    const char *name;
    unsigned int flags;
};

#define BLK  DTF_BLOCK
#define SPC  DTF_SPECIAL
#define FMT  DTF_FORMAT
#define VOI  (DTF_VOID | DTF_SPECIAL)
#define HD   DTF_HEAD
#define TSEC DTF_TABLESEC
#define SCP  DTF_SCOPE

static const struct tag_row g_tag[HTAG__COUNT] = {
    [HTAG_UNKNOWN]    = { "",           0 },
    [HTAG_A]          = { "a",          FMT },
    [HTAG_ABBR]       = { "abbr",       0 },
    [HTAG_ADDRESS]    = { "address",    BLK | SPC },
    [HTAG_APPLET]     = { "applet",     SPC | SCP },
    [HTAG_AREA]       = { "area",       VOI },
    [HTAG_ARTICLE]    = { "article",    BLK | SPC },
    [HTAG_ASIDE]      = { "aside",      BLK | SPC },
    [HTAG_AUDIO]      = { "audio",      0 },
    [HTAG_B]          = { "b",          FMT },
    [HTAG_BASE]       = { "base",       VOI | HD },
    [HTAG_BASEFONT]   = { "basefont",   VOI | HD },
    [HTAG_BDI]        = { "bdi",        0 },
    [HTAG_BDO]        = { "bdo",        0 },
    [HTAG_BIG]        = { "big",        FMT },
    [HTAG_BLOCKQUOTE] = { "blockquote", BLK | SPC },
    [HTAG_BODY]       = { "body",       BLK | SPC },
    [HTAG_BR]         = { "br",         VOI },
    [HTAG_BUTTON]     = { "button",     SPC },
    [HTAG_CANVAS]     = { "canvas",     0 },
    [HTAG_CAPTION]    = { "caption",    BLK | SPC | TSEC | SCP },
    [HTAG_CENTER]     = { "center",     BLK | SPC },
    [HTAG_CITE]       = { "cite",       0 },
    [HTAG_CODE]       = { "code",       FMT },
    [HTAG_COL]        = { "col",        VOI | TSEC },
    [HTAG_COLGROUP]   = { "colgroup",   BLK | SPC | TSEC },
    [HTAG_DATA]       = { "data",       0 },
    [HTAG_DATALIST]   = { "datalist",   0 },
    [HTAG_DD]         = { "dd",         BLK | SPC },
    [HTAG_DEL]        = { "del",        0 },
    [HTAG_DETAILS]    = { "details",    BLK | SPC },
    [HTAG_DFN]        = { "dfn",        0 },
    [HTAG_DIALOG]     = { "dialog",     BLK | SPC },
    [HTAG_DIR]        = { "dir",        BLK | SPC },
    [HTAG_DIV]        = { "div",        BLK | SPC },
    [HTAG_DL]         = { "dl",         BLK | SPC },
    [HTAG_DT]         = { "dt",         BLK | SPC },
    [HTAG_EM]         = { "em",         FMT },
    [HTAG_EMBED]      = { "embed",      VOI },
    [HTAG_FIELDSET]   = { "fieldset",   BLK | SPC },
    [HTAG_FIGCAPTION] = { "figcaption", BLK | SPC },
    [HTAG_FIGURE]     = { "figure",     BLK | SPC },
    [HTAG_FONT]       = { "font",       FMT },
    [HTAG_FOOTER]     = { "footer",     BLK | SPC },
    [HTAG_FORM]       = { "form",       BLK | SPC },
    [HTAG_FRAME]      = { "frame",      VOI },
    [HTAG_FRAMESET]   = { "frameset",   BLK | SPC },
    [HTAG_H1]         = { "h1",         BLK | SPC | DTF_HEADING },
    [HTAG_H2]         = { "h2",         BLK | SPC | DTF_HEADING },
    [HTAG_H3]         = { "h3",         BLK | SPC | DTF_HEADING },
    [HTAG_H4]         = { "h4",         BLK | SPC | DTF_HEADING },
    [HTAG_H5]         = { "h5",         BLK | SPC | DTF_HEADING },
    [HTAG_H6]         = { "h6",         BLK | SPC | DTF_HEADING },
    [HTAG_HEAD]       = { "head",       BLK | SPC },
    [HTAG_HEADER]     = { "header",     BLK | SPC },
    [HTAG_HGROUP]     = { "hgroup",     BLK | SPC },
    [HTAG_HR]         = { "hr",         VOI | BLK },
    [HTAG_HTML]       = { "html",       BLK | SPC | SCP },
    [HTAG_I]          = { "i",          FMT },
    [HTAG_IFRAME]     = { "iframe",     DTF_RAWTEXT | SPC },
    [HTAG_IMG]        = { "img",        VOI },
    [HTAG_INPUT]      = { "input",      VOI },
    [HTAG_INS]        = { "ins",        0 },
    [HTAG_KBD]        = { "kbd",        0 },
    [HTAG_KEYGEN]     = { "keygen",     VOI },
    [HTAG_LABEL]      = { "label",      0 },
    [HTAG_LEGEND]     = { "legend",     BLK },
    [HTAG_LI]         = { "li",         BLK | SPC },
    [HTAG_LINK]       = { "link",       VOI | HD },
    [HTAG_LISTING]    = { "listing",    BLK | SPC },
    [HTAG_MAIN]       = { "main",       BLK | SPC },
    [HTAG_MAP]        = { "map",        0 },
    [HTAG_MARK]       = { "mark",       0 },
    [HTAG_MARQUEE]    = { "marquee",    SPC | SCP },
    [HTAG_MENU]       = { "menu",       BLK | SPC },
    [HTAG_META]       = { "meta",       VOI | HD },
    [HTAG_METER]      = { "meter",      0 },
    [HTAG_NAV]        = { "nav",        BLK | SPC },
    [HTAG_NOBR]       = { "nobr",       FMT },
    [HTAG_NOFRAMES]   = { "noframes",   DTF_RAWTEXT | SPC },
    [HTAG_NOSCRIPT]   = { "noscript",   SPC | HD },
    [HTAG_OBJECT]     = { "object",     SPC | SCP },
    [HTAG_OL]         = { "ol",         BLK | SPC },
    [HTAG_OPTGROUP]   = { "optgroup",   BLK },
    [HTAG_OPTION]     = { "option",     BLK },
    [HTAG_OUTPUT]     = { "output",     0 },
    [HTAG_P]          = { "p",          BLK | SPC },
    [HTAG_PARAM]      = { "param",      VOI },
    [HTAG_PICTURE]    = { "picture",    0 },
    [HTAG_PRE]        = { "pre",        BLK | SPC },
    [HTAG_PROGRESS]   = { "progress",   0 },
    [HTAG_Q]          = { "q",          0 },
    [HTAG_RP]         = { "rp",         0 },
    [HTAG_RT]         = { "rt",         0 },
    [HTAG_RUBY]       = { "ruby",       0 },
    [HTAG_S]          = { "s",          FMT },
    [HTAG_SAMP]       = { "samp",       0 },
    [HTAG_SCRIPT]     = { "script",     DTF_RAWTEXT | SPC | HD },
    [HTAG_SECTION]    = { "section",    BLK | SPC },
    [HTAG_SELECT]     = { "select",     SPC },
    [HTAG_SLOT]       = { "slot",       0 },
    [HTAG_SMALL]      = { "small",      FMT },
    [HTAG_SOURCE]     = { "source",     VOI },
    [HTAG_SPAN]       = { "span",       0 },
    [HTAG_STRIKE]     = { "strike",     FMT },
    [HTAG_STRONG]     = { "strong",     FMT },
    [HTAG_STYLE]      = { "style",      DTF_RAWTEXT | SPC | HD },
    [HTAG_SUB]        = { "sub",        0 },
    [HTAG_SUMMARY]    = { "summary",    BLK | SPC },
    [HTAG_SUP]        = { "sup",        0 },
    [HTAG_SVG]        = { "svg",        0 },
    [HTAG_TABLE]      = { "table",      BLK | SPC | SCP },
    [HTAG_TBODY]      = { "tbody",      BLK | SPC | TSEC },
    [HTAG_TD]         = { "td",         BLK | SPC | TSEC | SCP },
    [HTAG_TEMPLATE]   = { "template",   SPC | HD | SCP },
    [HTAG_TEXTAREA]   = { "textarea",   DTF_RCDATA | SPC },
    [HTAG_TFOOT]      = { "tfoot",      BLK | SPC | TSEC },
    [HTAG_TH]         = { "th",         BLK | SPC | TSEC | SCP },
    [HTAG_THEAD]      = { "thead",      BLK | SPC | TSEC },
    [HTAG_TIME]       = { "time",       0 },
    [HTAG_TITLE]      = { "title",      DTF_RCDATA | SPC | HD },
    [HTAG_TR]         = { "tr",         BLK | SPC | TSEC },
    [HTAG_TRACK]      = { "track",      VOI },
    [HTAG_TT]         = { "tt",         FMT },
    [HTAG_U]          = { "u",          FMT },
    [HTAG_UL]         = { "ul",         BLK | SPC },
    [HTAG_VAR]        = { "var",        0 },
    [HTAG_VIDEO]      = { "video",      0 },
    [HTAG_WBR]        = { "wbr",        VOI },
    [HTAG_XMP]        = { "xmp",        DTF_RAWTEXT | BLK | SPC }
};

int dom_tag_lookup(const char *name, long len)
{
    unsigned long n;
    int i;

    if (!name)
        return HTAG_UNKNOWN;
    n = (len < 0) ? strlen(name) : (unsigned long)len;
    if (n == 0 || n > 10)
        return HTAG_UNKNOWN;
    for (i = 1; i < HTAG__COUNT; i++) {
        if (strlen(g_tag[i].name) == n && d_ieq(g_tag[i].name, name, n))
            return i;
    }
    return HTAG_UNKNOWN;
}

unsigned int dom_tag_flags(int tag_id)
{
    if (tag_id <= 0 || tag_id >= HTAG__COUNT)
        return 0;
    return g_tag[tag_id].flags;
}

const char *dom_tag_name(int tag_id)
{
    if (tag_id < 0 || tag_id >= HTAG__COUNT)
        return "";
    return g_tag[tag_id].name;
}

int dom_tag_is_void(int tag_id)
{
    return (dom_tag_flags(tag_id) & DTF_VOID) != 0;
}

int dom_heading_level(int tag_id)
{
    if (tag_id >= HTAG_H1 && tag_id <= HTAG_H6)
        return tag_id - HTAG_H1 + 1;
    return 0;
}

/* ------------------------------------------------------------------ *
 * Private document state
 * ------------------------------------------------------------------ */

struct dom_chunk {
    struct dom_chunk *next;
    unsigned long used, cap;
};

struct intern_ent {
    char *s;
    unsigned long len, hash;
    int tag_id;
};

struct id_ent {
    const char *id;
    unsigned long hash;
    struct dom_node *node;
};

struct dom_priv {
    struct dom_chunk *chunks;
    char empty[8];              /* the string a failed allocation gets */
    unsigned long arena_total;      /* bytes handed to malloc for chunks */

    unsigned long max_input, max_nodes, max_depth, max_arena;

    struct intern_ent *itab;
    unsigned long icap, icount;

    struct id_ent *idtab;
    unsigned long idcap, idcount;

    unsigned long style_cap, script_cap;
};

#define CHUNK_MIN  (8UL * 1024UL)
#define CHUNK_MAX  (1024UL * 1024UL)

static void *arena_alloc(struct dom_document *d, unsigned long n)
{
    struct dom_priv *pv = (struct dom_priv *)d->priv;
    struct dom_chunk *c;
    unsigned long want, hdr = sizeof(struct dom_chunk);
    char *base;

    n = (n + 15UL) & ~15UL;
    if (n == 0)
        n = 16;

    c = pv->chunks;
    if (c && c->cap - c->used >= n) {
        base = (char *)(c + 1) + c->used;
        c->used += n;
        memset(base, 0, n);
        return base;
    }

    want = CHUNK_MIN;
    while (want < n && want < CHUNK_MAX)
        want *= 2;
    if (want < n)
        want = n;
    if (pv->arena_total + want + hdr > pv->max_arena) {
        d->truncated |= DOM_TRUNC_MEMORY;
        return 0;
    }
    c = (struct dom_chunk *)malloc(hdr + want);
    if (!c) {
        d->oom = 1;
        return 0;
    }
    c->cap = want;
    c->used = n;
    c->next = pv->chunks;
    pv->chunks = c;
    pv->arena_total += hdr + want;
    base = (char *)(c + 1);
    memset(base, 0, n);
    return base;
}

/* Copy n bytes plus a NUL into the arena. */
static char *arena_str(struct dom_document *d, const char *s, unsigned long n)
{
    char *p = (char *)arena_alloc(d, n + 1);

    if (!p)
        return 0;
    if (n)
        memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* ---- interning ---- */

static int intern_grow(struct dom_document *d)
{
    struct dom_priv *pv = (struct dom_priv *)d->priv;
    unsigned long ncap = pv->icap ? pv->icap * 2 : 128;
    struct intern_ent *nt;
    unsigned long i;

    nt = (struct intern_ent *)calloc(ncap, sizeof *nt);
    if (!nt) {
        d->oom = 1;
        return 0;
    }
    for (i = 0; i < pv->icap; i++) {
        unsigned long h;
        if (!pv->itab[i].s)
            continue;
        h = pv->itab[i].hash & (ncap - 1);
        while (nt[h].s)
            h = (h + 1) & (ncap - 1);
        nt[h] = pv->itab[i];
    }
    free(pv->itab);
    pv->itab = nt;
    pv->icap = ncap;
    return 1;
}

/* Intern a name, ASCII-lowercasing it. Returns the canonical pointer
 * and, when tag_id is non-null, the HTAG_* id computed once per
 * distinct name rather than once per occurrence. */
static const char *intern(struct dom_document *d, const char *s,
                          unsigned long n, int *tag_id)
{
    struct dom_priv *pv = (struct dom_priv *)d->priv;
    char stackbuf[64];
    char *low = stackbuf;
    unsigned long h, i;

    if (n > sizeof stackbuf) {
        low = (char *)malloc(n);
        if (!low) {
            d->oom = 1;
            return 0;
        }
    }
    for (i = 0; i < n; i++)
        low[i] = (char)d_lower((unsigned char)s[i]);

    if (pv->icount * 4 >= pv->icap * 3) {
        if (!intern_grow(d)) {
            if (low != stackbuf)
                free(low);
            return 0;
        }
    }
    h = d_hash(low, n);
    i = h & (pv->icap - 1);
    for (;;) {
        struct intern_ent *e = &pv->itab[i];
        if (!e->s)
            break;
        if (e->len == n && e->hash == h && memcmp(e->s, low, n) == 0) {
            if (tag_id)
                *tag_id = e->tag_id;
            if (low != stackbuf)
                free(low);
            return e->s;
        }
        i = (i + 1) & (pv->icap - 1);
    }
    {
        struct intern_ent *e = &pv->itab[i];
        e->s = arena_str(d, low, n);
        if (!e->s) {
            if (low != stackbuf)
                free(low);
            return 0;
        }
        e->len = n;
        e->hash = h;
        e->tag_id = dom_tag_lookup(e->s, (long)n);
        pv->icount++;
        if (tag_id)
            *tag_id = e->tag_id;
        if (low != stackbuf)
            free(low);
        return e->s;
    }
}

/* ---- id index ---- */

static int id_grow(struct dom_document *d)
{
    struct dom_priv *pv = (struct dom_priv *)d->priv;
    unsigned long ncap = pv->idcap ? pv->idcap * 2 : 64;
    struct id_ent *nt;
    unsigned long i;

    nt = (struct id_ent *)calloc(ncap, sizeof *nt);
    if (!nt) {
        d->oom = 1;
        return 0;
    }
    for (i = 0; i < pv->idcap; i++) {
        unsigned long h;
        if (!pv->idtab[i].id)
            continue;
        h = pv->idtab[i].hash & (ncap - 1);
        while (nt[h].id)
            h = (h + 1) & (ncap - 1);
        nt[h] = pv->idtab[i];
    }
    free(pv->idtab);
    pv->idtab = nt;
    pv->idcap = ncap;
    return 1;
}

/* First element in document order wins, matching getElementById. */
static void id_insert(struct dom_document *d, const char *id,
                      struct dom_node *n)
{
    struct dom_priv *pv = (struct dom_priv *)d->priv;
    unsigned long len = strlen(id), h, i;

    if (len == 0)
        return;
    if (pv->idcount * 4 >= pv->idcap * 3) {
        if (!id_grow(d))
            return;
    }
    h = d_hash(id, len);
    i = h & (pv->idcap - 1);
    for (;;) {
        struct id_ent *e = &pv->idtab[i];
        if (!e->id) {
            e->id = id;
            e->hash = h;
            e->node = n;
            pv->idcount++;
            return;
        }
        if (e->hash == h && strcmp(e->id, id) == 0)
            return;                     /* an earlier element owns it */
        i = (i + 1) & (pv->idcap - 1);
    }
}

/* Fallback for the rare case where the index has gone stale: an id
 * attribute was changed or removed after it was indexed. Linear, but
 * only reached when the fast path's answer no longer holds. */
static struct dom_node *id_scan(const struct dom_document *d, const char *id)
{
    struct dom_node *n;

    for (n = d->root; n; n = dom_next(n)) {
        const char *v;
        if (n->type != DOM_ELEMENT)
            continue;
        v = dom_get_attr_n(n, "id", 2);
        if (v && strcmp(v, id) == 0)
            return n;
    }
    return 0;
}

struct dom_node *dom_get_element_by_id(const struct dom_document *d,
                                       const char *id)
{
    struct dom_priv *pv;
    unsigned long len, h, i;

    if (!d || !id)
        return 0;
    pv = (struct dom_priv *)d->priv;
    if (!pv || !pv->idtab)
        return 0;
    len = strlen(id);
    if (len == 0)
        return 0;
    h = d_hash(id, len);
    i = h & (pv->idcap - 1);
    for (;;) {
        struct id_ent *e = &pv->idtab[i];
        const char *v;
        if (!e->id)
            return 0;
        if (e->hash == h && strcmp(e->id, id) == 0) {
            /* The index is add-only, so an entry can outlive the
             * attribute that created it. Confirm before answering. */
            v = e->node ? dom_get_attr_n(e->node, "id", 2) : 0;
            if (v && strcmp(v, id) == 0 && e->node->parent)
                return e->node;
            e->node = id_scan(d, id);
            return e->node;
        }
        i = (i + 1) & (pv->idcap - 1);
    }
}

/* ------------------------------------------------------------------ *
 * Document lifetime
 * ------------------------------------------------------------------ */

struct dom_document *dom_document_new_limits(const struct dom_limits *lim)
{
    struct dom_document *d;
    struct dom_priv *pv;

    d = (struct dom_document *)calloc(1, sizeof *d);
    if (!d)
        return 0;
    pv = (struct dom_priv *)calloc(1, sizeof *pv);
    if (!pv) {
        free(d);
        return 0;
    }
    d->priv = pv;
    pv->max_input = (lim && lim->max_input) ? lim->max_input : DOM_MAX_INPUT;
    pv->max_nodes = (lim && lim->max_nodes) ? lim->max_nodes : DOM_MAX_NODES;
    pv->max_depth = (lim && lim->max_depth) ? lim->max_depth : DOM_MAX_DEPTH;
    pv->max_arena = (lim && lim->max_arena) ? lim->max_arena : DOM_MAX_ARENA;

    if (!intern_grow(d) || !id_grow(d)) {
        dom_document_free(d);
        return 0;
    }

    d->root = (struct dom_node *)arena_alloc(d, sizeof *d->root);
    if (!d->root) {
        dom_document_free(d);
        return 0;
    }
    d->root->type = DOM_DOCUMENT;
    d->root->doc = d;
    d->root->depth = 0;
    d->root->tag = "#document";
    d->nnodes = 1;

    d->title = "";
    /* These two grow with realloc rather than living in the arena,
     * because the CSS and JS agents want one flat buffer. They are the
     * only heap blocks the document owns outside its chunks. */
    d->style_text = (char *)malloc(1);
    d->script_text = (char *)malloc(1);
    if (!d->style_text || !d->script_text) {
        dom_document_free(d);
        return 0;
    }
    d->style_text[0] = 0;
    d->script_text[0] = 0;
    pv->style_cap = 1;
    pv->script_cap = 1;
    return d;
}

struct dom_document *dom_document_new(void)
{
    return dom_document_new_limits(0);
}

void dom_document_free(struct dom_document *d)
{
    struct dom_priv *pv;
    struct dom_chunk *c;

    if (!d)
        return;
    pv = (struct dom_priv *)d->priv;
    if (pv) {
        c = pv->chunks;
        while (c) {
            struct dom_chunk *nx = c->next;
            free(c);
            c = nx;
        }
        free(pv->itab);
        free(pv->idtab);
        free(pv);
    }
    free(d->style_text);
    free(d->script_text);
    free(d);
}

/* Input cap lives in priv; the parser asks for it. */
unsigned long dom_limit_input(const struct dom_document *d)
{
    return ((struct dom_priv *)d->priv)->max_input;
}

unsigned long dom_limit_depth(const struct dom_document *d)
{
    return ((struct dom_priv *)d->priv)->max_depth;
}

unsigned long dom_memory_used(const struct dom_document *d)
{
    const struct dom_priv *pv;

    if (!d || !d->priv)
        return 0;
    pv = (const struct dom_priv *)d->priv;
    return pv->arena_total + pv->icap * sizeof(struct intern_ent) +
           pv->idcap * sizeof(struct id_ent) + pv->style_cap +
           pv->script_cap + sizeof *d + sizeof *pv;
}

/* ------------------------------------------------------------------ *
 * Node construction
 * ------------------------------------------------------------------ */

static struct dom_node *node_new(struct dom_document *d, int type)
{
    struct dom_priv *pv = (struct dom_priv *)d->priv;
    struct dom_node *n;

    if (d->nnodes >= pv->max_nodes) {
        d->truncated |= DOM_TRUNC_NODES;
        return 0;
    }
    n = (struct dom_node *)arena_alloc(d, sizeof *n);
    if (!n)
        return 0;
    n->type = (unsigned char)type;
    n->doc = d;
    n->index = d->nnodes;
    n->tag = "";
    d->nnodes++;
    return n;
}

const char *dom_intern_name(struct dom_document *d, const char *s, long n)
{
    unsigned long len;

    if (!d || !s)
        return 0;
    len = (n < 0) ? strlen(s) : (unsigned long)n;
    if (len == 0)
        return 0;
    if (len > 128)
        len = 128;
    return intern(d, s, len, 0);
}

/* Append to an existing text node. HTML5 never produces two adjacent
 * text nodes, and the serializer relies on that: "a" followed by "b"
 * would write "ab" and re-parse as a single node. */
int dom_text_append(struct dom_node *t, const char *s, unsigned long n)
{
    char *p;

    if (!t || t->type != DOM_TEXT || !s)
        return 0;
    if (n == 0)
        return 1;
    if (t->text_len + n > DOM_MAX_TEXT) {
        t->doc->truncated |= DOM_TRUNC_TEXT;
        return 0;
    }
    p = (char *)arena_alloc(t->doc, t->text_len + n + 1);
    if (!p)
        return 0;
    memcpy(p, t->text, t->text_len);
    memcpy(p + t->text_len, s, n);
    p[t->text_len + n] = 0;
    t->text = p;
    t->text_len += n;
    return 1;
}

struct dom_node *dom_create_element(struct dom_document *d,
                                    const char *tag, long taglen)
{
    struct dom_node *n;
    unsigned long n_len;
    const char *interned;
    int tid = HTAG_UNKNOWN;

    if (!d || !tag)
        return 0;
    n_len = (taglen < 0) ? strlen(tag) : (unsigned long)taglen;
    if (n_len == 0)
        return 0;
    if (n_len > 128)
        n_len = 128;
    interned = intern(d, tag, n_len, &tid);
    if (!interned)
        return 0;
    n = node_new(d, DOM_ELEMENT);
    if (!n)
        return 0;
    n->tag = interned;
    n->tag_id = tid;
    return n;
}

static struct dom_node *make_chardata(struct dom_document *d, int type,
                                      const char *s, unsigned long n)
{
    struct dom_node *node;

    if (!d)
        return 0;
    if (n > DOM_MAX_TEXT) {
        n = DOM_MAX_TEXT;
        d->truncated |= DOM_TRUNC_TEXT;
    }
    node = node_new(d, type);
    if (!node)
        return 0;
    node->text = arena_str(d, s ? s : "", n);
    if (!node->text) {
        /* Out of arena. The node still has to be safe to read, so it
         * gets the document's own always-empty string rather than a
         * literal or a null. */
        node->text = ((struct dom_priv *)d->priv)->empty;
        node->text_len = 0;
        return 0;
    }
    node->text_len = n;
    return node;
}

struct dom_node *dom_create_text(struct dom_document *d,
                                 const char *s, unsigned long n)
{
    return make_chardata(d, DOM_TEXT, s, n);
}

struct dom_node *dom_create_comment(struct dom_document *d,
                                    const char *s, unsigned long n)
{
    return make_chardata(d, DOM_COMMENT, s, n);
}

struct dom_node *dom_create_doctype(struct dom_document *d,
                                    const char *s, unsigned long n)
{
    return make_chardata(d, DOM_DOCTYPE, s, n);
}

/* ------------------------------------------------------------------ *
 * Tree editing
 * ------------------------------------------------------------------ */

/* Would inserting `child` under `parent` create a cycle? */
static int is_ancestor(const struct dom_node *maybe_anc,
                       const struct dom_node *n, unsigned long cap)
{
    unsigned long guard = 0;

    while (n) {
        if (n == maybe_anc)
            return 1;
        if (++guard > cap + 4)
            return 1;               /* corrupt chain: refuse the move */
        n = n->parent;
    }
    return 0;
}

/* Re-stamp depth over a subtree, iteratively. Returns 0 if the subtree
 * would exceed the depth cap. */
static int restamp_depth(struct dom_node *root, unsigned int base,
                         unsigned long cap)
{
    struct dom_node *n = root;

    if (base > cap)
        return 0;
    root->depth = base;
    for (;;) {
        if (n->first_child) {
            if (n->depth + 1 > cap)
                return 0;
            n->first_child->depth = n->depth + 1;
            n = n->first_child;
            continue;
        }
        while (n != root && !n->next_sibling)
            n = n->parent;
        if (n == root)
            break;
        n->next_sibling->depth = n->depth;
        n = n->next_sibling;
    }
    return 1;
}

static int link_ok(struct dom_node *parent, struct dom_node *child)
{
    unsigned long cap;

    if (!parent || !child || parent == child)
        return 0;
    if (child->type == DOM_DOCUMENT)
        return 0;
    if (parent->type != DOM_DOCUMENT && parent->type != DOM_ELEMENT)
        return 0;
    cap = dom_limit_depth(parent->doc);
    if (is_ancestor(child, parent, cap))
        return 0;
    if (!restamp_depth(child, parent->depth + 1, cap)) {
        parent->doc->truncated |= DOM_TRUNC_DEPTH;
        return 0;
    }
    return 1;
}

static void unlink_node(struct dom_node *child)
{
    struct dom_node *p = child->parent;

    if (!p)
        return;
    if (child->prev_sibling)
        child->prev_sibling->next_sibling = child->next_sibling;
    else
        p->first_child = child->next_sibling;
    if (child->next_sibling)
        child->next_sibling->prev_sibling = child->prev_sibling;
    else
        p->last_child = child->prev_sibling;
    child->parent = 0;
    child->prev_sibling = 0;
    child->next_sibling = 0;
}

int dom_append_child(struct dom_node *parent, struct dom_node *child)
{
    if (!link_ok(parent, child))
        return 0;
    unlink_node(child);
    child->parent = parent;
    child->prev_sibling = parent->last_child;
    child->next_sibling = 0;
    if (parent->last_child)
        parent->last_child->next_sibling = child;
    else
        parent->first_child = child;
    parent->last_child = child;
    return 1;
}

int dom_insert_before(struct dom_node *parent, struct dom_node *child,
                      struct dom_node *ref)
{
    if (!ref)
        return dom_append_child(parent, child);
    if (ref->parent != parent)
        return 0;
    if (!link_ok(parent, child))
        return 0;
    unlink_node(child);
    child->parent = parent;
    child->prev_sibling = ref->prev_sibling;
    child->next_sibling = ref;
    if (ref->prev_sibling)
        ref->prev_sibling->next_sibling = child;
    else
        parent->first_child = child;
    ref->prev_sibling = child;
    return 1;
}

void dom_remove_child(struct dom_node *parent, struct dom_node *child)
{
    if (!parent || !child || child->parent != parent)
        return;
    unlink_node(child);
}

/* ------------------------------------------------------------------ *
 * Attributes
 * ------------------------------------------------------------------ */

const char *dom_get_attr_n(const struct dom_node *n,
                           const char *name, long namelen)
{
    unsigned long len;
    unsigned int i;

    if (!n || !name || n->type != DOM_ELEMENT)
        return 0;
    len = (namelen < 0) ? strlen(name) : (unsigned long)namelen;
    for (i = 0; i < n->nattr; i++) {
        const char *an = n->attr[i].name;
        if (strlen(an) == len && d_ieq(an, name, len))
            return n->attr[i].value;
    }
    return 0;
}

const char *dom_get_attr(const struct dom_node *n, const char *name)
{
    return dom_get_attr_n(n, name, -1);
}

int dom_has_attr(const struct dom_node *n, const char *name)
{
    return dom_get_attr_n(n, name, -1) != 0;
}

unsigned int dom_attr_count(const struct dom_node *n)
{
    return (n && n->type == DOM_ELEMENT) ? n->nattr : 0;
}

const struct dom_attr *dom_attr_at(const struct dom_node *n, unsigned int i)
{
    if (!n || n->type != DOM_ELEMENT || i >= n->nattr)
        return 0;
    return &n->attr[i];
}

int dom_set_attr_n(struct dom_node *n, const char *name, long namelen,
                   const char *value, unsigned long vallen)
{
    struct dom_document *d;
    unsigned long nlen;
    const char *iname;
    char *val;
    unsigned int i;

    if (!n || n->type != DOM_ELEMENT || !name)
        return 0;
    d = n->doc;
    nlen = (namelen < 0) ? strlen(name) : (unsigned long)namelen;
    if (nlen == 0)
        return 0;
    if (nlen > 128)
        nlen = 128;
    if (!value) {
        value = "";
        vallen = 0;
    }
    iname = intern(d, name, nlen, 0);
    if (!iname)
        return 0;
    val = arena_str(d, value, vallen);
    if (!val)
        return 0;

    for (i = 0; i < n->nattr; i++) {
        if (n->attr[i].name == iname) {
            n->attr[i].value = val;
            n->attr[i].len = vallen;
            if (iname[0] == 'i' && iname[1] == 'd' && iname[2] == 0)
                id_insert(d, val, n);
            return 1;
        }
    }
    if (n->nattr >= DOM_MAX_ATTRS) {
        d->truncated |= DOM_TRUNC_ATTRS;
        return 0;
    }
    if (n->nattr == n->cattr) {
        unsigned int ncap = n->cattr ? (unsigned int)(n->cattr * 2) : 4;
        struct dom_attr *na;
        if (ncap > DOM_MAX_ATTRS)
            ncap = DOM_MAX_ATTRS;
        na = (struct dom_attr *)arena_alloc(d, (unsigned long)ncap *
                                            sizeof *na);
        if (!na)
            return 0;
        if (n->nattr)
            memcpy(na, n->attr, (unsigned long)n->nattr * sizeof *na);
        n->attr = na;
        n->cattr = (unsigned short)ncap;
    }
    n->attr[n->nattr].name = iname;
    n->attr[n->nattr].value = val;
    n->attr[n->nattr].len = vallen;
    n->nattr++;
    if (iname[0] == 'i' && iname[1] == 'd' && iname[2] == 0)
        id_insert(d, val, n);
    return 1;
}

int dom_set_attr(struct dom_node *n, const char *name, const char *value)
{
    return dom_set_attr_n(n, name, -1, value, value ? strlen(value) : 0);
}

int dom_remove_attr(struct dom_node *n, const char *name)
{
    unsigned long len;
    unsigned int i;

    if (!n || n->type != DOM_ELEMENT || !name)
        return 0;
    len = strlen(name);
    for (i = 0; i < n->nattr; i++) {
        const char *an = n->attr[i].name;
        if (strlen(an) == len && d_ieq(an, name, len)) {
            unsigned int j;
            for (j = i; j + 1 < n->nattr; j++)
                n->attr[j] = n->attr[j + 1];
            n->nattr--;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * Traversal
 * ------------------------------------------------------------------ */

struct dom_node *dom_next_within(const struct dom_node *n,
                                 const struct dom_node *root)
{
    if (!n)
        return 0;
    if (n->first_child)
        return n->first_child;
    while (n && n != root) {
        if (n->next_sibling)
            return n->next_sibling;
        n = n->parent;
    }
    return 0;
}

struct dom_node *dom_next(const struct dom_node *n)
{
    return dom_next_within(n, 0);
}

struct dom_node *dom_next_skip(const struct dom_node *n,
                               const struct dom_node *root)
{
    while (n && n != root) {
        if (n->next_sibling)
            return n->next_sibling;
        n = n->parent;
    }
    return 0;
}

struct dom_node *dom_first_element_child(const struct dom_node *n)
{
    struct dom_node *c;

    if (!n)
        return 0;
    for (c = n->first_child; c; c = c->next_sibling) {
        if (c->type == DOM_ELEMENT)
            return c;
    }
    return 0;
}

struct dom_node *dom_next_element_sibling(const struct dom_node *n)
{
    struct dom_node *c;

    if (!n)
        return 0;
    for (c = n->next_sibling; c; c = c->next_sibling) {
        if (c->type == DOM_ELEMENT)
            return c;
    }
    return 0;
}

struct dom_node *dom_parent_element(const struct dom_node *n)
{
    if (!n || !n->parent || n->parent->type != DOM_ELEMENT)
        return 0;
    return n->parent;
}

unsigned int dom_element_index(const struct dom_node *n)
{
    struct dom_node *c;
    unsigned int i = 0;

    if (!n || !n->parent)
        return 0;
    for (c = n->parent->first_child; c; c = c->next_sibling) {
        if (c->type != DOM_ELEMENT)
            continue;
        i++;
        if (c == n)
            return i;
    }
    return 0;
}

unsigned int dom_element_child_count(const struct dom_node *n)
{
    struct dom_node *c;
    unsigned int i = 0;

    if (!n)
        return 0;
    for (c = n->first_child; c; c = c->next_sibling) {
        if (c->type == DOM_ELEMENT)
            i++;
    }
    return i;
}

/* ------------------------------------------------------------------ *
 * Queries
 * ------------------------------------------------------------------ */

int dom_tag_is(const struct dom_node *n, const char *tag)
{
    unsigned long len;

    if (!n || n->type != DOM_ELEMENT || !tag)
        return 0;
    len = strlen(tag);
    return strlen(n->tag) == len && d_ieq(n->tag, tag, len);
}

struct dom_node *dom_find_tag(struct dom_node *root, const char *tag)
{
    struct dom_node *n;

    if (!root || !tag)
        return 0;
    for (n = root; n; n = dom_next_within(n, root)) {
        if (n->type == DOM_ELEMENT && dom_tag_is(n, tag))
            return n;
    }
    return 0;
}

int dom_has_class_n(const struct dom_node *n, const char *cls, long clslen)
{
    const char *v;
    unsigned long len, i;

    if (!n || !cls)
        return 0;
    len = (clslen < 0) ? strlen(cls) : (unsigned long)clslen;
    if (len == 0)
        return 0;
    v = dom_get_attr_n(n, "class", 5);
    if (!v)
        return 0;
    i = 0;
    for (;;) {
        unsigned long start;
        while (v[i] && d_space((unsigned char)v[i]))
            i++;
        if (!v[i])
            return 0;
        start = i;
        while (v[i] && !d_space((unsigned char)v[i]))
            i++;
        if (i - start == len && memcmp(v + start, cls, len) == 0)
            return 1;
    }
}

int dom_has_class(const struct dom_node *n, const char *cls)
{
    return dom_has_class_n(n, cls, -1);
}

unsigned long dom_text_content_into(const struct dom_node *n,
                                    char *buf, unsigned long cap)
{
    const struct dom_node *c;
    unsigned long w = 0;

    if (!n) {
        if (buf && cap)
            buf[0] = 0;
        return 0;
    }
    for (c = n; c; c = dom_next_within(c, n)) {
        if (c->type != DOM_TEXT)
            continue;
        if (buf && cap) {
            unsigned long room = (w < cap - 1) ? (cap - 1 - w) : 0;
            unsigned long take = (c->text_len < room) ? c->text_len : room;
            if (take)
                memcpy(buf + w, c->text, take);
        }
        w += c->text_len;
    }
    if (buf && cap)
        buf[(w < cap) ? w : cap - 1] = 0;
    return w;
}

char *dom_text_content(const struct dom_node *n, unsigned long *out_len)
{
    unsigned long need = dom_text_content_into(n, 0, 0);
    char *buf = (char *)malloc(need + 1);

    if (!buf)
        return 0;
    dom_text_content_into(n, buf, need + 1);
    if (out_len)
        *out_len = need;
    return buf;
}

/* ------------------------------------------------------------------ *
 * Collected style / script text and the title
 * ------------------------------------------------------------------ */

static int collect(struct dom_document *d, char **buf, unsigned long *len,
                   unsigned long *cap, const char *s, unsigned long n)
{
    if (*len >= DOM_MAX_COLLECT) {
        d->truncated |= DOM_TRUNC_COLLECT;
        return 0;
    }
    if (*len + n > DOM_MAX_COLLECT) {
        n = DOM_MAX_COLLECT - *len;
        d->truncated |= DOM_TRUNC_COLLECT;
    }
    if (*len + n + 2 > *cap) {
        unsigned long ncap = *cap ? *cap : 1024;
        char *nb;
        while (ncap < *len + n + 2)
            ncap *= 2;
        nb = (char *)realloc(*buf, ncap);
        if (!nb) {
            d->oom = 1;
            return 0;
        }
        *buf = nb;
        *cap = ncap;
    }
    if (n)
        memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[(*len)++] = '\n';
    (*buf)[*len] = 0;
    return 1;
}

int dom_collect_style(struct dom_document *d, const char *s, unsigned long n)
{
    struct dom_priv *pv = (struct dom_priv *)d->priv;
    return collect(d, &d->style_text, &d->style_len, &pv->style_cap, s, n);
}

int dom_collect_script(struct dom_document *d, const char *s, unsigned long n)
{
    struct dom_priv *pv = (struct dom_priv *)d->priv;
    return collect(d, &d->script_text, &d->script_len, &pv->script_cap, s, n);
}

int dom_set_title(struct dom_document *d, const char *s, unsigned long n)
{
    char *t;

    if (!d)
        return 0;
    if (d->title && d->title[0])
        return 1;                   /* the first <title> wins */
    if (n > 4096)
        n = 4096;
    t = arena_str(d, s, n);
    if (!t)
        return 0;
    d->title = t;
    return 1;
}

/* ------------------------------------------------------------------ *
 * Serialization
 * ------------------------------------------------------------------ */

struct sbuf {
    char *p;
    unsigned long len, cap;
    int fail;
};

static void sb_need(struct sbuf *b, unsigned long extra)
{
    unsigned long ncap;
    char *np;

    if (b->fail)
        return;
    if (b->len + extra + 1 <= b->cap)
        return;
    ncap = b->cap ? b->cap : 1024;
    while (ncap < b->len + extra + 1) {
        if (ncap > (~0UL) / 2) {
            b->fail = 1;
            return;
        }
        ncap *= 2;
    }
    np = (char *)realloc(b->p, ncap);
    if (!np) {
        b->fail = 1;
        return;
    }
    b->p = np;
    b->cap = ncap;
}

static void sb_put(struct sbuf *b, const char *s, unsigned long n)
{
    sb_need(b, n);
    if (b->fail)
        return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_puts(struct sbuf *b, const char *s)
{
    sb_put(b, s, strlen(s));
}

static void sb_esc(struct sbuf *b, const char *s, unsigned long n, int attr)
{
    unsigned long i, run = 0;

    for (i = 0; i < n; i++) {
        const char *rep = 0;
        char c = s[i];
        if (c == '&')
            rep = "&amp;";
        else if (!attr && c == '<')
            rep = "&lt;";
        else if (!attr && c == '>')
            rep = "&gt;";
        else if (attr && c == '"')
            rep = "&quot;";
        if (!rep) {
            run++;
            continue;
        }
        if (run)
            sb_put(b, s + i - run, run);
        run = 0;
        sb_puts(b, rep);
    }
    if (run)
        sb_put(b, s + n - run, run);
}

/* Elements whose text children are emitted verbatim. */
static int raw_parent(const struct dom_node *n)
{
    return n && n->type == DOM_ELEMENT &&
           (dom_tag_flags(n->tag_id) & DTF_RAWTEXT) != 0;
}

/* Elements that swallow a leading newline when re-parsed. */
static int eats_newline(const struct dom_node *n)
{
    return n && n->type == DOM_ELEMENT &&
           (n->tag_id == HTAG_PRE || n->tag_id == HTAG_TEXTAREA ||
            n->tag_id == HTAG_LISTING);
}

static void ser_open(struct sbuf *b, const struct dom_node *n)
{
    unsigned int i;

    switch (n->type) {
    case DOM_TEXT:
        if (raw_parent(n->parent))
            sb_put(b, n->text, n->text_len);
        else
            sb_esc(b, n->text, n->text_len, 0);
        return;
    case DOM_COMMENT:
        sb_puts(b, "<!--");
        sb_put(b, n->text, n->text_len);
        sb_puts(b, "-->");
        return;
    case DOM_DOCTYPE:
        sb_puts(b, "<!DOCTYPE ");
        sb_put(b, n->text, n->text_len);
        sb_puts(b, ">");
        return;
    case DOM_ELEMENT:
        break;
    default:
        return;
    }

    sb_puts(b, "<");
    sb_puts(b, n->tag);
    for (i = 0; i < n->nattr; i++) {
        sb_puts(b, " ");
        sb_puts(b, n->attr[i].name);
        sb_puts(b, "=\"");
        sb_esc(b, n->attr[i].value, n->attr[i].len, 1);
        sb_puts(b, "\"");
    }
    sb_puts(b, ">");
    /* <pre>\nfoo re-parses as "foo"; keep the round trip honest by
     * writing the newline the re-parse will eat. */
    if (eats_newline(n) && n->first_child &&
        n->first_child->type == DOM_TEXT && n->first_child->text_len &&
        n->first_child->text[0] == '\n')
        sb_puts(b, "\n");
}

static void ser_close(struct sbuf *b, const struct dom_node *n)
{
    if (n->type != DOM_ELEMENT)
        return;
    if (dom_tag_is_void(n->tag_id))
        return;
    sb_puts(b, "</");
    sb_puts(b, n->tag);
    sb_puts(b, ">");
}

char *dom_serialize(const struct dom_node *root, unsigned long *out_len)
{
    struct sbuf b;
    const struct dom_node **stack;
    unsigned long sp = 0, scap;
    const struct dom_node *n;
    int doc_boundary;

    if (out_len)
        *out_len = 0;
    if (!root)
        return 0;
    scap = (root->doc ? dom_limit_depth(root->doc) : DOM_MAX_DEPTH) + 8;

    b.p = 0;
    b.len = 0;
    b.cap = 0;
    b.fail = 0;
    sb_need(&b, 256);
    if (b.fail)
        return 0;
    b.p[0] = 0;

    stack = (const struct dom_node **)malloc(scap * sizeof *stack);
    if (!stack) {
        free(b.p);
        return 0;
    }

    doc_boundary = (root->type == DOM_DOCUMENT);
    n = doc_boundary ? root->first_child : root;

    while (n && !b.fail) {
        int descend;

        ser_open(&b, n);
        descend = (n->type == DOM_ELEMENT) && n->first_child &&
                  !dom_tag_is_void(n->tag_id);
        if (descend) {
            if (sp >= scap) {
                /* Cannot happen while the depth cap holds, but a
                 * hand-built tree could get here; stop descending
                 * rather than smash the stack. */
                ser_close(&b, n);
            } else {
                stack[sp++] = n;
                n = n->first_child;
                continue;
            }
        } else {
            ser_close(&b, n);
        }
        while (!n->next_sibling && sp > 0) {
            n = stack[--sp];
            ser_close(&b, n);
        }
        if (n->next_sibling && (sp > 0 || doc_boundary))
            n = n->next_sibling;
        else
            n = 0;
    }

    free(stack);
    if (b.fail) {
        free(b.p);
        return 0;
    }
    if (out_len)
        *out_len = b.len;
    return b.p;
}
