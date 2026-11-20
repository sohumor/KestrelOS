#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*usb_hid_emit_fn)(uint8_t scancode, void *ctx);

/* Returns false for invalid/error reports. Keep the last accepted report as
 * previous; error reports must not replace it. No repeat is generated. */
bool usb_hid_keyboard(const uint8_t previous[8], const uint8_t report[8],
                      usb_hid_emit_fn emit, void *ctx);

struct usb_hid_mouse {
    int dx, dy, dz;
    uint32_t buttons;
};

/* dy is positive up; dz follows PS/2 (negative for wheel up). */
bool usb_hid_mouse(const uint8_t *report, size_t len,
                   struct usb_hid_mouse *out);
