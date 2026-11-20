/* cc -O2 -ffunction-sections -fdata-sections -Ikernel/include -Iabi
 * tools/test_console_scroll.c -Wl,--gc-sections -o build/test_console_scroll */
#include <assert.h>
#include <stdio.h>
#include "../kernel/console.c"

struct pixel_cell { char ch; uint32_t fg, bg; };
static struct pixel_cell shadow[CON_MAX_ROWS][CON_MAX_COLS];
static struct pixel_cell device[CON_MAX_ROWS][CON_MAX_COLS];
static bool compositing;
static int flushes, cells_flushed;
static uint8_t before_ch[CON_MAX_ROWS][CON_MAX_COLS];
static uint8_t before_at[CON_MAX_ROWS][CON_MAX_COLS];

bool wm_active(void) { return compositing; }
void fb_draw_char(int x, int y, char ch, uint32_t fg, uint32_t bg)
{
    shadow[y / FONT_H][x / FONT_W] = (struct pixel_cell){ch, fg, bg};
}
void fb_flush_rect(int x, int y, int w, int h)
{
    flushes++;
    for (int r = y / FONT_H; r < (y + h) / FONT_H; r++)
        for (int c = x / FONT_W; c < (x + w) / FONT_W; c++) {
            device[r][c] = shadow[r][c];
            cells_flushed++;
        }
}
void fb_flush(void)
{
    assert(!"scroll must not request a full framebuffer flush");
}

static void verify(void)
{
    for (int r = 0; r < con_h; r++)
        for (int c = 0; c < con_w; c++) {
            uint8_t a = cell_at[r][c];
            bool caret = cursor_on && r == cur_row && c == cur_col;
            assert(device[r][c].ch == (char)cell_ch[r][c]);
            assert(device[r][c].fg == cga_rgb[caret ? a >> 4 : a & 15]);
            assert(device[r][c].bg == cga_rgb[caret ? a & 15 : a >> 4]);
        }
}

static void reset(int rows)
{
    fb_mode = true;
    con_w = 80;
    con_h = rows;
    cur_row = cur_col = 0;
    drawn_row = -1;
    cursor_on = true;
    full_flush = false;
    compositing = false;
    color = 7;
    for (int r = 0; r < con_h; r++) {
        for (int c = 0; c < con_w; c++) {
            cell_ch[r][c] = ' ';
            cell_at[r][c] = color;
        }
        mark_row(r);
    }
    fb_sync();
    flushes = cells_flushed = 0;
}

int main(void)
{
    reset(48);
    cursor_on = false;
    fb_sync();
    flushes = 0;
    scroll();
    fb_sync();
    verify();
    assert(flushes == 0); /* Empty rows already have the right pixels. */

    for (int rows = 1; rows <= 90; rows += 89) {
        reset(rows);
        for (int i = 0; i < 300; i++) {
            cursor_on = i % 3 != 0;
            cur_col = (i * 7) % con_w;
            cur_row = con_h - 1;
            color = (uint8_t)i;
            cell_put(cur_row, cur_col, (char)('!' + i % 90), color);
            /* Both pending dirty cells and an already-painted cursor. */
            if (i % 2)
                fb_sync();
            memcpy(before_ch, cell_ch, sizeof(cell_ch));
            memcpy(before_at, cell_at, sizeof(cell_at));
            compositing = i % 5 == 0;
            int prior_flushes = flushes;
            scroll();
            fb_sync();
            if (compositing)
                assert(flushes == prior_flushes);
            compositing = false;
            fb_sync();
            verify();
            for (int r = 0; r < con_h; r++)
                for (int c = 0; c < con_w; c++) {
                    assert(cell_ch[r][c] == (r + 1 < con_h ? before_ch[r + 1][c] : ' '));
                    assert(cell_at[r][c] == (r + 1 < con_h ? before_at[r + 1][c] : color));
                }
        }
    }
    reset(48);
    cursor_on = false;
    fb_sync();
    cell_put(1, 2, 'X', 7);
    fb_sync();
    cells_flushed = 0;
    scroll();
    fb_sync();
    verify();
    assert(cells_flushed == 2);
    puts("console scroll: empty, sparse, attributes, cursor, pending dirty, compositor, single/max row PASS");
    return 0;
}
