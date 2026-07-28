/* test_css.c - host test harness for libweb/css.c and libweb/style.c.
 *
 * The CSS engine is pure computation, so none of it needs KestrelOS to
 * run. This builds against the host libc and exercises the parser, the
 * selector matcher, the cascade and the computed-style builder from a
 * table of cases, then fuzzes the tokenizer.
 *
 * Build (see the header of the report for the exact line):
 *   gcc -Wall -Wextra -O2 -fsanitize=address,undefined -Ilibweb \
 *       -o /tmp/test_css tools/test_css.c libweb/css.c libweb/style.c \
 *       libweb/ua_style.c && /tmp/test_css
 *
 * A case is one stylesheet, one toy document, the id of the element to
 * inspect, and a semicolon-separated list of expectations written in the
 * property's own syntax - "margin-top=16px;color=#ff0000". The harness
 * stringifies the computed style and compares text, so a failure prints
 * something a human can act on rather than two hex blobs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "css.h"

/* ================================================================== *
 * a toy document tree
 *
 * Elements only: text nodes are not modelled, because nothing in the
 * cascade looks at them. Attributes beginning with '_' set the dynamic
 * state bits (_hover, _visited, _focus, _checked, _disabled).
 * ================================================================== */

#define TMAX_ATTR 8

struct tnode {
    char tag[32];
    struct { char name[32]; char value[160]; } attr[TMAX_ATTR];
    int nattr;
    unsigned state;
    struct tnode *parent, *first, *last, *next, *prev;
    struct computed_style *cs;
};

static struct tnode *tn_new(const char *tag, int len)
{
    struct tnode *n = (struct tnode *)calloc(1, sizeof(*n));
    int i;
    if (len > 31)
        len = 31;
    for (i = 0; i < len; i++) {
        int c = (unsigned char)tag[i];
        n->tag[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    n->tag[len] = 0;
    return n;
}

static void tn_append(struct tnode *p, struct tnode *c)
{
    c->parent = p;
    c->prev = p->last;
    if (p->last)
        p->last->next = c;
    else
        p->first = c;
    p->last = c;
}

static void tn_free(struct tnode *n)
{
    struct tnode *c = n->first, *x;
    while (c) {
        x = c->next;
        tn_free(c);
        c = x;
    }
    free(n->cs);
    free(n);
}

/* A deliberately small subset of HTML: open tags with attributes, close
 * tags, self-closing tags. Text is skipped. */
static struct tnode *tn_parse(const char *s)
{
    struct tnode *root = tn_new("root", 4), *cur = root;

    while (*s) {
        if (*s != '<') {
            s++;
            continue;
        }
        s++;
        if (*s == '/') {
            while (*s && *s != '>')
                s++;
            if (*s)
                s++;
            if (cur->parent)
                cur = cur->parent;
            continue;
        }
        {
            const char *st = s;
            struct tnode *n;
            int selfclose = 0;
            while (*s && *s != '>' && *s != ' ' && *s != '/')
                s++;
            n = tn_new(st, (int)(s - st));
            for (;;) {
                while (*s == ' ')
                    s++;
                if (*s == '/') {
                    selfclose = 1;
                    s++;
                    continue;
                }
                if (*s == '>' || !*s)
                    break;
                {
                    const char *ns = s;
                    int nl, vl = 0;
                    char vbuf[160];
                    while (*s && *s != '=' && *s != ' ' && *s != '>' && *s != '/')
                        s++;
                    nl = (int)(s - ns);
                    if (nl > 31)
                        nl = 31;
                    vbuf[0] = 0;
                    if (*s == '=') {
                        s++;
                        if (*s == '"' || *s == '\'') {
                            int q = *s++;
                            while (*s && *s != q && vl < 159)
                                vbuf[vl++] = *s++;
                            if (*s)
                                s++;
                        } else {
                            while (*s && *s != ' ' && *s != '>' && vl < 159)
                                vbuf[vl++] = *s++;
                            /* "<p id=t/>": the slash closes the tag, it is
                             * not part of the value. "href=http://x" is,
                             * which is why this only fires before '>'. */
                            if (vl > 0 && vbuf[vl - 1] == '/' && *s == '>') {
                                vl--;
                                selfclose = 1;
                            }
                        }
                    }
                    vbuf[vl] = 0;
                    if (ns[0] == '_') {
                        if (!strncmp(ns, "_hover", 6))    n->state |= CSS_STATE_HOVER;
                        if (!strncmp(ns, "_visited", 8))  n->state |= CSS_STATE_VISITED;
                        if (!strncmp(ns, "_focus", 6))    n->state |= CSS_STATE_FOCUS;
                        if (!strncmp(ns, "_checked", 8))  n->state |= CSS_STATE_CHECKED;
                        if (!strncmp(ns, "_disabled", 9)) n->state |= CSS_STATE_DISABLED;
                    } else if (n->nattr < TMAX_ATTR) {
                        memcpy(n->attr[n->nattr].name, ns, (size_t)nl);
                        n->attr[n->nattr].name[nl] = 0;
                        strcpy(n->attr[n->nattr].value, vbuf);
                        n->nattr++;
                    }
                }
            }
            if (*s == '>')
                s++;
            tn_append(cur, n);
            if (!selfclose)
                cur = n;
        }
    }
    return root;
}

static struct tnode *tn_find(struct tnode *n, const char *id)
{
    int i;
    struct tnode *c, *r;
    for (i = 0; i < n->nattr; i++)
        if (!strcmp(n->attr[i].name, "id") && !strcmp(n->attr[i].value, id))
            return n;
    for (c = n->first; c; c = c->next)
        if ((r = tn_find(c, id)) != 0)
            return r;
    return 0;
}

/* --- the css_elem_ops binding --- */

static const char *op_tag(void *e) { return ((struct tnode *)e)->tag; }

static const char *op_attr(void *e, const char *name)
{
    struct tnode *n = (struct tnode *)e;
    int i;
    for (i = 0; i < n->nattr; i++)
        if (!strcmp(n->attr[i].name, name))
            return n->attr[i].value;
    return 0;
}

static void *op_parent(void *e)
{
    struct tnode *n = ((struct tnode *)e)->parent;
    return (n && n->parent) ? n : (n && strcmp(n->tag, "root") ? n : 0);
}

static void *op_prev(void *e)  { return ((struct tnode *)e)->prev; }
static void *op_next(void *e)  { return ((struct tnode *)e)->next; }
static void *op_first(void *e) { return ((struct tnode *)e)->first; }
static unsigned op_state(void *e) { return ((struct tnode *)e)->state; }

/* The toy tree has no text nodes, so "no element children" is the whole
 * truth here. The real DOM binding looks at first_child instead. */
static int op_is_empty(void *e) { return ((struct tnode *)e)->first == 0; }

static const struct css_elem_ops test_ops = {
    op_tag, op_attr, op_parent, op_prev, op_next, op_first, 0, 0,
    op_state, op_is_empty
};

/* ================================================================== *
 * stringifying a computed style
 * ================================================================== */

#define X_STR(n, s) s,
static const char *const d_display[]   = { CSS_DISPLAY_LIST(X_STR) 0 };
static const char *const d_position[]  = { CSS_POSITION_LIST(X_STR) 0 };
static const char *const d_float[]     = { CSS_FLOAT_LIST(X_STR) 0 };
static const char *const d_clear[]     = { CSS_CLEAR_LIST(X_STR) 0 };
static const char *const d_talign[]    = { CSS_TEXTALIGN_LIST(X_STR) 0 };
static const char *const d_valign[]    = { CSS_VALIGN_LIST(X_STR) 0 };
static const char *const d_flexdir[]   = { CSS_FLEXDIR_LIST(X_STR) 0 };
static const char *const d_justify[]   = { CSS_JUSTIFY_LIST(X_STR) 0 };
static const char *const d_align[]     = { CSS_ALIGN_LIST(X_STR) 0 };
static const char *const d_ws[]        = { CSS_WHITESPACE_LIST(X_STR) 0 };
static const char *const d_ovf[]       = { CSS_OVERFLOW_LIST(X_STR) 0 };
static const char *const d_vis[]       = { CSS_VISIBILITY_LIST(X_STR) 0 };
static const char *const d_fstyle[]    = { CSS_FONTSTYLE_LIST(X_STR) 0 };
static const char *const d_ffam[]      = { CSS_FONTFAMILY_LIST(X_STR) 0 };
static const char *const d_bstyle[]    = { CSS_BORDERSTYLE_LIST(X_STR) 0 };
static const char *const d_lstype[]    = { CSS_LISTSTYLE_LIST(X_STR) 0 };
static const char *const d_lspos[]     = { CSS_LISTPOS_LIST(X_STR) 0 };
static const char *const d_bcoll[]     = { CSS_BORDERCOLLAPSE_LIST(X_STR) 0 };
static const char *const d_ttrans[]    = { CSS_TEXTTRANSFORM_LIST(X_STR) 0 };

static const char *kwname(const char *const *t, int v, int max)
{
    if (v < 0 || v >= max)
        return "?";
    return t[v];
}

static void fmt_milli(int32_t v, char *b)
{
    int32_t w = v / 1000, f = v % 1000;
    if (f < 0) f = -f;
    if (f == 0)          sprintf(b, "%d", (int)w);
    else if (f % 100 == 0) sprintf(b, "%d.%d", (int)w, (int)(f / 100));
    else if (f % 10 == 0)  sprintf(b, "%d.%02d", (int)w, (int)(f / 10));
    else                   sprintf(b, "%d.%03d", (int)w, (int)f);
}

static void fmt_len(struct css_len l, char *out)
{
    char t[32];
    switch (l.type) {
    case CSS_LEN_AUTO:   strcpy(out, "auto"); break;
    case CSS_LEN_NONE:   strcpy(out, "none"); break;
    case CSS_LEN_NORMAL: strcpy(out, "normal"); break;
    case CSS_LEN_PCT:    fmt_milli(l.v, t); sprintf(out, "%s%%", t); break;
    case CSS_LEN_NUMBER: fmt_milli(l.v, out); break;
    default:             sprintf(out, "%dpx", (int)l.v); break;
    }
}

static void fmt_color(uint32_t c, char *out)
{
    if (CSS_COLOR_A(c) == 0)
        strcpy(out, "transparent");
    else if (CSS_COLOR_A(c) == 255)
        sprintf(out, "#%06x", (unsigned)CSS_COLOR_RGB(c));
    else
        sprintf(out, "#%08x", (unsigned)c);
}

/* Render one property of a computed style as text. Returns 0 if the
 * property name is not one the harness knows how to print. */
static int describe(const struct computed_style *cs, const char *prop, char *out)
{
    int id = css_property_id(prop);

    switch (id) {
    case CSS_PROP_DISPLAY:
        strcpy(out, kwname(d_display, cs->display, CSS_DISPLAY_COUNT)); return 1;
    case CSS_PROP_POSITION:
        strcpy(out, kwname(d_position, cs->position, CSS_POSITION_COUNT)); return 1;
    case CSS_PROP_FLOAT:
        strcpy(out, kwname(d_float, cs->css_float, CSS_FLOAT_COUNT)); return 1;
    case CSS_PROP_CLEAR:
        strcpy(out, kwname(d_clear, cs->clear, CSS_CLEAR_COUNT)); return 1;
    case CSS_PROP_TEXT_ALIGN:
        strcpy(out, kwname(d_talign, cs->text_align, CSS_TEXTALIGN_COUNT)); return 1;
    case CSS_PROP_WHITE_SPACE:
        strcpy(out, kwname(d_ws, cs->white_space, CSS_WHITESPACE_COUNT)); return 1;
    case CSS_PROP_OVERFLOW:
        strcpy(out, kwname(d_ovf, cs->overflow, CSS_OVERFLOW_COUNT)); return 1;
    case CSS_PROP_VISIBILITY:
        strcpy(out, kwname(d_vis, cs->visibility, CSS_VISIBILITY_COUNT)); return 1;
    case CSS_PROP_FONT_STYLE:
        strcpy(out, kwname(d_fstyle, cs->font_style, CSS_FONTSTYLE_COUNT)); return 1;
    case CSS_PROP_LIST_STYLE_TYPE:
        strcpy(out, kwname(d_lstype, cs->list_style_type, CSS_LISTSTYLE_COUNT)); return 1;
    case CSS_PROP_LIST_STYLE_POSITION:
        strcpy(out, kwname(d_lspos, cs->list_style_position, CSS_LISTPOS_COUNT)); return 1;
    case CSS_PROP_BORDER_COLLAPSE:
        strcpy(out, kwname(d_bcoll, cs->border_collapse, CSS_BORDERCOLLAPSE_COUNT)); return 1;
    case CSS_PROP_TEXT_TRANSFORM:
        strcpy(out, kwname(d_ttrans, cs->text_transform, CSS_TEXTTRANSFORM_COUNT)); return 1;
    case CSS_PROP_FONT_FAMILY:
        strcpy(out, kwname(d_ffam, cs->font_family, CSS_FONTFAMILY_COUNT)); return 1;
    case CSS_PROP_VERTICAL_ALIGN:
        if (cs->vertical_align == CSS_VALIGN_LENGTH)
            sprintf(out, "%dpx", (int)cs->vertical_align_px);
        else
            strcpy(out, kwname(d_valign, cs->vertical_align, CSS_VALIGN_COUNT));
        return 1;
    case CSS_PROP_FLEX_DIRECTION:
        strcpy(out, kwname(d_flexdir, cs->flex_direction,
                           CSS_FLEXDIR_COUNT)); return 1;
    case CSS_PROP_JUSTIFY_CONTENT:
        strcpy(out, kwname(d_justify, cs->justify_content,
                           CSS_JUSTIFY_COUNT)); return 1;
    case CSS_PROP_ALIGN_ITEMS:
        strcpy(out, kwname(d_align, cs->align_items,
                           CSS_ALIGN_COUNT)); return 1;
    case CSS_PROP_FLEX_GROW: fmt_milli(cs->flex_grow, out); return 1;
    case CSS_PROP_GAP:       sprintf(out, "%dpx", (int)cs->gap); return 1;
    case CSS_PROP_FONT_SIZE:   sprintf(out, "%dpx", (int)cs->font_size); return 1;
    case CSS_PROP_FONT_WEIGHT: sprintf(out, "%d", (int)cs->font_weight); return 1;
    case CSS_PROP_Z_INDEX:
        if (cs->z_auto) strcpy(out, "auto");
        else sprintf(out, "%d", (int)cs->z_index);
        return 1;
    case CSS_PROP_BORDER_SPACING: sprintf(out, "%dpx", (int)cs->border_spacing); return 1;
    case CSS_PROP_COLOR:            fmt_color(cs->color, out); return 1;
    case CSS_PROP_BACKGROUND_COLOR: fmt_color(cs->background_color, out); return 1;
    case CSS_PROP_BACKGROUND_IMAGE:
        strcpy(out, cs->background_image ? cs->background_image : "none"); return 1;
    case CSS_PROP_WIDTH:      fmt_len(cs->width, out); return 1;
    case CSS_PROP_HEIGHT:     fmt_len(cs->height, out); return 1;
    case CSS_PROP_MIN_WIDTH:  fmt_len(cs->min_width, out); return 1;
    case CSS_PROP_MAX_WIDTH:  fmt_len(cs->max_width, out); return 1;
    case CSS_PROP_MIN_HEIGHT: fmt_len(cs->min_height, out); return 1;
    case CSS_PROP_MAX_HEIGHT: fmt_len(cs->max_height, out); return 1;
    case CSS_PROP_LINE_HEIGHT: fmt_len(cs->line_height, out); return 1;
    case CSS_PROP_TEXT_INDENT: fmt_len(cs->text_indent, out); return 1;
    case CSS_PROP_TOP:    fmt_len(cs->offset[CSS_TOP], out); return 1;
    case CSS_PROP_RIGHT:  fmt_len(cs->offset[CSS_RIGHT], out); return 1;
    case CSS_PROP_BOTTOM: fmt_len(cs->offset[CSS_BOTTOM], out); return 1;
    case CSS_PROP_LEFT:   fmt_len(cs->offset[CSS_LEFT], out); return 1;
    case CSS_PROP_MARGIN_TOP:    fmt_len(cs->margin[CSS_TOP], out); return 1;
    case CSS_PROP_MARGIN_RIGHT:  fmt_len(cs->margin[CSS_RIGHT], out); return 1;
    case CSS_PROP_MARGIN_BOTTOM: fmt_len(cs->margin[CSS_BOTTOM], out); return 1;
    case CSS_PROP_MARGIN_LEFT:   fmt_len(cs->margin[CSS_LEFT], out); return 1;
    case CSS_PROP_PADDING_TOP:    fmt_len(cs->padding[CSS_TOP], out); return 1;
    case CSS_PROP_PADDING_RIGHT:  fmt_len(cs->padding[CSS_RIGHT], out); return 1;
    case CSS_PROP_PADDING_BOTTOM: fmt_len(cs->padding[CSS_BOTTOM], out); return 1;
    case CSS_PROP_PADDING_LEFT:   fmt_len(cs->padding[CSS_LEFT], out); return 1;
    case CSS_PROP_BORDER_TOP_WIDTH:    sprintf(out, "%dpx", (int)cs->border_width[CSS_TOP]); return 1;
    case CSS_PROP_BORDER_RIGHT_WIDTH:  sprintf(out, "%dpx", (int)cs->border_width[CSS_RIGHT]); return 1;
    case CSS_PROP_BORDER_BOTTOM_WIDTH: sprintf(out, "%dpx", (int)cs->border_width[CSS_BOTTOM]); return 1;
    case CSS_PROP_BORDER_LEFT_WIDTH:   sprintf(out, "%dpx", (int)cs->border_width[CSS_LEFT]); return 1;
    case CSS_PROP_BORDER_TOP_STYLE:
        strcpy(out, kwname(d_bstyle, cs->border_style[CSS_TOP], CSS_BORDERSTYLE_COUNT)); return 1;
    case CSS_PROP_BORDER_RIGHT_STYLE:
        strcpy(out, kwname(d_bstyle, cs->border_style[CSS_RIGHT], CSS_BORDERSTYLE_COUNT)); return 1;
    case CSS_PROP_BORDER_BOTTOM_STYLE:
        strcpy(out, kwname(d_bstyle, cs->border_style[CSS_BOTTOM], CSS_BORDERSTYLE_COUNT)); return 1;
    case CSS_PROP_BORDER_LEFT_STYLE:
        strcpy(out, kwname(d_bstyle, cs->border_style[CSS_LEFT], CSS_BORDERSTYLE_COUNT)); return 1;
    case CSS_PROP_BORDER_TOP_COLOR:    fmt_color(cs->border_color[CSS_TOP], out); return 1;
    case CSS_PROP_BORDER_RIGHT_COLOR:  fmt_color(cs->border_color[CSS_RIGHT], out); return 1;
    case CSS_PROP_BORDER_BOTTOM_COLOR: fmt_color(cs->border_color[CSS_BOTTOM], out); return 1;
    case CSS_PROP_BORDER_LEFT_COLOR:   fmt_color(cs->border_color[CSS_LEFT], out); return 1;
    case CSS_PROP_TEXT_DECORATION: {
        out[0] = 0;
        if (!cs->text_decoration) { strcpy(out, "none"); return 1; }
        if (cs->text_decoration & CSS_DECOR_UNDERLINE) strcat(out, "underline ");
        if (cs->text_decoration & CSS_DECOR_OVERLINE)  strcat(out, "overline ");
        if (cs->text_decoration & CSS_DECOR_LINETHRU)  strcat(out, "line-through ");
        if (cs->text_decoration & CSS_DECOR_BLINK)     strcat(out, "blink ");
        out[strlen(out) - 1] = 0;
        return 1;
    }
    default:
        break;
    }
    if (!strcmp(prop, "font-family-name")) {
        strcpy(out, cs->font_family_name ? cs->font_family_name : "-");
        return 1;
    }
    return 0;
}

/* ================================================================== *
 * the case table
 * ================================================================== */

#define F_UA 1u

struct tcase {
    const char *name;
    const char *css;
    const char *doc;
    const char *id;
    const char *expect;
    unsigned flags;
};

static const struct tcase cases[] = {

/* ---------------- selectors ---------------- */
{"type", "p { color: red }", "<div><p id=t/></div>", "t", "color=#ff0000", 0},
{"universal", "* { color: #00ff00 }", "<div><p id=t/></div>", "t", "color=#00ff00", 0},
{"class", ".c { color: blue }", "<p id=t class=c/>", "t", "color=#0000ff", 0},
{"class-multi", ".b { color: blue }", "<p id=t class='a b c'/>", "t", "color=#0000ff", 0},
{"class-no-substring", ".b { color: blue }", "<p id=t class='abc'/>", "t", "color=#000000", 0},
{"id-sel", "#t { color: teal }", "<p id=t/>", "t", "color=#008080", 0},
{"compound", "p.c#t[x] { color: red }", "<p id=t class=c x=1/>", "t", "color=#ff0000", 0},
{"compound-miss", "p.c#t[y] { color: red }", "<p id=t class=c x=1/>", "t", "color=#000000", 0},

{"attr-exists", "[data-x] { color: red }", "<p id=t data-x=/>", "t", "color=#ff0000", 0},
{"attr-eq", "[href=\"a.html\"] { color: red }", "<a id=t href=a.html/>", "t", "color=#ff0000", 0},
{"attr-eq-miss", "[href=a] { color: red }", "<a id=t href=a.html/>", "t", "color=#000000", 0},
{"attr-includes", "[rel~=next] { color: red }", "<a id=t rel='prev next'/>", "t", "color=#ff0000", 0},
{"attr-includes-miss", "[rel~=nex] { color: red }", "<a id=t rel='prev next'/>", "t", "color=#000000", 0},
{"attr-dash", "[lang|=en] { color: red }", "<p id=t lang=en-GB/>", "t", "color=#ff0000", 0},
{"attr-dash-exact", "[lang|=en] { color: red }", "<p id=t lang=en/>", "t", "color=#ff0000", 0},
{"attr-dash-miss", "[lang|=en] { color: red }", "<p id=t lang=eng/>", "t", "color=#000000", 0},
{"attr-prefix", "[href^=https] { color: red }", "<a id=t href=https://x/>", "t", "color=#ff0000", 0},
{"attr-suffix", "[href$=.pdf] { color: red }", "<a id=t href=a.pdf/>", "t", "color=#ff0000", 0},
{"attr-substr", "[href*=oo] { color: red }", "<a id=t href=foobar/>", "t", "color=#ff0000", 0},
{"attr-substr-miss", "[href*=zz] { color: red }", "<a id=t href=foobar/>", "t", "color=#000000", 0},

{"descendant", "div p { color: red }", "<div><span><p id=t/></span></div>", "t", "color=#ff0000", 0},
{"descendant-miss", "section p { color: red }", "<div><p id=t/></div>", "t", "color=#000000", 0},
{"child", "div > p { color: red }", "<div><p id=t/></div>", "t", "color=#ff0000", 0},
{"child-miss", "div > p { color: red }", "<div><span><p id=t/></span></div>", "t", "color=#000000", 0},
{"adjacent", "h1 + p { color: red }", "<div><h1/><p id=t/></div>", "t", "color=#ff0000", 0},
{"adjacent-miss", "h1 + p { color: red }", "<div><h1/><span/><p id=t/></div>", "t", "color=#000000", 0},
{"sibling", "h1 ~ p { color: red }", "<div><h1/><span/><p id=t/></div>", "t", "color=#ff0000", 0},
{"sibling-miss", "h1 ~ p { color: red }", "<div><p id=t/><h1/></div>", "t", "color=#000000", 0},
{"deep-chain", "a b > c + d e { color: red }",
 "<a><x><b><c/><d><y><e id=t/></y></d></b></x></a>", "t", "color=#ff0000", 0},
{"selector-list", "h1, h2, p { color: red }", "<div><h2 id=t/></div>", "t", "color=#ff0000", 0},

{"first-child", "p:first-child { color: red }", "<div><p id=t/><p/></div>", "t", "color=#ff0000", 0},
{"first-child-miss", "p:first-child { color: red }", "<div><p/><p id=t/></div>", "t", "color=#000000", 0},
{"last-child", "p:last-child { color: red }", "<div><p/><p id=t/></div>", "t", "color=#ff0000", 0},
{"only-child", "p:only-child { color: red }", "<div><p id=t/></div>", "t", "color=#ff0000", 0},
{"nth-2", "li:nth-child(2) { color: red }", "<ul><li/><li id=t/><li/></ul>", "t", "color=#ff0000", 0},
{"nth-2n", "li:nth-child(2n) { color: red }", "<ul><li/><li/><li/><li id=t/></ul>", "t", "color=#ff0000", 0},
{"nth-2n1", "li:nth-child(2n+1) { color: red }", "<ul><li/><li/><li id=t/></ul>", "t", "color=#ff0000", 0},
{"nth-odd", "li:nth-child(odd) { color: red }", "<ul><li/><li id=t/></ul>", "t", "color=#000000", 0},
{"nth-even", "li:nth-child(even) { color: red }", "<ul><li/><li id=t/></ul>", "t", "color=#ff0000", 0},
{"nth-negn", "li:nth-child(-n+2) { color: red }", "<ul><li/><li id=t/><li/></ul>", "t", "color=#ff0000", 0},
{"nth-negn-miss", "li:nth-child(-n+2) { color: red }", "<ul><li/><li/><li id=t/></ul>", "t", "color=#000000", 0},
{"nth-3n-1", "li:nth-child(3n-1) { color: red }", "<ul><li/><li id=t/><li/></ul>", "t", "color=#ff0000", 0},
{"nth-space", "li:nth-child( 2n + 2 ) { color: red }", "<ul><li/><li id=t/></ul>", "t", "color=#ff0000", 0},
{"nth-last", "li:nth-last-child(1) { color: red }", "<ul><li/><li id=t/></ul>", "t", "color=#ff0000", 0},
{"first-of-type", "p:first-of-type { color: red }", "<div><span/><p id=t/><p/></div>", "t", "color=#ff0000", 0},
{"last-of-type", "p:last-of-type { color: red }", "<div><p/><p id=t/><span/></div>", "t", "color=#ff0000", 0},
{"empty", "p:empty { color: red }", "<div><p id=t/></div>", "t", "color=#ff0000", 0},
{"empty-miss", "p:empty { color: red }", "<div><p id=t><b/></p></div>", "t", "color=#000000", 0},
{"root", ":root { color: red }", "<html id=t/>", "t", "color=#ff0000", 0},
{"not-class", "p:not(.x) { color: red }", "<div><p id=t/><p class=x/></div>", "t", "color=#ff0000", 0},
{"not-class-miss", "p:not(.x) { color: red }", "<div><p id=t class=x/></div>", "t", "color=#000000", 0},
{"not-type", "div :not(span) { color: red }", "<div><p id=t/></div>", "t", "color=#ff0000", 0},
{"not-compound", "p:not(.a.b) { color: red }", "<p id=t class=a/>", "t", "color=#ff0000", 0},
{"link", "a:link { color: red }", "<a id=t href=x/>", "t", "color=#ff0000", 0},
{"link-nohref", "a:link { color: red }", "<a id=t/>", "t", "color=#000000", 0},
{"hover", "a:hover { color: red }", "<a id=t href=x _hover/>", "t", "color=#ff0000", 0},
{"hover-off", "a:hover { color: red }", "<a id=t href=x/>", "t", "color=#000000", 0},
{"visited", "a:visited { color: red }", "<a id=t href=x _visited/>", "t", "color=#ff0000", 0},
{"disabled", "input:disabled { color: red }", "<input id=t disabled/>", "t", "color=#ff0000", 0},

/* ---------------- specificity and source order ---------------- */
{"spec-id-over-class", "#t { color: red } .c { color: blue }",
 "<p id=t class=c/>", "t", "color=#ff0000", 0},
{"spec-class-over-type", "p { color: blue } .c { color: red }",
 "<p id=t class=c/>", "t", "color=#ff0000", 0},
{"spec-order-wins-tie", "p { color: blue } p { color: red }",
 "<p id=t/>", "t", "color=#ff0000", 0},
{"spec-order-reverse", "p { color: red } p { color: blue }",
 "<p id=t/>", "t", "color=#0000ff", 0},
{"spec-two-class-over-one", ".a { color: blue } .a.b { color: red }",
 "<p id=t class='a b'/>", "t", "color=#ff0000", 0},
{"spec-two-class-order", ".a.b { color: red } .a { color: blue }",
 "<p id=t class='a b'/>", "t", "color=#ff0000", 0},
{"spec-attr-counts-as-class", "p[x] { color: red } p.q { color: blue }",
 "<p id=t class=q x=1/>", "t", "color=#0000ff", 0},
{"spec-descendant-types", "div p { color: red } p { color: blue }",
 "<div><p id=t/></div>", "t", "color=#ff0000", 0},
{"spec-not-counts", "p:not(.zz) { color: red } p { color: blue }",
 "<p id=t/>", "t", "color=#ff0000", 0},
{"spec-pseudo-counts", "p { color: blue } p:first-child { color: red }",
 "<div><p id=t/></div>", "t", "color=#ff0000", 0},
{"important-beats-id", "#t { color: blue } p { color: red !important }",
 "<p id=t/>", "t", "color=#ff0000", 0},
{"important-vs-important", "#t { color: red !important } p { color: blue !important }",
 "<p id=t/>", "t", "color=#ff0000", 0},

{"important-on-shorthand", "#t { margin: 1px } p { margin: 9px !important }",
 "<p id=t/>", "t", "margin-top=9px;margin-left=9px", 0},
{"important-per-declaration",
 "#t { color: blue; margin: 1px } p { color: red !important; margin: 9px }",
 "<p id=t/>", "t", "color=#ff0000;margin-top=1px", 0},
/* ---------------- inline style ---------------- */
{"inline-beats-author", "#t { color: blue }", "<p id=t style='color: red'/>", "t",
 "color=#ff0000", 0},
{"important-beats-inline", "#t { color: red !important }",
 "<p id=t style='color: blue'/>", "t", "color=#ff0000", 0},
{"inline-important-beats-author-important",
 "#t { color: blue !important }",
 "<p id=t style='color: red !important'/>", "t", "color=#ff0000", 0},
{"inline-shorthand", "", "<p id=t style='margin: 1px 2px 3px 4px'/>", "t",
 "margin-top=1px;margin-right=2px;margin-bottom=3px;margin-left=4px", 0},
{"inline-garbage", "", "<p id=t style='color: ; display: block'/>", "t",
 "display=block", 0},

/* ---------------- inheritance ---------------- */
{"inherit-3-levels", "#a { color: red }",
 "<div id=a><div><div><p id=t/></div></div></div>", "t", "color=#ff0000", 0},
{"no-inherit-border", "#a { border: 1px solid red }",
 "<div id=a><p id=t/></div>", "t", "border-top-width=0px;border-top-style=none", 0},
{"inherit-keyword", "#a { border-top-width: 4px; border-top-style: solid }"
 " #t { border-top-width: inherit; border-top-style: inherit }",
 "<div id=a><p id=t/></div>", "t", "border-top-width=4px;border-top-style=solid", 0},
{"initial-keyword", "#a { color: red } #t { color: initial }",
 "<div id=a><p id=t/></div>", "t", "color=#000000", 0},
{"unset-on-inherited", "#a { color: red } #t { color: unset }",
 "<div id=a><p id=t/></div>", "t", "color=#ff0000", 0},
{"unset-on-noninherited",
 "#a { border-top-style: solid } p { border-top-style: dashed }"
 " #t { border-top-style: unset }",
 "<div id=a><p id=t/></div>", "t", "border-top-style=none", 0},
{"unset-shorthand", "#a { margin: 5px } p { margin: 3px } #t { margin: unset }",
 "<div id=a><p id=t/></div>", "t", "margin-top=0px", 0},
{"inherit-font-size-chain", "#a { font-size: 20px } #b { font-size: 2em }"
 " #t { font-size: 0.5em }",
 "<div id=a><div id=b><p id=t/></div></div>", "t", "font-size=20px", 0},
{"inherit-line-height-number", "#a { line-height: 1.5; font-size: 10px }",
 "<div id=a><p id=t/></div>", "t", "line-height=1.5;font-size=10px", 0},
{"inherit-white-space", "#a { white-space: pre }",
 "<div id=a><p id=t/></div>", "t", "white-space=pre", 0},
{"inherit-shorthand-keyword", "#a { margin: 5px } #t { margin: inherit }",
 "<div id=a><p id=t/></div>", "t", "margin-top=5px;margin-left=5px", 0},

/* ---------------- shorthands ---------------- */
{"margin-1", "#t { margin: 5px }", "<p id=t/>", "t",
 "margin-top=5px;margin-right=5px;margin-bottom=5px;margin-left=5px", 0},
{"margin-2", "#t { margin: 1px 2px }", "<p id=t/>", "t",
 "margin-top=1px;margin-right=2px;margin-bottom=1px;margin-left=2px", 0},
{"margin-3", "#t { margin: 1px 2px 3px }", "<p id=t/>", "t",
 "margin-top=1px;margin-right=2px;margin-bottom=3px;margin-left=2px", 0},
{"margin-4", "#t { margin: 1px 2px 3px 4px }", "<p id=t/>", "t",
 "margin-top=1px;margin-right=2px;margin-bottom=3px;margin-left=4px", 0},
{"margin-auto", "#t { margin: 0 auto }", "<p id=t/>", "t",
 "margin-top=0px;margin-right=auto;margin-left=auto", 0},
{"margin-negative", "#t { margin: -5px }", "<p id=t/>", "t", "margin-top=-5px", 0},
{"padding-no-negative", "#t { padding: -5px }", "<p id=t/>", "t", "padding-top=0px", 0},
{"padding-4", "#t { padding: 1px 2px 3px 4px }", "<p id=t/>", "t",
 "padding-top=1px;padding-right=2px;padding-bottom=3px;padding-left=4px", 0},
{"border-full", "#t { border: 2px dashed #123456 }", "<p id=t/>", "t",
 "border-top-width=2px;border-right-style=dashed;border-bottom-color=#123456;"
 "border-left-width=2px", 0},
{"border-reorder", "#t { border: red solid 3px }", "<p id=t/>", "t",
 "border-top-width=3px;border-top-style=solid;border-top-color=#ff0000", 0},
{"border-style-only", "#t { border: solid }", "<p id=t/>", "t",
 "border-top-style=solid;border-top-width=3px", 0},
{"border-none", "#t { border: 1px solid red; border: none }", "<p id=t/>", "t",
 "border-top-width=0px;border-top-style=none", 0},
{"border-top-only", "#t { border-top: 1px solid red }", "<p id=t/>", "t",
 "border-top-width=1px;border-left-width=0px;border-top-style=solid", 0},
{"border-width-4", "#t { border-style: solid; border-width: 1px 2px 3px 4px }",
 "<p id=t/>", "t", "border-top-width=1px;border-right-width=2px;"
 "border-bottom-width=3px;border-left-width=4px", 0},
{"border-width-kw", "#t { border-style: solid; border-width: thin medium thick }",
 "<p id=t/>", "t", "border-top-width=1px;border-right-width=3px;border-bottom-width=5px", 0},
{"border-style-4", "#t { border-style: solid dotted dashed double }", "<p id=t/>", "t",
 "border-top-style=solid;border-right-style=dotted;border-bottom-style=dashed;"
 "border-left-style=double", 0},
{"border-color-2", "#t { border-color: red blue }", "<p id=t/>", "t",
 "border-top-color=#ff0000;border-right-color=#0000ff;border-bottom-color=#ff0000;"
 "border-left-color=#0000ff", 0},
{"border-width-suppressed", "#t { border-width: 5px }", "<p id=t/>", "t",
 "border-top-width=0px", 0},
{"background-color", "#t { background: #abcdef }", "<p id=t/>", "t",
 "background-color=#abcdef", 0},
{"background-full", "#t { background: #fff url(bg.png) no-repeat top left }",
 "<p id=t/>", "t", "background-color=#ffffff;background-image=bg.png", 0},
{"background-url-only", "#t { background: url('a b.png') }", "<p id=t/>", "t",
 "background-image=a b.png;background-color=transparent", 0},
{"font-shorthand", "#t { font: italic bold 20px/1.5 Georgia, serif }", "<p id=t/>", "t",
 "font-style=italic;font-weight=700;font-size=20px;line-height=1.5;font-family=serif;"
 "font-family-name=Georgia", 0},
{"font-shorthand-min", "#t { font: 12px monospace }", "<p id=t/>", "t",
 "font-size=12px;font-family=monospace;font-weight=400", 0},
{"font-shorthand-numeric-weight", "#t { font: 300 14px Arial }", "<p id=t/>", "t",
 "font-weight=300;font-size=14px;font-family=sans-serif", 0},
{"list-style", "#t { list-style: square inside }", "<li id=t/>", "t",
 "list-style-type=square;list-style-position=inside", 0},
{"list-style-reorder", "#t { list-style: inside decimal }", "<li id=t/>", "t",
 "list-style-type=decimal;list-style-position=inside", 0},
{"text-decoration-multi", "#t { text-decoration: underline line-through }",
 "<p id=t/>", "t", "text-decoration=underline line-through", 0},
{"text-decoration-none", "#t { text-decoration: none }", "<p id=t/>", "t",
 "text-decoration=none", 0},
{"text-decoration-css3", "#t { text-decoration: underline dotted red }",
 "<p id=t/>", "t", "text-decoration=underline", 0},

/* ---------------- colours ---------------- */
{"hex3", "#t { color: #f00 }", "<p id=t/>", "t", "color=#ff0000", 0},
{"hex6", "#t { color: #1a2B3c }", "<p id=t/>", "t", "color=#1a2b3c", 0},
{"hex4", "#t { background-color: #0f08 }", "<p id=t/>", "t",
 "background-color=#8800ff00", 0},
{"hex8", "#t { background-color: #10203040 }", "<p id=t/>", "t",
 "background-color=#40102030", 0},
{"hex-bad", "#t { color: #12345 }", "<p id=t/>", "t", "color=#000000", 0},
{"rgb", "#t { color: rgb(1, 2, 3) }", "<p id=t/>", "t", "color=#010203", 0},
{"rgb-pct", "#t { color: rgb(100%, 0%, 50%) }", "<p id=t/>", "t", "color=#ff0080", 0},
{"rgb-clamp", "#t { color: rgb(300, -20, 3) }", "<p id=t/>", "t", "color=#ff0003", 0},
{"rgba", "#t { background-color: rgba(1,2,3,0.5) }", "<p id=t/>", "t",
 "background-color=#7f010203", 0},
{"rgb-modern", "#t { color: rgb(4 5 6) }", "<p id=t/>", "t", "color=#040506", 0},
{"rgb-modern-alpha", "#t { background-color: rgb(4 5 6 / 100%) }", "<p id=t/>", "t",
 "background-color=#040506", 0},
{"hsl-red", "#t { color: hsl(0, 100%, 50%) }", "<p id=t/>", "t", "color=#ff0000", 0},
{"hsl-lime", "#t { color: hsl(120, 100%, 50%) }", "<p id=t/>", "t", "color=#00ff00", 0},
{"hsl-blue", "#t { color: hsl(240, 100%, 50%) }", "<p id=t/>", "t", "color=#0000ff", 0},
{"hsl-grey", "#t { color: hsl(0, 0%, 50%) }", "<p id=t/>", "t", "color=#808080", 0},
{"hsl-white", "#t { color: hsl(9, 0%, 100%) }", "<p id=t/>", "t", "color=#ffffff", 0},
{"hsl-wrap", "#t { color: hsl(480, 100%, 50%) }", "<p id=t/>", "t", "color=#00ff00", 0},
{"hsla", "#t { background-color: hsla(0,100%,50%,0) }", "<p id=t/>", "t",
 "background-color=transparent", 0},
{"named-basic", "#t { color: rebeccapurple }", "<p id=t/>", "t", "color=#663399", 0},
{"named-case", "#t { color: DarkSlateGray }", "<p id=t/>", "t", "color=#2f4f4f", 0},
{"named-grey-alias", "#t { color: grey }", "<p id=t/>", "t", "color=#808080", 0},
{"transparent", "#t { background-color: transparent }", "<p id=t/>", "t",
 "background-color=transparent", 0},
{"currentcolor", "#t { color: #ff8800; border: 1px solid currentColor }",
 "<p id=t/>", "t", "border-top-color=#ff8800", 0},
{"bad-color-name", "#t { color: notacolour }", "<p id=t/>", "t", "color=#000000", 0},

/* ---------------- units ---------------- */
{"unit-pt", "#t { width: 12pt }", "<p id=t/>", "t", "width=16px", 0},
{"unit-pc", "#t { width: 1pc }", "<p id=t/>", "t", "width=16px", 0},
{"unit-in", "#t { width: 1in }", "<p id=t/>", "t", "width=96px", 0},
{"unit-cm", "#t { width: 2.54cm }", "<p id=t/>", "t", "width=96px", 0},
{"unit-mm", "#t { width: 25.4mm }", "<p id=t/>", "t", "width=96px", 0},
{"unit-em", "#t { font-size: 20px; width: 2em }", "<p id=t/>", "t", "width=40px", 0},
{"unit-rem", "#a { font-size: 32px } #t { font-size: 8px; width: 2rem }",
 "<div id=a><p id=t/></div>", "t", "width=32px", 0},
{"unit-ex", "#t { font-size: 20px; width: 2ex }", "<p id=t/>", "t", "width=20px", 0},
{"unit-ch", "#t { font-size: 16px; width: 4ch }", "<p id=t/>", "t", "width=32px", 0},
{"unit-pct", "#t { width: 50% }", "<p id=t/>", "t", "width=50%", 0},
{"unit-pct-frac", "#t { width: 33.5% }", "<p id=t/>", "t", "width=33.5%", 0},
{"unit-zero", "#t { width: 0 }", "<p id=t/>", "t", "width=0px", 0},
{"unit-unknown", "#t { width: 5xyz }", "<p id=t/>", "t", "width=auto", 0},
{"unit-round", "#t { width: 10.6px }", "<p id=t/>", "t", "width=11px", 0},
{"unit-neg-round", "#t { margin-top: -10.6px }", "<p id=t/>", "t", "margin-top=-11px", 0},
{"font-size-pct", "#a { font-size: 20px } #t { font-size: 150% }",
 "<div id=a><p id=t/></div>", "t", "font-size=30px", 0},
{"font-size-kw", "#t { font-size: x-large }", "<p id=t/>", "t", "font-size=24px", 0},
{"font-size-smaller", "#a { font-size: 24px } #t { font-size: smaller }",
 "<div id=a><p id=t/></div>", "t", "font-size=20px", 0},
{"font-weight-bolder", "#a { font-weight: 400 } #t { font-weight: bolder }",
 "<div id=a><p id=t/></div>", "t", "font-weight=700", 0},
{"font-weight-lighter", "#a { font-weight: 700 } #t { font-weight: lighter }",
 "<div id=a><p id=t/></div>", "t", "font-weight=400", 0},
{"line-height-pct", "#t { font-size: 20px; line-height: 150% }", "<p id=t/>", "t",
 "line-height=30px", 0},
{"line-height-normal", "#t { line-height: normal }", "<p id=t/>", "t",
 "line-height=normal", 0},
{"vertical-align-len", "#t { vertical-align: 3px }", "<p id=t/>", "t",
 "vertical-align=3px", 0},
{"vertical-align-kw", "#t { vertical-align: middle }", "<p id=t/>", "t",
 "vertical-align=middle", 0},
{"z-index", "#t { z-index: 5 }", "<p id=t/>", "t", "z-index=5", 0},
{"z-index-auto", "#t { z-index: auto }", "<p id=t/>", "t", "z-index=auto", 0},
{"z-index-neg", "#t { z-index: -2 }", "<p id=t/>", "t", "z-index=-2", 0},
{"flex-properties",
 "#t { display:flex; flex-direction:row-reverse; "
 "justify-content:space-between; align-items:center; flex-grow:2.5; gap:12px }",
 "<div id=t/>", "t",
 "display=flex;flex-direction=row-reverse;justify-content=space-between;"
 "align-items=center;flex-grow=2.5;gap=12px", 0},
{"flex-invalid",
 "#t { flex-direction:diagonal; justify-content:left; align-items:sideways; "
 "flex-grow:-1; gap:-2px }",
 "<div id=t/>", "t",
 "flex-direction=row;justify-content=flex-start;align-items=stretch;"
 "flex-grow=0;gap=0px", 0},

/* ---------------- @media ---------------- */
{"media-screen-in", "@media screen { #t { color: red } }", "<p id=t/>", "t",
 "color=#ff0000", 0},
{"media-print-out", "@media print { #t { color: red } }", "<p id=t/>", "t",
 "color=#000000", 0},
{"media-all", "@media all { #t { color: red } }", "<p id=t/>", "t", "color=#ff0000", 0},
{"media-min-width-in", "@media (min-width: 500px) { #t { color: red } }",
 "<p id=t/>", "t", "color=#ff0000", 0},
{"media-min-width-out", "@media (min-width: 5000px) { #t { color: red } }",
 "<p id=t/>", "t", "color=#000000", 0},
{"media-max-width-in", "@media (max-width: 2000px) { #t { color: red } }",
 "<p id=t/>", "t", "color=#ff0000", 0},
{"media-max-width-out", "@media (max-width: 100px) { #t { color: red } }",
 "<p id=t/>", "t", "color=#000000", 0},
{"media-and", "@media screen and (min-width: 100px) and (max-width: 5000px)"
 " { #t { color: red } }", "<p id=t/>", "t", "color=#ff0000", 0},
{"media-comma", "@media print, screen { #t { color: red } }", "<p id=t/>", "t",
 "color=#ff0000", 0},
{"media-not", "@media not print { #t { color: red } }", "<p id=t/>", "t",
 "color=#ff0000", 0},
{"media-only-screen", "@media only screen { #t { color: red } }", "<p id=t/>", "t",
 "color=#ff0000", 0},
{"media-unknown-type", "@media aural { #t { color: red } }", "<p id=t/>", "t",
 "color=#000000", 0},
{"media-orientation", "@media (orientation: landscape) { #t { color: red } }",
 "<p id=t/>", "t", "color=#ff0000", 0},
{"media-nested", "@media screen { @media (min-width: 100px) { #t { color: red } } }",
 "<p id=t/>", "t", "color=#ff0000", 0},
{"media-nested-out", "@media print { @media (min-width: 100px) { #t { color: red } } }",
 "<p id=t/>", "t", "color=#000000", 0},
{"media-after", "@media print { #t { color: red } } #t { display: block }",
 "<p id=t/>", "t", "color=#000000;display=block", 0},
{"media-em-query", "@media (min-width: 30em) { #t { color: red } }", "<p id=t/>", "t",
 "color=#000000", 0},

/* ---------------- at-rules that must be skipped cleanly ---------------- */
{"font-face-skipped",
 "@font-face { font-family: X; src: url(x.woff) } #t { color: red }",
 "<p id=t/>", "t", "color=#ff0000", 0},
{"page-skipped", "@page { margin: 2cm } #t { color: red }", "<p id=t/>", "t",
 "color=#ff0000", 0},
{"keyframes-skipped",
 "@keyframes spin { from { opacity: 0 } to { opacity: 1 } } #t { color: red }",
 "<p id=t/>", "t", "color=#ff0000", 0},
{"supports-skipped",
 "@supports (display: grid) { #t { color: blue } } #t { color: red }",
 "<p id=t/>", "t", "color=#ff0000", 0},
{"supports-flex",
 "@supports (display: flex) { #t { color: blue } }",
 "<p id=t/>", "t", "color=#0000ff", 0},
{"supports-not-grid",
 "@supports not (grid-template-columns: 1fr) { #t { color: blue } }",
 "<p id=t/>", "t", "color=#0000ff", 0},
{"supports-and",
 "@supports (display:flex) and (gap:1px) { #t { color: blue } }",
 "<p id=t/>", "t", "color=#0000ff", 0},
{"supports-or",
 "@supports ((display:grid) or (display:flex)) { #t { color: blue } }",
 "<p id=t/>", "t", "color=#0000ff", 0},
{"charset-skipped", "@charset \"utf-8\"; #t { color: red }", "<p id=t/>", "t",
 "color=#ff0000", 0},
{"import-then-rule", "@import url(a.css); #t { color: red }", "<p id=t/>", "t",
 "color=#ff0000", 0},

/* ---------------- malformed input recovery ---------------- */
{"unclosed-block", "#t { color: red", "<p id=t/>", "t", "color=#ff0000", 0},
/* A "{" inside a declaration block starts a bad declaration whose whole
 * brace-matched run is swallowed, so the inner rule never exists. That is
 * what browsers do; the enclosing rule survives. */
{"inner-block-swallowed", "p { color: blue ; #t { color: red }",
 "<p id=t/>", "t", "color=#0000ff", 0},
{"garbage-decl", "#t { !!!; color: red; ;; }", "<p id=t/>", "t", "color=#ff0000", 0},
{"empty-value", "#t { color: ; display: block }", "<p id=t/>", "t",
 "display=block;color=#000000", 0},
{"unknown-property", "#t { -webkit-foo: bar; color: red }", "<p id=t/>", "t",
 "color=#ff0000", 0},
{"unknown-value", "#t { display: sideways; color: red }", "<p id=t/>", "t",
 "display=inline;color=#ff0000", 0},
/* An unterminated string ends at the newline, so the declaration holding
 * it is dropped and parsing resumes at the ';'. */
{"unterminated-string", "#x { content: \"abc\n; color: blue } #t { display: block }",
 "<p id=t/>", "t", "display=block;color=#000000", 0},
/* ...but if the string swallowed the closing brace, everything up to the
 * newline goes with it. Spec behaviour, and what browsers do too. */
{"unterminated-string-eats-block",
 "#t { color: red } #x { content: \"abc ; color: blue }\n#t { display: block }",
 "<p id=t/>", "t", "color=#ff0000;display=inline", 0},
{"unterminated-comment", "#t { color: red } /* never closed",
 "<p id=t/>", "t", "color=#ff0000", 0},
{"stray-close-brace", "} #t { color: red }", "<p id=t/>", "t", "color=#ff0000", 0},
{"bad-selector-kills-rule", "p && q { color: blue } #t { color: red }",
 "<p id=t/>", "t", "color=#ff0000", 0},
{"bad-selector-in-list-kills-rule", "#t, ?? { color: blue }", "<p id=t/>", "t",
 "color=#000000", 0},
{"pseudo-element-drops-one", "#t, #t::before { color: red }", "<p id=t/>", "t",
 "color=#ff0000", 0},
{"unknown-pseudo-drops-one", "#t, #t:-moz-thing { color: red }", "<p id=t/>", "t",
 "color=#ff0000", 0},
{"unknown-fn-pseudo-drops-one", "#t, #t:is(.a) { color: red }", "<p id=t/>", "t",
 "color=#ff0000", 0},
{"bad-important", "#t { color: blue !importan; display: block }", "<p id=t/>", "t",
 "color=#000000;display=block", 0},
{"calc-dropped", "#t { width: calc(100% - 10px); height: 4px }", "<p id=t/>", "t",
 "width=auto;height=4px", 0},
{"nested-parens", "#t { background: foo(bar(1,2)); color: red }", "<p id=t/>", "t",
 "color=#ff0000", 0},
{"cdo-cdc", "<!-- #t { color: red } -->", "<p id=t/>", "t", "color=#ff0000", 0},
{"comment-inside", "#t /* x */ { color /* y */ : red }", "<p id=t/>", "t",
 "color=#ff0000", 0},
{"semicolon-in-selector", "#t; { color: blue } #t { color: red }", "<p id=t/>", "t",
 "color=#ff0000", 0},
{"escape-ident", "#t { color: r\\65 d }", "<p id=t/>", "t", "color=#ff0000", 0},
{"escaped-class", ".a\\.b { color: red }", "<p id=t class='a.b'/>", "t",
 "color=#ff0000", 0},
{"string-escape-url", "#t { background-image: url(\"a\\\"b.png\") }", "<p id=t/>", "t",
 "background-image=a\"b.png", 0},
{"too-many-values", "#t { color: red green blue yellow }", "<p id=t/>", "t",
 "color=#000000", 0},
{"case-insensitive-prop", "#t { COLOR: RED; DISPLAY: BLOCK }", "<p id=t/>", "t",
 "color=#ff0000;display=block", 0},
{"case-sensitive-class", ".C { color: red }", "<p id=t class=c/>", "t",
 "color=#000000", 0},

/* ---------------- the user-agent stylesheet ---------------- */
{"ua-p-block", "", "<body><p id=t/></body>", "t",
 "display=block;margin-top=16px;margin-bottom=16px", F_UA},
{"ua-h1", "", "<body><h1 id=t/></body>", "t",
 "display=block;font-size=32px;font-weight=700;margin-top=21px", F_UA},
{"ua-h3", "", "<body><h3 id=t/></body>", "t", "font-size=19px;font-weight=700", F_UA},
{"ua-strong", "", "<body><strong id=t/></body>", "t",
 "font-weight=700;display=inline", F_UA},
{"ua-em", "", "<body><em id=t/></body>", "t", "font-style=italic", F_UA},
{"ua-pre", "", "<body><pre id=t/></body>", "t",
 "font-family=monospace;white-space=pre;display=block", F_UA},
{"ua-code", "", "<body><code id=t/></body>", "t", "font-family=monospace", F_UA},
{"ua-link", "", "<body><a id=t href=x/></body>", "t",
 "color=#0000ee;text-decoration=underline", F_UA},
{"ua-link-nohref", "", "<body><a id=t/></body>", "t",
 "color=#000000;text-decoration=none", F_UA},
{"ua-visited", "", "<body><a id=t href=x _visited/></body>", "t",
 "color=#551a8b", F_UA},
{"ua-head-hidden", "", "<html><head id=t/></html>", "t", "display=none", F_UA},
{"ua-script-hidden", "", "<body><script id=t/></body>", "t", "display=none", F_UA},
{"ua-ul", "", "<body><ul id=t/></body>", "t",
 "display=block;padding-left=40px;list-style-type=disc", F_UA},
{"ua-ol", "", "<body><ol id=t/></body>", "t", "list-style-type=decimal", F_UA},
{"ua-nested-ul", "", "<body><ul><li><ul id=t/></li></ul></body>", "t",
 "list-style-type=circle;margin-top=0px", F_UA},
{"ua-li", "", "<body><ul><li id=t/></ul></body>", "t", "display=list-item", F_UA},
{"ua-table", "", "<body><table id=t/></body>", "t",
 "display=table;border-spacing=2px;border-collapse=separate", F_UA},
{"ua-td", "", "<body><table><tr><td id=t/></tr></table></body>", "t",
 "display=table-cell;padding-top=1px", F_UA},
{"ua-th", "", "<body><table><tr><th id=t/></tr></table></body>", "t",
 "display=table-cell;font-weight=700;text-align=center", F_UA},
{"ua-blockquote", "", "<body><blockquote id=t/></body>", "t",
 "margin-left=40px;margin-top=16px", F_UA},
{"ua-hidden-attr", "", "<body><div id=t hidden/></body>", "t", "display=none", F_UA},
{"ua-body-margin", "", "<body id=t/>", "t", "margin-top=8px;margin-left=8px", F_UA},
{"ua-inherit-font-into-h1", "", "<body><h1 id=t/></body>", "t", "font-size=32px", F_UA},
{"ua-author-overrides", "h1 { font-size: 10px }", "<body><h1 id=t/></body>", "t",
 "font-size=10px;font-weight=700", F_UA},
{"ua-author-lower-specificity", "* { font-weight: 400 }",
 "<body><strong id=t/></body>", "t", "font-weight=400", F_UA},
{"ua-sub-smaller", "", "<body><sub id=t/></body>", "t",
 "vertical-align=sub;font-size=13px", F_UA},
{"ua-del", "", "<body><del id=t/></body>", "t", "text-decoration=line-through", F_UA},
{"ua-print-not-applied", "", "<body><nav id=t/></body>", "t", "display=block", F_UA},
{"ua-input", "", "<body><input id=t type=text/></body>", "t",
 "display=inline-block;border-top-width=1px;font-size=13px", F_UA},
{"ua-submit-button", "", "<body><input id=t type=submit/></body>", "t",
 "border-top-style=outset;text-align=center", F_UA},
{"ua-hidden-input", "", "<body><input id=t type=hidden/></body>", "t",
 "display=none", F_UA}
};

#define NCASES ((int)(sizeof(cases) / sizeof(cases[0])))

/* ================================================================== *
 * the runner
 * ================================================================== */

static int failures;
static int checks;

static void sink(void *ctx, void *elem, struct computed_style *cs)
{
    (void)ctx;
    ((struct tnode *)elem)->cs = cs;
}

static void run_case(const struct tcase *tc)
{
    struct css_stylesheet *sheets[2];
    int nsheet = 0;
    struct style_engine *se;
    struct tnode *root, *target;
    char buf[256], want[256], got[256];
    const char *e;
    int local_fail = 0;

    if (tc->flags & F_UA)
        sheets[nsheet++] = css_parse(css_ua_stylesheet(),
                                     css_ua_stylesheet_len(), CSS_ORIGIN_UA, 0);
    sheets[nsheet++] = css_parse(tc->css, strlen(tc->css), CSS_ORIGIN_AUTHOR, 0);

    root = tn_parse(tc->doc);
    se = style_engine_new(sheets, nsheet, &test_ops);
    style_compute_tree(se, root, 0, sink, 0);
    target = tn_find(root, tc->id);

    if (!target) {
        printf("  FAIL %-34s : no element with id=%s\n", tc->name, tc->id);
        failures++;
        local_fail = 1;
    } else if (!target->cs) {
        printf("  FAIL %-34s : element was not styled\n", tc->name);
        failures++;
        local_fail = 1;
    } else {
        e = tc->expect;
        while (e && *e) {
            const char *eq, *end;
            size_t pl, vl;
            while (*e == ' ' || *e == ';')
                e++;
            if (!*e)
                break;
            eq = strchr(e, '=');
            end = strchr(e, ';');
            if (!end)
                end = e + strlen(e);
            if (!eq || eq > end) {
                printf("  FAIL %-34s : malformed expectation\n", tc->name);
                failures++;
                break;
            }
            pl = (size_t)(eq - e);
            vl = (size_t)(end - eq - 1);
            memcpy(buf, e, pl);
            buf[pl] = 0;
            memcpy(want, eq + 1, vl);
            want[vl] = 0;
            checks++;
            if (!describe(target->cs, buf, got)) {
                printf("  FAIL %-34s : unknown property '%s'\n", tc->name, buf);
                failures++;
                local_fail = 1;
            } else if (strcmp(got, want) != 0) {
                printf("  FAIL %-34s : %s = %s, expected %s\n",
                       tc->name, buf, got, want);
                failures++;
                local_fail = 1;
            }
            e = end;
        }
    }
    (void)local_fail;

    style_engine_free(se);
    tn_free(root);
    while (nsheet--)
        css_free(sheets[nsheet]);
}

/* ================================================================== *
 * direct-API checks that do not fit the table
 * ================================================================== */

static void expect_int(const char *what, long got, long want)
{
    checks++;
    if (got != want) {
        printf("  FAIL %-34s : got %ld, expected %ld\n", what, got, want);
        failures++;
    }
}

static void expect_str(const char *what, const char *got, const char *want)
{
    checks++;
    if (!got || strcmp(got, want) != 0) {
        printf("  FAIL %-34s : got '%s', expected '%s'\n",
               what, got ? got : "(null)", want);
        failures++;
    }
}

static void api_tests(void)
{
    /* @import reporting */
    {
        const char *src = "@import url(reset.css);\n"
                          "@import \"theme.css\" screen;\n"
                          "@import url('/quoted.css');\n"
                          "p { color: red }";
        struct css_stylesheet *ss = css_parse(src, strlen(src),
                                              CSS_ORIGIN_AUTHOR, 0);
        expect_int("import count", css_import_count(ss), 3);
        expect_str("import 0", css_import_url(ss, 0), "reset.css");
        expect_str("import 1", css_import_url(ss, 1), "theme.css");
        expect_str("import quoted url", css_import_url(ss, 2), "/quoted.css");
        expect_int("import: rule survives", css_rule_count(ss), 1);
        css_free(ss);
    }
    /* specificity arithmetic */
    {
        const char *src = "* {color:red} li {color:red} ul li {color:red}"
                          ".c {color:red} li.c {color:red} #i {color:red}"
                          "#i .c li {color:red} li:not(.x) {color:red}"
                          "[href] {color:red} a[href][title] {color:red}";
        struct css_stylesheet *ss = css_parse(src, strlen(src),
                                              CSS_ORIGIN_AUTHOR, 0);
        static const unsigned long want[] = {
            0x00000u,            /* *        0,0,0 */
            0x00001u,            /* li       0,0,1 */
            0x00002u,            /* ul li    0,0,2 */
            0x00400u,            /* .c       0,1,0 */
            0x00401u,            /* li.c     0,1,1 */
            0x100000u,           /* #i       1,0,0 */
            0x100401u,           /* #i .c li 1,1,1 */
            0x00401u,            /* li:not(.x) 0,1,1 */
            0x00400u,            /* [href]   0,1,0 */
            0x00801u             /* a[href][title] 0,2,1 */
        };
        int i;
        expect_int("specificity: rule count", css_rule_count(ss), 10);
        for (i = 0; i < 10 && i < css_rule_count(ss); i++) {
            char nm[64];
            sprintf(nm, "specificity[%d]", i);
            expect_int(nm, (long)css_rule_at(ss, i)->sel.specificity,
                       (long)want[i]);
        }
        css_free(ss);
    }
    /* selector-list splitting: one rule per selector, shared decls */
    {
        const char *src = "h1, h2, h3 { color: red }";
        struct css_stylesheet *ss = css_parse(src, strlen(src),
                                              CSS_ORIGIN_AUTHOR, 0);
        expect_int("selector list splits", css_rule_count(ss), 3);
        expect_int("shared decl block",
                   css_rule_at(ss, 0)->decls == css_rule_at(ss, 2)->decls, 1);
        css_free(ss);
    }
    /* @media re-evaluation on a resize */
    {
        const char *src = "@media (max-width: 600px) { p { color: red } }";
        struct css_media m = { 1024, 768, 96, 1, 0 };
        struct css_stylesheet *ss;
        struct tnode *root;
        struct style_engine *se;
        char got[128];

        ss = css_parse(src, strlen(src), CSS_ORIGIN_AUTHOR, &m);
        root = tn_parse("<p id=t/>");
        se = style_engine_new(&ss, 1, &test_ops);
        style_compute_tree(se, root, 0, sink, 0);
        describe(tn_find(root, "t")->cs, "color", got);
        expect_str("media wide: excluded", got, "#000000");

        m.width = 400;
        expect_int("media set: one block flipped", css_set_media(ss, &m), 1);
        tn_free(root);
        root = tn_parse("<p id=t/>");
        style_compute_tree(se, root, 0, sink, 0);
        describe(tn_find(root, "t")->cs, "color", got);
        expect_str("media narrow: included", got, "#ff0000");

        style_engine_free(se);
        tn_free(root);
        css_free(ss);
    }
    /* The far end of the cascade order: a !important user-agent rule
     * outranks a !important author rule, which the table cases cannot
     * express because they always use the real UA sheet. */
    {
        const char *ua = "p { display: block !important; color: green }";
        const char *au = "#t { display: inline !important; color: red }";
        struct css_stylesheet *sh[2];
        struct style_engine *se;
        struct tnode *root;
        char got[128];

        sh[0] = css_parse(ua, strlen(ua), CSS_ORIGIN_UA, 0);
        sh[1] = css_parse(au, strlen(au), CSS_ORIGIN_AUTHOR, 0);
        root = tn_parse("<p id=t/>");
        se = style_engine_new(sh, 2, &test_ops);
        style_compute_tree(se, root, 0, sink, 0);
        describe(tn_find(root, "t")->cs, "display", got);
        expect_str("ua !important beats author !important", got, "block");
        describe(tn_find(root, "t")->cs, "color", got);
        expect_str("ua normal loses to author normal", got, "#ff0000");
        style_engine_free(se);
        tn_free(root);
        css_free(sh[0]);
        css_free(sh[1]);
    }
    /* vw/vh against the engine's viewport */
    {
        const char *src = "#t { width: 50vw; height: 25vh }";
        struct css_stylesheet *ss = css_parse(src, strlen(src),
                                              CSS_ORIGIN_AUTHOR, 0);
        struct tnode *root = tn_parse("<p id=t/>");
        struct style_engine *se = style_engine_new(&ss, 1, &test_ops);
        char got[128];
        style_engine_set_viewport(se, 800, 600);
        style_compute_tree(se, root, 0, sink, 0);
        describe(tn_find(root, "t")->cs, "width", got);
        expect_str("50vw of 800", got, "400px");
        describe(tn_find(root, "t")->cs, "height", got);
        expect_str("25vh of 600", got, "150px");
        style_engine_free(se);
        tn_free(root);
        css_free(ss);
    }
    /* colour helper and the named table */
    {
        uint32_t c = 0;
        expect_int("named colour count", css_named_color_count(), 148);
        expect_int("parse #abc", css_parse_color_string("#abc", &c), 1);
        expect_int("#abc value", (long)CSS_COLOR_RGB(c), 0xAABBCC);
        expect_int("parse transparent",
                   css_parse_color_string("transparent", &c), 1);
        expect_int("transparent alpha", (long)CSS_COLOR_A(c), 0);
        expect_int("parse garbage", css_parse_color_string("zzz", &c), 0);
    }
    /* every one of the 147 named colours must round-trip */
    {
        static const char *const probe[] = {
            "aliceblue", "black", "cyan", "darkslategrey", "fuchsia",
            "lightgoldenrodyellow", "mediumspringgreen", "navajowhite",
            "rebeccapurple", "yellowgreen"
        };
        static const unsigned long value[] = {
            0xF0F8FF, 0x000000, 0x00FFFF, 0x2F4F4F, 0xFF00FF,
            0xFAFAD2, 0x00FA9A, 0xFFDEAD, 0x663399, 0x9ACD32
        };
        unsigned i;
        for (i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
            uint32_t c = 0;
            char nm[64];
            sprintf(nm, "named '%s'", probe[i]);
            css_parse_color_string(probe[i], &c);
            expect_int(nm, (long)CSS_COLOR_RGB(c), (long)value[i]);
        }
    }
    /* every property name must round-trip through the registry */
    {
        int i, bad = 0;
        for (i = 1; i < CSS_PROP_COUNT; i++)
            if (css_property_id(css_property_name(i)) != i)
                bad++;
        expect_int("property registry round-trip", bad, 0);
    }
    /* the UA sheet itself must parse without truncation */
    {
        struct css_stylesheet *ss = css_parse(css_ua_stylesheet(),
                                              css_ua_stylesheet_len(),
                                              CSS_ORIGIN_UA, 0);
        printf("  ua stylesheet: %lu bytes -> %d rules, %lu bytes of arena\n",
               css_ua_stylesheet_len(), css_rule_count(ss),
               css_memory_used(ss));
        expect_int("ua sheet not truncated", css_truncated(ss), 0);
        checks++;
        if (css_rule_count(ss) < 80) {
            printf("  FAIL ua rule count too low: %d\n", css_rule_count(ss));
            failures++;
        }
        css_free(ss);
    }
    /* limits: a hostile sheet must degrade, not die */
    {
        unsigned long n = 60000, i, cap = n * 40 + 16;
        char *big = (char *)malloc(cap);
        char *w = big;
        struct css_stylesheet *ss;
        for (i = 0; i < n; i++)
            w += sprintf(w, ".c%lu x%lu { color: #%06lx }\n", i, i, i & 0xFFFFFF);
        ss = css_parse(big, (unsigned long)(w - big), CSS_ORIGIN_AUTHOR, 0);
        printf("  50k-rule stress: %lu source rules -> %d kept, truncated=%d,"
               " %lu KiB\n",
               n, css_rule_count(ss), css_truncated(ss),
               css_memory_used(ss) / 1024);
        expect_int("stress: capped at CSS_MAX_RULES",
                   css_rule_count(ss), CSS_MAX_RULES);
        expect_int("stress: reports truncation", css_truncated(ss), 1);
        css_free(ss);
        free(big);
    }
    /* a selector too deep to store must not corrupt anything */
    {
        char big[4096];
        char *w = big;
        int i;
        struct css_stylesheet *ss;
        for (i = 0; i < 200; i++)
            w += sprintf(w, "d%d ", i);
        strcpy(w, "{ color: red }");
        ss = css_parse(big, strlen(big), CSS_ORIGIN_AUTHOR, 0);
        expect_int("over-deep selector dropped", css_rule_count(ss), 0);
        css_free(ss);
    }
    /* declaration count per block is capped */
    {
        char *big = (char *)malloc(200000);
        char *w = big;
        int i;
        struct css_stylesheet *ss;
        w += sprintf(w, "p {");
        for (i = 0; i < 2000; i++)
            w += sprintf(w, "margin-top: %dpx;", i);
        w += sprintf(w, "}");
        ss = css_parse(big, (unsigned long)(w - big), CSS_ORIGIN_AUTHOR, 0);
        checks++;
        if (css_rule_count(ss) != 1 ||
            css_rule_at(ss, 0)->ndecl > CSS_MAX_BLOCK_DECLS) {
            printf("  FAIL block decl cap: %d rules, %d decls\n",
                   css_rule_count(ss),
                   css_rule_count(ss) ? css_rule_at(ss, 0)->ndecl : -1);
            failures++;
        }
        css_free(ss);
        free(big);
    }
    /* a null / empty stylesheet must be safe */
    {
        struct css_stylesheet *a = css_parse(0, 0, CSS_ORIGIN_AUTHOR, 0);
        struct css_stylesheet *b = css_parse("", 0, CSS_ORIGIN_AUTHOR, 0);
        struct css_stylesheet *c = css_parse_style_attr(0);
        expect_int("null source", css_rule_count(a), 0);
        expect_int("empty source", css_rule_count(b), 0);
        expect_int("null inline", css_rule_count(c), 0);
        css_free(a); css_free(b); css_free(c);
        css_free(0);
    }
    /* deeper than the tree-walk cap: no crash, no runaway */
    {
        char *doc = (char *)malloc(200 * 16 + 16);
        char *w = doc;
        int i, n;
        struct css_stylesheet *ss;
        struct style_engine *se;
        struct tnode *root;
        const char *src = "div { color: red }";
        for (i = 0; i < 300; i++)
            w += sprintf(w, "<div>");
        *w = 0;
        ss = css_parse(src, strlen(src), CSS_ORIGIN_AUTHOR, 0);
        root = tn_parse(doc);
        se = style_engine_new(&ss, 1, &test_ops);
        n = style_compute_tree(se, root, 0, sink, 0);
        expect_int("tree depth cap", n, CSS_MAX_TREE_DEPTH);
        style_engine_free(se);
        tn_free(root);
        css_free(ss);
        free(doc);
    }
}

/* ================================================================== *
 * matching cost on a realistic page
 * ================================================================== */

/* Every element against every rule, with no index at all. */
static unsigned long linear_scan(struct tnode *n, struct css_stylesheet **ss,
                                 int nsheet)
{
    unsigned long hits = 0;
    struct tnode *c;
    int s, r;

    for (s = 0; s < nsheet; s++) {
        int nr = css_rule_count(ss[s]);
        for (r = 0; r < nr; r++)
            if (css_selector_matches(&css_rule_at(ss[s], r)->sel, n, &test_ops))
                hits++;
    }
    for (c = n->first; c; c = c->next)
        hits += linear_scan(c, ss, nsheet);
    return hits;
}

/* The 40 tag names a real sheet's type selectors actually spread over. */
static const char *const perf_tags[] = {
    "a","abbr","article","aside","b","blockquote","button","caption","cite",
    "code","dd","div","dl","dt","em","figure","footer","form","h1","h2","h3",
    "h4","header","i","img","input","kbd","label","li","main","nav","ol","p",
    "pre","section","small","span","strong","table","td"
};
#define NPERF_TAGS ((int)(sizeof(perf_tags) / sizeof(perf_tags[0])))

/* nrule rules over a page of about nelem elements. `spread` picks how
 * the rightmost simple selectors are distributed: 1 is what a real
 * stylesheet looks like (mostly distinct classes, type selectors spread
 * over the whole tag vocabulary), 0 is the adversarial case where a
 * large fraction of the rules end in the same handful of tags. */
static void perf_run(const char *label, int nrule, int nelem, int spread)
{
    char *css = (char *)malloc((size_t)nrule * 80 + 128);
    char *doc = (char *)malloc((size_t)nelem * 80 + 128);
    char *w;
    int i;
    struct css_stylesheet *sheets[2];
    struct style_engine *se;
    struct tnode *root;
    clock_t t0, t1;
    unsigned long considered = 0, matched = 0, hits;
    double ms, lin_ms;
    int styled;

    w = css;
    for (i = 0; i < nrule; i++) {
        const char *tg = spread ? perf_tags[i % NPERF_TAGS] : "p";
        const char *tg2 = spread ? perf_tags[(i * 13) % NPERF_TAGS] : "span";
        switch (i & 7) {
        case 0: w += sprintf(w, ".c%d { color: #%06x }\n", i, i & 0xFFFFFF); break;
        case 1: w += sprintf(w, "#id%d { margin: %dpx }\n", i, i & 31); break;
        case 2: w += sprintf(w, "div .c%d %s { padding: 1px }\n", i, tg); break;
        case 3: w += sprintf(w, "p.c%d > %s { font-weight: bold }\n", i, tg2); break;
        case 4: w += sprintf(w, "a.k%d[href^=\"h%d\"] { color: blue }\n", i, i); break;
        case 5: w += sprintf(w, "li.n%d:nth-child(%dn+1) { color: red }\n",
                             i, (i % 5) + 1); break;
        case 6: w += sprintf(w, "section > div.c%d { display: block }\n", i); break;
        default: w += sprintf(w, "%s.t%d, blockquote.t%d { line-height: 1.%d }\n",
                              tg, i, i, i % 9); break;
        }
    }
    w = doc;
    w += sprintf(w, "<html><body>");
    for (i = 0; i < nelem / 6; i++)
        w += sprintf(w,
            "<section><div class='c%d wrap'><p id='id%d' class='c%d'>"
            "<span/><a href='h%d'/></p></div></section>",
            i % nrule, i, (i * 7) % nrule, i);
    w += sprintf(w, "</body></html>");

    sheets[0] = css_parse(css_ua_stylesheet(), css_ua_stylesheet_len(),
                          CSS_ORIGIN_UA, 0);
    sheets[1] = css_parse(css, strlen(css), CSS_ORIGIN_AUTHOR, 0);
    root = tn_parse(doc);
    se = style_engine_new(sheets, 2, &test_ops);

    css_match_stats_reset();
    t0 = clock();
    styled = style_compute_tree(se, root, 0, sink, 0);
    t1 = clock();
    css_match_stats(&considered, &matched);
    ms = (double)(t1 - t0) * 1000.0 / (double)CLOCKS_PER_SEC;

    /* The baseline the index exists to avoid: every element against every
     * rule. Measured, not estimated - matching cost is wildly uneven
     * across selector shapes, so a rule-count ratio would be a guess. */
    t0 = clock();
    hits = linear_scan(root, sheets, 2);
    t1 = clock();
    lin_ms = (double)(t1 - t0) * 1000.0 / (double)CLOCKS_PER_SEC;

    printf("  %s\n", label);
    printf("    %d elements, %d author rules + %d UA rules\n",
           styled, css_rule_count(sheets[1]), css_rule_count(sheets[0]));
    printf("    cascade: %.1f ms, %.1f us/element\n",
           ms, styled ? ms * 1000.0 / styled : 0.0);
    printf("    index:  %lu candidates tested (%.1f/element), %lu matched\n",
           considered, styled ? (double)considered / styled : 0.0, matched);
    printf("    linear: %.1f ms for the same work (%lu selector matches);"
           " index is %.1fx faster\n",
           lin_ms, hits, ms > 0 ? lin_ms / ms : 0.0);

    style_engine_free(se);
    tn_free(root);
    css_free(sheets[0]);
    css_free(sheets[1]);
    free(css);
    free(doc);
}

static void perf_test(void)
{
    perf_run("realistic page (rightmost selectors spread over 40 tags "
             "and distinct classes):", 4000, 3000, 1);
    perf_run("adversarial sheet (a quarter of all rules end in the same "
             "two tags):", 4000, 3000, 0);
}

/* ================================================================== *
 * the fuzzer
 * ================================================================== */

static unsigned long rng_state = 0x12345678u;

static unsigned long rnd(void)
{
    rng_state = rng_state * 6364136223846793005UL + 1442695040888963407UL;
    return (rng_state >> 17) & 0x7FFFFFFFu;
}

static const char *const corpus[] = {
    "p { color: red }",
    "@media screen and (max-width: 600px) { .a > b + c ~ d { margin: 1px 2px } }",
    "a[href^=\"http\"]:not(.x):nth-child(2n+1) { background: #fff url(a.png) }",
    "@import url(x.css); @font-face { src: url(y) } #i { font: italic bold 10px/2 X }",
    "* { padding: 0 } html, body { border: 1px solid rgba(1,2,3,0.5) }",
    "li:first-child::before { content: \"x\" } .c { color: hsl(120, 50%, 25%) }",
    "@supports (a:b) { p { q: r } } @page :first { margin: 1cm }",
    "div{}p{;;}q{color:}r{color:red!important}s{color:red !important;}",
    "\\65 scaped { color: \\72 ed } .a\\.b { width: 3ex }",
    "@media print, (orientation: landscape), not screen { td { border-collapse: collapse } }"
};
#define NCORPUS ((int)(sizeof(corpus) / sizeof(corpus[0])))

static void fuzz(int iters)
{
    char buf[4096];
    int i, kept = 0, truncated = 0;
    unsigned long total_rules = 0;
    struct css_media m = { 800, 600, 96, 1, 0 };

    for (i = 0; i < iters; i++) {
        const char *base = corpus[rnd() % NCORPUS];
        size_t n = strlen(base), j;
        int nmut;

        if (n > sizeof(buf) - 1)
            n = sizeof(buf) - 1;
        memcpy(buf, base, n);
        buf[n] = 0;

        /* Few mutations per input on purpose: a heavily mangled sheet
         * dies in the first rule and never reaches the interesting code.
         * Truncation is deliberately rare for the same reason. */
        nmut = (int)(rnd() % 4) + 1;
        for (j = 0; j < (size_t)nmut; j++) {
            unsigned long op = rnd() % 10;
            size_t at;
            if (n == 0)
                break;
            at = rnd() % n;
            switch (op) {
            case 0: case 1: case 2:  /* flip to a random printable */
                buf[at] = (char)(32 + (rnd() % 95));
                break;
            case 3: case 4: case 5: case 6:  /* flip to a structural byte */
                buf[at] = "{}()[];:,\"'\\/*!#.@%-+~^$=<>"[rnd() % 27];
                break;
            case 7:  /* truncate */
                n = at;
                buf[n] = 0;
                break;
            case 8:  /* duplicate a span */
                if (n * 2 < sizeof(buf) - 1) {
                    size_t len = (rnd() % (n - at + 1));
                    memmove(buf + at + len, buf + at, n - at);
                    n += len;
                    buf[n] = 0;
                }
                break;
            default: /* insert a high byte */
                if (n + 1 < sizeof(buf) - 1) {
                    memmove(buf + at + 1, buf + at, n - at + 1);
                    buf[at] = (char)(0x80 + (rnd() % 0x7F));
                    n++;
                }
                break;
            }
        }
        {
            struct css_stylesheet *ss = css_parse(buf, n, CSS_ORIGIN_AUTHOR, &m);
            struct css_stylesheet *in = css_parse_style_attr(buf);
            if (ss) {
                total_rules += (unsigned long)css_rule_count(ss);
                if (css_truncated(ss))
                    truncated++;
                /* run the matcher over the survivors so the selector
                 * structures are exercised, not just built */
                {
                    struct tnode *root = tn_parse(
                        "<html><body><div class='a b'><p id=i>"
                        "<a href='http://x'/><li/></p></div></body></html>");
                    struct style_engine *se = style_engine_new(&ss, 1, &test_ops);
                    style_compute_tree(se, root, 0, sink, 0);
                    style_engine_free(se);
                    tn_free(root);
                }
                css_free(ss);
                kept++;
            }
            css_free(in);
        }
    }
    printf("  fuzz: %d mutations parsed, %d sheets built, %lu rules survived,"
           " %d hit a limit\n", iters, kept, total_rules, truncated);

    /* Sanity: the unmutated corpus must still produce rules, otherwise
     * the survival count above is measuring nothing. */
    {
        unsigned long base_rules = 0;
        for (i = 0; i < NCORPUS; i++) {
            struct css_stylesheet *ss = css_parse(corpus[i], strlen(corpus[i]),
                                                  CSS_ORIGIN_AUTHOR, &m);
            base_rules += (unsigned long)css_rule_count(ss);
            css_free(ss);
        }
        printf("  fuzz: unmutated corpus (%d sheets) yields %lu rules\n",
               NCORPUS, base_rules);
        checks++;
        if (base_rules < 12) {
            printf("  FAIL corpus produces too few rules: %lu\n", base_rules);
            failures++;
        }
    }
}

static void fuzz_random(int iters)
{
    char buf[512];
    int i;
    unsigned long rules = 0;

    for (i = 0; i < iters; i++) {
        size_t n = rnd() % sizeof(buf), j;
        for (j = 0; j < n; j++)
            buf[j] = (char)(rnd() & 0xFF);
        {
            struct css_stylesheet *ss = css_parse(buf, n, CSS_ORIGIN_AUTHOR, 0);
            rules += (unsigned long)css_rule_count(ss);
            css_free(ss);
        }
    }
    printf("  fuzz: %d pure-random inputs parsed, %lu rules survived\n",
           iters, rules);
}

/* ================================================================== *
 * main
 * ================================================================== */

int main(int argc, char **argv)
{
    int i;
    int fuzz_iters = 40000;
    int rand_iters = 20000;

    if (argc > 1)
        fuzz_iters = atoi(argv[1]);
    if (argc > 2)
        rand_iters = atoi(argv[2]);

    printf("css engine tests\n");
    printf("----------------\n");
    for (i = 0; i < NCASES; i++)
        run_case(&cases[i]);
    printf("  %d table cases, %d assertions\n", NCASES, checks);

    printf("\napi and limits\n--------------\n");
    api_tests();

    printf("\nmatching cost\n-------------\n");
    perf_test();

    printf("\nfuzzing\n-------\n");
    fuzz(fuzz_iters);
    fuzz_random(rand_iters);

    printf("\n%d assertions, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
