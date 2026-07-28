/* css.h - the KestrelOS CSS engine: tokenizer, parser, cascade.
 *
 * Scope: CSS 2.1 plus the CSS3 bits the real web actually leans on
 * (attribute substring selectors, the sibling combinators, :not(),
 * :nth-child(), rgba/hsl colours, rem units, @media width queries).
 *
 * Like html.h this file is free of every KestrelOS dependency: malloc,
 * free and the mem/str routines are all it needs, so the same source
 * builds against the host libc for tools/test_css.c and against the
 * KestrelOS libc for the browser. There is no floating point anywhere:
 * fractional CSS numbers are carried as fixed-point thousandths.
 *
 * Three layers, each usable on its own:
 *
 *   css_parse()          bytes -> struct css_stylesheet. Never fails on
 *                        malformed input: CSS error recovery drops the
 *                        bad declaration or block and keeps the rest.
 *   css_match()          a stylesheet + an element -> the rules that
 *                        match, via a rightmost-simple-selector index so
 *                        a 20 000 rule sheet does not turn matching into
 *                        an O(elements * rules) scan.
 *   style_compute_tree() (style.c) UA + author sheets + inline styles ->
 *                        a struct computed_style per element, cascaded,
 *                        inherited and with lengths resolved.
 *
 * The engine never touches the DOM directly. It reaches an element
 * through struct css_elem_ops, so the same matcher drives the real DOM,
 * the test harness's toy tree, and anything else later. That is the
 * house modularity rule: a registry of operations, not a hard-wired call.
 */

#pragma once

#include <stdint.h>

/* ------------------------------------------------------------------ *
 * Hard limits. Every one of these exists so that a hostile or merely
 * enormous stylesheet degrades instead of exhausting the 64 KiB user
 * stack or the brk heap. Hitting any of them sets css_truncated().
 * ------------------------------------------------------------------ */
#define CSS_MAX_SOURCE        (2UL * 1024UL * 1024UL) /* bytes per sheet   */
#define CSS_MAX_RULES         20000    /* selectors, after list splitting  */
#define CSS_MAX_DECLS         120000   /* declarations across the sheet    */
#define CSS_MAX_ARENA         (8UL * 1024UL * 1024UL) /* strings + nodes   */
#define CSS_MAX_COMPOUNDS     16       /* "a b c d" is 4 compounds         */
#define CSS_MAX_SIMPLES       16       /* simples in one compound          */
#define CSS_MAX_BLOCK_DECLS   512      /* declarations in one { }          */
#define CSS_MAX_SEL_LIST      256      /* selectors in one comma list      */
#define CSS_MAX_COMPONENTS    16       /* component values in one value    */
#define CSS_MAX_MEDIA         256      /* @media blocks per sheet          */
#define CSS_MAX_MEDIA_FEATS   8        /* features in one media query      */
#define CSS_MAX_IMPORTS       32       /* @import urls reported            */
#define CSS_MAX_IDENT         256      /* bytes in one identifier/string   */
#define CSS_MAX_NEST          64       /* block nesting during recovery    */
#define CSS_MAX_TREE_DEPTH    256      /* element depth style.c will walk  */

/* ------------------------------------------------------------------ *
 * Fixed point. CSS numbers are carried as thousandths so "1.5em" is
 * 1500 and "0.375" is 375. There is no libm and no float here.
 * ------------------------------------------------------------------ */
typedef int32_t css_num;
#define CSS_NUM_ONE 1000

/* ------------------------------------------------------------------ *
 * Colours. Stored 0xAARRGGBB: the framebuffer has no alpha channel, so
 * the alpha byte is advisory - the painter may ignore it, but it has to
 * be carried because "transparent" and rgba(0,0,0,0) must be
 * distinguishable from opaque black.
 * ------------------------------------------------------------------ */
#define CSS_RGB(r, g, b)   (0xFF000000u | ((uint32_t)(r) << 16) | \
                            ((uint32_t)(g) << 8) | (uint32_t)(b))
#define CSS_COLOR_A(c)     (((c) >> 24) & 0xFFu)
#define CSS_COLOR_R(c)     (((c) >> 16) & 0xFFu)
#define CSS_COLOR_G(c)     (((c) >> 8) & 0xFFu)
#define CSS_COLOR_B(c)     ((c) & 0xFFu)
#define CSS_COLOR_RGB(c)   ((c) & 0x00FFFFFFu)
#define CSS_TRANSPARENT    0x00000000u

/* ------------------------------------------------------------------ *
 * Keyword registries.
 *
 * Each enumerated property declares its accepted keywords once, as an
 * X-list. The enum constants and the string table are both generated
 * from it, so the parser's idea of "block" and the computed style's
 * CSS_DISPLAY_BLOCK can never drift apart. Order is significant: the
 * enum value IS the index into the string table.
 * ------------------------------------------------------------------ */
#define CSS_ENUM_X(n, s) n,

#define CSS_DISPLAY_LIST(X) \
    X(CSS_DISPLAY_INLINE,             "inline") \
    X(CSS_DISPLAY_BLOCK,              "block") \
    X(CSS_DISPLAY_INLINE_BLOCK,       "inline-block") \
    X(CSS_DISPLAY_LIST_ITEM,          "list-item") \
    X(CSS_DISPLAY_NONE,               "none") \
    X(CSS_DISPLAY_TABLE,              "table") \
    X(CSS_DISPLAY_INLINE_TABLE,       "inline-table") \
    X(CSS_DISPLAY_TABLE_ROW,          "table-row") \
    X(CSS_DISPLAY_TABLE_CELL,         "table-cell") \
    X(CSS_DISPLAY_TABLE_ROW_GROUP,    "table-row-group") \
    X(CSS_DISPLAY_TABLE_HEADER_GROUP, "table-header-group") \
    X(CSS_DISPLAY_TABLE_FOOTER_GROUP, "table-footer-group") \
    X(CSS_DISPLAY_TABLE_COLUMN,       "table-column") \
    X(CSS_DISPLAY_TABLE_COLUMN_GROUP, "table-column-group") \
    X(CSS_DISPLAY_TABLE_CAPTION,      "table-caption") \
    X(CSS_DISPLAY_FLEX,               "flex") \
    X(CSS_DISPLAY_INLINE_FLEX,        "inline-flex") \
    X(CSS_DISPLAY_GRID,               "grid") \
    X(CSS_DISPLAY_INLINE_GRID,        "inline-grid")
enum { CSS_DISPLAY_LIST(CSS_ENUM_X) CSS_DISPLAY_COUNT };

#define CSS_FLEXDIR_LIST(X) \
    X(CSS_FLEXDIR_ROW,            "row") \
    X(CSS_FLEXDIR_ROW_REVERSE,    "row-reverse") \
    X(CSS_FLEXDIR_COLUMN,         "column") \
    X(CSS_FLEXDIR_COLUMN_REVERSE, "column-reverse")
enum { CSS_FLEXDIR_LIST(CSS_ENUM_X) CSS_FLEXDIR_COUNT };

#define CSS_JUSTIFY_LIST(X) \
    X(CSS_JUSTIFY_START,         "flex-start") \
    X(CSS_JUSTIFY_END,           "flex-end") \
    X(CSS_JUSTIFY_CENTER,        "center") \
    X(CSS_JUSTIFY_SPACE_BETWEEN, "space-between") \
    X(CSS_JUSTIFY_SPACE_AROUND,  "space-around") \
    X(CSS_JUSTIFY_SPACE_EVENLY,  "space-evenly")
enum { CSS_JUSTIFY_LIST(CSS_ENUM_X) CSS_JUSTIFY_COUNT };

#define CSS_ALIGN_LIST(X) \
    X(CSS_ALIGN_STRETCH, "stretch") \
    X(CSS_ALIGN_START,   "flex-start") \
    X(CSS_ALIGN_END,     "flex-end") \
    X(CSS_ALIGN_CENTER,  "center") \
    X(CSS_ALIGN_BASELINE,"baseline")
enum { CSS_ALIGN_LIST(CSS_ENUM_X) CSS_ALIGN_COUNT };

#define CSS_POSITION_LIST(X) \
    X(CSS_POSITION_STATIC,   "static") \
    X(CSS_POSITION_RELATIVE, "relative") \
    X(CSS_POSITION_ABSOLUTE, "absolute") \
    X(CSS_POSITION_FIXED,    "fixed") \
    X(CSS_POSITION_STICKY,   "sticky")
enum { CSS_POSITION_LIST(CSS_ENUM_X) CSS_POSITION_COUNT };

#define CSS_FLOAT_LIST(X) \
    X(CSS_FLOAT_NONE,  "none") \
    X(CSS_FLOAT_LEFT,  "left") \
    X(CSS_FLOAT_RIGHT, "right")
enum { CSS_FLOAT_LIST(CSS_ENUM_X) CSS_FLOAT_COUNT };

#define CSS_CLEAR_LIST(X) \
    X(CSS_CLEAR_NONE,  "none") \
    X(CSS_CLEAR_LEFT,  "left") \
    X(CSS_CLEAR_RIGHT, "right") \
    X(CSS_CLEAR_BOTH,  "both")
enum { CSS_CLEAR_LIST(CSS_ENUM_X) CSS_CLEAR_COUNT };

#define CSS_TEXTALIGN_LIST(X) \
    X(CSS_TEXTALIGN_LEFT,    "left") \
    X(CSS_TEXTALIGN_RIGHT,   "right") \
    X(CSS_TEXTALIGN_CENTER,  "center") \
    X(CSS_TEXTALIGN_JUSTIFY, "justify") \
    X(CSS_TEXTALIGN_START,   "start") \
    X(CSS_TEXTALIGN_END,     "end")
enum { CSS_TEXTALIGN_LIST(CSS_ENUM_X) CSS_TEXTALIGN_COUNT };

#define CSS_VALIGN_LIST(X) \
    X(CSS_VALIGN_BASELINE,    "baseline") \
    X(CSS_VALIGN_SUB,         "sub") \
    X(CSS_VALIGN_SUPER,       "super") \
    X(CSS_VALIGN_TOP,         "top") \
    X(CSS_VALIGN_TEXT_TOP,    "text-top") \
    X(CSS_VALIGN_MIDDLE,      "middle") \
    X(CSS_VALIGN_BOTTOM,      "bottom") \
    X(CSS_VALIGN_TEXT_BOTTOM, "text-bottom") \
    X(CSS_VALIGN_LENGTH,      "\1length")
enum { CSS_VALIGN_LIST(CSS_ENUM_X) CSS_VALIGN_COUNT };

#define CSS_WHITESPACE_LIST(X) \
    X(CSS_WHITESPACE_NORMAL,   "normal") \
    X(CSS_WHITESPACE_PRE,      "pre") \
    X(CSS_WHITESPACE_NOWRAP,   "nowrap") \
    X(CSS_WHITESPACE_PRE_WRAP, "pre-wrap") \
    X(CSS_WHITESPACE_PRE_LINE, "pre-line")
enum { CSS_WHITESPACE_LIST(CSS_ENUM_X) CSS_WHITESPACE_COUNT };

#define CSS_OVERFLOW_LIST(X) \
    X(CSS_OVERFLOW_VISIBLE, "visible") \
    X(CSS_OVERFLOW_HIDDEN,  "hidden") \
    X(CSS_OVERFLOW_SCROLL,  "scroll") \
    X(CSS_OVERFLOW_AUTO,    "auto") \
    X(CSS_OVERFLOW_CLIP,    "clip")
enum { CSS_OVERFLOW_LIST(CSS_ENUM_X) CSS_OVERFLOW_COUNT };

#define CSS_VISIBILITY_LIST(X) \
    X(CSS_VISIBILITY_VISIBLE,  "visible") \
    X(CSS_VISIBILITY_HIDDEN,   "hidden") \
    X(CSS_VISIBILITY_COLLAPSE, "collapse")
enum { CSS_VISIBILITY_LIST(CSS_ENUM_X) CSS_VISIBILITY_COUNT };

#define CSS_FONTSTYLE_LIST(X) \
    X(CSS_FONTSTYLE_NORMAL,  "normal") \
    X(CSS_FONTSTYLE_ITALIC,  "italic") \
    X(CSS_FONTSTYLE_OBLIQUE, "oblique")
enum { CSS_FONTSTYLE_LIST(CSS_ENUM_X) CSS_FONTSTYLE_COUNT };

/* The font family collapses to a class, because the framebuffer has one
 * bitmap face in a few weights: which class is picked is all the painter
 * can act on. The concrete first family name is kept alongside it. */
#define CSS_FONTFAMILY_LIST(X) \
    X(CSS_FONTFAMILY_SERIF,     "serif") \
    X(CSS_FONTFAMILY_SANS,      "sans-serif") \
    X(CSS_FONTFAMILY_MONO,      "monospace") \
    X(CSS_FONTFAMILY_CURSIVE,   "cursive") \
    X(CSS_FONTFAMILY_FANTASY,   "fantasy") \
    X(CSS_FONTFAMILY_SYSTEM,    "system-ui")
enum { CSS_FONTFAMILY_LIST(CSS_ENUM_X) CSS_FONTFAMILY_COUNT };

#define CSS_BORDERSTYLE_LIST(X) \
    X(CSS_BORDERSTYLE_NONE,   "none") \
    X(CSS_BORDERSTYLE_HIDDEN, "hidden") \
    X(CSS_BORDERSTYLE_DOTTED, "dotted") \
    X(CSS_BORDERSTYLE_DASHED, "dashed") \
    X(CSS_BORDERSTYLE_SOLID,  "solid") \
    X(CSS_BORDERSTYLE_DOUBLE, "double") \
    X(CSS_BORDERSTYLE_GROOVE, "groove") \
    X(CSS_BORDERSTYLE_RIDGE,  "ridge") \
    X(CSS_BORDERSTYLE_INSET,  "inset") \
    X(CSS_BORDERSTYLE_OUTSET, "outset")
enum { CSS_BORDERSTYLE_LIST(CSS_ENUM_X) CSS_BORDERSTYLE_COUNT };

#define CSS_LISTSTYLE_LIST(X) \
    X(CSS_LISTSTYLE_DISC,                "disc") \
    X(CSS_LISTSTYLE_CIRCLE,              "circle") \
    X(CSS_LISTSTYLE_SQUARE,              "square") \
    X(CSS_LISTSTYLE_DECIMAL,             "decimal") \
    X(CSS_LISTSTYLE_DECIMAL_LEADING_ZERO,"decimal-leading-zero") \
    X(CSS_LISTSTYLE_LOWER_ROMAN,         "lower-roman") \
    X(CSS_LISTSTYLE_UPPER_ROMAN,         "upper-roman") \
    X(CSS_LISTSTYLE_LOWER_ALPHA,         "lower-alpha") \
    X(CSS_LISTSTYLE_UPPER_ALPHA,         "upper-alpha") \
    X(CSS_LISTSTYLE_LOWER_LATIN,         "lower-latin") \
    X(CSS_LISTSTYLE_UPPER_LATIN,         "upper-latin") \
    X(CSS_LISTSTYLE_NONE,                "none")
enum { CSS_LISTSTYLE_LIST(CSS_ENUM_X) CSS_LISTSTYLE_COUNT };

#define CSS_LISTPOS_LIST(X) \
    X(CSS_LISTPOS_OUTSIDE, "outside") \
    X(CSS_LISTPOS_INSIDE,  "inside")
enum { CSS_LISTPOS_LIST(CSS_ENUM_X) CSS_LISTPOS_COUNT };

#define CSS_BORDERCOLLAPSE_LIST(X) \
    X(CSS_BORDERCOLLAPSE_SEPARATE, "separate") \
    X(CSS_BORDERCOLLAPSE_COLLAPSE, "collapse")
enum { CSS_BORDERCOLLAPSE_LIST(CSS_ENUM_X) CSS_BORDERCOLLAPSE_COUNT };

#define CSS_TEXTTRANSFORM_LIST(X) \
    X(CSS_TEXTTRANSFORM_NONE,       "none") \
    X(CSS_TEXTTRANSFORM_CAPITALIZE, "capitalize") \
    X(CSS_TEXTTRANSFORM_UPPERCASE,  "uppercase") \
    X(CSS_TEXTTRANSFORM_LOWERCASE,  "lowercase")
enum { CSS_TEXTTRANSFORM_LIST(CSS_ENUM_X) CSS_TEXTTRANSFORM_COUNT };

/* font-size keywords survive parsing because "smaller" and "larger" need
 * the parent's font size, which only the cascade knows. */
#define CSS_FONTSIZEKW_LIST(X) \
    X(CSS_FONTSIZEKW_XX_SMALL, "xx-small") \
    X(CSS_FONTSIZEKW_X_SMALL,  "x-small") \
    X(CSS_FONTSIZEKW_SMALL,    "small") \
    X(CSS_FONTSIZEKW_MEDIUM,   "medium") \
    X(CSS_FONTSIZEKW_LARGE,    "large") \
    X(CSS_FONTSIZEKW_X_LARGE,  "x-large") \
    X(CSS_FONTSIZEKW_XX_LARGE, "xx-large") \
    X(CSS_FONTSIZEKW_SMALLER,  "smaller") \
    X(CSS_FONTSIZEKW_LARGER,   "larger")
enum { CSS_FONTSIZEKW_LIST(CSS_ENUM_X) CSS_FONTSIZEKW_COUNT };

/* Likewise font-weight: bolder/lighter are relative to the parent. */
#define CSS_FONTWEIGHTKW_LIST(X) \
    X(CSS_FONTWEIGHTKW_NORMAL,  "normal") \
    X(CSS_FONTWEIGHTKW_BOLD,    "bold") \
    X(CSS_FONTWEIGHTKW_BOLDER,  "bolder") \
    X(CSS_FONTWEIGHTKW_LIGHTER, "lighter")
enum { CSS_FONTWEIGHTKW_LIST(CSS_ENUM_X) CSS_FONTWEIGHTKW_COUNT };

/* text-decoration is a bitmask, not an enumeration. */
#define CSS_DECOR_NONE      0x00u
#define CSS_DECOR_UNDERLINE 0x01u
#define CSS_DECOR_OVERLINE  0x02u
#define CSS_DECOR_LINETHRU  0x04u
#define CSS_DECOR_BLINK     0x08u

/* ------------------------------------------------------------------ *
 * Properties.
 *
 * Only longhands are stored. Every shorthand is expanded by the parser,
 * so the cascade and the computed-style builder never see one.
 * ------------------------------------------------------------------ */
#define CSS_PROP_LIST(X) \
    X(CSS_PROP_NONE,                "") \
    X(CSS_PROP_DISPLAY,             "display") \
    X(CSS_PROP_POSITION,            "position") \
    X(CSS_PROP_FLOAT,               "float") \
    X(CSS_PROP_CLEAR,               "clear") \
    X(CSS_PROP_WIDTH,               "width") \
    X(CSS_PROP_HEIGHT,              "height") \
    X(CSS_PROP_MIN_WIDTH,           "min-width") \
    X(CSS_PROP_MAX_WIDTH,           "max-width") \
    X(CSS_PROP_MIN_HEIGHT,          "min-height") \
    X(CSS_PROP_MAX_HEIGHT,          "max-height") \
    X(CSS_PROP_TOP,                 "top") \
    X(CSS_PROP_RIGHT,               "right") \
    X(CSS_PROP_BOTTOM,              "bottom") \
    X(CSS_PROP_LEFT,                "left") \
    X(CSS_PROP_MARGIN_TOP,          "margin-top") \
    X(CSS_PROP_MARGIN_RIGHT,        "margin-right") \
    X(CSS_PROP_MARGIN_BOTTOM,       "margin-bottom") \
    X(CSS_PROP_MARGIN_LEFT,         "margin-left") \
    X(CSS_PROP_PADDING_TOP,         "padding-top") \
    X(CSS_PROP_PADDING_RIGHT,       "padding-right") \
    X(CSS_PROP_PADDING_BOTTOM,      "padding-bottom") \
    X(CSS_PROP_PADDING_LEFT,        "padding-left") \
    X(CSS_PROP_BORDER_TOP_WIDTH,    "border-top-width") \
    X(CSS_PROP_BORDER_RIGHT_WIDTH,  "border-right-width") \
    X(CSS_PROP_BORDER_BOTTOM_WIDTH, "border-bottom-width") \
    X(CSS_PROP_BORDER_LEFT_WIDTH,   "border-left-width") \
    X(CSS_PROP_BORDER_TOP_STYLE,    "border-top-style") \
    X(CSS_PROP_BORDER_RIGHT_STYLE,  "border-right-style") \
    X(CSS_PROP_BORDER_BOTTOM_STYLE, "border-bottom-style") \
    X(CSS_PROP_BORDER_LEFT_STYLE,   "border-left-style") \
    X(CSS_PROP_BORDER_TOP_COLOR,    "border-top-color") \
    X(CSS_PROP_BORDER_RIGHT_COLOR,  "border-right-color") \
    X(CSS_PROP_BORDER_BOTTOM_COLOR, "border-bottom-color") \
    X(CSS_PROP_BORDER_LEFT_COLOR,   "border-left-color") \
    X(CSS_PROP_FONT_FAMILY,         "font-family") \
    X(CSS_PROP_FONT_SIZE,           "font-size") \
    X(CSS_PROP_FONT_WEIGHT,         "font-weight") \
    X(CSS_PROP_FONT_STYLE,          "font-style") \
    X(CSS_PROP_COLOR,               "color") \
    X(CSS_PROP_BACKGROUND_COLOR,    "background-color") \
    X(CSS_PROP_BACKGROUND_IMAGE,    "background-image") \
    X(CSS_PROP_TEXT_ALIGN,          "text-align") \
    X(CSS_PROP_TEXT_DECORATION,     "text-decoration") \
    X(CSS_PROP_TEXT_INDENT,         "text-indent") \
    X(CSS_PROP_TEXT_TRANSFORM,      "text-transform") \
    X(CSS_PROP_VERTICAL_ALIGN,      "vertical-align") \
    X(CSS_PROP_LINE_HEIGHT,         "line-height") \
    X(CSS_PROP_LIST_STYLE_TYPE,     "list-style-type") \
    X(CSS_PROP_LIST_STYLE_POSITION, "list-style-position") \
    X(CSS_PROP_WHITE_SPACE,         "white-space") \
    X(CSS_PROP_OVERFLOW,            "overflow") \
    X(CSS_PROP_VISIBILITY,          "visibility") \
    X(CSS_PROP_BORDER_COLLAPSE,     "border-collapse") \
    X(CSS_PROP_BORDER_SPACING,      "border-spacing") \
    X(CSS_PROP_Z_INDEX,             "z-index") \
    X(CSS_PROP_FLEX_DIRECTION,      "flex-direction") \
    X(CSS_PROP_JUSTIFY_CONTENT,     "justify-content") \
    X(CSS_PROP_ALIGN_ITEMS,         "align-items") \
    X(CSS_PROP_FLEX_GROW,           "flex-grow") \
    X(CSS_PROP_GAP,                 "gap")
enum { CSS_PROP_LIST(CSS_ENUM_X) CSS_PROP_COUNT };

/* ------------------------------------------------------------------ *
 * Values.
 * ------------------------------------------------------------------ */
enum {
    CSS_VAL_NONE = 0,   /* empty slot                                   */
    CSS_VAL_KEYWORD,    /* .kw is the property's keyword index          */
    CSS_VAL_LENGTH,     /* .num thousandths, .unit says which           */
    CSS_VAL_NUMBER,     /* .num thousandths, unitless                   */
    CSS_VAL_PERCENT,    /* .num thousandths of a percent (50% = 50000)  */
    CSS_VAL_COLOR,      /* .color                                       */
    CSS_VAL_STRING,     /* .str                                         */
    CSS_VAL_URL,        /* .str                                         */
    CSS_VAL_IDENT,      /* .str, an identifier no keyword table claimed */
    CSS_VAL_AUTO,
    CSS_VAL_INHERIT,
    CSS_VAL_INITIAL,
    CSS_VAL_BITS,       /* .kw is a bitmask (text-decoration)           */
    CSS_VAL_UNSET       /* inherit if the property inherits, else initial */
};

enum {
    CSS_UNIT_NONE = 0,
    CSS_UNIT_PX, CSS_UNIT_EM, CSS_UNIT_REM, CSS_UNIT_EX, CSS_UNIT_CH,
    CSS_UNIT_PT, CSS_UNIT_PC, CSS_UNIT_CM, CSS_UNIT_MM, CSS_UNIT_IN,
    CSS_UNIT_VW, CSS_UNIT_VH,
    CSS_UNIT_COUNT
};

struct css_value {
    uint8_t  type;      /* CSS_VAL_*                                     */
    uint8_t  unit;      /* CSS_UNIT_*                                    */
    int16_t  kw;        /* keyword index / bitmask                       */
    css_num  num;       /* thousandths                                   */
    uint32_t color;     /* 0xAARRGGBB                                    */
    const char *str;    /* arena-owned, NUL terminated, or 0             */
};

struct css_decl {
    uint16_t prop;      /* CSS_PROP_*                                    */
    uint8_t  important;
    uint8_t  pad;
    struct css_value val;
};

/* ------------------------------------------------------------------ *
 * Selectors.
 * ------------------------------------------------------------------ */
enum {
    CSS_SIMPLE_UNIVERSAL = 0,
    CSS_SIMPLE_TYPE,
    CSS_SIMPLE_CLASS,
    CSS_SIMPLE_ID,
    CSS_SIMPLE_ATTR,
    CSS_SIMPLE_PSEUDO
};

enum {
    CSS_ATTR_EXISTS = 0, /* [a]      */
    CSS_ATTR_EQ,         /* [a=v]    */
    CSS_ATTR_INCLUDES,   /* [a~=v]   */
    CSS_ATTR_DASH,       /* [a|=v]   */
    CSS_ATTR_PREFIX,     /* [a^=v]   */
    CSS_ATTR_SUFFIX,     /* [a$=v]   */
    CSS_ATTR_SUBSTR      /* [a*=v]   */
};

enum {
    CSS_PSEUDO_FIRST_CHILD = 0,
    CSS_PSEUDO_LAST_CHILD,
    CSS_PSEUDO_ONLY_CHILD,
    CSS_PSEUDO_NTH_CHILD,
    CSS_PSEUDO_NTH_LAST_CHILD,
    CSS_PSEUDO_FIRST_OF_TYPE,
    CSS_PSEUDO_LAST_OF_TYPE,
    CSS_PSEUDO_EMPTY,
    CSS_PSEUDO_ROOT,
    CSS_PSEUDO_LINK,
    CSS_PSEUDO_VISITED,
    CSS_PSEUDO_HOVER,
    CSS_PSEUDO_ACTIVE,
    CSS_PSEUDO_FOCUS,
    CSS_PSEUDO_CHECKED,
    CSS_PSEUDO_DISABLED,
    CSS_PSEUDO_ENABLED,
    CSS_PSEUDO_NOT,
    CSS_PSEUDO_COUNT
};

/* Dynamic element state, supplied by the caller through css_elem_ops. */
#define CSS_STATE_HOVER    0x01u
#define CSS_STATE_ACTIVE   0x02u
#define CSS_STATE_FOCUS    0x04u
#define CSS_STATE_VISITED  0x08u
#define CSS_STATE_CHECKED  0x10u
#define CSS_STATE_DISABLED 0x20u

enum {
    CSS_COMB_NONE = 0,   /* first compound of the selector */
    CSS_COMB_DESCENDANT, /* "a b"  */
    CSS_COMB_CHILD,      /* "a > b"*/
    CSS_COMB_ADJACENT,   /* "a + b"*/
    CSS_COMB_SIBLING     /* "a ~ b"*/
};

struct css_compound;

struct css_simple {
    uint8_t kind;        /* CSS_SIMPLE_*  */
    uint8_t op;          /* CSS_ATTR_*    */
    uint8_t pseudo;      /* CSS_PSEUDO_*  */
    uint8_t negated;     /* inside :not() */
    int32_t a, b;        /* :nth-child(an+b) */
    const char *name;    /* tag / class / id / attribute name */
    const char *value;   /* attribute value */
    const struct css_compound *sub; /* :not() argument */
};

struct css_compound {
    const struct css_simple *simples;
    int n;
    uint8_t combinator;  /* links this compound to the one on its LEFT */
};

struct css_selector {
    const struct css_compound *parts;
    int nparts;
    uint32_t specificity; /* (a << 20) | (b << 10) | c, each capped 1023 */
};

/* ------------------------------------------------------------------ *
 * Rules and stylesheets.
 * ------------------------------------------------------------------ */
enum { CSS_ORIGIN_UA = 0, CSS_ORIGIN_USER, CSS_ORIGIN_AUTHOR, CSS_ORIGIN_INLINE };

struct css_rule {
    struct css_selector sel;
    const struct css_decl *decls;
    int ndecl;
    uint32_t order;      /* global source order, assigned by the caller */
    int16_t media;       /* index into the sheet's media blocks, or -1  */
    uint8_t origin;
};

/* The environment a media query is evaluated against. */
struct css_media {
    int32_t width;       /* viewport width in px                        */
    int32_t height;      /* viewport height in px                       */
    int32_t dpi;         /* resolution, 96 unless the caller knows       */
    uint8_t screen;      /* 1 = "screen" (so "print" blocks are skipped) */
    uint8_t monochrome;
};

struct css_stylesheet;

/* Parse a stylesheet. `origin` is CSS_ORIGIN_*; `media` may be 0, in
 * which case a default 1024x768 screen is assumed. Returns 0 only when
 * the very first allocation fails - malformed input never fails. */
struct css_stylesheet *css_parse(const char *src, unsigned long len,
                                 int origin, const struct css_media *media);

/* Parse the contents of a style="" attribute into a one-rule sheet whose
 * single rule has a universal selector and CSS_ORIGIN_INLINE. */
struct css_stylesheet *css_parse_style_attr(const char *text);

void css_free(struct css_stylesheet *ss);

int  css_rule_count(const struct css_stylesheet *ss);
const struct css_rule *css_rule_at(const struct css_stylesheet *ss, int i);
int  css_truncated(const struct css_stylesheet *ss);
unsigned long css_memory_used(const struct css_stylesheet *ss);

/* @import urls, in source order, for the caller to fetch and parse. */
int  css_import_count(const struct css_stylesheet *ss);
const char *css_import_url(const struct css_stylesheet *ss, int i);

/* Renumber every rule's source order so rules from several sheets sort
 * correctly against each other. Returns the next free order value. */
uint32_t css_set_order_base(struct css_stylesheet *ss, uint32_t base);

/* Re-evaluate the sheet's @media blocks against a new environment (a
 * window resize). Returns the number of blocks whose state changed. */
int css_set_media(struct css_stylesheet *ss, const struct css_media *m);

/* ------------------------------------------------------------------ *
 * The element interface.
 *
 * Everything the matcher needs to know about a document node. All the
 * pointers except tag/parent may be 0; the matcher falls back to attr().
 * Sibling walks must skip non-element nodes.
 * ------------------------------------------------------------------ */
struct css_elem_ops {
    const char *(*tag)(void *e);                     /* lowercase, never 0 */
    const char *(*attr)(void *e, const char *name);  /* or 0               */
    void *(*parent)(void *e);                        /* element parent     */
    void *(*prev)(void *e);                          /* prev element sib   */
    void *(*next)(void *e);                          /* next element sib   */
    void *(*first_child)(void *e);                   /* optional           */
    const char *(*id)(void *e);                      /* optional           */
    int  (*has_class)(void *e, const char *cls);     /* optional           */
    unsigned (*state)(void *e);                      /* optional, CSS_STATE_*/
    /* 1 when the element has no child nodes at all - text included, which
     * first_child() cannot tell you because it skips to elements. Without
     * it :empty is never satisfied. Optional. */
    int  (*is_empty)(void *e);
};

/* Does one selector match this element? */
int css_selector_matches(const struct css_selector *sel, void *e,
                         const struct css_elem_ops *ops);

/* Report every rule of `ss` that matches `e`, in ascending rule order.
 * Returns the number reported. The callback must not free the sheet. */
typedef void (*css_match_fn)(void *ctx, const struct css_rule *r);
int css_match(const struct css_stylesheet *ss, void *e,
              const struct css_elem_ops *ops, css_match_fn cb, void *ctx);

/* Number of candidate rules the index handed the matcher on the last
 * css_match() call, and the number that actually matched. Diagnostics
 * for "is the index earning its keep on this page". */
void css_match_stats(unsigned long *considered, unsigned long *matched);
void css_match_stats_reset(void);

/* ------------------------------------------------------------------ *
 * Computed style.
 * ------------------------------------------------------------------ */
enum {
    CSS_LEN_AUTO = 0,  /* width: auto, margin: auto                     */
    CSS_LEN_PX,        /* .v is integer pixels                          */
    CSS_LEN_PCT,       /* .v is thousandths of a percent                */
    CSS_LEN_NONE,      /* max-width: none                               */
    CSS_LEN_NORMAL,    /* line-height: normal                           */
    CSS_LEN_NUMBER     /* line-height: 1.5, .v thousandths              */
};

struct css_len {
    int32_t v;
    uint8_t type;
};

/* Side indices, in CSS shorthand order. */
enum { CSS_TOP = 0, CSS_RIGHT = 1, CSS_BOTTOM = 2, CSS_LEFT = 3 };

struct computed_style {
    uint8_t display, position, css_float, clear;
    uint8_t text_align, vertical_align, white_space, overflow;
    uint8_t visibility, font_style, font_family, list_style_type;
    uint8_t list_style_position, border_collapse, text_transform;
    uint8_t text_decoration;             /* CSS_DECOR_* bitmask         */
    uint8_t flex_direction, justify_content, align_items;
    uint8_t border_style[4];
    uint8_t z_auto;
    uint16_t font_weight;                /* 100..900                    */

    int32_t font_size;                   /* px, always resolved         */
    int32_t border_width[4];             /* px, 0 when style is none    */
    int32_t border_spacing;              /* px                          */
    int32_t z_index;
    int32_t vertical_align_px;           /* when vertical_align == LENGTH */
    int32_t flex_grow;                   /* unitless, thousandths        */
    int32_t gap;                         /* px                           */

    struct css_len width, height;
    struct css_len min_width, max_width, min_height, max_height;
    struct css_len margin[4], padding[4];
    struct css_len offset[4];            /* top/right/bottom/left       */
    struct css_len line_height;
    struct css_len text_indent;

    uint32_t color;                      /* 0xAARRGGBB, always opaque   */
    uint32_t background_color;           /* alpha 0 means transparent   */
    uint32_t border_color[4];

    const char *background_image;        /* url, or 0                   */
    const char *font_family_name;        /* first named family, or 0    */
};

/* The initial value of every property, before any cascade. */
void css_style_initial(struct computed_style *cs);

/* Colour helpers, exposed because the UA sheet tests use them. */
int css_parse_color_string(const char *s, uint32_t *out);
int css_named_color_count(void);
const char *css_property_name(int prop);
int css_property_id(const char *name);
int css_property_inherited(int prop);

/* ------------------------------------------------------------------ *
 * The cascade (style.c).
 * ------------------------------------------------------------------ */
struct style_engine;

/* Create an engine over a fixed list of sheets. The sheets are borrowed,
 * not owned; they must outlive the engine. Rule order is renumbered so
 * that later sheets win ties against earlier ones. */
struct style_engine *style_engine_new(struct css_stylesheet **sheets, int n,
                                      const struct css_elem_ops *ops);
void style_engine_free(struct style_engine *se);

/* The viewport vw/vh resolve against. Defaults to 1024x768. */
void style_engine_set_viewport(struct style_engine *se, int32_t w, int32_t h);

/* Compute the style of one element. `parent` is the parent's computed
 * style, or 0 at the root. `inline_css` is the element's style=""
 * attribute text, or 0. `root_font_size` resolves rem. Returns 0 only on
 * allocation failure; the result is owned by the caller. */
struct computed_style *style_compute(struct style_engine *se, void *elem,
                                     const struct computed_style *parent,
                                     const char *inline_css,
                                     int32_t root_font_size);

/* Walk a subtree, computing a style for every element and handing it to
 * `sink` for attachment to the node. Recursion is capped at
 * CSS_MAX_TREE_DEPTH; deeper elements inherit the style at the cap.
 * Returns the number of elements styled. */
typedef void (*style_sink_fn)(void *ctx, void *elem,
                              struct computed_style *cs);
int style_compute_tree(struct style_engine *se, void *root,
                       const struct computed_style *initial,
                       style_sink_fn sink, void *ctx);

/* The user-agent stylesheet source (ua_style.c). Parse it once at
 * startup with css_parse(..., CSS_ORIGIN_UA, ...). */
const char *css_ua_stylesheet(void);
unsigned long css_ua_stylesheet_len(void);

/* ------------------------------------------------------------------ *
 * The DOM binding.
 *
 * These three exist only when style.c is compiled with -DCSS_WITH_DOM,
 * which is how the browser builds it. The host tests supply their own
 * struct css_elem_ops and do not link them. Passing a struct dom_node *
 * as void * keeps dom.h out of this header.
 * ------------------------------------------------------------------ */
const struct css_elem_ops *css_dom_ops(void);

/* A style_sink_fn that stores the computed style in dom_node.style,
 * freeing whatever was there before. Pass it to style_compute_tree(). */
void css_style_dom_sink(void *ctx, void *elem, struct computed_style *cs);

/* Free every computed style hung off a subtree and null the slots. */
void css_style_dom_free(void *node);
