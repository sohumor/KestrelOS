#include "kernel.h"
#include "console.h"
#include "string.h"
#include "io.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

static volatile uint16_t *vga;
static int cur_row, cur_col;
static uint8_t color;

static void update_cursor(void)
{
    uint16_t pos = cur_row * VGA_WIDTH + cur_col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, pos >> 8);
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

void console_putc(char c)
{
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
