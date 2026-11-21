/* style.c - the cascade: matched rules -> struct computed_style.
 *
 * The cascade order is the one from CSS Cascade level 4, section 6.4.1,
 * lowest priority first:
 *
 *   0  user agent, normal          4  author, !important
 *   1  user, normal                5  style="", !important
 *   2  author, normal              6  user, !important
 *   3  style="", normal            7  user agent, !important
 *
 * Within one level the higher specificity wins, and on a specificity tie
 * the later source order wins. That is the whole of it: there is no
 * sorting step, because each property keeps the (level, specificity,
 * order) triple of the declaration currently winning it and a new
 * declaration only displaces it if it beats that triple.
 *
 * Inheritance and the `inherit` keyword are the same operation - copy
 * one property from the parent's computed style - so both go through
 * copy_prop(). css_property_inherited() in css.c is the single list of
 * which properties that happens to automatically.
 *
 * Nothing here knows what a DOM is. Elements are reached through
 * struct css_elem_ops, so the host test harness drives the identical
 * code path the browser does. The concrete DOM binding lives at the
 * bottom of this file behind CSS_WITH_DOM.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "css.h"

int css_stylesheet_prefers_dark(const struct css_stylesheet *ss);

#define STYLE_MAX_ELEMENTS 200000
#define STYLE_DEFAULT_FONT_SIZE 16
#define STYLE_MIN_FONT_SIZE 1
#define STYLE_MAX_FONT_SIZE 400
#define STYLE_MAX_VAR_DEPTH 8
#define STYLE_RAW_BUF 4096
#define STYLE_POOL_MAX_BYTES 65536UL

/* Shorthand raw-expr markers stored in css_value.kw (must match css.c). */
#define CSS_RAW_NONE       0
#define CSS_RAW_BOX_TOP    1
#define CSS_RAW_BOX_RIGHT  2
#define CSS_RAW_BOX_BOTTOM 3
#define CSS_RAW_BOX_LEFT   4
#define CSS_RAW_BORDER     5
#define CSS_RAW_BG         6
#define CSS_RAW_FLEX       7
#define CSS_RAW_GAP        8
#define CSS_RAW_GRID_LINE  9
#define CSS_RAW_FONT       10
#define CSS_RAW_LIST_STYLE 11
#define CSS_RAW_OUTLINE    12

struct cascade_slot {
    uint8_t  used;
    uint8_t  level;
    uint32_t spec;
    uint32_t order;
};

struct custom_slot {
    const char *name;
    const char *raw;
    uint8_t  level;
    uint32_t spec;
    uint32_t order;
};

struct custom_entry {
    char *name;
    char *value;
    uint8_t state;   /* 0=unresolved, 1=visiting, 2=valid, 3=invalid */
};

struct custom_env {
    const struct custom_env *parent;
    struct custom_entry *entries;
    int count;
};

struct str_node {
    struct str_node *next;
    char s[1];
};

struct style_engine {
    struct css_stylesheet **sheets;
    int nsheet;
    struct css_elem_ops ops;
    int32_t vw, vh;

    /* Per-element scratch, reused. It lives here rather than on the
     * stack because style_compute_tree() recurses and 2 KB per frame
     * would eat the 64 KiB user stack at a depth of 32. */
    struct cascade_slot slots[CSS_PROP_COUNT];
    struct css_value spec[CSS_PROP_COUNT];
    struct custom_slot *cscratch;
    int ncscratch, ccscratch;

    const struct custom_env *cur_env;
    int in_tree_walk;
    int has_customs;
    int prefers_dark;

    struct str_node *str_pool;
    unsigned long str_pool_bytes;
    int styled;
};

/* ================================================================== *
 * small freestanding helpers
 * ================================================================== */

static int s_lc(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int s_is_ws(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int s_is_digit(int c) { return c >= '0' && c <= '9'; }

static int s_is_ident_ch(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c >= 0x80;
}

static int s_stricmp(const char *a, const char *b)
{
    while (*a && *b) {
        int x = s_lc((unsigned char)*a), y = s_lc((unsigned char)*b);
        if (x != y)
            return x - y;
        a++; b++;
    }
    return s_lc((unsigned char)*a) - s_lc((unsigned char)*b);
}

static int s_stricmp_n(const char *a, const char *b, unsigned long n)
{
    unsigned long i;
    for (i = 0; i < n; i++) {
        int x = s_lc((unsigned char)a[i]);
        int y = s_lc((unsigned char)b[i]);
        if (x != y || !a[i] || !b[i])
            return x - y;
    }
    return 0;
}

static char *s_dup_range(const char *s, int len)
{
    char *p;
    if (len < 0) len = 0;
    p = (char *)malloc((unsigned long)len + 1);
    if (!p)
        return 0;
    if (len > 0)
        memcpy(p, s, (unsigned long)len);
    p[len] = 0;
    return p;
}

static char *s_dup_str(const char *s)
{
    return s ? s_dup_range(s, (int)strlen(s)) : 0;
}

static const char *se_intern_str(struct style_engine *se, const char *s, int len)
{
    struct str_node *n;
    unsigned long sz;
    if (!se || !s || len < 0)
        return 0;
    sz = sizeof(*n) + (unsigned long)len;
    if (se->str_pool_bytes + sz > STYLE_POOL_MAX_BYTES)
        return 0;
    n = (struct str_node *)malloc(sz);
    if (!n)
        return 0;
    if (len > 0)
        memcpy(n->s, s, (unsigned long)len);
    n->s[len] = 0;
    n->next = se->str_pool;
    se->str_pool = n;
    se->str_pool_bytes += sz;
    return n->s;
}

static void trim_span(const char *s, int len, int *out_start, int *out_len)
{
    int a = 0, b = len;
    while (a < b && s_is_ws((unsigned char)s[a]))
        a++;
    while (b > a && s_is_ws((unsigned char)s[b - 1]))
        b--;
    *out_start = a;
    *out_len = b - a;
}

static int kw_lookup(const char *const *table, const char *s)
{
    int i;
    if (!table)
        return -1;
    for (i = 0; table[i]; i++)
        if (s_stricmp(table[i], s) == 0)
            return i;
    return -1;
}

#define CSS_STR_X(n, s) s,
static const char *const kw_display[]     = { CSS_DISPLAY_LIST(CSS_STR_X) 0 };
static const char *const kw_flexdir[]     = { CSS_FLEXDIR_LIST(CSS_STR_X) 0 };
static const char *const kw_flexwrap[]    = { CSS_FLEXWRAP_LIST(CSS_STR_X) 0 };
static const char *const kw_justify[]     = { CSS_JUSTIFY_LIST(CSS_STR_X) 0 };
static const char *const kw_align[]       = { CSS_ALIGN_LIST(CSS_STR_X) 0 };
static const char *const kw_position[]    = { CSS_POSITION_LIST(CSS_STR_X) 0 };
static const char *const kw_float[]       = { CSS_FLOAT_LIST(CSS_STR_X) 0 };
static const char *const kw_clear[]       = { CSS_CLEAR_LIST(CSS_STR_X) 0 };
static const char *const kw_textalign[]   = { CSS_TEXTALIGN_LIST(CSS_STR_X) 0 };
static const char *const kw_valign[]      = { CSS_VALIGN_LIST(CSS_STR_X) 0 };
static const char *const kw_whitespace[]  = { CSS_WHITESPACE_LIST(CSS_STR_X) 0 };
static const char *const kw_overflow[]    = { CSS_OVERFLOW_LIST(CSS_STR_X) 0 };
static const char *const kw_visibility[]  = { CSS_VISIBILITY_LIST(CSS_STR_X) 0 };
static const char *const kw_fontstyle[]   = { CSS_FONTSTYLE_LIST(CSS_STR_X) 0 };
static const char *const kw_fontfamily[]  = { CSS_FONTFAMILY_LIST(CSS_STR_X) 0 };
static const char *const kw_borderstyle[] = { CSS_BORDERSTYLE_LIST(CSS_STR_X) 0 };
static const char *const kw_liststyle[]   = { CSS_LISTSTYLE_LIST(CSS_STR_X) 0 };
static const char *const kw_listpos[]     = { CSS_LISTPOS_LIST(CSS_STR_X) 0 };
static const char *const kw_bcollapse[]   = { CSS_BORDERCOLLAPSE_LIST(CSS_STR_X) 0 };
static const char *const kw_ttransform[]  = { CSS_TEXTTRANSFORM_LIST(CSS_STR_X) 0 };
static const char *const kw_boxsizing[]   = { CSS_BOXSIZING_LIST(CSS_STR_X) 0 };
static const char *const kw_colorscheme[] = { CSS_COLORSCHEME_LIST(CSS_STR_X) 0 };
static const char *const kw_fontsize[]    = { CSS_FONTSIZEKW_LIST(CSS_STR_X) 0 };
static const char *const kw_fontweight[]  = { CSS_FONTWEIGHTKW_LIST(CSS_STR_X) 0 };
static const char *const kw_borderwidth[] = { "thin", "medium", "thick", 0 };
static const char *const kw_decor[]       = { "none", "underline", "overline",
                                              "line-through", "blink", 0 };
static const char *const kw_objectfit[]   = { CSS_OBJECTFIT_LIST(CSS_STR_X) 0 };
static const char *const kw_alignself[]   = { CSS_ALIGNSELF_LIST(CSS_STR_X) 0 };
static const char *const kw_textoverflow[]= { CSS_TEXTOVERFLOW_LIST(CSS_STR_X) 0 };
static const char *const kw_wordbreak[]   = { CSS_WORDBREAK_LIST(CSS_STR_X) 0 };
static const char *const kw_overflowwrap[]= { CSS_OVERFLOWWRAP_LIST(CSS_STR_X) 0 };
static const char *const kw_tablelayout[] = { CSS_TABLELAYOUT_LIST(CSS_STR_X) 0 };

/* ================================================================== *
 * initial values
 * ================================================================== */

static struct css_len len_auto(void)
{
    struct css_len l;
    l.v = 0;
    l.pct = 0;
    l.type = CSS_LEN_AUTO;
    return l;
}

static struct css_len len_px(int32_t px)
{
    struct css_len l;
    l.v = px;
    l.pct = 0;
    l.type = CSS_LEN_PX;
    return l;
}

void css_style_initial(struct computed_style *cs)
{
    int i;

    memset(cs, 0, sizeof(*cs));
    cs->display = CSS_DISPLAY_INLINE;
    cs->position = CSS_POSITION_STATIC;
    cs->css_float = CSS_FLOAT_NONE;
    cs->clear = CSS_CLEAR_NONE;
    cs->box_sizing = CSS_BOXSIZING_CONTENT;
    cs->text_align = CSS_TEXTALIGN_START;
    cs->vertical_align = CSS_VALIGN_BASELINE;
    cs->white_space = CSS_WHITESPACE_NORMAL;
    cs->overflow = CSS_OVERFLOW_VISIBLE;
    cs->visibility = CSS_VISIBILITY_VISIBLE;
    cs->font_style = CSS_FONTSTYLE_NORMAL;
    cs->font_family = CSS_FONTFAMILY_SERIF;
    cs->list_style_type = CSS_LISTSTYLE_DISC;
    cs->list_style_position = CSS_LISTPOS_OUTSIDE;
    cs->border_collapse = CSS_BORDERCOLLAPSE_SEPARATE;
    cs->text_transform = CSS_TEXTTRANSFORM_NONE;
    cs->text_decoration = CSS_DECOR_NONE;
    cs->flex_direction = CSS_FLEXDIR_ROW;
    cs->flex_wrap = CSS_FLEXWRAP_NOWRAP;
    cs->justify_content = CSS_JUSTIFY_START;
    cs->align_items = CSS_ALIGN_STRETCH;
    cs->color_scheme = CSS_COLORSCHEME_NORMAL;
    cs->font_weight = 400;
    cs->opacity = 1000;
    cs->font_size = STYLE_DEFAULT_FONT_SIZE;
    cs->border_spacing = 0;
    cs->z_index = 0;
    cs->z_auto = 1;
    cs->vertical_align_px = 0;
    cs->flex_grow = 0;
    cs->flex_shrink = 1000;
    cs->flex_basis = len_auto();
    cs->gap = 0;
    cs->row_gap = 0;
    cs->column_gap = 0;
    cs->width = len_auto();
    cs->height = len_auto();
    cs->min_width = len_px(0);
    cs->min_height = len_px(0);
    cs->max_width.type = CSS_LEN_NONE;
    cs->max_height.type = CSS_LEN_NONE;
    for (i = 0; i < 4; i++) {
        cs->margin[i] = len_px(0);
        cs->padding[i] = len_px(0);
        cs->offset[i] = len_auto();
        cs->border_style[i] = CSS_BORDERSTYLE_NONE;
        cs->border_width[i] = 0;
        cs->border_color[i] = CSS_RGB(0, 0, 0);
        cs->border_radius[i] = 0;
    }
    cs->line_height.type = CSS_LEN_NORMAL;
    cs->text_indent = len_px(0);
    cs->color = CSS_RGB(0, 0, 0);
    cs->background_color = CSS_TRANSPARENT;
    cs->grad_angle_deg = 180;
    cs->grid_col_span = 1;
    cs->grid_row_span = 1;
    cs->background_image = 0;
    cs->font_family_name = 0;
    cs->align_self = CSS_ALIGNSELF_AUTO;
    cs->object_fit = CSS_OBJECTFIT_FILL;
    cs->outline_style = CSS_BORDERSTYLE_NONE;
    cs->text_overflow = CSS_TEXTOVERFLOW_CLIP;
    cs->word_break = CSS_WORDBREAK_NORMAL;
    cs->overflow_wrap = CSS_OVERFLOWWRAP_NORMAL;
    cs->table_layout = CSS_TABLELAYOUT_AUTO;
    cs->outline_width = 0;
    cs->outline_offset = 0;
    cs->outline_color = CSS_RGB(0, 0, 0);
    cs->order = 0;
    cs->aspect_ratio = 0;
    cs->letter_spacing = 0;
    cs->word_spacing = 0;
    cs->has_transform = 0;
    cs->translate_x = len_px(0);
    cs->translate_y = len_px(0);
    cs->scale_x = 1000;
    cs->scale_y = 1000;
}

/* ================================================================== *
 * one property at a time
 * ================================================================== */

static void copy_prop(struct computed_style *d, const struct computed_style *s,
                      int prop)
{
    int i;
    switch (prop) {
    case CSS_PROP_DISPLAY:        d->display = s->display; break;
    case CSS_PROP_POSITION:       d->position = s->position; break;
    case CSS_PROP_FLOAT:          d->css_float = s->css_float; break;
    case CSS_PROP_CLEAR:          d->clear = s->clear; break;
    case CSS_PROP_BOX_SIZING:     d->box_sizing = s->box_sizing; break;
    case CSS_PROP_WIDTH:          d->width = s->width; break;
    case CSS_PROP_HEIGHT:         d->height = s->height; break;
    case CSS_PROP_MIN_WIDTH:      d->min_width = s->min_width; break;
    case CSS_PROP_MAX_WIDTH:      d->max_width = s->max_width; break;
    case CSS_PROP_MIN_HEIGHT:     d->min_height = s->min_height; break;
    case CSS_PROP_MAX_HEIGHT:     d->max_height = s->max_height; break;
    case CSS_PROP_TOP:            d->offset[CSS_TOP] = s->offset[CSS_TOP]; break;
    case CSS_PROP_RIGHT:          d->offset[CSS_RIGHT] = s->offset[CSS_RIGHT]; break;
    case CSS_PROP_BOTTOM:         d->offset[CSS_BOTTOM] = s->offset[CSS_BOTTOM]; break;
    case CSS_PROP_LEFT:           d->offset[CSS_LEFT] = s->offset[CSS_LEFT]; break;
    case CSS_PROP_MARGIN_TOP:     d->margin[CSS_TOP] = s->margin[CSS_TOP]; break;
    case CSS_PROP_MARGIN_RIGHT:   d->margin[CSS_RIGHT] = s->margin[CSS_RIGHT]; break;
    case CSS_PROP_MARGIN_BOTTOM:  d->margin[CSS_BOTTOM] = s->margin[CSS_BOTTOM]; break;
    case CSS_PROP_MARGIN_LEFT:    d->margin[CSS_LEFT] = s->margin[CSS_LEFT]; break;
    case CSS_PROP_PADDING_TOP:    d->padding[CSS_TOP] = s->padding[CSS_TOP]; break;
    case CSS_PROP_PADDING_RIGHT:  d->padding[CSS_RIGHT] = s->padding[CSS_RIGHT]; break;
    case CSS_PROP_PADDING_BOTTOM: d->padding[CSS_BOTTOM] = s->padding[CSS_BOTTOM]; break;
    case CSS_PROP_PADDING_LEFT:   d->padding[CSS_LEFT] = s->padding[CSS_LEFT]; break;
    case CSS_PROP_BORDER_TOP_WIDTH:    d->border_width[CSS_TOP] = s->border_width[CSS_TOP]; break;
    case CSS_PROP_BORDER_RIGHT_WIDTH:  d->border_width[CSS_RIGHT] = s->border_width[CSS_RIGHT]; break;
    case CSS_PROP_BORDER_BOTTOM_WIDTH: d->border_width[CSS_BOTTOM] = s->border_width[CSS_BOTTOM]; break;
    case CSS_PROP_BORDER_LEFT_WIDTH:   d->border_width[CSS_LEFT] = s->border_width[CSS_LEFT]; break;
    case CSS_PROP_BORDER_TOP_STYLE:    d->border_style[CSS_TOP] = s->border_style[CSS_TOP]; break;
    case CSS_PROP_BORDER_RIGHT_STYLE:  d->border_style[CSS_RIGHT] = s->border_style[CSS_RIGHT]; break;
    case CSS_PROP_BORDER_BOTTOM_STYLE: d->border_style[CSS_BOTTOM] = s->border_style[CSS_BOTTOM]; break;
    case CSS_PROP_BORDER_LEFT_STYLE:   d->border_style[CSS_LEFT] = s->border_style[CSS_LEFT]; break;
    case CSS_PROP_BORDER_TOP_COLOR:    d->border_color[CSS_TOP] = s->border_color[CSS_TOP]; break;
    case CSS_PROP_BORDER_RIGHT_COLOR:  d->border_color[CSS_RIGHT] = s->border_color[CSS_RIGHT]; break;
    case CSS_PROP_BORDER_BOTTOM_COLOR: d->border_color[CSS_BOTTOM] = s->border_color[CSS_BOTTOM]; break;
    case CSS_PROP_BORDER_LEFT_COLOR:   d->border_color[CSS_LEFT] = s->border_color[CSS_LEFT]; break;
    case CSS_PROP_BORDER_TL_RADIUS:    d->border_radius[CSS_TL] = s->border_radius[CSS_TL]; break;
    case CSS_PROP_BORDER_TR_RADIUS:    d->border_radius[CSS_TR] = s->border_radius[CSS_TR]; break;
    case CSS_PROP_BORDER_BR_RADIUS:    d->border_radius[CSS_BR] = s->border_radius[CSS_BR]; break;
    case CSS_PROP_BORDER_BL_RADIUS:    d->border_radius[CSS_BL] = s->border_radius[CSS_BL]; break;
    case CSS_PROP_FONT_FAMILY:
        d->font_family = s->font_family;
        d->font_family_name = s->font_family_name;
        break;
    case CSS_PROP_FONT_SIZE:      d->font_size = s->font_size; break;
    case CSS_PROP_FONT_WEIGHT:    d->font_weight = s->font_weight; break;
    case CSS_PROP_FONT_STYLE:     d->font_style = s->font_style; break;
    case CSS_PROP_COLOR:          d->color = s->color; break;
    case CSS_PROP_COLOR_SCHEME:   d->color_scheme = s->color_scheme; break;
    case CSS_PROP_OPACITY:        d->opacity = s->opacity; break;
    case CSS_PROP_BACKGROUND_COLOR: d->background_color = s->background_color; break;
    case CSS_PROP_BACKGROUND_IMAGE:
        d->background_image = s->background_image;
        d->has_gradient = s->has_gradient;
        d->grad_nstops = s->grad_nstops;
        d->grad_angle_deg = s->grad_angle_deg;
        for (i = 0; i < CSS_GRAD_MAX_STOPS; i++) {
            d->grad_colors[i] = s->grad_colors[i];
            d->grad_pos[i] = s->grad_pos[i];
        }
        break;
    case CSS_PROP_BOX_SHADOW:
        d->has_shadow = s->has_shadow;
        d->shadow_inset = s->shadow_inset;
        d->shadow_x = s->shadow_x;
        d->shadow_y = s->shadow_y;
        d->shadow_blur = s->shadow_blur;
        d->shadow_spread = s->shadow_spread;
        d->shadow_color = s->shadow_color;
        break;
    case CSS_PROP_TEXT_ALIGN:     d->text_align = s->text_align; break;
    case CSS_PROP_TEXT_DECORATION: d->text_decoration = s->text_decoration; break;
    case CSS_PROP_TEXT_INDENT:    d->text_indent = s->text_indent; break;
    case CSS_PROP_TEXT_TRANSFORM: d->text_transform = s->text_transform; break;
    case CSS_PROP_VERTICAL_ALIGN:
        d->vertical_align = s->vertical_align;
        d->vertical_align_px = s->vertical_align_px;
        break;
    case CSS_PROP_LINE_HEIGHT:    d->line_height = s->line_height; break;
    case CSS_PROP_LIST_STYLE_TYPE: d->list_style_type = s->list_style_type; break;
    case CSS_PROP_LIST_STYLE_POSITION: d->list_style_position = s->list_style_position; break;
    case CSS_PROP_WHITE_SPACE:    d->white_space = s->white_space; break;
    case CSS_PROP_OVERFLOW:       d->overflow = s->overflow; break;
    case CSS_PROP_VISIBILITY:     d->visibility = s->visibility; break;
    case CSS_PROP_BORDER_COLLAPSE: d->border_collapse = s->border_collapse; break;
    case CSS_PROP_BORDER_SPACING: d->border_spacing = s->border_spacing; break;
    case CSS_PROP_Z_INDEX:
        d->z_index = s->z_index;
        d->z_auto = s->z_auto;
        break;
    case CSS_PROP_FLEX_DIRECTION:  d->flex_direction = s->flex_direction; break;
    case CSS_PROP_FLEX_WRAP:       d->flex_wrap = s->flex_wrap; break;
    case CSS_PROP_JUSTIFY_CONTENT: d->justify_content = s->justify_content; break;
    case CSS_PROP_ALIGN_ITEMS:     d->align_items = s->align_items; break;
    case CSS_PROP_FLEX_GROW:       d->flex_grow = s->flex_grow; break;
    case CSS_PROP_FLEX_SHRINK:     d->flex_shrink = s->flex_shrink; break;
    case CSS_PROP_FLEX_BASIS:      d->flex_basis = s->flex_basis; break;
    case CSS_PROP_GAP:
        d->gap = s->gap;
        d->row_gap = s->row_gap;
        d->column_gap = s->column_gap;
        break;
    case CSS_PROP_ROW_GAP:         d->row_gap = s->row_gap; break;
    case CSS_PROP_COLUMN_GAP:      d->column_gap = s->column_gap; break;
    case CSS_PROP_GRID_TEMPLATE_COLS:
        d->grid_ncols = s->grid_ncols;
        for (i = 0; i < CSS_GRID_MAX_COLS; i++)
            d->grid_cols[i] = s->grid_cols[i];
        break;
    case CSS_PROP_GRID_TEMPLATE_ROWS:
        d->grid_nrows = s->grid_nrows;
        for (i = 0; i < CSS_GRID_MAX_ROWS; i++)
            d->grid_rows[i] = s->grid_rows[i];
        break;
    case CSS_PROP_GRID_COL_START:  d->grid_col_start = s->grid_col_start; break;
    case CSS_PROP_GRID_COL_END:
        d->grid_col_end = s->grid_col_end;
        d->grid_col_span = s->grid_col_span;
        break;
    case CSS_PROP_GRID_ROW_START:  d->grid_row_start = s->grid_row_start; break;
    case CSS_PROP_GRID_ROW_END:
        d->grid_row_end = s->grid_row_end;
        d->grid_row_span = s->grid_row_span;
        break;
    case CSS_PROP_ASPECT_RATIO:    d->aspect_ratio = s->aspect_ratio; break;
    case CSS_PROP_OBJECT_FIT:      d->object_fit = s->object_fit; break;
    case CSS_PROP_ALIGN_SELF:      d->align_self = s->align_self; break;
    case CSS_PROP_ORDER:           d->order = s->order; break;
    case CSS_PROP_OUTLINE_WIDTH:   d->outline_width = s->outline_width; break;
    case CSS_PROP_OUTLINE_STYLE:   d->outline_style = s->outline_style; break;
    case CSS_PROP_OUTLINE_COLOR:   d->outline_color = s->outline_color; break;
    case CSS_PROP_OUTLINE_OFFSET:  d->outline_offset = s->outline_offset; break;
    case CSS_PROP_TEXT_OVERFLOW:   d->text_overflow = s->text_overflow; break;
    case CSS_PROP_WORD_BREAK:      d->word_break = s->word_break; break;
    case CSS_PROP_OVERFLOW_WRAP:   d->overflow_wrap = s->overflow_wrap; break;
    case CSS_PROP_LETTER_SPACING:  d->letter_spacing = s->letter_spacing; break;
    case CSS_PROP_WORD_SPACING:    d->word_spacing = s->word_spacing; break;
    case CSS_PROP_TABLE_LAYOUT:    d->table_layout = s->table_layout; break;
    case CSS_PROP_TRANSFORM:
        d->has_transform = s->has_transform;
        d->translate_x = s->translate_x;
        d->translate_y = s->translate_y;
        d->scale_x = s->scale_x;
        d->scale_y = s->scale_y;
        break;
    default: break;
    }
}

/* ================================================================== *
 * length resolution
 * ================================================================== */

static int32_t clamp_px(int64_t v)
{
    if (v < -1000000) return -1000000;
    if (v >  1000000) return  1000000;
    return (int32_t)v;
}

static int32_t clamp_pct_milli(int64_t v)
{
    if (v < -1000000000LL) return -1000000000;
    if (v >  1000000000LL) return  1000000000;
    return (int32_t)v;
}

static int32_t round_milli(int64_t m)
{
    if (m >= 0)
        return clamp_px((m + 500) / 1000);
    return clamp_px(-((-m + 500) / 1000));
}

static int64_t div_round64(int64_t num, int64_t den)
{
    int neg = 0;
    if (den == 0)
        return 0;
    if (den < 0) { den = -den; num = -num; }
    if (num < 0) { neg = 1; num = -num; }
    {
        int64_t q = (num + den / 2) / den;
        return neg ? -q : q;
    }
}

static int64_t resolve_px_milli(const struct style_engine *se,
                                int64_t n, int unit, int32_t fs,
                                int32_t root_fs)
{
    switch (unit) {
    case CSS_UNIT_PX:  return n;
    case CSS_UNIT_EM:  return n * fs;
    case CSS_UNIT_REM: return n * root_fs;
    case CSS_UNIT_EX:  return n * (fs / 2 > 0 ? fs / 2 : 1);
    case CSS_UNIT_CH:  return n * (fs / 2 > 0 ? fs / 2 : 1);
    case CSS_UNIT_PT:  return n * 4 / 3;
    case CSS_UNIT_PC:  return n * 16;
    case CSS_UNIT_IN:  return n * 96;
    case CSS_UNIT_CM:  return n * 9600 / 254;
    case CSS_UNIT_MM:  return n * 960 / 254;
    case CSS_UNIT_VW:  return (int64_t)se->vw * n / 100;
    case CSS_UNIT_VH:  return (int64_t)se->vh * n / 100;
    default:           return n;
    }
}

static int32_t resolve_px(const struct style_engine *se,
                          const struct css_value *v, int32_t fs, int32_t root_fs)
{
    return round_milli(resolve_px_milli(se, v->num, v->unit, fs, root_fs));
}

static struct css_len to_len(const struct style_engine *se,
                             const struct css_value *v, int32_t fs,
                             int32_t root_fs)
{
    struct css_len l;

    l.v = 0;
    l.pct = 0;
    switch (v->type) {
    case CSS_VAL_AUTO:
        l.type = CSS_LEN_AUTO;
        break;
    case CSS_VAL_PERCENT:
        l.type = CSS_LEN_PCT;
        l.v = v->num;
        break;
    case CSS_VAL_LENGTH:
        l.type = CSS_LEN_PX;
        l.v = resolve_px(se, v, fs, root_fs);
        break;
    case CSS_VAL_KEYWORD:            /* max-width: none */
        l.type = CSS_LEN_NONE;
        break;
    default:
        l.type = CSS_LEN_PX;
        break;
    }
    return l;
}

static const int32_t abs_font_size[7] = { 9, 10, 13, 16, 18, 24, 32 };

static int32_t clamp_font(int32_t v)
{
    if (v < STYLE_MIN_FONT_SIZE) return STYLE_MIN_FONT_SIZE;
    if (v > STYLE_MAX_FONT_SIZE) return STYLE_MAX_FONT_SIZE;
    return v;
}

static uint16_t relative_weight(uint16_t base, int bolder)
{
    if (bolder) {
        if (base < 400) return 400;
        if (base < 600) return 700;
        return 900;
    }
    if (base <= 500) return 100;
    if (base <= 700) return 400;
    return 700;
}

/* ================================================================== *
 * custom property inheritance & var() expansion
 * ================================================================== */

static int expand_vars_buf(const struct custom_env *env, const char *src,
                           int src_len, char *out, int cap, int depth,
                           int *hit_cycle);

static const char *lookup_custom_var(const struct custom_env *env,
                                     const char *name, int depth,
                                     int *hit_cycle)
{
    const struct custom_env *e = env;
    while (e) {
        int i;
        for (i = 0; i < e->count; i++) {
            struct custom_entry *ce = &e->entries[i];
            if (strcmp(ce->name, name) != 0)
                continue;
            if (ce->state == 2)
                return ce->value;
            if (ce->state == 3)
                return 0;
            if (ce->state == 1) {
                ce->state = 3;
                if (hit_cycle) *hit_cycle = 1;
                return 0;
            }
            if (depth >= STYLE_MAX_VAR_DEPTH) {
                ce->state = 3;
                if (hit_cycle) *hit_cycle = 1;
                return 0;
            }
            {
                char buf[STYLE_RAW_BUF];
                int cyc = 0;
                char *raw = ce->value;
                ce->state = 1;
                if (!raw || !expand_vars_buf(e, raw, (int)strlen(raw), buf,
                                             sizeof(buf), depth + 1, &cyc) || cyc) {
                    free(raw);
                    ce->value = 0;
                    ce->state = 3;
                    if (cyc && hit_cycle) *hit_cycle = 1;
                    return 0;
                }
                free(raw);
                ce->value = s_dup_str(buf);
                if (!ce->value) {
                    ce->state = 3;
                    return 0;
                }
                ce->state = 2;
                return ce->value;
            }
        }
        e = e->parent;
    }
    return 0;
}

static int expand_vars_buf(const struct custom_env *env, const char *src,
                           int src_len, char *out, int cap, int depth,
                           int *hit_cycle)
{
    int i = 0, o = 0;

    if (!src || cap <= 0)
        return 0;
    if (depth > STYLE_MAX_VAR_DEPTH) {
        if (hit_cycle) *hit_cycle = 1;
        return 0;
    }

    while (i < src_len) {
        int c = (unsigned char)src[i];
        if (c == '"' || c == '\'') {
            int q = c;
            if (o >= cap - 1) return 0;
            out[o++] = (char)q;
            i++;
            while (i < src_len) {
                int sc = (unsigned char)src[i];
                if (o >= cap - 1) return 0;
                out[o++] = (char)sc;
                i++;
                if (sc == '\\' && i < src_len) {
                    if (o >= cap - 1) return 0;
                    out[o++] = src[i++];
                    continue;
                }
                if (sc == q)
                    break;
            }
            continue;
        }
        if ((c == 'v' || c == 'V') && i + 4 <= src_len &&
            s_stricmp_n(src + i, "var(", 4) == 0 &&
            (i == 0 || !s_is_ident_ch((unsigned char)src[i - 1]))) {
            int k = i + 4;
            char vname[CSS_MAX_IDENT];
            int nlen = 0, has_fb = 0, fb_start = 0, fb_len = 0, pdepth = 1;
            const char *val;
            int local_cyc = 0;

            while (k < src_len && s_is_ws((unsigned char)src[k]))
                k++;
            while (k < src_len && src[k] != ',' && src[k] != ')' &&
                   !s_is_ws((unsigned char)src[k])) {
                if (nlen < CSS_MAX_IDENT - 1)
                    vname[nlen++] = src[k];
                k++;
            }
            vname[nlen] = 0;
            if (nlen < 3 || vname[0] != '-' || vname[1] != '-')
                return 0;
            while (k < src_len && s_is_ws((unsigned char)src[k]))
                k++;
            if (k < src_len && src[k] == ',') {
                has_fb = 1;
                k++;
                while (k < src_len && s_is_ws((unsigned char)src[k]))
                    k++;
                fb_start = k;
                while (k < src_len && pdepth > 0) {
                    int fc = (unsigned char)src[k];
                    if (fc == '"' || fc == '\'') {
                        int q = fc;
                        k++;
                        while (k < src_len) {
                            if (src[k] == '\\' && k + 1 < src_len) { k += 2; continue; }
                            if (src[k] == q) { k++; break; }
                            k++;
                        }
                        continue;
                    }
                    if (fc == '(' || fc == '[' || fc == '{') pdepth++;
                    else if (fc == ')' || fc == ']' || fc == '}') {
                        pdepth--;
                        if (pdepth == 0)
                            break;
                    }
                    k++;
                }
                if (pdepth != 0)
                    return 0;
                fb_len = k - fb_start;
                while (fb_len > 0 && s_is_ws((unsigned char)src[fb_start + fb_len - 1]))
                    fb_len--;
                k++; /* consume ')' */
            } else if (k < src_len && src[k] == ')') {
                k++;
            } else {
                return 0;
            }

            val = lookup_custom_var(env, vname, depth, &local_cyc);
            if (local_cyc && hit_cycle)
                *hit_cycle = 1;
            if (val) {
                int vlen = (int)strlen(val);
                if (o + vlen >= cap)
                    return 0;
                if (vlen > 0)
                    memcpy(out + o, val, (unsigned long)vlen);
                o += vlen;
            } else if (has_fb && !local_cyc) {
                char fb_buf[STYLE_RAW_BUF];
                int flen;
                if (!expand_vars_buf(env, src + fb_start, fb_len, fb_buf,
                                     sizeof(fb_buf), depth + 1, hit_cycle))
                    return 0;
                flen = (int)strlen(fb_buf);
                if (o + flen >= cap)
                    return 0;
                if (flen > 0)
                    memcpy(out + o, fb_buf, (unsigned long)flen);
                o += flen;
            } else {
                return 0;
            }
            i = k;
            continue;
        }
        if (o >= cap - 1)
            return 0;
        out[o++] = (char)c;
        i++;
    }
    out[o] = 0;
    return 1;
}

static void free_custom_env(struct custom_env *env)
{
    int i;
    if (!env)
        return;
    for (i = 0; i < env->count; i++) {
        free(env->entries[i].name);
        free(env->entries[i].value);
    }
    free(env->entries);
    free(env);
}

static struct custom_env *build_custom_env(struct style_engine *se,
                                           const struct custom_env *parent_env)
{
    struct custom_env *env;
    int i;

    if (se->ncscratch == 0)
        return (struct custom_env *)parent_env;

    env = (struct custom_env *)calloc(1, sizeof(*env));
    if (!env)
        return (struct custom_env *)parent_env;
    env->parent = parent_env;
    env->entries = (struct custom_entry *)calloc((unsigned long)se->ncscratch,
                                                 sizeof(struct custom_entry));
    if (!env->entries) {
        free(env);
        return (struct custom_env *)parent_env;
    }
    env->count = se->ncscratch;
    for (i = 0; i < se->ncscratch; i++) {
        const char *raw = se->cscratch[i].raw;
        env->entries[i].name = s_dup_str(se->cscratch[i].name);
        if (!env->entries[i].name) {
            env->entries[i].state = 3;
            continue;
        }
        if (!raw) {
            env->entries[i].state = 3;
            continue;
        }
        if (s_stricmp(raw, "initial") == 0) {
            env->entries[i].state = 3;
        } else if (s_stricmp(raw, "inherit") == 0 ||
                   s_stricmp(raw, "unset") == 0) {
            const char *pv = lookup_custom_var(parent_env, env->entries[i].name,
                                               0, 0);
            if (pv) {
                env->entries[i].value = s_dup_str(pv);
                env->entries[i].state = env->entries[i].value ? 2 : 3;
            } else {
                env->entries[i].state = 3;
            }
        } else {
            env->entries[i].value = s_dup_str(raw);
            env->entries[i].state = env->entries[i].value ? 0 : 3;
        }
    }
    for (i = 0; i < env->count; i++) {
        if (env->entries[i].state == 0)
            (void)lookup_custom_var(env, env->entries[i].name, 0, 0);
    }
    return env;
}

/* ================================================================== *
 * token / argument splitting helpers
 * ================================================================== */

static int split_top_tokens(const char *s, int len, int *starts, int *lens,
                            int max_tok)
{
    int i = 0, n = 0;
    while (i < len && n < max_tok) {
        int st, depth = 0;
        while (i < len && s_is_ws((unsigned char)s[i]))
            i++;
        if (i >= len)
            break;
        st = i;
        while (i < len) {
            int c = (unsigned char)s[i];
            if (c == '"' || c == '\'') {
                int q = c;
                i++;
                while (i < len) {
                    if (s[i] == '\\' && i + 1 < len) { i += 2; continue; }
                    if (s[i] == q) { i++; break; }
                    i++;
                }
                continue;
            }
            if (c == '(' || c == '[') depth++;
            else if ((c == ')' || c == ']') && depth > 0) depth--;
            else if (depth == 0 && s_is_ws(c)) break;
            i++;
        }
        starts[n] = st;
        lens[n] = i - st;
        n++;
    }
    return n;
}

static int split_comma_args(const char *s, int len, int *starts, int *lens,
                            int max_args)
{
    int i = 0, n = 0, st = 0, depth = 0;
    while (i <= len && n < max_args) {
        int c = (i < len) ? (unsigned char)s[i] : ',';
        if (i < len && (c == '"' || c == '\'')) {
            int q = c;
            i++;
            while (i < len) {
                if (s[i] == '\\' && i + 1 < len) { i += 2; continue; }
                if (s[i] == q) { i++; break; }
                i++;
            }
            continue;
        }
        if (i < len && (c == '(' || c == '[' || c == '{')) {
            depth++;
            i++;
            continue;
        }
        if (i < len && (c == ')' || c == ']' || c == '}')) {
            if (depth > 0) depth--;
            i++;
            continue;
        }
        if (i == len || (depth == 0 && c == ',')) {
            int a, al;
            trim_span(s + st, i - st, &a, &al);
            starts[n] = st + a;
            lens[n] = al;
            n++;
            st = i + 1;
        }
        i++;
    }
    return n;
}

/* ================================================================== *
 * recursive-descent calc() / min() / max() / clamp()
 * ================================================================== */

struct calc_val {
    int64_t px_milli;
    int64_t pct_milli;
    int has_unit;
};

struct calc_parser {
    const struct style_engine *se;
    const char *s;
    int len;
    int i;
    int32_t fs;
    int32_t root_fs;
    int depth;
};

static void cp_ws(struct calc_parser *cp)
{
    while (cp->i < cp->len && s_is_ws((unsigned char)cp->s[cp->i]))
        cp->i++;
}

static int parse_number_milli(const char *s, int len, int *idx,
                              int64_t *num_milli, int *is_int)
{
    int i = *idx, sign = 1, fdigits = 0, had_frac = 0, had_exp = 0;
    int64_t ip = 0, fp = 0, expv = 0;
    int expsign = 1;

    if (i < len && (s[i] == '+' || s[i] == '-')) {
        if (s[i] == '-') sign = -1;
        i++;
    }
    if (i >= len || (!s_is_digit((unsigned char)s[i]) &&
                     !(s[i] == '.' && i + 1 < len &&
                       s_is_digit((unsigned char)s[i + 1]))))
        return 0;

    while (i < len && s_is_digit((unsigned char)s[i])) {
        if (ip < 100000000LL)
            ip = ip * 10 + (s[i] - '0');
        i++;
    }
    if (i + 1 < len && s[i] == '.' && s_is_digit((unsigned char)s[i + 1])) {
        had_frac = 1;
        i++;
        while (i < len && s_is_digit((unsigned char)s[i])) {
            if (fdigits < 3) {
                fp = fp * 10 + (s[i] - '0');
                fdigits++;
            }
            i++;
        }
    }
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        int k = i + 1, es = 1;
        if (k < len && (s[k] == '+' || s[k] == '-')) {
            if (s[k] == '-') es = -1;
            k++;
        }
        if (k < len && s_is_digit((unsigned char)s[k])) {
            had_exp = 1;
            expsign = es;
            i = k;
            while (i < len && s_is_digit((unsigned char)s[i])) {
                if (expv < 100)
                    expv = expv * 10 + (s[i] - '0');
                i++;
            }
        }
    }
    while (fdigits < 3) {
        fp *= 10;
        fdigits++;
    }
    {
        int64_t v = ip * 1000 + fp;
        if (had_exp) {
            int64_t e = expv;
            while (e-- > 0) {
                if (expsign > 0) {
                    if (v > 2000000000LL / 10) { v = 2000000000LL; break; }
                    v *= 10;
                } else {
                    v /= 10;
                }
            }
        }
        v *= sign;
        if (v < -2000000000LL) v = -2000000000LL;
        if (v >  2000000000LL) v =  2000000000LL;
        *num_milli = v;
    }
    if (is_int)
        *is_int = (!had_frac && !had_exp);
    *idx = i;
    return 1;
}

static int parse_unit_id_str(const char *u)
{
    if (s_stricmp(u, "px") == 0)  return CSS_UNIT_PX;
    if (s_stricmp(u, "em") == 0)  return CSS_UNIT_EM;
    if (s_stricmp(u, "rem") == 0) return CSS_UNIT_REM;
    if (s_stricmp(u, "ex") == 0)  return CSS_UNIT_EX;
    if (s_stricmp(u, "ch") == 0)  return CSS_UNIT_CH;
    if (s_stricmp(u, "pt") == 0)  return CSS_UNIT_PT;
    if (s_stricmp(u, "pc") == 0)  return CSS_UNIT_PC;
    if (s_stricmp(u, "cm") == 0)  return CSS_UNIT_CM;
    if (s_stricmp(u, "mm") == 0)  return CSS_UNIT_MM;
    if (s_stricmp(u, "in") == 0)  return CSS_UNIT_IN;
    if (s_stricmp(u, "vw") == 0)  return CSS_UNIT_VW;
    if (s_stricmp(u, "vh") == 0)  return CSS_UNIT_VH;
    return CSS_UNIT_NONE;
}

static int cp_eval_expr(struct calc_parser *cp, struct calc_val *out);

static int64_t calc_cmp_score(const struct style_engine *se, struct calc_val v)
{
    return v.px_milli + (v.pct_milli * (int64_t)se->vw) / 100;
}

static int calc_less(const struct style_engine *se,
                     struct calc_val a, struct calc_val b)
{
    if (a.pct_milli == b.pct_milli)
        return a.px_milli < b.px_milli;
    return calc_cmp_score(se, a) < calc_cmp_score(se, b);
}

static int cp_eval_primary(struct calc_parser *cp, struct calc_val *out)
{
    int64_t num = 0;

    if (cp->depth > 24)
        return 0;
    cp_ws(cp);
    if (cp->i >= cp->len)
        return 0;

    if (cp->s[cp->i] == '(') {
        cp->i++;
        cp->depth++;
        if (!cp_eval_expr(cp, out))
            return 0;
        cp->depth--;
        cp_ws(cp);
        if (cp->i >= cp->len || cp->s[cp->i] != ')')
            return 0;
        cp->i++;
        return 1;
    }

    if ((cp->s[cp->i] >= 'a' && cp->s[cp->i] <= 'z') ||
        (cp->s[cp->i] >= 'A' && cp->s[cp->i] <= 'Z')) {
        int st = cp->i, flen;
        char fname[16];
        int is_calc, is_min, is_max, is_clamp;
        struct calc_val args[16];
        int narg = 0, k;

        while (cp->i < cp->len &&
               ((cp->s[cp->i] >= 'a' && cp->s[cp->i] <= 'z') ||
                (cp->s[cp->i] >= 'A' && cp->s[cp->i] <= 'Z') ||
                cp->s[cp->i] == '-'))
            cp->i++;
        flen = cp->i - st;
        if (flen <= 0 || flen >= (int)sizeof(fname) ||
            cp->i >= cp->len || cp->s[cp->i] != '(')
            return 0;
        memcpy(fname, cp->s + st, (unsigned long)flen);
        fname[flen] = 0;
        is_calc  = (s_stricmp(fname, "calc") == 0);
        is_min   = (s_stricmp(fname, "min") == 0);
        is_max   = (s_stricmp(fname, "max") == 0);
        is_clamp = (s_stricmp(fname, "clamp") == 0);
        if (!is_calc && !is_min && !is_max && !is_clamp)
            return 0;
        cp->i++; /* '(' */
        cp->depth++;
        for (;;) {
            if (narg >= 16)
                return 0;
            if (!cp_eval_expr(cp, &args[narg]))
                return 0;
            narg++;
            cp_ws(cp);
            if (cp->i < cp->len && cp->s[cp->i] == ',') {
                if (is_calc)
                    return 0;
                cp->i++;
                continue;
            }
            if (cp->i < cp->len && cp->s[cp->i] == ')') {
                cp->i++;
                break;
            }
            return 0;
        }
        cp->depth--;
        if (is_calc) {
            if (narg != 1) return 0;
            *out = args[0];
            return 1;
        }
        for (k = 1; k < narg; k++)
            if (args[k].has_unit != args[0].has_unit)
                return 0;
        if (is_min) {
            *out = args[0];
            for (k = 1; k < narg; k++)
                if (calc_less(cp->se, args[k], *out))
                    *out = args[k];
            return 1;
        }
        if (is_max) {
            *out = args[0];
            for (k = 1; k < narg; k++)
                if (calc_less(cp->se, *out, args[k]))
                    *out = args[k];
            return 1;
        }
        /* clamp(lo, val, hi) = max(lo, min(val, hi)) */
        if (narg != 3)
            return 0;
        {
            struct calc_val mid = calc_less(cp->se, args[1], args[2]) ? args[1] : args[2];
            *out = calc_less(cp->se, args[0], mid) ? mid : args[0];
        }
        return 1;
    }

    if (!parse_number_milli(cp->s, cp->len, &cp->i, &num, 0))
        return 0;
    if (cp->i < cp->len && cp->s[cp->i] == '%') {
        cp->i++;
        out->px_milli = 0;
        out->pct_milli = num;
        out->has_unit = 1;
        return 1;
    }
    if (cp->i < cp->len &&
        ((cp->s[cp->i] >= 'a' && cp->s[cp->i] <= 'z') ||
         (cp->s[cp->i] >= 'A' && cp->s[cp->i] <= 'Z'))) {
        char ub[16];
        int ul = 0, u;
        while (cp->i < cp->len &&
               ((cp->s[cp->i] >= 'a' && cp->s[cp->i] <= 'z') ||
                (cp->s[cp->i] >= 'A' && cp->s[cp->i] <= 'Z'))) {
            if (ul < (int)sizeof(ub) - 1)
                ub[ul++] = cp->s[cp->i];
            cp->i++;
        }
        ub[ul] = 0;
        u = parse_unit_id_str(ub);
        if (u == CSS_UNIT_NONE)
            return 0;
        out->px_milli = resolve_px_milli(cp->se, num, u, cp->fs, cp->root_fs);
        out->pct_milli = 0;
        out->has_unit = 1;
        return 1;
    }
    out->px_milli = num;
    out->pct_milli = 0;
    out->has_unit = 0;
    return 1;
}

static int cp_eval_factor(struct calc_parser *cp, struct calc_val *out)
{
    cp_ws(cp);
    if (cp->i < cp->len && (cp->s[cp->i] == '+' || cp->s[cp->i] == '-')) {
        int neg = (cp->s[cp->i] == '-');
        cp->i++;
        if (cp->depth > 24)
            return 0;
        cp->depth++;
        if (!cp_eval_factor(cp, out))
            return 0;
        cp->depth--;
        if (neg) {
            out->px_milli = -out->px_milli;
            out->pct_milli = -out->pct_milli;
        }
        return 1;
    }
    return cp_eval_primary(cp, out);
}

static int cp_eval_term(struct calc_parser *cp, struct calc_val *out)
{
    if (!cp_eval_factor(cp, out))
        return 0;
    for (;;) {
        char op;
        struct calc_val rhs;
        cp_ws(cp);
        if (cp->i >= cp->len || (cp->s[cp->i] != '*' && cp->s[cp->i] != '/'))
            break;
        op = cp->s[cp->i++];
        if (!cp_eval_factor(cp, &rhs))
            return 0;
        if (op == '*') {
            if (out->has_unit && rhs.has_unit)
                return 0;
            if (!out->has_unit && rhs.has_unit) {
                int64_t f = out->px_milli;
                out->px_milli = div_round64(rhs.px_milli * f, 1000);
                out->pct_milli = div_round64(rhs.pct_milli * f, 1000);
                out->has_unit = 1;
            } else {
                out->px_milli = div_round64(out->px_milli * rhs.px_milli, 1000);
                out->pct_milli = div_round64(out->pct_milli * rhs.px_milli, 1000);
            }
        } else {
            if (rhs.has_unit || rhs.px_milli == 0)
                return 0;
            out->px_milli = div_round64(out->px_milli * 1000, rhs.px_milli);
            out->pct_milli = div_round64(out->pct_milli * 1000, rhs.px_milli);
        }
    }
    return 1;
}

static int cp_eval_expr(struct calc_parser *cp, struct calc_val *out)
{
    if (!cp_eval_term(cp, out))
        return 0;
    for (;;) {
        char op;
        struct calc_val rhs;
        cp_ws(cp);
        if (cp->i >= cp->len || (cp->s[cp->i] != '+' && cp->s[cp->i] != '-'))
            break;
        op = cp->s[cp->i++];
        if (!cp_eval_term(cp, &rhs))
            return 0;
        if (out->has_unit != rhs.has_unit)
            return 0;
        if (op == '+') {
            out->px_milli += rhs.px_milli;
            out->pct_milli += rhs.pct_milli;
        } else {
            out->px_milli -= rhs.px_milli;
            out->pct_milli -= rhs.pct_milli;
        }
    }
    return 1;
}

static int eval_calc_str(const struct style_engine *se, const char *s, int len,
                         int32_t fs, int32_t root_fs, struct calc_val *out)
{
    struct calc_parser cp;
    cp.se = se;
    cp.s = s;
    cp.len = len;
    cp.i = 0;
    cp.fs = fs;
    cp.root_fs = root_fs;
    cp.depth = 0;
    if (!cp_eval_expr(&cp, out))
        return 0;
    cp_ws(&cp);
    return cp.i == cp.len;
}

static int eval_len_str(const struct style_engine *se, const char *s, int len,
                        int32_t fs, int32_t root_fs, int allow_neg,
                        int allow_pct, int accept_auto, int accept_none,
                        struct css_len *out)
{
    int a, al;
    char tmp[128];
    struct calc_val cv;

    trim_span(s, len, &a, &al);
    if (al <= 0)
        return 0;
    if (al < (int)sizeof(tmp)) {
        memcpy(tmp, s + a, (unsigned long)al);
        tmp[al] = 0;
        if (accept_auto && s_stricmp(tmp, "auto") == 0) {
            *out = len_auto();
            return 1;
        }
        if (accept_none && s_stricmp(tmp, "none") == 0) {
            out->type = CSS_LEN_NONE;
            out->v = 0;
            out->pct = 0;
            return 1;
        }
    }
    if (!eval_calc_str(se, s + a, al, fs, root_fs, &cv))
        return 0;
    if (!cv.has_unit && cv.px_milli != 0)
        return 0;
    if (!allow_pct && cv.pct_milli != 0)
        return 0;
    out->pct = 0;
    if (cv.pct_milli == 0) {
        int32_t px = round_milli(cv.px_milli);
        if (!allow_neg && px < 0)
            px = 0;
        out->type = CSS_LEN_PX;
        out->v = px;
    } else {
        int32_t px = round_milli(cv.px_milli);
        int32_t pct = clamp_pct_milli(cv.pct_milli);
        if (px == 0) {
            if (!allow_neg && pct < 0)
                pct = 0;
            out->type = CSS_LEN_PCT;
            out->v = pct;
        } else {
            out->type = CSS_LEN_CALC;
            out->pct = pct;
            out->v = px;
        }
    }
    return 1;
}

/* ================================================================== *
 * color parsing & light-dark()
 * ================================================================== */

static uint32_t s_hsl_to_rgb(int64_t h, int64_t s, int64_t l, int alpha)
{
    int64_t c, x, m, hp, k, r, g, b;
    int sect;

    h %= 360000;
    if (h < 0) h += 360000;
    if (s < 0) s = 0;
    if (s > 1000) s = 1000;
    if (l < 0) l = 0;
    if (l > 1000) l = 1000;

    c = 2 * l - 1000;
    if (c < 0) c = -c;
    c = (1000 - c) * s / 1000;
    hp = h / 60;
    k = hp % 2000;
    x = k - 1000;
    if (x < 0) x = -x;
    x = c * (1000 - x) / 1000;
    m = l - c / 2;

    sect = (int)(hp / 1000);
    switch (sect) {
    case 0:  r = c; g = x; b = 0; break;
    case 1:  r = x; g = c; b = 0; break;
    case 2:  r = 0; g = c; b = x; break;
    case 3:  r = 0; g = x; b = c; break;
    case 4:  r = x; g = 0; b = c; break;
    default: r = c; g = 0; b = x; break;
    }
    r = ((r + m) * 255 + 500) / 1000;
    g = ((g + m) * 255 + 500) / 1000;
    b = ((b + m) * 255 + 500) / 1000;
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return ((uint32_t)(alpha & 0xFF) << 24) | ((uint32_t)r << 16) |
           ((uint32_t)g << 8) | (uint32_t)b;
}

static int eval_color_str_d(const struct style_engine *se,
                            const struct computed_style *cs,
                            const struct computed_style *parent,
                            const char *s, int len, uint32_t *out,
                            int allow_current, int depth)
{
    int a, al;
    char buf[256];

    if (depth > 8)
        return 0;
    trim_span(s, len, &a, &al);
    if (al <= 0 || al >= (int)sizeof(buf))
        return 0;
    memcpy(buf, s + a, (unsigned long)al);
    buf[al] = 0;

    if (s_stricmp(buf, "currentcolor") == 0) {
        if (allow_current && cs) *out = cs->color;
        else if (parent)         *out = parent->color;
        else                     *out = CSS_RGB(0, 0, 0);
        return 1;
    }

    if (al > 11 && s_stricmp_n(buf, "light-dark(", 11) == 0 &&
        buf[al - 1] == ')') {
        int starts[4], lens[4];
        int na = split_comma_args(buf + 11, al - 12, starts, lens, 4);
        uint32_t c1 = 0, c2 = 0;
        int is_dark;
        if (na != 2)
            return 0;
        if (!eval_color_str_d(se, cs, parent, buf + 11 + starts[0], lens[0],
                              &c1, allow_current, depth + 1))
            return 0;
        if (!eval_color_str_d(se, cs, parent, buf + 11 + starts[1], lens[1],
                              &c2, allow_current, depth + 1))
            return 0;
        is_dark = cs && (cs->color_scheme == CSS_COLORSCHEME_DARK ||
                         (cs->color_scheme == CSS_COLORSCHEME_NORMAL &&
                          se->prefers_dark));
        *out = is_dark ? c2 : c1;
        return 1;
    }

    if (buf[al - 1] == ')' &&
        (s_stricmp_n(buf, "rgb(", 4) == 0 || s_stricmp_n(buf, "rgba(", 5) == 0 ||
         s_stricmp_n(buf, "hsl(", 4) == 0 || s_stricmp_n(buf, "hsla(", 5) == 0)) {
        int is_hsl = (s_lc((unsigned char)buf[0]) == 'h');
        const char *open = strchr(buf, '(');
        int inner_len = (int)(buf + al - 1 - (open + 1));
        const char *p = open + 1;
        int i = 0, n = 0;
        int64_t arg[4] = { 0, 0, 0, 0 };
        int argpct[4] = { 0, 0, 0, 0 };
        int alpha = 255;

        while (i < inner_len && n < 4) {
            while (i < inner_len && (s_is_ws((unsigned char)p[i]) ||
                                     p[i] == ',' || p[i] == '/'))
                i++;
            if (i >= inner_len)
                break;
            if (i + 4 <= inner_len && s_stricmp_n(p + i, "none", 4) == 0 &&
                (i + 4 == inner_len || s_is_ws((unsigned char)p[i + 4]) ||
                 p[i + 4] == ',' || p[i + 4] == '/')) {
                arg[n] = 0; argpct[n] = 0; n++;
                i += 4;
                continue;
            }
            {
                int st = i, pdepth = 0;
                struct calc_val cv;
                while (i < inner_len) {
                    int ch = (unsigned char)p[i];
                    if (ch == '(') pdepth++;
                    else if (ch == ')' && pdepth > 0) pdepth--;
                    else if (pdepth == 0 && (s_is_ws(ch) || ch == ',' || ch == '/'))
                        break;
                    i++;
                }
                if (!eval_calc_str(se, p + st, i - st, 16, 16, &cv))
                    return 0;
                if (cv.pct_milli != 0) {
                    arg[n] = cv.pct_milli;
                    argpct[n] = 1;
                } else if (!cv.has_unit) {
                    arg[n] = cv.px_milli;
                    argpct[n] = 0;
                } else {
                    return 0;
                }
                n++;
            }
        }
        while (i < inner_len && s_is_ws((unsigned char)p[i]))
            i++;
        if (i < inner_len || n < 3)
            return 0;
        if (n >= 4) {
            int64_t alv = arg[3];
            if (argpct[3]) alv = alv * 1000 / 100000;
            alpha = (int)(alv * 255 / 1000);
            if (alpha < 0) alpha = 0;
            if (alpha > 255) alpha = 255;
        }
        if (is_hsl) {
            int64_t h = arg[0];
            int64_t s2 = argpct[1] ? arg[1] / 100 : arg[1];
            int64_t l2 = argpct[2] ? arg[2] / 100 : arg[2];
            *out = s_hsl_to_rgb(h, s2, l2, alpha);
            return 1;
        }
        for (i = 0; i < 3; i++) {
            int64_t c = argpct[i] ? ((arg[i] * 255 + 50000) / 100000)
                                  : ((arg[i] + 500) / 1000);
            if (c < 0) c = 0;
            if (c > 255) c = 255;
            arg[i] = c;
        }
        *out = ((uint32_t)alpha << 24) | ((uint32_t)arg[0] << 16) |
               ((uint32_t)arg[1] << 8) | (uint32_t)arg[2];
        return 1;
    }

    return css_parse_color_string(buf, out);
}

static int eval_color_str(const struct style_engine *se,
                          const struct computed_style *cs,
                          const struct computed_style *parent,
                          const char *s, int len, uint32_t *out,
                          int allow_current)
{
    return eval_color_str_d(se, cs, parent, s, len, out, allow_current, 0);
}

/* ================================================================== *
 * complex value & shorthand parsers (post-var expansion)
 * ================================================================== */

static void apply_box_shadow_str(const struct style_engine *se,
                                 struct computed_style *cs,
                                 const struct computed_style *parent,
                                 const char *s, int len, int32_t root_fs)
{
    int cstarts[4], clens[4], nc;
    int tstarts[8], tlens[8], nt;
    int32_t lens[4] = { 0, 0, 0, 0 };
    int nlen = 0, inset = 0, have_color = 0, i;
    uint32_t col = cs->color;
    int a, al;

    trim_span(s, len, &a, &al);
    if (al <= 0)
        return;
    if (al == 4 && s_stricmp_n(s + a, "none", 4) == 0) {
        cs->has_shadow = 0;
        cs->shadow_inset = 0;
        cs->shadow_x = cs->shadow_y = cs->shadow_blur = cs->shadow_spread = 0;
        cs->shadow_color = 0;
        return;
    }
    nc = split_comma_args(s + a, al, cstarts, clens, 4);
    if (nc <= 0)
        return;
    nt = split_top_tokens(s + a + cstarts[0], clens[0], tstarts, tlens, 8);
    for (i = 0; i < nt; i++) {
        const char *tok = s + a + cstarts[0] + tstarts[i];
        int tl = tlens[i];
        struct css_len l;
        uint32_t c;
        if (tl == 5 && s_stricmp_n(tok, "inset", 5) == 0) {
            if (inset) return;
            inset = 1;
        } else if (eval_len_str(se, tok, tl, cs->font_size, root_fs, 1, 0,
                                0, 0, &l) && l.type == CSS_LEN_PX) {
            if (nlen >= 4) return;
            lens[nlen++] = l.v;
        } else if (!have_color && eval_color_str(se, cs, parent, tok, tl, &c, 1)) {
            col = c;
            have_color = 1;
        } else {
            return;
        }
    }
    if (nlen < 2 || lens[2] < 0)
        return;
    cs->has_shadow = 1;
    cs->shadow_inset = (uint8_t)inset;
    cs->shadow_x = lens[0];
    cs->shadow_y = lens[1];
    cs->shadow_blur = lens[2];
    cs->shadow_spread = lens[3];
    cs->shadow_color = col;
}

static int parse_grad_angle(const char *s, int len, int16_t *out_deg)
{
    int tstarts[4], tlens[4], nt;
    int a, al;

    trim_span(s, len, &a, &al);
    if (al <= 0) return 0;
    s += a; len = al;

    nt = split_top_tokens(s, len, tstarts, tlens, 4);
    if (nt >= 2 && tlens[0] == 2 && s_stricmp_n(s + tstarts[0], "to", 2) == 0) {
        int top = 0, bottom = 0, left = 0, right = 0, i;
        if (nt > 3) return 0;
        for (i = 1; i < nt; i++) {
            const char *w = s + tstarts[i];
            int wl = tlens[i];
            if (wl == 3 && s_stricmp_n(w, "top", 3) == 0)         top = 1;
            else if (wl == 6 && s_stricmp_n(w, "bottom", 6) == 0) bottom = 1;
            else if (wl == 4 && s_stricmp_n(w, "left", 4) == 0)   left = 1;
            else if (wl == 5 && s_stricmp_n(w, "right", 5) == 0)  right = 1;
            else return 0;
        }
        if ((top && bottom) || (left && right)) return 0;
        if (top && right)    { *out_deg = 45; return 1; }
        if (bottom && right) { *out_deg = 135; return 1; }
        if (bottom && left)  { *out_deg = 225; return 1; }
        if (top && left)     { *out_deg = 315; return 1; }
        if (top)             { *out_deg = 0; return 1; }
        if (right)           { *out_deg = 90; return 1; }
        if (bottom)          { *out_deg = 180; return 1; }
        if (left)            { *out_deg = 270; return 1; }
        return 0;
    }
    if (nt == 1) {
        int idx = 0;
        int64_t num = 0;
        int32_t deg = 0;
        const char *u;
        if (!parse_number_milli(s, len, &idx, &num, 0))
            return 0;
        u = s + idx;
        if (len - idx == 3 && s_stricmp_n(u, "deg", 3) == 0)
            deg = round_milli(num);
        else if (len - idx == 4 && s_stricmp_n(u, "turn", 4) == 0)
            deg = round_milli(num * 360);
        else if (len - idx == 4 && s_stricmp_n(u, "grad", 4) == 0)
            deg = round_milli(num * 9 / 10);
        else if (len - idx == 3 && s_stricmp_n(u, "rad", 3) == 0)
            deg = round_milli(num * 57296 / 1000);
        else if (idx == len && num == 0)
            deg = 0;
        else
            return 0;
        deg = ((deg % 360) + 360) % 360;
        *out_deg = (int16_t)deg;
        return 1;
    }
    return 0;
}

static int apply_linear_gradient_str(const struct style_engine *se,
                                     struct computed_style *cs,
                                     const struct computed_style *parent,
                                     const char *s, int len)
{
    int a, al, starts[12], lens[12], nargs, stop_start = 0, i, nstops;
    int16_t angle = 180;
    uint32_t cols[12];
    int16_t pos[12];

    trim_span(s, len, &a, &al);
    if (al < 18 || s_stricmp_n(s + a, "linear-gradient(", 16) != 0 ||
        s[a + al - 1] != ')')
        return 0;
    nargs = split_comma_args(s + a + 16, al - 17, starts, lens, 12);
    if (nargs < 2)
        return 0;
    if (parse_grad_angle(s + a + 16 + starts[0], lens[0], &angle))
        stop_start = 1;
    nstops = nargs - stop_start;
    if (nstops < 2)
        return 0;

    for (i = 0; i < nstops; i++) {
        const char *arg = s + a + 16 + starts[stop_start + i];
        int argl = lens[stop_start + i];
        int ts[3], tl[3], nt;
        nt = split_top_tokens(arg, argl, ts, tl, 3);
        if (nt < 1 || nt > 2)
            return 0;
        if (!eval_color_str(se, cs, parent, arg + ts[0], tl[0], &cols[i], 1))
            return 0;
        if (nt == 2) {
            struct calc_val cv;
            if (!eval_calc_str(se, arg + ts[1], tl[1], cs->font_size, 16, &cv))
                return 0;
            if (cv.pct_milli != 0 || !cv.has_unit) {
                int32_t p = (int32_t)(cv.pct_milli / 100);
                if (!cv.has_unit && cv.px_milli == 0) p = 0;
                else if (!cv.has_unit) return 0;
                if (p < 0) p = 0;
                if (p > 1000) p = 1000;
                pos[i] = (int16_t)p;
            } else {
                pos[i] = (cv.px_milli <= 0) ? 0 : 1000;
            }
        } else {
            pos[i] = -1;
        }
    }

    if (pos[0] < 0) pos[0] = 0;
    if (pos[nstops - 1] < 0) pos[nstops - 1] = 1000;
    for (i = 1; i < nstops; i++) {
        if (pos[i] >= 0) {
            int j;
            for (j = 0; j < i; j++)
                if (pos[j] >= 0 && pos[i] < pos[j])
                    pos[i] = pos[j];
        }
    }
    for (i = 0; i < nstops; ) {
        if (pos[i] >= 0) {
            i++;
            continue;
        }
        {
            int j = i + 1;
            int16_t p0 = pos[i - 1], p1;
            int span, k;
            while (j < nstops && pos[j] < 0)
                j++;
            p1 = pos[j];
            span = j - (i - 1);
            for (k = i; k < j; k++)
                pos[k] = (int16_t)(p0 + (int32_t)(p1 - p0) * (k - (i - 1)) / span);
            i = j + 1;
        }
    }

    if (nstops > CSS_GRAD_MAX_STOPS) {
        cols[CSS_GRAD_MAX_STOPS - 1] = cols[nstops - 1];
        pos[CSS_GRAD_MAX_STOPS - 1] = pos[nstops - 1];
        nstops = CSS_GRAD_MAX_STOPS;
    }
    cs->has_gradient = 1;
    cs->grad_nstops = (uint8_t)nstops;
    cs->grad_angle_deg = angle;
    for (i = 0; i < nstops; i++) {
        cs->grad_colors[i] = cols[i];
        cs->grad_pos[i] = pos[i];
    }
    cs->background_image = 0;
    return 1;
}

static int parse_single_grid_track(const struct style_engine *se,
                                   const char *s, int len, int32_t fs,
                                   int32_t root_fs, struct css_grid_track *out)
{
    int a, al;
    trim_span(s, len, &a, &al);
    if (al <= 0)
        return 0;
    s += a; len = al;

    if ((len == 4 && s_stricmp_n(s, "auto", 4) == 0) ||
        (len == 11 && (s_stricmp_n(s, "min-content", 11) == 0 ||
                       s_stricmp_n(s, "max-content", 11) == 0))) {
        out->kind = CSS_GRID_AUTO;
        out->val = 0;
        return 1;
    }
    if (len > 2 && s_stricmp_n(s + len - 2, "fr", 2) == 0) {
        int idx = 0;
        int64_t num = 0;
        if (!parse_number_milli(s, len - 2, &idx, &num, 0) || idx != len - 2 ||
            num < 0)
            return 0;
        out->kind = CSS_GRID_FR;
        out->val = (int32_t)num;
        return 1;
    }
    if (len > 7 && s_stricmp_n(s, "minmax(", 7) == 0 && s[len - 1] == ')') {
        int starts[3], lens[3];
        int na = split_comma_args(s + 7, len - 8, starts, lens, 3);
        if (na != 2)
            return 0;
        if (parse_single_grid_track(se, s + 7 + starts[1], lens[1], fs,
                                    root_fs, out))
            return 1;
        return parse_single_grid_track(se, s + 7 + starts[0], lens[0], fs,
                                       root_fs, out);
    }
    {
        struct css_len l;
        if (!eval_len_str(se, s, len, fs, root_fs, 0, 1, 0, 0, &l))
            return 0;
        if (l.type == CSS_LEN_PCT) {
            out->kind = CSS_GRID_PCT;
            out->val = l.v;
            return 1;
        }
        if (l.type == CSS_LEN_PX) {
            out->kind = CSS_GRID_PX;
            out->val = l.v;
            return 1;
        }
    }
    return 0;
}

static int parse_grid_tracks_list(const struct style_engine *se,
                                  const char *s, int len, int32_t fs,
                                  int32_t root_fs, struct css_grid_track *out,
                                  int *count, int max_cols, int depth)
{
    int ts[24], tl[24], nt, i;

    if (depth > 4)
        return 0;
    nt = split_top_tokens(s, len, ts, tl, 24);
    if (nt <= 0)
        return 0;
    for (i = 0; i < nt; i++) {
        const char *tok = s + ts[i];
        int len_i = tl[i];
        if (len_i >= 2 && tok[0] == '[' && tok[len_i - 1] == ']')
            continue;
        if (len_i > 7 && s_stricmp_n(tok, "repeat(", 7) == 0 &&
            tok[len_i - 1] == ')') {
            int starts[3], lens[3], na, rep = 1;
            struct css_grid_track sub[CSS_GRID_MAX_COLS];
            int subn = 0, r, j;

            na = split_comma_args(tok + 7, len_i - 8, starts, lens, 3);
            if (na != 2)
                return 0;
            {
                const char *cnt_s = tok + 7 + starts[0];
                int cnt_l = lens[0], idx = 0, is_int = 0;
                int64_t num = 0;
                if ((cnt_l == 9 && s_stricmp_n(cnt_s, "auto-fill", 9) == 0) ||
                    (cnt_l == 8 && s_stricmp_n(cnt_s, "auto-fit", 8) == 0)) {
                    rep = 1;
                } else {
                    if (!parse_number_milli(cnt_s, cnt_l, &idx, &num, &is_int) ||
                        idx != cnt_l || !is_int || num <= 0)
                        return 0;
                    rep = (int)(num / 1000);
                    if (rep > max_cols) rep = max_cols;
                }
            }
            if (!parse_grid_tracks_list(se, tok + 7 + starts[1], lens[1], fs,
                                        root_fs, sub, &subn,
                                        CSS_GRID_MAX_COLS, depth + 1) ||
                subn <= 0)
                return 0;
            for (r = 0; r < rep && *count < max_cols; r++) {
                for (j = 0; j < subn && *count < max_cols; j++)
                    out[(*count)++] = sub[j];
            }
            continue;
        }
        if (*count < max_cols) {
            struct css_grid_track tr;
            if (!parse_single_grid_track(se, tok, len_i, fs, root_fs, &tr))
                return 0;
            out[(*count)++] = tr;
        } else {
            struct css_grid_track tr;
            if (!parse_single_grid_track(se, tok, len_i, fs, root_fs, &tr))
                return 0;
        }
    }
    return 1;
}

static void apply_grid_tracks_str(const struct style_engine *se,
                                  const char *s, int len, int32_t fs,
                                  int32_t root_fs, struct css_grid_track *dst,
                                  uint8_t *dst_n, int max_tracks)
{
    int a, al, count = 0;
    struct css_grid_track tracks[CSS_GRID_MAX_COLS];

    if (max_tracks > CSS_GRID_MAX_COLS)
        max_tracks = CSS_GRID_MAX_COLS;
    trim_span(s, len, &a, &al);
    if (al <= 0)
        return;
    if (al == 4 && s_stricmp_n(s + a, "none", 4) == 0) {
        *dst_n = 0;
        return;
    }
    if (!parse_grid_tracks_list(se, s + a, al, fs, root_fs,
                                tracks, &count, max_tracks, 0))
        return;
    *dst_n = (uint8_t)count;
    for (a = 0; a < count; a++)
        dst[a] = tracks[a];
}

static int apply_aspect_ratio_str(const struct style_engine *se,
                                  struct computed_style *cs, const char *s,
                                  int len, int32_t root_fs)
{
    int a, al, i, depth = 0, slash = -1;
    struct calc_val cv, cv2;

    trim_span(s, len, &a, &al);
    if (al <= 0)
        return 0;
    s += a;
    len = al;
    if (len == 4 && s_stricmp_n(s, "auto", 4) == 0) {
        cs->aspect_ratio = 0;
        return 1;
    }
    for (i = 0; i < len; i++) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')' && depth > 0) depth--;
        else if (depth == 0 && s[i] == '/') {
            if (slash >= 0)
                return 0;
            slash = i;
        }
    }
    if (slash < 0) {
        int64_t r;
        if (!eval_calc_str(se, s, len, cs->font_size, root_fs, &cv) ||
            cv.has_unit || cv.pct_milli != 0 || cv.px_milli <= 0)
            return 0;
        r = cv.px_milli;
        if (r > 100000000LL) r = 100000000LL;
        cs->aspect_ratio = (int32_t)r;
        return 1;
    }
    if (!eval_calc_str(se, s, slash, cs->font_size, root_fs, &cv) ||
        cv.has_unit || cv.pct_milli != 0 || cv.px_milli <= 0)
        return 0;
    if (!eval_calc_str(se, s + slash + 1, len - slash - 1, cs->font_size,
                       root_fs, &cv2) ||
        cv2.has_unit || cv2.pct_milli != 0 || cv2.px_milli <= 0)
        return 0;
    {
        int64_t r = div_round64(cv.px_milli * 1000LL, cv2.px_milli);
        if (r < 1) r = 1;
        if (r > 100000000LL) r = 100000000LL;
        cs->aspect_ratio = (int32_t)r;
    }
    return 1;
}

static int eval_scale_arg(const struct style_engine *se, const char *s, int len,
                          int32_t fs, int32_t root_fs, int64_t *out_milli)
{
    struct calc_val cv;
    if (!eval_calc_str(se, s, len, fs, root_fs, &cv))
        return 0;
    if (!cv.has_unit && cv.pct_milli == 0) {
        *out_milli = cv.px_milli;
        return 1;
    }
    if (cv.pct_milli != 0 && cv.px_milli == 0) {
        *out_milli = cv.pct_milli / 100;
        return 1;
    }
    return 0;
}

static void unpack_css_len(struct css_len l, int64_t *px, int64_t *pct)
{
    if (l.type == CSS_LEN_PCT) {
        *px = 0;
        *pct = l.v;
    } else if (l.type == CSS_LEN_CALC) {
        *px = l.v;
        *pct = l.pct;
    } else {
        *px = l.v;
        *pct = 0;
    }
}

static struct css_len pack_css_len(int64_t px, int64_t pct)
{
    struct css_len l;
    if (pct == 0) {
        l.type = CSS_LEN_PX;
        l.v = clamp_px(px);
        l.pct = 0;
    } else if (px == 0) {
        l.type = CSS_LEN_PCT;
        l.v = clamp_pct_milli(pct);
        l.pct = 0;
    } else {
        l.type = CSS_LEN_CALC;
        l.v = clamp_px(px);
        l.pct = clamp_pct_milli(pct);
    }
    return l;
}

static int apply_transform_str(const struct style_engine *se,
                               struct computed_style *cs, const char *s,
                               int len, int32_t root_fs)
{
    int a, al, i = 0;
    int64_t tx_px = 0, tx_pct = 0, ty_px = 0, ty_pct = 0;
    int64_t sx = 1000, sy = 1000;
    int nfn = 0;

    trim_span(s, len, &a, &al);
    if (al <= 0)
        return 0;
    s += a;
    len = al;
    if (len == 4 && s_stricmp_n(s, "none", 4) == 0) {
        cs->has_transform = 0;
        cs->translate_x = len_px(0);
        cs->translate_y = len_px(0);
        cs->scale_x = 1000;
        cs->scale_y = 1000;
        return 1;
    }
    while (i < len) {
        int nst, nlen, ast, alen, depth, na;
        int starts[4], lens[4];
        struct css_len dx, dy, dz;
        int64_t dpx, dpct, qpx, qpct, vx, vy;
        const char *fn, *arg;

        while (i < len && s_is_ws((unsigned char)s[i]))
            i++;
        if (i >= len)
            break;
        nst = i;
        while (i < len && s[i] != '(' && !s_is_ws((unsigned char)s[i]))
            i++;
        nlen = i - nst;
        if (nlen <= 0 || i >= len || s[i] != '(')
            return 0;
        i++;
        ast = i;
        depth = 1;
        while (i < len && depth > 0) {
            int ch = (unsigned char)s[i];
            if (ch == '"' || ch == '\'') {
                int q = ch;
                i++;
                while (i < len) {
                    if (s[i] == '\\' && i + 1 < len) { i += 2; continue; }
                    if (s[i] == q) { i++; break; }
                    i++;
                }
                continue;
            }
            if (ch == '(') depth++;
            else if (ch == ')') depth--;
            i++;
        }
        if (depth != 0)
            return 0;
        alen = (i - 1) - ast;
        fn = s + nst;
        arg = s + ast;
        na = split_comma_args(arg, alen, starts, lens, 4);

        if (nlen == 9 && s_stricmp_n(fn, "translate", 9) == 0) {
            if (na < 1 || na > 2) return 0;
            if (!eval_len_str(se, arg + starts[0], lens[0], cs->font_size,
                              root_fs, 1, 1, 0, 0, &dx))
                return 0;
            if (na == 2) {
                if (!eval_len_str(se, arg + starts[1], lens[1], cs->font_size,
                                  root_fs, 1, 1, 0, 0, &dy))
                    return 0;
            } else {
                dy = len_px(0);
            }
        } else if (nlen == 10 && s_stricmp_n(fn, "translatex", 10) == 0) {
            if (na != 1) return 0;
            if (!eval_len_str(se, arg + starts[0], lens[0], cs->font_size,
                              root_fs, 1, 1, 0, 0, &dx))
                return 0;
            dy = len_px(0);
        } else if (nlen == 10 && s_stricmp_n(fn, "translatey", 10) == 0) {
            if (na != 1) return 0;
            dx = len_px(0);
            if (!eval_len_str(se, arg + starts[0], lens[0], cs->font_size,
                              root_fs, 1, 1, 0, 0, &dy))
                return 0;
        } else if (nlen == 11 && s_stricmp_n(fn, "translate3d", 11) == 0) {
            if (na != 3) return 0;
            if (!eval_len_str(se, arg + starts[0], lens[0], cs->font_size,
                              root_fs, 1, 1, 0, 0, &dx) ||
                !eval_len_str(se, arg + starts[1], lens[1], cs->font_size,
                              root_fs, 1, 1, 0, 0, &dy) ||
                !eval_len_str(se, arg + starts[2], lens[2], cs->font_size,
                              root_fs, 1, 1, 0, 0, &dz))
                return 0;
        } else if (nlen == 5 && s_stricmp_n(fn, "scale", 5) == 0) {
            if (na < 1 || na > 2) return 0;
            if (!eval_scale_arg(se, arg + starts[0], lens[0], cs->font_size,
                                root_fs, &vx))
                return 0;
            if (na == 2) {
                if (!eval_scale_arg(se, arg + starts[1], lens[1], cs->font_size,
                                    root_fs, &vy))
                    return 0;
            } else {
                vy = vx;
            }
            sx = div_round64(sx * vx, 1000);
            sy = div_round64(sy * vy, 1000);
            nfn++;
            continue;
        } else if (nlen == 6 && s_stricmp_n(fn, "scalex", 6) == 0) {
            if (na != 1 || !eval_scale_arg(se, arg + starts[0], lens[0],
                                           cs->font_size, root_fs, &vx))
                return 0;
            sx = div_round64(sx * vx, 1000);
            nfn++;
            continue;
        } else if (nlen == 6 && s_stricmp_n(fn, "scaley", 6) == 0) {
            if (na != 1 || !eval_scale_arg(se, arg + starts[0], lens[0],
                                           cs->font_size, root_fs, &vy))
                return 0;
            sy = div_round64(sy * vy, 1000);
            nfn++;
            continue;
        } else {
            return 0;
        }

        unpack_css_len(dx, &dpx, &dpct);
        unpack_css_len(dy, &qpx, &qpct);
        tx_px += div_round64(sx * dpx, 1000);
        tx_pct += div_round64(sx * dpct, 1000);
        ty_px += div_round64(sy * qpx, 1000);
        ty_pct += div_round64(sy * qpct, 1000);
        nfn++;
    }
    if (nfn <= 0)
        return 0;
    if (sx < -10000000LL) sx = -10000000LL;
    if (sx >  10000000LL) sx =  10000000LL;
    if (sy < -10000000LL) sy = -10000000LL;
    if (sy >  10000000LL) sy =  10000000LL;
    cs->has_transform = 1;
    cs->translate_x = pack_css_len(tx_px, tx_pct);
    cs->translate_y = pack_css_len(ty_px, ty_pct);
    cs->scale_x = (int32_t)sx;
    cs->scale_y = (int32_t)sy;
    return 1;
}

static int parse_grid_line_str(const struct style_engine *se,
                               const char *s, int len,
                               int *is_auto, int *is_span, int32_t *val)
{
    int ts[3], tl[3], nt;

    *is_auto = 0;
    *is_span = 0;
    *val = 0;
    nt = split_top_tokens(s, len, ts, tl, 3);
    if (nt <= 0 || nt > 2)
        return 0;
    if (nt == 1) {
        const char *tok = s + ts[0];
        int l = tl[0];
        struct calc_val cv;
        if (l == 4 && s_stricmp_n(tok, "auto", 4) == 0) {
            *is_auto = 1;
            return 1;
        }
        if (l == 4 && s_stricmp_n(tok, "span", 4) == 0) {
            *is_span = 1;
            *val = 1;
            return 1;
        }
        if (eval_calc_str(se, tok, l, 16, 16, &cv) &&
            !cv.has_unit && cv.pct_milli == 0 && (cv.px_milli % 1000) == 0 &&
            cv.px_milli != 0) {
            *val = (int32_t)(cv.px_milli / 1000);
            return 1;
        }
        return 0;
    }
    {
        const char *n_tok = 0;
        int n_len = 0;
        struct calc_val cv;
        if (tl[0] == 4 && s_stricmp_n(s + ts[0], "span", 4) == 0) {
            n_tok = s + ts[1]; n_len = tl[1];
        } else if (tl[1] == 4 && s_stricmp_n(s + ts[1], "span", 4) == 0) {
            n_tok = s + ts[0]; n_len = tl[0];
        } else {
            return 0;
        }
        if (!eval_calc_str(se, n_tok, n_len, 16, 16, &cv) ||
            cv.has_unit || cv.pct_milli != 0 || (cv.px_milli % 1000) != 0 ||
            cv.px_milli <= 0)
            return 0;
        *is_span = 1;
        *val = (int32_t)(cv.px_milli / 1000);
        if (*val < 1) *val = 1;
        if (*val > 255) *val = 255;
        return 1;
    }
}

static void apply_grid_line_val(const struct style_engine *se,
                                struct computed_style *cs, int prop,
                                const char *s, int len)
{
    int is_auto = 0, is_span = 0;
    int32_t v = 0;
    int is_col = (prop == CSS_PROP_GRID_COL_START || prop == CSS_PROP_GRID_COL_END);
    int is_start = (prop == CSS_PROP_GRID_COL_START || prop == CSS_PROP_GRID_ROW_START);

    if (!parse_grid_line_str(se, s, len, &is_auto, &is_span, &v))
        return;
    if (is_col) {
        if (is_span) {
            if (is_start) cs->grid_col_start = 0;
            else          cs->grid_col_end = 0;
            cs->grid_col_span = (uint8_t)v;
        } else if (is_auto) {
            if (is_start) cs->grid_col_start = 0;
            else          cs->grid_col_end = 0;
        } else {
            int16_t line = (int16_t)clamp_px(v);
            if (is_start) cs->grid_col_start = line;
            else          cs->grid_col_end = line;
        }
    } else {
        if (is_span) {
            if (is_start) cs->grid_row_start = 0;
            else          cs->grid_row_end = 0;
            cs->grid_row_span = (uint8_t)v;
        } else if (is_auto) {
            if (is_start) cs->grid_row_start = 0;
            else          cs->grid_row_end = 0;
        } else {
            int16_t line = (int16_t)clamp_px(v);
            if (is_start) cs->grid_row_start = line;
            else          cs->grid_row_end = line;
        }
    }
}

static int eval_font_family_str(struct style_engine *se, const char *s, int len,
                                uint8_t *out_cls, const char **out_name)
{
    int starts[16], lens[16], n, i;
    int cls = -1;
    const char *first_name = 0;

    static const struct { const char *n; int c; } fam[] = {
        { "arial",           CSS_FONTFAMILY_SANS },
        { "helvetica",       CSS_FONTFAMILY_SANS },
        { "verdana",         CSS_FONTFAMILY_SANS },
        { "tahoma",          CSS_FONTFAMILY_SANS },
        { "segoe ui",        CSS_FONTFAMILY_SANS },
        { "roboto",          CSS_FONTFAMILY_SANS },
        { "ui-sans-serif",   CSS_FONTFAMILY_SANS },
        { "-apple-system",   CSS_FONTFAMILY_SANS },
        { "times",           CSS_FONTFAMILY_SERIF },
        { "times new roman", CSS_FONTFAMILY_SERIF },
        { "georgia",         CSS_FONTFAMILY_SERIF },
        { "garamond",        CSS_FONTFAMILY_SERIF },
        { "ui-serif",        CSS_FONTFAMILY_SERIF },
        { "courier",         CSS_FONTFAMILY_MONO },
        { "courier new",     CSS_FONTFAMILY_MONO },
        { "consolas",        CSS_FONTFAMILY_MONO },
        { "menlo",           CSS_FONTFAMILY_MONO },
        { "monaco",          CSS_FONTFAMILY_MONO },
        { "ui-monospace",    CSS_FONTFAMILY_MONO }
    };

    n = split_comma_args(s, len, starts, lens, 16);
    for (i = 0; i < n; i++) {
        const char *f = s + starts[i];
        int fl = lens[i], k;
        char buf[128];
        unsigned j;
        if (fl <= 0)
            return 0;
        if (fl >= 2 && ((f[0] == '"' && f[fl - 1] == '"') ||
                        (f[0] == '\'' && f[fl - 1] == '\''))) {
            f++;
            fl -= 2;
        }
        if (fl <= 0 || fl >= (int)sizeof(buf))
            continue;
        memcpy(buf, f, (unsigned long)fl);
        buf[fl] = 0;
        if (!first_name)
            first_name = se_intern_str(se, buf, fl);
        k = kw_lookup(kw_fontfamily, buf);
        if (k >= 0 && cls < 0)
            cls = k;
        if (cls < 0) {
            for (j = 0; j < sizeof(fam) / sizeof(fam[0]); j++)
                if (s_stricmp(buf, fam[j].n) == 0) {
                    cls = fam[j].c;
                    break;
                }
        }
    }
    if (!first_name)
        return 0;
    *out_cls = (uint8_t)(cls < 0 ? CSS_FONTFAMILY_SERIF : cls);
    *out_name = first_name;
    return 1;
}

static int parse_url_str(struct style_engine *se, const char *s, int len,
                         const char **out_url)
{
    int a, al;
    trim_span(s, len, &a, &al);
    if (al < 5 || s_stricmp_n(s + a, "url(", 4) != 0 || s[a + al - 1] != ')')
        return 0;
    s += a + 4;
    al -= 5;
    trim_span(s, al, &a, &al);
    s += a;
    if (al >= 2 && ((s[0] == '"' && s[al - 1] == '"') ||
                    (s[0] == '\'' && s[al - 1] == '\''))) {
        s++;
        al -= 2;
    }
    *out_url = se_intern_str(se, s, al);
    return *out_url != 0;
}

static int apply_direct_raw_prop(struct style_engine *se,
                                 struct computed_style *cs,
                                 const struct computed_style *parent,
                                 int prop, const char *s, int len,
                                 int32_t root_fs)
{
    int a, al;
    char buf[256];
    struct calc_val cv;
    struct css_len l;
    uint32_t c;

    trim_span(s, len, &a, &al);
    if (al <= 0)
        return 0;
    s += a;
    len = al;
    if (len < (int)sizeof(buf)) {
        memcpy(buf, s, (unsigned long)len);
        buf[len] = 0;
    } else {
        buf[0] = 0;
    }

    switch (prop) {
    case CSS_PROP_DISPLAY:
    case CSS_PROP_POSITION:
    case CSS_PROP_FLOAT:
    case CSS_PROP_CLEAR:
    case CSS_PROP_BOX_SIZING:
    case CSS_PROP_TEXT_ALIGN:
    case CSS_PROP_WHITE_SPACE:
    case CSS_PROP_OVERFLOW:
    case CSS_PROP_VISIBILITY:
    case CSS_PROP_FONT_STYLE:
    case CSS_PROP_LIST_STYLE_TYPE:
    case CSS_PROP_LIST_STYLE_POSITION:
    case CSS_PROP_BORDER_COLLAPSE:
    case CSS_PROP_TEXT_TRANSFORM:
    case CSS_PROP_FLEX_DIRECTION:
    case CSS_PROP_FLEX_WRAP:
    case CSS_PROP_JUSTIFY_CONTENT:
    case CSS_PROP_ALIGN_ITEMS:
    case CSS_PROP_ALIGN_SELF:
    case CSS_PROP_OBJECT_FIT:
    case CSS_PROP_TEXT_OVERFLOW:
    case CSS_PROP_WORD_BREAK:
    case CSS_PROP_OVERFLOW_WRAP:
    case CSS_PROP_TABLE_LAYOUT: {
        const char *const *tbl = 0;
        int k = -1;
        if (!buf[0]) return 0;
        switch (prop) {
        case CSS_PROP_DISPLAY:        tbl = kw_display; break;
        case CSS_PROP_POSITION:       tbl = kw_position; break;
        case CSS_PROP_FLOAT:          tbl = kw_float; break;
        case CSS_PROP_CLEAR:          tbl = kw_clear; break;
        case CSS_PROP_BOX_SIZING:     tbl = kw_boxsizing; break;
        case CSS_PROP_TEXT_ALIGN:     tbl = kw_textalign; break;
        case CSS_PROP_WHITE_SPACE:    tbl = kw_whitespace; break;
        case CSS_PROP_OVERFLOW:       tbl = kw_overflow; break;
        case CSS_PROP_VISIBILITY:     tbl = kw_visibility; break;
        case CSS_PROP_FONT_STYLE:     tbl = kw_fontstyle; break;
        case CSS_PROP_LIST_STYLE_TYPE: tbl = kw_liststyle; break;
        case CSS_PROP_LIST_STYLE_POSITION: tbl = kw_listpos; break;
        case CSS_PROP_BORDER_COLLAPSE: tbl = kw_bcollapse; break;
        case CSS_PROP_TEXT_TRANSFORM: tbl = kw_ttransform; break;
        case CSS_PROP_FLEX_DIRECTION: tbl = kw_flexdir; break;
        case CSS_PROP_FLEX_WRAP:      tbl = kw_flexwrap; break;
        case CSS_PROP_JUSTIFY_CONTENT:
            if (s_stricmp(buf, "start") == 0) k = CSS_JUSTIFY_START;
            else if (s_stricmp(buf, "end") == 0) k = CSS_JUSTIFY_END;
            else tbl = kw_justify;
            break;
        case CSS_PROP_ALIGN_ITEMS:
            if (s_stricmp(buf, "start") == 0) k = CSS_ALIGN_START;
            else if (s_stricmp(buf, "end") == 0) k = CSS_ALIGN_END;
            else tbl = kw_align;
            break;
        case CSS_PROP_ALIGN_SELF:
            if (s_stricmp(buf, "start") == 0) k = CSS_ALIGNSELF_START;
            else if (s_stricmp(buf, "end") == 0) k = CSS_ALIGNSELF_END;
            else tbl = kw_alignself;
            break;
        case CSS_PROP_OBJECT_FIT:     tbl = kw_objectfit; break;
        case CSS_PROP_TEXT_OVERFLOW:  tbl = kw_textoverflow; break;
        case CSS_PROP_WORD_BREAK:     tbl = kw_wordbreak; break;
        case CSS_PROP_OVERFLOW_WRAP:  tbl = kw_overflowwrap; break;
        case CSS_PROP_TABLE_LAYOUT:   tbl = kw_tablelayout; break;
        default: break;
        }
        if (k < 0 && tbl)
            k = kw_lookup(tbl, buf);
        if (k < 0) return 0;
        switch (prop) {
        case CSS_PROP_DISPLAY:        cs->display = (uint8_t)k; break;
        case CSS_PROP_POSITION:       cs->position = (uint8_t)k; break;
        case CSS_PROP_FLOAT:          cs->css_float = (uint8_t)k; break;
        case CSS_PROP_CLEAR:          cs->clear = (uint8_t)k; break;
        case CSS_PROP_BOX_SIZING:     cs->box_sizing = (uint8_t)k; break;
        case CSS_PROP_TEXT_ALIGN:     cs->text_align = (uint8_t)k; break;
        case CSS_PROP_WHITE_SPACE:    cs->white_space = (uint8_t)k; break;
        case CSS_PROP_OVERFLOW:       cs->overflow = (uint8_t)k; break;
        case CSS_PROP_VISIBILITY:     cs->visibility = (uint8_t)k; break;
        case CSS_PROP_FONT_STYLE:     cs->font_style = (uint8_t)k; break;
        case CSS_PROP_LIST_STYLE_TYPE: cs->list_style_type = (uint8_t)k; break;
        case CSS_PROP_LIST_STYLE_POSITION: cs->list_style_position = (uint8_t)k; break;
        case CSS_PROP_BORDER_COLLAPSE: cs->border_collapse = (uint8_t)k; break;
        case CSS_PROP_TEXT_TRANSFORM: cs->text_transform = (uint8_t)k; break;
        case CSS_PROP_FLEX_DIRECTION: cs->flex_direction = (uint8_t)k; break;
        case CSS_PROP_FLEX_WRAP:      cs->flex_wrap = (uint8_t)k; break;
        case CSS_PROP_JUSTIFY_CONTENT: cs->justify_content = (uint8_t)k; break;
        case CSS_PROP_ALIGN_ITEMS:    cs->align_items = (uint8_t)k; break;
        case CSS_PROP_ALIGN_SELF:     cs->align_self = (uint8_t)k; break;
        case CSS_PROP_OBJECT_FIT:     cs->object_fit = (uint8_t)k; break;
        case CSS_PROP_TEXT_OVERFLOW:  cs->text_overflow = (uint8_t)k; break;
        case CSS_PROP_WORD_BREAK:     cs->word_break = (uint8_t)k; break;
        case CSS_PROP_OVERFLOW_WRAP:  cs->overflow_wrap = (uint8_t)k; break;
        case CSS_PROP_TABLE_LAYOUT:   cs->table_layout = (uint8_t)k; break;
        default: break;
        }
        return 1;
    }

    case CSS_PROP_COLOR_SCHEME: {
        int ts[3], tl[3], nt = split_top_tokens(s, len, ts, tl, 3);
        if (nt == 1 && tl[0] < 32) {
            char tbuf[32];
            int k;
            memcpy(tbuf, s + ts[0], (unsigned long)tl[0]);
            tbuf[tl[0]] = 0;
            k = kw_lookup(kw_colorscheme, tbuf);
            if (k >= 0) { cs->color_scheme = (uint8_t)k; return 1; }
        } else if (nt == 2) {
            if ((tl[0] == 5 && s_stricmp_n(s + ts[0], "light", 5) == 0 &&
                 tl[1] == 4 && s_stricmp_n(s + ts[1], "dark", 4) == 0) ||
                (tl[0] == 4 && s_stricmp_n(s + ts[0], "dark", 4) == 0 &&
                 tl[1] == 5 && s_stricmp_n(s + ts[1], "light", 5) == 0)) {
                cs->color_scheme = CSS_COLORSCHEME_NORMAL;
                return 1;
            }
        }
        return 0;
    }

    case CSS_PROP_WIDTH:
        if (!eval_len_str(se, s, len, cs->font_size, root_fs, 0, 1, 1, 0, &l)) return 0;
        cs->width = l; return 1;
    case CSS_PROP_HEIGHT:
        if (!eval_len_str(se, s, len, cs->font_size, root_fs, 0, 1, 1, 0, &l)) return 0;
        cs->height = l; return 1;
    case CSS_PROP_MIN_WIDTH:
        if (!eval_len_str(se, s, len, cs->font_size, root_fs, 0, 1, 1, 0, &l)) return 0;
        cs->min_width = l; return 1;
    case CSS_PROP_MAX_WIDTH:
        if (!eval_len_str(se, s, len, cs->font_size, root_fs, 0, 1, 0, 1, &l)) return 0;
        cs->max_width = l; return 1;
    case CSS_PROP_MIN_HEIGHT:
        if (!eval_len_str(se, s, len, cs->font_size, root_fs, 0, 1, 1, 0, &l)) return 0;
        cs->min_height = l; return 1;
    case CSS_PROP_MAX_HEIGHT:
        if (!eval_len_str(se, s, len, cs->font_size, root_fs, 0, 1, 0, 1, &l)) return 0;
        cs->max_height = l; return 1;
    case CSS_PROP_FLEX_BASIS:
        if (!eval_len_str(se, s, len, cs->font_size, root_fs, 0, 1, 1, 0, &l)) return 0;
        cs->flex_basis = l; return 1;

    case CSS_PROP_TOP:
    case CSS_PROP_RIGHT:
    case CSS_PROP_BOTTOM:
    case CSS_PROP_LEFT:
        if (!eval_len_str(se, s, len, cs->font_size, root_fs, 1, 1, 1, 0, &l)) return 0;
        cs->offset[prop - CSS_PROP_TOP] = l; return 1;

    case CSS_PROP_MARGIN_TOP:
    case CSS_PROP_MARGIN_RIGHT:
    case CSS_PROP_MARGIN_BOTTOM:
    case CSS_PROP_MARGIN_LEFT:
        if (!eval_len_str(se, s, len, cs->font_size, root_fs, 1, 1, 1, 0, &l)) return 0;
        cs->margin[prop - CSS_PROP_MARGIN_TOP] = l; return 1;

    case CSS_PROP_PADDING_TOP:
    case CSS_PROP_PADDING_RIGHT:
    case CSS_PROP_PADDING_BOTTOM:
    case CSS_PROP_PADDING_LEFT:
        if (!eval_len_str(se, s, len, cs->font_size, root_fs, 0, 1, 0, 0, &l)) return 0;
        cs->padding[prop - CSS_PROP_PADDING_TOP] = l; return 1;

    case CSS_PROP_TEXT_INDENT:
        if (!eval_len_str(se, s, len, cs->font_size, root_fs, 1, 1, 0, 0, &l)) return 0;
        cs->text_indent = l; return 1;

    case CSS_PROP_BORDER_TOP_WIDTH:
    case CSS_PROP_BORDER_RIGHT_WIDTH:
    case CSS_PROP_BORDER_BOTTOM_WIDTH:
    case CSS_PROP_BORDER_LEFT_WIDTH: {
        int side = prop - CSS_PROP_BORDER_TOP_WIDTH;
        if (buf[0]) {
            int k = kw_lookup(kw_borderwidth, buf);
            if (k >= 0) {
                static const int32_t bw[3] = { 1, 3, 5 };
                cs->border_width[side] = bw[k];
                return 1;
            }
        }
        if (!eval_len_str(se, s, len, cs->font_size, root_fs, 0, 0, 0, 0, &l))
            return 0;
        cs->border_width[side] = l.v;
        return 1;
    }

    case CSS_PROP_BORDER_TOP_STYLE:
    case CSS_PROP_BORDER_RIGHT_STYLE:
    case CSS_PROP_BORDER_BOTTOM_STYLE:
    case CSS_PROP_BORDER_LEFT_STYLE: {
        int k = buf[0] ? kw_lookup(kw_borderstyle, buf) : -1;
        if (k < 0) return 0;
        cs->border_style[prop - CSS_PROP_BORDER_TOP_STYLE] = (uint8_t)k;
        return 1;
    }

    case CSS_PROP_BORDER_TOP_COLOR:
    case CSS_PROP_BORDER_RIGHT_COLOR:
    case CSS_PROP_BORDER_BOTTOM_COLOR:
    case CSS_PROP_BORDER_LEFT_COLOR:
        if (!eval_color_str(se, cs, parent, s, len, &c, 1)) return 0;
        cs->border_color[prop - CSS_PROP_BORDER_TOP_COLOR] = c;
        return 1;

    case CSS_PROP_BORDER_TL_RADIUS:
    case CSS_PROP_BORDER_TR_RADIUS:
    case CSS_PROP_BORDER_BR_RADIUS:
    case CSS_PROP_BORDER_BL_RADIUS: {
        int corner = prop - CSS_PROP_BORDER_TL_RADIUS;
        if (!eval_len_str(se, s, len, cs->font_size, root_fs, 0, 1, 0, 0, &l))
            return 0;
        if (l.type == CSS_LEN_PX) {
            cs->border_radius[corner] = l.v;
        } else {
            int32_t ref = 0, pct = (l.type == CSS_LEN_PCT) ? l.v : l.pct;
            int32_t extra = (l.type == CSS_LEN_CALC) ? l.v : 0;
            int32_t px;
            if (cs->width.type == CSS_LEN_PX && cs->height.type == CSS_LEN_PX)
                ref = (cs->width.v < cs->height.v) ? cs->width.v : cs->height.v;
            else if (cs->width.type == CSS_LEN_PX)
                ref = cs->width.v;
            else if (cs->height.type == CSS_LEN_PX)
                ref = cs->height.v;
            px = clamp_px((int64_t)ref * pct / 100000 + extra);
            cs->border_radius[corner] = px > 0 ? px : 0;
        }
        return 1;
    }

    case CSS_PROP_COLOR:
        if (!eval_color_str(se, cs, parent, s, len, &c, 0)) return 0;
        cs->color = c;
        return 1;

    case CSS_PROP_BACKGROUND_COLOR:
        if (!eval_color_str(se, cs, parent, s, len, &c, 1)) return 0;
        cs->background_color = c;
        return 1;

    case CSS_PROP_BACKGROUND_IMAGE: {
        const char *u;
        if (buf[0] && s_stricmp(buf, "none") == 0) {
            cs->background_image = 0;
            cs->has_gradient = 0;
            cs->grad_nstops = 0;
            return 1;
        }
        if (apply_linear_gradient_str(se, cs, parent, s, len))
            return 1;
        if (parse_url_str(se, s, len, &u)) {
            cs->background_image = u;
            cs->has_gradient = 0;
            cs->grad_nstops = 0;
            return 1;
        }
        return 0;
    }

    case CSS_PROP_OPACITY: {
        int64_t m;
        if (!eval_calc_str(se, s, len, cs->font_size, root_fs, &cv) ||
            (cv.has_unit && cv.pct_milli == 0))
            return 0;
        m = (cv.pct_milli != 0) ? (cv.pct_milli / 100) : cv.px_milli;
        if (m < 0) m = 0;
        if (m > 1000) m = 1000;
        cs->opacity = (uint16_t)m;
        return 1;
    }

    case CSS_PROP_FLEX_GROW:
    case CSS_PROP_FLEX_SHRINK: {
        int32_t v;
        if (!eval_calc_str(se, s, len, cs->font_size, root_fs, &cv) ||
            cv.has_unit || cv.pct_milli != 0)
            return 0;
        v = clamp_px(cv.px_milli);
        if (v < 0) return 0;
        if (prop == CSS_PROP_FLEX_GROW) cs->flex_grow = v;
        else                            cs->flex_shrink = v;
        return 1;
    }

    case CSS_PROP_GAP:
    case CSS_PROP_ROW_GAP:
    case CSS_PROP_COLUMN_GAP: {
        int ts[3], tl[3], nt = split_top_tokens(s, len, ts, tl, 3);
        int32_t g0 = 0, g1 = 0;
        if (nt < 1 || (prop != CSS_PROP_GAP && nt != 1) || nt > 2)
            return 0;
        if (tl[0] == 6 && s_stricmp_n(s + ts[0], "normal", 6) == 0)
            g0 = 0;
        else if (eval_len_str(se, s + ts[0], tl[0], cs->font_size, root_fs,
                              0, 0, 0, 0, &l))
            g0 = l.v;
        else
            return 0;
        if (nt == 2) {
            if (tl[1] == 6 && s_stricmp_n(s + ts[1], "normal", 6) == 0)
                g1 = 0;
            else if (eval_len_str(se, s + ts[1], tl[1], cs->font_size, root_fs,
                                  0, 0, 0, 0, &l))
                g1 = l.v;
            else
                return 0;
        } else {
            g1 = g0;
        }
        if (prop == CSS_PROP_GAP) {
            cs->gap = g0;
            cs->row_gap = g0;
            cs->column_gap = g1;
        } else if (prop == CSS_PROP_ROW_GAP) {
            cs->row_gap = g0;
        } else {
            cs->column_gap = g0;
        }
        return 1;
    }

    case CSS_PROP_BORDER_SPACING:
        if (!eval_len_str(se, s, len, cs->font_size, root_fs, 0, 0, 0, 0, &l))
            return 0;
        cs->border_spacing = l.v;
        return 1;

    case CSS_PROP_Z_INDEX:
        if (buf[0] && s_stricmp(buf, "auto") == 0) {
            cs->z_auto = 1;
            cs->z_index = 0;
            return 1;
        }
        if (!eval_calc_str(se, s, len, cs->font_size, root_fs, &cv) ||
            cv.has_unit || cv.pct_milli != 0)
            return 0;
        cs->z_auto = 0;
        cs->z_index = (int32_t)(cv.px_milli / 1000);
        return 1;

    case CSS_PROP_LINE_HEIGHT:
        if (buf[0] && s_stricmp(buf, "normal") == 0) {
            cs->line_height.type = CSS_LEN_NORMAL;
            cs->line_height.v = 0;
            cs->line_height.pct = 0;
            return 1;
        }
        if (!eval_calc_str(se, s, len, cs->font_size, root_fs, &cv))
            return 0;
        if (!cv.has_unit) {
            if (cv.px_milli < 0) return 0;
            cs->line_height.type = CSS_LEN_NUMBER;
            cs->line_height.v = clamp_px(cv.px_milli);
            cs->line_height.pct = 0;
        } else {
            int32_t px = round_milli(cv.px_milli +
                                     (int64_t)cs->font_size * cv.pct_milli / 100);
            if (px < 0) return 0;
            cs->line_height = len_px(px);
        }
        return 1;

    case CSS_PROP_FONT_WEIGHT:
        if (buf[0]) {
            int k = kw_lookup(kw_fontweight, buf);
            if (k >= 0) {
                uint16_t base = parent ? parent->font_weight : 400;
                switch (k) {
                case CSS_FONTWEIGHTKW_NORMAL:  cs->font_weight = 400; break;
                case CSS_FONTWEIGHTKW_BOLD:    cs->font_weight = 700; break;
                case CSS_FONTWEIGHTKW_BOLDER:  cs->font_weight = relative_weight(base, 1); break;
                case CSS_FONTWEIGHTKW_LIGHTER: cs->font_weight = relative_weight(base, 0); break;
                default: break;
                }
                return 1;
            }
        }
        if (eval_calc_str(se, s, len, cs->font_size, root_fs, &cv) &&
            !cv.has_unit && cv.pct_milli == 0 &&
            cv.px_milli >= 1000 && cv.px_milli <= 1000000) {
            cs->font_weight = (uint16_t)(cv.px_milli / 1000);
            return 1;
        }
        return 0;

    case CSS_PROP_FONT_FAMILY: {
        uint8_t cls = CSS_FONTFAMILY_SERIF;
        const char *fn = 0;
        if (!eval_font_family_str(se, s, len, &cls, &fn))
            return 0;
        cs->font_family = cls;
        cs->font_family_name = fn;
        return 1;
    }

    case CSS_PROP_TEXT_DECORATION: {
        int ts[8], tl[8], nt = split_top_tokens(s, len, ts, tl, 8), i, bits = 0;
        if (nt <= 0) return 0;
        for (i = 0; i < nt; i++) {
            char tb[32];
            int k;
            if (tl[i] >= (int)sizeof(tb)) continue;
            memcpy(tb, s + ts[i], (unsigned long)tl[i]);
            tb[tl[i]] = 0;
            k = kw_lookup(kw_decor, tb);
            if (k > 0) bits |= 1 << (k - 1);
        }
        cs->text_decoration = (uint8_t)bits;
        return 1;
    }

    case CSS_PROP_VERTICAL_ALIGN:
        if (buf[0]) {
            int k = kw_lookup(kw_valign, buf);
            if (k >= 0 && k != CSS_VALIGN_LENGTH) {
                cs->vertical_align = (uint8_t)k;
                cs->vertical_align_px = 0;
                return 1;
            }
        }
        if (eval_calc_str(se, s, len, cs->font_size, root_fs, &cv) && cv.has_unit) {
            cs->vertical_align = CSS_VALIGN_LENGTH;
            cs->vertical_align_px = round_milli(cv.px_milli +
                                                (int64_t)cs->font_size * cv.pct_milli / 100);
            return 1;
        }
        return 0;

    case CSS_PROP_BOX_SHADOW:
        apply_box_shadow_str(se, cs, parent, s, len, root_fs);
        return 1;

    case CSS_PROP_GRID_TEMPLATE_COLS:
        apply_grid_tracks_str(se, s, len, cs->font_size, root_fs,
                              cs->grid_cols, &cs->grid_ncols, CSS_GRID_MAX_COLS);
        return 1;

    case CSS_PROP_GRID_TEMPLATE_ROWS:
        apply_grid_tracks_str(se, s, len, cs->font_size, root_fs,
                              cs->grid_rows, &cs->grid_nrows, CSS_GRID_MAX_ROWS);
        return 1;

    case CSS_PROP_GRID_COL_START:
    case CSS_PROP_GRID_COL_END:
    case CSS_PROP_GRID_ROW_START:
    case CSS_PROP_GRID_ROW_END:
        apply_grid_line_val(se, cs, prop, s, len);
        return 1;

    case CSS_PROP_ASPECT_RATIO:
        return apply_aspect_ratio_str(se, cs, s, len, root_fs);

    case CSS_PROP_ORDER:
        if (!eval_calc_str(se, s, len, cs->font_size, root_fs, &cv) ||
            cv.has_unit || cv.pct_milli != 0 || (cv.px_milli % 1000) != 0)
            return 0;
        cs->order = (int32_t)(cv.px_milli / 1000);
        return 1;

    case CSS_PROP_OUTLINE_WIDTH:
        if (buf[0]) {
            int k = kw_lookup(kw_borderwidth, buf);
            if (k >= 0) {
                static const int32_t bw[3] = { 1, 3, 5 };
                cs->outline_width = bw[k];
                return 1;
            }
        }
        if (!eval_len_str(se, s, len, cs->font_size, root_fs, 0, 0, 0, 0, &l))
            return 0;
        cs->outline_width = l.v;
        return 1;

    case CSS_PROP_OUTLINE_STYLE: {
        int k = -1;
        if (!buf[0]) return 0;
        if (s_stricmp(buf, "auto") == 0) k = CSS_BORDERSTYLE_SOLID;
        else k = kw_lookup(kw_borderstyle, buf);
        if (k < 0) return 0;
        cs->outline_style = (uint8_t)k;
        return 1;
    }

    case CSS_PROP_OUTLINE_COLOR:
        if (buf[0] && s_stricmp(buf, "invert") == 0) {
            cs->outline_color = cs->color;
            return 1;
        }
        if (!eval_color_str(se, cs, parent, s, len, &c, 1)) return 0;
        cs->outline_color = c;
        return 1;

    case CSS_PROP_OUTLINE_OFFSET:
        if (!eval_len_str(se, s, len, cs->font_size, root_fs, 1, 0, 0, 0, &l))
            return 0;
        cs->outline_offset = l.v;
        return 1;

    case CSS_PROP_LETTER_SPACING:
        if (buf[0] && s_stricmp(buf, "normal") == 0) {
            cs->letter_spacing = 0;
            return 1;
        }
        if (!eval_len_str(se, s, len, cs->font_size, root_fs, 1, 0, 0, 0, &l))
            return 0;
        cs->letter_spacing = l.v;
        return 1;

    case CSS_PROP_WORD_SPACING:
        if (buf[0] && s_stricmp(buf, "normal") == 0) {
            cs->word_spacing = 0;
            return 1;
        }
        if (!eval_len_str(se, s, len, cs->font_size, root_fs, 1, 0, 0, 0, &l))
            return 0;
        cs->word_spacing = l.v;
        return 1;

    case CSS_PROP_TRANSFORM:
        return apply_transform_str(se, cs, s, len, root_fs);

    default:
        return 0;
    }
}

static void apply_raw_expr(struct style_engine *se, struct computed_style *cs,
                           const struct computed_style *parent,
                           const struct computed_style *initial, int prop,
                           int raw_kw, const char *raw_str, int32_t root_fs)
{
    char exp_buf[STYLE_RAW_BUF];
    int a, al, elen;

    if (!raw_str)
        return;
    if (!expand_vars_buf(se->cur_env, raw_str, (int)strlen(raw_str), exp_buf,
                         sizeof(exp_buf), 0, 0))
        return;
    elen = (int)strlen(exp_buf);
    trim_span(exp_buf, elen, &a, &al);
    if (al <= 0)
        return;

    if ((al == 7 && s_stricmp_n(exp_buf + a, "inherit", 7) == 0)) {
        copy_prop(cs, parent ? parent : initial, prop);
        return;
    }
    if ((al == 7 && s_stricmp_n(exp_buf + a, "initial", 7) == 0)) {
        copy_prop(cs, initial, prop);
        if (prop == CSS_PROP_OUTLINE_COLOR) cs->outline_color = cs->color;
        else if (prop == CSS_PROP_OUTLINE_WIDTH) cs->outline_width = 3;
        return;
    }
    if ((al == 5 && s_stricmp_n(exp_buf + a, "unset", 5) == 0)) {
        copy_prop(cs, css_property_inherited(prop)
                          ? (parent ? parent : initial) : initial, prop);
        if (prop == CSS_PROP_OUTLINE_COLOR) cs->outline_color = cs->color;
        else if (prop == CSS_PROP_OUTLINE_WIDTH) cs->outline_width = 3;
        return;
    }

    if (raw_kw == CSS_RAW_NONE) {
        (void)apply_direct_raw_prop(se, cs, parent, prop, exp_buf + a, al,
                                    root_fs);
        return;
    }

    if (raw_kw >= CSS_RAW_BOX_TOP && raw_kw <= CSS_RAW_BOX_LEFT) {
        int ts[8], tl[8], nt, n = 0, i;
        int side = raw_kw - CSS_RAW_BOX_TOP;
        int pick = 0;
        nt = split_top_tokens(exp_buf + a, al, ts, tl, 8);
        for (i = 0; i < nt; i++) {
            if (tl[i] == 1 && exp_buf[a + ts[i]] == '/')
                break;
            if (n >= 4) return;
            n++;
        }
        if (n < 1 || n > 4)
            return;
        if (n == 1)      pick = 0;
        else if (n == 2) pick = (side & 1) ? 1 : 0;
        else if (n == 3) pick = (side == 0) ? 0 : (side == 2 ? 2 : 1);
        else             pick = side;
        (void)apply_direct_raw_prop(se, cs, parent, prop,
                                    exp_buf + a + ts[pick], tl[pick], root_fs);
        return;
    }

    if (raw_kw == CSS_RAW_BORDER || raw_kw == CSS_RAW_OUTLINE) {
        int ts[8], tl[8], nt, i;
        int is_out = (raw_kw == CSS_RAW_OUTLINE);
        int is_w = is_out ? (prop == CSS_PROP_OUTLINE_WIDTH)
                          : (prop >= CSS_PROP_BORDER_TOP_WIDTH &&
                             prop <= CSS_PROP_BORDER_LEFT_WIDTH);
        int is_s = is_out ? (prop == CSS_PROP_OUTLINE_STYLE)
                          : (prop >= CSS_PROP_BORDER_TOP_STYLE &&
                             prop <= CSS_PROP_BORDER_LEFT_STYLE);
        int w_idx = -1, s_idx = -1, c_idx = -1;
        if (al == 4 && s_stricmp_n(exp_buf + a, "none", 4) == 0) {
            if (is_w) (void)apply_direct_raw_prop(se, cs, parent, prop, "0px", 3, root_fs);
            else if (is_s) (void)apply_direct_raw_prop(se, cs, parent, prop, "none", 4, root_fs);
            else if (is_out) cs->outline_color = cs->color;
            return;
        }
        nt = split_top_tokens(exp_buf + a, al, ts, tl, 8);
        for (i = 0; i < nt; i++) {
            const char *tok = exp_buf + a + ts[i];
            int len_i = tl[i];
            char tb[64];
            struct css_len l;
            uint32_t c;
            if (len_i < (int)sizeof(tb)) {
                memcpy(tb, tok, (unsigned long)len_i);
                tb[len_i] = 0;
                if (s_idx < 0 && (kw_lookup(kw_borderstyle, tb) >= 0 ||
                                  (is_out && s_stricmp(tb, "auto") == 0))) {
                    s_idx = i; continue;
                }
                if (w_idx < 0 && kw_lookup(kw_borderwidth, tb) >= 0) {
                    w_idx = i; continue;
                }
            }
            if (w_idx < 0 && eval_len_str(se, tok, len_i, cs->font_size, root_fs,
                                          0, 0, 0, 0, &l)) {
                w_idx = i; continue;
            }
            if (c_idx < 0 && (eval_color_str(se, cs, parent, tok, len_i, &c, 1) ||
                              (is_out && len_i == 6 &&
                               s_stricmp_n(tok, "invert", 6) == 0))) {
                c_idx = i; continue;
            }
            return;
        }
        if (is_w) {
            if (w_idx >= 0)
                (void)apply_direct_raw_prop(se, cs, parent, prop,
                                            exp_buf + a + ts[w_idx], tl[w_idx], root_fs);
            else
                (void)apply_direct_raw_prop(se, cs, parent, prop, "medium", 6, root_fs);
        } else if (is_s) {
            if (s_idx >= 0)
                (void)apply_direct_raw_prop(se, cs, parent, prop,
                                            exp_buf + a + ts[s_idx], tl[s_idx], root_fs);
            else
                (void)apply_direct_raw_prop(se, cs, parent, prop, "none", 4, root_fs);
        } else {
            if (c_idx >= 0)
                (void)apply_direct_raw_prop(se, cs, parent, prop,
                                            exp_buf + a + ts[c_idx], tl[c_idx], root_fs);
            else if (is_out)
                cs->outline_color = cs->color;
        }
        return;
    }

    if (raw_kw == CSS_RAW_BG) {
        int ts[16], tl[16], nt, i;
        int col_idx = -1, img_idx = -1, none_img = 0;
        nt = split_top_tokens(exp_buf + a, al, ts, tl, 16);
        for (i = 0; i < nt; i++) {
            const char *tok = exp_buf + a + ts[i];
            int len_i = tl[i];
            uint32_t c;
            if (len_i > 16 && s_stricmp_n(tok, "linear-gradient(", 16) == 0) {
                img_idx = i; continue;
            }
            if (len_i > 4 && s_stricmp_n(tok, "url(", 4) == 0) {
                img_idx = i; continue;
            }
            if (len_i == 4 && s_stricmp_n(tok, "none", 4) == 0) {
                none_img = 1; continue;
            }
            if (col_idx < 0 && eval_color_str(se, cs, parent, tok, len_i, &c, 1)) {
                col_idx = i; continue;
            }
        }
        if (prop == CSS_PROP_BACKGROUND_COLOR) {
            if (col_idx >= 0)
                (void)apply_direct_raw_prop(se, cs, parent, prop,
                                            exp_buf + a + ts[col_idx], tl[col_idx], root_fs);
            else if (img_idx >= 0 || none_img)
                cs->background_color = CSS_TRANSPARENT;
        } else if (prop == CSS_PROP_BACKGROUND_IMAGE) {
            if (img_idx >= 0)
                (void)apply_direct_raw_prop(se, cs, parent, prop,
                                            exp_buf + a + ts[img_idx], tl[img_idx], root_fs);
            else
                (void)apply_direct_raw_prop(se, cs, parent, prop, "none", 4, root_fs);
        }
        return;
    }

    if (raw_kw == CSS_RAW_FLEX) {
        int ts[4], tl[4], nt, i, n_num = 0, have_b = 0;
        int32_t nums[2] = { 1000, 1000 };
        struct css_len basis = len_px(0);
        nt = split_top_tokens(exp_buf + a, al, ts, tl, 4);
        if (nt == 1 && tl[0] == 4 && s_stricmp_n(exp_buf + a + ts[0], "none", 4) == 0) {
            if (prop == CSS_PROP_FLEX_GROW) cs->flex_grow = 0;
            else if (prop == CSS_PROP_FLEX_SHRINK) cs->flex_shrink = 0;
            else if (prop == CSS_PROP_FLEX_BASIS) cs->flex_basis = len_auto();
            return;
        }
        if (nt == 1 && tl[0] == 4 && s_stricmp_n(exp_buf + a + ts[0], "auto", 4) == 0) {
            if (prop == CSS_PROP_FLEX_GROW) cs->flex_grow = 1000;
            else if (prop == CSS_PROP_FLEX_SHRINK) cs->flex_shrink = 1000;
            else if (prop == CSS_PROP_FLEX_BASIS) cs->flex_basis = len_auto();
            return;
        }
        for (i = 0; i < nt; i++) {
            const char *tok = exp_buf + a + ts[i];
            int len_i = tl[i];
            struct calc_val cv;
            struct css_len bl;
            if (eval_calc_str(se, tok, len_i, cs->font_size, root_fs, &cv) &&
                !cv.has_unit && cv.pct_milli == 0 && cv.px_milli >= 0) {
                if (n_num < 2) {
                    nums[n_num++] = clamp_px(cv.px_milli);
                    continue;
                }
                if (n_num == 2 && !have_b && cv.px_milli == 0) {
                    basis = len_px(0);
                    have_b = 1;
                    continue;
                }
                return;
            }
            if (!have_b && eval_len_str(se, tok, len_i, cs->font_size, root_fs,
                                        0, 1, 1, 0, &bl)) {
                basis = bl;
                have_b = 1;
                continue;
            }
            return;
        }
        if (n_num == 0 && !have_b) return;
        if (prop == CSS_PROP_FLEX_GROW)
            cs->flex_grow = (n_num >= 1) ? nums[0] : 1000;
        else if (prop == CSS_PROP_FLEX_SHRINK)
            cs->flex_shrink = (n_num >= 2) ? nums[1] : 1000;
        else if (prop == CSS_PROP_FLEX_BASIS)
            cs->flex_basis = have_b ? basis : len_px(0);
        return;
    }

    if (raw_kw == CSS_RAW_GAP) {
        int ts[4], tl[4], nt = split_top_tokens(exp_buf + a, al, ts, tl, 4);
        int idx;
        if (nt < 1 || nt > 2) return;
        idx = (prop == CSS_PROP_COLUMN_GAP && nt == 2) ? 1 : 0;
        (void)apply_direct_raw_prop(se, cs, parent,
                                    (prop == CSS_PROP_GAP) ? CSS_PROP_GAP : prop,
                                    exp_buf + a + ts[idx], tl[idx], root_fs);
        return;
    }

    if (raw_kw == CSS_RAW_GRID_LINE) {
        const char *str = exp_buf + a;
        int i, slash = -1, depth = 0;
        int is_start = (prop == CSS_PROP_GRID_COL_START || prop == CSS_PROP_GRID_ROW_START);
        for (i = 0; i < al; i++) {
            if (str[i] == '(') depth++;
            else if (str[i] == ')' && depth > 0) depth--;
            else if (depth == 0 && str[i] == '/') { slash = i; break; }
        }
        if (slash < 0) {
            if (is_start) apply_grid_line_val(se, cs, prop, str, al);
            else          apply_grid_line_val(se, cs, prop, "auto", 4);
        } else {
            if (is_start) apply_grid_line_val(se, cs, prop, str, slash);
            else          apply_grid_line_val(se, cs, prop, str + slash + 1,
                                              al - slash - 1);
        }
        return;
    }

    if (raw_kw == CSS_RAW_LIST_STYLE) {
        int ts[4], tl[4], nt = split_top_tokens(exp_buf + a, al, ts, tl, 4), i;
        for (i = 0; i < nt; i++) {
            char tb[48];
            int k;
            if (tl[i] >= (int)sizeof(tb)) continue;
            memcpy(tb, exp_buf + a + ts[i], (unsigned long)tl[i]);
            tb[tl[i]] = 0;
            if (prop == CSS_PROP_LIST_STYLE_POSITION &&
                (k = kw_lookup(kw_listpos, tb)) >= 0) {
                cs->list_style_position = (uint8_t)k;
                return;
            }
            if (prop == CSS_PROP_LIST_STYLE_TYPE &&
                (k = kw_lookup(kw_liststyle, tb)) >= 0) {
                cs->list_style_type = (uint8_t)k;
                return;
            }
        }
        return;
    }

    if (raw_kw == CSS_RAW_FONT) {
        int ts[12], tl[12], nt = split_top_tokens(exp_buf + a, al, ts, tl, 12);
        int i = 0, size_at = -1, lh_start = -1, lh_len = 0, sz_len = 0;
        const char *sz_ptr = 0;
        for (i = 0; i < nt; i++) {
            const char *tok = exp_buf + a + ts[i];
            int len_i = tl[i], k;
            char tb[32];
            if (len_i < (int)sizeof(tb)) {
                memcpy(tb, tok, (unsigned long)len_i);
                tb[len_i] = 0;
                if ((k = kw_lookup(kw_fontstyle, tb)) > 0) {
                    if (prop == CSS_PROP_FONT_STYLE) cs->font_style = (uint8_t)k;
                    continue;
                }
                if ((k = kw_lookup(kw_fontweight, tb)) >= 0) {
                    if (prop == CSS_PROP_FONT_WEIGHT)
                        (void)apply_direct_raw_prop(se, cs, parent, prop, tok, len_i, root_fs);
                    continue;
                }
                if (s_stricmp(tb, "normal") == 0 || s_stricmp(tb, "small-caps") == 0)
                    continue;
            }
            {
                struct calc_val cv;
                if (eval_calc_str(se, tok, len_i, cs->font_size, root_fs, &cv) &&
                    !cv.has_unit && cv.pct_milli == 0 &&
                    cv.px_milli >= 1000 && cv.px_milli <= 1000000) {
                    if (prop == CSS_PROP_FONT_WEIGHT)
                        cs->font_weight = (uint16_t)(cv.px_milli / 1000);
                    continue;
                }
            }
            size_at = i;
            break;
        }
        if (size_at < 0)
            return;
        {
            const char *st = exp_buf + a + ts[size_at];
            int sl = tl[size_at], pdepth = 0, j, slash = -1;
            for (j = 0; j < sl; j++) {
                if (st[j] == '(') pdepth++;
                else if (st[j] == ')' && pdepth > 0) pdepth--;
                else if (pdepth == 0 && st[j] == '/') { slash = j; break; }
            }
            sz_ptr = st;
            if (slash >= 0) {
                sz_len = slash;
                lh_start = a + ts[size_at] + slash + 1;
                lh_len = sl - slash - 1;
                i = size_at + 1;
            } else {
                sz_len = sl;
                i = size_at + 1;
                if (i < nt && tl[i] == 1 && exp_buf[a + ts[i]] == '/' && i + 1 < nt) {
                    lh_start = a + ts[i + 1];
                    lh_len = tl[i + 1];
                    i += 2;
                }
            }
        }
        if (prop == CSS_PROP_LINE_HEIGHT && lh_start >= 0)
            (void)apply_direct_raw_prop(se, cs, parent, prop,
                                        exp_buf + lh_start, lh_len, root_fs);
        else if (prop == CSS_PROP_FONT_FAMILY && i < nt)
            (void)apply_direct_raw_prop(se, cs, parent, prop,
                                        exp_buf + a + ts[i],
                                        al - ts[i], root_fs);
        else if (prop == CSS_PROP_FONT_SIZE && sz_ptr)
            (void)apply_direct_raw_prop(se, cs, parent, prop, sz_ptr, sz_len,
                                        root_fs);
    }
}

/* ================================================================== *
 * applying a specified value
 * ================================================================== */

static void apply_value(struct style_engine *se, struct computed_style *cs,
                        const struct computed_style *parent,
                        const struct computed_style *initial, int prop,
                        const struct css_value *v, int32_t root_fs)
{
    int32_t fs = cs->font_size;
    int side;

    if (v->type == CSS_VAL_INHERIT) {
        copy_prop(cs, parent ? parent : initial, prop);
        return;
    }
    if (v->type == CSS_VAL_INITIAL) {
        copy_prop(cs, initial, prop);
        if (prop == CSS_PROP_OUTLINE_COLOR) cs->outline_color = cs->color;
        else if (prop == CSS_PROP_OUTLINE_WIDTH) cs->outline_width = 3;
        return;
    }
    if (v->type == CSS_VAL_UNSET) {
        if (css_property_inherited(prop))
            copy_prop(cs, parent ? parent : initial, prop);
        else {
            copy_prop(cs, initial, prop);
            if (prop == CSS_PROP_OUTLINE_COLOR) cs->outline_color = cs->color;
            else if (prop == CSS_PROP_OUTLINE_WIDTH) cs->outline_width = 3;
        }
        return;
    }
    if (v->type == CSS_VAL_RAW_EXPR) {
        apply_raw_expr(se, cs, parent, initial, prop, v->kw, v->str, root_fs);
        return;
    }

    switch (prop) {
    case CSS_PROP_DISPLAY:
        if (v->type == CSS_VAL_KEYWORD) cs->display = (uint8_t)v->kw;
        break;
    case CSS_PROP_POSITION:
        if (v->type == CSS_VAL_KEYWORD) cs->position = (uint8_t)v->kw;
        break;
    case CSS_PROP_FLOAT:
        if (v->type == CSS_VAL_KEYWORD) cs->css_float = (uint8_t)v->kw;
        break;
    case CSS_PROP_CLEAR:
        if (v->type == CSS_VAL_KEYWORD) cs->clear = (uint8_t)v->kw;
        break;
    case CSS_PROP_BOX_SIZING:
        if (v->type == CSS_VAL_KEYWORD) cs->box_sizing = (uint8_t)v->kw;
        break;
    case CSS_PROP_TEXT_ALIGN:
        if (v->type == CSS_VAL_KEYWORD) cs->text_align = (uint8_t)v->kw;
        break;
    case CSS_PROP_WHITE_SPACE:
        if (v->type == CSS_VAL_KEYWORD) cs->white_space = (uint8_t)v->kw;
        break;
    case CSS_PROP_OVERFLOW:
        if (v->type == CSS_VAL_KEYWORD) cs->overflow = (uint8_t)v->kw;
        break;
    case CSS_PROP_VISIBILITY:
        if (v->type == CSS_VAL_KEYWORD) cs->visibility = (uint8_t)v->kw;
        break;
    case CSS_PROP_FONT_STYLE:
        if (v->type == CSS_VAL_KEYWORD) cs->font_style = (uint8_t)v->kw;
        break;
    case CSS_PROP_LIST_STYLE_TYPE:
        if (v->type == CSS_VAL_KEYWORD) cs->list_style_type = (uint8_t)v->kw;
        break;
    case CSS_PROP_LIST_STYLE_POSITION:
        if (v->type == CSS_VAL_KEYWORD) cs->list_style_position = (uint8_t)v->kw;
        break;
    case CSS_PROP_BORDER_COLLAPSE:
        if (v->type == CSS_VAL_KEYWORD) cs->border_collapse = (uint8_t)v->kw;
        break;
    case CSS_PROP_TEXT_TRANSFORM:
        if (v->type == CSS_VAL_KEYWORD) cs->text_transform = (uint8_t)v->kw;
        break;
    case CSS_PROP_TEXT_DECORATION:
        if (v->type == CSS_VAL_BITS) cs->text_decoration = (uint8_t)v->kw;
        break;
    case CSS_PROP_FLEX_DIRECTION:
        if (v->type == CSS_VAL_KEYWORD) cs->flex_direction = (uint8_t)v->kw;
        break;
    case CSS_PROP_FLEX_WRAP:
        if (v->type == CSS_VAL_KEYWORD) cs->flex_wrap = (uint8_t)v->kw;
        break;
    case CSS_PROP_JUSTIFY_CONTENT:
        if (v->type == CSS_VAL_KEYWORD) cs->justify_content = (uint8_t)v->kw;
        break;
    case CSS_PROP_ALIGN_ITEMS:
        if (v->type == CSS_VAL_KEYWORD) cs->align_items = (uint8_t)v->kw;
        break;
    case CSS_PROP_COLOR_SCHEME:
        if (v->type == CSS_VAL_KEYWORD) cs->color_scheme = (uint8_t)v->kw;
        break;
    case CSS_PROP_FONT_FAMILY:
        if (v->type == CSS_VAL_KEYWORD) {
            cs->font_family = (uint8_t)v->kw;
            cs->font_family_name = v->str;
        }
        break;

    case CSS_PROP_WIDTH:      cs->width = to_len(se, v, fs, root_fs); break;
    case CSS_PROP_HEIGHT:     cs->height = to_len(se, v, fs, root_fs); break;
    case CSS_PROP_MIN_WIDTH:  cs->min_width = to_len(se, v, fs, root_fs); break;
    case CSS_PROP_MAX_WIDTH:  cs->max_width = to_len(se, v, fs, root_fs); break;
    case CSS_PROP_MIN_HEIGHT: cs->min_height = to_len(se, v, fs, root_fs); break;
    case CSS_PROP_MAX_HEIGHT: cs->max_height = to_len(se, v, fs, root_fs); break;
    case CSS_PROP_FLEX_BASIS: cs->flex_basis = to_len(se, v, fs, root_fs); break;

    case CSS_PROP_TOP:
    case CSS_PROP_RIGHT:
    case CSS_PROP_BOTTOM:
    case CSS_PROP_LEFT:
        side = prop - CSS_PROP_TOP;
        cs->offset[side] = to_len(se, v, fs, root_fs);
        break;
    case CSS_PROP_MARGIN_TOP:
    case CSS_PROP_MARGIN_RIGHT:
    case CSS_PROP_MARGIN_BOTTOM:
    case CSS_PROP_MARGIN_LEFT:
        side = prop - CSS_PROP_MARGIN_TOP;
        cs->margin[side] = to_len(se, v, fs, root_fs);
        break;
    case CSS_PROP_PADDING_TOP:
    case CSS_PROP_PADDING_RIGHT:
    case CSS_PROP_PADDING_BOTTOM:
    case CSS_PROP_PADDING_LEFT:
        side = prop - CSS_PROP_PADDING_TOP;
        cs->padding[side] = to_len(se, v, fs, root_fs);
        break;

    case CSS_PROP_BORDER_TOP_WIDTH:
    case CSS_PROP_BORDER_RIGHT_WIDTH:
    case CSS_PROP_BORDER_BOTTOM_WIDTH:
    case CSS_PROP_BORDER_LEFT_WIDTH:
        side = prop - CSS_PROP_BORDER_TOP_WIDTH;
        if (v->type == CSS_VAL_KEYWORD) {
            static const int32_t bw[3] = { 1, 3, 5 };  /* thin/medium/thick */
            int k = v->kw;
            if (k < 0) k = 0;
            if (k > 2) k = 2;
            cs->border_width[side] = bw[k];
        } else if (v->type == CSS_VAL_LENGTH) {
            cs->border_width[side] = resolve_px(se, v, fs, root_fs);
        }
        break;
    case CSS_PROP_BORDER_TOP_STYLE:
    case CSS_PROP_BORDER_RIGHT_STYLE:
    case CSS_PROP_BORDER_BOTTOM_STYLE:
    case CSS_PROP_BORDER_LEFT_STYLE:
        side = prop - CSS_PROP_BORDER_TOP_STYLE;
        if (v->type == CSS_VAL_KEYWORD)
            cs->border_style[side] = (uint8_t)v->kw;
        break;
    case CSS_PROP_BORDER_TOP_COLOR:
    case CSS_PROP_BORDER_RIGHT_COLOR:
    case CSS_PROP_BORDER_BOTTOM_COLOR:
    case CSS_PROP_BORDER_LEFT_COLOR:
        side = prop - CSS_PROP_BORDER_TOP_COLOR;
        if (v->type == CSS_VAL_COLOR)
            cs->border_color[side] = v->color;
        else if (v->type == CSS_VAL_IDENT)      /* currentColor */
            cs->border_color[side] = cs->color;
        break;

    case CSS_PROP_BORDER_TL_RADIUS:
    case CSS_PROP_BORDER_TR_RADIUS:
    case CSS_PROP_BORDER_BR_RADIUS:
    case CSS_PROP_BORDER_BL_RADIUS:
        side = prop - CSS_PROP_BORDER_TL_RADIUS;
        if (v->type == CSS_VAL_LENGTH) {
            int32_t px = resolve_px(se, v, fs, root_fs);
            cs->border_radius[side] = px > 0 ? px : 0;
        } else if (v->type == CSS_VAL_PERCENT) {
            int32_t ref = 0;
            int32_t px;
            if (cs->width.type == CSS_LEN_PX && cs->height.type == CSS_LEN_PX)
                ref = (cs->width.v < cs->height.v) ? cs->width.v : cs->height.v;
            else if (cs->width.type == CSS_LEN_PX)
                ref = cs->width.v;
            else if (cs->height.type == CSS_LEN_PX)
                ref = cs->height.v;
            px = clamp_px((int64_t)ref * v->num / 100000);
            cs->border_radius[side] = px > 0 ? px : 0;
        }
        break;

    case CSS_PROP_COLOR:
        if (v->type == CSS_VAL_COLOR)
            cs->color = v->color;
        else if (v->type == CSS_VAL_IDENT && parent)
            cs->color = parent->color;
        break;
    case CSS_PROP_OPACITY: {
        int32_t m = 1000;
        if (v->type == CSS_VAL_NUMBER)
            m = v->num;
        else if (v->type == CSS_VAL_PERCENT)
            m = v->num / 100;
        if (m < 0) m = 0;
        if (m > 1000) m = 1000;
        cs->opacity = (uint16_t)m;
        break;
    }
    case CSS_PROP_BACKGROUND_COLOR:
        if (v->type == CSS_VAL_COLOR)
            cs->background_color = v->color;
        else if (v->type == CSS_VAL_IDENT)
            cs->background_color = cs->color;
        break;
    case CSS_PROP_BACKGROUND_IMAGE:
        cs->has_gradient = 0;
        cs->grad_nstops = 0;
        if (v->type == CSS_VAL_URL)
            cs->background_image = v->str;
        else
            cs->background_image = 0;
        break;
    case CSS_PROP_BOX_SHADOW:
        cs->has_shadow = 0;
        cs->shadow_inset = 0;
        cs->shadow_x = cs->shadow_y = cs->shadow_blur = cs->shadow_spread = 0;
        cs->shadow_color = 0;
        break;

    case CSS_PROP_TEXT_INDENT:
        cs->text_indent = to_len(se, v, fs, root_fs);
        break;
    case CSS_PROP_VERTICAL_ALIGN:
        if (v->type == CSS_VAL_KEYWORD) {
            cs->vertical_align = (uint8_t)v->kw;
            cs->vertical_align_px = 0;
        } else if (v->type == CSS_VAL_LENGTH) {
            cs->vertical_align = CSS_VALIGN_LENGTH;
            cs->vertical_align_px = resolve_px(se, v, fs, root_fs);
        } else if (v->type == CSS_VAL_PERCENT) {
            cs->vertical_align = CSS_VALIGN_LENGTH;
            cs->vertical_align_px = clamp_px((int64_t)fs * v->num / 100000);
        }
        break;
    case CSS_PROP_LINE_HEIGHT:
        if (v->type == CSS_VAL_KEYWORD) {
            cs->line_height.type = CSS_LEN_NORMAL;
            cs->line_height.v = 0;
        } else if (v->type == CSS_VAL_NUMBER) {
            cs->line_height.type = CSS_LEN_NUMBER;
            cs->line_height.v = v->num;
        } else if (v->type == CSS_VAL_LENGTH) {
            cs->line_height.type = CSS_LEN_PX;
            cs->line_height.v = resolve_px(se, v, fs, root_fs);
        } else if (v->type == CSS_VAL_PERCENT) {
            cs->line_height.type = CSS_LEN_PX;
            cs->line_height.v = clamp_px((int64_t)fs * v->num / 100000);
        }
        break;
    case CSS_PROP_BORDER_SPACING:
        if (v->type == CSS_VAL_LENGTH)
            cs->border_spacing = resolve_px(se, v, fs, root_fs);
        break;
    case CSS_PROP_FLEX_GROW:
        if (v->type == CSS_VAL_NUMBER) {
            int32_t grow = v->num;
            if (grow < 0) grow = 0;
            if (grow > 1000000) grow = 1000000;
            cs->flex_grow = grow;
        }
        break;
    case CSS_PROP_FLEX_SHRINK:
        if (v->type == CSS_VAL_NUMBER) {
            int32_t shrink = v->num;
            if (shrink < 0) shrink = 0;
            if (shrink > 1000000) shrink = 1000000;
            cs->flex_shrink = shrink;
        }
        break;
    case CSS_PROP_GAP:
        if (v->type == CSS_VAL_LENGTH) {
            int32_t gap = resolve_px(se, v, fs, root_fs);
            gap = gap > 0 ? gap : 0;
            cs->gap = gap;
            cs->row_gap = gap;
            cs->column_gap = gap;
        }
        break;
    case CSS_PROP_ROW_GAP:
        if (v->type == CSS_VAL_LENGTH) {
            int32_t gap = resolve_px(se, v, fs, root_fs);
            cs->row_gap = gap > 0 ? gap : 0;
        }
        break;
    case CSS_PROP_COLUMN_GAP:
        if (v->type == CSS_VAL_LENGTH) {
            int32_t gap = resolve_px(se, v, fs, root_fs);
            cs->column_gap = gap > 0 ? gap : 0;
        }
        break;
    case CSS_PROP_GRID_TEMPLATE_COLS:
        cs->grid_ncols = 0;
        break;
    case CSS_PROP_GRID_COL_START:
        if (v->type == CSS_VAL_AUTO) {
            cs->grid_col_start = 0;
        } else if (v->type == CSS_VAL_KEYWORD && v->kw == 1) {
            cs->grid_col_start = 0;
            cs->grid_col_span = (uint8_t)(v->num > 0 && v->num <= 255 ? v->num : 1);
        } else if (v->type == CSS_VAL_NUMBER) {
            cs->grid_col_start = (int16_t)clamp_px(v->num / 1000);
        }
        break;
    case CSS_PROP_GRID_COL_END:
        if (v->type == CSS_VAL_AUTO) {
            cs->grid_col_end = 0;
        } else if (v->type == CSS_VAL_KEYWORD && v->kw == 1) {
            cs->grid_col_end = 0;
            cs->grid_col_span = (uint8_t)(v->num > 0 && v->num <= 255 ? v->num : 1);
        } else if (v->type == CSS_VAL_NUMBER) {
            cs->grid_col_end = (int16_t)clamp_px(v->num / 1000);
        }
        break;
    case CSS_PROP_GRID_ROW_START:
        if (v->type == CSS_VAL_AUTO) {
            cs->grid_row_start = 0;
        } else if (v->type == CSS_VAL_KEYWORD && v->kw == 1) {
            cs->grid_row_start = 0;
            cs->grid_row_span = (uint8_t)(v->num > 0 && v->num <= 255 ? v->num : 1);
        } else if (v->type == CSS_VAL_NUMBER) {
            cs->grid_row_start = (int16_t)clamp_px(v->num / 1000);
        }
        break;
    case CSS_PROP_GRID_ROW_END:
        if (v->type == CSS_VAL_AUTO) {
            cs->grid_row_end = 0;
        } else if (v->type == CSS_VAL_KEYWORD && v->kw == 1) {
            cs->grid_row_end = 0;
            cs->grid_row_span = (uint8_t)(v->num > 0 && v->num <= 255 ? v->num : 1);
        } else if (v->type == CSS_VAL_NUMBER) {
            cs->grid_row_end = (int16_t)clamp_px(v->num / 1000);
        }
        break;
    case CSS_PROP_ASPECT_RATIO:
        if (v->type == CSS_VAL_AUTO)
            cs->aspect_ratio = 0;
        else if (v->type == CSS_VAL_NUMBER)
            cs->aspect_ratio = v->num;
        break;
    case CSS_PROP_OBJECT_FIT:
        if (v->type == CSS_VAL_KEYWORD) cs->object_fit = (uint8_t)v->kw;
        break;
    case CSS_PROP_ALIGN_SELF:
        if (v->type == CSS_VAL_KEYWORD) cs->align_self = (uint8_t)v->kw;
        break;
    case CSS_PROP_ORDER:
        if (v->type == CSS_VAL_NUMBER) cs->order = (int32_t)(v->num / 1000);
        break;
    case CSS_PROP_OUTLINE_WIDTH:
        if (v->type == CSS_VAL_KEYWORD) {
            static const int32_t bw[3] = { 1, 3, 5 };
            int k = v->kw;
            if (k < 0) k = 0;
            if (k > 2) k = 2;
            cs->outline_width = bw[k];
        } else if (v->type == CSS_VAL_LENGTH) {
            cs->outline_width = resolve_px(se, v, fs, root_fs);
        }
        break;
    case CSS_PROP_OUTLINE_STYLE:
        if (v->type == CSS_VAL_KEYWORD) cs->outline_style = (uint8_t)v->kw;
        break;
    case CSS_PROP_OUTLINE_COLOR:
        if (v->type == CSS_VAL_COLOR)
            cs->outline_color = v->color;
        else if (v->type == CSS_VAL_IDENT)
            cs->outline_color = cs->color;
        break;
    case CSS_PROP_OUTLINE_OFFSET:
        if (v->type == CSS_VAL_LENGTH)
            cs->outline_offset = resolve_px(se, v, fs, root_fs);
        break;
    case CSS_PROP_TEXT_OVERFLOW:
        if (v->type == CSS_VAL_KEYWORD) cs->text_overflow = (uint8_t)v->kw;
        break;
    case CSS_PROP_WORD_BREAK:
        if (v->type == CSS_VAL_KEYWORD) cs->word_break = (uint8_t)v->kw;
        break;
    case CSS_PROP_OVERFLOW_WRAP:
        if (v->type == CSS_VAL_KEYWORD) cs->overflow_wrap = (uint8_t)v->kw;
        break;
    case CSS_PROP_LETTER_SPACING:
        if (v->type == CSS_VAL_KEYWORD)
            cs->letter_spacing = 0;
        else if (v->type == CSS_VAL_LENGTH)
            cs->letter_spacing = resolve_px(se, v, fs, root_fs);
        break;
    case CSS_PROP_WORD_SPACING:
        if (v->type == CSS_VAL_KEYWORD)
            cs->word_spacing = 0;
        else if (v->type == CSS_VAL_LENGTH)
            cs->word_spacing = resolve_px(se, v, fs, root_fs);
        break;
    case CSS_PROP_TABLE_LAYOUT:
        if (v->type == CSS_VAL_KEYWORD)
            cs->table_layout = (uint8_t)v->kw;
        break;
    case CSS_PROP_Z_INDEX:
        if (v->type == CSS_VAL_AUTO) {
            cs->z_auto = 1;
            cs->z_index = 0;
        } else if (v->type == CSS_VAL_NUMBER) {
            cs->z_auto = 0;
            cs->z_index = (int32_t)(v->num / 1000);
        }
        break;
    case CSS_PROP_FONT_WEIGHT:
        if (v->type == CSS_VAL_NUMBER) {
            int32_t w = v->num / 1000;
            if (w < 1) w = 1;
            if (w > 1000) w = 1000;
            cs->font_weight = (uint16_t)w;
        } else if (v->type == CSS_VAL_KEYWORD) {
            uint16_t base = parent ? parent->font_weight : 400;
            switch (v->kw) {
            case CSS_FONTWEIGHTKW_NORMAL:  cs->font_weight = 400; break;
            case CSS_FONTWEIGHTKW_BOLD:    cs->font_weight = 700; break;
            case CSS_FONTWEIGHTKW_BOLDER:  cs->font_weight = relative_weight(base, 1); break;
            case CSS_FONTWEIGHTKW_LIGHTER: cs->font_weight = relative_weight(base, 0); break;
            default: break;
            }
        }
        break;
    default:
        break;
    }
}

/* font-size has to be settled before anything else, because em depends
 * on it and it depends only on the parent. */
static void apply_font_size(struct style_engine *se, struct computed_style *cs,
                            const struct computed_style *parent,
                            const struct css_value *v, int32_t root_fs)
{
    int32_t pfs = parent ? parent->font_size : STYLE_DEFAULT_FONT_SIZE;

    if (!v) {
        cs->font_size = pfs;
        return;
    }
    if (v->type == CSS_VAL_RAW_EXPR && v->str) {
        char exp_buf[STYLE_RAW_BUF];
        int a, al;
        if (!expand_vars_buf(se->cur_env, v->str, (int)strlen(v->str),
                             exp_buf, sizeof(exp_buf), 0, 0)) {
            cs->font_size = pfs;
            return;
        }
        trim_span(exp_buf, (int)strlen(exp_buf), &a, &al);
        if (al > 0) {
            char buf[128];
            struct calc_val cv;
            if (al < (int)sizeof(buf)) {
                int k;
                memcpy(buf, exp_buf + a, (unsigned long)al);
                buf[al] = 0;
                if (s_stricmp(buf, "inherit") == 0 ||
                    s_stricmp(buf, "unset") == 0) {
                    cs->font_size = pfs;
                    return;
                }
                if (s_stricmp(buf, "initial") == 0) {
                    cs->font_size = STYLE_DEFAULT_FONT_SIZE;
                    return;
                }
                k = kw_lookup(kw_fontsize, buf);
                if (k >= 0) {
                    if (k < 7) cs->font_size = abs_font_size[k];
                    else if (k == CSS_FONTSIZEKW_SMALLER)
                        cs->font_size = clamp_font((pfs * 5 + 3) / 6);
                    else if (k == CSS_FONTSIZEKW_LARGER)
                        cs->font_size = clamp_font((pfs * 6 + 2) / 5);
                    if (cs->font_size < STYLE_MIN_FONT_SIZE)
                        cs->font_size = STYLE_MIN_FONT_SIZE;
                    return;
                }
            }
            if (eval_calc_str(se, exp_buf + a, al, pfs, root_fs, &cv) &&
                (cv.has_unit || cv.px_milli == 0)) {
                int64_t px = cv.px_milli + (int64_t)pfs * cv.pct_milli / 100;
                cs->font_size = clamp_font(round_milli(px));
                return;
            }
        }
        cs->font_size = pfs;
        return;
    }

    switch (v->type) {
    case CSS_VAL_INHERIT:
    case CSS_VAL_UNSET:          /* font-size inherits, so unset == inherit */
        cs->font_size = pfs;
        break;
    case CSS_VAL_INITIAL:
        cs->font_size = STYLE_DEFAULT_FONT_SIZE;
        break;
    case CSS_VAL_KEYWORD:
        if (v->kw >= 0 && v->kw < 7)
            cs->font_size = abs_font_size[v->kw];
        else if (v->kw == CSS_FONTSIZEKW_SMALLER)
            cs->font_size = clamp_font((pfs * 5 + 3) / 6);
        else if (v->kw == CSS_FONTSIZEKW_LARGER)
            cs->font_size = clamp_font((pfs * 6 + 2) / 5);
        break;
    case CSS_VAL_PERCENT:
        cs->font_size = clamp_font(clamp_px((int64_t)pfs * v->num / 100000));
        break;
    case CSS_VAL_LENGTH:
        /* em and % on font-size are relative to the PARENT's size. */
        cs->font_size = clamp_font(resolve_px(se, v, pfs, root_fs));
        break;
    default:
        break;
    }
    if (cs->font_size < STYLE_MIN_FONT_SIZE)
        cs->font_size = STYLE_MIN_FONT_SIZE;
}

/* ================================================================== *
 * the cascade proper
 * ================================================================== */

static int level_of(int origin, int important)
{
    switch (origin) {
    case CSS_ORIGIN_UA:     return important ? 7 : 0;
    case CSS_ORIGIN_USER:   return important ? 6 : 1;
    case CSS_ORIGIN_AUTHOR: return important ? 4 : 2;
    case CSS_ORIGIN_INLINE: return important ? 5 : 3;
    default:                return important ? 4 : 2;
    }
}

static void consider(struct style_engine *se, const struct css_decl *d,
                     uint32_t spec, uint32_t order, int origin)
{
    int prop = d->prop;
    int lvl;
    struct cascade_slot *sl;

    if (prop <= 0 || prop >= CSS_PROP_COUNT)
        return;
    lvl = level_of(origin, d->important);
    sl = &se->slots[prop];
    if (sl->used) {
        if (lvl < sl->level)
            return;
        if (lvl == sl->level) {
            if (spec < sl->spec)
                return;
            if (spec == sl->spec && order < sl->order)
                return;
        }
    }
    sl->used = 1;
    sl->level = (uint8_t)lvl;
    sl->spec = spec;
    sl->order = order;
    se->spec[prop] = d->val;
}

static void consider_custom(struct style_engine *se,
                            const struct css_custom_decl *cd,
                            uint32_t spec, uint32_t order, int origin)
{
    int lvl = level_of(origin, cd->important);
    int i;
    struct custom_slot *cs = 0;

    if (!cd->name)
        return;
    for (i = 0; i < se->ncscratch; i++) {
        if (strcmp(se->cscratch[i].name, cd->name) == 0) {
            cs = &se->cscratch[i];
            break;
        }
    }
    if (cs) {
        if (lvl < cs->level)
            return;
        if (lvl == cs->level) {
            if (spec < cs->spec)
                return;
            if (spec == cs->spec && order < cs->order)
                return;
        }
    } else {
        if (se->ncscratch >= CSS_MAX_BLOCK_DECLS)
            return;
        if (se->ncscratch >= se->ccscratch) {
            int nc = se->ccscratch ? se->ccscratch * 2 : 16;
            struct custom_slot *arr = (struct custom_slot *)
                realloc(se->cscratch, sizeof(*arr) * (unsigned long)nc);
            if (!arr)
                return;
            se->cscratch = arr;
            se->ccscratch = nc;
        }
        cs = &se->cscratch[se->ncscratch++];
        cs->name = cd->name;
    }
    cs->raw = cd->value;
    cs->level = (uint8_t)lvl;
    cs->spec = spec;
    cs->order = order;
}

static void match_cb(void *ctx, const struct css_rule *r)
{
    struct style_engine *se = (struct style_engine *)ctx;
    int i;

    for (i = 0; i < r->ndecl; i++)
        consider(se, &r->decls[i], r->sel.specificity, r->order, r->origin);
    for (i = 0; i < r->ncustom; i++)
        consider_custom(se, &r->customs[i], r->sel.specificity, r->order,
                        r->origin);
}

static void match_customs_only_cb(void *ctx, const struct css_rule *r)
{
    struct style_engine *se = (struct style_engine *)ctx;
    int i;

    for (i = 0; i < r->ncustom; i++)
        consider_custom(se, &r->customs[i], r->sel.specificity, r->order,
                        r->origin);
}

struct style_engine *style_engine_new(struct css_stylesheet **sheets, int n,
                                      const struct css_elem_ops *ops)
{
    struct style_engine *se;
    uint32_t base = 0;
    int i, j;

    if (!ops || !ops->tag)
        return 0;
    if (n < 0)
        n = 0;
    se = (struct style_engine *)calloc(1, sizeof(*se));
    if (!se)
        return 0;
    if (n > 0) {
        se->sheets = (struct css_stylesheet **)
            calloc((unsigned long)n, sizeof(*se->sheets));
        if (!se->sheets) {
            free(se);
            return 0;
        }
        for (i = 0; i < n; i++)
            se->sheets[i] = sheets[i];
    }
    se->nsheet = n;
    se->ops = *ops;
    se->vw = 1024;
    se->vh = 768;
    for (i = 0; i < n; i++) {
        if (se->sheets[i]) {
            int nr;
            base = css_set_order_base(se->sheets[i], base);
            nr = css_rule_count(se->sheets[i]);
            for (j = 0; j < nr && !se->has_customs; j++) {
                const struct css_rule *r = css_rule_at(se->sheets[i], j);
                if (r && r->ncustom > 0)
                    se->has_customs = 1;
            }
        }
    }
    return se;
}

void style_engine_free(struct style_engine *se)
{
    struct str_node *sn, *nxn;
    if (!se)
        return;
    sn = se->str_pool;
    while (sn) {
        nxn = sn->next;
        free(sn);
        sn = nxn;
    }
    free(se->cscratch);
    free(se->sheets);
    free(se);
}

void style_engine_set_viewport(struct style_engine *se, int32_t w, int32_t h)
{
    if (!se)
        return;
    se->vw = w > 0 ? w : 1;
    se->vh = h > 0 ? h : 1;
}

static struct custom_env *build_ancestor_env(struct style_engine *se, void *elem)
{
    void *anc[CSS_MAX_TREE_DEPTH];
    int n = 0, i;
    void *p;
    struct custom_env *cur = 0;

    if (!se->ops.parent)
        return 0;
    for (p = se->ops.parent(elem); p && n < CSS_MAX_TREE_DEPTH; p = se->ops.parent(p))
        anc[n++] = p;

    for (i = n - 1; i >= 0; i--) {
        struct css_stylesheet *inl = 0;
        const char *icss = se->ops.attr ? se->ops.attr(anc[i], "style") : 0;
        int j;
        se->ncscratch = 0;
        for (j = 0; j < se->nsheet; j++)
            if (se->sheets[j])
                css_match(se->sheets[j], anc[i], &se->ops,
                          match_customs_only_cb, se);
        if (icss && *icss) {
            inl = css_parse_style_attr(icss);
            if (inl) {
                const struct css_rule *r = css_rule_at(inl, 0);
                if (r)
                    for (j = 0; j < r->ncustom; j++)
                        consider_custom(se, &r->customs[j], 0xFFFFFFFFu,
                                        0xFFFFFFFFu, CSS_ORIGIN_INLINE);
            }
        }
        cur = build_custom_env(se, cur);
        if (inl)
            css_free(inl);
    }
    return cur;
}

static void free_env_chain(struct custom_env *top)
{
    while (top) {
        struct custom_env *p = (struct custom_env *)top->parent;
        free_custom_env(top);
        top = p;
    }
}

static struct computed_style *style_compute_internal(
    struct style_engine *se, void *elem, const struct computed_style *parent,
    const char *inline_css, int32_t root_font_size,
    const struct custom_env *parent_env, struct custom_env **out_env)
{
    struct computed_style *cs;
    struct computed_style initial;
    struct css_stylesheet *inl = 0;
    struct custom_env *local_env;
    const struct custom_env *prev_env = se->cur_env;
    int i, prop;

    cs = (struct computed_style *)malloc(sizeof(*cs));
    if (!cs)
        return 0;

    se->prefers_dark = 0;
    for (i = 0; i < se->nsheet; i++)
        if (se->sheets[i] && css_stylesheet_prefers_dark(se->sheets[i])) {
            se->prefers_dark = 1;
            break;
        }

    css_style_initial(&initial);
    memset(se->slots, 0, sizeof(se->slots));
    memset(se->spec, 0, sizeof(se->spec));
    se->ncscratch = 0;

    for (i = 0; i < se->nsheet; i++)
        if (se->sheets[i])
            css_match(se->sheets[i], elem, &se->ops, match_cb, se);

    if (inline_css && *inline_css) {
        inl = css_parse_style_attr(inline_css);
        if (inl) {
            const struct css_rule *r = css_rule_at(inl, 0);
            if (r) {
                for (i = 0; i < r->ndecl; i++)
                    consider(se, &r->decls[i], 0xFFFFFFFFu, 0xFFFFFFFFu,
                             CSS_ORIGIN_INLINE);
                for (i = 0; i < r->ncustom; i++)
                    consider_custom(se, &r->customs[i], 0xFFFFFFFFu,
                                    0xFFFFFFFFu, CSS_ORIGIN_INLINE);
            }
        }
    }

    local_env = build_custom_env(se, parent_env);
    se->cur_env = local_env;

    /* 1. initial values, 2. inherit what is inheritable, 3. font-size,
     * 4. color-scheme, 5. color, 6. every other specified value. */
    *cs = initial;
    if (parent)
        for (prop = 1; prop < CSS_PROP_COUNT; prop++)
            if (css_property_inherited(prop))
                copy_prop(cs, parent, prop);

    if (root_font_size <= 0)
        root_font_size = STYLE_DEFAULT_FONT_SIZE;
    apply_font_size(se, cs, parent,
                    se->slots[CSS_PROP_FONT_SIZE].used
                        ? &se->spec[CSS_PROP_FONT_SIZE] : 0,
                    root_font_size);

    /* color-scheme before color / light-dark() consumers */
    if (se->slots[CSS_PROP_COLOR_SCHEME].used)
        apply_value(se, cs, parent, &initial, CSS_PROP_COLOR_SCHEME,
                    &se->spec[CSS_PROP_COLOR_SCHEME], root_font_size);

    /* colour before anything that can say currentColor */
    if (se->slots[CSS_PROP_COLOR].used)
        apply_value(se, cs, parent, &initial, CSS_PROP_COLOR,
                    &se->spec[CSS_PROP_COLOR], root_font_size);

    for (prop = 1; prop < CSS_PROP_COUNT; prop++) {
        if (!se->slots[prop].used)
            continue;
        if (prop == CSS_PROP_FONT_SIZE || prop == CSS_PROP_COLOR_SCHEME ||
            prop == CSS_PROP_COLOR)
            continue;
        apply_value(se, cs, parent, &initial, prop, &se->spec[prop],
                    root_font_size);
    }

    /* A border with no style draws nothing, whatever its width says. */
    for (i = 0; i < 4; i++)
        if (cs->border_style[i] == CSS_BORDERSTYLE_NONE ||
            cs->border_style[i] == CSS_BORDERSTYLE_HIDDEN)
            cs->border_width[i] = 0;
    if (!se->slots[CSS_PROP_OUTLINE_COLOR].used)
        cs->outline_color = cs->color;
    if (cs->outline_style == CSS_BORDERSTYLE_NONE ||
        cs->outline_style == CSS_BORDERSTYLE_HIDDEN)
        cs->outline_width = 0;
    else if (!se->slots[CSS_PROP_OUTLINE_WIDTH].used)
        cs->outline_width = 3;

    if (cs->grid_col_start > 0 && cs->grid_col_end > cs->grid_col_start) {
        int32_t sp = cs->grid_col_end - cs->grid_col_start;
        if (sp > 255) sp = 255;
        cs->grid_col_span = (uint8_t)sp;
    }
    if (cs->grid_row_start > 0 && cs->grid_row_end > cs->grid_row_start) {
        int32_t sp = cs->grid_row_end - cs->grid_row_start;
        if (sp > 255) sp = 255;
        cs->grid_row_span = (uint8_t)sp;
    }

    if (inl)
        css_free(inl);

    se->cur_env = prev_env;
    if (out_env)
        *out_env = local_env;
    else if (local_env != parent_env)
        free_custom_env(local_env);
    return cs;
}

struct computed_style *style_compute(struct style_engine *se, void *elem,
                                     const struct computed_style *parent,
                                     const char *inline_css,
                                     int32_t root_font_size)
{
    struct custom_env *anc_env = 0;
    struct computed_style *cs;

    if (!se || !elem)
        return 0;
    if (!se->in_tree_walk && se->has_customs)
        anc_env = build_ancestor_env(se, elem);
    cs = style_compute_internal(se, elem, parent, inline_css, root_font_size,
                                anc_env ? anc_env : se->cur_env, 0);
    if (anc_env)
        free_env_chain(anc_env);
    return cs;
}

/* ================================================================== *
 * the tree walk
 * ================================================================== */

static void walk(struct style_engine *se, void *e,
                 const struct computed_style *parent,
                 const struct custom_env *parent_env, int depth,
                 int32_t root_fs, style_sink_fn sink, void *ctx)
{
    struct computed_style *cs;
    struct custom_env *elem_env = 0;
    const char *inline_css;
    void *child;

    if (!e || se->styled >= STYLE_MAX_ELEMENTS)
        return;
    inline_css = se->ops.attr ? se->ops.attr(e, "style") : 0;
    cs = style_compute_internal(se, e, parent, inline_css, root_fs,
                                parent_env, &elem_env);
    if (!cs)
        return;
    se->styled++;
    if (sink)
        sink(ctx, e, cs);

    if (depth + 1 >= CSS_MAX_TREE_DEPTH) {
        /* Past the cap we stop descending rather than risk the stack.
         * A document nested 128 elements deep is pathological. */
        if (elem_env && elem_env != parent_env)
            free_custom_env(elem_env);
        return;
    }
    if (!se->ops.first_child) {
        if (elem_env && elem_env != parent_env)
            free_custom_env(elem_env);
        return;
    }
    for (child = se->ops.first_child(e); child;
         child = se->ops.next ? se->ops.next(child) : 0) {
        walk(se, child, cs, elem_env, depth + 1, root_fs, sink, ctx);
        if (se->styled >= STYLE_MAX_ELEMENTS)
            break;
    }
    if (elem_env && elem_env != parent_env)
        free_custom_env(elem_env);
}

int style_compute_tree(struct style_engine *se, void *root,
                       const struct computed_style *initial,
                       style_sink_fn sink, void *ctx)
{
    int32_t root_fs = STYLE_DEFAULT_FONT_SIZE;

    if (!se || !root)
        return 0;
    se->styled = 0;
    se->in_tree_walk = 1;
    if (initial && initial->font_size > 0)
        root_fs = initial->font_size;
    walk(se, root, initial, 0, 0, root_fs, sink, ctx);
    se->in_tree_walk = 0;
    return se->styled;
}

/* ================================================================== *
 * the DOM binding
 *
 * The only part of the engine that includes dom.h. Compiled into the
 * browser; left out of the host tests, which supply their own ops.
 * ================================================================== */
#ifdef CSS_WITH_DOM

#include "dom.h"

#define DN(e) ((struct dom_node *)(e))

static const char *dom_ops_tag(void *e)
{
    return DN(e)->tag ? DN(e)->tag : "";
}

static const char *dom_ops_attr(void *e, const char *name)
{
    return dom_get_attr(DN(e), name);
}

static void *dom_ops_parent(void *e)
{
    return dom_parent_element(DN(e));
}

static void *dom_ops_prev(void *e)
{
    struct dom_node *n = DN(e)->prev_sibling;
    while (n && n->type != DOM_ELEMENT)
        n = n->prev_sibling;
    return n;
}

static void *dom_ops_next(void *e)
{
    return dom_next_element_sibling(DN(e));
}

static void *dom_ops_first_child(void *e)
{
    return dom_first_element_child(DN(e));
}

static const char *dom_ops_id(void *e)
{
    return dom_get_attr(DN(e), "id");
}

static int dom_ops_has_class(void *e, const char *cls)
{
    return dom_has_class(DN(e), cls);
}

static int dom_ops_is_empty(void *e)
{
    return DN(e)->first_child == 0;
}

static unsigned dom_ops_state(void *e)
{
    struct dom_node *n = DN(e);
    unsigned state = 0;

    if (dom_has_attr(n, "checked") || dom_has_attr(n, "selected"))
        state |= CSS_STATE_CHECKED;
    if (dom_has_attr(n, "disabled"))
        state |= CSS_STATE_DISABLED;
    return state;
}

const struct css_elem_ops *css_dom_ops(void)
{
    static const struct css_elem_ops ops = {
        dom_ops_tag, dom_ops_attr, dom_ops_parent, dom_ops_prev,
        dom_ops_next, dom_ops_first_child, dom_ops_id, dom_ops_has_class,
        dom_ops_state, dom_ops_is_empty
    };
    return &ops;
}

/* Hang the computed style off struct dom_node's reserved slot. The DOM
 * never reads or frees it, so css_style_dom_free() is the other half. */
void css_style_dom_sink(void *ctx, void *elem, struct computed_style *cs)
{
    (void)ctx;
    free(DN(elem)->style);
    DN(elem)->style = cs;
}

static void dom_free_styles(struct dom_node *n, int depth)
{
    struct dom_node *c;

    if (!n || depth > DOM_MAX_DEPTH)
        return;
    free(n->style);
    n->style = 0;
    for (c = n->first_child; c; c = c->next_sibling)
        dom_free_styles(c, depth + 1);
}

void css_style_dom_free(void *node)
{
    dom_free_styles(DN(node), 0);
}

#undef DN

#endif /* CSS_WITH_DOM */
