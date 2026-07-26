#pragma once

/* KestrelOS image decoding: PNG, GIF, baseline JPEG and BMP.
 *
 * Everything here is pure computation over a caller-supplied byte buffer:
 * no syscalls, no I/O, no floating point, no recursion. The decoders are
 * written to be safe on hostile input -- every allocation is bounded by
 * the limits below and every read is range checked -- because image
 * decoding is the classic way to take a browser apart.
 *
 * Pixels come out in the framebuffer's format, 0x00RRGGBB. The hardware
 * has no alpha channel, so transparency is reported separately as an
 * 8-bit coverage plane for the caller to composite with.
 */

#include <stdint.h>

/* ---------------------------------------------------------------- limits */

/* Refuse anything larger than this on either axis. */
#define IMG_MAX_DIM      8192
/* ...and refuse anything with more pixels than this in total (64 MPix).
 * At 4 bytes a pixel plus a coverage byte that is a 320 MiB ceiling, which
 * is already generous; real page images are thousands of times smaller. */
#define IMG_MAX_PIXELS   67108864UL
/* Largest encoded file we will look at at all (64 MiB). */
#define IMG_MAX_INPUT    (64UL * 1024UL * 1024UL)
/* Largest decompressed PNG image data (post-inflate) we will accept.
 * The exact expected size is computed from IHDR, so this is only a
 * backstop against absurd headers. */
#define IMG_MAX_RAW      (320UL * 1024UL * 1024UL)

/* ---------------------------------------------------------------- image */

struct image {
    int       w, h;       /* > 0 on success */
    uint32_t *px;         /* w*h pixels, 0x00RRGGBB, no row padding */
    int       has_alpha;  /* non-zero when alpha[] is meaningful */
    uint8_t  *alpha;      /* w*h coverage, 255 = opaque; NULL if !has_alpha */
};

/* ---------------------------------------------------------------- status */

enum {
    IMG_OK            =  0,
    IMG_ERR_FORMAT    = -1,  /* not a container we recognise */
    IMG_ERR_TRUNCATED = -2,  /* ran off the end of the buffer */
    IMG_ERR_CORRUPT   = -3,  /* structurally invalid / failed a checksum */
    IMG_ERR_UNSUPPORTED = -4,/* recognised, but a variant we do not decode */
    IMG_ERR_TOOBIG    = -5,  /* exceeds the limits above */
    IMG_ERR_NOMEM     = -6,  /* allocation failed */
    IMG_ERR_INVAL     = -7,  /* bad arguments from the caller */
    IMG_ERR_PROGRESSIVE = -8 /* progressive JPEG: a separate state machine */
};

/* A short, stable, human-readable description of a status code. Never NULL. */
const char *img_error(int code);

/* ---------------------------------------------------------------- formats */

enum {
    IMG_FMT_UNKNOWN = 0,
    IMG_FMT_PNG,
    IMG_FMT_GIF,
    IMG_FMT_JPEG,
    IMG_FMT_BMP
};

/* Identify a buffer by its magic bytes. Never reads past len. */
int img_sniff(const void *data, unsigned long len);

/* Name of a format constant, e.g. "PNG". Never NULL. */
const char *img_format_name(int fmt);

/* ---------------------------------------------------------------- decoding */

/* Sniff the format and decode. On IMG_OK, *out owns heap memory that must
 * be released with img_free(). On failure *out is zeroed and nothing is
 * left allocated. */
int img_decode(const void *data, unsigned long len, struct image *out);

/* Release everything a successful decode/scale allocated and zero *im.
 * Safe on a zeroed or already-freed struct, and on NULL. */
void img_free(struct image *im);

/* Header-only probe: the intrinsic size without decoding the pixels, which
 * is what layout needs before it has the image. `frames` is the animation
 * frame count (1 for stills, and for GIF the number of image descriptors
 * found). Cheap: parses headers only. */
struct img_info {
    int fmt;
    int w, h;
    int frames;
    int has_alpha;   /* the format declares transparency */
};
int img_probe(const void *data, unsigned long len, struct img_info *out);

/* ---------------------------------------------------------------- scaling */

/* Resample src to exactly w x h. Uses an area-average (box) filter on any
 * axis that shrinks and bilinear interpolation on any axis that grows, so
 * a mixed resize gets the right filter on each axis. Colour is filtered
 * alpha-weighted, so fully transparent pixels do not bleed their RGB into
 * their neighbours. All integer arithmetic. *out is a fresh image to be
 * released with img_free(); src is not modified and may not alias out. */
int img_scale(const struct image *src, int w, int h, struct image *out);

/* ---------------------------------------------------------- per-format */

/* These may be called directly when the format is already known. Each
 * behaves exactly like img_decode() once the format matches. */
int png_decode(const void *data, unsigned long len, struct image *out);
int jpeg_decode(const void *data, unsigned long len, struct image *out);
int bmp_decode(const void *data, unsigned long len, struct image *out);

/* GIF, with the frame count reported separately. Only the FIRST frame is
 * decoded -- animation is out of scope. When `frames` is non-NULL it
 * receives the number of image descriptors in the file, so a caller can
 * tell the user "animated GIF, 34 frames, showing frame 1" rather than
 * silently pretending it is a still. */
int gif_decode(const void *data, unsigned long len, struct image *out,
               int *frames);

/* ---------------------------------------------------------- internal */

/* Shared by the decoders: validate w/h against the limits and allocate a
 * zeroed pixel plane (plus a coverage plane when want_alpha). On failure
 * *im is zeroed. */
int img__alloc(struct image *im, long w, long h, int want_alpha);

/* Pack/clamp helpers used by every decoder. */
static inline uint32_t img__rgb(int r, int g, int b)
{
    return ((uint32_t)(r & 0xff) << 16) | ((uint32_t)(g & 0xff) << 8) |
           (uint32_t)(b & 0xff);
}
static inline int img__clamp8(long v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : (int)v);
}
