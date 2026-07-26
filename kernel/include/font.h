#pragma once

#include <stdint.h>

/* KestrelOS' own 8x16 console face, designed in tools/mkfont.py and
 * emitted into kernel/font.c. Each row is one byte, bit 7 = leftmost
 * pixel. Glyphs cover ASCII 0x20..0x7E; anything else gets the box. */

#define FONT_W 8
#define FONT_H 16

#define FONT_FIRST 0x20
#define FONT_LAST  0x7E

extern const uint8_t font8x16[95][16];
extern const uint8_t font8x16_fallback[16];

static inline const uint8_t *font_glyph(unsigned char c)
{
    if (c < FONT_FIRST || c > FONT_LAST)
        return font8x16_fallback;
    return font8x16[c - FONT_FIRST];
}
