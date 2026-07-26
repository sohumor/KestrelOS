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

/* Compositor limits. A create that asks for more is refused, not clamped
 * (except K_WIN_DESKTOP, which is always granted the screen size capped to
 * these bounds and reports what it got in struct k_wininfo). */
#define WM_WINDOW_LIMIT   USER_WIN_MAX   /* 16 live windows */
/* A window may cover the largest mode the bootloader will select (1440p).
 * USER_WIN_STRIDE is 16 MiB, and 2560*1440*4 is 14.06 MiB, so a full-screen
 * window still fits inside one slot. */
#define WM_WINDOW_MAX_W   2560
#define WM_WINDOW_MAX_H   1440

/* Decoration metrics, in case a panel or a shell wants to reason about the
 * space a decorated window occupies. The frame of a decorated window is
 * (x - 1, y - 1 - 20) sized (w + 2, h + 2 + 20). */
#define WM_DECOR_TITLE_H  20
#define WM_DECOR_BORDER   1

/* Geometry contract: struct k_wincreate's x/y and wm_sys_move's x/y are the
 * top-left corner of the CLIENT AREA (the pixel buffer), not of the frame.
 * The compositor then slides the window so the whole frame — title bar
 * included — stays on screen. */

void wm_init(void);
bool wm_active(void);

/* Compose one frame: repaint whatever changed. Driven by a kernel thread. */
void wm_tick(void);

/* Called when a task exits so its windows do not outlive it. Must run
 * BEFORE the address space is torn down (see kernel/proc.c task_exit): it
 * clears the window's PTEs itself and returns the frames to the PMM, and
 * it detects an already-destroyed address space and leaves the frames to
 * vmm_destroy_user() rather than freeing them twice. */
void wm_cleanup_task(int pid);

/* Syscall entry points (return 0 / -1, or as documented in kestrel_abi.h). */
long wm_sys_create(uint64_t ureq, uint64_t uout);
long wm_sys_destroy(uint64_t wid);
long wm_sys_flush(uint64_t wid);
long wm_sys_event(uint64_t wid, uint64_t uevent, uint64_t timeout_ms);
long wm_sys_move(uint64_t wid, int x, int y);
