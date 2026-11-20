#include "usb_hid_core.h"

/* Bit 7 marks E0-prefixed set-1 keys. */
static const uint8_t keymap[0x66] = {
    [0x04] = 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23,
    0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10,
    0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
    [0x1E] = 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    0x0A, 0x0B, 0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D,
    0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29, 0x33, 0x34,
    0x35, 0x3A,
    [0x3A] = 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42,
    0x43, 0x44, 0x57, 0x58,
    [0x47] = 0x46,
    [0x49] = 0xD2, 0xC7, 0xC9, 0xD3, 0xCF, 0xD1, 0xCD, 0xCB,
    0xD0, 0xC8, 0x45, 0xB5, 0x37, 0x4A, 0x4E, 0x9C, 0x4F,
    0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49, 0x52,
    0x53, 0x56, 0xDD,
};

static const uint8_t modifiers[8] = {
    0x1D, 0x2A, 0x38, 0xDB, 0x9D, 0x36, 0xB8, 0xDC,
};

static bool contains(const uint8_t report[8], uint8_t key, int end)
{
    for (int i = 2; i < end; i++)
        if (report[i] == key)
            return true;
    return false;
}

static void send_key(uint8_t code, bool release, usb_hid_emit_fn emit, void *ctx)
{
    if (!code)
        return;
    if (code & 0x80)
        emit(0xE0, ctx);
    emit((code & 0x7F) | (release ? 0x80 : 0), ctx);
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
        if (key < sizeof(keymap) && !contains(report, key, 8) &&
            !contains(previous, key, i))
            send_key(keymap[key], true, emit, ctx);
    }
    for (int i = 0; i < 8; i++)
        if ((previous[0] ^ report[0]) & (1u << i))
            send_key(modifiers[i], !(report[0] & (1u << i)), emit, ctx);
    for (int i = 2; i < 8; i++) {
        uint8_t key = report[i];
        if (key < sizeof(keymap) && !contains(previous, key, 8) &&
            !contains(report, key, i))
            send_key(keymap[key], false, emit, ctx);
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
