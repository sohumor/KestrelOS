/* gpaint.c - a small paint program.
 *
 * Freehand drawing with the mouse, a 16-colour palette, five brush
 * sizes, an eraser, clear, and save/load in the KPX1 format below.
 *
 * The KPX1 image format (defined here, nothing external)
 * ------------------------------------------------------
 *   offset  size  meaning
 *   0       4     magic, the ASCII bytes "KPX1"
 *   4       4     width in pixels,  little-endian uint32
 *   8       4     height in pixels, little-endian uint32
 *   12      4     flags, little-endian uint32, 0 in this version
 *   16      w*h*4 pixels, row-major, top row first, little-endian
 *                 uint32 each, 0x00RRGGBB (the high byte is 0)
 * The file is exactly 16 + w*h*4 bytes. There is no compression: this
 * is a canvas dump, chosen so that reading it back is a single read and
 * any other program on the system can consume it without a library.
 *
 * Implementation note: the canvas is an off-screen gui_window - the same
 * struct, with px pointing at a malloc'd buffer and no compositor window
 * behind it - so every libgui primitive draws into the picture with no
 * duplicate rasteriser here.
 *
 * usage: gpaint [file.kpx]
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui.h"

#define WIN_W     764
#define WIN_H     560
#define TOOL_W    96
#define TOP_H     38
#define STATUS_H  22
#define CANVAS_X  TOOL_W
#define CANVAS_Y  TOP_H
#define CANVAS_W  (WIN_W - TOOL_W)
#define CANVAS_H  (WIN_H - TOP_H - STATUS_H)
#define MAX_PATH  256

#define KPX_MAGIC "KPX1"
#define KPX_HEADER 16

static const uint32_t swatches[16] = {
    0x000000U, 0xFFFFFFU, 0x808080U, 0xC8C8C8U,
    0xB03A2EU, 0xE67E22U, 0xE8C547U, 0x4F8F4FU,
    0x2E8B77U, 0x2E6DB4U, 0x3B4E9BU, 0x7D4E9BU,
    0xC2529BU, 0x8B5A2BU, 0xD9C7A0U, 0x1B2A3AU
};

static const int brushes[5] = { 1, 2, 4, 8, 16 };

static gui_window canvas;              /* off-screen picture */
static uint32_t canvas_bg = 0xFFFFFFU;
static uint32_t colour = 0x000000U;
static int brush = 2;                  /* index into brushes[] */
static int eraser;
static int last_x = -1, last_y = -1;
static char status[128];
static char namebuf[MAX_PATH] = "/paint.kpx";
static gui_textbox_state namebox;

/* ---------------------------------------------------------- the canvas */

static void canvas_dot(int x, int y, uint32_t c)
{
    int r = brushes[brush] / 2;

    if (r < 1)
        gui_pixel(&canvas, x, y, c);
    else
        gui_disc(&canvas, x, y, r, c);
}

/* Stamp along the segment so a fast drag still leaves a solid stroke. */
static void canvas_stroke(int x0, int y0, int x1, int y1, uint32_t c)
{
    int dx = x1 - x0, dy = y1 - y0;
    int steps, i;

    if (dx < 0)
        dx = -dx;
    if (dy < 0)
        dy = -dy;
    steps = dx > dy ? dx : dy;
    if (steps == 0) {
        canvas_dot(x0, y0, c);
        return;
    }
    for (i = 0; i <= steps; i++)
        canvas_dot(x0 + ((x1 - x0) * i) / steps,
                   y0 + ((y1 - y0) * i) / steps, c);
}

static void canvas_clear(void)
{
    gui_rect(&canvas, 0, 0, canvas.w, canvas.h, canvas_bg);
}

/* ------------------------------------------------------------ file I/O */

static void put_le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static uint32_t get_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int save_image(const char *path)
{
    unsigned char head[KPX_HEADER];
    unsigned long total = (unsigned long)canvas.w * canvas.h;
    unsigned long done = 0;
    int fd;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    memcpy(head, KPX_MAGIC, 4);
    put_le32(head + 4, (uint32_t)canvas.w);
    put_le32(head + 8, (uint32_t)canvas.h);
    put_le32(head + 12, 0);
    if (write(fd, head, sizeof(head)) != (long)sizeof(head)) {
        close(fd);
        return -1;
    }
    /* The buffer is already little-endian 0x00RRGGBB in memory, so the
     * pixels go out as they stand. */
    while (done < total) {
        unsigned long chunk = total - done;
        long n;

        if (chunk > 4096)
            chunk = 4096;
        n = write(fd, canvas.px + done, chunk * sizeof(uint32_t));
        if (n <= 0) {
            close(fd);
            return -1;
        }
        done += (unsigned long)n / sizeof(uint32_t);
    }
    close(fd);
    return 0;
}

static int load_image(const char *path)
{
    unsigned char head[KPX_HEADER];
    uint32_t w, h;
    unsigned long row;
    int fd;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    if (read(fd, head, sizeof(head)) != (long)sizeof(head) ||
        memcmp(head, KPX_MAGIC, 4) != 0) {
        close(fd);
        return -2;
    }
    w = get_le32(head + 4);
    h = get_le32(head + 8);
    if (w == 0 || h == 0 || w > 4096 || h > 4096) {
        close(fd);
        return -2;
    }

    canvas_clear();
    for (row = 0; row < h && row < (unsigned long)canvas.h; row++) {
        static uint32_t line[4096];
        unsigned long want = w * sizeof(uint32_t);
        unsigned long got = 0;
        unsigned long copy = w < (uint32_t)canvas.w ? w
                                                    : (uint32_t)canvas.w;

        while (got < want) {
            long n = read(fd, (char *)line + got, want - got);

            if (n <= 0)
                break;
            got += (unsigned long)n;
        }
        if (got < want)
            break;
        memcpy(canvas.px + row * canvas.w, line, copy * sizeof(uint32_t));
    }
    close(fd);
    return 0;
}

/* ------------------------------------------------------------ painting */

static void blit_canvas(gui_window *win, int x, int y, int w, int h)
{
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > canvas.w)
        w = canvas.w - x;
    if (y + h > canvas.h)
        h = canvas.h - y;
    if (w <= 0 || h <= 0)
        return;
    gui_bitmap(win, CANVAS_X + x, CANVAS_Y + y, w, h,
               canvas.px + (long)y * canvas.w + x, canvas.w);
}

static void draw_status(gui_window *win)
{
    int y = WIN_H - STATUS_H;

    gui_rect(win, 0, y, WIN_W, STATUS_H, GUI_PANEL);
    gui_rect(win, 0, y, WIN_W, 1, GUI_BORDER);
    gui_clip(win, gui_mkrc(8, y, WIN_W - 16, STATUS_H));
    gui_text(win, 8, y + (STATUS_H - GUI_FONT_H) / 2, status, GUI_TEXT_DIM,
             GUI_TRANSPARENT);
    gui_unclip(win);
}

static void draw_chrome(gui_window *win, gui_ui *ui, int *action)
{
    gui_rc r;
    int i, y;

    /* top bar: file name and the file actions */
    gui_rect(win, 0, 0, WIN_W, TOP_H, GUI_PANEL);
    gui_text(win, 8, (TOP_H - GUI_FONT_H) / 2, "file", GUI_TEXT_DIM,
             GUI_TRANSPARENT);
    if (gui_textbox(win, gui_mkrc(46, 6, WIN_W - 46 - 220, 26), &namebox,
                    ui))
        *action = 1;
    if (gui_button(win, gui_mkrc(WIN_W - 210, 6, 66, 26), "Save", ui))
        *action = 1;
    if (gui_button(win, gui_mkrc(WIN_W - 138, 6, 66, 26), "Load", ui))
        *action = 2;
    if (gui_button(win, gui_mkrc(WIN_W - 66, 6, 56, 26), "Clear", ui))
        *action = 3;

    /* left tool column */
    gui_rect(win, 0, TOP_H, TOOL_W, WIN_H - TOP_H - STATUS_H, GUI_PANEL);
    y = TOP_H + 8;
    for (i = 0; i < 16; i++) {
        int col = i % 2;
        int row = i / 2;

        r = gui_mkrc(10 + col * 38, y + row * 30, 34, 24);
        gui_rect(win, r.x, r.y, r.w, r.h, swatches[i]);
        gui_frame(win, r.x, r.y, r.w, r.h,
                  swatches[i] == colour && !eraser ? GUI_ACCENT
                                                   : GUI_BORDER);
        if (swatches[i] == colour && !eraser)
            gui_frame(win, r.x - 2, r.y - 2, r.w + 4, r.h + 4, GUI_ACCENT);
        if (gui_hit(r, ui->up_x, ui->up_y) && ui->up &&
            gui_hit(r, ui->down_x, ui->down_y)) {
            colour = swatches[i];
            eraser = 0;
        }
    }

    y += 8 * 30 + 10;
    gui_text(win, 12, y, "brush", GUI_TEXT_DIM, GUI_TRANSPARENT);
    y += GUI_FONT_H + 4;
    for (i = 0; i < 5; i++) {
        char lab[8];

        snprintf(lab, sizeof(lab), "%d", brushes[i]);
        r = gui_mkrc(10 + (i % 3) * 26, y + (i / 3) * 28, 24, 24);
        if (gui_button_ex(win, r, lab, ui, 1, i == brush))
            brush = i;
    }

    y += 2 * 28 + 8;
    if (gui_button_ex(win, gui_mkrc(10, y, 76, 24), "Eraser", ui, 1,
                      eraser))
        eraser = !eraser;
}

static void full_repaint(gui_window *win, gui_ui *ui, int *action)
{
    draw_chrome(win, ui, action);
    blit_canvas(win, 0, 0, canvas.w, canvas.h);
    draw_status(win);
}

/* ------------------------------------------------------------------ main */

static void set_status_default(void)
{
    snprintf(status, sizeof(status),
             "%dx%d canvas   brush %d   %s   KPX1 images", canvas.w,
             canvas.h, brushes[brush], eraser ? "eraser" : "pen");
}

int main(int argc, char **argv)
{
    struct k_event ev;
    gui_window *win;
    gui_ui ui;
    int dirty = 1, i, action = 0, had_file = 0;

    memset(&ui, 0, sizeof(ui));
    namebox.buf = namebuf;
    namebox.cap = sizeof(namebuf);
    namebox.len = (int)strlen(namebuf);
    namebox.caret = namebox.len;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) == 0)
            continue;
        snprintf(namebuf, sizeof(namebuf), "%s", argv[i]);
        namebox.len = (int)strlen(namebuf);
        namebox.caret = namebox.len;
        had_file = 1;
    }

    canvas.px = malloc((unsigned long)CANVAS_W * CANVAS_H *
                       sizeof(uint32_t));
    if (!canvas.px) {
        printf("gpaint: out of memory for a %dx%d canvas\n", CANVAS_W,
               CANVAS_H);
        return 1;
    }
    canvas.wid = 0;
    canvas.open = 0;
    canvas.w = CANVAS_W;
    canvas.h = CANVAS_H;
    canvas.clip = gui_mkrc(0, 0, CANVAS_W, CANVAS_H);
    canvas_clear();

    win = gui_open("Paint", 60, 40, WIN_W, WIN_H, 0);
    if (!win) {
        printf("gpaint: no window manager available\n");
        free(canvas.px);
        return 1;
    }

    if (had_file && load_image(namebuf) == 0)
        snprintf(status, sizeof(status), "loaded %s", namebuf);
    else
        set_status_default();

    while (win->open) {
        int got = gui_next_event(win, &ev, 16);
        int drew = 0, bx = 0, by = 0, bw = 0, bh = 0;

        if (got < 0)
            break;
        gui_ui_begin(&ui);
        if (got > 0) {
            gui_ui_event(&ui, &ev);
            if (ev.type == KEV_CLOSE)
                break;

            if (ev.type == KEV_MOUSE_DOWN || ev.type == KEV_MOUSE_MOVE ||
                ev.type == KEV_MOUSE_UP) {
                int cxp = ev.x - CANVAS_X;
                int cyp = ev.y - CANVAS_Y;
                int inside = cxp >= 0 && cyp >= 0 && cxp < canvas.w &&
                             cyp < canvas.h;
                int px = last_x, py = last_y;

                if (ev.type == KEV_MOUSE_UP || !inside) {
                    last_x = -1;
                    last_y = -1;
                }
                if (inside && ((ev.buttons & (unsigned)K_MOUSE_LEFT) ||
                               ev.type == KEV_MOUSE_DOWN)) {
                    uint32_t c = eraser ? canvas_bg : colour;
                    int pad = brushes[brush] + 2;

                    if (px < 0) {
                        canvas_dot(cxp, cyp, c);
                        px = cxp;
                        py = cyp;
                    } else {
                        canvas_stroke(px, py, cxp, cyp, c);
                    }
                    last_x = cxp;
                    last_y = cyp;
                    bx = (px < cxp ? px : cxp) - pad;
                    by = (py < cyp ? py : cyp) - pad;
                    bw = (px > cxp ? px - cxp : cxp - px) + 2 * pad;
                    bh = (py > cyp ? py - cyp : cyp - py) + 2 * pad;
                    snprintf(status, sizeof(status),
                             "%d, %d   brush %d   %s", cxp, cyp,
                             brushes[brush], eraser ? "eraser" : "pen");
                    drew = 1;
                }
            }
            dirty = 1;
        } else if (namebox.focus) {
            dirty = 1;
        }

        /* While a stroke is in progress only the touched rectangle and
         * the status line are redrawn: the chrome cannot have changed
         * and repainting a third of a megapixel per mouse move would
         * make drawing feel like wading. */
        if (drew) {
            blit_canvas(win, bx, by, bw, bh);
            draw_status(win);
            if (gui_flush(win) != 0)
                break;
            dirty = 0;
            continue;
        }

        if (!dirty)
            continue;

        action = 0;
        full_repaint(win, &ui, &action);
        if (action == 1) {
            if (save_image(namebuf) == 0)
                snprintf(status, sizeof(status), "saved %s (%dx%d)",
                         namebuf, canvas.w, canvas.h);
            else
                snprintf(status, sizeof(status), "cannot write %s",
                         namebuf);
            draw_status(win);
        } else if (action == 2) {
            int rc = load_image(namebuf);

            if (rc == 0)
                snprintf(status, sizeof(status), "loaded %s", namebuf);
            else if (rc == -2)
                snprintf(status, sizeof(status), "%s is not a KPX1 image",
                         namebuf);
            else
                snprintf(status, sizeof(status), "cannot read %s", namebuf);
            blit_canvas(win, 0, 0, canvas.w, canvas.h);
            draw_status(win);
        } else if (action == 3) {
            canvas_clear();
            set_status_default();
            blit_canvas(win, 0, 0, canvas.w, canvas.h);
            draw_status(win);
        }

        if (gui_flush(win) != 0)
            break;
        dirty = 0;
    }

    gui_close(win);
    free(canvas.px);
    return 0;
}
