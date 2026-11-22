#include "keyboard.h"
#include "keyboard_seat.h"
#include "interrupts.h"
#include "io.h"
#include "random.h"

/* Translate PS/2 set 1 positions to physical keyboard usages. */
static const uint8_t base[0x59] = {
    [0x01] = 0x29, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24,
    0x25, 0x26, 0x27, 0x2d, 0x2e, 0x2a, 0x2b, 0x14,
    0x1a, 0x08, 0x15, 0x17, 0x1c, 0x18, 0x0c, 0x12,
    0x13, 0x2f, 0x30, 0x28, 0xe0, 0x04, 0x16, 0x07,
    0x09, 0x0a, 0x0b, 0x0d, 0x0e, 0x0f, 0x33, 0x34,
    0x35, 0xe1, 0x31, 0x1d, 0x1b, 0x06, 0x19, 0x05,
    0x11, 0x10, 0x36, 0x37, 0x38, 0xe5, 0x55, 0xe2,
    0x2c, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0x40, 0x41, 0x42, 0x43,
    [0x57] = 0x44, 0x45,
};

static const uint8_t extended[0x5d] = {
    [0x1c] = KKEY_KP_ENTER, [0x1d] = KKEY_RCTRL,
    [0x38] = KKEY_RALT,
    [0x47] = KKEY_HOME, [0x48] = KKEY_UP, [0x49] = KKEY_PAGEUP,
    [0x4b] = KKEY_LEFT, [0x4d] = KKEY_RIGHT, [0x4f] = KKEY_END,
    [0x50] = KKEY_DOWN, [0x51] = KKEY_PAGEDOWN, [0x53] = KKEY_DELETE,
    [0x5b] = KKEY_LGUI, [0x5c] = KKEY_RGUI,
};

static struct keyboard_source ps2_keyboard;
static bool e0;
static unsigned pause_left;

static void kbd_irq(struct regs *r)
{
    (void)r;
    uint8_t sc = inb(0x60);
    entropy_pool_add_interrupt(ENTROPY_KEYBOARD, sc);

    if (pause_left) {
        pause_left--;
        return;
    }
    if (sc == 0xe1) {
        pause_left = 5;
        e0 = false;
        return;
    }
    if (sc == 0xe0) {
        e0 = true;
        return;
    }

    unsigned code = sc & 0x7f;
    const uint8_t *map = e0 ? extended : base;
    unsigned count = e0 ? sizeof(extended) : sizeof(base);
    e0 = false;
    if (code < count && map[code])
        keyboard_source_key(&ps2_keyboard, map[code], !(sc & 0x80));
}

void keyboard_init(void)
{
    irq_install_handler(1, kbd_irq);
    pic_clear_mask(1);
    /* drain any pending byte */
    inb(0x60);
}
