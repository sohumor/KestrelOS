#include "keyboard_seat.h"
#include "input.h"
#include "kestrel_abi.h"
#include "spinlock.h"

static spinlock_t seat_lock = SPINLOCK_INIT;
static unsigned modifier_holders[8];
static bool caps;

static const uint8_t layout[] = {
    '\n', 27, '\b', '\t', ' ', '-', '=', '[', ']', '\\', '\\', ';', '\'', '`',
    ',', '.', '/',
};

static const uint8_t shifted_layout[] = {
    '\n', 27, '\b', '\t', ' ', '_', '+', '{', '}', '|', '|', ':', '"', '~',
    '<', '>', '?',
};

static bool held(uint8_t left, uint8_t right)
{
    return modifier_holders[left - KKEY_LCTRL] ||
           modifier_holders[right - KKEY_LCTRL];
}

static uint8_t seat_byte(uint8_t usage)
{
    bool shift = held(KKEY_LSHIFT, KKEY_RSHIFT);
    bool ctrl = held(KKEY_LCTRL, KKEY_RCTRL);
    bool alt = held(KKEY_LALT, KKEY_RALT);

    if (alt) {
        if (usage == KKEY_TAB)
            return KEY_WM_NEXT;
        if (usage == KKEY_F4)
            return KEY_WM_CLOSE;
        if (usage >= KKEY_1 && usage <= KKEY_4)
            return KEY_WM_WS1 + usage - KKEY_1;
    }
    if (ctrl && usage == KKEY_M)
        return KEY_MISSION;
    if (usage >= KKEY_A && usage <= KKEY_Z) {
        if (ctrl)
            return 1 + usage - KKEY_A;
        return ((caps != shift) ? 'A' : 'a') + usage - KKEY_A;
    }
    if (usage >= KKEY_1 && usage <= KKEY_0)
        return (shift ? "!@#$%^&*()" : "1234567890")[usage - KKEY_1];
    if (usage >= KKEY_ENTER && usage <= KKEY_SLASH)
        return (shift ? shifted_layout : layout)[usage - KKEY_ENTER];

    switch (usage) {
    case KKEY_UP:          return KEY_UP;
    case KKEY_DOWN:        return KEY_DOWN;
    case KKEY_LEFT:        return KEY_LEFT;
    case KKEY_RIGHT:       return KEY_RIGHT;
    case KKEY_HOME:        return KEY_HOME;
    case KKEY_END:         return KEY_END;
    case KKEY_PAGEUP:      return KEY_PGUP;
    case KKEY_PAGEDOWN:    return KEY_PGDN;
    case KKEY_DELETE:      return KEY_DELETE;
    case KKEY_KP_ENTER:    return '\n';
    case KKEY_KP_MULTIPLY: return '*';
    default:               return 0;
    }
}

void keyboard_source_key(struct keyboard_source *source, uint8_t usage,
                         bool down)
{
    uint64_t flags = spin_lock_irqsave(&seat_lock);
    uint8_t c = 0;

    if (usage >= KKEY_LCTRL && usage <= KKEY_RGUI) {
        unsigned i = usage - KKEY_LCTRL;
        uint8_t bit = 1U << i;

        if (down && !(source->modifiers & bit)) {
            source->modifiers |= bit;
            modifier_holders[i]++;
            if (usage == KKEY_LGUI || usage == KKEY_RGUI)
                c = KEY_LAUNCHER;
        } else if (!down && (source->modifiers & bit)) {
            source->modifiers &= ~bit;
            modifier_holders[i]--;
        }
    } else if (usage == KKEY_CAPS) {
        if (down && !source->caps_down)
            caps = !caps;
        source->caps_down = down;
    } else if (down) {
        c = seat_byte(usage);
    }
    if (c)
        input_push(c);
    spin_unlock_irqrestore(&seat_lock, flags);
}

void keyboard_source_clear(struct keyboard_source *source)
{
    uint64_t flags = spin_lock_irqsave(&seat_lock);
    for (unsigned i = 0; i < 8; i++)
        if (source->modifiers & (1U << i))
            modifier_holders[i]--;
    *source = (struct keyboard_source){0};
    spin_unlock_irqrestore(&seat_lock, flags);
}
