/* Hangar image encoding and validation. See hangar_fmt.h. */

#include "hangar_fmt.h"

uint32_t hangar_fnv(uint32_t h, const void *data, size_t len)
{
    const uint8_t *p = data;

    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193U;
    }
    return h;
}

size_t hangar_rle_bound(size_t n)
{
    return n + n / 2 + 2;
}

size_t hangar_rle_encode(const uint32_t *src, size_t n,
                         uint32_t *dst, size_t cap)
{
    size_t i = 0, o = 0;

    while (i < n) {
        size_t run = 1;

        while (i + run < n && src[i + run] == src[i] &&
               run < (size_t)(HANGAR_RLE_RUN - 1))
            run++;
        if (run >= 3) {
            if (o + 2 > cap)
                return 0;
            dst[o++] = HANGAR_RLE_RUN | (uint32_t)run;
            dst[o++] = src[i];
            i += run;
            continue;
        }

        /* Literal: extend until a run of three starts or input ends. */
        size_t start = i, lit = 0;
        while (i < n) {
            if (i + 2 < n && src[i] == src[i + 1] && src[i] == src[i + 2])
                break;
            i++;
            lit++;
        }
        if (o + 1 + lit > cap)
            return 0;
        dst[o++] = (uint32_t)lit;
        for (size_t k = 0; k < lit; k++)
            dst[o++] = src[start + k];
    }
    return o;
}

int hangar_rle_decode(const uint32_t *src, size_t nsrc,
                      uint32_t *dst, size_t n)
{
    size_t i = 0, o = 0;

    while (i < nsrc) {
        uint32_t ctrl = src[i++];
        size_t count = ctrl & ~HANGAR_RLE_RUN;

        if (count == 0 || count > n - o)
            return -1;
        if (ctrl & HANGAR_RLE_RUN) {
            if (i >= nsrc)
                return -1;
            for (size_t k = 0; k < count; k++)
                dst[o++] = src[i];
            i++;
        } else {
            if (count > nsrc - i)
                return -1;
            for (size_t k = 0; k < count; k++)
                dst[o++] = src[i++];
        }
    }
    return o == n ? 0 : -1;
}

void hangar_thumb(const uint32_t *pix, int w, int h,
                  uint32_t *out, int tw, int th, uint32_t bg)
{
    int dw, dh, ox, oy;

    for (int i = 0; i < tw * th; i++)
        out[i] = bg;
    if (!pix || w <= 0 || h <= 0 || tw <= 0 || th <= 0)
        return;

    /* Fit preserving aspect ratio. */
    if ((long)w * th >= (long)h * tw) {
        dw = tw;
        dh = (int)((long)h * tw / w);
    } else {
        dh = th;
        dw = (int)((long)w * th / h);
    }
    if (dw < 1)
        dw = 1;
    if (dh < 1)
        dh = 1;
    ox = (tw - dw) / 2;
    oy = (th - dh) / 2;

    for (int ty = 0; ty < dh; ty++) {
        int y0 = (int)((long)ty * h / dh);
        int y1 = (int)((long)(ty + 1) * h / dh);
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int tx = 0; tx < dw; tx++) {
            int x0 = (int)((long)tx * w / dw);
            int x1 = (int)((long)(tx + 1) * w / dw);
            uint64_t r = 0, g = 0, b = 0, cnt = 0;
            int sx = (x1 - x0 + 3) / 4, sy = (y1 - y0 + 3) / 4;

            if (x1 <= x0)
                x1 = x0 + 1;
            if (sx < 1)
                sx = 1;
            if (sy < 1)
                sy = 1;
            /* Sample at most a 4x4 grid per output pixel: thumbnails of a
             * 1440p window must stay cheap. */
            for (int y = y0; y < y1; y += sy) {
                for (int x = x0; x < x1; x += sx) {
                    uint32_t p = pix[(long)y * w + x];
                    r += (p >> 16) & 0xFF;
                    g += (p >> 8) & 0xFF;
                    b += p & 0xFF;
                    cnt++;
                }
            }
            out[(oy + ty) * tw + ox + tx] =
                0xFF000000U | (uint32_t)((r / cnt) << 16) |
                (uint32_t)((g / cnt) << 8) | (uint32_t)(b / cnt);
        }
    }
}

static uint32_t manifest_sum(const struct hangar_manifest *m)
{
    struct hangar_manifest tmp = *m;

    tmp.checksum = 0;
    return hangar_fnv(HANGAR_FNV_INIT, &tmp, sizeof(tmp));
}

void hangar_manifest_seal(struct hangar_manifest *m)
{
    m->magic = HANGAR_MAGIC;
    m->version = HANGAR_VERSION;
    m->size = (uint32_t)sizeof(*m);
    m->checksum = manifest_sum(m);
}

static int str_terminated(const char *s, size_t cap)
{
    for (size_t i = 0; i < cap; i++)
        if (s[i] == '\0')
            return 1;
    return 0;
}

const char *hangar_manifest_check(const struct hangar_manifest *m)
{
    if (m->magic != HANGAR_MAGIC)
        return "not a hangar image";
    if (m->version != HANGAR_VERSION || m->size != sizeof(*m))
        return "hangar image from another KestrelOS version";
    if (m->checksum != manifest_sum(m))
        return "hangar manifest is corrupt";
    if (m->narea > HANGAR_MAX_AREAS || m->nfile > HANGAR_MAX_FILES ||
        m->nwin > HANGAR_MAX_WINS || m->npages > HANGAR_MAX_PAGES ||
        m->nchunks > HANGAR_MAX_CHUNKS)
        return "hangar manifest counts out of range";
    if ((m->npages == 0) != (m->nchunks == 0))
        return "hangar memory stream is inconsistent";
    if (!str_terminated(m->name, sizeof(m->name)) ||
        !str_terminated(m->unveil, sizeof(m->unveil)))
        return "hangar manifest strings are unterminated";
    if (m->heap_start > m->brk || m->brk >= HANGAR_USER_TOP)
        return "hangar heap bounds are invalid";
    for (uint32_t i = 0; i < m->narea; i++) {
        const struct hangar_area *a = &m->areas[i];
        /* ELF segments need not be page aligned. */
        if (a->start >= a->end || a->end > HANGAR_USER_TOP)
            return "hangar memory area is invalid";
    }
    for (uint32_t i = 0; i < m->nfile; i++) {
        const struct hangar_file *f = &m->files[i];
        if (f->fd < 0 || f->fd >= HANGAR_MAX_FILES ||
            !str_terminated(f->path, sizeof(f->path)) || f->path[0] != '/')
            return "hangar open-file record is invalid";
        for (uint32_t j = 0; j < i; j++)
            if (m->files[j].fd == f->fd)
                return "hangar open-file record is duplicated";
    }
    for (uint32_t i = 0; i < m->nwin; i++) {
        const struct hangar_win *w = &m->wins[i];
        if (w->w <= 0 || w->h <= 0 || w->w > HANGAR_WIN_MAX_W ||
            w->h > HANGAR_WIN_MAX_H || w->workspace > 3 ||
            !str_terminated(w->title, sizeof(w->title)))
            return "hangar window record is invalid";
        for (uint32_t j = 0; j < i; j++)
            if (m->wins[j].uwid == w->uwid)
                return "hangar window record is duplicated";
    }
    if (m->regs.rip >= HANGAR_USER_TOP || m->regs.rsp >= HANGAR_USER_TOP)
        return "hangar register state is outside user space";
    if (m->thumb_w > 512 || m->thumb_h > 512)
        return "hangar thumbnail is invalid";
    return 0;
}
