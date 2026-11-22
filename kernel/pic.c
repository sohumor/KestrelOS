#include "interrupts.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define EOI       0x20
#define PIC_VECTOR_BASE 0x20
#define PIC_LINE_COUNT 16

_Static_assert(PIC_LINE_COUNT <= IRQ_LINE_COUNT, "PIC lines must fit IRQ table");

static int pic_is_spurious(unsigned irq)
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

static enum irq_route pic_claim(unsigned vector, unsigned *line)
{
    if (vector < PIC_VECTOR_BASE || vector >= PIC_VECTOR_BASE + PIC_LINE_COUNT)
        return IRQ_UNCLAIMED;
    *line = vector - PIC_VECTOR_BASE;
    return pic_is_spurious(*line) ? IRQ_SPURIOUS : IRQ_DELIVER;
}

void pic_send_eoi(int irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, EOI);
    outb(PIC1_CMD, EOI);
}

static void pic_complete(unsigned irq)
{
    pic_send_eoi(irq);
}

static const struct irq_controller pic_controller = {
    .claim = pic_claim,
    .complete = pic_complete,
};

void pic_init(void)
{
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, PIC_VECTOR_BASE); io_wait();
    outb(PIC2_DATA, PIC_VECTOR_BASE + 8); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();     /* slave on IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();     /* 8086 mode */
    outb(PIC2_DATA, 0x01); io_wait();

    /* mask everything except the cascade line */
    outb(PIC1_DATA, 0xFB);
    outb(PIC2_DATA, 0xFF);
    irq_controller_bind(&pic_controller);
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
