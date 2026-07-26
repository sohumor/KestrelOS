#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "bootinfo.h"
#include "kestrel_abi.h"

/* Kernel virtual window onto the VBE linear framebuffer. The e1000
 * register file already owns 0xFFFFFFFFC0000000..+128 KiB, so start
 * 256 MiB clear of it. The largest mode stage 2 will set is 2560x1440x32:
 * a 14 MiB aperture plus a 14 MiB shadow buffer. */
#define FB_VA_BASE 0xFFFFFFFFD0000000ULL

/* Sanity limits applied to whatever the BIOS reports. 2560x1600 is the
 * widest mode in the bootloader's preference list; the byte cap leaves
 * room for a padded pitch above that. */
#define FB_MAX_W      2560
#define FB_MAX_H      1600
#define FB_MAX_BYTES  (48u * 1024 * 1024)

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
