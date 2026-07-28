#include "kernel.h"
#include "console.h"
#include "string.h"
#include "io.h"
#include "fb.h"
#include "font.h"
#include "spinlock.h"

/* System console: an ANSI/VT100 subset (CSI 2J/H/f/K/m/A-D and ?25l/h)
 * on top of one of two backends.
 *
 * Without a framebuffer this is the classic 80x25 VGA text buffer at
 * 0xB8000 with a hardware cursor, exactly as before. With one, the same
 * character grid is rasterised through the 8x16 font into fb's shadow
 * buffer, the CGA palette supplies the 16 colours, and a block cursor is
 * drawn by inverting the cell under the caret.
 *
 * The framebuffer path keeps a per-row dirty column range so an ordinary
 * putc rasterises and flushes a single 8x16 cell; a full-screen flush at
 * 1024x768 moves 3 MiB and would be far too slow per character. Scrolling
 * shifts the shadow buffer by one text row with a memmove and repaints
 * only the new bottom line.
 */

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

/* Static grid budget for the framebuffer backend. 320x90 cells of the 8x16
 * font covers 2560x1440, the largest mode the bootloader will select. */
#define CON_MAX_COLS 320
#define CON_MAX_ROWS 90

#define ESC_MAX_PARAMS 4

static volatile uint16_t *vga;
static int cur_row, cur_col;
static uint8_t color;

/* Backend state. con_w/con_h are the live geometry for both backends. */
static bool fb_mode;
static int con_w = VGA_WIDTH, con_h = VGA_HEIGHT;

static uint8_t cell_ch[CON_MAX_ROWS][CON_MAX_COLS];
static uint8_t cell_at[CON_MAX_ROWS][CON_MAX_COLS];
static int16_t row_lo[CON_MAX_ROWS];   /* dirty column range; lo > hi = clean */
static int16_t row_hi[CON_MAX_ROWS];
static bool full_flush;                /* next sync pushes the whole screen */
static bool cursor_on = true;
static int drawn_row = -1, drawn_col;  /* where the block cursor is painted */

/* The 16 CGA colours as 0x00RRGGBB, indexed by enum vga_color. */
static const uint32_t cga_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

/* ANSI escape parser state */
enum esc_state {
    ESC_NONE = 0,   /* plain output */
    ESC_GOT_ESC,    /* saw ESC, waiting for '[' */
    ESC_GOT_CSI,    /* inside CSI: collecting params/final byte */
};

static int esc_state;
static int esc_params[ESC_MAX_PARAMS];
static int esc_nparams;
static int esc_have_digit;
static int esc_private;     /* saw '?' after CSI */
static spinlock_t console_lock = SPINLOCK_INIT;

/* ANSI color index (0-7) -> VGA color */
static const uint8_t ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

/* ---------------- framebuffer backend helpers ---------------- */

static void mark(int r, int c)
{
    if (r < 0 || r >= con_h || c < 0 || c >= con_w)
        return;
    if (c < row_lo[r])
        row_lo[r] = (int16_t)c;
    if (c > row_hi[r])
        row_hi[r] = (int16_t)c;
}

static void mark_row(int r)
{
    if (r < 0 || r >= con_h)
        return;
    row_lo[r] = 0;
    row_hi[r] = (int16_t)(con_w - 1);
}

static void paint_cell(int r, int c)
{
    uint8_t a = cell_at[r][c];
    uint32_t fg = cga_rgb[a & 0x0F];
    uint32_t bg = cga_rgb[(a >> 4) & 0x0F];

    if (cursor_on && r == cur_row && c == cur_col) {
        uint32_t t = fg;
        fg = bg;
        bg = t;
    }
    fb_draw_char(c * FONT_W, r * FONT_H, (char)cell_ch[r][c], fg, bg);
}

/* Rasterise every dirty range into the shadow buffer, optionally pushing
 * each repainted span to the device. */
static void paint_dirty(bool blit)
{
    for (int r = 0; r < con_h; r++) {
        int lo = row_lo[r], hi = row_hi[r];
        if (lo > hi)
            continue;
        for (int c = lo; c <= hi; c++)
            paint_cell(r, c);
        if (blit)
            fb_flush_rect(lo * FONT_W, r * FONT_H,
                          (hi - lo + 1) * FONT_W, FONT_H);
        row_lo[r] = (int16_t)con_w;
        row_hi[r] = -1;
    }
}

/* Called after every operation that can move the caret or change a cell. */
static void fb_sync(void)
{
    if (!fb_mode)
        return;

    if (drawn_row >= 0 &&
        (!cursor_on || drawn_row != cur_row || drawn_col != cur_col)) {
        mark(drawn_row, drawn_col);      /* repaint without the block */
        drawn_row = -1;
    }
    if (cursor_on && drawn_row < 0)
        mark(cur_row, cur_col);

    if (full_flush) {
        paint_dirty(false);
        fb_flush();
        full_flush = false;
    } else {
        paint_dirty(true);
    }

    if (cursor_on) {
        drawn_row = cur_row;
        drawn_col = cur_col;
    }
}

/* ---------------- shared cell access ---------------- */

static void cell_put(int r, int c, char ch, uint8_t attr)
{
    if (r < 0 || r >= con_h || c < 0 || c >= con_w)
        return;
    if (fb_mode) {
        if (cell_ch[r][c] != (uint8_t)ch || cell_at[r][c] != attr) {
            cell_ch[r][c] = (uint8_t)ch;
            cell_at[r][c] = attr;
            mark(r, c);
        }
    } else {
        vga[r * VGA_WIDTH + c] = (uint8_t)ch | ((uint16_t)attr << 8);
    }
}

static void update_cursor(void)
{
    if (fb_mode)
        return;
    uint16_t pos = cur_row * VGA_WIDTH + cur_col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, pos >> 8);
}

static void cursor_visible(int show)
{
    cursor_on = show ? true : false;
    if (fb_mode)
        return;
    outb(0x3D4, 0x0A);
    uint8_t v = inb(0x3D5);
    if (show)
        v &= (uint8_t)~0x20;
    else
        v |= 0x20;
    outb(0x3D4, 0x0A);
    outb(0x3D5, v);
}

void console_set_color(uint8_t fg, uint8_t bg)
{
    color = fg | (bg << 4);
}

void console_clear(void)
{
    for (int r = 0; r < con_h; r++)
        for (int c = 0; c < con_w; c++)
            cell_put(r, c, ' ', color);
    cur_row = 0;
    cur_col = 0;
    if (fb_mode) {
        /* Repaint the margins outside the character grid too. */
        fb_fill_rect(0, 0, (int)fb_width(), (int)fb_height(),
                     cga_rgb[(color >> 4) & 0x0F]);
        for (int r = 0; r < con_h; r++)
            mark_row(r);
        drawn_row = -1;
        full_flush = true;
    }
    update_cursor();
    fb_sync();
}

void console_init(void)
{
    vga = P2V(0xB8000);
    fb_mode = fb_present();

    if (fb_mode) {
        con_w = (int)(fb_width() / FONT_W);
        con_h = (int)(fb_height() / FONT_H);
        if (con_w > CON_MAX_COLS)
            con_w = CON_MAX_COLS;
        if (con_h > CON_MAX_ROWS)
            con_h = CON_MAX_ROWS;
        if (con_w < 1)
            con_w = 1;
        if (con_h < 1)
            con_h = 1;
    } else {
        con_w = VGA_WIDTH;
        con_h = VGA_HEIGHT;
    }

    for (int r = 0; r < CON_MAX_ROWS; r++) {
        row_lo[r] = (int16_t)con_w;
        row_hi[r] = -1;
    }
    memset(cell_ch, ' ', sizeof(cell_ch));
    memset(cell_at, 0, sizeof(cell_at));
    drawn_row = -1;
    full_flush = false;
    cursor_on = true;

    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    esc_state = ESC_NONE;
    console_clear();
}

static void scroll(void)
{
    if (fb_mode) {
        /* The shadow buffer has to agree with the grid before its pixels
         * are shifted, and the old block cursor must not be dragged
         * along with them. */
        if (drawn_row >= 0) {
            mark(drawn_row, drawn_col);
            drawn_row = -1;
        }
        paint_dirty(false);

        memmove(cell_ch[0], cell_ch[1],
                (size_t)(con_h - 1) * CON_MAX_COLS);
        memmove(cell_at[0], cell_at[1],
                (size_t)(con_h - 1) * CON_MAX_COLS);
        for (int c = 0; c < con_w; c++) {
            cell_ch[con_h - 1][c] = ' ';
            cell_at[con_h - 1][c] = color;
        }

        uint32_t *b = fb_back();
        if (b) {
            size_t stride = fb_width();
            memmove(b, b + stride * FONT_H,
                    stride * (size_t)(con_h - 1) * FONT_H * sizeof(uint32_t));
        }
        mark_row(con_h - 1);
        full_flush = true;
    } else {
        memmove((void *)vga, (void *)(vga + VGA_WIDTH),
                (VGA_HEIGHT - 1) * VGA_WIDTH * 2);
        for (int i = 0; i < VGA_WIDTH; i++)
            vga[(VGA_HEIGHT - 1) * VGA_WIDTH + i] = ' ' | (color << 8);
    }
    cur_row = con_h - 1;
}

/* param n with default d (missing/zero params default per VT100 rules) */
static int esc_param(int n, int d)
{
    if (n >= esc_nparams || esc_params[n] <= 0)
        return d;
    return esc_params[n];
}

static void esc_do_sgr(void)
{
    uint8_t fg = color & 0x0F;
    uint8_t bg = (color >> 4) & 0x0F;
    int n = esc_nparams ? esc_nparams : 1; /* bare CSI m == CSI 0m */

    for (int i = 0; i < n; i++) {
        int p = (i < esc_nparams) ? esc_params[i] : 0;
        if (p == 0) {
            fg = VGA_LIGHT_GREY;
            bg = VGA_BLACK;
        } else if (p == 1) {
            fg |= 0x08;
        } else if (p == 7) {
            uint8_t t = fg;
            fg = bg;
            bg = t;
        } else if (p >= 30 && p <= 37) {
            fg = ansi_to_vga[p - 30] | (fg & 0x08);
        } else if (p >= 40 && p <= 47) {
            bg = ansi_to_vga[p - 40];
        } else if (p >= 90 && p <= 97) {
            fg = ansi_to_vga[p - 90] | 0x08;
        }
        /* unknown SGR params ignored */
    }
    color = fg | (bg << 4);
}

static void esc_final(char c)
{
    int n;

    if (esc_private) {
        /* CSI ? 25 l/h : hide/show cursor */
        if (esc_param(0, 0) == 25) {
            if (c == 'l')
                cursor_visible(0);
            else if (c == 'h')
                cursor_visible(1);
            fb_sync();
        }
        return;
    }

    switch (c) {
    case 'J':
        if (esc_param(0, 0) == 2)
            console_clear();
        break;
    case 'H':
    case 'f':
        cur_row = esc_param(0, 1) - 1;
        cur_col = esc_param(1, 1) - 1;
        if (cur_row < 0) cur_row = 0;
        if (cur_row > con_h - 1) cur_row = con_h - 1;
        if (cur_col < 0) cur_col = 0;
        if (cur_col > con_w - 1) cur_col = con_w - 1;
        break;
    case 'K':
        for (int i = cur_col; i < con_w; i++)
            cell_put(cur_row, i, ' ', color);
        break;
    case 'm':
        esc_do_sgr();
        break;
    case 'A':
        n = esc_param(0, 1);
        cur_row -= n;
        if (cur_row < 0) cur_row = 0;
        break;
    case 'B':
        n = esc_param(0, 1);
        cur_row += n;
        if (cur_row > con_h - 1) cur_row = con_h - 1;
        break;
    case 'C':
        n = esc_param(0, 1);
        cur_col += n;
        if (cur_col > con_w - 1) cur_col = con_w - 1;
        break;
    case 'D':
        n = esc_param(0, 1);
        cur_col -= n;
        if (cur_col < 0) cur_col = 0;
        break;
    default:
        /* unsupported final byte: swallow */
        break;
    }
    update_cursor();
    fb_sync();
}

/* returns 1 if the char was consumed by the escape parser */
static int esc_consume(char c)
{
    switch (esc_state) {
    case ESC_NONE:
        if (c == 0x1B) {
            esc_state = ESC_GOT_ESC;
            return 1;
        }
        return 0;
    case ESC_GOT_ESC:
        if (c == '[') {
            esc_state = ESC_GOT_CSI;
            esc_nparams = 0;
            esc_have_digit = 0;
            esc_private = 0;
            for (int i = 0; i < ESC_MAX_PARAMS; i++)
                esc_params[i] = 0;
        } else {
            esc_state = ESC_NONE; /* non-CSI escape: swallow */
        }
        return 1;
    case ESC_GOT_CSI:
        if (c >= '0' && c <= '9') {
            if (esc_nparams < ESC_MAX_PARAMS) {
                int i = esc_nparams;
                esc_params[i] = esc_params[i] * 10 + (c - '0');
                if (esc_params[i] > 9999)
                    esc_params[i] = 9999;
            }
            esc_have_digit = 1;
        } else if (c == ';') {
            if (esc_nparams < ESC_MAX_PARAMS)
                esc_nparams++;
            else {
                /* too many params: abort sequence */
                esc_state = ESC_NONE;
            }
            esc_have_digit = 0;
        } else if (c == '?') {
            esc_private = 1;
        } else if (c >= 0x40 && c <= 0x7E) {
            if (esc_have_digit && esc_nparams < ESC_MAX_PARAMS)
                esc_nparams++;
            esc_state = ESC_NONE;
            esc_final(c);
        } else {
            /* unexpected byte: swallow and reset */
            esc_state = ESC_NONE;
        }
        return 1;
    }
    return 0;
}

void console_putc(char c)
{
    uint64_t flags = spin_lock_irqsave(&console_lock);
    if (esc_consume(c)) {
        spin_unlock_irqrestore(&console_lock, flags);
        return;
    }

    switch (c) {
    case '\n':
        cur_col = 0;
        cur_row++;
        break;
    case '\r':
        cur_col = 0;
        break;
    case '\b':
        if (cur_col > 0) {
            cur_col--;
            cell_put(cur_row, cur_col, ' ', color);
        }
        break;
    case '\t':
        cur_col = (cur_col + 8) & ~7;
        if (cur_col >= con_w) {
            cur_col = 0;
            cur_row++;
        }
        break;
    default:
        cell_put(cur_row, cur_col, c, color);
        cur_col++;
        if (cur_col >= con_w) {
            cur_col = 0;
            cur_row++;
        }
    }
    if (cur_row >= con_h)
        scroll();
    update_cursor();
    fb_sync();
    spin_unlock_irqrestore(&console_lock, flags);
}

void console_write(const char *s)
{
    while (*s)
        console_putc(*s++);
}
