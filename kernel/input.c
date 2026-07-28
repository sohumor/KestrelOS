#include "input.h"
#include "interrupts.h"
#include "proc.h"
#include "spinlock.h"

#define BUFSZ 512

static volatile uint8_t buf[BUFSZ];
static volatile unsigned head, tail;
static spinlock_t input_lock = SPINLOCK_INIT;

void input_push(uint8_t c)
{
    uint64_t flags = spin_lock_irqsave(&input_lock);
    unsigned next = (head + 1) % BUFSZ;
    if (next == tail) {
        spin_unlock_irqrestore(&input_lock, flags);
        return;                   /* full: drop */
    }
    buf[head] = c;
    head = next;
    spin_unlock_irqrestore(&input_lock, flags);
}

bool input_available(void)
{
    uint64_t flags = spin_lock_irqsave(&input_lock);
    bool available = head != tail;
    spin_unlock_irqrestore(&input_lock, flags);
    return available;
}

int input_trygetc(void)
{
    uint64_t flags = spin_lock_irqsave(&input_lock);
    if (head == tail) {
        spin_unlock_irqrestore(&input_lock, flags);
        return -1;
    }
    uint8_t c = buf[tail];
    tail = (tail + 1) % BUFSZ;
    spin_unlock_irqrestore(&input_lock, flags);
    return c;
}

int input_getc(void)
{
    for (;;) {
        int c = input_trygetc();
        if (c >= 0)
            return c;
        if (sched_active)
            yield();
        else
            __asm__ volatile("sti; hlt");
    }
}
