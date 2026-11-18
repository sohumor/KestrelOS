/* KestrelOS libimg: standalone SVG rasterizer.
 *
 * Pure integer fixed-point 2D vector rasterizer (zero float/double, zero
 * recursion, heap-allocated scanline/point buffers, small stack frames).
 *
 * Supports:
 * - Sniffing & probing width, height, and viewBox (1..2048 clamp, 128x128
 *   default or viewBox aspect when width/height are omitted or percentages).
 * - <svg>, <g> (inherited fill, stroke, stroke-width, opacity, fill-opacity,
 *   stroke-opacity, fill-rule, color, transform=translate/scale/rotate/matrix).
 * - <rect> (with rx/ry), <circle>, <ellipse>, <line>, <polyline>, <polygon>,
 *   and <path> (M/m, L/l, H/h, V/v, C/c, S/s, Q/q, T/t, A/a, Z/z) with
 *   adaptive iterative integer De Casteljau Bezier subdivision.
 * - Colors: #rgb, #rgba, #rrggbb, #rrggbbaa, rgb(), rgba(), none,
 *   transparent, currentColor, and common CSS/SVG named colors.
 * - Porter-Duff SRC_OVER alpha compositing onto image::px + image::alpha.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "img.h"

#define SVG_DEFAULT_DIM  128
#define SVG_MIN_DIM      1
#define SVG_MAX_DIM      2048

#define FP_SHIFT         16
#define FP_ONE           (1LL << FP_SHIFT)
#define FP_COORD_LIMIT   (10000000LL * FP_ONE)

/* Device space uses 1/256 px (DEV_SHIFT = 8) for polygon fill and
 * 1/16 px (S4 = DEV >> 4) for capsule stroke distance tests. */
#define DEV_SHIFT        8
#define DEV_ONE          (1LL << DEV_SHIFT)
#define DEV_HALF         (1LL << (DEV_SHIFT - 1))

/* 4 * (sqrt(2) - 1) / 3 in 16.16 fixed point (~0.55228475 * 65536). */
#define KAPPA_16         36195LL

#define SVG_MAX_DEPTH    32
#define SVG_MAX_ATTRS    32
#define SVG_MAX_PTS      8192
#define SVG_MAX_SUBS     256
#define SVG_MAX_CROSS    8192
#define SVG_BEZ_STACK    32
#define SVG_MAX_SHAPES   4096

/* ---------------------------------------------------------- 64-bit integer math */

static uint64_t umul_div64(uint64_t a, uint64_t b, uint64_t d)
{
    uint64_t a0, a1, b0, b1, p00, p01, p10, p11, mid, lo, hi, q, rem;
    int i;

    if (d == 0)
        return 0;
    if (a < (1ULL << 31) && b < (1ULL << 31))
        return (a * b) / d;

    a0 = a & 0xffffffffULL;
    a1 = a >> 32;
    b0 = b & 0xffffffffULL;
    b1 = b >> 32;
    p00 = a0 * b0;
    p01 = a0 * b1;
    p10 = a1 * b0;
    p11 = a1 * b1;
    mid = (p00 >> 32) + (p01 & 0xffffffffULL) + (p10 & 0xffffffffULL);
    lo = (p00 & 0xffffffffULL) | (mid << 32);
    hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);

    if (hi == 0)
        return lo / d;
    if (hi >= d)
        return 0x7fffffffffffffffULL;

    q = 0;
    rem = hi;
    for (i = 63; i >= 0; i--) {
        int ov = (rem >> 63) != 0;
        rem = (rem << 1) | ((lo >> i) & 1ULL);
        if (ov || rem >= d) {
            rem -= d;
            q |= (1ULL << i);
        }
    }
    return q;
}

static int64_t mul_div64(int64_t a, int64_t b, int64_t d)
{
    int neg = 0;
    uint64_t ua, ub, ud, r;

    if (d == 0 || a == 0 || b == 0)
        return 0;
    if (d < 0) {
        neg ^= 1;
        ud = (uint64_t)(-d);
    } else {
        ud = (uint64_t)d;
    }
    if (a < 0) {
        neg ^= 1;
        ua = (uint64_t)(-a);
    } else {
        ua = (uint64_t)a;
    }
    if (b < 0) {
        neg ^= 1;
        ub = (uint64_t)(-b);
    } else {
        ub = (uint64_t)b;
    }

    r = umul_div64(ua, ub, ud);
    if (r > 0x7fffffffffffffffULL)
        r = 0x7fffffffffffffffULL;
    return neg ? -(int64_t)r : (int64_t)r;
}

static int64_t fp_mul16(int64_t a, int64_t b)
{
    if (a > -1073741824LL && a < 1073741824LL &&
        b > -1073741824LL && b < 1073741824LL) {
        int64_t p = a * b;
        return p >= 0 ? (p >> FP_SHIFT) : -((-p) >> FP_SHIFT);
    }
    return mul_div64(a, b, FP_ONE);
}

static int64_t clamp_fp(int64_t v)
{
    if (v > FP_COORD_LIMIT)
        return FP_COORD_LIMIT;
    if (v < -FP_COORD_LIMIT)
        return -FP_COORD_LIMIT;
    return v;
}

static int64_t fp_add(int64_t a, int64_t b)
{
    if (a > 0 && b > FP_COORD_LIMIT - a)
        return FP_COORD_LIMIT;
    if (a < 0 && b < -FP_COORD_LIMIT - a)
        return -FP_COORD_LIMIT;
    return a + b;
}

static uint64_t isqrt64(uint64_t x)
{
    uint64_t res = 0;
    uint64_t bit = 1ULL << 62;

    while (bit > x)
        bit >>= 2;
    while (bit != 0) {
        if (x >= res + bit) {
            x -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0)))
        q--;
    return q;
}

static int64_t ceil_div(int64_t a, int64_t b)
{
    return -floor_div(-a, b);
}

/* sin(0..90 deg) in 16.16 fixed point. */
static const int32_t svg_sin90[91] = {
        0,  1144,  2287,  3430,  4572,  5712,  6850,  7987,  9121, 10252,
    11380, 12505, 13626, 14742, 15855, 16962, 18064, 19161, 20252, 21336,
    22415, 23486, 24550, 25607, 26656, 27697, 28729, 29753, 30767, 31772,
    32768, 33754, 34729, 35693, 36647, 37590, 38521, 39441, 40348, 41243,
    42126, 42995, 43852, 44695, 45525, 46341, 47143, 47930, 48703, 49461,
    50203, 50931, 51643, 52339, 53020, 53684, 54332, 54963, 55578, 56175,
    56756, 57319, 57865, 58393, 58903, 59396, 59870, 60326, 60764, 61183,
    61584, 61966, 62328, 62672, 62997, 63303, 63589, 63856, 64104, 64332,
    64540, 64729, 64898, 65048, 65177, 65287, 65376, 65446, 65496, 65526,
    65536
};

static int64_t fp_sin_deg(int64_t deg_fp)
{
    int64_t full = 360LL * FP_ONE;
    int64_t d = deg_fp % full;
    int sign = 1;
    int idx, frac;
    int64_t v0, v1;

    if (d < 0)
        d += full;
    if (d >= 180LL * FP_ONE) {
        d -= 180LL * FP_ONE;
        sign = -1;
    }
    if (d > 90LL * FP_ONE)
        d = 180LL * FP_ONE - d;
    idx = (int)(d >> FP_SHIFT);
    frac = (int)(d & (FP_ONE - 1));
    if (idx >= 90)
        return sign * 65536LL;
    v0 = svg_sin90[idx];
    v1 = svg_sin90[idx + 1];
    return sign * (v0 + (((v1 - v0) * frac) >> FP_SHIFT));
}

static int64_t fp_cos_deg(int64_t deg_fp)
{
    return fp_sin_deg(deg_fp + 90LL * FP_ONE);
}

/* ---------------------------------------------------------- string helpers */

static int svg_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int svg_alpha_c(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int svg_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int svg_ieq_n(const char *a, unsigned long n, const char *b)
{
    unsigned long i = 0;

    while (i < n && b[i]) {
        int ca = (unsigned char)a[i];
        int cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb)
            return 0;
        i++;
    }
    return i == n && !b[i];
}

static int svg_tag_ieq(const char *tag, unsigned long len, const char *target)
{
    unsigned long i;

    for (i = 0; i < len; i++) {
        if (tag[i] == ':') {
            tag += i + 1;
            len -= i + 1;
            break;
        }
    }
    return svg_ieq_n(tag, len, target);
}

/* Parse a decimal/scientific number into 16.16 fixed point. */
static int svg_parse_num(const char **pp, const char *end, int64_t *out)
{
    const char *p = *pp;
    uint64_t whole = 0, frac = 0, fscale = 1;
    int sign = 1, have = 0, fdigits = 0;
    int64_t val;

    while (p < end && (svg_ws(*p) || *p == ','))
        p++;
    if (p < end && (*p == '-' || *p == '+')) {
        if (*p == '-')
            sign = -1;
        p++;
    }
    while (p < end && *p >= '0' && *p <= '9') {
        have = 1;
        if (whole < 100000000ULL)
            whole = whole * 10ULL + (uint64_t)(*p - '0');
        p++;
    }
    if (p < end && *p == '.') {
        p++;
        while (p < end && *p >= '0' && *p <= '9') {
            have = 1;
            if (fdigits < 6) {
                frac = frac * 10ULL + (uint64_t)(*p - '0');
                fscale *= 10ULL;
                fdigits++;
            }
            p++;
        }
    }
    if (!have)
        return 0;

    val = (int64_t)(whole * (uint64_t)FP_ONE +
                    (frac * (uint64_t)FP_ONE + fscale / 2ULL) / fscale);

    /* Scientific notation (e.g. 1.5e-3), only if digits follow e/E. */
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char *ep = p + 1;
        int esign = 1, ehave = 0, exp = 0;
        if (ep < end && (*ep == '-' || *ep == '+')) {
            if (*ep == '-')
                esign = -1;
            ep++;
        }
        while (ep < end && *ep >= '0' && *ep <= '9') {
            ehave = 1;
            if (exp < 20)
                exp = exp * 10 + (*ep - '0');
            ep++;
        }
        if (ehave) {
            int k;
            p = ep;
            if (esign > 0) {
                for (k = 0; k < exp; k++) {
                    if (val > FP_COORD_LIMIT / 10) {
                        val = FP_COORD_LIMIT;
                        break;
                    }
                    val *= 10;
                }
            } else {
                for (k = 0; k < exp && val > 0; k++)
                    val = (val + 5) / 10;
            }
        }
    }

    val *= sign;
    *out = clamp_fp(val);
    *pp = p;
    return 1;
}

static int svg_parse_len(const char *s, unsigned long len,
                         int64_t *out_fp, int *is_pct)
{
    const char *p = s;
    const char *end = s + len;
    int64_t v;

    *is_pct = 0;
    if (!s || len == 0)
        return 0;
    if (!svg_parse_num(&p, end, &v))
        return 0;
    while (p < end && svg_ws(*p))
        p++;
    if (p < end) {
        unsigned long ulen = (unsigned long)(end - p);
        while (ulen > 0 && svg_ws(p[ulen - 1]))
            ulen--;
        if (ulen == 1 && p[0] == '%') {
            *is_pct = 1;
            return 0;
        }
        if (ulen == 2) {
            if (svg_ieq_n(p, 2, "px")) {
                /* 1:1 */
            } else if (svg_ieq_n(p, 2, "pt")) {
                v = mul_div64(v, 4, 3);
            } else if (svg_ieq_n(p, 2, "pc")) {
                v = clamp_fp(v * 16);
            } else if (svg_ieq_n(p, 2, "in")) {
                v = clamp_fp(v * 96);
            } else if (svg_ieq_n(p, 2, "cm")) {
                v = mul_div64(v, 9600, 254);
            } else if (svg_ieq_n(p, 2, "mm")) {
                v = mul_div64(v, 960, 254);
            } else {
                return 0;
            }
        } else if (ulen > 0) {
            return 0;
        }
    }
    *out_fp = v;
    return 1;
}

static int svg_parse_opacity(const char *s, unsigned long len, int *out_alpha)
{
    const char *p = s;
    const char *end = s + len;
    int64_t v;

    if (!s || len == 0)
        return 0;
    if (!svg_parse_num(&p, end, &v))
        return 0;
    while (p < end && svg_ws(*p))
        p++;
    if (p < end && *p == '%')
        v = mul_div64(v, 1, 100);
    if (v <= 0)
        *out_alpha = 0;
    else if (v >= FP_ONE)
        *out_alpha = 255;
    else
        *out_alpha = (int)((v * 255LL + (FP_ONE / 2)) >> FP_SHIFT);
    return 1;
}

static int svg_parse_rgb_comp(const char **pp, const char *end, int *out)
{
    int64_t v;

    if (!svg_parse_num(pp, end, &v))
        return 0;
    if (*pp < end && **pp == '%') {
        (*pp)++;
        v = mul_div64(v, 255, 100);
    }
    v = (v + (FP_ONE >> 1)) >> FP_SHIFT;
    *out = img__clamp8((long)v);
    return 1;
}

static uint32_t svg_parse_color(const char *s, unsigned long len,
                                uint32_t fallback, uint32_t cur_color)
{
    static const struct {
        const char *name;
        uint32_t rgb;
    } named[] = {
        { "aliceblue", 0xf0f8ffu }, { "aqua", 0x00ffffu },
        { "aquamarine", 0x7fffd4u }, { "azure", 0xf0ffffu },
        { "beige", 0xf5f5dcu }, { "black", 0x000000u },
        { "blue", 0x0000ffu }, { "blueviolet", 0x8a2be2u },
        { "brown", 0xa52a2au }, { "cadetblue", 0x5f9ea0u },
        { "chartreuse", 0x7fff00u }, { "chocolate", 0xd2691eu },
        { "coral", 0xff7f50u }, { "cornflowerblue", 0x6495edu },
        { "crimson", 0xdc143cu }, { "cyan", 0x00ffffu },
        { "darkblue", 0x00008bu }, { "darkcyan", 0x008b8bu },
        { "darkgray", 0xa9a9a9u }, { "darkgreen", 0x006400u },
        { "darkgrey", 0xa9a9a9u }, { "darkmagenta", 0x8b008bu },
        { "darkorange", 0xff8c00u }, { "darkred", 0x8b0000u },
        { "deeppink", 0xff1493u }, { "deepskyblue", 0x00bfffu },
        { "dimgray", 0x696969u }, { "dimgrey", 0x696969u },
        { "dodgerblue", 0x1e90ffu }, { "firebrick", 0xb22222u },
        { "forestgreen", 0x228b22u }, { "fuchsia", 0xff00ffu },
        { "gold", 0xffd700u }, { "goldenrod", 0xdaa520u },
        { "gray", 0x808080u }, { "green", 0x008000u },
        { "greenyellow", 0xadff2fu }, { "grey", 0x808080u },
        { "hotpink", 0xff69b4u }, { "indianred", 0xcd5c5cu },
        { "indigo", 0x4b0082u }, { "ivory", 0xfffff0u },
        { "khaki", 0xf0e68cu }, { "lavender", 0xe6e6fau },
        { "lightblue", 0xadd8e6u }, { "lightgray", 0xd3d3d3u },
        { "lightgreen", 0x90ee90u }, { "lightgrey", 0xd3d3d3u },
        { "lime", 0x00ff00u }, { "limegreen", 0x32cd32u },
        { "magenta", 0xff00ffu }, { "maroon", 0x800000u },
        { "midnightblue", 0x191970u }, { "navy", 0x000080u },
        { "olive", 0x808000u }, { "orange", 0xffa500u },
        { "orangered", 0xff4500u }, { "orchid", 0xda70d6u },
        { "peru", 0xcd853fu }, { "pink", 0xffc0cbu },
        { "plum", 0xdda0ddu }, { "purple", 0x800080u },
        { "rebeccapurple", 0x663399u }, { "red", 0xff0000u },
        { "rosybrown", 0xbc8f8fu }, { "royalblue", 0x4169e1u },
        { "salmon", 0xfa8072u }, { "seagreen", 0x2e8b57u },
        { "sienna", 0xa0522du }, { "silver", 0xc0c0c0u },
        { "skyblue", 0x87ceebu }, { "slateblue", 0x6a5acdu },
        { "slategray", 0x708090u }, { "snow", 0xfffafau },
        { "springgreen", 0x00ff7fu }, { "steelblue", 0x4682b4u },
        { "tan", 0xd2b48cu }, { "teal", 0x008080u },
        { "tomato", 0xff6347u }, { "turquoise", 0x40e0d0u },
        { "violet", 0xee82eeu }, { "wheat", 0xf5deb3u },
        { "white", 0xffffffu }, { "yellow", 0xffff00u },
        { "yellowgreen", 0x9acd32u }
    };
    const char *end = s + len;
    unsigned long i;

    if (!s || len == 0)
        return fallback;
    while (s < end && svg_ws(*s))
        s++;
    while (end > s && svg_ws(end[-1]))
        end--;
    len = (unsigned long)(end - s);
    if (len == 0)
        return fallback;

    if (svg_ieq_n(s, len, "none") || svg_ieq_n(s, len, "transparent"))
        return 0;
    if (svg_ieq_n(s, len, "currentColor"))
        return 0xff000000u | (cur_color & 0x00ffffffu);

    if (s[0] == '#') {
        int h[8];
        unsigned long n = len - 1;
        if (n != 3 && n != 4 && n != 6 && n != 8)
            return fallback;
        for (i = 0; i < n; i++) {
            h[i] = svg_hex(s[1 + i]);
            if (h[i] < 0)
                return fallback;
        }
        if (n == 3) {
            return 0xff000000u | ((uint32_t)(h[0] * 17) << 16) |
                   ((uint32_t)(h[1] * 17) << 8) | (uint32_t)(h[2] * 17);
        }
        if (n == 4) {
            return ((uint32_t)(h[3] * 17) << 24) |
                   ((uint32_t)(h[0] * 17) << 16) |
                   ((uint32_t)(h[1] * 17) << 8) | (uint32_t)(h[2] * 17);
        }
        if (n == 6) {
            return 0xff000000u |
                   ((uint32_t)((h[0] << 4) | h[1]) << 16) |
                   ((uint32_t)((h[2] << 4) | h[3]) << 8) |
                   (uint32_t)((h[4] << 4) | h[5]);
        }
        return ((uint32_t)((h[6] << 4) | h[7]) << 24) |
               ((uint32_t)((h[0] << 4) | h[1]) << 16) |
               ((uint32_t)((h[2] << 4) | h[3]) << 8) |
               (uint32_t)((h[4] << 4) | h[5]);
    }

    if ((len >= 5 && svg_ieq_n(s, 4, "rgb(") && s[len - 1] == ')') ||
        (len >= 6 && svg_ieq_n(s, 5, "rgba(") && s[len - 1] == ')')) {
        int has_a = (s[3] == 'a' || s[3] == 'A');
        const char *p = s + (has_a ? 5 : 4);
        const char *pe = end - 1;
        int r, g, b, a = 255;

        if (!svg_parse_rgb_comp(&p, pe, &r) ||
            !svg_parse_rgb_comp(&p, pe, &g) ||
            !svg_parse_rgb_comp(&p, pe, &b))
            return fallback;
        while (p < pe && (svg_ws(*p) || *p == ',' || *p == '/'))
            p++;
        if (p < pe) {
            if (!svg_parse_opacity(p, (unsigned long)(pe - p), &a))
                return fallback;
        }
        return ((uint32_t)a << 24) | ((uint32_t)r << 16) |
               ((uint32_t)g << 8) | (uint32_t)b;
    }

    for (i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        if (svg_ieq_n(s, len, named[i].name))
            return 0xff000000u | named[i].rgb;
    }
    return fallback;
}

/* ---------------------------------------------------------- transforms & state */

struct svg_mat {
    int64_t a, b, c, d, tx, ty;
};

static void mat_ident(struct svg_mat *m)
{
    m->a = FP_ONE; m->b = 0;
    m->c = 0;      m->d = FP_ONE;
    m->tx = 0;     m->ty = 0;
}

static void mat_mul(struct svg_mat *out, const struct svg_mat *m,
                    const struct svg_mat *l)
{
    struct svg_mat r;

    r.a  = fp_add(fp_mul16(m->a, l->a),  fp_mul16(m->c, l->b));
    r.b  = fp_add(fp_mul16(m->b, l->a),  fp_mul16(m->d, l->b));
    r.c  = fp_add(fp_mul16(m->a, l->c),  fp_mul16(m->c, l->d));
    r.d  = fp_add(fp_mul16(m->b, l->c),  fp_mul16(m->d, l->d));
    r.tx = fp_add(fp_add(fp_mul16(m->a, l->tx), fp_mul16(m->c, l->ty)), m->tx);
    r.ty = fp_add(fp_add(fp_mul16(m->b, l->tx), fp_mul16(m->d, l->ty)), m->ty);
    *out = r;
}

static void svg_apply_transform(const char *s, unsigned long len,
                                struct svg_mat *m)
{
    const char *p = s;
    const char *end = s + len;

    while (p < end) {
        const char *fn, *args, *args_end;
        unsigned long fnlen;
        struct svg_mat loc;

        while (p < end && (svg_ws(*p) || *p == ','))
            p++;
        if (p >= end)
            break;
        fn = p;
        while (p < end && *p != '(' && !svg_ws(*p))
            p++;
        fnlen = (unsigned long)(p - fn);
        while (p < end && svg_ws(*p))
            p++;
        if (p >= end || *p != '(')
            break;
        p++;
        args = p;
        while (p < end && *p != ')')
            p++;
        if (p >= end)
            break;
        args_end = p;
        p++;

        mat_ident(&loc);
        if (svg_ieq_n(fn, fnlen, "translate")) {
            int64_t tx = 0, ty = 0;
            if (svg_parse_num(&args, args_end, &tx)) {
                svg_parse_num(&args, args_end, &ty);
                loc.tx = tx;
                loc.ty = ty;
                mat_mul(m, m, &loc);
            }
        } else if (svg_ieq_n(fn, fnlen, "scale")) {
            int64_t sx = FP_ONE, sy = FP_ONE;
            if (svg_parse_num(&args, args_end, &sx)) {
                if (!svg_parse_num(&args, args_end, &sy))
                    sy = sx;
                loc.a = sx;
                loc.d = sy;
                mat_mul(m, m, &loc);
            }
        } else if (svg_ieq_n(fn, fnlen, "rotate")) {
            int64_t deg = 0, cx = 0, cy = 0;
            if (svg_parse_num(&args, args_end, &deg)) {
                int have_c = svg_parse_num(&args, args_end, &cx) &&
                             svg_parse_num(&args, args_end, &cy);
                int64_t sn = fp_sin_deg(deg);
                int64_t cs = fp_cos_deg(deg);
                loc.a = cs;  loc.b = sn;
                loc.c = -sn; loc.d = cs;
                if (have_c) {
                    loc.tx = fp_add(cx, -(fp_mul16(cs, cx) - fp_mul16(sn, cy)));
                    loc.ty = fp_add(cy, -(fp_mul16(sn, cx) + fp_mul16(cs, cy)));
                }
                mat_mul(m, m, &loc);
            }
        } else if (svg_ieq_n(fn, fnlen, "matrix")) {
            int64_t v[6];
            int k;
            for (k = 0; k < 6; k++) {
                if (!svg_parse_num(&args, args_end, &v[k]))
                    break;
            }
            if (k == 6) {
                loc.a = v[0]; loc.b = v[1];
                loc.c = v[2]; loc.d = v[3];
                loc.tx = v[4]; loc.ty = v[5];
                mat_mul(m, m, &loc);
            }
        }
    }
}

struct svg_state {
    uint32_t fill;
    uint32_t stroke;
    uint32_t current_color;
    int64_t  stroke_width;
    int      fill_alpha;
    int      stroke_alpha;
    int      opacity;
    int      fill_evenodd;
    struct svg_mat mat;
};

struct svg_attr {
    const char *name;
    unsigned long nlen;
    const char *val;
    unsigned long vlen;
};

struct svg_subpath {
    int start;
    int count;
    uint8_t closed;
};

struct svg_cross {
    int64_t x;
    int32_t dir;
};

struct svg_elem_frame {
    uint8_t is_skip;
    uint8_t has_state;
};

struct svg_bez_item {
    int64_t x0, y0, x1, y1, x2, y2, x3, y3;
    int depth;
};

struct svg_ctx {
    struct image im;
    int out_w, out_h;

    /* viewBox mapping from user space (16.16) to device space (256ths/px). */
    int64_t vb_x, vb_y, vb_w, vb_h;
    int64_t dev_ox, dev_oy, dev_w, dev_h;

    struct svg_state stack[SVG_MAX_DEPTH];
    struct svg_elem_frame elem_stack[SVG_MAX_DEPTH];
    int state_top;
    int elem_depth;
    int skip_depth;
    int shapes;

    struct svg_state cur_st;
    struct svg_attr attrs[SVG_MAX_ATTRS];
    int nattr;

    int64_t *pt_x;
    int64_t *pt_y;
    struct svg_subpath *subs;
    struct svg_cross *cross;
    struct svg_bez_item *bez;
    uint8_t *cov;
};

/* ---------------------------------------------------------- header & probing */

struct svg_root_info {
    int out_w, out_h;
    int64_t vb_x, vb_y, vb_w, vb_h;
    int64_t dev_ox, dev_oy, dev_w, dev_h;
    unsigned long after_tag_pos;
    int self_closing;
};

static int svg_parse_attrs_raw(const char *p, unsigned long len,
                               unsigned long *pos,
                               struct svg_attr *attrs, int max_attrs,
                               int *out_nattr, int *out_self_close)
{
    int n = 0;

    *out_self_close = 0;
    while (*pos < len) {
        const char *name, *val;
        unsigned long nlen, vlen;
        char q;

        while (*pos < len && svg_ws(p[*pos]))
            (*pos)++;
        if (*pos >= len)
            return IMG_ERR_TRUNCATED;
        if (p[*pos] == '>') {
            (*pos)++;
            *out_nattr = n;
            return IMG_OK;
        }
        if (p[*pos] == '/' && *pos + 1 < len && p[*pos + 1] == '>') {
            *pos += 2;
            *out_self_close = 1;
            *out_nattr = n;
            return IMG_OK;
        }
        if (p[*pos] == '/') {
            (*pos)++;
            continue;
        }

        name = p + *pos;
        while (*pos < len && !svg_ws(p[*pos]) && p[*pos] != '=' &&
               p[*pos] != '>' && p[*pos] != '/')
            (*pos)++;
        nlen = (unsigned long)((p + *pos) - name);
        if (nlen == 0) {
            (*pos)++;
            continue;
        }

        while (*pos < len && svg_ws(p[*pos]))
            (*pos)++;
        if (*pos >= len)
            return IMG_ERR_TRUNCATED;
        if (p[*pos] != '=')
            continue;
        (*pos)++;
        while (*pos < len && svg_ws(p[*pos]))
            (*pos)++;
        if (*pos >= len)
            return IMG_ERR_TRUNCATED;

        q = p[*pos];
        if (q == '"' || q == '\'') {
            (*pos)++;
            val = p + *pos;
            while (*pos < len && p[*pos] != q)
                (*pos)++;
            if (*pos >= len)
                return IMG_ERR_TRUNCATED;
            vlen = (unsigned long)((p + *pos) - val);
            (*pos)++;
        } else {
            val = p + *pos;
            while (*pos < len && !svg_ws(p[*pos]) && p[*pos] != '>')
                (*pos)++;
            vlen = (unsigned long)((p + *pos) - val);
        }

        if (n < max_attrs) {
            attrs[n].name = name;
            attrs[n].nlen = nlen;
            attrs[n].val = val;
            attrs[n].vlen = vlen;
            n++;
        }
    }
    return IMG_ERR_TRUNCATED;
}

static const struct svg_attr *svg_find_attr(const struct svg_attr *attrs,
                                            int nattr, const char *name)
{
    int i;

    for (i = 0; i < nattr; i++) {
        if (svg_ieq_n(attrs[i].name, attrs[i].nlen, name))
            return &attrs[i];
    }
    return 0;
}

static int svg_clamp_dim(int64_t v_fp)
{
    int64_t px = (v_fp + (FP_ONE >> 1)) >> FP_SHIFT;
    if (px < SVG_MIN_DIM)
        return SVG_MIN_DIM;
    if (px > SVG_MAX_DIM)
        return SVG_MAX_DIM;
    return (int)px;
}

static int svg_skip_prolog_to_svg(const char *p, unsigned long len,
                                  unsigned long *out_pos)
{
    unsigned long pos = 0;

    if (len >= 3 && (uint8_t)p[0] == 0xef &&
        (uint8_t)p[1] == 0xbb && (uint8_t)p[2] == 0xbf)
        pos = 3;

    while (pos < len) {
        while (pos < len && svg_ws(p[pos]))
            pos++;
        if (pos >= len)
            break;
        if (p[pos] != '<')
            return IMG_ERR_FORMAT;
        if (pos + 2 <= len && p[pos + 1] == '?') {
            pos += 2;
            while (pos + 2 <= len && !(p[pos] == '?' && p[pos + 1] == '>'))
                pos++;
            if (pos + 2 > len)
                return IMG_ERR_TRUNCATED;
            pos += 2;
            continue;
        }
        if (pos + 4 <= len && p[pos + 1] == '!' &&
            p[pos + 2] == '-' && p[pos + 3] == '-') {
            pos += 4;
            while (pos + 3 <= len &&
                   !(p[pos] == '-' && p[pos + 1] == '-' && p[pos + 2] == '>'))
                pos++;
            if (pos + 3 > len)
                return IMG_ERR_TRUNCATED;
            pos += 3;
            continue;
        }
        if (pos + 2 <= len && p[pos + 1] == '!') {
            int depth = 1;
            pos += 2;
            while (pos < len && depth > 0) {
                if (p[pos] == '<') depth++;
                else if (p[pos] == '>') depth--;
                pos++;
            }
            if (depth > 0)
                return IMG_ERR_TRUNCATED;
            continue;
        }
        /* First real element must be <svg. */
        {
            unsigned long tstart = pos + 1;
            unsigned long tend = tstart;
            while (tend < len && !svg_ws(p[tend]) && p[tend] != '>' &&
                   p[tend] != '/')
                tend++;
            if (tend >= len)
                return IMG_ERR_TRUNCATED;
            if (!svg_tag_ieq(p + tstart, tend - tstart, "svg"))
                return IMG_ERR_FORMAT;
            *out_pos = tend;
            return IMG_OK;
        }
    }
    return IMG_ERR_TRUNCATED;
}

static void svg_compute_viewport(const struct svg_attr *attrs, int nattr,
                                 struct svg_root_info *ri)
{
    const struct svg_attr *aw = svg_find_attr(attrs, nattr, "width");
    const struct svg_attr *ah = svg_find_attr(attrs, nattr, "height");
    const struct svg_attr *avb = svg_find_attr(attrs, nattr, "viewBox");
    const struct svg_attr *apar = svg_find_attr(attrs, nattr, "preserveAspectRatio");
    int64_t w_fp = 0, h_fp = 0;
    int pct_w = 0, pct_h = 0;
    int have_w = 0, have_h = 0, have_vb = 0;
    int64_t vbx = 0, vby = 0, vbw = 0, vbh = 0;

    if (aw)
        have_w = svg_parse_len(aw->val, aw->vlen, &w_fp, &pct_w) && w_fp > 0;
    if (ah)
        have_h = svg_parse_len(ah->val, ah->vlen, &h_fp, &pct_h) && h_fp > 0;
    (void)pct_w;
    (void)pct_h;

    if (avb) {
        const char *vp = avb->val;
        const char *ve = avb->val + avb->vlen;
        if (svg_parse_num(&vp, ve, &vbx) &&
            svg_parse_num(&vp, ve, &vby) &&
            svg_parse_num(&vp, ve, &vbw) &&
            svg_parse_num(&vp, ve, &vbh) &&
            vbw > 0 && vbh > 0) {
            have_vb = 1;
        }
    }

    if (have_w && have_h) {
        ri->out_w = svg_clamp_dim(w_fp);
        ri->out_h = svg_clamp_dim(h_fp);
    } else if (have_w) {
        ri->out_w = svg_clamp_dim(w_fp);
        if (have_vb) {
            int64_t ch = mul_div64((int64_t)ri->out_w << FP_SHIFT, vbh, vbw);
            ri->out_h = svg_clamp_dim(ch);
        } else {
            ri->out_h = SVG_DEFAULT_DIM;
        }
    } else if (have_h) {
        ri->out_h = svg_clamp_dim(h_fp);
        if (have_vb) {
            int64_t cw = mul_div64((int64_t)ri->out_h << FP_SHIFT, vbw, vbh);
            ri->out_w = svg_clamp_dim(cw);
        } else {
            ri->out_w = SVG_DEFAULT_DIM;
        }
    } else if (have_vb) {
        if (vbw >= vbh) {
            ri->out_w = SVG_DEFAULT_DIM;
            ri->out_h = svg_clamp_dim(
                mul_div64((int64_t)SVG_DEFAULT_DIM << FP_SHIFT, vbh, vbw));
        } else {
            ri->out_h = SVG_DEFAULT_DIM;
            ri->out_w = svg_clamp_dim(
                mul_div64((int64_t)SVG_DEFAULT_DIM << FP_SHIFT, vbw, vbh));
        }
    } else {
        ri->out_w = SVG_DEFAULT_DIM;
        ri->out_h = SVG_DEFAULT_DIM;
    }

    if (!have_vb) {
        ri->vb_x = 0;
        ri->vb_y = 0;
        ri->vb_w = (int64_t)ri->out_w << FP_SHIFT;
        ri->vb_h = (int64_t)ri->out_h << FP_SHIFT;
        ri->dev_ox = 0;
        ri->dev_oy = 0;
        ri->dev_w = (int64_t)ri->out_w << DEV_SHIFT;
        ri->dev_h = (int64_t)ri->out_h << DEV_SHIFT;
        return;
    }

    ri->vb_x = vbx;
    ri->vb_y = vby;
    ri->vb_w = vbw;
    ri->vb_h = vbh;
    ri->dev_ox = 0;
    ri->dev_oy = 0;
    ri->dev_w = (int64_t)ri->out_w << DEV_SHIFT;
    ri->dev_h = (int64_t)ri->out_h << DEV_SHIFT;

    if (apar) {
        const char *p = apar->val;
        const char *pe = apar->val + apar->vlen;
        while (p < pe && svg_ws(*p)) p++;
        if ((unsigned long)(pe - p) >= 4 && svg_ieq_n(p, 4, "none"))
            return;
    }

    /* Default xMidYMid meet scaling. */
    {
        int64_t full_w = (int64_t)ri->out_w << DEV_SHIFT;
        int64_t full_h = (int64_t)ri->out_h << DEV_SHIFT;
        int64_t scaled_h = mul_div64(full_w, vbh, vbw);

        if (scaled_h <= full_h) {
            if (scaled_h < 1) scaled_h = 1;
            ri->dev_h = scaled_h;
            ri->dev_oy = (full_h - scaled_h) / 2;
        } else {
            int64_t scaled_w = mul_div64(full_h, vbw, vbh);
            if (scaled_w < 1) scaled_w = 1;
            ri->dev_w = scaled_w;
            ri->dev_ox = (full_w - scaled_w) / 2;
        }
    }
}

static int svg_parse_root(const char *p, unsigned long len,
                          struct svg_attr *attrs, struct svg_root_info *ri)
{
    unsigned long pos = 0;
    int nattr = 0, self_close = 0, rc;

    rc = svg_skip_prolog_to_svg(p, len, &pos);
    if (rc != IMG_OK)
        return rc;
    rc = svg_parse_attrs_raw(p, len, &pos, attrs, SVG_MAX_ATTRS,
                             &nattr, &self_close);
    if (rc != IMG_OK)
        return rc;
    svg_compute_viewport(attrs, nattr, ri);
    ri->after_tag_pos = pos;
    ri->self_closing = self_close;
    return nattr;
}

int svg_probe(const void *data, unsigned long len, struct img_info *out)
{
    const char *p = (const char *)data;
    struct svg_attr *attrs;
    struct svg_root_info ri;
    int rc;

    if (!out)
        return IMG_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    out->fmt = IMG_FMT_SVG;
    out->frames = 1;
    out->has_alpha = 1;
    if (!p || len > IMG_MAX_INPUT)
        return IMG_ERR_INVAL;

    attrs = (struct svg_attr *)calloc(SVG_MAX_ATTRS, sizeof(*attrs));
    if (!attrs)
        return IMG_ERR_NOMEM;
    rc = svg_parse_root(p, len, attrs, &ri);
    free(attrs);
    if (rc < 0)
        return rc;

    out->w = ri.out_w;
    out->h = ri.out_h;
    return IMG_OK;
}

/* ---------------------------------------------------------- style resolution */

static void svg_apply_prop(struct svg_state *st, const char *name,
                           unsigned long nlen, const char *val,
                           unsigned long vlen)
{
    if (svg_ieq_n(name, nlen, "color")) {
        uint32_t c = svg_parse_color(val, vlen, 0xff000000u | st->current_color,
                                     st->current_color);
        if (c)
            st->current_color = c & 0x00ffffffu;
    } else if (svg_ieq_n(name, nlen, "fill")) {
        st->fill = svg_parse_color(val, vlen, st->fill, st->current_color);
    } else if (svg_ieq_n(name, nlen, "stroke")) {
        st->stroke = svg_parse_color(val, vlen, st->stroke, st->current_color);
    } else if (svg_ieq_n(name, nlen, "stroke-width")) {
        int64_t w;
        int pct;
        if (svg_parse_len(val, vlen, &w, &pct) && w >= 0)
            st->stroke_width = w;
    } else if (svg_ieq_n(name, nlen, "opacity")) {
        int op;
        if (svg_parse_opacity(val, vlen, &op))
            st->opacity = (st->opacity * op + 127) / 255;
    } else if (svg_ieq_n(name, nlen, "fill-opacity")) {
        int op;
        if (svg_parse_opacity(val, vlen, &op))
            st->fill_alpha = op;
    } else if (svg_ieq_n(name, nlen, "stroke-opacity")) {
        int op;
        if (svg_parse_opacity(val, vlen, &op))
            st->stroke_alpha = op;
    } else if (svg_ieq_n(name, nlen, "fill-rule")) {
        const char *s = val, *e = val + vlen;
        while (s < e && svg_ws(*s)) s++;
        while (e > s && svg_ws(e[-1])) e--;
        if (svg_ieq_n(s, (unsigned long)(e - s), "evenodd"))
            st->fill_evenodd = 1;
        else if (svg_ieq_n(s, (unsigned long)(e - s), "nonzero"))
            st->fill_evenodd = 0;
    } else if (svg_ieq_n(name, nlen, "transform")) {
        svg_apply_transform(val, vlen, &st->mat);
    }
}

static void svg_apply_style_str(struct svg_state *st, const char *s,
                                unsigned long len)
{
    const char *p = s;
    const char *end = s + len;

    while (p < end) {
        const char *pn, *pv;
        unsigned long pnl, pvl;

        while (p < end && (svg_ws(*p) || *p == ';'))
            p++;
        pn = p;
        while (p < end && *p != ':' && *p != ';')
            p++;
        pnl = (unsigned long)(p - pn);
        while (pnl > 0 && svg_ws(pn[pnl - 1]))
            pnl--;
        if (p >= end || *p != ':') {
            if (p < end) p++;
            continue;
        }
        p++;
        while (p < end && svg_ws(*p))
            p++;
        pv = p;
        while (p < end && *p != ';')
            p++;
        pvl = (unsigned long)(p - pv);
        while (pvl > 0 && svg_ws(pv[pvl - 1]))
            pvl--;
        if (pnl > 0)
            svg_apply_prop(st, pn, pnl, pv, pvl);
    }
}

static void svg_apply_attrs(struct svg_state *st, const struct svg_attr *attrs,
                            int nattr)
{
    const struct svg_attr *sty;
    int i;

    for (i = 0; i < nattr; i++) {
        if (!svg_ieq_n(attrs[i].name, attrs[i].nlen, "style"))
            svg_apply_prop(st, attrs[i].name, attrs[i].nlen,
                           attrs[i].val, attrs[i].vlen);
    }
    sty = svg_find_attr(attrs, nattr, "style");
    if (sty)
        svg_apply_style_str(st, sty->val, sty->vlen);
}

static uint32_t svg_effective_color(uint32_t base_argb, int elem_alpha,
                                    int group_opacity)
{
    uint32_t ba = base_argb >> 24;
    uint32_t a;

    if (ba == 0 || elem_alpha == 0 || group_opacity == 0)
        return 0;
    a = (ba * (uint32_t)elem_alpha + 127u) / 255u;
    a = (a * (uint32_t)group_opacity + 127u) / 255u;
    if (a == 0)
        return 0;
    return (a << 24) | (base_argb & 0x00ffffffu);
}

/* ---------------------------------------------------------- coordinate xform */

static void svg_xform_pt(const struct svg_ctx *ctx, const struct svg_mat *m,
                         int64_t ux, int64_t uy, int64_t *dx, int64_t *dy)
{
    int64_t tx = fp_add(fp_add(fp_mul16(m->a, ux), fp_mul16(m->c, uy)), m->tx);
    int64_t ty = fp_add(fp_add(fp_mul16(m->b, ux), fp_mul16(m->d, uy)), m->ty);
    int64_t px = ctx->dev_ox + mul_div64(tx - ctx->vb_x, ctx->dev_w, ctx->vb_w);
    int64_t py = ctx->dev_oy + mul_div64(ty - ctx->vb_y, ctx->dev_h, ctx->vb_h);

    if (px > 1000000LL * DEV_ONE) px = 1000000LL * DEV_ONE;
    if (px < -1000000LL * DEV_ONE) px = -1000000LL * DEV_ONE;
    if (py > 1000000LL * DEV_ONE) py = 1000000LL * DEV_ONE;
    if (py < -1000000LL * DEV_ONE) py = -1000000LL * DEV_ONE;
    *dx = px;
    *dy = py;
}

static int64_t svg_stroke_s4(const struct svg_ctx *ctx, const struct svg_state *st)
{
    int64_t dx1, dy1, dx2, dy2, len1, len2, avg_dev;
    int64_t lim = 1000000LL * DEV_ONE;

    if (st->stroke_width <= 0)
        return 0;
    dx1 = mul_div64(st->mat.a, ctx->dev_w, ctx->vb_w);
    dy1 = mul_div64(st->mat.b, ctx->dev_h, ctx->vb_h);
    dx2 = mul_div64(st->mat.c, ctx->dev_w, ctx->vb_w);
    dy2 = mul_div64(st->mat.d, ctx->dev_h, ctx->vb_h);
    if (dx1 > lim) dx1 = lim; else if (dx1 < -lim) dx1 = -lim;
    if (dy1 > lim) dy1 = lim; else if (dy1 < -lim) dy1 = -lim;
    if (dx2 > lim) dx2 = lim; else if (dx2 < -lim) dx2 = -lim;
    if (dy2 > lim) dy2 = lim; else if (dy2 < -lim) dy2 = -lim;
    len1 = (int64_t)isqrt64((uint64_t)(dx1 * dx1) + (uint64_t)(dy1 * dy1));
    len2 = (int64_t)isqrt64((uint64_t)(dx2 * dx2) + (uint64_t)(dy2 * dy2));
    avg_dev = (len1 + len2 + 1) / 2;
    if (avg_dev < 1)
        avg_dev = 1;
    /* st->stroke_width is 16.16, avg_dev is 1/256 px -> convert to 1/16 px. */
    {
        int64_t w_dev = mul_div64(st->stroke_width, avg_dev, FP_ONE);
        int64_t w_s4 = (w_dev + 8) >> 4;
        return w_s4 < 16 ? 16 : w_s4;
    }
}

/* ---------------------------------------------------------- pixel compositing */

static void svg_blend_px(struct image *im, int x, int y, uint32_t argb)
{
    unsigned long idx;
    uint32_t sa, da, sr, sg, sb, dr, dg, db, w_src, w_dst, denom;
    uint32_t dc;

    if (x < 0 || y < 0 || x >= im->w || y >= im->h)
        return;
    sa = argb >> 24;
    if (sa == 0)
        return;

    idx = (unsigned long)y * (unsigned long)im->w + (unsigned long)x;
    sr = (argb >> 16) & 0xffu;
    sg = (argb >> 8) & 0xffu;
    sb = argb & 0xffu;
    da = im->alpha[idx];

    if (sa >= 255 || da == 0) {
        im->px[idx] = img__rgb((int)sr, (int)sg, (int)sb);
        im->alpha[idx] = (uint8_t)sa;
        return;
    }

    dc = im->px[idx];
    dr = (dc >> 16) & 0xffu;
    dg = (dc >> 8) & 0xffu;
    db = dc & 0xffu;

    w_src = sa * 255u;
    w_dst = da * (255u - sa);
    denom = w_src + w_dst;
    if (denom == 0) {
        im->px[idx] = 0;
        im->alpha[idx] = 0;
        return;
    }

    im->alpha[idx] = (uint8_t)((denom + 127u) / 255u);
    im->px[idx] = img__rgb(
        (int)((w_src * sr + w_dst * dr + denom / 2u) / denom),
        (int)((w_src * sg + w_dst * dg + denom / 2u) / denom),
        (int)((w_src * sb + w_dst * db + denom / 2u) / denom));
}

/* ---------------------------------------------------------- path builder */

static void svg_add_pt(struct svg_ctx *ctx, int *nsub, int cur_sub,
                       int *npts, int64_t dx, int64_t dy)
{
    (void)nsub;
    if (cur_sub < 0 || *npts >= SVG_MAX_PTS)
        return;
    ctx->pt_x[*npts] = dx;
    ctx->pt_y[*npts] = dy;
    (*npts)++;
    ctx->subs[cur_sub].count++;
}

static void svg_flatten_quad(struct svg_ctx *ctx, int *nsub, int cur_sub,
                             int *npts, int64_t x0, int64_t y0,
                             int64_t x1, int64_t y1,
                             int64_t x2, int64_t y2)
{
    int sp = 0;

    ctx->bez[sp].x0 = x0; ctx->bez[sp].y0 = y0;
    ctx->bez[sp].x1 = x1; ctx->bez[sp].y1 = y1;
    ctx->bez[sp].x2 = x2; ctx->bez[sp].y2 = y2;
    ctx->bez[sp].depth = 0;
    sp++;

    while (sp > 0 && *npts < SVG_MAX_PTS) {
        struct svg_bez_item b = ctx->bez[--sp];
        int64_t ex = 2 * b.x1 - b.x0 - b.x2;
        int64_t ey = 2 * b.y1 - b.y0 - b.y2;
        if (ex < 0) ex = -ex;
        if (ey < 0) ey = -ey;

        if ((ex <= 64 && ey <= 64) || b.depth >= 10 || sp + 2 > SVG_BEZ_STACK) {
            svg_add_pt(ctx, nsub, cur_sub, npts, b.x2, b.y2);
        } else {
            int64_t q0x = (b.x0 + b.x1) >> 1;
            int64_t q0y = (b.y0 + b.y1) >> 1;
            int64_t q1x = (b.x1 + b.x2) >> 1;
            int64_t q1y = (b.y1 + b.y2) >> 1;
            int64_t rx = (q0x + q1x) >> 1;
            int64_t ry = (q0y + q1y) >> 1;

            ctx->bez[sp].x0 = rx;  ctx->bez[sp].y0 = ry;
            ctx->bez[sp].x1 = q1x; ctx->bez[sp].y1 = q1y;
            ctx->bez[sp].x2 = b.x2; ctx->bez[sp].y2 = b.y2;
            ctx->bez[sp].depth = b.depth + 1;
            sp++;

            ctx->bez[sp].x0 = b.x0; ctx->bez[sp].y0 = b.y0;
            ctx->bez[sp].x1 = q0x;  ctx->bez[sp].y1 = q0y;
            ctx->bez[sp].x2 = rx;   ctx->bez[sp].y2 = ry;
            ctx->bez[sp].depth = b.depth + 1;
            sp++;
        }
    }
}

static void svg_flatten_cubic(struct svg_ctx *ctx, int *nsub, int cur_sub,
                              int *npts, int64_t x0, int64_t y0,
                              int64_t x1, int64_t y1,
                              int64_t x2, int64_t y2,
                              int64_t x3, int64_t y3)
{
    int sp = 0;

    ctx->bez[sp].x0 = x0; ctx->bez[sp].y0 = y0;
    ctx->bez[sp].x1 = x1; ctx->bez[sp].y1 = y1;
    ctx->bez[sp].x2 = x2; ctx->bez[sp].y2 = y2;
    ctx->bez[sp].x3 = x3; ctx->bez[sp].y3 = y3;
    ctx->bez[sp].depth = 0;
    sp++;

    while (sp > 0 && *npts < SVG_MAX_PTS) {
        struct svg_bez_item b = ctx->bez[--sp];
        int64_t d1x = 3 * b.x1 - 2 * b.x0 - b.x3;
        int64_t d1y = 3 * b.y1 - 2 * b.y0 - b.y3;
        int64_t d2x = 3 * b.x2 - b.x0 - 2 * b.x3;
        int64_t d2y = 3 * b.y2 - b.y0 - 2 * b.y3;
        int64_t m;

        if (d1x < 0) d1x = -d1x;
        if (d1y < 0) d1y = -d1y;
        if (d2x < 0) d2x = -d2x;
        if (d2y < 0) d2y = -d2y;
        m = d1x > d1y ? d1x : d1y;
        if (d2x > m) m = d2x;
        if (d2y > m) m = d2y;

        if (m <= 128 || b.depth >= 10 || sp + 2 > SVG_BEZ_STACK) {
            svg_add_pt(ctx, nsub, cur_sub, npts, b.x3, b.y3);
        } else {
            int64_t ax = (b.x0 + b.x1) >> 1, ay = (b.y0 + b.y1) >> 1;
            int64_t bx = (b.x1 + b.x2) >> 1, by = (b.y1 + b.y2) >> 1;
            int64_t cx = (b.x2 + b.x3) >> 1, cy = (b.y2 + b.y3) >> 1;
            int64_t dx = (ax + bx) >> 1, dy = (ay + by) >> 1;
            int64_t ex = (bx + cx) >> 1, ey = (by + cy) >> 1;
            int64_t fx = (dx + ex) >> 1, fy = (dy + ey) >> 1;

            ctx->bez[sp].x0 = fx; ctx->bez[sp].y0 = fy;
            ctx->bez[sp].x1 = ex; ctx->bez[sp].y1 = ey;
            ctx->bez[sp].x2 = cx; ctx->bez[sp].y2 = cy;
            ctx->bez[sp].x3 = b.x3; ctx->bez[sp].y3 = b.y3;
            ctx->bez[sp].depth = b.depth + 1;
            sp++;

            ctx->bez[sp].x0 = b.x0; ctx->bez[sp].y0 = b.y0;
            ctx->bez[sp].x1 = ax;   ctx->bez[sp].y1 = ay;
            ctx->bez[sp].x2 = dx;   ctx->bez[sp].y2 = dy;
            ctx->bez[sp].x3 = fx;   ctx->bez[sp].y3 = fy;
            ctx->bez[sp].depth = b.depth + 1;
            sp++;
        }
    }
}

/* ---------------------------------------------------------- rasterizer */

static void svg_sort_cross(struct svg_cross *c, int n)
{
    int gap, i, j;

    for (gap = n / 2; gap > 0; gap /= 2) {
        for (i = gap; i < n; i++) {
            struct svg_cross tmp = c[i];
            for (j = i; j >= gap && c[j - gap].x > tmp.x; j -= gap)
                c[j] = c[j - gap];
            c[j] = tmp;
        }
    }
}

static void svg_fill_subpaths(struct svg_ctx *ctx, int nsub, int evenodd,
                              uint32_t color)
{
    int64_t ymin = 0, ymax = 0;
    int have = 0, sp, i;
    int y, y0_px, y1_px;

    if (!color || nsub <= 0)
        return;

    for (sp = 0; sp < nsub; sp++) {
        if (ctx->subs[sp].count < 3)
            continue;
        for (i = 0; i < ctx->subs[sp].count; i++) {
            int64_t vy = ctx->pt_y[ctx->subs[sp].start + i];
            if (!have || vy < ymin) ymin = vy;
            if (!have || vy > ymax) ymax = vy;
            have = 1;
        }
    }
    if (!have)
        return;

    y0_px = (int)ceil_div(ymin - DEV_HALF, DEV_ONE);
    y1_px = (int)floor_div(ymax - DEV_HALF, DEV_ONE);
    if (y0_px < 0) y0_px = 0;
    if (y1_px >= ctx->im.h) y1_px = ctx->im.h - 1;

    for (y = y0_px; y <= y1_px; y++) {
        int64_t sy = (int64_t)y * DEV_ONE + DEV_HALF;
        int nc = 0;

        for (sp = 0; sp < nsub; sp++) {
            int base = ctx->subs[sp].start;
            int cnt = ctx->subs[sp].count;
            if (cnt < 3)
                continue;
            for (i = 0; i < cnt; i++) {
                int j = (i == 0) ? (cnt - 1) : (i - 1);
                int64_t py0 = ctx->pt_y[base + j];
                int64_t py1 = ctx->pt_y[base + i];
                int dir = 0;

                if (py0 <= sy && py1 > sy)
                    dir = 1;
                else if (py1 <= sy && py0 > sy)
                    dir = -1;
                if (dir != 0 && nc < SVG_MAX_CROSS) {
                    int64_t px0 = ctx->pt_x[base + j];
                    int64_t px1 = ctx->pt_x[base + i];
                    int64_t ix = px0 + mul_div64(sy - py0, px1 - px0, py1 - py0);
                    ctx->cross[nc].x = ix;
                    ctx->cross[nc].dir = dir;
                    nc++;
                }
            }
        }

        if (nc < 2)
            continue;
        svg_sort_cross(ctx->cross, nc);

        if (evenodd) {
            for (i = 0; i + 1 < nc; i += 2) {
                int x0 = (int)ceil_div(ctx->cross[i].x - DEV_HALF, DEV_ONE);
                int x1 = (int)floor_div(ctx->cross[i + 1].x - DEV_HALF - 1, DEV_ONE);
                int x;
                if (x0 < 0) x0 = 0;
                if (x1 >= ctx->im.w) x1 = ctx->im.w - 1;
                for (x = x0; x <= x1; x++)
                    svg_blend_px(&ctx->im, x, y, color);
            }
        } else {
            int wind = 0;
            for (i = 0; i + 1 < nc; i++) {
                wind += ctx->cross[i].dir;
                if (wind != 0) {
                    int x0 = (int)ceil_div(ctx->cross[i].x - DEV_HALF, DEV_ONE);
                    int x1 = (int)floor_div(ctx->cross[i + 1].x - DEV_HALF - 1, DEV_ONE);
                    int x;
                    if (x0 < 0) x0 = 0;
                    if (x1 >= ctx->im.w) x1 = ctx->im.w - 1;
                    for (x = x0; x <= x1; x++)
                        svg_blend_px(&ctx->im, x, y, color);
                }
            }
        }
    }
}

static void svg_stroke_capsule(struct svg_ctx *ctx,
                               int64_t x0_d, int64_t y0_d,
                               int64_t x1_d, int64_t y1_d,
                               int64_t r_s4, uint32_t color, int use_cov)
{
    int64_t lim = (int64_t)(ctx->im.w > ctx->im.h ? ctx->im.w : ctx->im.h) * 2 + 256;
    int64_t x0, y0, x1, y1, dx, dy, len2, r2;
    int64_t minx, maxx, miny, maxy;
    int px0, px1, py0, py1, px, py;

    if (lim < 4096)
        lim = 4096;
    x0 = x0_d >> 4;
    y0 = y0_d >> 4;
    x1 = x1_d >> 4;
    y1 = y1_d >> 4;

    minx = x0 < x1 ? x0 : x1;
    maxx = x0 > x1 ? x0 : x1;
    miny = y0 < y1 ? y0 : y1;
    maxy = y0 > y1 ? y0 : y1;

    px0 = (int)floor_div(minx - r_s4, 16) - 1;
    px1 = (int)ceil_div(maxx + r_s4, 16) + 1;
    py0 = (int)floor_div(miny - r_s4, 16) - 1;
    py1 = (int)ceil_div(maxy + r_s4, 16) + 1;

    if (px1 < 0 || py1 < 0 || px0 >= ctx->im.w || py0 >= ctx->im.h)
        return;
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 >= ctx->im.w) px1 = ctx->im.w - 1;
    if (py1 >= ctx->im.h) py1 = ctx->im.h - 1;

    /* Clamp huge off-screen coordinates before squaring. */
    if (x0 < -lim * 16) x0 = -lim * 16;
    if (x0 > lim * 16)  x0 = lim * 16;
    if (y0 < -lim * 16) y0 = -lim * 16;
    if (y0 > lim * 16)  y0 = lim * 16;
    if (x1 < -lim * 16) x1 = -lim * 16;
    if (x1 > lim * 16)  x1 = lim * 16;
    if (y1 < -lim * 16) y1 = -lim * 16;
    if (y1 > lim * 16)  y1 = lim * 16;

    dx = x1 - x0;
    dy = y1 - y0;
    len2 = dx * dx + dy * dy;
    r2 = r_s4 * r_s4;

    for (py = py0; py <= py1; py++) {
        int64_t pys = (int64_t)py * 16 + 7;
        int64_t vy = pys - y0;
        for (px = px0; px <= px1; px++) {
            int64_t pxs = (int64_t)px * 16 + 7;
            int64_t vx = pxs - x0;
            int64_t dist2;

            if (len2 == 0) {
                dist2 = vx * vx + vy * vy;
            } else {
                int64_t dot = vx * dx + vy * dy;
                if (dot <= 0) {
                    dist2 = vx * vx + vy * vy;
                } else if (dot >= len2) {
                    int64_t ux = pxs - x1;
                    int64_t uy = pys - y1;
                    dist2 = ux * ux + uy * uy;
                } else {
                    int64_t tq = (dot << 16) / len2;
                    int64_t cx = vx - ((tq * dx) >> 16);
                    int64_t cy = vy - ((tq * dy) >> 16);
                    dist2 = cx * cx + cy * cy;
                }
            }

            if (dist2 <= r2) {
                if (use_cov)
                    ctx->cov[(unsigned long)py * (unsigned long)ctx->im.w + (unsigned long)px] = 1;
                else
                    svg_blend_px(&ctx->im, px, py, color);
            }
        }
    }
}

static void svg_stroke_subpaths(struct svg_ctx *ctx, int nsub,
                                int64_t sw_s4, uint32_t color)
{
    int64_t r_s4 = sw_s4 / 2;
    uint32_t sa = color >> 24;
    int use_cov = (sa > 0 && sa < 255);
    int sp, i;

    if (!color || sw_s4 <= 0 || nsub <= 0)
        return;
    if (r_s4 < 8)
        r_s4 = 8;

    if (use_cov && !ctx->cov) {
        ctx->cov = (uint8_t *)calloc((unsigned long)ctx->im.w * (unsigned long)ctx->im.h, 1);
        if (!ctx->cov)
            use_cov = 0;
    }

    if (use_cov) {
        /* Clear only the bounding box of the stroked path. */
        int64_t minx = 0, maxx = 0, miny = 0, maxy = 0;
        int have = 0, bx0, bx1, by0, by1, x, y;
        for (sp = 0; sp < nsub; sp++) {
            int base = ctx->subs[sp].start;
            int cnt = ctx->subs[sp].count;
            for (i = 0; i < cnt; i++) {
                int64_t px = ctx->pt_x[base + i] >> 4;
                int64_t py = ctx->pt_y[base + i] >> 4;
                if (!have || px < minx) minx = px;
                if (!have || px > maxx) maxx = px;
                if (!have || py < miny) miny = py;
                if (!have || py > maxy) maxy = py;
                have = 1;
            }
        }
        if (!have)
            return;
        bx0 = (int)floor_div(minx - r_s4, 16) - 1;
        bx1 = (int)ceil_div(maxx + r_s4, 16) + 1;
        by0 = (int)floor_div(miny - r_s4, 16) - 1;
        by1 = (int)ceil_div(maxy + r_s4, 16) + 1;
        if (bx0 < 0) bx0 = 0;
        if (by0 < 0) by0 = 0;
        if (bx1 >= ctx->im.w) bx1 = ctx->im.w - 1;
        if (by1 >= ctx->im.h) by1 = ctx->im.h - 1;
        for (y = by0; y <= by1; y++) {
            if (bx1 >= bx0)
                memset(ctx->cov + (unsigned long)y * (unsigned long)ctx->im.w + (unsigned long)bx0,
                       0, (unsigned long)(bx1 - bx0 + 1));
        }

        for (sp = 0; sp < nsub; sp++) {
            int base = ctx->subs[sp].start;
            int cnt = ctx->subs[sp].count;
            if (cnt < 2)
                continue;
            for (i = 1; i < cnt; i++) {
                svg_stroke_capsule(ctx, ctx->pt_x[base + i - 1], ctx->pt_y[base + i - 1],
                                   ctx->pt_x[base + i], ctx->pt_y[base + i],
                                   r_s4, color, 1);
            }
            if (ctx->subs[sp].closed) {
                svg_stroke_capsule(ctx, ctx->pt_x[base + cnt - 1], ctx->pt_y[base + cnt - 1],
                                   ctx->pt_x[base], ctx->pt_y[base],
                                   r_s4, color, 1);
            }
        }

        for (y = by0; y <= by1; y++) {
            const uint8_t *crow = ctx->cov + (unsigned long)y * (unsigned long)ctx->im.w;
            for (x = bx0; x <= bx1; x++) {
                if (crow[x])
                    svg_blend_px(&ctx->im, x, y, color);
            }
        }
        return;
    }

    for (sp = 0; sp < nsub; sp++) {
        int base = ctx->subs[sp].start;
        int cnt = ctx->subs[sp].count;
        if (cnt < 2)
            continue;
        for (i = 1; i < cnt; i++) {
            svg_stroke_capsule(ctx, ctx->pt_x[base + i - 1], ctx->pt_y[base + i - 1],
                               ctx->pt_x[base + i], ctx->pt_y[base + i],
                               r_s4, color, 0);
        }
        if (ctx->subs[sp].closed) {
            svg_stroke_capsule(ctx, ctx->pt_x[base + cnt - 1], ctx->pt_y[base + cnt - 1],
                               ctx->pt_x[base], ctx->pt_y[base],
                               r_s4, color, 0);
        }
    }
}

/* ---------------------------------------------------------- shape emitters */

static void svg_emit_cubic_user(struct svg_ctx *ctx, const struct svg_mat *m,
                                int *nsub, int cur_sub, int *npts,
                                int64_t x0, int64_t y0, int64_t x1, int64_t y1,
                                int64_t x2, int64_t y2, int64_t x3, int64_t y3)
{
    int64_t dx0, dy0, dx1, dy1, dx2, dy2, dx3, dy3;

    svg_xform_pt(ctx, m, x0, y0, &dx0, &dy0);
    svg_xform_pt(ctx, m, x1, y1, &dx1, &dy1);
    svg_xform_pt(ctx, m, x2, y2, &dx2, &dy2);
    svg_xform_pt(ctx, m, x3, y3, &dx3, &dy3);
    svg_flatten_cubic(ctx, nsub, cur_sub, npts,
                      dx0, dy0, dx1, dy1, dx2, dy2, dx3, dy3);
}

static void svg_render_rect(struct svg_ctx *ctx, const struct svg_state *st,
                            uint32_t fill, uint32_t stroke, int64_t sw_s4)
{
    const struct svg_attr *ax = svg_find_attr(ctx->attrs, ctx->nattr, "x");
    const struct svg_attr *ay = svg_find_attr(ctx->attrs, ctx->nattr, "y");
    const struct svg_attr *aw = svg_find_attr(ctx->attrs, ctx->nattr, "width");
    const struct svg_attr *ah = svg_find_attr(ctx->attrs, ctx->nattr, "height");
    const struct svg_attr *arx = svg_find_attr(ctx->attrs, ctx->nattr, "rx");
    const struct svg_attr *ary = svg_find_attr(ctx->attrs, ctx->nattr, "ry");
    int64_t x = 0, y = 0, w = 0, h = 0, rx = 0, ry = 0;
    int pct, have_rx = 0, have_ry = 0;
    int nsub = 1, npts = 0;
    int64_t dx, dy;

    if (ax) svg_parse_len(ax->val, ax->vlen, &x, &pct);
    if (ay) svg_parse_len(ay->val, ay->vlen, &y, &pct);
    if (aw) svg_parse_len(aw->val, aw->vlen, &w, &pct);
    if (ah) svg_parse_len(ah->val, ah->vlen, &h, &pct);
    if (w <= 0 || h <= 0)
        return;
    if (arx) have_rx = svg_parse_len(arx->val, arx->vlen, &rx, &pct) && rx > 0;
    if (ary) have_ry = svg_parse_len(ary->val, ary->vlen, &ry, &pct) && ry > 0;
    if (have_rx && !have_ry) ry = rx;
    if (have_ry && !have_rx) rx = ry;
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx > w / 2) rx = w / 2;
    if (ry > h / 2) ry = h / 2;

    ctx->subs[0].start = 0;
    ctx->subs[0].count = 0;
    ctx->subs[0].closed = 1;

    if (rx > 0 && ry > 0) {
        int64_t kx = fp_mul16(rx, KAPPA_16);
        int64_t ky = fp_mul16(ry, KAPPA_16);
        int64_t x1 = fp_add(x, w), y1 = fp_add(y, h);

        svg_xform_pt(ctx, &st->mat, fp_add(x, rx), y, &dx, &dy);
        svg_add_pt(ctx, &nsub, 0, &npts, dx, dy);

        svg_xform_pt(ctx, &st->mat, fp_add(x1, -rx), y, &dx, &dy);
        svg_add_pt(ctx, &nsub, 0, &npts, dx, dy);
        svg_emit_cubic_user(ctx, &st->mat, &nsub, 0, &npts,
                            fp_add(x1, -rx), y,
                            fp_add(x1, -rx + kx), y,
                            x1, fp_add(y, ry - ky),
                            x1, fp_add(y, ry));

        svg_xform_pt(ctx, &st->mat, x1, fp_add(y1, -ry), &dx, &dy);
        svg_add_pt(ctx, &nsub, 0, &npts, dx, dy);
        svg_emit_cubic_user(ctx, &st->mat, &nsub, 0, &npts,
                            x1, fp_add(y1, -ry),
                            x1, fp_add(y1, -ry + ky),
                            fp_add(x1, -rx + kx), y1,
                            fp_add(x1, -rx), y1);

        svg_xform_pt(ctx, &st->mat, fp_add(x, rx), y1, &dx, &dy);
        svg_add_pt(ctx, &nsub, 0, &npts, dx, dy);
        svg_emit_cubic_user(ctx, &st->mat, &nsub, 0, &npts,
                            fp_add(x, rx), y1,
                            fp_add(x, rx - kx), y1,
                            x, fp_add(y1, -ry + ky),
                            x, fp_add(y1, -ry));

        svg_xform_pt(ctx, &st->mat, x, fp_add(y, ry), &dx, &dy);
        svg_add_pt(ctx, &nsub, 0, &npts, dx, dy);
        svg_emit_cubic_user(ctx, &st->mat, &nsub, 0, &npts,
                            x, fp_add(y, ry),
                            x, fp_add(y, ry - ky),
                            fp_add(x, rx - kx), y,
                            fp_add(x, rx), y);
    } else {
        svg_xform_pt(ctx, &st->mat, x, y, &dx, &dy);
        svg_add_pt(ctx, &nsub, 0, &npts, dx, dy);
        svg_xform_pt(ctx, &st->mat, fp_add(x, w), y, &dx, &dy);
        svg_add_pt(ctx, &nsub, 0, &npts, dx, dy);
        svg_xform_pt(ctx, &st->mat, fp_add(x, w), fp_add(y, h), &dx, &dy);
        svg_add_pt(ctx, &nsub, 0, &npts, dx, dy);
        svg_xform_pt(ctx, &st->mat, x, fp_add(y, h), &dx, &dy);
        svg_add_pt(ctx, &nsub, 0, &npts, dx, dy);
    }

    if (fill)
        svg_fill_subpaths(ctx, nsub, st->fill_evenodd, fill);
    if (stroke)
        svg_stroke_subpaths(ctx, nsub, sw_s4, stroke);
    ctx->shapes++;
}

static void svg_render_ellipse(struct svg_ctx *ctx, const struct svg_state *st,
                               int is_circle, uint32_t fill, uint32_t stroke,
                               int64_t sw_s4)
{
    const struct svg_attr *acx = svg_find_attr(ctx->attrs, ctx->nattr, "cx");
    const struct svg_attr *acy = svg_find_attr(ctx->attrs, ctx->nattr, "cy");
    int64_t cx = 0, cy = 0, rx = 0, ry = 0, kx, ky;
    int pct, nsub = 1, npts = 0;
    int64_t dx, dy;

    if (acx) svg_parse_len(acx->val, acx->vlen, &cx, &pct);
    if (acy) svg_parse_len(acy->val, acy->vlen, &cy, &pct);
    if (is_circle) {
        const struct svg_attr *ar = svg_find_attr(ctx->attrs, ctx->nattr, "r");
        if (ar) svg_parse_len(ar->val, ar->vlen, &rx, &pct);
        ry = rx;
    } else {
        const struct svg_attr *arx = svg_find_attr(ctx->attrs, ctx->nattr, "rx");
        const struct svg_attr *ary = svg_find_attr(ctx->attrs, ctx->nattr, "ry");
        if (arx) svg_parse_len(arx->val, arx->vlen, &rx, &pct);
        if (ary) svg_parse_len(ary->val, ary->vlen, &ry, &pct);
    }
    if (rx <= 0 || ry <= 0)
        return;

    kx = fp_mul16(rx, KAPPA_16);
    ky = fp_mul16(ry, KAPPA_16);

    ctx->subs[0].start = 0;
    ctx->subs[0].count = 0;
    ctx->subs[0].closed = 1;

    svg_xform_pt(ctx, &st->mat, fp_add(cx, rx), cy, &dx, &dy);
    svg_add_pt(ctx, &nsub, 0, &npts, dx, dy);

    svg_emit_cubic_user(ctx, &st->mat, &nsub, 0, &npts,
                        fp_add(cx, rx), cy,
                        fp_add(cx, rx), fp_add(cy, ky),
                        fp_add(cx, kx), fp_add(cy, ry),
                        cx, fp_add(cy, ry));
    svg_emit_cubic_user(ctx, &st->mat, &nsub, 0, &npts,
                        cx, fp_add(cy, ry),
                        fp_add(cx, -kx), fp_add(cy, ry),
                        fp_add(cx, -rx), fp_add(cy, ky),
                        fp_add(cx, -rx), cy);
    svg_emit_cubic_user(ctx, &st->mat, &nsub, 0, &npts,
                        fp_add(cx, -rx), cy,
                        fp_add(cx, -rx), fp_add(cy, -ky),
                        fp_add(cx, -kx), fp_add(cy, -ry),
                        cx, fp_add(cy, -ry));
    svg_emit_cubic_user(ctx, &st->mat, &nsub, 0, &npts,
                        cx, fp_add(cy, -ry),
                        fp_add(cx, kx), fp_add(cy, -ry),
                        fp_add(cx, rx), fp_add(cy, -ky),
                        fp_add(cx, rx), cy);

    if (fill)
        svg_fill_subpaths(ctx, nsub, st->fill_evenodd, fill);
    if (stroke)
        svg_stroke_subpaths(ctx, nsub, sw_s4, stroke);
    ctx->shapes++;
}

static void svg_render_line(struct svg_ctx *ctx, const struct svg_state *st,
                            uint32_t stroke, int64_t sw_s4)
{
    const struct svg_attr *ax1 = svg_find_attr(ctx->attrs, ctx->nattr, "x1");
    const struct svg_attr *ay1 = svg_find_attr(ctx->attrs, ctx->nattr, "y1");
    const struct svg_attr *ax2 = svg_find_attr(ctx->attrs, ctx->nattr, "x2");
    const struct svg_attr *ay2 = svg_find_attr(ctx->attrs, ctx->nattr, "y2");
    int64_t x1 = 0, y1 = 0, x2 = 0, y2 = 0, dx, dy;
    int pct, nsub = 1, npts = 0;

    if (!stroke)
        return;
    if (ax1) svg_parse_len(ax1->val, ax1->vlen, &x1, &pct);
    if (ay1) svg_parse_len(ay1->val, ay1->vlen, &y1, &pct);
    if (ax2) svg_parse_len(ax2->val, ax2->vlen, &x2, &pct);
    if (ay2) svg_parse_len(ay2->val, ay2->vlen, &y2, &pct);

    ctx->subs[0].start = 0;
    ctx->subs[0].count = 0;
    ctx->subs[0].closed = 0;

    svg_xform_pt(ctx, &st->mat, x1, y1, &dx, &dy);
    svg_add_pt(ctx, &nsub, 0, &npts, dx, dy);
    svg_xform_pt(ctx, &st->mat, x2, y2, &dx, &dy);
    svg_add_pt(ctx, &nsub, 0, &npts, dx, dy);

    svg_stroke_subpaths(ctx, nsub, sw_s4, stroke);
    ctx->shapes++;
}

static void svg_render_poly(struct svg_ctx *ctx, const struct svg_state *st,
                            int closed, uint32_t fill, uint32_t stroke,
                            int64_t sw_s4)
{
    const struct svg_attr *ap = svg_find_attr(ctx->attrs, ctx->nattr, "points");
    const char *p, *end;
    int nsub = 1, npts = 0;

    if (!ap)
        return;
    p = ap->val;
    end = ap->val + ap->vlen;
    ctx->subs[0].start = 0;
    ctx->subs[0].count = 0;
    ctx->subs[0].closed = (uint8_t)closed;

    while (p < end && npts < SVG_MAX_PTS) {
        int64_t ux, uy, dx, dy;
        const char *before = p;
        if (!svg_parse_num(&p, end, &ux) || !svg_parse_num(&p, end, &uy))
            break;
        svg_xform_pt(ctx, &st->mat, ux, uy, &dx, &dy);
        svg_add_pt(ctx, &nsub, 0, &npts, dx, dy);
        if (p == before)
            break;
    }

    if (npts < 2)
        return;
    if (fill)
        svg_fill_subpaths(ctx, nsub, st->fill_evenodd, fill);
    if (stroke)
        svg_stroke_subpaths(ctx, nsub, sw_s4, stroke);
    ctx->shapes++;
}

static int svg_parse_flag(const char **pp, const char *end, int *flag)
{
    const char *p = *pp;

    while (p < end && (svg_ws(*p) || *p == ','))
        p++;
    if (p >= end || (*p != '0' && *p != '1'))
        return 0;
    *flag = (*p == '1');
    *pp = p + 1;
    return 1;
}

static void svg_render_path(struct svg_ctx *ctx, const struct svg_state *st,
                            uint32_t fill, uint32_t stroke, int64_t sw_s4)
{
    const struct svg_attr *ad = svg_find_attr(ctx->attrs, ctx->nattr, "d");
    const char *p, *end;
    int64_t cx = 0, cy = 0, sx = 0, sy = 0;
    int64_t last_cx2 = 0, last_cy2 = 0, last_qx = 0, last_qy = 0;
    char cmd = 0, prev_cmd = 0;
    int nsub = 0, npts = 0, cur = -1;

    if (!ad)
        return;
    p = ad->val;
    end = ad->val + ad->vlen;

    while (p < end && npts < SVG_MAX_PTS) {
        int rel;

        while (p < end && (svg_ws(*p) || *p == ','))
            p++;
        if (p >= end)
            break;
        if (svg_alpha_c(*p)) {
            cmd = *p++;
            while (p < end && (svg_ws(*p) || *p == ','))
                p++;
        }
        if (!cmd)
            break;
        rel = (cmd >= 'a' && cmd <= 'z');

        if (cmd == 'Z' || cmd == 'z') {
            if (cur >= 0) {
                ctx->subs[cur].closed = 1;
                cx = sx;
                cy = sy;
            }
            prev_cmd = cmd;
            cmd = 0;
            continue;
        }

        if (cmd == 'M' || cmd == 'm') {
            int64_t nx, ny, dx, dy;
            if (!svg_parse_num(&p, end, &nx) || !svg_parse_num(&p, end, &ny))
                break;
            if (rel) {
                nx = fp_add(cx, nx);
                ny = fp_add(cy, ny);
            }
            if (nsub >= SVG_MAX_SUBS)
                break;
            cur = nsub++;
            ctx->subs[cur].start = npts;
            ctx->subs[cur].count = 0;
            ctx->subs[cur].closed = 0;
            cx = sx = nx;
            cy = sy = ny;
            svg_xform_pt(ctx, &st->mat, cx, cy, &dx, &dy);
            svg_add_pt(ctx, &nsub, cur, &npts, dx, dy);
            prev_cmd = cmd;
            cmd = rel ? 'l' : 'L';
            continue;
        }

        if (cur < 0)
            break;

        if (cmd == 'L' || cmd == 'l') {
            int64_t nx, ny, dx, dy;
            if (!svg_parse_num(&p, end, &nx) || !svg_parse_num(&p, end, &ny))
                break;
            if (rel) {
                nx = fp_add(cx, nx);
                ny = fp_add(cy, ny);
            }
            cx = nx; cy = ny;
            svg_xform_pt(ctx, &st->mat, cx, cy, &dx, &dy);
            svg_add_pt(ctx, &nsub, cur, &npts, dx, dy);
            prev_cmd = cmd;
        } else if (cmd == 'H' || cmd == 'h') {
            int64_t nx, dx, dy;
            if (!svg_parse_num(&p, end, &nx))
                break;
            cx = rel ? fp_add(cx, nx) : nx;
            svg_xform_pt(ctx, &st->mat, cx, cy, &dx, &dy);
            svg_add_pt(ctx, &nsub, cur, &npts, dx, dy);
            prev_cmd = cmd;
        } else if (cmd == 'V' || cmd == 'v') {
            int64_t ny, dx, dy;
            if (!svg_parse_num(&p, end, &ny))
                break;
            cy = rel ? fp_add(cy, ny) : ny;
            svg_xform_pt(ctx, &st->mat, cx, cy, &dx, &dy);
            svg_add_pt(ctx, &nsub, cur, &npts, dx, dy);
            prev_cmd = cmd;
        } else if (cmd == 'Q' || cmd == 'q' || cmd == 'T' || cmd == 't') {
            int64_t qx, qy, nx, ny, ox = cx, oy = cy;
            int64_t dx0, dy0, dx1, dy1, dx2, dy2;

            if (cmd == 'Q' || cmd == 'q') {
                if (!svg_parse_num(&p, end, &qx) || !svg_parse_num(&p, end, &qy) ||
                    !svg_parse_num(&p, end, &nx) || !svg_parse_num(&p, end, &ny))
                    break;
                if (rel) {
                    qx = fp_add(ox, qx); qy = fp_add(oy, qy);
                    nx = fp_add(ox, nx); ny = fp_add(oy, ny);
                }
            } else {
                if (!svg_parse_num(&p, end, &nx) || !svg_parse_num(&p, end, &ny))
                    break;
                if (rel) {
                    nx = fp_add(ox, nx); ny = fp_add(oy, ny);
                }
                if (prev_cmd == 'Q' || prev_cmd == 'q' ||
                    prev_cmd == 'T' || prev_cmd == 't') {
                    qx = fp_add(ox, ox - last_qx);
                    qy = fp_add(oy, oy - last_qy);
                } else {
                    qx = ox; qy = oy;
                }
            }
            svg_xform_pt(ctx, &st->mat, ox, oy, &dx0, &dy0);
            svg_xform_pt(ctx, &st->mat, qx, qy, &dx1, &dy1);
            svg_xform_pt(ctx, &st->mat, nx, ny, &dx2, &dy2);
            svg_flatten_quad(ctx, &nsub, cur, &npts,
                             dx0, dy0, dx1, dy1, dx2, dy2);
            last_qx = qx; last_qy = qy;
            cx = nx; cy = ny;
            prev_cmd = cmd;
        } else if (cmd == 'C' || cmd == 'c' || cmd == 'S' || cmd == 's') {
            int64_t x1, y1, x2, y2, nx, ny, ox = cx, oy = cy;

            if (cmd == 'C' || cmd == 'c') {
                if (!svg_parse_num(&p, end, &x1) || !svg_parse_num(&p, end, &y1) ||
                    !svg_parse_num(&p, end, &x2) || !svg_parse_num(&p, end, &y2) ||
                    !svg_parse_num(&p, end, &nx) || !svg_parse_num(&p, end, &ny))
                    break;
                if (rel) {
                    x1 = fp_add(ox, x1); y1 = fp_add(oy, y1);
                    x2 = fp_add(ox, x2); y2 = fp_add(oy, y2);
                    nx = fp_add(ox, nx); ny = fp_add(oy, ny);
                }
            } else {
                if (!svg_parse_num(&p, end, &x2) || !svg_parse_num(&p, end, &y2) ||
                    !svg_parse_num(&p, end, &nx) || !svg_parse_num(&p, end, &ny))
                    break;
                if (rel) {
                    x2 = fp_add(ox, x2); y2 = fp_add(oy, y2);
                    nx = fp_add(ox, nx); ny = fp_add(oy, ny);
                }
                if (prev_cmd == 'C' || prev_cmd == 'c' ||
                    prev_cmd == 'S' || prev_cmd == 's') {
                    x1 = fp_add(ox, ox - last_cx2);
                    y1 = fp_add(oy, oy - last_cy2);
                } else {
                    x1 = ox; y1 = oy;
                }
            }
            svg_emit_cubic_user(ctx, &st->mat, &nsub, cur, &npts,
                                ox, oy, x1, y1, x2, y2, nx, ny);
            last_cx2 = x2; last_cy2 = y2;
            cx = nx; cy = ny;
            prev_cmd = cmd;
        } else if (cmd == 'A' || cmd == 'a') {
            int64_t rx, ry, xrot, nx, ny, dx, dy;
            int large_arc, sweep;
            if (!svg_parse_num(&p, end, &rx) || !svg_parse_num(&p, end, &ry) ||
                !svg_parse_num(&p, end, &xrot) ||
                !svg_parse_flag(&p, end, &large_arc) ||
                !svg_parse_flag(&p, end, &sweep) ||
                !svg_parse_num(&p, end, &nx) || !svg_parse_num(&p, end, &ny))
                break;
            (void)rx; (void)ry; (void)xrot; (void)large_arc; (void)sweep;
            if (rel) {
                nx = fp_add(cx, nx);
                ny = fp_add(cy, ny);
            }
            cx = nx; cy = ny;
            svg_xform_pt(ctx, &st->mat, cx, cy, &dx, &dy);
            svg_add_pt(ctx, &nsub, cur, &npts, dx, dy);
            prev_cmd = cmd;
        } else {
            break;
        }
    }

    if (nsub <= 0 || npts < 2)
        return;
    if (fill)
        svg_fill_subpaths(ctx, nsub, st->fill_evenodd, fill);
    if (stroke)
        svg_stroke_subpaths(ctx, nsub, sw_s4, stroke);
    ctx->shapes++;
}

/* ---------------------------------------------------------- document walker */

static int svg_is_skip_tag(const char *tag, unsigned long len)
{
    return svg_tag_ieq(tag, len, "defs") ||
           svg_tag_ieq(tag, len, "clipPath") ||
           svg_tag_ieq(tag, len, "mask") ||
           svg_tag_ieq(tag, len, "pattern") ||
           svg_tag_ieq(tag, len, "symbol") ||
           svg_tag_ieq(tag, len, "style") ||
           svg_tag_ieq(tag, len, "title") ||
           svg_tag_ieq(tag, len, "desc") ||
           svg_tag_ieq(tag, len, "metadata");
}

static void svg_dispatch_shape(struct svg_ctx *ctx, const char *tag,
                               unsigned long tlen)
{
    uint32_t fill, stroke;
    int64_t sw_s4;

    if (ctx->shapes >= SVG_MAX_SHAPES)
        return;
    ctx->cur_st = ctx->stack[ctx->state_top];
    svg_apply_attrs(&ctx->cur_st, ctx->attrs, ctx->nattr);

    fill = svg_effective_color(ctx->cur_st.fill, ctx->cur_st.fill_alpha,
                               ctx->cur_st.opacity);
    stroke = svg_effective_color(ctx->cur_st.stroke, ctx->cur_st.stroke_alpha,
                                 ctx->cur_st.opacity);
    sw_s4 = svg_stroke_s4(ctx, &ctx->cur_st);

    if (svg_tag_ieq(tag, tlen, "rect"))
        svg_render_rect(ctx, &ctx->cur_st, fill, stroke, sw_s4);
    else if (svg_tag_ieq(tag, tlen, "circle"))
        svg_render_ellipse(ctx, &ctx->cur_st, 1, fill, stroke, sw_s4);
    else if (svg_tag_ieq(tag, tlen, "ellipse"))
        svg_render_ellipse(ctx, &ctx->cur_st, 0, fill, stroke, sw_s4);
    else if (svg_tag_ieq(tag, tlen, "line"))
        svg_render_line(ctx, &ctx->cur_st, stroke, sw_s4);
    else if (svg_tag_ieq(tag, tlen, "polyline"))
        svg_render_poly(ctx, &ctx->cur_st, 0, fill, stroke, sw_s4);
    else if (svg_tag_ieq(tag, tlen, "polygon"))
        svg_render_poly(ctx, &ctx->cur_st, 1, fill, stroke, sw_s4);
    else if (svg_tag_ieq(tag, tlen, "path"))
        svg_render_path(ctx, &ctx->cur_st, fill, stroke, sw_s4);
}

static int svg_walk_doc(struct svg_ctx *ctx, const char *p, unsigned long len,
                        unsigned long pos)
{
    while (pos < len && ctx->elem_depth > 0) {
        if (p[pos] != '<') {
            pos++;
            continue;
        }
        if (pos + 2 <= len && p[pos + 1] == '?') {
            pos += 2;
            while (pos + 2 <= len && !(p[pos] == '?' && p[pos + 1] == '>'))
                pos++;
            if (pos + 2 > len)
                return IMG_ERR_TRUNCATED;
            pos += 2;
            continue;
        }
        if (pos + 4 <= len && p[pos + 1] == '!' &&
            p[pos + 2] == '-' && p[pos + 3] == '-') {
            pos += 4;
            while (pos + 3 <= len &&
                   !(p[pos] == '-' && p[pos + 1] == '-' && p[pos + 2] == '>'))
                pos++;
            if (pos + 3 > len)
                return IMG_ERR_TRUNCATED;
            pos += 3;
            continue;
        }
        if (pos + 9 <= len && memcmp(p + pos, "<![CDATA[", 9) == 0) {
            pos += 9;
            while (pos + 3 <= len &&
                   !(p[pos] == ']' && p[pos + 1] == ']' && p[pos + 2] == '>'))
                pos++;
            if (pos + 3 > len)
                return IMG_ERR_TRUNCATED;
            pos += 3;
            continue;
        }
        if (pos + 2 <= len && p[pos + 1] == '!') {
            pos += 2;
            while (pos < len && p[pos] != '>')
                pos++;
            if (pos >= len)
                return IMG_ERR_TRUNCATED;
            pos++;
            continue;
        }
        if (pos + 2 <= len && p[pos + 1] == '/') {
            pos += 2;
            while (pos < len && p[pos] != '>')
                pos++;
            if (pos >= len)
                return IMG_ERR_TRUNCATED;
            pos++;
            ctx->elem_depth--;
            if (ctx->elem_depth < SVG_MAX_DEPTH) {
                struct svg_elem_frame fr = ctx->elem_stack[ctx->elem_depth];
                if (fr.is_skip && ctx->skip_depth > 0)
                    ctx->skip_depth--;
                if (fr.has_state && ctx->state_top > 0)
                    ctx->state_top--;
            }
            continue;
        }

        /* Opening element tag. */
        {
            const char *tag = p + pos + 1;
            unsigned long tstart = pos + 1;
            unsigned long tend = tstart;
            unsigned long tlen;
            int self_close = 0, rc;

            while (tend < len && !svg_ws(p[tend]) && p[tend] != '>' &&
                   p[tend] != '/')
                tend++;
            if (tend >= len)
                return IMG_ERR_TRUNCATED;
            tlen = tend - tstart;
            pos = tend;
            ctx->nattr = 0;
            rc = svg_parse_attrs_raw(p, len, &pos, ctx->attrs, SVG_MAX_ATTRS,
                                     &ctx->nattr, &self_close);
            if (rc != IMG_OK)
                return rc;

            if (ctx->skip_depth == 0 && tlen > 0) {
                if (!svg_is_skip_tag(tag, tlen))
                    svg_dispatch_shape(ctx, tag, tlen);
            }

            if (!self_close) {
                int is_skip = svg_is_skip_tag(tag, tlen);
                int has_state = 0;

                if (ctx->elem_depth < SVG_MAX_DEPTH) {
                    if (is_skip) {
                        ctx->skip_depth++;
                    } else if (ctx->skip_depth == 0 &&
                               ctx->state_top + 1 < SVG_MAX_DEPTH) {
                        ctx->stack[ctx->state_top + 1] = ctx->stack[ctx->state_top];
                        ctx->state_top++;
                        svg_apply_attrs(&ctx->stack[ctx->state_top],
                                        ctx->attrs, ctx->nattr);
                        has_state = 1;
                    }
                    ctx->elem_stack[ctx->elem_depth].is_skip = (uint8_t)is_skip;
                    ctx->elem_stack[ctx->elem_depth].has_state = (uint8_t)has_state;
                }
                ctx->elem_depth++;
            }
        }
    }

    return ctx->elem_depth == 0 ? IMG_OK : IMG_ERR_TRUNCATED;
}

static void svg_free_ctx(struct svg_ctx *ctx)
{
    if (!ctx)
        return;
    free(ctx->pt_x);
    free(ctx->pt_y);
    free(ctx->subs);
    free(ctx->cross);
    free(ctx->bez);
    free(ctx->cov);
    free(ctx);
}

int svg_decode(const void *data, unsigned long len, struct image *out)
{
    const char *p = (const char *)data;
    struct svg_ctx *ctx;
    struct svg_root_info ri;
    int rc;

    if (!out)
        return IMG_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    if (!p || len > IMG_MAX_INPUT)
        return IMG_ERR_INVAL;

    ctx = (struct svg_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return IMG_ERR_NOMEM;

    rc = svg_parse_root(p, len, ctx->attrs, &ri);
    if (rc < 0) {
        svg_free_ctx(ctx);
        return rc;
    }
    ctx->nattr = rc;

    ctx->out_w = ri.out_w;
    ctx->out_h = ri.out_h;
    ctx->vb_x = ri.vb_x;
    ctx->vb_y = ri.vb_y;
    ctx->vb_w = ri.vb_w;
    ctx->vb_h = ri.vb_h;
    ctx->dev_ox = ri.dev_ox;
    ctx->dev_oy = ri.dev_oy;
    ctx->dev_w = ri.dev_w;
    ctx->dev_h = ri.dev_h;

    rc = img__alloc(&ctx->im, ctx->out_w, ctx->out_h, 1);
    if (rc != IMG_OK) {
        svg_free_ctx(ctx);
        return rc;
    }
    memset(ctx->im.alpha, 0,
           (unsigned long)ctx->out_w * (unsigned long)ctx->out_h);

    if (ri.self_closing) {
        *out = ctx->im;
        memset(&ctx->im, 0, sizeof(ctx->im));
        svg_free_ctx(ctx);
        return IMG_OK;
    }

    ctx->pt_x  = (int64_t *)calloc(SVG_MAX_PTS, sizeof(int64_t));
    ctx->pt_y  = (int64_t *)calloc(SVG_MAX_PTS, sizeof(int64_t));
    ctx->subs  = (struct svg_subpath *)calloc(SVG_MAX_SUBS, sizeof(*ctx->subs));
    ctx->cross = (struct svg_cross *)calloc(SVG_MAX_CROSS, sizeof(*ctx->cross));
    ctx->bez   = (struct svg_bez_item *)calloc(SVG_BEZ_STACK, sizeof(*ctx->bez));
    if (!ctx->pt_x || !ctx->pt_y || !ctx->subs || !ctx->cross || !ctx->bez) {
        img_free(&ctx->im);
        svg_free_ctx(ctx);
        return IMG_ERR_NOMEM;
    }

    /* Root <svg> initial style. */
    ctx->state_top = 0;
    ctx->stack[0].fill = 0xff000000u;
    ctx->stack[0].stroke = 0;
    ctx->stack[0].current_color = 0x000000u;
    ctx->stack[0].stroke_width = FP_ONE;
    ctx->stack[0].fill_alpha = 255;
    ctx->stack[0].stroke_alpha = 255;
    ctx->stack[0].opacity = 255;
    ctx->stack[0].fill_evenodd = 0;
    mat_ident(&ctx->stack[0].mat);
    svg_apply_attrs(&ctx->stack[0], ctx->attrs, ctx->nattr);
    /* Root <svg> transform applies to children; clear transform on root if not
     * desired? Actually applying root transform if present is valid SVG2. */

    ctx->elem_depth = 1;
    ctx->elem_stack[0].is_skip = 0;
    ctx->elem_stack[0].has_state = 0;

    rc = svg_walk_doc(ctx, p, len, ri.after_tag_pos);
    if (rc != IMG_OK) {
        img_free(&ctx->im);
        svg_free_ctx(ctx);
        return rc;
    }

    *out = ctx->im;
    memset(&ctx->im, 0, sizeof(ctx->im));
    svg_free_ctx(ctx);
    return IMG_OK;
}
