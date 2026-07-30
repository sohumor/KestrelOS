/* Host regression tests for libgui's pure raster primitives. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gui.h"

#define W 96
#define H 72
#define GUARD 0xA55AA55AU
#define BG 0x102030U

static gui_theme test_theme = {
    .desk_top = 0x111A2A, .desk_bottom = 0x070B12,
    .panel = 0x151F2F, .panel_hi = 0x243249,
    .surface = 0x192436, .surface_alt = 0x223047,
    .sunken = 0x0F1724, .border = 0x0A101A, .edge = 0x3B4D67,
    .text = 0xE9F0F8, .text_dim = 0x91A0B5, .text_inv = 0x101827,
    .accent = 0x60A5FA, .accent_dark = 0x335F91,
    .ok = 0x4ADE80, .warn = 0xFBBF24, .error = 0xFB7185,
    .white = 0xF8FAFC, .black = 0x060A11,
};

const gui_theme *gui_theme_current(void)
{
    return &test_theme;
}

static int changed_pixels(const uint32_t *pixels, uint32_t bg)
{
    int changed = 0;

    for (int i = 0; i < W * H; i++)
        if (pixels[i] != bg)
            changed++;
    return changed;
}

int main(void)
{
    uint32_t storage[W * H + 2];
    gui_window win;

    storage[0] = GUARD;
    storage[W * H + 1] = GUARD;
    for (int i = 0; i < W * H; i++)
        storage[i + 1] = BG;

    memset(&win, 0, sizeof(win));
    win.w = W;
    win.h = H;
    win.px = storage + 1;
    win.clip = gui_mkrc(0, 0, W, H);

    gui_round_rect(&win, gui_mkrc(10, 8, 30, 24), 7, 0xCC3355);
    assert(gui_peek(&win, 10, 8) == BG);
    assert(gui_peek(&win, 25, 20) == 0xCC3355);

    gui_round_frame(&win, gui_mkrc(44, 8, 30, 24), 7, 0x33CC88);
    assert(gui_peek(&win, 59, 8) == 0x33CC88);
    assert(gui_peek(&win, 59, 20) == BG);

    gui_hgradient(&win, gui_mkrc(4, 38, 40, 8), 0x000000, 0xFFFFFF);
    assert(gui_peek(&win, 4, 40) == 0x000000);
    assert(gui_peek(&win, 43, 40) == 0xFFFFFF);

    for (int icon = GUI_ICON_APPS; icon <= GUI_ICON_CHECK; icon++)
        gui_icon(&win, 48 + (icon % 4) * 11, 38 + (icon / 4) * 7,
                 16, (enum gui_icon)icon, 0xFFFFFF);

    assert(changed_pixels(win.px, BG) > 500);
    assert(storage[0] == GUARD);
    assert(storage[W * H + 1] == GUARD);
    assert(gui_mix(0x000000, 0xFFFFFF, 128) == 0x7F7F7F);
    assert(gui_shade(0x808080, 25) == 0x9F9F9F);

    puts("gui primitives: ok");
    return 0;
}
