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

static void test_paint_views(void)
{
    uint32_t backing[8 * 6];
    const uint32_t image[] = { 1, 2, 0, 3, 4, 0 };
    gui_window root = {
        .w = 8, .h = 6, .px = backing, .open = 1,
        .clip = { 1, 1, 6, 4 },
    };
    gui_window pane = { 0 }, inset;

    for (unsigned i = 0; i < sizeof(backing) / sizeof(*backing); i++)
        backing[i] = GUARD;
    assert(gui_paint_view(&pane, &root, gui_mkrc(0, 1, 3, 3)) == -1);
    assert(pane.px == NULL);
    assert(gui_paint_view(&pane, &root, gui_mkrc(2, 1, 4, 4)) == 0);
    assert(!pane.open && pane.px == &backing[10]);
    gui_clear(&pane, BG);

    gui_clip(&pane, gui_mkrc(1, 0, 3, 4));
    assert(gui_paint_view(&inset, &pane, gui_mkrc(0, 0, 2, 3)) == -1);
    assert(gui_paint_view(&inset, &pane, gui_mkrc(1, 1, 2, 3)) == 0);
    gui_clip(&inset, gui_mkrc(0, 0, 1, 1));
    gui_unclip(&inset);
    gui_bitmap(&inset, 0, 1, 2, 2, image, 3);
    gui_scroll_up(&inset, 1, BG);
    gui_blend_pixel(&inset, 1, 2, 0xFFFFFF, 255);
    gui_pixel(&inset, 2, 0, 0);
    gui_pixel(&inset, 0, -1, 0);
    assert(gui_peek(&inset, 0, 0) == 1);
    assert(gui_peek(&inset, 1, 1) == 4);
    assert(gui_peek(&inset, 1, 2) == 0xFFFFFF);

    for (int y = 0; y < root.h; y++) {
        for (int x = 0; x < root.w; x++) {
            uint32_t expected = GUARD;
            if (x >= 2 && x < 6 && y >= 1 && y < 5)
                expected = BG;
            if (x >= 3 && x < 5 && y >= 2 && y < 4)
                expected = image[(y - 2) * 3 + (x - 3)];
            if (x == 4 && y == 4)
                expected = 0xFFFFFF;
            assert(backing[y * root.w + x] == expected);
        }
    }
    puts("gui paint views: nested bounds and backing stride ok");
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

    /* Filled discs and weighted lines: the primitives gpaint's brush,
     * shapes and eyedropper preview exercise. */
    gui_fill_circle(&win, 80, 66, 5, 0x2277EE);
    assert(gui_peek(&win, 80, 66) == 0x2277EE);
    assert(gui_peek(&win, 78, 66) == 0x2277EE);
    gui_disc(&win, 12, 52, 4, 0xEE2277);
    assert(gui_peek(&win, 12, 52) == 0xEE2277);

    gui_line_w(&win, 4, 60, 44, 60, 0x0A0A0A, 3);
    assert(gui_peek(&win, 24, 60) == 0x0A0A0A);
    assert(gui_peek(&win, 24, 61) == 0x0A0A0A);
    assert(gui_peek(&win, 24, 59) == 0x0A0A0A);
    assert(gui_peek(&win, 24, 62) == BG);
    assert(gui_peek(&win, 24, 58) == BG);
    gui_line_w(&win, 8, 66, 8, 70, 0x335577, 3);
    assert(gui_peek(&win, 7, 68) == 0x335577);
    assert(gui_peek(&win, 9, 68) == 0x335577);

    assert(changed_pixels(win.px, BG) > 500);
    assert(storage[0] == GUARD);
    assert(storage[W * H + 1] == GUARD);
    assert(gui_mix(0x000000, 0xFFFFFF, 128) == 0x7F7F7F);
    assert(gui_shade(0x808080, 25) == 0x9F9F9F);

    test_paint_views();
    puts("gui primitives: ok");
    return 0;
}
