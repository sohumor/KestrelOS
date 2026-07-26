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

void idt_init(void);
void irq_install_handler(int irq, irq_handler_t handler);

void pic_init(void);
void pic_send_eoi(int irq);
void pic_set_mask(int irq);
void pic_clear_mask(int irq);
int  pic_is_spurious(int irq);

static inline void sti(void) { __asm__ volatile("sti"); }
static inline void cli(void) { __asm__ volatile("cli"); }

static inline uint64_t read_cr2(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}
