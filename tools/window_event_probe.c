#define wm_sys_create wm_probe_create
#include "../kernel/wm.c"
#undef wm_sys_create

static bool probe_waiting(int wid)
{
    int pid = wins[wid].pid;
    struct k_psinfo info;

    for (int i = 0; task_psinfo(i, &info) == 0; i++) {
        if (info.pid != pid)
            continue;
        return info.state == K_STATE_SLEEPING &&
               window_events[wid].channel.waiters != NULL;
    }
    return false;
}

long wm_sys_create(uint64_t ureq, uint64_t uout)
{
    struct k_wincreate req;
    if (copy_from_user(&req, (const void *)ureq, sizeof(req)) < 0 ||
        strncmp(req.title, "probe-replacement", sizeof(req.title)) != 0)
        return wm_probe_create(ureq, uout);

    uint64_t deadline = timer_ticks() + TIMER_HZ;
    while (timer_ticks() < deadline) {
        wm_lock();
        uint64_t f = spin_lock_irqsave(&window_event_lock);
        int wid;
        for (wid = 0; wid < WM_MAX_WINDOWS; wid++)
            if (wins[wid].used && !wins[wid].dying &&
                strcmp(wins[wid].title, "probe-victim") == 0 &&
                probe_waiting(wid))
                break;
        spin_unlock_irqrestore(&window_event_lock, f);
        if (wid == WM_MAX_WINDOWS) {
            wm_unlock();
            task_sleep_ticks(1);
            continue;
        }

        /* The old owner cannot revalidate the slot until both operations
         * finish, even if destruction makes it runnable on another CPU. */
        win_destroy(&wins[wid]);
        struct k_wininfo info;
        long rc = wm_probe_create(ureq, uout);
        if (rc == 0 && copy_from_user(&info, (const void *)uout, sizeof(info)) == 0 &&
            info.wid == (uint32_t)wid)
            kprintf("WINDOW-SLOT-REUSED\n");
        else
            kprintf("WINDOW-REUSE-FAIL\n");
        wm_unlock();
        return rc;
    }
    kprintf("WINDOW-REUSE-FAIL\n");
    return -1;
}
