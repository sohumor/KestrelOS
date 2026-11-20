#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "usb_hid_core.h"

static uint8_t bytes[64];
static size_t count;

static void emit(uint8_t sc, void *ctx)
{
    assert(ctx == bytes);
    assert(count < sizeof(bytes));
    bytes[count++] = sc;
}

static void check(const uint8_t old[8], const uint8_t next[8],
                  const uint8_t *expected, size_t len)
{
    count = 0;
    assert(usb_hid_keyboard(old, next, emit, bytes));
    assert(count == len);
    assert(!len || memcmp(bytes, expected, len) == 0);
}

int main(void)
{
    const uint8_t empty[8] = {0};
    const uint8_t a[8] = {0, 0, 4};
    const uint8_t shifted[8] = {2, 0, 4};
    const uint8_t left[8] = {0, 0, 0x50};
    const uint8_t ctrl[8] = {0x10};
    const uint8_t multi[8] = {0, 0, 4, 5, 6};
    const uint8_t duplicate[8] = {0, 0, 4, 4};
    const uint8_t unknown[8] = {0, 0, 0xFF};
    check(empty, a, (uint8_t[]){0x1E}, 1);
    check(a, empty, (uint8_t[]){0x9E}, 1);
    check(empty, shifted, (uint8_t[]){0x2A, 0x1E}, 2);
    check(shifted, empty, (uint8_t[]){0x9E, 0xAA}, 2);
    check(empty, left, (uint8_t[]){0xE0, 0x4B}, 2);
    check(left, empty, (uint8_t[]){0xE0, 0xCB}, 2);
    check(empty, ctrl, (uint8_t[]){0xE0, 0x1D}, 2);
    check(ctrl, empty, (uint8_t[]){0xE0, 0x9D}, 2);
    check(empty, multi, (uint8_t[]){0x1E, 0x30, 0x2E}, 3);
    check(multi, a, (uint8_t[]){0xB0, 0xAE}, 2);
    check(a, a, NULL, 0);
    check(empty, empty, NULL, 0);
    check(empty, unknown, NULL, 0);
    check(empty, duplicate, (uint8_t[]){0x1E}, 1);
    check(duplicate, empty, (uint8_t[]){0x9E}, 1);
    for (uint8_t error = 1; error <= 3; error++) {
        uint8_t bad[8] = {0xFF, 0, error};
        count = 0;
        assert(!usb_hid_keyboard(a, bad, emit, bytes));
        assert(count == 0);
    }
    assert(!usb_hid_keyboard(NULL, a, emit, bytes));
    assert(!usb_hid_keyboard(empty, NULL, emit, bytes));
    assert(!usb_hid_keyboard(empty, a, NULL, bytes));

    const uint8_t mods[8] = {0xFF};
    check(empty, mods, (uint8_t[]){0x1D, 0x2A, 0x38, 0xE0, 0x5B,
          0xE0, 0x1D, 0x36, 0xE0, 0x38, 0xE0, 0x5C}, 12);
    const uint8_t navigation[8] = {0, 0, 0x4A, 0x4D, 0x4B, 0x4E, 0x4C, 0x58};
    check(empty, navigation, (uint8_t[]){0xE0, 0x47, 0xE0, 0x4F,
          0xE0, 0x49, 0xE0, 0x51, 0xE0, 0x53, 0xE0, 0x1C}, 12);
    const uint8_t function[8] = {0, 0, 0x3A, 0x43, 0x44, 0x45, 0x28, 0x39};
    check(empty, function, (uint8_t[]){0x3B, 0x44, 0x57, 0x58, 0x1C, 0x3A}, 6);

    struct usb_hid_mouse m;
    assert(usb_hid_mouse((uint8_t[]){0xFF, 10, 20}, 3, &m));
    assert(m.buttons == 7 && m.dx == 10 && m.dy == -20 && m.dz == 0);
    assert(usb_hid_mouse((uint8_t[]){2, 0x80, 0x80, 1}, 4, &m));
    assert(m.buttons == 2 && m.dx == -128 && m.dy == 128 && m.dz == -1);
    assert(usb_hid_mouse((uint8_t[]){0, 0, 0, 0xFF}, 4, &m));
    assert(m.buttons == 0 && m.dx == 0 && m.dy == 0 && m.dz == 1);
    assert(!usb_hid_mouse(empty, 2, &m));
    assert(!usb_hid_mouse(NULL, 3, &m));
    assert(!usb_hid_mouse(empty, 3, NULL));
    puts("PASS: USB HID boot keyboard and mouse translation checks");
    return 0;
}
