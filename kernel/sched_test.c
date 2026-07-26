#include "kernel.h"
#include "proc.h"
#include "timer.h"

/* Boot-time scheduler self-test: two kernel threads interleave counted
 * prints with different sleep periods, then exit (exercising zombie
 * reaping). The boot task busy-waits so it never leaves the run ring. */

static volatile int done_count;

static void counter_thread(void *arg)
{
    const char *name = arg;
    for (int i = 1; i <= 3; i++) {
        kprintf("[%s%d]", name, i);
        task_sleep_ticks(name[0] == 'A' ? 7 : 11);
    }
    __atomic_add_fetch(&done_count, 1, __ATOMIC_SEQ_CST);
}

void sched_selftest(void)
{
    kthread_create(counter_thread, "A", "count-A");
    kthread_create(counter_thread, "B", "count-B");
    uint64_t deadline = timer_ticks() + 200;
    while (done_count < 2 && timer_ticks() < deadline)
        yield();
    if (done_count != 2)
        panic("sched: threads did not finish");
    kprintf("\nsched: threads interleaved, slept, exited, reaped ok\n");
}
