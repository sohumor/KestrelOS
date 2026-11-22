#pragma once

#include <stdint.h>

/* Register frame pushed by the ISR stubs (see isr.asm). Field order must
 * match the push order exactly: rax is pushed last (lowest address). */
struct regs {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t vector, err;
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*irq_handler_t)(struct regs *r);
typedef void (*irq_listener_t)(struct regs *r, void *context);

void idt_init(void);
void idt_load(void);
void irq_install_handler(int irq, irq_handler_t handler);

/* Subscribers to a shared line must check their own source. Registration
 * returns 0 or -1 (invalid, duplicate, or full); removal returns 0 or -1
 * (not registered). Removal waits for any in-flight call on the line.
 * Call only from task/boot context, without locks taken by callbacks.
 * Callbacks run with local interrupts disabled and cannot sleep. */
int irq_subscribe(unsigned irq, irq_listener_t listener, void *context);
int irq_unsubscribe(unsigned irq, irq_listener_t listener, void *context);

#define IRQ_LINE_COUNT 16

enum irq_route {
    IRQ_UNCLAIMED,
    IRQ_SPURIOUS,
    IRQ_DELIVER,
};

struct irq_controller {
    /* A delivered vector supplies a valid logical line. A spurious
     * vector is consumed here, with any controller-specific EOI. */
    enum irq_route (*claim)(unsigned vector, unsigned *line);
    void (*complete)(unsigned line);
};

/* Install once during boot, before external interrupts are enabled. */
void irq_controller_bind(const struct irq_controller *controller);

void pic_init(void);
void pic_send_eoi(int irq);
void pic_set_mask(int irq);
void pic_clear_mask(int irq);

static inline void sti(void) { __asm__ volatile("sti"); }
static inline void cli(void) { __asm__ volatile("cli"); }

static inline uint64_t read_cr2(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}
