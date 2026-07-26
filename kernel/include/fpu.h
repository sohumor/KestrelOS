#pragma once

#include <stdint.h>

/* x87/SSE support. The kernel itself is built with -mno-sse and never
 * touches FP registers, so state only has to be swapped when switching
 * between tasks (which may run FP code in ring 3). */

#define FPU_STATE_SIZE 512      /* FXSAVE area, must be 16-byte aligned */

void fpu_init(void);            /* enable SSE on this CPU */
void fpu_state_init(void *area);/* prime a fresh task's save area */

static inline void fpu_save(void *area)
{
    __asm__ volatile("fxsave (%0)" : : "r"(area) : "memory");
}

static inline void fpu_restore(const void *area)
{
    __asm__ volatile("fxrstor (%0)" : : "r"(area) : "memory");
}
