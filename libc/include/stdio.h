#pragma once

/* KestrelOS libc: formatted console I/O (freestanding).
 *
 * printf family supports: %s %c %d %i %u %x %X %p %lu %ld %lx %llu %lld
 * %llx %% with field width and '0'/'-' flags. %s also supports byte
 * precision .N and .* (bare . is zero; a negative * means omitted).
 * Precision on other supported conversions is consumed but otherwise
 * ignored. Output goes to fd 1.
 */

#include <stdarg.h>

#define EOF (-1)

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vprintf(const char *fmt, va_list ap);
int snprintf(char *buf, unsigned long size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, unsigned long size, const char *fmt, va_list ap);

int puts(const char *s);      /* writes s + '\n' to fd 1 */
int putchar(int c);           /* writes one byte to fd 1, returns c */

/* Raw blocking read of one byte from fd 0. May return KEY_* codes
 * (>= 0x80, see kestrel_abi.h) for special keys; ctrl-X arrives as
 * 1..26, ESC as 27. Returns EOF only on read error. */
int getchar(void);
