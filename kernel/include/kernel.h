#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

#define KERNEL_VERSION "0.1.0"

/* All physical memory is direct-mapped here. */
#define PHYS_MAP_BASE 0xFFFF800000000000ULL
#define KERNEL_OFFSET 0xFFFFFFFF80000000ULL

#define P2V(p) ((void *)((uint64_t)(p) + PHYS_MAP_BASE))
#define V2P(v) ((uint64_t)(v) - PHYS_MAP_BASE)

#define PAGE_SIZE 4096ULL

void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list ap);

__attribute__((noreturn))
void panic(const char *fmt, ...);

static inline void hang(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}
