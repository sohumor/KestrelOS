#pragma once

#include <stdbool.h>

void serial_init(void);
void serial_init_irq(void);
void serial_putc(char c);
void serial_write(const char *s);
bool serial_rx_ready(void);
char serial_getc(void);
