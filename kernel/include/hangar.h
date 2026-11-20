#pragma once

#include <stdint.h>

struct regs;

/* Hangar: park a running app to disk and resume it later, across
 * reboots. Images live under /hangar (root-only); the kernel reads and
 * writes them on the user's behalf and enforces per-uid access. */
void hangar_init(void);

/* Syscall-exit checkpoint for a task with a pending park request. Parks
 * and exits on success; returns (the task keeps running) on refusal. */
void hangar_checkpoint(struct regs *r);

long sys_hangar(uint64_t op, uint64_t a, uint64_t b, uint64_t c);
