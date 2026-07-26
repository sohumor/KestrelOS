#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "bootinfo.h"
#include "kestrel_abi.h"

/* Kernel virtual window onto the VBE linear framebuffer. The e1000
 * register file already owns 0xFFFFFFFFC0000000..+128 KiB, so start
 * 256 MiB clear of it. The largest mode stage 2 will set is
 * 1280x1024x32 (5 MiB), comfortably inside FB_MAX_BYTES. */
#define FB_VA_BASE 0xFFFFFFFFD0000000ULL

/* Sanity limits applied to whatever the BIOS reports. */
#define FB_MAX_W      2048
#define FB_MAX_H      1536
#define FB_MAX_BYTES  (32u * 1024 * 1024)

void      fb_init(struct bootinfo *bi);
bool      fb_present(void);
void      fb_get(struct k_fbinfo *out);

/* Shadow buffer: width * height 0x00RRGGBB pixels, no row padding.
 * Every primitive below draws here; nothing reaches the device until
 * one of the flush calls runs. NULL when there is no framebuffer. */
uint32_t *fb_back(void);
uint32_t  fb_width(void);
uint32_t  fb_height(void);

void fb_flush(void);
void fb_flush_rect(int x, int y, int w, int h);

void fb_fill_rect(int x, int y, int w, int h, uint32_t rgb);
void fb_blit(int dx, int dy, int w, int h, const uint32_t *src,
             int src_stride_px);
void fb_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);
void fb_draw_text(int x, int y, const char *s, uint32_t fg, uint32_t bg);

uint64_t fb_phys_base(void);
uint32_t fb_phys_size(void);
