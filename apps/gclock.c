/* gclock.c - an analogue clock.
 *
 * A drawn dial: rim, hour ticks, minute pips, three hands computed from
 * SYS_TIME, and the digital reading underneath. Repainted when the
 * second changes. Times are UTC - KestrelOS has no timezone database.
 *
 * The trigonometry is libgui's fixed-point sine (there is no libm and no
 * guarantee of an FPU in userspace), so every coordinate here is integer
 * arithmetic in Q16.
 */

#include <kestrel.h>
#include <stdio.h>

#include "gui.h"

#define WIN_W  268
#define WIN_H  318
#define FACE_R 118
#define CX     (WIN_W / 2)
#define CY     (FACE_R + 12)

/* Point on the dial: angle in tenths of a degree clockwise from 12. */
static void dial_point(int angle, int radius, int *x, int *y)
{
    *x = CX + (int)(((long long)gui_sin_q16(angle) * radius) >> 16);
    *y = CY - (int)(((long long)gui_cos_q16(angle) * radius) >> 16);
}

static void draw_face(gui_window *win)
{
    int i;

    gui_disc(win, CX, CY, FACE_R + 6, gui_shade(GUI_PANEL, -18));
    gui_disc(win, CX, CY, FACE_R + 2, GUI_PANEL_HI);
    gui_disc(win, CX, CY, FACE_R, gui_shade(GUI_SURFACE, -6));
    gui_circle(win, CX, CY, FACE_R, GUI_EDGE);
    gui_circle(win, CX, CY, FACE_R - 1, gui_shade(GUI_EDGE, -20));

    for (i = 0; i < 60; i++) {
        int angle = i * 60;             /* 6 degrees per minute */
        int x0, y0, x1, y1;
        int hour = (i % 5) == 0;

        dial_point(angle, FACE_R - (hour ? 18 : 8), &x0, &y0);
        dial_point(angle, FACE_R - 4, &x1, &y1);
        gui_line_w(win, x0, y0, x1, y1,
                   hour ? GUI_TEXT : GUI_TEXT_DIM, hour ? 3 : 1);
    }

    for (i = 0; i < 12; i++) {
        static const char *label[12] = { "12", "1", "2", "3", "4", "5",
                                         "6", "7", "8", "9", "10", "11" };
        int x, y;

        dial_point(i * 300, FACE_R - 34, &x, &y);
        gui_text(win, x - gui_text_w(label[i]) / 2, y - GUI_FONT_H / 2,
                 label[i], GUI_TEXT, GUI_TRANSPARENT);
    }
}

static void draw_hands(gui_window *win, const struct gui_tm *tm)
{
    int x, y;
    int hour_angle = (tm->hour % 12) * 300 + tm->min * 5;
    int min_angle = tm->min * 60 + tm->sec;
    int sec_angle = tm->sec * 60;

    dial_point(hour_angle, FACE_R / 2, &x, &y);
    gui_line_w(win, CX, CY, x, y, GUI_TEXT, 6);

    dial_point(min_angle, (FACE_R * 3) / 4, &x, &y);
    gui_line_w(win, CX, CY, x, y, GUI_TEXT, 4);

    dial_point(sec_angle, (FACE_R * 17) / 20, &x, &y);
    gui_line_w(win, CX, CY, x, y, GUI_ERROR, 2);
    /* counterweight, so the second hand reads as pivoting */
    dial_point(sec_angle + 1800, FACE_R / 6, &x, &y);
    gui_line_w(win, CX, CY, x, y, GUI_ERROR, 2);

    gui_disc(win, CX, CY, 6, GUI_PANEL_HI);
    gui_disc(win, CX, CY, 4, GUI_ERROR);
}

static void draw_readout(gui_window *win, const struct gui_tm *tm)
{
    char line[64];
    int y = CY + FACE_R + 16;

    gui_rect(win, 0, y - 4, WIN_W, WIN_H - y + 4, GUI_SURFACE);
    snprintf(line, sizeof(line), "%02d:%02d:%02d UTC", tm->hour, tm->min,
             tm->sec);
    gui_text(win, (WIN_W - gui_text_w(line)) / 2, y, line, GUI_ACCENT,
             GUI_TRANSPARENT);
    snprintf(line, sizeof(line), "%s %d %s %d", gui_day_name(tm->wday),
             tm->day, gui_month_name(tm->mon), tm->year);
    gui_text(win, (WIN_W - gui_text_w(line)) / 2, y + GUI_FONT_H + 4, line,
             GUI_TEXT_DIM, GUI_TRANSPARENT);
}

int main(int argc, char **argv)
{
    struct k_event ev;
    struct gui_tm tm;
    gui_window *win;
    unsigned long last = 0, now;

    (void)argc;
    (void)argv;

    win = gui_open("Clock", 620, 90, WIN_W, WIN_H, 0);
    if (!win) {
        printf("gclock: no window manager available\n");
        return 1;
    }

    for (;;) {
        int got;

        now = gui_time();
        if (now != last) {
            last = now;
            gui_gmtime(now, &tm);
            gui_clear(win, GUI_SURFACE);
            draw_face(win);
            draw_hands(win, &tm);
            draw_readout(win, &tm);
            if (gui_flush(win) != 0)
                break;
        }

        got = gui_next_event(win, &ev, 100);
        if (got < 0)
            break;
        if (got > 0 && ev.type == KEV_CLOSE)
            break;
        if (got > 0 && ev.type == KEV_KEY &&
            (ev.key == 'q' || ev.key == 27))
            break;
    }

    gui_close(win);
    return 0;
}
