#pragma once

#include <stdint.h>

/* VGA text-mode colors */
enum vga_color {
    VGA_BLACK = 0, VGA_BLUE, VGA_GREEN, VGA_CYAN,
    VGA_RED, VGA_MAGENTA, VGA_BROWN, VGA_LIGHT_GREY,
    VGA_DARK_GREY, VGA_LIGHT_BLUE, VGA_LIGHT_GREEN, VGA_LIGHT_CYAN,
    VGA_LIGHT_RED, VGA_LIGHT_MAGENTA, VGA_YELLOW, VGA_WHITE,
};

void console_init(void);
void console_clear(void);
void console_putc(char c);
void console_write(const char *s);
void console_set_color(uint8_t fg, uint8_t bg);
