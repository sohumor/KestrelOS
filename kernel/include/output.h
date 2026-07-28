#pragma once

#include <stdint.h>

/*
 * Serialize the two system-console backends as one stream.  Callers which
 * already produce a complete buffer should use output_write(); streaming
 * formatters may hold a transaction and emit with output_putc_locked().
 */
uint64_t output_begin(void);
void output_end(uint64_t flags);
void output_putc_locked(char c);
void output_write(const char *buf, unsigned long len);
