#include "interrupts.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define EOI       0x20

void pic_init(void)
{
    /* remap IRQs 0-15 to vectors 0x20-0x2F */
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();
    outb(PIC2_DATA, 0x28); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();     /* slave on IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();     /* 8086 mode */
    outb(PIC2_DATA, 0x01); io_wait();

    /* mask everything except the cascade line */
    outb(PIC1_DATA, 0xFB);
    outb(PIC2_DATA, 0xFF);
}

void pic_send_eoi(int irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, EOI);
    outb(PIC1_CMD, EOI);
}

void pic_set_mask(int irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8)
        irq -= 8;
    outb(port, inb(port) | (1 << irq));
}

void pic_clear_mask(int irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8)
        irq -= 8;
    outb(port, inb(port) & ~(1 << irq));
}

int pic_is_spurious(int irq)
{
    if (irq == 7) {
        outb(PIC1_CMD, 0x0B);
        return !(inb(PIC1_CMD) & 0x80);
    }
    if (irq == 15) {
        outb(PIC2_CMD, 0x0B);
        if (!(inb(PIC2_CMD) & 0x80)) {
            outb(PIC1_CMD, EOI);   /* master still saw the cascade */
            return 1;
        }
    }
    return 0;
}
