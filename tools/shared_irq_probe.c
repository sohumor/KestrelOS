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

static unsigned remover_cpu, callback_cpu, hits;
static bool remover_ready, in_callback, removing, callback_leaving, removed;
static bool overlapped, quiesced;

#define HANDSHAKE_CYCLES 100000000
#define OVERLAP_CYCLES 25000000

static void blocking_irq(struct regs *r, void *context)
{
    (void)r;
    (void)context;
    if (__atomic_add_fetch(&hits, 1, __ATOMIC_RELAXED) != 1)
        return;

    callback_cpu = smp_cpu_index();
    __atomic_store_n(&in_callback, true, __ATOMIC_RELEASE);
    uint64_t start = __builtin_ia32_rdtsc();
    while (!__atomic_load_n(&removing, __ATOMIC_ACQUIRE) &&
           __builtin_ia32_rdtsc() - start < HANDSHAKE_CYCLES)
        __builtin_ia32_pause();
    overlapped = __atomic_load_n(&removing, __ATOMIC_ACQUIRE);
    if (overlapped) {
        start = __builtin_ia32_rdtsc();
        while (!__atomic_load_n(&removed, __ATOMIC_ACQUIRE) &&
               __builtin_ia32_rdtsc() - start < OVERLAP_CYCLES)
            __builtin_ia32_pause();
    }
    __atomic_store_n(&callback_leaving, true, __ATOMIC_RELEASE);
}

static void cross_cpu_remove(void *arg)
{
    (void)arg;
    while (!smp_cpu_index())
        yield();
    cli();
    remover_cpu = smp_cpu_index();
    __atomic_store_n(&remover_ready, true, __ATOMIC_RELEASE);
    uint64_t start = __builtin_ia32_rdtsc();
    while (!__atomic_load_n(&in_callback, __ATOMIC_ACQUIRE) &&
           __builtin_ia32_rdtsc() - start < HANDSHAKE_CYCLES)
        __builtin_ia32_pause();
    if (__atomic_load_n(&in_callback, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&removing, true, __ATOMIC_RELEASE);
        quiesced = irq_unsubscribe(0, blocking_irq, NULL) == 0 &&
                  __atomic_load_n(&callback_leaving, __ATOMIC_ACQUIRE);
    }
    __atomic_store_n(&removed, true, __ATOMIC_RELEASE);
    sti();
}

void __real_smp_start_aps(void);
void __wrap_smp_start_aps(void);

void __wrap_smp_start_aps(void)
{
    __real_smp_start_aps();
    if (smp_cpu_count() < 2 ||
        !kthread_create(cross_cpu_remove, NULL, "irq-remove-probe")) {
        kprintf("FAIL cross-CPU IRQ removal: second CPU unavailable\n");
        return;
    }

    for (unsigned i = 0; i < 100 &&
         !__atomic_load_n(&remover_ready, __ATOMIC_ACQUIRE); i++) {
        smp_broadcast_reschedule();
        task_sleep_ticks(1);
    }
    if (!__atomic_load_n(&remover_ready, __ATOMIC_ACQUIRE) ||
        irq_subscribe(0, blocking_irq, NULL) < 0) {
        kprintf("FAIL cross-CPU IRQ removal: could not arm probe\n");
        return;
    }
    for (unsigned i = 0; i < 100 &&
         !__atomic_load_n(&removed, __ATOMIC_ACQUIRE); i++)
        task_sleep_ticks(1);
    task_sleep_ticks(3);

    if (!__atomic_load_n(&removed, __ATOMIC_ACQUIRE) || !quiesced ||
        !overlapped || callback_cpu == remover_cpu ||
        __atomic_load_n(&hits, __ATOMIC_RELAXED) != 1) {
        kprintf("FAIL cross-CPU IRQ removal\n");
        return;
    }
    kprintf("PASS cross-CPU IRQ removal: CPU %u waited for callback on CPU %u\n",
            remover_cpu, callback_cpu);
}
