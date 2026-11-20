#include "kernel.h"
#include "../kernel/input.c"
#include "initcall.h"
#include "timer.h"

#define PENDING (-2)

static int reader_result;

static void console_reader(void *arg)
{
    (void)arg;
    __atomic_store_n(&reader_result, input_getc(), __ATOMIC_RELEASE);
}

static bool check_reader(bool cancel)
{
    int pid;
    __atomic_store_n(&reader_result, PENDING, __ATOMIC_RELAXED);
    struct task *reader =
        kthread_create_with_pid(console_reader, NULL, "console-probe", &pid);
    if (!reader)
        return false;

    bool parked = false;
    uint64_t deadline = timer_ticks() + TIMER_HZ;
    do {
        uint64_t flags = spin_lock_irqsave(&input_lock);
        for (int i = 0; input_ready.waiters != NULL; i++) {
            struct k_psinfo info;
            if (task_psinfo(i, &info) != 0)
                break;
            if (info.pid == pid) {
                parked = info.state == K_STATE_SLEEPING;
                break;
            }
        }
        spin_unlock_irqrestore(&input_lock, flags);
        if (!parked)
            task_sleep_ticks(1);
    } while (!parked && timer_ticks() < deadline);

    if (!parked)
        return false;
    if (cancel)
        task_kill(reader);
    else
        input_push('W');

    deadline = timer_ticks() + TIMER_HZ;
    int result;
    while ((result = __atomic_load_n(&reader_result, __ATOMIC_ACQUIRE)) == PENDING &&
           timer_ticks() < deadline)
        task_sleep_ticks(1);
    return result == (cancel ? -1 : 'W');
}

static int console_input_probe(void)
{
    input_push('Q');
    if (input_getc() != 'Q' || !check_reader(false) || !check_reader(true)) {
        kprintf("FAIL console input events\n");
        return -1;
    }
    kprintf("PASS console input events: queued input, parked reader wake, and cancellation\n");
    return 0;
}

late_initcall(console_input_probe);
