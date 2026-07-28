#include "kernel.h"
#include "fpu.h"
#include "string.h"

static void fpu_enable_cpu(void)
{
    uint64_t cr0, cr4;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);        /* EM = 0: no x87 emulation */
    cr0 |= (1ULL << 1);         /* MP = 1: monitor coprocessor */
    cr0 |= (1ULL << 5);         /* NE = 1: native FP exceptions */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    __asm__ volatile("fninit");

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);         /* OSFXSR: FXSAVE/FXRSTOR + SSE */
    cr4 |= (1ULL << 10);        /* OSXMMEXCPT: #XF for SSE exceptions */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));

}

void fpu_init(void)
{
    fpu_enable_cpu();
    kprintf("fpu: x87 + SSE enabled (FXSAVE state per task)\n");
}

void fpu_init_ap(void)
{
    fpu_enable_cpu();
}

void fpu_state_init(void *area)
{
    uint8_t *p = area;
    memset(p, 0, FPU_STATE_SIZE);
    *(uint16_t *)(p + 0) = 0x037F;      /* FCW: mask all x87 exceptions */
    *(uint32_t *)(p + 24) = 0x1F80;     /* MXCSR: mask all SSE exceptions */
    *(uint32_t *)(p + 28) = 0xFFFF;     /* MXCSR_MASK */
}
