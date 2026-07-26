#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Special (non-ASCII) key codes pushed into the input buffer. */
#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_LEFT   0x82
#define KEY_RIGHT  0x83
#define KEY_HOME   0x84
#define KEY_END    0x85
#define KEY_PGUP   0x86
#define KEY_PGDN   0x87
#define KEY_DELETE 0x88

void input_push(uint8_t c);
bool input_available(void);
int  input_getc(void);         /* blocking */
int  input_trygetc(void);      /* -1 if empty */
