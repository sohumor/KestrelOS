#include "kernel.h"
#include "timer.h"
#include "interrupts.h"
#include "io.h"
#include "random.h"
#include "smp.h"

static volatile uint64_t ticks;

static void timer_irq(struct regs *r)
{
    (void)r;
    ticks++;
    entropy_pool_add_interrupt(ENTROPY_TIMER, (uint32_t)ticks);
    smp_broadcast_reschedule();
}

void timer_init(uint32_t hz)
{
    uint32_t divisor = 1193182 / hz;
    outb(0x43, 0x36);                    /* channel 0, lo/hi, mode 3 */
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
    irq_install_handler(0, timer_irq);
    pic_clear_mask(0);
}

uint64_t timer_ticks(void)
{
    return ticks;
}

void timer_sleep(uint64_t t)
{
    uint64_t target = ticks + t;
    while (ticks < target)
        __asm__ volatile("hlt");
}
