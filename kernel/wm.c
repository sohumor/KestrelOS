#include "kernel.h"
#include "string.h"
#include "console.h"
#include "fb.h"
#include "font.h"
#include "input.h"
#include "mouse.h"
#include "pmm.h"
#include "vmm.h"
#include "proc.h"
#include "timer.h"
#include "uproc.h"
#include "wm.h"

#include "pulse.h"
#include "hangar_fmt.h"

/* KestrelOS window manager / compositor.
 *
 * A window is a kernel object that owns one contiguous run of physical
 * frames holding 0x00RRGGBB pixels, width*height, no row padding. The run
 * is mapped into the creating process at USER_WIN_BASE + wid*USER_WIN_STRIDE
 * with PTE_U|PTE_W, so an application draws by writing memory and then
 * tells the compositor which window changed with SYS_WIN_FLUSH. Nothing is
 * copied on the syscall path; the only copy is the one the compositor makes
 * when it stacks the window onto the screen.
 *
 * Compositing is damage driven. Everything that can change a pixel --- a
 * flush, a move, a raise, a focus change, a create, a destroy, the pointer
 * moving --- calls damage() with the screen rectangle it invalidated. Once
 * per tick (~50 Hz, from the housekeeper thread) wm_tick() takes the damage
 * list, repaints only those rectangles into fb_back(), and publishes them
 * with fb_flush_rect(). A quiet desktop costs nothing; a moving pointer
 * costs two 12x19 rectangles.
 *
 * Each damage rectangle is painted bottom-to-top, starting at the
 * highest opaque client that covers each piece, or the desktop fill if
 * none is found. The pointer is drawn last; no save-under is needed.
 *
 * Console handover. wm_active() is false until the first window exists and
 * the compositor never writes a pixel before that, so the boot console owns
 * the framebuffer exactly as it does today. Keyboard input is only taken
 * away from the console while the compositor is active AND some window has
 * focus. When the last window goes away the compositor stands down and asks
 * wm_tick() to run console_init(), which repaints the text console.
 *
 * Locking. No IRQ handler enters the window manager. A recursive sleepable
 * mutex serializes its syscall and compositor entry points across CPUs,
 * including the paint pass; contenders sleep rather than spinning while a
 * frame is drawn. The small irq_save sections remain local atomicity guards.
 * `compositing` still defers frame release until the end of the paint pass.
 */

/* ------------------------------------------------------------ geometry */

#define WM_MAX_WINDOWS  USER_WIN_MAX          /* 16 */
#define WM_MAX_W        WM_WINDOW_MAX_W        /* 1440p, from wm.h */
#define WM_MAX_H        WM_WINDOW_MAX_H
#define WM_EVQ          32                    /* per-window events */

#define WM_TITLE_H      30
#define WM_BORDER       1
#define WM_BUTTON_SZ    18
#define WM_BUTTON_GAP   6
#define WM_BUTTON_PAD   7                     /* gap right of the close box */
#define WM_TITLE_PAD    10                    /* gap left of the title text */

/* Frames the compositor refuses to eat into, so a huge window request
 * fails instead of starving the kernel (pmm_alloc*() panics when dry). */
#define WM_PMM_RESERVE  512

/* Self-heal: repaint everything this often, so kernel output that reached
 * the shadow buffer behind the compositor's back cannot linger. */
#define WM_HEAL_TICKS   50                    /* ~1 s at 50 Hz */

#define WM_MAX_DAMAGE   8
#define WM_SPLIT_BUDGET 4
#define WM_SHADOW_PX    5

/* ---------------------------------------------------------------- theme */

#define C_CURSOR_EDGE   0x00000000
#define C_CURSOR_FILL   0x00FFFFFF

static const struct k_wm_theme wm_theme_default = {
    .desktop_bg      = 0x00090D16,
    .title_active    = 0x00172234,
    .title_inactive  = 0x00131B28,
    .border_active   = 0x004B6382,
    .border_inactive = 0x00232E3D,
    .accent          = 0x005BA8FF,
    .text_active     = 0x00EDF4FC,
    .text_inactive   = 0x008B9AAF,
    .btn_bg          = 0x0028394F,
    .close_bg        = 0x00E05A65,
    .close_fg        = 0x00FFFFFF,
    ._pad            = 0,
};

static struct k_wm_theme wm_theme = {
    .desktop_bg      = 0x00090D16,
    .title_active    = 0x00172234,
    .title_inactive  = 0x00131B28,
    .border_active   = 0x004B6382,
    .border_inactive = 0x00232E3D,
    .accent          = 0x005BA8FF,
    .text_active     = 0x00EDF4FC,
    .text_inactive   = 0x008B9AAF,
    .btn_bg          = 0x0028394F,
    .close_bg        = 0x00E05A65,
    .close_fg        = 0x00FFFFFF,
    ._pad            = 0,
};

/* 12x19 arrow. 'X' is the black outline, '.' the white body, ' ' passes
 * the background through, which is what makes it readable over anything. */
#define WM_CUR_W 12
#define WM_CUR_H 19

static const char *const wm_cursor[WM_CUR_H] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.........X ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX   X..X   ",
    "     X..X   ",
    "      X..X  ",
    "      X..X  ",
    "       XX   ",
};

/* ----------------------------------------------------------- structures */

struct rect {
    int x, y, w, h;
};

struct window {
    bool      used;          /* slot allocated */
    bool      dying;         /* unlinked; frames not released yet */
    uint32_t  wid;           /* == slot index, the compositor's id */
    uint32_t  uwid;          /* id the owner uses; fixes the user VA */
    int       pid;           /* owning process */
    uint32_t  uid;           /* desktop controls stay within this uid */
    uint64_t *pml4;          /* owner address space, for the unmap */

    int       x, y;          /* client-area origin on screen */
    int       w, h;          /* client-area size */
    int       orig_x, orig_y;/* pre-maximize client origin */
    int       orig_w, orig_h;/* pre-maximize client size */
    uint32_t  flags;         /* K_WIN_* */
    bool      minimized;     /* linked but omitted from hit-test/paint */
    bool      maximized;     /* toggled by title [+] or K_WIN_CTL_MAXIMIZE */
    bool      pinned;        /* visible on every workspace */
    uint8_t   workspace;     /* 0 .. K_WM_MAX_WORKSPACES - 1 */
    char      title[64];

    uint64_t  phys;          /* first frame of the pixel run */
    int       npages;
    uint32_t *pix;           /* kernel view of the pixels (direct map) */
    uint64_t  uva;           /* user view */

    struct k_event evq[WM_EVQ];
    unsigned  ev_head, ev_tail;
};

static struct window wins[WM_MAX_WINDOWS];

/* Slot lifetimes can end before their sleepers return. Keep wait state
 * outside the window record so zeroing or reusing it cannot unlink them. */
static struct {
    struct task_event_channel channel;
    uint64_t generation;
} window_events[WM_MAX_WINDOWS];
static spinlock_t window_event_lock = SPINLOCK_INIT;

/* Stacking order, bottom first, holding slot indices. K_WIN_DESKTOP
 * windows are kept as a block at the bottom and never leave it. */
static uint8_t zord[WM_MAX_WINDOWS];
static int     nz;

static bool     active;              /* compositor owns the framebuffer */
static bool     restore_console;     /* wm_tick must repaint the console */
static bool     full_repaint;
static volatile bool compositing;    /* a paint pass is in flight */

static int      focus = -1;          /* slot index, or -1 */
static int      current_ws = 0;      /* active workspace 0..3 */
static int      dragging = -1;       /* slot being dragged by the title bar */
static int      drag_dx, drag_dy;    /* pointer offset inside the window */
static int      press_grab = -1;     /* slot that owns the button press */

static int      ptr_x, ptr_y;        /* where the cursor is drawn */
static uint32_t tickno;

static struct rect dmg[WM_MAX_DAMAGE];
static int         ndmg;

/* Whole-manager task-context mutex. Recursive entry is useful because the
 * public paths call helpers such as damage(), focus, and destroy in layers. */
static volatile int wm_held;
static struct task *wm_owner;
static int wm_depth;

static void wm_lock(void)
{
    if (__atomic_load_n(&wm_owner, __ATOMIC_ACQUIRE) == current) {
        wm_depth++;
        return;
    }
    while (__atomic_exchange_n(&wm_held, 1, __ATOMIC_ACQUIRE)) {
        if (sched_active)
            task_sleep_ticks(1);
        else
            __asm__ volatile("pause");
    }
    __atomic_store_n(&wm_owner, current, __ATOMIC_RELEASE);
    wm_depth = 1;
}

static void wm_unlock(void)
{
    if (--wm_depth == 0) {
        __atomic_store_n(&wm_owner, NULL, __ATOMIC_RELEASE);
        __atomic_store_n(&wm_held, 0, __ATOMIC_RELEASE);
    }
}

/* ------------------------------------------------------- rectangle math */

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

static bool rect_isect(const struct rect *a, const struct rect *b,
                       struct rect *out)
{
    int x0 = imax(a->x, b->x);
    int y0 = imax(a->y, b->y);
    int x1 = imin(a->x + a->w, b->x + b->w);
    int y1 = imin(a->y + a->h, b->y + b->h);

    if (x1 <= x0 || y1 <= y0)
        return false;
    out->x = x0;
    out->y = y0;
    out->w = x1 - x0;
    out->h = y1 - y0;
    return true;
}

static void rect_union(const struct rect *a, const struct rect *b,
                       struct rect *out)
{
    int x0 = imin(a->x, b->x);
    int y0 = imin(a->y, b->y);
    int x1 = imax(a->x + a->w, b->x + b->w);
    int y1 = imax(a->y + a->h, b->y + b->h);

    out->x = x0;
    out->y = y0;
    out->w = x1 - x0;
    out->h = y1 - y0;
}

static uint64_t rect_area(const struct rect *r)
{
    if (r->w <= 0 || r->h <= 0)
        return 0;
    return (uint64_t)r->w * (uint64_t)r->h;
}

static bool rect_contains(const struct rect *r, int x, int y)
{
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static void screen_rect(struct rect *r)
{
    r->x = 0;
    r->y = 0;
    r->w = (int)fb_width();
    r->h = (int)fb_height();
}

/* --------------------------------------------------------------- damage */

/* Add one invalidated screen rectangle. Rectangles are merged whenever the
 * union is no more expensive to paint than the two pieces, which keeps the
 * common cases (a pointer step, a window flush) at one or two entries. */
static void damage(int x, int y, int w, int h)
{
    struct rect r, scr, clipped, u;
    uint64_t f;

    if (!active || w <= 0 || h <= 0)
        return;

    r.x = x; r.y = y; r.w = w; r.h = h;
    screen_rect(&scr);
    if (!rect_isect(&r, &scr, &clipped))
        return;

    f = irq_save();

    for (int i = 0; i < ndmg; i++) {
        rect_union(&dmg[i], &clipped, &u);
        if (rect_area(&u) <= rect_area(&dmg[i]) + rect_area(&clipped)) {
            dmg[i] = u;
            irq_restore(f);
            return;
        }
    }

    if (ndmg < WM_MAX_DAMAGE) {
        dmg[ndmg++] = clipped;
        irq_restore(f);
        return;
    }

    /* Full: fold into whichever neighbour grows least. */
    int best = 0;
    uint64_t best_cost = (uint64_t)-1;
    for (int i = 0; i < ndmg; i++) {
        rect_union(&dmg[i], &clipped, &u);
        uint64_t cost = rect_area(&u) - rect_area(&dmg[i]);
        if (cost < best_cost) {
            best_cost = cost;
            best = i;
        }
    }
    rect_union(&dmg[best], &clipped, &u);
    dmg[best] = u;
    irq_restore(f);
}

static void damage_all(void)
{
    uint64_t f = irq_save();
    full_repaint = true;
    irq_restore(f);
}

/* ------------------------------------------------ window geometry rules */

static inline int win_index(const struct window *win)
{
    return (int)(win - wins);
}

static inline bool win_decorated(const struct window *win)
{
    return !(win->flags & (K_WIN_NODECOR | K_WIN_DESKTOP));
}

static inline bool win_visible(const struct window *win)
{
    return win->used && !win->dying && !win->minimized &&
           ((win->flags & (K_WIN_DESKTOP | K_WIN_NODECOR)) ||
            win->pinned || (int)win->workspace == current_ws);
}

/* Outer rectangle including decorations. */
static void win_frame(const struct window *win, struct rect *out)
{
    if (!win_decorated(win)) {
        out->x = win->x;
        out->y = win->y;
        out->w = win->w;
        out->h = win->h;
        return;
    }
    out->x = win->x - WM_BORDER;
    out->y = win->y - WM_BORDER - WM_TITLE_H;
    out->w = win->w + 2 * WM_BORDER;
    out->h = win->h + 2 * WM_BORDER + WM_TITLE_H;
}

/* Outer bounding rectangle including decorations and drop shadow, for damage(). */
static void win_bounds(const struct window *win, struct rect *out)
{
    win_frame(win, out);
    if (win_decorated(win)) {
        out->w += WM_SHADOW_PX;
        out->h += WM_SHADOW_PX;
    }
}

/* Title bar and caption buttons in screen coordinates. Button widths are
 * zero when the window is too narrow to hold usable controls. */
static void win_title_rects(const struct window *win, struct rect *bar,
                            struct rect *maximize, struct rect *minimize,
                            struct rect *close)
{
    bar->x = win->x;
    bar->y = win->y - WM_TITLE_H;
    bar->w = win->w;
    bar->h = WM_TITLE_H;

    close->x = bar->x + bar->w - WM_BUTTON_SZ - WM_BUTTON_PAD;
    close->y = bar->y + (WM_TITLE_H - WM_BUTTON_SZ) / 2;
    close->w = WM_BUTTON_SZ;
    close->h = WM_BUTTON_SZ;

    minimize->x = close->x - WM_BUTTON_GAP - WM_BUTTON_SZ;
    minimize->y = close->y;
    minimize->w = WM_BUTTON_SZ;
    minimize->h = WM_BUTTON_SZ;

    maximize->x = minimize->x - WM_BUTTON_GAP - WM_BUTTON_SZ;
    maximize->y = close->y;
    maximize->w = WM_BUTTON_SZ;
    maximize->h = WM_BUTTON_SZ;

    if (bar->w < 3 * WM_BUTTON_SZ + 2 * WM_BUTTON_GAP +
                 2 * WM_BUTTON_PAD) {
        maximize->w = 0;
        maximize->h = 0;
    }
    if (bar->w < 2 * WM_BUTTON_SZ + WM_BUTTON_GAP +
                 2 * WM_BUTTON_PAD) {
        minimize->w = 0;
        minimize->h = 0;
    }
    if (bar->w < WM_BUTTON_SZ + 2 * WM_BUTTON_PAD) {
        close->w = 0;
        close->h = 0;
    }
}

/* Hangar caption button, left of maximize. Hidden on narrow windows so
 * the title keeps some room. */
static void win_park_rect(const struct window *win, const struct rect *maximize,
                          struct rect *park)
{
    park->x = maximize->x - WM_BUTTON_GAP - WM_BUTTON_SZ;
    park->y = maximize->y;
    park->w = WM_BUTTON_SZ;
    park->h = WM_BUTTON_SZ;
    if (!maximize->w || (win->flags & K_WIN_DESKTOP) ||
        win->w < 4 * WM_BUTTON_SZ + 3 * WM_BUTTON_GAP +
                 2 * WM_BUTTON_PAD + 96) {
        park->w = 0;
        park->h = 0;
    }
}

/* Keep the frame on screen. A window larger than the screen is allowed to
 * hang off the far edge but never off the near one, so its top-left corner
 * (and therefore its title bar) is always reachable. */
static void win_clamp(struct window *win)
{
    struct rect f;
    int sw = (int)fb_width(), sh = (int)fb_height();
    int lo, hi, dx = 0, dy = 0;

    if (win->flags & K_WIN_DESKTOP) {
        win->x = 0;
        win->y = 0;
        return;
    }

    win_frame(win, &f);

    lo = imin(0, sw - f.w);
    hi = imax(0, sw - f.w);
    if (f.x < lo)
        dx = lo - f.x;
    else if (f.x > hi)
        dx = hi - f.x;

    lo = imin(0, sh - f.h);
    hi = imax(0, sh - f.h);
    if (f.y < lo)
        dy = lo - f.y;
    else if (f.y > hi)
        dy = hi - f.y;

    win->x += dx;
    win->y += dy;
}

/* ------------------------------------------------------ stacking order */

/* Callers hold interrupts off. */
static void z_remove(int idx)
{
    int j = 0;
    for (int i = 0; i < nz; i++)
        if (zord[i] != (uint8_t)idx)
            zord[j++] = zord[i];
    nz = j;
}

static int z_desktop_count(void)
{
    int n = 0;
    for (int i = 0; i < nz; i++)
        if (wins[zord[i]].flags & K_WIN_DESKTOP)
            n++;
    return n;
}

/* Insert at the top of the window's own group: desktop windows above the
 * other desktop windows but below everything else, the rest at the front. */
static void z_insert(int idx)
{
    int at = (wins[idx].flags & K_WIN_DESKTOP) ? z_desktop_count() : nz;

    if (!(wins[idx].flags & (K_WIN_DESKTOP | K_WIN_OVERLAY)))
        while (at > 0 && (wins[zord[at - 1]].flags & K_WIN_OVERLAY))
            at--;

    for (int i = nz; i > at; i--)
        zord[i] = zord[i - 1];
    zord[at] = (uint8_t)idx;
    nz++;
}

/* Returns true when the order actually changed. */
static bool z_raise(int idx)
{
    uint64_t f = irq_save();
    bool changed = false;

    if (!(wins[idx].flags & K_WIN_DESKTOP) && nz > 0 &&
        zord[nz - 1] != (uint8_t)idx) {
        z_remove(idx);
        z_insert(idx);
        changed = true;
    }
    irq_restore(f);
    return changed;
}

/* Topmost visible focusable window on the active workspace, or -1. */
static int z_top_focusable(void)
{
    for (int i = nz - 1; i >= 0; i--) {
        const struct window *win = &wins[zord[i]];
        if (!(win->flags & (K_WIN_DESKTOP | K_WIN_OVERLAY)) && win_visible(win))
            return zord[i];
    }
    return -1;
}

/* ---------------------------------------------------------- event queue */

/* Called with wm_lock held, after publishing the queue or lifetime change. */
static void evq_notify(int idx)
{
    uint64_t f = spin_lock_irqsave(&window_event_lock);
    task_event_notify(&window_events[idx].channel, 1);
    spin_unlock_irqrestore(&window_event_lock, f);
}

static void evq_push(struct window *win, uint32_t type, int x, int y,
                     uint32_t key, uint32_t buttons)
{
    uint64_t f = irq_save();

    /* Collapse a run of moves so a fast pointer cannot flush older,
     * more interesting events out of a slow client's queue. */
    if (type == KEV_MOUSE_MOVE && win->ev_head != win->ev_tail) {
        unsigned last = (win->ev_head + WM_EVQ - 1) % WM_EVQ;
        if (win->evq[last].type == KEV_MOUSE_MOVE) {
            win->evq[last].x = x;
            win->evq[last].y = y;
            win->evq[last].buttons = buttons;
            irq_restore(f);
            return;
        }
    }

    unsigned next = (win->ev_head + 1) % WM_EVQ;
    if (next == win->ev_tail)
        win->ev_tail = (win->ev_tail + 1) % WM_EVQ;   /* drop the oldest */

    win->evq[win->ev_head].type = type;
    win->evq[win->ev_head].x = x;
    win->evq[win->ev_head].y = y;
    win->evq[win->ev_head].key = key;
    win->evq[win->ev_head].buttons = buttons;
    win->ev_head = next;

    irq_restore(f);
    evq_notify(win->wid);
}

static bool evq_pop(struct window *win, struct k_event *out)
{
    uint64_t f = irq_save();
    bool got = false;

    if (win->ev_head != win->ev_tail) {
        *out = win->evq[win->ev_tail];
        win->ev_tail = (win->ev_tail + 1) % WM_EVQ;
        got = true;
    }
    irq_restore(f);
    return got;
}

/* --------------------------------------------------------------- focus */

static void set_focus(int idx)
{
    struct rect f;
    int old;

    if (idx >= 0 &&
        ((wins[idx].flags & (K_WIN_DESKTOP | K_WIN_OVERLAY)) || !win_visible(&wins[idx])))
        return;                       /* the desktop layer never takes focus */
    if (focus == idx)
        return;

    old = focus;
    __atomic_store_n(&focus, idx, __ATOMIC_RELEASE);

    if (old >= 0 && wins[old].used && !wins[old].dying) {
        evq_push(&wins[old], KEV_FOCUS, 0, 0, 0, 0);
        win_bounds(&wins[old], &f);
        damage(f.x, f.y, f.w, f.h);
    }
    if (idx >= 0) {
        evq_push(&wins[idx], KEV_FOCUS, 0, 0, 1, 0);
        win_bounds(&wins[idx], &f);
        damage(f.x, f.y, f.w, f.h);
    }
    pulse_emit(K_TOPIC_WIN, K_PULSE_WIN_FOCUS,
                  idx >= 0 ? (uint32_t)idx : (uint32_t)-1,
                  idx >= 0 ? (uint32_t)wins[idx].pid : 0,
                  idx >= 0 ? wins[idx].title : "");
}

/* ------------------------------------------------- clipped drawing ops */

/* Every painter below takes the damage rectangle currently being composed
 * and writes nothing outside it: the shadow buffer must stay identical to
 * the device everywhere that is not about to be flushed. */

static void fill_clip(const struct rect *clip, int x, int y, int w, int h,
                      uint32_t rgb)
{
    struct rect r, o;

    r.x = x; r.y = y; r.w = w; r.h = h;
    if (rect_isect(&r, clip, &o))
        fb_fill_rect(o.x, o.y, o.w, o.h, rgb);
}

static void blit_clip(const struct rect *clip, int x, int y, int w, int h,
                      const uint32_t *src, int stride)
{
    struct rect r, o;

    if (!src || stride <= 0)
        return;
    r.x = x; r.y = y; r.w = w; r.h = h;
    if (!rect_isect(&r, clip, &o))
        return;
    fb_blit(o.x, o.y, o.w, o.h,
            src + (uint64_t)(o.y - y) * (uint64_t)stride + (o.x - x), stride);
}

static void glyph_clip(const struct rect *clip, int x, int y, char c,
                       uint32_t fg, uint32_t bg)
{
    uint32_t *back = fb_back();
    const uint8_t *gl;
    struct rect g, o;
    int stride;

    if (!back)
        return;
    g.x = x; g.y = y; g.w = FONT_W; g.h = FONT_H;
    if (!rect_isect(&g, clip, &o))
        return;

    gl = font_glyph((unsigned char)c);
    stride = (int)fb_width();
    for (int r = 0; r < o.h; r++) {
        int py = o.y + r;
        uint8_t bits = gl[py - y];
        uint32_t *row = back + (uint64_t)py * (uint64_t)stride;
        for (int cx = 0; cx < o.w; cx++) {
            int px = o.x + cx;
            row[px] = (bits & (0x80u >> (px - x))) ? fg : bg;
        }
    }
}

static void text_clip(const struct rect *clip, int x, int y, const char *s,
                      uint32_t fg, uint32_t bg)
{
    if (!s)
        return;
    for (; *s; s++, x += FONT_W) {
        if (x >= clip->x + clip->w)
            break;
        glyph_clip(clip, x, y, *s, fg, bg);
    }
}

/* A compact rounded caption button. The compositor has no alpha channel, so
 * the corners are shaped with short horizontal spans over the title colour. */
static void caption_button(const struct rect *clip, const struct rect *r,
                           uint32_t bg)
{
    fill_clip(clip, r->x + 3, r->y, r->w - 6, r->h, bg);
    fill_clip(clip, r->x + 1, r->y + 2, r->w - 2, r->h - 4, bg);
    fill_clip(clip, r->x, r->y + 4, r->w, r->h - 8, bg);
}

/* ---------------------------------------------------------- compositing */

static inline uint32_t darken_shadow_px(uint32_t p, int d)
{
    switch (d) {
    case 0:  return p - ((p >> 2) & 0x3F3F3FU) - ((p >> 4) & 0x0F0F0FU);
    case 1:  return p - ((p >> 2) & 0x3F3F3FU);
    case 2:  return p - ((p >> 3) & 0x1F1F1FU) - ((p >> 4) & 0x0F0F0FU);
    case 3:  return p - ((p >> 3) & 0x1F1F1FU);
    default: return p - ((p >> 4) & 0x0F0F0FU);
    }
}

static void paint_shadow(const struct window *win, const struct rect *clip)
{
    uint32_t *back = fb_back();
    struct rect frame, bot, right, o;
    int stride;

    if (!back)
        return;
    win_frame(win, &frame);
    stride = (int)fb_width();

    /* Bottom shadow band: (wx-1..wx+w+5, wy+h+1..wy+h+6) */
    bot.x = frame.x;
    bot.y = frame.y + frame.h;
    bot.w = frame.w + WM_SHADOW_PX;
    bot.h = WM_SHADOW_PX;
    if (rect_isect(&bot, clip, &o)) {
        for (int r = 0; r < o.h; r++) {
            int py = o.y + r;
            int dy = py - bot.y;
            uint32_t *row = back + (uint64_t)py * (uint64_t)stride;
            for (int c = 0; c < o.w; c++) {
                int px = o.x + c;
                int dx = px - (frame.x + frame.w);
                int d = dx > dy ? dx : dy;
                row[px] = darken_shadow_px(row[px], d);
            }
        }
    }

    /* Right shadow band: (wx+w+1..wx+w+6, frame.y+2..wy+h+1) */
    if (frame.h > 2) {
        right.x = frame.x + frame.w;
        right.y = frame.y + 2;
        right.w = WM_SHADOW_PX;
        right.h = frame.h - 2;
        if (rect_isect(&right, clip, &o)) {
            for (int r = 0; r < o.h; r++) {
                int py = o.y + r;
                uint32_t *row = back + (uint64_t)py * (uint64_t)stride;
                for (int c = 0; c < o.w; c++) {
                    int px = o.x + c;
                    row[px] = darken_shadow_px(row[px], px - right.x);
                }
            }
        }
    }
}

static void paint_decor(const struct window *win, const struct rect *clip)
{
    struct rect frame, bar, maximize, minimize, close, tclip, o, park;
    bool foc = (focus == win_index(win));
    uint32_t border = foc ? wm_theme.border_active : wm_theme.border_inactive;
    uint32_t barbg  = foc ? wm_theme.title_active  : wm_theme.title_inactive;
    uint32_t edge   = foc ? wm_theme.accent        : wm_theme.border_inactive;
    uint32_t text   = foc ? wm_theme.text_active   : wm_theme.text_inactive;
    uint32_t mbg    = foc ? wm_theme.btn_bg        : wm_theme.border_inactive;
    uint32_t mfg    = text;

    win_frame(win, &frame);

    /* 1px border, drawn as four bars so the client area is never touched */
    fill_clip(clip, frame.x, frame.y, frame.w, WM_BORDER, border);
    fill_clip(clip, frame.x, frame.y + frame.h - WM_BORDER, frame.w,
              WM_BORDER, border);
    fill_clip(clip, frame.x, frame.y, WM_BORDER, frame.h, border);
    fill_clip(clip, frame.x + frame.w - WM_BORDER, frame.y, WM_BORDER,
              frame.h, border);

    win_title_rects(win, &bar, &maximize, &minimize, &close);
    win_park_rect(win, &maximize, &park);
    fill_clip(clip, bar.x, bar.y, bar.w, bar.h, barbg);
    fill_clip(clip, bar.x, bar.y + bar.h - 2, bar.w, 2, edge);

    /* Title text, clipped to the space left of the caption buttons so a
     * long name is cut off rather than scribbled over the buttons. */
    tclip.x = bar.x + WM_TITLE_PAD;
    tclip.y = bar.y;
    tclip.h = bar.h - 1;
    tclip.w = (park.w ? park.x - 8 :
               maximize.w ? maximize.x - 8 :
               minimize.w ? minimize.x - 8 :
               close.w ? close.x - 8 : bar.x + bar.w - 4) - tclip.x;
    if (tclip.w > 0 && rect_isect(&tclip, clip, &o))
        text_clip(&o, tclip.x, bar.y + (WM_TITLE_H - FONT_H) / 2,
                  win->title, text, barbg);

    if (maximize.w) {
        caption_button(clip, &maximize, mbg);
        if (win->maximized) {
            fill_clip(clip, maximize.x + 7, maximize.y + 4, 7, 1, mfg);
            fill_clip(clip, maximize.x + 13, maximize.y + 4, 1, 7, mfg);
            fill_clip(clip, maximize.x + 4, maximize.y + 7, 7, 2, mfg);
            fill_clip(clip, maximize.x + 4, maximize.y + 13, 7, 1, mfg);
            fill_clip(clip, maximize.x + 4, maximize.y + 7, 1, 7, mfg);
            fill_clip(clip, maximize.x + 10, maximize.y + 7, 1, 7, mfg);
        } else {
            fill_clip(clip, maximize.x + 5, maximize.y + 5, 8, 2, mfg);
            fill_clip(clip, maximize.x + 5, maximize.y + 12, 8, 1, mfg);
            fill_clip(clip, maximize.x + 5, maximize.y + 5, 1, 8, mfg);
            fill_clip(clip, maximize.x + 12, maximize.y + 5, 1, 8, mfg);
        }
    }

    if (park.w) {
        /* Hangar: an arrow settling onto a bay floor. */
        int cx = park.x + park.w / 2;

        caption_button(clip, &park, mbg);
        fill_clip(clip, cx - 1, park.y + 4, 2, park.h - 10, mfg);
        for (int i = 1; i <= 3; i++)
            fill_clip(clip, cx - 1 - i, park.y + park.h - 7 - i,
                      2 + 2 * i, 1, mfg);
        fill_clip(clip, park.x + 4, park.y + park.h - 4, park.w - 8, 1, mfg);
        fill_clip(clip, park.x + 4, park.y + park.h - 7, 1, 3, mfg);
        fill_clip(clip, park.x + park.w - 5, park.y + park.h - 7, 1, 3, mfg);
    }

    if (minimize.w) {
        caption_button(clip, &minimize, mbg);
        fill_clip(clip, minimize.x + 5, minimize.y + minimize.h - 6,
                  minimize.w - 10, 2, mfg);
    }

    if (close.w) {
        uint32_t cbg = foc ? wm_theme.close_bg : wm_theme.border_inactive;
        uint32_t cfg = foc ? wm_theme.close_fg : wm_theme.text_inactive;

        caption_button(clip, &close, cbg);
        for (int i = 5; i < close.w - 5; i++) {
            fill_clip(clip, close.x + i, close.y + i, 2, 1, cfg);
            fill_clip(clip, close.x + i,
                      close.y + close.h - 1 - i, 2, 1, cfg);
        }
    }
}

static void paint_window(const struct window *win, const struct rect *clip)
{
    if (win_decorated(win)) {
        paint_shadow(win, clip);
        paint_decor(win, clip);
    }
    blit_clip(clip, win->x, win->y, win->w, win->h, win->pix, win->w);
}

static void paint_cursor(const struct rect *clip)
{
    uint32_t *back = fb_back();
    struct rect cr, o;
    int stride;

    if (!back)
        return;
    cr.x = ptr_x; cr.y = ptr_y; cr.w = WM_CUR_W; cr.h = WM_CUR_H;
    if (!rect_isect(&cr, clip, &o))
        return;

    stride = (int)fb_width();
    for (int r = 0; r < o.h; r++) {
        const char *row = wm_cursor[o.y - ptr_y + r];
        uint32_t *dst = back + (uint64_t)(o.y + r) * (uint64_t)stride;
        for (int c = 0; c < o.w; c++) {
            char ch = row[o.x - ptr_x + c];
            if (ch == 'X')
                dst[o.x + c] = C_CURSOR_EDGE;
            else if (ch == '.')
                dst[o.x + c] = C_CURSOR_FILL;
        }
    }
}

static void paint_stack(const struct rect *clip, int opaque)
{
    if (opaque < 0) {
        fb_fill_rect(clip->x, clip->y, clip->w, clip->h,
                     wm_theme.desktop_bg);
    } else {
        const struct window *win = &wins[zord[opaque]];
        blit_clip(clip, win->x, win->y, win->w, win->h, win->pix, win->w);
    }

    for (int i = opaque + 1; i < nz; i++) {
        struct window *win = &wins[zord[i]];
        if (!win_visible(win) || !win->pix)
            continue;
        paint_window(win, clip);
    }
}

/* Only clients are used as opaque bases. In particular, upper window
 * shadows still need the pixels drawn underneath them. */
static void compose_stack(const struct rect *clip, int top, int *splits)
{
    for (int i = top; i >= 0; i--) {
        const struct window *win = &wins[zord[i]];
        if (!win_visible(win) || !win->pix)
            continue;
        struct rect client = {win->x, win->y, win->w, win->h}, cover;
        if (!rect_isect(clip, &client, &cover))
            continue;
        if (cover.w == clip->w && cover.h == clip->h) {
            paint_stack(clip, i);
            return;
        }
        if (!*splits)
            continue;

        --*splits;
        int right = cover.x + cover.w, bottom = cover.y + cover.h;
        struct rect rest[] = {
            {clip->x, clip->y, clip->w, cover.y - clip->y},
            {clip->x, bottom, clip->w, clip->y + clip->h - bottom},
            {clip->x, cover.y, cover.x - clip->x, cover.h},
            {right, cover.y, clip->x + clip->w - right, cover.h},
        };
        paint_stack(&cover, i);
        for (unsigned j = 0; j < sizeof(rest) / sizeof(rest[0]); j++)
            if (rest[j].w > 0 && rest[j].h > 0)
                compose_stack(&rest[j], i - 1, splits);
        return;
    }
    paint_stack(clip, -1);
}

static void compose_rect(const struct rect *clip)
{
    int splits = WM_SPLIT_BUDGET;
    compose_stack(clip, nz - 1, &splits);
    paint_cursor(clip);
}

/* -------------------------------------------------------- hit testing */

enum {
    HIT_NONE = 0,
    HIT_CLIENT,
    HIT_TITLE,
    HIT_MAXIMIZE,
    HIT_PARK,
    HIT_MINIMIZE,
    HIT_CLOSE,
    HIT_FRAME,
};

/* Topmost window under (mx,my), with the region it was hit in. */
static int hit_test(int mx, int my, int *region)
{
    *region = HIT_NONE;

    for (int i = nz - 1; i >= 0; i--) {
        int idx = zord[i];
        struct window *win = &wins[idx];
        struct rect frame, client, bar, maximize, minimize, close, park;

        if (!win_visible(win))
            continue;

        win_frame(win, &frame);
        if (!rect_contains(&frame, mx, my))
            continue;

        client.x = win->x; client.y = win->y;
        client.w = win->w; client.h = win->h;
        if (rect_contains(&client, mx, my)) {
            *region = HIT_CLIENT;
            return idx;
        }
        if (win_decorated(win)) {
            win_title_rects(win, &bar, &maximize, &minimize, &close);
            win_park_rect(win, &maximize, &park);
            if (park.w && rect_contains(&park, mx, my)) {
                *region = HIT_PARK;
                return idx;
            }
            if (close.w && rect_contains(&close, mx, my)) {
                *region = HIT_CLOSE;
                return idx;
            }
            if (minimize.w && rect_contains(&minimize, mx, my)) {
                *region = HIT_MINIMIZE;
                return idx;
            }
            if (maximize.w && rect_contains(&maximize, mx, my)) {
                *region = HIT_MAXIMIZE;
                return idx;
            }
            if (rect_contains(&bar, mx, my)) {
                *region = HIT_TITLE;
                return idx;
            }
        }
        *region = HIT_FRAME;
        return idx;
    }
    return -1;
}

/* ------------------------------------------------------- input routing */

static int wm_resize_window(int slot, int nw, int nh);

static void switch_workspace(int ws)
{
    if (ws < 0 || ws >= K_WM_MAX_WORKSPACES)
        return;
    if (current_ws == ws)
        return;
    current_ws = ws;
    dragging = -1;
    press_grab = -1;
    set_focus(z_top_focusable());
    damage_all();
    pulse_emit(K_TOPIC_WIN, K_PULSE_WIN_WS, (uint32_t)current_ws, 0, "ws");
}

static void move_window(int idx, int nx, int ny)
{
    struct window *win = &wins[idx];
    struct rect before, after;

    win_bounds(win, &before);
    win->x = nx;
    win->y = ny;
    win_clamp(win);
    win_bounds(win, &after);

    if (before.x == after.x && before.y == after.y)
        return;
    damage(before.x, before.y, before.w, before.h);
    damage(after.x, after.y, after.w, after.h);
}

static void minimize_window(int idx)
{
    struct window *win = &wins[idx];
    struct rect bounds;

    if (win->minimized || (win->flags & K_WIN_DESKTOP))
        return;
    win_bounds(win, &bounds);
    if (focus == idx)
        set_focus(-1);
    win->minimized = true;
    damage(bounds.x, bounds.y, bounds.w, bounds.h);
    if (focus < 0)
        set_focus(z_top_focusable());
}

static void restore_window(int idx, bool activate)
{
    struct window *win = &wins[idx];
    struct rect bounds;

    if (win->flags & K_WIN_DESKTOP)
        return;
    if (activate && win_decorated(win) && !win->pinned &&
        (int)win->workspace != current_ws)
        switch_workspace(win->workspace);
    win->minimized = false;
    z_raise(idx);
    win_bounds(win, &bounds);
    damage(bounds.x, bounds.y, bounds.w, bounds.h);
    if (activate)
        set_focus(idx);
}

static int toggle_maximize(int slot)
{
    struct window *win;
    struct rect bounds;
    int sw = (int)fb_width(), sh = (int)fb_height();

    if (slot < 0 || slot >= WM_MAX_WINDOWS)
        return -1;
    win = &wins[slot];
    if (!win->used || win->dying || (win->flags & K_WIN_DESKTOP))
        return -1;

    if (!win->maximized) {
        int wx = 0;
        int wy = win_decorated(win) ? (WM_DECOR_TITLE_H + 34) : 34;
        int ww = sw;
        int wh = sh - wy - 58;

        if (wh < 48) {
            wy = win_decorated(win) ? (WM_DECOR_TITLE_H + WM_BORDER) : 0;
            wh = sh - wy - (win_decorated(win) ? WM_BORDER : 0);
        }
        if (ww < 64) ww = 64;
        if (ww > WM_MAX_W) ww = WM_MAX_W;
        if (wh < 48) wh = 48;
        if (wh > WM_MAX_H) wh = WM_MAX_H;

        win->orig_x = win->x;
        win->orig_y = win->y;
        win->orig_w = win->w;
        win->orig_h = win->h;

        if (wm_resize_window(slot, ww, wh) < 0)
            return -1;
        win->maximized = true;
        move_window(slot, wx, wy);
        win_bounds(win, &bounds);
        damage(bounds.x, bounds.y, bounds.w, bounds.h);
    } else {
        int rw = imax(64, imin(WM_MAX_W, win->orig_w));
        int rh = imax(48, imin(WM_MAX_H, win->orig_h));

        if (wm_resize_window(slot, rw, rh) < 0)
            return -1;
        win->maximized = false;
        move_window(slot, win->orig_x, win->orig_y);
        win_bounds(win, &bounds);
        damage(bounds.x, bounds.y, bounds.w, bounds.h);
    }
    return 0;
}

/* Alt-Tab walks backward through the current workspace stack and restores a
 * minimized candidate when necessary. Captionless shell popups participate
 * naturally, which makes the launcher dismissible with the same shortcut. */
static void focus_next_window(void)
{
    int start = nz - 1;

    for (int i = 0; i < nz; i++) {
        if (zord[i] == (uint8_t)focus) {
            start = i - 1;
            break;
        }
    }
    for (int pass = 0; pass < nz; pass++) {
        int pos = start - pass;
        int idx;

        while (pos < 0)
            pos += nz;
        idx = zord[pos];
        if (wins[idx].used && !wins[idx].dying &&
            !(wins[idx].flags & (K_WIN_DESKTOP | K_WIN_OVERLAY)) &&
            ((wins[idx].flags & K_WIN_NODECOR) || wins[idx].pinned ||
             (int)wins[idx].workspace == current_ws)) {
            restore_window(idx, true);
            return;
        }
    }
}

static void route_to_desktop(uint32_t key)
{
    for (int i = nz - 1; i >= 0; i--) {
        struct window *win = &wins[zord[i]];

        if (win->used && !win->dying && (win->flags & K_WIN_DESKTOP)) {
            evq_push(win, KEV_KEY, 0, 0, key, 0);
            return;
        }
    }
}

static void on_press(int mx, int my, uint32_t buttons)
{
    struct window *win;
    int region, idx = hit_test(mx, my, &region);

    if (idx < 0)
        return;
    win = &wins[idx];

    if (!(win->flags & K_WIN_DESKTOP)) {
        struct rect f;
        if (z_raise(idx)) {
            win_bounds(win, &f);
            damage(f.x, f.y, f.w, f.h);
        }
        set_focus(idx);
    }

    switch (region) {
    case HIT_CLOSE:
        evq_push(win, KEV_CLOSE, 0, 0, 0, buttons);
        break;
    case HIT_PARK:
        /* The shell owns Hangar policy and feedback: it asks the kernel
         * to park the window's process and reports why if it cannot. */
        if (buttons & K_MOUSE_LEFT) {
            for (int i = nz - 1; i >= 0; i--) {
                struct window *d = &wins[zord[i]];
                if (d->used && !d->dying && (d->flags & K_WIN_DESKTOP)) {
                    evq_push(d, KEV_HANGAR, 0, 0, win->wid, 0);
                    break;
                }
            }
        }
        break;
    case HIT_MAXIMIZE:
        if (buttons & K_MOUSE_LEFT)
            toggle_maximize(idx);
        break;
    case HIT_MINIMIZE:
        if (buttons & K_MOUSE_LEFT)
            minimize_window(idx);
        break;
    case HIT_TITLE:
        if (buttons & K_MOUSE_LEFT) {
            dragging = idx;
            drag_dx = mx - win->x;
            drag_dy = my - win->y;
        }
        break;
    case HIT_CLIENT:
        press_grab = idx;
        evq_push(win, KEV_MOUSE_DOWN, mx - win->x, my - win->y, 0, buttons);
        break;
    default:
        break;
    }
}

static void on_move(int mx, int my, uint32_t buttons)
{
    int region, idx;

    if (dragging >= 0) {
        if (wins[dragging].used && !wins[dragging].dying)
            move_window(dragging, mx - drag_dx, my - drag_dy);
        else
            dragging = -1;
        return;
    }
    if (press_grab >= 0) {
        struct window *win = &wins[press_grab];
        if (win->used && !win->dying)
            evq_push(win, KEV_MOUSE_MOVE, mx - win->x, my - win->y, 0,
                     buttons);
        else
            press_grab = -1;
        return;
    }

    idx = hit_test(mx, my, &region);
    if (idx >= 0 && region == HIT_CLIENT)
        evq_push(&wins[idx], KEV_MOUSE_MOVE, mx - wins[idx].x,
                 my - wins[idx].y, 0, buttons);
}

static void on_release(int mx, int my, uint32_t buttons)
{
    bool left_up = !(buttons & K_MOUSE_LEFT);

    if (dragging >= 0 && left_up)
        dragging = -1;

    if (press_grab >= 0) {
        struct window *win = &wins[press_grab];
        if (win->used && !win->dying)
            evq_push(win, KEV_MOUSE_UP, mx - win->x, my - win->y, 0, buttons);
        if (left_up)
            press_grab = -1;
    }
}

static void on_wheel(int mx, int my, uint32_t delta, uint32_t buttons)
{
    int region, idx = hit_test(mx, my, &region);

    if (idx < 0 || region != HIT_CLIENT)
        return;
    struct window *win = &wins[idx];
    if (!win_visible(win))
        return;
    evq_push(win, KEV_MOUSE_WHEEL, mx - win->x, my - win->y, delta, buttons);
}

static void pump_mouse(void)
{
    struct k_event ev;
    struct k_mouse m;

    while (mouse_pop_event(&ev)) {
        switch (ev.type) {
        case KEV_MOUSE_MOVE:
            on_move(ev.x, ev.y, ev.buttons);
            break;
        case KEV_MOUSE_DOWN:
            on_press(ev.x, ev.y, ev.buttons);
            break;
        case KEV_MOUSE_UP:
            on_release(ev.x, ev.y, ev.buttons);
            break;
        case KEV_MOUSE_WHEEL:
            on_wheel(ev.x, ev.y, ev.key, ev.buttons);
            break;
        default:
            break;
        }
    }

    memset(&m, 0, sizeof(m));
    mouse_get(&m);
    if (m.present && (m.x != ptr_x || m.y != ptr_y)) {
        damage(ptr_x, ptr_y, WM_CUR_W, WM_CUR_H);
        ptr_x = m.x;
        ptr_y = m.y;
        damage(ptr_x, ptr_y, WM_CUR_W, WM_CUR_H);
    }
}

/* Keystrokes are only taken from the shared console buffer while the
 * compositor is up AND a window has focus. Anything else and the text
 * console and the shell keep every byte, exactly as without a compositor. */
static void pump_keyboard(void)
{
    for (int i = 0; i < 64; i++) {
        bool focused = (focus >= 0 && wins[focus].used && !wins[focus].dying);
        int c = focused ? input_trygetc() : input_trygetc_wm_hotkey();
        if (c < 0)
            return;
        if (c == KEY_WM_NEXT) {
            focus_next_window();
            continue;
        }
        if (c == KEY_WM_CLOSE) {
            if (focus >= 0 && wins[focus].used && !wins[focus].dying)
                evq_push(&wins[focus], KEV_CLOSE, 0, 0, 0, 0);
            continue;
        }
        if (c >= KEY_WM_WS1 && c <= KEY_WM_WS4) {
            switch_workspace(c - KEY_WM_WS1);
            continue;
        }
        if (c == KEY_LAUNCHER || c == KEY_MISSION) {
            route_to_desktop((uint32_t)c);
            continue;
        }
        if (focus < 0 || !wins[focus].used || wins[focus].dying)
            continue;
        evq_push(&wins[focus], KEV_KEY, 0, 0, (uint32_t)c, 0);
    }
}

/* ------------------------------------------------ allocation / teardown */

/* Clear the PTEs for a mapped run without freeing the page tables; the
 * tables themselves belong to the address space and go away with it. */
static void unmap_run(uint64_t *pml4, uint64_t va, int npages)
{
    for (int i = 0; i < npages; i++) {
        uint64_t a = va + (uint64_t)i * PAGE_SIZE;
        uint64_t *t = pml4;
        int idx[4] = { (int)((a >> 39) & 511), (int)((a >> 30) & 511),
                       (int)((a >> 21) & 511), (int)((a >> 12) & 511) };
        bool ok = true;

        for (int lvl = 0; lvl < 3; lvl++) {
            uint64_t e = t[idx[lvl]];
            if (!(e & PTE_P) || (e & PTE_PS)) {
                ok = false;
                break;
            }
            t = P2V(PTE_ADDR(e));
        }
        if (!ok)
            continue;
        t[idx[3]] = 0;
        invlpg(a);
    }
}

/* True while the address space the window was mapped into is still the one
 * its owner is running on. When the owner has already been torn down its
 * page tables (and with them the pixel frames) are gone, so this is also
 * the test for "are these frames still ours to free". */
static bool owner_space_alive(const struct window *win)
{
    return task_address_space_matches(win->pid, win->pml4);
}

/* Slow half of a destroy: drop the user mapping, return the frames, free
 * the slot. Never called with the compositor mid-pass. */
static void win_release(struct window *win)
{
    uint64_t f;

    if (win->phys && win->npages > 0 && owner_space_alive(win)) {
        unmap_run(win->pml4, win->uva, win->npages);
        pmm_free_contig(win->phys, win->npages);
    }
    /* Otherwise vmm_destroy_user() already reclaimed the frames along with
     * the rest of the user half; freeing them again would corrupt the
     * allocator. */

    f = irq_save();
    memset(win, 0, sizeof(*win));
    irq_restore(f);
}

/* Caller holds wm_lock(). Dynamically reallocate and remap the pixel buffer. */
static int wm_resize_window(int slot, int nw, int nh)
{
    struct window *win;
    struct rect old_bounds, new_bounds;
    uint64_t need, new_phys, old_phys, f;
    uint32_t *new_pix;
    int new_npages, old_npages, copy_w, copy_h;

    if (slot < 0 || slot >= WM_MAX_WINDOWS)
        return -1;
    win = &wins[slot];
    if (!win->used || win->dying || !win->pix ||
        (win->flags & K_WIN_DESKTOP))
        return -1;
    if (nw < 64 || nw > WM_MAX_W || nh < 48 || nh > WM_MAX_H)
        return -1;
    if (nw == win->w && nh == win->h)
        return 0;

    new_npages = (int)(((uint64_t)nw * (uint64_t)nh * 4 + PAGE_SIZE - 1) /
                       PAGE_SIZE);
    if ((uint64_t)new_npages * PAGE_SIZE > USER_WIN_STRIDE)
        return -1;
    if (!owner_space_alive(win))
        return -1;

    need = (uint64_t)new_npages + (uint64_t)new_npages / 512 + 8 +
           WM_PMM_RESERVE;
    if (pmm_free_pages() < need)
        return -1;

    new_phys = pmm_alloc_contig(new_npages);
    new_pix = (uint32_t *)P2V(new_phys);

    copy_w = imin(win->w, nw);
    copy_h = imin(win->h, nh);
    for (int row = 0; row < copy_h; row++) {
        memcpy(new_pix + (uint64_t)row * (uint64_t)nw,
               win->pix + (uint64_t)row * (uint64_t)win->w,
               (size_t)copy_w * sizeof(uint32_t));
    }

    win_bounds(win, &old_bounds);
    old_phys = win->phys;
    old_npages = win->npages;

    unmap_run(win->pml4, win->uva, old_npages);
    for (int i = 0; i < new_npages; i++)
        vmm_map_page(win->pml4, win->uva + (uint64_t)i * PAGE_SIZE,
                     new_phys + (uint64_t)i * PAGE_SIZE, PTE_U | PTE_W);

    f = irq_save();
    win->w = nw;
    win->h = nh;
    win->phys = new_phys;
    win->npages = new_npages;
    win->pix = new_pix;
    bool can_free = !compositing;
    irq_restore(f);

    if (can_free && old_phys && old_npages > 0)
        pmm_free_contig(old_phys, old_npages);

    win_clamp(win);
    win_bounds(win, &new_bounds);
    damage(old_bounds.x, old_bounds.y, old_bounds.w, old_bounds.h);
    damage(new_bounds.x, new_bounds.y, new_bounds.w, new_bounds.h);
    evq_push(win, KEV_RESIZE, nw, nh, 0, 0);
    return 0;
}

/* Fast half: take the window out of every list that the compositor and the
 * input router walk, so nothing can reach it again. Interrupts off. */
static void win_unlink(struct window *win)
{
    int idx = win_index(win);

    z_remove(idx);
    win->dying = true;
    window_events[idx].generation++;
    evq_notify(idx);
    if (focus == idx)
        __atomic_store_n(&focus, -1, __ATOMIC_RELEASE);
    if (dragging == idx)
        dragging = -1;
    if (press_grab == idx)
        press_grab = -1;
}

static int live_window_count(void)
{
    int n = 0;

    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (wins[i].used && !wins[i].dying)
            n++;
    return n;
}

static void win_destroy(struct window *win)
{
    struct rect bounds;
    char title[24];
    uint32_t wid = win->wid;
    uint32_t pid = (uint32_t)win->pid;
    bool defer;
    uint64_t f;

    strncpy(title, win->title, sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
    win_bounds(win, &bounds);

    f = irq_save();
    win_unlink(win);
    defer = compositing;
    irq_restore(f);

    damage(bounds.x, bounds.y, bounds.w, bounds.h);
    pulse_emit(K_TOPIC_WIN, K_PULSE_WIN_CLOSE, wid, pid, title);

    /* Focus follows the stack: hand it to whatever is now on top. */
    if (focus < 0)
        set_focus(z_top_focusable());

    if (!defer)
        win_release(win);

    if (live_window_count() == 0)
        restore_console = true;
}

static void reap_dying(void)
{
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (wins[i].used && wins[i].dying)
            win_release(&wins[i]);
}

/* -------------------------------------------------------- public entry */

void wm_init(void)
{
    memset(wins, 0, sizeof(wins));
    memset(dmg, 0, sizeof(dmg));
    nz = 0;
    ndmg = 0;
    __atomic_store_n(&active, false, __ATOMIC_RELEASE);
    restore_console = false;
    full_repaint = false;
    compositing = false;
    __atomic_store_n(&focus, -1, __ATOMIC_RELEASE);
    current_ws = 0;
    wm_theme = wm_theme_default;
    dragging = -1;
    press_grab = -1;
    ptr_x = ptr_y = 0;
    tickno = 0;

    kprintf("wm: compositor ready (%d windows max, %dx%d each)\n",
            WM_MAX_WINDOWS, WM_MAX_W, WM_MAX_H);
}

bool wm_active(void)
{
    return __atomic_load_n(&active, __ATOMIC_ACQUIRE);
}

bool wm_keyboard_captured(void)
{
    return __atomic_load_n(&active, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&focus, __ATOMIC_ACQUIRE) >= 0;
}

void wm_tick(void)
{
    struct rect todo[WM_MAX_DAMAGE];
    int ntodo;
    uint64_t f;

    wm_lock();

    /* Standing down: the text console gets the screen back. Done here, on
     * a kernel thread, rather than inside the syscall that destroyed the
     * last window, because console_init() repaints the whole screen. */
    if (restore_console) {
        f = irq_save();
        restore_console = false;
        __atomic_store_n(&active, false, __ATOMIC_RELEASE);
        ndmg = 0;
        full_repaint = false;
        irq_restore(f);
        reap_dying();
        console_init();
        wm_unlock();
        return;
    }

    if (!active || !fb_present()) {
        wm_unlock();
        return;
    }

    pump_mouse();
    pump_keyboard();

    if (++tickno % WM_HEAL_TICKS == 0)
        damage_all();

    f = irq_save();
    if (full_repaint) {
        full_repaint = false;
        ndmg = 0;
        screen_rect(&todo[0]);
        ntodo = 1;
    } else {
        ntodo = ndmg;
        for (int i = 0; i < ntodo; i++)
            todo[i] = dmg[i];
        ndmg = 0;
    }
    compositing = (ntodo > 0);
    irq_restore(f);

    if (ntodo == 0) {
        reap_dying();
        wm_unlock();
        return;
    }

    for (int i = 0; i < ntodo; i++)
        compose_rect(&todo[i]);
    for (int i = 0; i < ntodo; i++)
        fb_flush_rect(todo[i].x, todo[i].y, todo[i].w, todo[i].h);

    compositing = false;
    reap_dying();
    wm_unlock();
}

void wm_cleanup_task(int pid)
{
    wm_lock();
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (wins[i].used && !wins[i].dying && wins[i].pid == pid)
            win_destroy(&wins[i]);
    wm_unlock();
}

/* ------------------------------------------------------------ syscalls */

/* Never trust a wid: bounds-check it, require the slot to be live, and
 * require the caller to be the process that created it. */
/* The process-visible id for a new window: its slot, unless the process
 * already holds that id through a window resumed from the Hangar. */
static uint32_t owner_free_uwid(int pid, uint32_t slot)
{
    for (uint32_t cand = slot; cand < slot + WM_MAX_WINDOWS; cand++) {
        uint32_t u = cand % WM_MAX_WINDOWS;
        bool taken = false;
        for (int i = 0; i < WM_MAX_WINDOWS; i++)
            if ((uint32_t)i != slot && wins[i].used && wins[i].pid == pid &&
                wins[i].uwid == u)
                taken = true;
        if (!taken)
            return u;
    }
    return slot;
}

static struct window *win_lookup(uint64_t wid)
{
    struct window *win;

    if (wid >= WM_MAX_WINDOWS)
        return NULL;
    if (!current)
        return NULL;
    win = &wins[wid];
    if (win->used && !win->dying && win->pix &&
        win->pid == current->pid && win->uwid == wid)
        return win;
    /* A window resumed from the Hangar keeps the id its process knew,
     * which need not be the slot it landed in. */
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        win = &wins[i];
        if (win->used && !win->dying && win->pix &&
            win->pid == current->pid && win->uwid == wid)
            return win;
    }
    return NULL;
}

static void sanitize_title(char *dst, const char *src)
{
    int i;

    for (i = 0; i < 63 && src[i]; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
    }
    dst[i] = '\0';
}

long wm_sys_create(uint64_t ureq, uint64_t uout)
{
    struct k_wincreate req;
    struct k_wininfo info;
    struct k_mouse pointer;
    struct window *win = NULL;
    struct rect frame;
    uint32_t w, h;
    uint64_t need, phys, f;
    int npages, slot = -1;

    if (!fb_present())
        return -1;
    if (!current)
        return -1;
    if (copy_from_user(&req, (const void *)ureq, sizeof(req)) < 0)
        return -1;
    if (req.flags & ~(uint32_t)(K_WIN_NODECOR | K_WIN_DESKTOP | K_WIN_OVERLAY))
        return -1;
    if ((req.flags & K_WIN_OVERLAY) &&
        (!(req.flags & K_WIN_NODECOR) || (req.flags & K_WIN_DESKTOP)))
        return -1;
    if (!user_range_ok((const void *)uout, sizeof(info)))
        return -1;

    w = req.width;
    h = req.height;

    if (req.flags & K_WIN_DESKTOP) {
        /* The background layer is granted the screen, capped at the
         * compositor's per-window ceiling; k_wininfo reports what it got. */
        w = fb_width();
        h = fb_height();
        if (w > WM_MAX_W)
            w = WM_MAX_W;
        if (h > WM_MAX_H)
            h = WM_MAX_H;
    }
    if (w == 0 || h == 0 || w > WM_MAX_W || h > WM_MAX_H)
        return -1;

    npages = (int)(((uint64_t)w * h * 4 + PAGE_SIZE - 1) / PAGE_SIZE);
    if ((uint64_t)npages * PAGE_SIZE > USER_WIN_STRIDE)
        return -1;

    /* pmm_alloc_contig() and vmm_map_page() panic when memory runs out, so
     * refuse the window instead of taking the machine down with it. The
     * slack covers the page tables the mapping will need. */
    need = (uint64_t)npages + (uint64_t)npages / 512 + 8 + WM_PMM_RESERVE;
    if (pmm_free_pages() < need)
        return -1;

    wm_lock();

    if (req.flags & K_WIN_DESKTOP) {
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            if (wins[i].used && !wins[i].dying &&
                (wins[i].flags & K_WIN_DESKTOP)) {
                wm_unlock();
                return -1;
            }
        }
    }

    /* Reserve the slot before the slow work so two concurrent creates
     * cannot land on the same wid (and the same user address). */
    f = irq_save();
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!wins[i].used) {
            slot = i;
            memset(&wins[i], 0, sizeof(wins[i]));
            wins[i].used = true;
            wins[i].wid = (uint32_t)i;
            wins[i].pid = current->pid;
            wins[i].uid = current->uid;
            wins[i].uwid = owner_free_uwid(current->pid, (uint32_t)i);
            break;
        }
    }
    irq_restore(f);
    if (slot < 0) {
        wm_unlock();
        return -1;
    }

    win = &wins[slot];
    win->pml4 = current->pml4;
    win->w = (int)w;
    win->h = (int)h;
    win->flags = req.flags;
    win->npages = npages;
    win->uva = USER_WIN_BASE + (uint64_t)win->uwid * USER_WIN_STRIDE;
    win->x = req.x;
    win->y = req.y;
    sanitize_title(win->title, req.title);
    win_clamp(win);
    win->orig_x = win->x;
    win->orig_y = win->y;
    win->orig_w = win->w;
    win->orig_h = win->h;
    win->maximized = false;
    win->pinned = false;
    win->workspace = (uint8_t)current_ws;

    phys = pmm_alloc_contig(npages);
    for (int i = 0; i < npages; i++)
        vmm_map_page(win->pml4, win->uva + (uint64_t)i * PAGE_SIZE,
                     phys + (uint64_t)i * PAGE_SIZE, PTE_U | PTE_W);

    win->phys = phys;
    win->pix = (uint32_t *)P2V(phys);

    info.wid = win->uwid;
    info.width = w;
    info.height = h;
    info.buffer = win->uva;
    if (copy_to_user((void *)uout, &info, sizeof(info)) < 0) {
        f = irq_save();
        win_unlink(win);
        irq_restore(f);
        win_release(win);
        wm_unlock();
        return -1;
    }

    /* First window: take the framebuffer over from the boot console. */
    memset(&pointer, 0, sizeof(pointer));
    mouse_get(&pointer);

    f = irq_save();
    if (!active) {
        __atomic_store_n(&active, true, __ATOMIC_RELEASE);
        ndmg = 0;
        full_repaint = true;
        ptr_x = pointer.x;
        ptr_y = pointer.y;
    }
    z_insert(slot);
    irq_restore(f);

    pulse_emit(K_TOPIC_WIN, K_PULSE_WIN_OPEN, win->wid,
                  (uint32_t)win->pid, win->title);
    set_focus(slot);
    win_bounds(win, &frame);
    damage(frame.x, frame.y, frame.w, frame.h);
    wm_unlock();
    return 0;
}

long wm_sys_destroy(uint64_t wid)
{
    wm_lock();
    struct window *win = win_lookup(wid);

    if (!win) {
        wm_unlock();
        return -1;
    }
    win_destroy(win);
    wm_unlock();
    return 0;
}

long wm_sys_flush(uint64_t wid)
{
    wm_lock();
    struct window *win = win_lookup(wid);

    if (!win) {
        wm_unlock();
        return -1;
    }
    damage(win->x, win->y, win->w, win->h);
    wm_unlock();
    return 0;
}

long wm_sys_event(uint64_t wid, uint64_t uevent, uint64_t timeout_ms)
{
    struct k_event ev;
    if (!user_range_ok((const void *)uevent, sizeof(ev)))
        return -1;

    if (timeout_ms > (1ULL << 40))
        timeout_ms = 1ULL << 40;
    uint64_t ticks = (timeout_ms * TIMER_HZ) / 1000;
    if (timeout_ms && !ticks)
        ticks = 1;

    wm_lock();
    struct window *win = win_lookup(wid);
    if (!win) {
        wm_unlock();
        return -1;
    }
    uint64_t generation = window_events[wid].generation;
    uint64_t deadline = timer_ticks() + ticks;
    for (;;) {
        if (!win || window_events[wid].generation != generation) {
            wm_unlock();
            return -1;
        }

        if (evq_pop(win, &ev)) {
            wm_unlock();
            if (copy_to_user((void *)uevent, &ev, sizeof(ev)) < 0)
                return -1;
            return 1;
        }
        if (timeout_ms == 0 || timer_ticks() >= deadline) {
            wm_unlock();
            return 0;
        }

        /* A publisher must take the handoff lock to notify, so it cannot
         * miss us between dropping the manager mutex and parking. */
        uint64_t f = spin_lock_irqsave(&window_event_lock);
        wm_unlock();
        if (task_event_wait_locked(&window_events[wid].channel, 1, deadline,
                                   &window_event_lock, f) < 0)
            return 0;
        /* A pending kill or park is only acted on when the task leaves a
         * syscall, so a woken wait must not loop back to sleep on it. */
        if (current->kill_pending || current->park_state)
            return 0;
        wm_lock();
        win = win_lookup(wid);
    }
}

long wm_sys_move(uint64_t wid, int x, int y)
{
    wm_lock();
    struct window *win = win_lookup(wid);

    if (!win) {
        wm_unlock();
        return -1;
    }
    if (win->flags & K_WIN_DESKTOP) {
        wm_unlock();
        return -1;                    /* the background layer does not move */
    }
    move_window(win_index(win), x, y);
    wm_unlock();
    return 0;
}

/* A desktop window is the capability for shell policy operations. Keeping
 * this check inside the compositor means an ordinary GUI program cannot list
 * or manipulate its neighbours merely by guessing their small window ids. */
static bool caller_is_desktop(void)
{
    if (!current)
        return false;
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (wins[i].used && !wins[i].dying &&
            wins[i].pid == current->pid &&
            (wins[i].flags & K_WIN_DESKTOP))
            return true;
    return false;
}

long wm_sys_list(uint64_t index, uint64_t uout)
{
    struct k_winsummary out;
    uint64_t seen = 0;

    if (!user_range_ok((const void *)uout, sizeof(out)) || !current)
        return -1;

    wm_lock();
    for (int zi = nz - 1; zi >= 0; zi--) {
        struct window *win = &wins[zord[zi]];

        /* Panels and the desktop's launcher popup are shell surfaces, not
         * tasks. Only decorated application windows belong in the dock. */
        if (!win->used || win->dying ||
            (win->flags & (K_WIN_DESKTOP | K_WIN_NODECOR)) ||
            (win->uid != current->uid && current->uid != 0))
            continue;
        if (seen++ != index)
            continue;

        memset(&out, 0, sizeof(out));
        out.wid = win->wid;
        out.pid = win->pid;
        out.x = win->x;
        out.y = win->y;
        out.width = (uint32_t)win->w;
        out.height = (uint32_t)win->h;
        out.flags = win->flags;
        if (focus == win_index(win))
            out.state |= K_WIN_STATE_FOCUSED;
        if (win->minimized)
            out.state |= K_WIN_STATE_MINIMIZED;
        if (win->pinned)
            out.state |= K_WIN_STATE_PINNED;
        if (win->maximized)
            out.state |= K_WIN_STATE_MAXIMIZED;
        out.state |= ((uint32_t)(win->workspace & K_WIN_STATE_WS_MASK)
                      << K_WIN_STATE_WS_SHIFT);
        out.state |= ((uint32_t)(current_ws & K_WIN_STATE_WS_MASK)
                      << K_WIN_STATE_ACTIVE_WS_SHIFT);
        strncpy(out.title, win->title, sizeof(out.title) - 1);
        wm_unlock();
        return copy_to_user((void *)uout, &out, sizeof(out)) < 0 ? -1 : 0;
    }
    wm_unlock();
    return -1;
}

static void sync_window_visibility(int idx)
{
    struct window *win = &wins[idx];
    struct rect bounds;

    if (!win->minimized) {
        win_bounds(win, &bounds);
        damage(bounds.x, bounds.y, bounds.w, bounds.h);
    }
    if (!win->pinned && (int)win->workspace != current_ws) {
        if (dragging == idx)
            dragging = -1;
        if (press_grab == idx)
            press_grab = -1;
        if (focus == idx)
            set_focus(z_top_focusable());
    }
}

long wm_sys_ctl(uint64_t wid, uint64_t action)
{
    struct window *win;
    int idx;

    wm_lock();
    if (!current) {
        wm_unlock();
        return -1;
    }
    if (action == K_WIN_CTL_SWITCH_WS) {
        if (wid >= K_WM_MAX_WORKSPACES ||
            (!caller_is_desktop() && current->uid != 0 && !wm_active())) {
            wm_unlock();
            return -1;
        }
        switch_workspace((int)wid);
        wm_unlock();
        return 0;
    }
    if (wid >= WM_MAX_WINDOWS) {
        wm_unlock();
        return -1;
    }
    win = &wins[wid];
    if (!win->used || win->dying || (win->flags & K_WIN_DESKTOP) ||
        (win->uid != current->uid && current->uid != 0)) {
        wm_unlock();
        return -1;
    }
    idx = win_index(win);

    if ((action & 0xFFU) == K_WIN_CTL_MOVE_WS) {
        uint64_t ws = action >> 8;
        if (ws >= K_WM_MAX_WORKSPACES) {
            wm_unlock();
            return -1;
        }
        win->workspace = (uint8_t)ws;
        sync_window_visibility(idx);
        wm_unlock();
        return 0;
    }

    switch (action) {
    case K_WIN_CTL_FOCUS:
    case K_WIN_CTL_RESTORE:
        restore_window(idx, true);
        break;
    case K_WIN_CTL_MINIMIZE:
        minimize_window(idx);
        break;
    case K_WIN_CTL_CLOSE:
        evq_push(win, KEV_CLOSE, 0, 0, 0, 0);
        break;
    case K_WIN_CTL_MAXIMIZE:
        if (win->minimized)
            restore_window(idx, true);
        if (toggle_maximize(idx) < 0) {
            wm_unlock();
            return -1;
        }
        break;
    case K_WIN_CTL_TOGGLE_PIN:
        win->pinned = !win->pinned;
        sync_window_visibility(idx);
        break;
    default:
        wm_unlock();
        return -1;
    }
    wm_unlock();
    return 0;
}

long wm_sys_theme(uint64_t utheme)
{
    struct k_wm_theme t;
    uint32_t uid;

    if (!current)
        return -1;
    if (copy_from_user(&t, (const void *)utheme, sizeof(t)) < 0)
        return -1;

    wm_lock();
    if (!caller_is_desktop() && current->uid != 0) {
        wm_unlock();
        return -1;
    }
    wm_theme = t;
    damage_all();
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (wins[i].used && !wins[i].dying)
            evq_push(&wins[i], KEV_THEME, 0, 0, 0, 0);
    }
    uid = current->uid;
    wm_unlock();

    pulse_emit(K_TOPIC_THEME, K_PULSE_THEME_SET, uid, 0, "theme");
    return 0;
}

long wm_sys_resize(uint64_t wid, uint64_t w, uint64_t h)
{
    long rc;

    if (w < 64 || w > WM_MAX_W || h < 48 || h > WM_MAX_H)
        return -1;

    wm_lock();
    struct window *win = win_lookup(wid);
    if (!win) {
        wm_unlock();
        return -1;
    }
    rc = wm_resize_window(win_index(win), (int)w, (int)h);
    wm_unlock();
    return rc;
}

/* ------------------------------------------------------------- Hangar */

int wm_hangar_window(int pid, int index, struct hangar_win *meta,
                     const uint32_t **pix)
{
    int seen = 0, rc = 0;

    wm_lock();
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        struct window *win = &wins[i];
        if (!win->used || win->dying || win->pid != pid || !win->pix)
            continue;
        if (win->flags & K_WIN_DESKTOP) {
            rc = -1;
            break;
        }
        if (seen++ != index)
            continue;
        memset(meta, 0, sizeof(*meta));
        meta->uwid = win->uwid;
        meta->flags = win->flags;
        meta->x = win->x;
        meta->y = win->y;
        meta->w = win->w;
        meta->h = win->h;
        meta->orig_x = win->orig_x;
        meta->orig_y = win->orig_y;
        meta->orig_w = win->orig_w;
        meta->orig_h = win->orig_h;
        meta->minimized = win->minimized;
        meta->maximized = win->maximized;
        meta->pinned = win->pinned;
        meta->workspace = win->workspace;
        memcpy(meta->title, win->title, sizeof(meta->title));
        meta->title[sizeof(meta->title) - 1] = '\0';
        *pix = win->pix;
        rc = 1;
        break;
    }
    wm_unlock();
    return rc;
}

int wm_hangar_count(int pid)
{
    int n = 0;

    wm_lock();
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (wins[i].used && !wins[i].dying && wins[i].pid == pid)
            n++;
    wm_unlock();
    return n;
}

uint32_t *wm_hangar_restore(const struct hangar_win *meta, int *slot_out)
{
    struct window *win;
    uint64_t need, phys, f;
    int npages, slot = -1;

    if (!fb_present() || !current || meta->uwid >= WM_MAX_WINDOWS ||
        meta->w <= 0 || meta->h <= 0 || meta->w > WM_MAX_W ||
        meta->h > WM_MAX_H)
        return NULL;
    npages = (int)(((uint64_t)meta->w * (uint64_t)meta->h * 4 + PAGE_SIZE - 1)
                   / PAGE_SIZE);
    need = (uint64_t)npages + (uint64_t)npages / 512 + 8 + WM_PMM_RESERVE;
    if (pmm_free_pages() < need)
        return NULL;

    wm_lock();
    f = irq_save();
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!wins[i].used) {
            slot = i;
            memset(&wins[i], 0, sizeof(wins[i]));
            wins[i].used = true;
            wins[i].wid = (uint32_t)i;
            wins[i].pid = current->pid;
            wins[i].uid = current->uid;
            wins[i].uwid = meta->uwid;
            break;
        }
    }
    irq_restore(f);
    if (slot < 0) {
        wm_unlock();
        return NULL;
    }
    win = &wins[slot];
    win->pml4 = current->pml4;
    win->w = meta->w;
    win->h = meta->h;
    win->flags = meta->flags & K_WIN_NODECOR;
    win->npages = npages;
    win->uva = USER_WIN_BASE + (uint64_t)meta->uwid * USER_WIN_STRIDE;
    win->x = meta->x;
    win->y = meta->y;
    sanitize_title(win->title, meta->title);
    win_clamp(win);
    win->orig_x = meta->orig_x;
    win->orig_y = meta->orig_y;
    win->orig_w = meta->orig_w;
    win->orig_h = meta->orig_h;
    win->maximized = meta->maximized;
    win->pinned = meta->pinned;
    win->workspace = meta->workspace < K_WM_MAX_WORKSPACES
                   ? meta->workspace : (uint8_t)current_ws;

    phys = pmm_alloc_contig(npages);
    for (int i = 0; i < npages; i++)
        vmm_map_page(win->pml4, win->uva + (uint64_t)i * PAGE_SIZE,
                     phys + (uint64_t)i * PAGE_SIZE, PTE_U | PTE_W);
    win->phys = phys;
    win->pix = (uint32_t *)P2V(phys);
    memset(win->pix, 0, (size_t)npages * PAGE_SIZE);
    wm_unlock();
    *slot_out = slot;
    return win->pix;
}

void wm_hangar_show(int slot, bool minimized)
{
    struct k_mouse pointer;
    struct rect frame;
    struct window *win;
    uint64_t f;

    if (slot < 0 || slot >= WM_MAX_WINDOWS)
        return;
    memset(&pointer, 0, sizeof(pointer));
    mouse_get(&pointer);

    wm_lock();
    win = &wins[slot];
    if (!win->used || win->dying || !current || win->pid != current->pid) {
        wm_unlock();
        return;
    }
    f = irq_save();
    if (!active) {
        __atomic_store_n(&active, true, __ATOMIC_RELEASE);
        ndmg = 0;
        full_repaint = true;
        ptr_x = pointer.x;
        ptr_y = pointer.y;
    }
    z_insert(slot);
    irq_restore(f);
    pulse_emit(K_TOPIC_WIN, K_PULSE_WIN_OPEN, win->wid,
               (uint32_t)win->pid, win->title);
    sync_window_visibility(slot);
    if (minimized)
        minimize_window(slot);
    else if (win->workspace == current_ws || win->pinned)
        set_focus(slot);
    win_bounds(win, &frame);
    damage(frame.x, frame.y, frame.w, frame.h);
    evq_push(win, KEV_THEME, 0, 0, 0, 0);
    wm_unlock();
}
