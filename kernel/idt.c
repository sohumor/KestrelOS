#include "kernel.h"
#include "interrupts.h"
#include "gdt.h"
#include "console.h"
#include "smp.h"

struct idt_entry {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t  ist;
    uint8_t  flags;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t rsv;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

extern uint64_t isr_table[256];

static struct idt_entry idt[256];
static struct idtr idtr;
static irq_handler_t irq_handlers[16];

/* Optional hook the syscall layer installs for vector 0x80. */
void (*syscall_entry_hook)(struct regs *r);
/* Optional hook the scheduler installs; called after every IRQ. */
void (*irq_preempt_hook)(struct regs *r);

static const char *exception_names[32] = {
    "divide error", "debug", "NMI", "breakpoint",
    "overflow", "bound range", "invalid opcode", "device not available",
    "double fault", "coproc segment", "invalid TSS", "segment not present",
    "stack fault", "general protection fault", "page fault", "reserved",
    "x87 FP", "alignment check", "machine check", "SIMD FP",
    "virtualization", "control protection", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved",
};

static void set_gate(int n, uint64_t handler, uint8_t flags)
{
    idt[n].off_lo = handler & 0xFFFF;
    idt[n].sel = SEL_KCODE;
    idt[n].ist = 0;
    idt[n].flags = flags;
    idt[n].off_mid = (handler >> 16) & 0xFFFF;
    idt[n].off_hi = handler >> 32;
    idt[n].rsv = 0;
}

void idt_init(void)
{
    for (int i = 0; i < 256; i++)
        set_gate(i, isr_table[i], 0x8E);      /* interrupt gate, DPL 0 */
    set_gate(0x80, isr_table[0x80], 0xEE);    /* syscall: DPL 3 */

    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)idt;
    idt_load();
}

void idt_load(void)
{
    __asm__ volatile("lidt %0" : : "m"(idtr));
}

void irq_install_handler(int irq, irq_handler_t handler)
{
    irq_handlers[irq] = handler;
}

static void dump_exception(struct regs *r)
{
    console_set_color(VGA_WHITE, VGA_RED);
    kprintf("\nEXCEPTION %lu (%s), err=%lx\n", r->vector,
            exception_names[r->vector & 31], r->err);
    kprintf("rip=%016lx cs=%02lx rflags=%016lx\n", r->rip, r->cs, r->rflags);
    kprintf("rsp=%016lx ss=%02lx\n", r->rsp, r->ss);
    kprintf("rax=%016lx rbx=%016lx rcx=%016lx\n", r->rax, r->rbx, r->rcx);
    kprintf("rdx=%016lx rsi=%016lx rdi=%016lx\n", r->rdx, r->rsi, r->rdi);
    kprintf("rbp=%016lx r8 =%016lx r9 =%016lx\n", r->rbp, r->r8, r->r9);
    kprintf("r10=%016lx r11=%016lx r12=%016lx\n", r->r10, r->r11, r->r12);
    kprintf("r13=%016lx r14=%016lx r15=%016lx\n", r->r13, r->r14, r->r15);
    if (r->vector == 14)
        kprintf("cr2=%016lx\n", read_cr2());
}

/* Weak-linked here; proc.c overrides behaviour via this hook when user
 * processes exist so a faulting user program doesn't panic the kernel. */
void (*user_fault_hook)(struct regs *r);

void isr_dispatch(struct regs *r)
{
    if (r->vector < 32) {
        if (r->vector == 3) {
            kprintf("[int3 breakpoint at rip=%016lx — ok]\n", r->rip);
            return;
        }
        /* Fault from ring 3? Let the process layer kill the task. */
        if ((r->cs & 3) == 3 && user_fault_hook) {
            user_fault_hook(r);
            return;
        }
        dump_exception(r);
        panic("unhandled exception");
    }

    if (r->vector >= 32 && r->vector < 48) {
        int irq = r->vector - 32;
        if ((irq == 7 || irq == 15) && pic_is_spurious(irq))
            return;
        if (irq_handlers[irq])
            irq_handlers[irq](r);
        pic_send_eoi(irq);
        if (irq_preempt_hook)
            irq_preempt_hook(r);
        return;
    }

    if (r->vector == 0x80) {
        if (syscall_entry_hook)
            syscall_entry_hook(r);
        else
            kprintf("[syscall with no handler installed]\n");
        return;
    }

    if (r->vector == SMP_RESCHEDULE_VECTOR) {
        smp_handle_reschedule();
        if (irq_preempt_hook)
            irq_preempt_hook(r);
        return;
    }

    kprintf("[unexpected interrupt vector %lu]\n", r->vector);
}
