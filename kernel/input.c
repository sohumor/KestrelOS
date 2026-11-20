#include "input.h"
#include "interrupts.h"
#include "proc.h"
#include "spinlock.h"
#include "wm.h"

extern bool wm_keyboard_captured(void) __attribute__((weak));

#define BUFSZ 512
#define INPUT_READY 1U

static volatile uint8_t buf[BUFSZ];
static volatile unsigned head, tail;
static spinlock_t input_lock = SPINLOCK_INIT;
static struct task_event_channel input_ready;

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
    task_event_notify(&input_ready, INPUT_READY);
    spin_unlock_irqrestore(&input_lock, flags);
}

bool input_available(void)
{
    uint64_t flags = spin_lock_irqsave(&input_lock);
    bool available = head != tail;
    spin_unlock_irqrestore(&input_lock, flags);
    return available;
}

static int input_pop_locked(void)
{
    if (head == tail)
        return -1;
    uint8_t c = buf[tail];
    tail = (tail + 1) % BUFSZ;
    return c;
}

int input_trygetc(void)
{
    uint64_t flags = spin_lock_irqsave(&input_lock);
    int c = input_pop_locked();
    spin_unlock_irqrestore(&input_lock, flags);
    return c;
}

int input_trygetc_wm_hotkey(void)
{
    uint64_t flags = spin_lock_irqsave(&input_lock);
    if (head == tail) {
        spin_unlock_irqrestore(&input_lock, flags);
        return -1;
    }
    uint8_t c = buf[tail];
    if (c < KEY_WM_NEXT || c > KEY_MISSION) {
        spin_unlock_irqrestore(&input_lock, flags);
        return -1;
    }
    tail = (tail + 1) % BUFSZ;
    spin_unlock_irqrestore(&input_lock, flags);
    return c;
}

int input_getc(void)
{
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&input_lock);
        bool captured = wm_keyboard_captured && wm_keyboard_captured();
        if (!captured) {
            int c = input_pop_locked();
            if (c >= 0) {
                spin_unlock_irqrestore(&input_lock, flags);
                return c;
            }
            if (sched_active) {
                if (task_event_wait_locked(&input_ready, INPUT_READY,
                                           UINT64_MAX, &input_lock, flags) < 0)
                    return -1;
                continue;
            }
        }
        spin_unlock_irqrestore(&input_lock, flags);
        if (sched_active) {
            if (current && current->kill_pending)
                return -1;
            /* Focus can make queued bytes readable without an enqueue. */
            task_sleep_ticks(1);
        } else {
            __asm__ volatile("sti; hlt");
        }
    }
}
