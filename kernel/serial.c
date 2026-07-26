#include "serial.h"
#include "input.h"
#include "interrupts.h"
#include "io.h"

#define COM1 0x3F8

void serial_init(void)
{
    outb(COM1 + 1, 0x00);    /* disable interrupts */
    outb(COM1 + 3, 0x80);    /* DLAB on */
    outb(COM1 + 0, 0x01);    /* divisor 1 -> 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);    /* 8N1 */
    outb(COM1 + 2, 0xC7);    /* FIFO on, clear, 14-byte threshold */
    outb(COM1 + 4, 0x0B);    /* DTR | RTS | OUT2 */
}

void serial_putc(char c)
{
    if (c == '\n')
        serial_putc('\r');
    while (!(inb(COM1 + 5) & 0x20))
        ;
    outb(COM1, c);
}

void serial_write(const char *s)
{
    while (*s)
        serial_putc(*s++);
}

bool serial_rx_ready(void)
{
    return inb(COM1 + 5) & 1;
}

char serial_getc(void)
{
    while (!serial_rx_ready())
        ;
    return inb(COM1);
}

/* --- IRQ-driven input, feeding the shared console input buffer ---
 * Translates VT100 escape sequences (arrows, home/end, delete) into the
 * same special key codes the keyboard driver produces. */

static int esc_state;   /* 0: normal, 1: got ESC, 2: got ESC [ */

static void serial_handle_byte(uint8_t b)
{
    switch (esc_state) {
    case 1:
        if (b == '[') {
            esc_state = 2;
            return;
        }
        esc_state = 0;
        input_push(27);
        break;
    case 2:
        esc_state = 0;
        switch (b) {
        case 'A': input_push(KEY_UP); return;
        case 'B': input_push(KEY_DOWN); return;
        case 'C': input_push(KEY_RIGHT); return;
        case 'D': input_push(KEY_LEFT); return;
        case 'H': input_push(KEY_HOME); return;
        case 'F': input_push(KEY_END); return;
        case '3': esc_state = 3; return;
        default: return;                  /* swallow unknown sequence */
        }
    case 3:
        esc_state = 0;
        if (b == '~')
            input_push(KEY_DELETE);
        return;
    }

    if (b == 27) {
        esc_state = 1;
        return;
    }
    if (b == '\r')
        b = '\n';
    else if (b == 0x7F)
        b = '\b';
    input_push(b);
}

static void serial_irq(struct regs *r)
{
    (void)r;
    while (inb(COM1 + 5) & 1)
        serial_handle_byte(inb(COM1));
}

void serial_init_irq(void)
{
    outb(COM1 + 1, 0x01);    /* enable "data available" interrupt */
    irq_install_handler(4, serial_irq);
    pic_clear_mask(4);
}
