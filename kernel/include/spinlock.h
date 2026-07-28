#pragma once

#include <stdint.h>

/* A small x86 test-and-set lock. Interrupt state is kept separate so callers
 * can use a raw lock in the scheduler's context-switch handoff. */
typedef struct {
    volatile uint32_t value;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_lock(spinlock_t *lock)
{
    for (;;) {
        uint32_t one = 1;
        __asm__ volatile("xchgl %0, %1"
                         : "+r"(one), "+m"(lock->value)
                         :
                         : "memory");
        if (one == 0)
            return;
        while (__atomic_load_n(&lock->value, __ATOMIC_RELAXED))
            __asm__ volatile("pause");
    }
}

static inline int spin_trylock(spinlock_t *lock)
{
    uint32_t one = 1;
    __asm__ volatile("xchgl %0, %1"
                     : "+r"(one), "+m"(lock->value)
                     :
                     : "memory");
    return one == 0;
}

static inline void spin_unlock(spinlock_t *lock)
{
    __atomic_store_n(&lock->value, 0, __ATOMIC_RELEASE);
}

static inline uint64_t spin_lock_irqsave(spinlock_t *lock)
{
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    spin_lock(lock);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags)
{
    spin_unlock(lock);
    if (flags & 0x200)
        __asm__ volatile("sti" : : : "memory");
}
