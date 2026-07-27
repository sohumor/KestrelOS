/* KestrelOS libc: formatted console I/O.
 *
 * The formatting core is a small sink-based engine shared by the
 * string (vsnprintf) and console (vprintf) paths. Console output is
 * buffered through a 512-byte stack buffer and flushed in chunks to
 * write(1, ...), so printf output length is unbounded.
 *
 * Supported: %s %c %d %i %u %x %X %p %% with '0'/'-' flags, field
 * width and l/ll length modifiers (both mean 64-bit here). Strings also
 * support a byte precision written as .N or .* (a bare . means zero).
 */

#include <stdio.h>
#include <string.h>
#include <kestrel.h>

#define PRINTF_CHUNK 512
#define PRINTF_INT_MAX 2147483647

struct fmt_sink {
    char *buf;              /* chunk buffer (fd mode) or output string  */
    unsigned long cap;      /* chunk size (fd mode) or size arg (string) */
    unsigned long pos;      /* bytes pending in chunk (fd mode only)    */
    unsigned long total;    /* total bytes the format would produce     */
    int fd;                 /* -1 = string mode                         */
    int error;              /* write() failed at some point             */
};

static void sink_flush(struct fmt_sink *s)
{
    unsigned long off = 0;

    while (off < s->pos) {
        long w = write(s->fd, s->buf + off, s->pos - off);
        if (w <= 0) {
            s->error = 1;
            break;
        }
        off += (unsigned long)w;
    }
    s->pos = 0;
}

static void sink_putc(struct fmt_sink *s, char c)
{
    if (s->fd >= 0) {
        s->buf[s->pos++] = c;
        if (s->pos == s->cap)
            sink_flush(s);
    } else {
        /* string mode: keep room for the terminating NUL */
        if (s->cap > 0 && s->total + 1 < s->cap)
            s->buf[s->total] = c;
    }
    s->total++;
}

static void sink_pad(struct fmt_sink *s, char c, int count)
{
    while (count-- > 0)
        sink_putc(s, c);
}

static void sink_str(struct fmt_sink *s, const char *str, int len,
                     int width, int left)
{
    int pad = width - len;
    int i;

    if (!left && pad > 0)
        sink_pad(s, ' ', pad);
    for (i = 0; i < len; i++)
        sink_putc(s, str[i]);
    if (left && pad > 0)
        sink_pad(s, ' ', pad);
}

/* A precision makes %s safe for length-delimited byte strings: do not inspect
 * even one byte beyond the requested limit. */
static int string_len(const char *str, int precision)
{
    int len = 0;

    if (precision < 0)
        return (int)strlen(str);
    while (len < precision && str[len] != '\0')
        len++;
    return len;
}

/* Emit an unsigned value in the given base, honouring sign, width and
 * the '0'/'-' flags. Zero padding goes between the sign and digits. */
static void sink_num(struct fmt_sink *s, unsigned long long v, unsigned base,
                     int negative, int upper, int width, int zero, int left)
{
    static const char lc[] = "0123456789abcdef";
    static const char uc[] = "0123456789ABCDEF";
    const char *digits = upper ? uc : lc;
    char tmp[24];
    int n = 0;
    int len, pad, i;

    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v != 0);

    len = n + (negative ? 1 : 0);
    pad = width - len;

    if (left) {
        if (negative)
            sink_putc(s, '-');
        for (i = n - 1; i >= 0; i--)
            sink_putc(s, tmp[i]);
        if (pad > 0)
            sink_pad(s, ' ', pad);
    } else if (zero) {
        if (negative)
            sink_putc(s, '-');
        if (pad > 0)
            sink_pad(s, '0', pad);
        for (i = n - 1; i >= 0; i--)
            sink_putc(s, tmp[i]);
    } else {
        if (pad > 0)
            sink_pad(s, ' ', pad);
        if (negative)
            sink_putc(s, '-');
        for (i = n - 1; i >= 0; i--)
            sink_putc(s, tmp[i]);
    }
}

static void format_core(struct fmt_sink *s, const char *fmt, va_list ap)
{
    while (*fmt) {
        int zero = 0, left = 0, width = 0, precision = -1, longs = 0;
        char conv;

        if (*fmt != '%') {
            sink_putc(s, *fmt++);
            continue;
        }
        fmt++;

        /* flags */
        for (;;) {
            if (*fmt == '0') {
                zero = 1;
                fmt++;
            } else if (*fmt == '-') {
                left = 1;
                fmt++;
            } else {
                break;
            }
        }

        /* field width */
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Precision. Only %s applies it today, but parse and consume it for
         * every supported conversion so a .* cannot misalign later args. */
        if (*fmt == '.') {
            fmt++;
            precision = 0;                 /* a bare '.' means zero */
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                fmt++;
                if (precision < 0)
                    precision = -1;         /* negative means omitted */
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    int digit = *fmt - '0';

                    if (precision >
                        (PRINTF_INT_MAX - digit) / 10)
                        precision = PRINTF_INT_MAX;
                    else
                        precision = precision * 10 + digit;
                    fmt++;
                }
            }
        }

        /* length: l / ll (both 64-bit on x86-64) */
        while (*fmt == 'l') {
            longs++;
            fmt++;
        }

        conv = *fmt;
        if (conv == '\0')
            break;
        fmt++;

        switch (conv) {
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (str == 0)
                str = "(null)";
            sink_str(s, str, string_len(str, precision), width, left);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            sink_str(s, &c, 1, width, left);
            break;
        }
        case 'd':
        case 'i': {
            long long v = longs ? va_arg(ap, long long)
                                : (long long)va_arg(ap, int);
            int neg = v < 0;
            unsigned long long uv = neg ? (unsigned long long)-(v + 1) + 1
                                        : (unsigned long long)v;
            sink_num(s, uv, 10, neg, 0, width, zero, left);
            break;
        }
        case 'u': {
            unsigned long long v = longs ? va_arg(ap, unsigned long long)
                                         : (unsigned long long)va_arg(ap, unsigned int);
            sink_num(s, v, 10, 0, 0, width, zero, left);
            break;
        }
        case 'x':
        case 'X': {
            unsigned long long v = longs ? va_arg(ap, unsigned long long)
                                         : (unsigned long long)va_arg(ap, unsigned int);
            sink_num(s, v, 16, 0, conv == 'X', width, zero, left);
            break;
        }
        case 'p': {
            unsigned long long v = (unsigned long long)(unsigned long)va_arg(ap, void *);
            sink_putc(s, '0');
            sink_putc(s, 'x');
            sink_num(s, v, 16, 0, 0, 16, 1, 0);
            break;
        }
        case '%':
            sink_putc(s, '%');
            break;
        default:
            /* unknown conversion: echo it literally */
            sink_putc(s, '%');
            sink_putc(s, conv);
            break;
        }
    }
}

int vsnprintf(char *buf, unsigned long size, const char *fmt, va_list ap)
{
    struct fmt_sink s;

    s.buf = buf;
    s.cap = size;
    s.pos = 0;
    s.total = 0;
    s.fd = -1;
    s.error = 0;

    format_core(&s, fmt, ap);

    if (size > 0) {
        unsigned long end = s.total < size - 1 ? s.total : size - 1;
        buf[end] = '\0';
    }
    return (int)s.total;
}

int snprintf(char *buf, unsigned long size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *fmt, va_list ap)
{
    char chunk[PRINTF_CHUNK];
    struct fmt_sink s;

    s.buf = chunk;
    s.cap = PRINTF_CHUNK;
    s.pos = 0;
    s.total = 0;
    s.fd = 1;
    s.error = 0;

    format_core(&s, fmt, ap);
    sink_flush(&s);

    if (s.error)
        return -1;
    return (int)s.total;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *s)
{
    unsigned long len = strlen(s);

    if (len > 0 && write(1, s, len) < 0)
        return EOF;
    if (write(1, "\n", 1) < 0)
        return EOF;
    return (int)(len + 1);
}

int putchar(int c)
{
    unsigned char b = (unsigned char)c;

    if (write(1, &b, 1) < 0)
        return EOF;
    return (int)b;
}

int getchar(void)
{
    unsigned char b;
    long n = read(0, &b, 1);

    if (n != 1)
        return EOF;
    return (int)b;
}
