#include <stdio.h>

#define main gclock_app_main
#include "../apps/gclock.c"
#undef main

#define STRIDE (WIN_W + 16)
#define ROWS (WIN_H + 8)
#define GUARD 0xDEADC0DEu

static uint32_t pixels[STRIDE * ROWS];

static int check_frame(int w, int h, int expect_date)
{
    enum { X = 7, Y = 3 };
    gui_window root = {
        .w = STRIDE, .h = ROWS, .px = pixels,
        .clip = { 0, 0, STRIDE, ROWS },
    };
    gui_window frame;
    struct gui_tm tm = { .hour = 12, .min = 34, .sec = 56,
                         .wday = 4, .day = 12, .mon = 10, .year = 2026 };
    int time_pixels = 0, date_pixels = 0;

    for (int i = 0; i < STRIDE * ROWS; i++)
        pixels[i] = GUARD;
    if (gui_paint_view(&frame, &root, gui_mkrc(X, Y, w, h)) != 0)
        return -1;
    paint_clock(&frame, &tm);
    if (gui_peek(&frame, CX, CY) != GUI_ERROR)
        return -1;

    for (int y = 0; y < ROWS; y++) {
        for (int x = 0; x < STRIDE; x++) {
            uint32_t px = pixels[y * STRIDE + x];
            if (x < X || x >= X + w || y < Y || y >= Y + h) {
                if (px != GUARD)
                    return -1;
            } else if (y >= Y + READOUT_Y) {
                if (px == GUI_ACCENT)
                    time_pixels++;
                if (px == GUI_TEXT_DIM)
                    date_pixels++;
            }
        }
    }
    return time_pixels && (date_pixels != 0) == expect_date ? 0 : -1;
}

int main(void)
{
    if (check_frame(WIN_W, WIN_H, 1) < 0 ||
        check_frame(WIN_W - 28, READOUT_Y + 4 + GUI_FONT_H, 0) < 0) {
        puts("FAIL Clock paint panes");
        return 1;
    }
    puts("PASS Clock paint panes: dial and readout stay inside offset and cropped targets");
    return 0;
}
