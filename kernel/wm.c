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
 * Paint order inside a damage rectangle: desktop fill, K_WIN_DESKTOP
 * windows, ordinary windows bottom-to-top, then the pointer. Because every
 * rectangle is recomposited from the bottom up there is no save-under to
 * get wrong and the cursor cannot leave a trail.
 *
 * Console handover. wm_active() is false until the first window exists and
 * the compositor never writes a pixel before that, so the boot console owns
 * the framebuffer exactly as it does today. Keyboard input is only taken
 * away from the console while the compositor is active AND some window has
 * focus. When the last window goes away the compositor stands down and asks
 * wm_tick() to run console_init(), which repaints the text console.
 *
 * Locking. The window table, the z-order, the per-window event queues and
 * the damage list are touched from ordinary task context (syscalls) and from
 * the compositor thread, both preemptible, so every mutation of shared state
 * runs under irq_save()/irq_restore(). The compositing pass itself is far
 * too long to hold interrupts off, so it snapshots the damage list, sets
 * `compositing`, and any destroy that lands in that window is deferred: the
 * slot stays reserved (so it cannot be recycled under the painter) and the
 * frames are released at the end of the pass.
 */

/* ------------------------------------------------------------ geometry */

#define WM_MAX_WINDOWS  USER_WIN_MAX          /* 16 */
#define WM_MAX_W        WM_WINDOW_MAX_W        /* 1440p, from wm.h */
#define WM_MAX_H        WM_WINDOW_MAX_H
#define WM_EVQ          32                    /* per-window events */

#define WM_TITLE_H      20
#define WM_BORDER       1
#define WM_CLOSE_SZ     14
#define WM_CLOSE_PAD    3                     /* gap right of the close box */
#define WM_TITLE_PAD    6                     /* gap left of the title text */

/* Frames the compositor refuses to eat into, so a huge window request
 * fails instead of starving the kernel (pmm_alloc*() panics when dry). */
#define WM_PMM_RESERVE  512

/* Self-heal: repaint everything this often, so kernel output that reached
 * the shadow buffer behind the compositor's back cannot linger. */
#define WM_HEAL_TICKS   50                    /* ~1 s at 50 Hz */

#define WM_MAX_DAMAGE   8

/* ---------------------------------------------------------------- theme */

#define C_DESKTOP       0x00162032
#define C_BORDER_F      0x004E8CD0
#define C_BORDER_B      0x00303845
#define C_TITLE_F       0x002F6FB5
#define C_TITLE_B       0x0039414E
#define C_TITLE_EDGE_F  0x001C4A7C
#define C_TITLE_EDGE_B  0x00262C36
#define C_TEXT_F        0x00FFFFFF
#define C_TEXT_B        0x00A2AAB6
#define C_CLOSE_F       0x00C0392B
#define C_CLOSE_B       0x004A525E
#define C_CLOSE_X_F     0x00FFFFFF
#define C_CLOSE_X_B     0x00B0B8C4
#define C_CURSOR_EDGE   0x00000000
#define C_CURSOR_FILL   0x00FFFFFF

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
    uint32_t  wid;           /* == slot index; fixes the user VA */
    int       pid;           /* owning process */
    uint64_t *pml4;          /* owner address space, for the unmap */

    int       x, y;          /* client-area origin on screen */
    int       w, h;          /* client-area size */
    uint32_t  flags;         /* K_WIN_* */
    char      title[64];

    uint64_t  phys;          /* first frame of the pixel run */
    int       npages;
    uint32_t *pix;           /* kernel view of the pixels (direct map) */
    uint64_t  uva;           /* user view */

    struct k_event evq[WM_EVQ];
    unsigned  ev_head, ev_tail;
};

static struct window wins[WM_MAX_WINDOWS];

/* Stacking order, bottom first, holding slot indices. K_WIN_DESKTOP
 * windows are kept as a block at the bottom and never leave it. */
static uint8_t zord[WM_MAX_WINDOWS];
static int     nz;

static bool     active;              /* compositor owns the framebuffer */
static bool     restore_console;     /* wm_tick must repaint the console */
static bool     full_repaint;
static volatile bool compositing;    /* a paint pass is in flight */

static int      focus = -1;          /* slot index, or -1 */
static int      dragging = -1;       /* slot being dragged by the title bar */
static int      drag_dx, drag_dy;    /* pointer offset inside the window */
static int      press_grab = -1;     /* slot that owns the button press */

static int      ptr_x, ptr_y;        /* where the cursor is drawn */
static uint32_t tickno;

static struct rect dmg[WM_MAX_DAMAGE];
static int         ndmg;

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

/* Title bar and close box in screen coordinates. close->w is 0 when the
 * window is too narrow to hold a usable close box. */
static void win_title_rects(const struct window *win, struct rect *bar,
                            struct rect *close)
{
    bar->x = win->x;
    bar->y = win->y - WM_TITLE_H;
    bar->w = win->w;
    bar->h = WM_TITLE_H;

    close->x = bar->x + bar->w - WM_CLOSE_SZ - WM_CLOSE_PAD;
    close->y = bar->y + (WM_TITLE_H - WM_CLOSE_SZ) / 2;
    close->w = WM_CLOSE_SZ;
    close->h = WM_CLOSE_SZ;

    if (bar->w < WM_CLOSE_SZ + 2 * WM_CLOSE_PAD) {
        close->w = 0;
        close->h = 0;
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

/* Topmost focusable window, or -1. */
static int z_top_focusable(void)
{
    for (int i = nz - 1; i >= 0; i--)
        if (!(wins[zord[i]].flags & K_WIN_DESKTOP))
            return zord[i];
    return -1;
}

/* ---------------------------------------------------------- event queue */

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

    if (idx >= 0 && (wins[idx].flags & K_WIN_DESKTOP))
        return;                       /* the desktop layer never takes focus */
    if (focus == idx)
        return;

    old = focus;
    focus = idx;

    if (old >= 0 && wins[old].used && !wins[old].dying) {
        evq_push(&wins[old], KEV_FOCUS, 0, 0, 0, 0);
        win_frame(&wins[old], &f);
        damage(f.x, f.y, f.w, f.h);
    }
    if (idx >= 0) {
        evq_push(&wins[idx], KEV_FOCUS, 0, 0, 1, 0);
        win_frame(&wins[idx], &f);
        damage(f.x, f.y, f.w, f.h);
    }
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

/* ---------------------------------------------------------- compositing */

static void paint_decor(const struct window *win, const struct rect *clip)
{
    struct rect frame, bar, close, tclip, o;
    bool foc = (focus == win_index(win));
    uint32_t border = foc ? C_BORDER_F : C_BORDER_B;
    uint32_t barbg  = foc ? C_TITLE_F  : C_TITLE_B;
    uint32_t edge   = foc ? C_TITLE_EDGE_F : C_TITLE_EDGE_B;
    uint32_t text   = foc ? C_TEXT_F   : C_TEXT_B;

    win_frame(win, &frame);

    /* 1px border, drawn as four bars so the client area is never touched */
    fill_clip(clip, frame.x, frame.y, frame.w, WM_BORDER, border);
    fill_clip(clip, frame.x, frame.y + frame.h - WM_BORDER, frame.w,
              WM_BORDER, border);
    fill_clip(clip, frame.x, frame.y, WM_BORDER, frame.h, border);
    fill_clip(clip, frame.x + frame.w - WM_BORDER, frame.y, WM_BORDER,
              frame.h, border);

    win_title_rects(win, &bar, &close);
    fill_clip(clip, bar.x, bar.y, bar.w, bar.h, barbg);
    fill_clip(clip, bar.x, bar.y + bar.h - 1, bar.w, 1, edge);

    /* Title text, clipped to the space left of the close box so a long
     * name is cut off rather than scribbled over the button. */
    tclip.x = bar.x + WM_TITLE_PAD;
    tclip.y = bar.y;
    tclip.h = bar.h - 1;
    tclip.w = (close.w ? close.x - 4 : bar.x + bar.w - 2) - tclip.x;
    if (tclip.w > 0 && rect_isect(&tclip, clip, &o))
        text_clip(&o, tclip.x, bar.y + (WM_TITLE_H - FONT_H) / 2,
                  win->title, text, barbg);

    if (close.w) {
        uint32_t cbg = foc ? C_CLOSE_F : C_CLOSE_B;
        uint32_t cfg = foc ? C_CLOSE_X_F : C_CLOSE_X_B;

        fill_clip(clip, close.x, close.y, close.w, close.h, cbg);
        for (int i = 4; i < close.w - 4; i++) {
            fill_clip(clip, close.x + i, close.y + i, 2, 1, cfg);
            fill_clip(clip, close.x + i, close.y + close.h - 1 - i, 2, 1, cfg);
        }
    }
}

static void paint_window(const struct window *win, const struct rect *clip)
{
    if (win_decorated(win))
        paint_decor(win, clip);
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

/* True when a K_WIN_DESKTOP window already covers `clip` completely, in
 * which case the background fill underneath it is wasted work. */
static bool desktop_covers(const struct rect *clip)
{
    for (int i = 0; i < nz; i++) {
        const struct window *win = &wins[zord[i]];
        struct rect r;

        if (!(win->flags & K_WIN_DESKTOP))
            continue;
        r.x = win->x; r.y = win->y; r.w = win->w; r.h = win->h;
        if (clip->x >= r.x && clip->y >= r.y &&
            clip->x + clip->w <= r.x + r.w &&
            clip->y + clip->h <= r.y + r.h)
            return true;
    }
    return false;
}

static void compose_rect(const struct rect *clip)
{
    if (!desktop_covers(clip))
        fb_fill_rect(clip->x, clip->y, clip->w, clip->h, C_DESKTOP);

    for (int i = 0; i < nz; i++) {
        struct window *win = &wins[zord[i]];
        if (!win->used || win->dying || !win->pix)
            continue;
        paint_window(win, clip);
    }

    paint_cursor(clip);
}

/* -------------------------------------------------------- hit testing */

enum {
    HIT_NONE = 0,
    HIT_CLIENT,
    HIT_TITLE,
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
        struct rect frame, client, bar, close;

        if (!win->used || win->dying)
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
            win_title_rects(win, &bar, &close);
            if (close.w && rect_contains(&close, mx, my)) {
                *region = HIT_CLOSE;
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

static void move_window(int idx, int nx, int ny)
{
    struct window *win = &wins[idx];
    struct rect before, after;

    win_frame(win, &before);
    win->x = nx;
    win->y = ny;
    win_clamp(win);
    win_frame(win, &after);

    if (before.x == after.x && before.y == after.y)
        return;
    damage(before.x, before.y, before.w, before.h);
    damage(after.x, after.y, after.w, after.h);
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
            win_frame(win, &f);
            damage(f.x, f.y, f.w, f.h);
        }
        set_focus(idx);
    }

    switch (region) {
    case HIT_CLOSE:
        evq_push(win, KEV_CLOSE, 0, 0, 0, buttons);
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
    if (focus < 0 || !wins[focus].used || wins[focus].dying)
        return;

    for (int i = 0; i < 64; i++) {
        int c = input_trygetc();
        if (c < 0)
            return;
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
    struct task *t = task_find(win->pid);

    return t && t->pml4 == win->pml4 && t->pml4 != vmm_kernel_pml4();
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

/* Fast half: take the window out of every list that the compositor and the
 * input router walk, so nothing can reach it again. Interrupts off. */
static void win_unlink(struct window *win)
{
    int idx = win_index(win);

    z_remove(idx);
    win->dying = true;
    if (focus == idx)
        focus = -1;
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
    struct rect frame;
    bool defer;
    uint64_t f;

    win_frame(win, &frame);

    f = irq_save();
    win_unlink(win);
    defer = compositing;
    irq_restore(f);

    damage(frame.x, frame.y, frame.w, frame.h);

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
    active = false;
    restore_console = false;
    full_repaint = false;
    compositing = false;
    focus = -1;
    dragging = -1;
    press_grab = -1;
    ptr_x = ptr_y = 0;
    tickno = 0;

    kprintf("wm: compositor ready (%d windows max, %dx%d each)\n",
            WM_MAX_WINDOWS, WM_MAX_W, WM_MAX_H);
}

bool wm_active(void)
{
    return active;
}

void wm_tick(void)
{
    struct rect todo[WM_MAX_DAMAGE];
    int ntodo;
    uint64_t f;

    /* Standing down: the text console gets the screen back. Done here, on
     * a kernel thread, rather than inside the syscall that destroyed the
     * last window, because console_init() repaints the whole screen. */
    if (restore_console) {
        f = irq_save();
        restore_console = false;
        active = false;
        ndmg = 0;
        full_repaint = false;
        irq_restore(f);
        reap_dying();
        console_init();
        return;
    }

    if (!active || !fb_present())
        return;

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
        return;
    }

    for (int i = 0; i < ntodo; i++)
        compose_rect(&todo[i]);
    for (int i = 0; i < ntodo; i++)
        fb_flush_rect(todo[i].x, todo[i].y, todo[i].w, todo[i].h);

    compositing = false;
    reap_dying();
}

void wm_cleanup_task(int pid)
{
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (wins[i].used && !wins[i].dying && wins[i].pid == pid)
            win_destroy(&wins[i]);
}

/* ------------------------------------------------------------ syscalls */

/* Never trust a wid: bounds-check it, require the slot to be live, and
 * require the caller to be the process that created it. */
static struct window *win_lookup(uint64_t wid)
{
    struct window *win;

    if (wid >= WM_MAX_WINDOWS)
        return NULL;
    win = &wins[wid];
    if (!win->used || win->dying || !win->pix)
        return NULL;
    if (!current || win->pid != current->pid)
        return NULL;
    return win;
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
            break;
        }
    }
    irq_restore(f);
    if (slot < 0)
        return -1;

    win = &wins[slot];
    win->pml4 = current->pml4;
    win->w = (int)w;
    win->h = (int)h;
    win->flags = req.flags;
    win->npages = npages;
    win->uva = USER_WIN_BASE + (uint64_t)slot * USER_WIN_STRIDE;
    win->x = req.x;
    win->y = req.y;
    sanitize_title(win->title, req.title);
    win_clamp(win);

    phys = pmm_alloc_contig(npages);
    for (int i = 0; i < npages; i++)
        vmm_map_page(win->pml4, win->uva + (uint64_t)i * PAGE_SIZE,
                     phys + (uint64_t)i * PAGE_SIZE, PTE_U | PTE_W);

    win->phys = phys;
    win->pix = (uint32_t *)P2V(phys);

    info.wid = (uint32_t)slot;
    info.width = w;
    info.height = h;
    info.buffer = win->uva;
    if (copy_to_user((void *)uout, &info, sizeof(info)) < 0) {
        f = irq_save();
        win_unlink(win);
        irq_restore(f);
        win_release(win);
        return -1;
    }

    /* First window: take the framebuffer over from the boot console. */
    memset(&pointer, 0, sizeof(pointer));
    mouse_get(&pointer);

    f = irq_save();
    if (!active) {
        active = true;
        ndmg = 0;
        full_repaint = true;
        ptr_x = pointer.x;
        ptr_y = pointer.y;
    }
    z_insert(slot);
    irq_restore(f);

    set_focus(slot);
    win_frame(win, &frame);
    damage(frame.x, frame.y, frame.w, frame.h);
    return 0;
}

long wm_sys_destroy(uint64_t wid)
{
    struct window *win = win_lookup(wid);

    if (!win)
        return -1;
    win_destroy(win);
    return 0;
}

long wm_sys_flush(uint64_t wid)
{
    struct window *win = win_lookup(wid);

    if (!win)
        return -1;
    damage(win->x, win->y, win->w, win->h);
    return 0;
}

long wm_sys_event(uint64_t wid, uint64_t uevent, uint64_t timeout_ms)
{
    struct k_event ev;
    uint64_t deadline, ticks;

    if (!win_lookup(wid))
        return -1;
    if (!user_range_ok((const void *)uevent, sizeof(ev)))
        return -1;

    if (timeout_ms > (1ULL << 40))
        timeout_ms = 1ULL << 40;
    ticks = (timeout_ms * TIMER_HZ) / 1000;
    if (timeout_ms && !ticks)
        ticks = 1;
    deadline = timer_ticks() + ticks;

    for (;;) {
        /* Re-resolve every round: the window can be destroyed while this
         * task sleeps, and the slot must not be followed after that. */
        struct window *win = win_lookup(wid);
        if (!win)
            return -1;

        if (evq_pop(win, &ev)) {
            if (copy_to_user((void *)uevent, &ev, sizeof(ev)) < 0)
                return -1;
            return 1;
        }
        if (timeout_ms == 0 || timer_ticks() >= deadline)
            return 0;
        /* A pending kill is only acted on when the task leaves a syscall,
         * so a long wait must not sit on it: give up and let the syscall
         * layer's task_check_kill() do its job. */
        if (current->kill_pending)
            return 0;
        task_sleep_ticks(1);
    }
}

long wm_sys_move(uint64_t wid, int x, int y)
{
    struct window *win = win_lookup(wid);

    if (!win)
        return -1;
    if (win->flags & K_WIN_DESKTOP)
        return -1;                    /* the background layer does not move */
    move_window(win_index(win), x, y);
    return 0;
}
