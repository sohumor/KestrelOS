#include "kernel.h"
#include "console.h"
#include "string.h"
#include "io.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

#define ESC_MAX_PARAMS 4

static volatile uint16_t *vga;
static int cur_row, cur_col;
static uint8_t color;

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

/* ANSI color index (0-7) -> VGA color */
static const uint8_t ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static void update_cursor(void)
{
    uint16_t pos = cur_row * VGA_WIDTH + cur_col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, pos >> 8);
}

static void cursor_visible(int show)
{
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
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        vga[i] = ' ' | (color << 8);
    cur_row = 0;
    cur_col = 0;
    update_cursor();
}

void console_init(void)
{
    vga = P2V(0xB8000);
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    esc_state = ESC_NONE;
    console_clear();
}

static void scroll(void)
{
    memmove((void *)vga, (void *)(vga + VGA_WIDTH),
            (VGA_HEIGHT - 1) * VGA_WIDTH * 2);
    for (int i = 0; i < VGA_WIDTH; i++)
        vga[(VGA_HEIGHT - 1) * VGA_WIDTH + i] = ' ' | (color << 8);
    cur_row = VGA_HEIGHT - 1;
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
        if (cur_row > VGA_HEIGHT - 1) cur_row = VGA_HEIGHT - 1;
        if (cur_col < 0) cur_col = 0;
        if (cur_col > VGA_WIDTH - 1) cur_col = VGA_WIDTH - 1;
        break;
    case 'K':
        for (int i = cur_col; i < VGA_WIDTH; i++)
            vga[cur_row * VGA_WIDTH + i] = ' ' | (color << 8);
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
        if (cur_row > VGA_HEIGHT - 1) cur_row = VGA_HEIGHT - 1;
        break;
    case 'C':
        n = esc_param(0, 1);
        cur_col += n;
        if (cur_col > VGA_WIDTH - 1) cur_col = VGA_WIDTH - 1;
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
    if (esc_consume(c))
        return;

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
            vga[cur_row * VGA_WIDTH + cur_col] = ' ' | (color << 8);
        }
        break;
    case '\t':
        cur_col = (cur_col + 8) & ~7;
        if (cur_col >= VGA_WIDTH) {
            cur_col = 0;
            cur_row++;
        }
        break;
    default:
        vga[cur_row * VGA_WIDTH + cur_col] = (uint8_t)c | (color << 8);
        cur_col++;
        if (cur_col >= VGA_WIDTH) {
            cur_col = 0;
            cur_row++;
        }
    }
    if (cur_row >= VGA_HEIGHT)
        scroll();
    update_cursor();
}

void console_write(const char *s)
{
    while (*s)
        console_putc(*s++);
}
