#include "../kernel/random.c"
#include "initcall.h"

static int reader_result;

static void entropy_reader(void *arg)
{
    (void)arg;
    uint8_t byte;
    int rc = random_get_bytes(&byte, 1, RANDOM_STRONG);
    __atomic_store_n(&reader_result, rc == 0 ? 1 : -1, __ATOMIC_RELEASE);
}

static int entropy_wait_probe(void)
{
    static const uint8_t test_sample[32] = { 1 };
    uint8_t byte = 0;
    int pid;
    bool parked = false;

    /* Exercise the below-threshold path even on a hardware-seeded guest. */
    uint64_t irq = spin_lock_irqsave(&random_lock);
    credited_bits = 0;
    spin_unlock_irqrestore(&random_lock, irq);

    bool nonblocking = random_get_bytes(&byte, 1, RANDOM_NONBLOCK) == -1;
    if (kthread_create_with_pid(entropy_reader, NULL, "entropy-probe", &pid)) {
        uint64_t deadline = timer_ticks() + 20;
        while (!parked && timer_ticks() < deadline) {
            irq = spin_lock_irqsave(&random_lock);
            struct k_psinfo info;
            for (int i = 0; task_psinfo(i, &info) == 0; i++) {
                if (info.pid == pid) {
                    parked = info.state == K_STATE_SLEEPING &&
                             entropy_ready_event.waiters != NULL;
                    break;
                }
            }
            spin_unlock_irqrestore(&random_lock, irq);
            if (!parked)
                task_sleep_ticks(1);
        }
    }

    entropy_pool_add(ENTROPY_BOOT, test_sample, sizeof(test_sample), 8);
    task_sleep_ticks(1);
    bool stayed_blocked = !random_ready() &&
        __atomic_load_n(&reader_result, __ATOMIC_ACQUIRE) == 0;
    entropy_pool_add(ENTROPY_BOOT, test_sample, sizeof(test_sample),
                     ENTROPY_READY_BITS);
    uint64_t deadline = timer_ticks() + 20;
    while (!__atomic_load_n(&reader_result, __ATOMIC_ACQUIRE) &&
           timer_ticks() < deadline)
        task_sleep_ticks(1);
    int result = __atomic_load_n(&reader_result, __ATOMIC_ACQUIRE);
    if (!nonblocking || !parked || !stayed_blocked || result != 1) {
        kprintf("FAIL entropy readiness: nonblocking=%d parked=%d below=%d result=%d\n",
                nonblocking, parked, stayed_blocked, result);
        return -1;
    }
    kprintf("PASS entropy readiness: reader stayed blocked below threshold and woke at readiness\n");
    return 0;
}

late_initcall(entropy_wait_probe);
