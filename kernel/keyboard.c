#include "keyboard.h"
#include "input.h"
#include "interrupts.h"
#include "io.h"

/* US QWERTY, scancode set 1 */
static const char map_normal[0x40] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0,
};

static const char map_shift[0x40] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0,
};

static bool shift, ctrl, caps, ext;

static void kbd_irq(struct regs *r)
{
    (void)r;
    uint8_t sc = inb(0x60);

    if (sc == 0xE0) {
        ext = true;
        return;
    }

    bool release = sc & 0x80;
    sc &= 0x7F;

    if (ext) {
        ext = false;
        /* Right ctrl (E0 1D / E0 9D) carries state, so its break code must
         * be handled before the release filter or `ctrl` latches on. */
        if (sc == 0x1D) {
            ctrl = !release;
            return;
        }
        if (release)
            return;
        switch (sc) {
        case 0x48: input_push(KEY_UP); return;
        case 0x50: input_push(KEY_DOWN); return;
        case 0x4B: input_push(KEY_LEFT); return;
        case 0x4D: input_push(KEY_RIGHT); return;
        case 0x47: input_push(KEY_HOME); return;
        case 0x4F: input_push(KEY_END); return;
        case 0x49: input_push(KEY_PGUP); return;
        case 0x51: input_push(KEY_PGDN); return;
        case 0x53: input_push(KEY_DELETE); return;
        case 0x1C: input_push('\n'); return;      /* keypad enter */
        }
        return;
    }

    switch (sc) {
    case 0x2A: case 0x36: shift = !release; return;
    case 0x1D: ctrl = !release; return;
    case 0x3A: if (!release) caps = !caps; return;
    }

    if (release || sc >= 0x40)
        return;

    char c = shift ? map_shift[sc] : map_normal[sc];
    if (!c)
        return;
    if (caps && !shift && c >= 'a' && c <= 'z')
        c -= 32;
    else if (caps && shift && c >= 'A' && c <= 'Z')
        c += 32;
    if (ctrl && (c >= 'a' && c <= 'z'))
        c -= 96;                                  /* ctrl-a = 1 ... */
    else if (ctrl && (c >= 'A' && c <= 'Z'))
        c -= 64;
    input_push((uint8_t)c);
}

void keyboard_init(void)
{
    irq_install_handler(1, kbd_irq);
    pic_clear_mask(1);
    /* drain any pending byte */
    inb(0x60);
}
