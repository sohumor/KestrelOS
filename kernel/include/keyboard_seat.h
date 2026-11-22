#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Physical key identities from the HID Keyboard/Keypad usage page. */
enum keyboard_usage {
    KKEY_A = 0x04, KKEY_M = 0x10, KKEY_Z = 0x1d,
    KKEY_1 = 0x1e, KKEY_4 = 0x21, KKEY_0 = 0x27,
    KKEY_ENTER = 0x28, KKEY_ESCAPE, KKEY_BACKSPACE, KKEY_TAB, KKEY_SPACE,
    KKEY_MINUS, KKEY_EQUALS, KKEY_LBRACKET, KKEY_RBRACKET, KKEY_BACKSLASH,
    KKEY_NONUS_HASH, KKEY_SEMICOLON, KKEY_APOSTROPHE, KKEY_GRAVE,
    KKEY_COMMA, KKEY_PERIOD, KKEY_SLASH, KKEY_CAPS,
    KKEY_F4 = 0x3d,
    KKEY_HOME = 0x4a, KKEY_PAGEUP, KKEY_DELETE, KKEY_END, KKEY_PAGEDOWN,
    KKEY_RIGHT, KKEY_LEFT, KKEY_DOWN, KKEY_UP,
    KKEY_KP_MULTIPLY = 0x55, KKEY_KP_ENTER = 0x58,
    KKEY_LCTRL = 0xe0, KKEY_LSHIFT, KKEY_LALT, KKEY_LGUI,
    KKEY_RCTRL, KKEY_RSHIFT, KKEY_RALT, KKEY_RGUI,
};

struct keyboard_source {
    uint8_t modifiers;
    bool caps_down;
};

void keyboard_source_key(struct keyboard_source *source, uint8_t usage,
                         bool down);
void keyboard_source_clear(struct keyboard_source *source);
