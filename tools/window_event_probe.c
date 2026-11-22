#include "fb.h"
#include "initcall.h"

static unsigned copied_pixels;

static void probe_blit(int x, int y, int w, int h, const uint32_t *src,
                       int stride)
{
    copied_pixels += (unsigned)w * (unsigned)h;
    fb_blit(x, y, w, h, src, stride);
}

#define fb_blit probe_blit
#define wm_sys_create wm_probe_create
#include "../kernel/wm.c"
#undef wm_sys_create
#undef fb_blit

#define SCENE_W 96
#define SCENE_H 80

static uint32_t scene_under[SCENE_W * SCENE_H];
static uint32_t scene_over[SCENE_W * SCENE_H];
static uint32_t scene_before[SCENE_W * SCENE_H];
static uint32_t scene_reference[SCENE_W * SCENE_H];

static void snapshot(uint32_t *out, const uint32_t *in, unsigned out_stride,
                     unsigned in_stride)
{
    for (unsigned y = 0; y < SCENE_H; y++)
        memcpy(out + y * out_stride, in + y * in_stride, SCENE_W * sizeof(*out));
}

static bool check_scene(const struct rect *clip, unsigned expected_copies)
{
    uint32_t *back = fb_back();
    unsigned stride = fb_width();

    snapshot(scene_before, back, SCENE_W, stride);
    copied_pixels = 0;
    paint_stack(clip, -1);
    paint_cursor(clip);
    unsigned reference_copies = copied_pixels;
    snapshot(scene_reference, back, SCENE_W, stride);
    snapshot(back, scene_before, stride, SCENE_W);
    copied_pixels = 0;
    compose_rect(clip);

    bool same = true;
    for (unsigned y = 0; y < SCENE_H; y++)
        if (memcmp(back + y * stride, scene_reference + y * SCENE_W,
                   SCENE_W * sizeof(*back)) != 0)
            same = false;
    snapshot(back, scene_before, stride, SCENE_W);
    return same && copied_pixels < reference_copies &&
           (!expected_copies || copied_pixels == expected_copies);
}

static int check_occlusion(void)
{
    const struct rect covered = {54, 54, 12, 10}, all = {0, 0, SCENE_W, SCENE_H};
    bool passed = false;

    if (fb_back() && fb_width() >= SCENE_W && fb_height() >= SCENE_H) {
        for (unsigned i = 0; i < SCENE_W * SCENE_H; i++) {
            scene_under[i] = 0x0040c000 + i;
            scene_over[i] = 0x00d04000 + i;
        }
        wins[0] = (struct window){
            .used = true, .pix = scene_under, .flags = K_WIN_DESKTOP,
            .w = SCENE_W, .h = SCENE_H,
        };
        for (int i = 1; i <= 6; i++) {
            wins[i] = (struct window){
                .used = true, .pix = scene_over,
                .x = 6 + i * 8, .y = 34 + i * 3, .w = 24, .h = 16,
            };
            zord[i] = i;
        }
        nz = 7;
        ptr_x = 62;
        ptr_y = 59;
        passed = check_scene(&covered, covered.w * covered.h) &&
                 check_scene(&all, 0);
        memset(wins, 0, sizeof(wins));
        memset(zord, 0, sizeof(zord));
        nz = ptr_x = ptr_y = 0;
    }
    kprintf("%s compositor occlusion: same pixels, fewer covered writes\n",
            passed ? "PASS" : "FAIL");
    return passed ? 0 : -1;
}
late_initcall(check_occlusion);

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
