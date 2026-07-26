#include "kernel.h"
#include "console.h"
#include "serial.h"
#include "string.h"

static void kputc(char c)
{
    console_putc(c);
    serial_putc(c);
}

static void kputs(const char *s)
{
    while (*s)
        kputc(*s++);
}

static void print_num(uint64_t val, int base, bool negative, int width,
                      char pad, bool upper)
{
    char buf[24];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;

    if (val == 0)
        buf[i++] = '0';
    while (val) {
        buf[i++] = digits[val % base];
        val /= base;
    }
    if (negative)
        buf[i++] = '-';
    while (i < width && i < (int)sizeof(buf) - 1)
        buf[i++] = pad;
    while (i--)
        kputc(buf[i]);
}

void kvprintf(const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            kputc(*fmt);
            continue;
        }
        fmt++;

        char pad = ' ';
        int width = 0;
        int longs = 0;

        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        while (*fmt == 'l') {
            longs++;
            fmt++;
        }
        if (*fmt == 'z') {
            longs = 2;
            fmt++;
        }

        switch (*fmt) {
        case 'c':
            kputc((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            kputs(s ? s : "(null)");
            break;
        }
        case 'd':
        case 'i': {
            int64_t v = longs ? va_arg(ap, int64_t) : va_arg(ap, int32_t);
            bool neg = v < 0;
            print_num(neg ? -(uint64_t)v : (uint64_t)v, 10, neg, width, pad, false);
            break;
        }
        case 'u': {
            uint64_t v = longs ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t);
            print_num(v, 10, false, width, pad, false);
            break;
        }
        case 'x': {
            uint64_t v = longs ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t);
            print_num(v, 16, false, width, pad, false);
            break;
        }
        case 'X': {
            uint64_t v = longs ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t);
            print_num(v, 16, false, width, pad, true);
            break;
        }
        case 'p':
            kputs("0x");
            print_num((uint64_t)va_arg(ap, void *), 16, false, 16, '0', false);
            break;
        case '%':
            kputc('%');
            break;
        default:
            kputc('%');
            kputc(*fmt);
        }
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

void panic(const char *fmt, ...)
{
    va_list ap;
    console_set_color(VGA_WHITE, VGA_RED);
    kprintf("\n*** KERNEL PANIC: ");
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf(" ***\n");
    hang();
}
