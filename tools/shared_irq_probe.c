#include "kernel.h"
#include "interrupts.h"
#include "initcall.h"
#include "proc.h"

static unsigned first_ticks, second_ticks, legacy_ticks;

static void count_irq(struct regs *r, void *context)
{
    (void)r;
    __atomic_add_fetch((unsigned *)context, 1, __ATOMIC_RELAXED);
}

static void count_legacy_irq(struct regs *r)
{
    count_irq(r, &legacy_ticks);
}

static int shared_irq_probe(void)
{
    irq_install_handler(0, count_legacy_irq);
    bool registered = irq_subscribe(0, count_irq, &first_ticks) == 0 &&
                      irq_subscribe(0, count_irq, &second_ticks) == 0 &&
                      irq_subscribe(0, count_irq, &first_ticks) == -1;
    task_sleep_ticks(3);
    int removed = irq_unsubscribe(0, count_irq, &first_ticks);
    unsigned first = __atomic_load_n(&first_ticks, __ATOMIC_RELAXED);
    unsigned second = __atomic_load_n(&second_ticks, __ATOMIC_RELAXED);
    unsigned legacy = __atomic_load_n(&legacy_ticks, __ATOMIC_RELAXED);
    task_sleep_ticks(3);
    bool quiesced = first == __atomic_load_n(&first_ticks, __ATOMIC_RELAXED);
    bool live = second < __atomic_load_n(&second_ticks, __ATOMIC_RELAXED) &&
                legacy < __atomic_load_n(&legacy_ticks, __ATOMIC_RELAXED);
    int removed_second = irq_unsubscribe(0, count_irq, &second_ticks);
    irq_install_handler(0, NULL);

    if (!registered || removed || removed_second || !first || !second || !legacy ||
        !quiesced || !live) {
        kprintf("FAIL shared IRQ subscriptions\n");
        return -1;
    }
    kprintf("PASS shared IRQ subscriptions: hardware PIT delivery, legacy coexistence, and removal\n");
    return 0;
}

late_initcall(shared_irq_probe);
