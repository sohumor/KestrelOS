#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "kestrel_abi.h"

/* Window manager / compositor.
 *
 * Windows are kernel objects. Each owns a pixel buffer (32-bit 0x00RRGGBB,
 * tightly packed) that is mapped into the creating process, so drawing is
 * a plain memory write with no message passing. The compositor stacks the
 * windows onto the framebuffer, draws the decorations and the pointer, and
 * routes input to the focused window.
 *
 * The syscall entry points take raw user pointers and are called only from
 * kernel/syscall.c, which has already enabled interrupts. */

void wm_init(void);
bool wm_active(void);

/* Compose one frame: repaint whatever changed. Driven by a kernel thread. */
void wm_tick(void);

/* Called when a task exits so its windows do not outlive it. */
void wm_cleanup_task(int pid);

/* Syscall entry points (return 0 / -1, or as documented in kestrel_abi.h). */
long wm_sys_create(uint64_t ureq, uint64_t uout);
long wm_sys_destroy(uint64_t wid);
long wm_sys_flush(uint64_t wid);
long wm_sys_event(uint64_t wid, uint64_t uevent, uint64_t timeout_ms);
long wm_sys_move(uint64_t wid, int x, int y);
