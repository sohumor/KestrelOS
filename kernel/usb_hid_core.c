#include "usb_hid_core.h"
#include "keyboard_seat.h"

static bool contains(const uint8_t report[8], uint8_t key, int end)
{
    for (int i = 2; i < end; i++)
        if (report[i] == key)
            return true;
    return false;
}

bool usb_hid_keyboard(const uint8_t previous[8], const uint8_t report[8],
                      usb_hid_emit_fn emit, void *ctx)
{
    if (!previous || !report || !emit)
        return false;
    for (int i = 2; i < 8; i++)
        if (report[i] >= 1 && report[i] <= 3)
            return false;

    /* Release old keys before changing modifiers, then make new keys. */
    for (int i = 2; i < 8; i++) {
        uint8_t key = previous[i];
        if (key >= KKEY_A && key <= 0x65 && !contains(report, key, 8) &&
            !contains(previous, key, i))
            emit(key, false, ctx);
    }
    for (int i = 0; i < 8; i++)
        if ((previous[0] ^ report[0]) & (1u << i))
            emit(KKEY_LCTRL + i, (report[0] & (1u << i)) != 0, ctx);
    for (int i = 2; i < 8; i++) {
        uint8_t key = report[i];
        if (key >= KKEY_A && key <= 0x65 && !contains(previous, key, 8) &&
            !contains(report, key, i))
            emit(key, true, ctx);
    }
    return true;
}

bool usb_hid_mouse(const uint8_t *report, size_t len,
                   struct usb_hid_mouse *out)
{
    if (!report || !out || len < 3)
        return false;
    out->buttons = report[0] & 7;
    out->dx = (int8_t)report[1];
    out->dy = -(int)(int8_t)report[2];
    out->dz = len >= 4 ? -(int)(int8_t)report[3] : 0;
    return true;
}
