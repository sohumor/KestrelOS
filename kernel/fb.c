#include "kernel.h"
#include "string.h"
#include "pmm.h"
#include "vmm.h"
#include "console.h"
#include "font.h"
#include "fb.h"

/* VBE linear framebuffer.
 *
 * Stage 2 picks a 32-bpp direct-color mode before entering long mode and
 * hands the geometry over in the bootinfo block. The aperture lives above
 * RAM, outside the direct map, so it is mapped explicitly at FB_VA_BASE.
 * It is mapped *write-back* on purpose: an uncached aperture makes even
 * scrolling a text console unusable.
 *
 * All drawing goes to a shadow buffer in ordinary RAM (tight rows, no
 * padding) and is pushed to the device by fb_flush()/fb_flush_rect(),
 * which is where the device pitch is honoured. Reading back from the
 * aperture is slow, so nothing ever does.
 *
 * Every primitive clips against the screen; negative origins and
 * oversized rectangles are silently trimmed.
 */

static bool      have_fb;
static uint8_t  *dev;            /* mapped aperture, device pitch */
static uint32_t *back;           /* shadow, fbw pixels per row */
static uint32_t  fbw, fbh, fbpitch, fbbpp;
static uint64_t  fbphys;
static uint32_t  fbsize;         /* bytes of aperture actually mapped */

/* Trim a rectangle to the screen. On entry the origin may be negative and
 * the extents may run past the edges; on exit they are inside the screen
 * and ox/oy hold how many pixels were cut off the left and top (the source
 * offset a blit has to skip). Returns false if nothing survives. */
static bool clip(int *x, int *y, int *w, int *h, int *ox, int *oy)
{
    int cx = *x, cy = *y, cw = *w, ch = *h;
    int dx = 0, dy = 0;

    if (!have_fb || cw <= 0 || ch <= 0)
        return false;
    if (cx < 0) {
        dx = -cx;
        cw -= dx;
        cx = 0;
    }
    if (cy < 0) {
        dy = -cy;
        ch -= dy;
        cy = 0;
    }
    if (cx > (int)fbw || cy > (int)fbh)
        return false;
    if (cx + cw > (int)fbw)
        cw = (int)fbw - cx;
    if (cy + ch > (int)fbh)
        ch = (int)fbh - cy;
    if (cw <= 0 || ch <= 0)
        return false;

    *x = cx;
    *y = cy;
    *w = cw;
    *h = ch;
    if (ox)
        *ox = dx;
    if (oy)
        *oy = dy;
    return true;
}

void fb_init(struct bootinfo *bi)
{
    have_fb = false;
    dev = NULL;
    back = NULL;
    fbw = fbh = fbpitch = fbbpp = fbsize = 0;
    fbphys = 0;

    if (!bi || !bi->fb_present) {
        kprintf("fb: no linear framebuffer, staying in text mode\n");
        return;
    }

    uint32_t w = bi->fb_width, h = bi->fb_height;
    uint32_t pitch = bi->fb_pitch, bpp = bi->fb_bpp;
    uint64_t phys = bi->fb_phys;

    if (bpp != 32 || w < 320 || h < 200 || w > FB_MAX_W || h > FB_MAX_H ||
        pitch < w * 4 || phys == 0 || (phys & (PAGE_SIZE - 1))) {
        kprintf("fb: rejecting mode %ux%u %ubpp pitch %u at 0x%lx\n",
                w, h, bpp, pitch, phys);
        return;
    }

    uint64_t aperture = (uint64_t)pitch * h;
    uint64_t shadow = (uint64_t)w * h * 4;
    if (aperture > FB_MAX_BYTES || shadow > FB_MAX_BYTES) {
        kprintf("fb: mode %ux%u needs too much memory\n", w, h);
        return;
    }

    /* pmm_alloc_contig() panics when it cannot satisfy a request, so ask
     * first and fall back to the text console rather than killing the
     * machine over a cosmetic feature. */
    int npages = (int)((shadow + PAGE_SIZE - 1) / PAGE_SIZE);
    if (pmm_free_pages() < (uint64_t)npages + 256) {
        kprintf("fb: not enough memory for a %d-page shadow buffer\n", npages);
        return;
    }
    uint64_t backphys = pmm_alloc_contig(npages);
    back = (uint32_t *)P2V(backphys);

    uint64_t mapped = (aperture + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint64_t off = 0; off < mapped; off += PAGE_SIZE)
        vmm_map_page(vmm_kernel_pml4(), FB_VA_BASE + off, phys + off,
                     PTE_W | PTE_P);

    dev = (uint8_t *)FB_VA_BASE;
    fbw = w;
    fbh = h;
    fbpitch = pitch;
    fbbpp = bpp;
    fbphys = phys;
    fbsize = (uint32_t)mapped;
    have_fb = true;

    memset(back, 0, shadow);
    fb_flush();

    /* Re-run console_init() so the system console switches to the
     * framebuffer backend the moment there is one; it is idempotent, so
     * kmain() calling it again afterwards is harmless. */
    console_init();

    kprintf("fb: %ux%u %u bpp, pitch %u, aperture 0x%lx (%u KiB), "
            "shadow %u KiB\n",
            fbw, fbh, fbbpp, fbpitch, fbphys, fbsize / 1024,
            (uint32_t)(shadow / 1024));
}

bool fb_present(void)
{
    return have_fb;
}

void fb_get(struct k_fbinfo *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!have_fb)
        return;
    out->width = fbw;
    out->height = fbh;
    out->pitch = fbpitch;
    out->bpp = fbbpp;
    out->present = 1;
}

uint32_t *fb_back(void)
{
    return have_fb ? back : NULL;
}

uint32_t fb_width(void)
{
    return fbw;
}

uint32_t fb_height(void)
{
    return fbh;
}

uint64_t fb_phys_base(void)
{
    return fbphys;
}

uint32_t fb_phys_size(void)
{
    return fbsize;
}

void fb_flush(void)
{
    if (!have_fb)
        return;
    if (fbpitch == fbw * 4) {
        memcpy(dev, back, (size_t)fbw * fbh * 4);
        return;
    }
    for (uint32_t y = 0; y < fbh; y++)
        memcpy(dev + (uint64_t)y * fbpitch, back + (uint64_t)y * fbw,
               (size_t)fbw * 4);
}

void fb_flush_rect(int x, int y, int w, int h)
{
    if (!clip(&x, &y, &w, &h, NULL, NULL))
        return;
    for (int r = 0; r < h; r++)
        memcpy(dev + (uint64_t)(y + r) * fbpitch + (uint64_t)x * 4,
               back + (uint64_t)(y + r) * fbw + x, (size_t)w * 4);
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t rgb)
{
    if (!clip(&x, &y, &w, &h, NULL, NULL))
        return;
    for (int r = 0; r < h; r++) {
        uint32_t *p = back + (uint64_t)(y + r) * fbw + x;
        for (int c = 0; c < w; c++)
            p[c] = rgb;
    }
}

void fb_blit(int dx, int dy, int w, int h, const uint32_t *src,
             int src_stride_px)
{
    int ox = 0, oy = 0;

    if (!src || src_stride_px <= 0)
        return;
    if (!clip(&dx, &dy, &w, &h, &ox, &oy))
        return;
    for (int r = 0; r < h; r++)
        memcpy(back + (uint64_t)(dy + r) * fbw + dx,
               src + (uint64_t)(oy + r) * src_stride_px + ox, (size_t)w * 4);
}

void fb_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg)
{
    if (!have_fb)
        return;

    const uint8_t *glyph = font_glyph((unsigned char)c);

    for (int r = 0; r < FONT_H; r++) {
        int py = y + r;
        if (py < 0 || py >= (int)fbh)
            continue;
        uint32_t *row = back + (uint64_t)py * fbw;
        uint8_t bits = glyph[r];
        for (int b = 0; b < FONT_W; b++) {
            int px = x + b;
            if (px < 0 || px >= (int)fbw)
                continue;
            row[px] = (bits & (0x80u >> b)) ? fg : bg;
        }
    }
}

void fb_draw_text(int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
    if (!have_fb || !s)
        return;
    for (; *s; s++, x += FONT_W) {
        if (x >= (int)fbw)
            break;
        fb_draw_char(x, y, *s, fg, bg);
    }
}
