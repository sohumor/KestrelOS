#include "kernel.h"
#include "wm.h"

/* Placeholder compositor: the window syscalls exist and fail cleanly so the
 * rest of the system can be built and tested before the real implementation
 * lands. Replaced wholesale by the compositor in kernel/wm.c. */

void wm_init(void)
{
}

bool wm_active(void)
{
    return false;
}

void wm_tick(void)
{
}

void wm_cleanup_task(int pid)
{
    (void)pid;
}

long wm_sys_create(uint64_t ureq, uint64_t uout)
{
    (void)ureq;
    (void)uout;
    return -1;
}

long wm_sys_destroy(uint64_t wid)
{
    (void)wid;
    return -1;
}

long wm_sys_flush(uint64_t wid)
{
    (void)wid;
    return -1;
}

long wm_sys_event(uint64_t wid, uint64_t uevent, uint64_t timeout_ms)
{
    (void)wid;
    (void)uevent;
    (void)timeout_ms;
    return -1;
}

long wm_sys_move(uint64_t wid, int x, int y)
{
    (void)wid;
    (void)x;
    (void)y;
    return -1;
}
