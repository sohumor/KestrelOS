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

#define STYLE_MAX_ELEMENTS 200000
#define STYLE_DEFAULT_FONT_SIZE 16
#define STYLE_MIN_FONT_SIZE 1
#define STYLE_MAX_FONT_SIZE 400

struct cascade_slot {
    uint8_t  used;
    uint8_t  level;
    uint32_t spec;
    uint32_t order;
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
    int styled;
};

/* ================================================================== *
 * initial values
 * ================================================================== */

static struct css_len len_auto(void)
{
    struct css_len l;
    l.v = 0;
    l.type = CSS_LEN_AUTO;
    return l;
}

static struct css_len len_px(int32_t px)
{
    struct css_len l;
    l.v = px;
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
    cs->font_weight = 400;
    cs->font_size = STYLE_DEFAULT_FONT_SIZE;
    cs->border_spacing = 0;
    cs->z_index = 0;
    cs->z_auto = 1;
    cs->vertical_align_px = 0;
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
    }
    cs->line_height.type = CSS_LEN_NORMAL;
    cs->text_indent = len_px(0);
    cs->color = CSS_RGB(0, 0, 0);
    cs->background_color = CSS_TRANSPARENT;
    cs->background_image = 0;
    cs->font_family_name = 0;
}

/* ================================================================== *
 * one property at a time
 *
 * copy_prop() is the only place that knows which fields belong to which
 * property. Automatic inheritance and the explicit `inherit` keyword are
 * both just a call to it, which is why they cannot disagree.
 * ================================================================== */

static void copy_prop(struct computed_style *d, const struct computed_style *s,
                      int prop)
{
    switch (prop) {
    case CSS_PROP_DISPLAY:        d->display = s->display; break;
    case CSS_PROP_POSITION:       d->position = s->position; break;
    case CSS_PROP_FLOAT:          d->css_float = s->css_float; break;
    case CSS_PROP_CLEAR:          d->clear = s->clear; break;
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
    case CSS_PROP_FONT_FAMILY:
        d->font_family = s->font_family;
        d->font_family_name = s->font_family_name;
        break;
    case CSS_PROP_FONT_SIZE:      d->font_size = s->font_size; break;
    case CSS_PROP_FONT_WEIGHT:    d->font_weight = s->font_weight; break;
    case CSS_PROP_FONT_STYLE:     d->font_style = s->font_style; break;
    case CSS_PROP_COLOR:          d->color = s->color; break;
    case CSS_PROP_BACKGROUND_COLOR: d->background_color = s->background_color; break;
    case CSS_PROP_BACKGROUND_IMAGE: d->background_image = s->background_image; break;
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

static int32_t round_milli(int64_t m)
{
    if (m >= 0)
        return clamp_px((m + 500) / 1000);
    return clamp_px(-((-m + 500) / 1000));
}

/* Resolve a length to whole pixels. `fs` is the font size em is relative
 * to. ex and ch are half the font size, which is exactly right for the
 * 8x16 bitmap face and close enough for anything else we could draw. */
static int32_t resolve_px(const struct style_engine *se,
                          const struct css_value *v, int32_t fs, int32_t root_fs)
{
    int64_t n = v->num;

    switch (v->unit) {
    case CSS_UNIT_PX:  return round_milli(n);
    case CSS_UNIT_EM:  return round_milli(n * fs);
    case CSS_UNIT_REM: return round_milli(n * root_fs);
    case CSS_UNIT_EX:  return round_milli(n * (fs / 2 > 0 ? fs / 2 : 1));
    case CSS_UNIT_CH:  return round_milli(n * (fs / 2 > 0 ? fs / 2 : 1));
    case CSS_UNIT_PT:  return round_milli(n * 4 / 3);
    case CSS_UNIT_PC:  return round_milli(n * 16);
    case CSS_UNIT_IN:  return round_milli(n * 96);
    case CSS_UNIT_CM:  return round_milli(n * 9600 / 254);
    case CSS_UNIT_MM:  return round_milli(n * 960 / 254);
    case CSS_UNIT_VW:  return clamp_px((int64_t)se->vw * n / 100000);
    case CSS_UNIT_VH:  return clamp_px((int64_t)se->vh * n / 100000);
    default:           return round_milli(n);
    }
}

/* A width/margin/padding style value: pixels where resolvable, and a
 * tagged percentage or auto where only layout can finish the job. */
static struct css_len to_len(const struct style_engine *se,
                             const struct css_value *v, int32_t fs,
                             int32_t root_fs)
{
    struct css_len l;

    l.v = 0;
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
        return;
    }
    if (v->type == CSS_VAL_UNSET) {
        /* `unset` is `inherit` on an inherited property and `initial`
         * on every other one - which is exactly what the inherit flag
         * in the property registry already knows. */
        if (css_property_inherited(prop))
            copy_prop(cs, parent ? parent : initial, prop);
        else
            copy_prop(cs, initial, prop);
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

    case CSS_PROP_COLOR:
        if (v->type == CSS_VAL_COLOR)
            cs->color = v->color;
        else if (v->type == CSS_VAL_IDENT && parent)
            cs->color = parent->color;
        break;
    case CSS_PROP_BACKGROUND_COLOR:
        if (v->type == CSS_VAL_COLOR)
            cs->background_color = v->color;
        else if (v->type == CSS_VAL_IDENT)
            cs->background_color = cs->color;
        break;
    case CSS_PROP_BACKGROUND_IMAGE:
        if (v->type == CSS_VAL_URL)
            cs->background_image = v->str;
        else
            cs->background_image = 0;
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

static void match_cb(void *ctx, const struct css_rule *r)
{
    struct style_engine *se = (struct style_engine *)ctx;
    int i;

    for (i = 0; i < r->ndecl; i++)
        consider(se, &r->decls[i], r->sel.specificity, r->order, r->origin);
}

struct style_engine *style_engine_new(struct css_stylesheet **sheets, int n,
                                      const struct css_elem_ops *ops)
{
    struct style_engine *se;
    uint32_t base = 0;
    int i;

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
    /* Later sheets must beat earlier ones on a specificity tie, so their
     * rules get strictly larger source-order numbers. */
    for (i = 0; i < n; i++)
        if (se->sheets[i])
            base = css_set_order_base(se->sheets[i], base);
    return se;
}

void style_engine_free(struct style_engine *se)
{
    if (!se)
        return;
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

struct computed_style *style_compute(struct style_engine *se, void *elem,
                                     const struct computed_style *parent,
                                     const char *inline_css,
                                     int32_t root_font_size)
{
    struct computed_style *cs;
    struct computed_style initial;
    struct css_stylesheet *inl = 0;
    int i, prop;

    if (!se || !elem)
        return 0;
    cs = (struct computed_style *)malloc(sizeof(*cs));
    if (!cs)
        return 0;

    css_style_initial(&initial);
    memset(se->slots, 0, sizeof(se->slots));
    memset(se->spec, 0, sizeof(se->spec));

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
            }
        }
    }

    /* 1. initial values, 2. inherit what is inheritable, 3. font-size,
     * 4. every other specified value. */
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

    /* colour before anything that can say currentColor */
    if (se->slots[CSS_PROP_COLOR].used)
        apply_value(se, cs, parent, &initial, CSS_PROP_COLOR,
                    &se->spec[CSS_PROP_COLOR], root_font_size);

    for (prop = 1; prop < CSS_PROP_COUNT; prop++) {
        if (!se->slots[prop].used)
            continue;
        if (prop == CSS_PROP_FONT_SIZE || prop == CSS_PROP_COLOR)
            continue;
        apply_value(se, cs, parent, &initial, prop, &se->spec[prop],
                    root_font_size);
    }

    /* A border with no style draws nothing, whatever its width says. */
    for (i = 0; i < 4; i++)
        if (cs->border_style[i] == CSS_BORDERSTYLE_NONE ||
            cs->border_style[i] == CSS_BORDERSTYLE_HIDDEN)
            cs->border_width[i] = 0;

    if (inl)
        css_free(inl);
    return cs;
}

/* ================================================================== *
 * the tree walk
 * ================================================================== */

static void walk(struct style_engine *se, void *e,
                 const struct computed_style *parent, int depth,
                 int32_t root_fs, style_sink_fn sink, void *ctx)
{
    struct computed_style *cs;
    const char *inline_css;
    void *child;

    if (!e || se->styled >= STYLE_MAX_ELEMENTS)
        return;
    inline_css = se->ops.attr ? se->ops.attr(e, "style") : 0;
    cs = style_compute(se, e, parent, inline_css, root_fs);
    if (!cs)
        return;
    se->styled++;
    if (sink)
        sink(ctx, e, cs);

    if (depth + 1 >= CSS_MAX_TREE_DEPTH) {
        /* Past the cap we stop descending rather than risk the stack.
         * A document nested 128 elements deep is pathological. */
        return;
    }
    if (!se->ops.first_child)
        return;
    for (child = se->ops.first_child(e); child;
         child = se->ops.next ? se->ops.next(child) : 0) {
        walk(se, child, cs, depth + 1, root_fs, sink, ctx);
        if (se->styled >= STYLE_MAX_ELEMENTS)
            return;
    }
}

int style_compute_tree(struct style_engine *se, void *root,
                       const struct computed_style *initial,
                       style_sink_fn sink, void *ctx)
{
    int32_t root_fs = STYLE_DEFAULT_FONT_SIZE;

    if (!se || !root)
        return 0;
    se->styled = 0;
    if (initial && initial->font_size > 0)
        root_fs = initial->font_size;
    walk(se, root, initial, 0, root_fs, sink, ctx);
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

const struct css_elem_ops *css_dom_ops(void)
{
    static const struct css_elem_ops ops = {
        dom_ops_tag, dom_ops_attr, dom_ops_parent, dom_ops_prev,
        dom_ops_next, dom_ops_first_child, dom_ops_id, dom_ops_has_class,
        0, dom_ops_is_empty
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
