#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "kestrel_abi.h"

/* PS/2 auxiliary-device (mouse) driver. Shares the 8042 with keyboard.c. */

void mouse_init(int screen_w, int screen_h);
bool mouse_present(void);
void mouse_get(struct k_mouse *out);
bool mouse_pop_event(struct k_event *ev);
void mouse_set_bounds(int w, int h);
