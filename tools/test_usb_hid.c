#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "keyboard_seat.h"
#include "usb_hid_core.h"

#define MAKE(usage) ((usage) | 0x100)
#define BREAK(usage) (usage)

static uint16_t keys[32];
static size_t count;

static void emit(uint8_t usage, bool down, void *ctx)
{
    assert(ctx == keys);
    assert(count < sizeof(keys) / sizeof(keys[0]));
    keys[count++] = down ? MAKE(usage) : BREAK(usage);
}

static void check(const uint8_t old[8], const uint8_t next[8],
                  const uint16_t *expected, size_t len)
{
    count = 0;
    assert(usb_hid_keyboard(old, next, emit, keys));
    assert(count == len);
    assert(!len || memcmp(keys, expected, len * sizeof(keys[0])) == 0);
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
    check(empty, a, (uint16_t[]){MAKE(KKEY_A)}, 1);
    check(a, empty, (uint16_t[]){BREAK(KKEY_A)}, 1);
    check(empty, shifted, (uint16_t[]){MAKE(KKEY_LSHIFT), MAKE(KKEY_A)}, 2);
    check(shifted, empty, (uint16_t[]){BREAK(KKEY_A), BREAK(KKEY_LSHIFT)}, 2);
    check(empty, left, (uint16_t[]){MAKE(KKEY_LEFT)}, 1);
    check(left, empty, (uint16_t[]){BREAK(KKEY_LEFT)}, 1);
    check(empty, ctrl, (uint16_t[]){MAKE(KKEY_RCTRL)}, 1);
    check(ctrl, empty, (uint16_t[]){BREAK(KKEY_RCTRL)}, 1);
    check(empty, multi, (uint16_t[]){MAKE(KKEY_A), MAKE(5), MAKE(6)}, 3);
    check(multi, a, (uint16_t[]){BREAK(5), BREAK(6)}, 2);
    check(a, a, NULL, 0);
    check(empty, empty, NULL, 0);
    check(empty, unknown, NULL, 0);
    check(empty, duplicate, (uint16_t[]){MAKE(KKEY_A)}, 1);
    check(duplicate, empty, (uint16_t[]){BREAK(KKEY_A)}, 1);
    for (uint8_t error = 1; error <= 3; error++) {
        uint8_t bad[8] = {0xFF, 0, error};
        count = 0;
        assert(!usb_hid_keyboard(a, bad, emit, keys));
        assert(count == 0);
    }
    assert(!usb_hid_keyboard(NULL, a, emit, keys));
    assert(!usb_hid_keyboard(empty, NULL, emit, keys));
    assert(!usb_hid_keyboard(empty, a, NULL, keys));

    const uint8_t mods[8] = {0xFF};
    check(empty, mods, (uint16_t[]){MAKE(KKEY_LCTRL), MAKE(KKEY_LSHIFT),
          MAKE(KKEY_LALT), MAKE(KKEY_LGUI), MAKE(KKEY_RCTRL),
          MAKE(KKEY_RSHIFT), MAKE(KKEY_RALT), MAKE(KKEY_RGUI)}, 8);
    const uint8_t navigation[8] = {0, 0, 0x4A, 0x4D, 0x4B, 0x4E, 0x4C, 0x58};
    check(empty, navigation, (uint16_t[]){MAKE(KKEY_HOME), MAKE(KKEY_END),
          MAKE(KKEY_PAGEUP), MAKE(KKEY_PAGEDOWN), MAKE(KKEY_DELETE),
          MAKE(KKEY_KP_ENTER)}, 6);
    const uint8_t function[8] = {0, 0, 0x3A, 0x43, 0x44, 0x45, 0x28, 0x39};
    check(empty, function, (uint16_t[]){MAKE(0x3a), MAKE(0x43), MAKE(0x44),
          MAKE(0x45), MAKE(KKEY_ENTER), MAKE(KKEY_CAPS)}, 6);

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
