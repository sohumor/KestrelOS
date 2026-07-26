/* libgui: the widget set.
 *
 * Immediate mode: a widget is a function that draws itself from the state
 * the caller owns and reports what the user did to it this frame. There is
 * no retained tree, no layout engine and no callbacks - a frame is a
 * straight line of calls, which is why an app's paint routine reads like
 * the screen it produces.
 *
 * Input arrives as a gui_ui the caller folds events into (gui_ui_begin at
 * the top of the frame, gui_ui_event for each event). One-shot fields are
 * consumed by the first widget that uses them: a focused text box takes
 * the key, so a list box drawn afterwards will not also scroll on it.
 */

#include "gui.h"

#include <string.h>

#define DOUBLE_CLICK_MS 400
#define CARET_BLINK_MS  500

/* ------------------------------------------------------------ ui state */

void gui_ui_begin(gui_ui *ui)
{
    if (!ui)
        return;
    ui->down = 0;
    ui->up = 0;
    ui->dbl = 0;
    ui->key = 0;
    ui->wheel = 0;
    ui->down_btn = 0;
    ui->up_btn = 0;
}

void gui_ui_event(gui_ui *ui, const struct k_event *ev)
{
    if (!ui || !ev)
        return;

    switch (ev->type) {
    case KEV_MOUSE_MOVE:
        ui->mx = ev->x;
        ui->my = ev->y;
        ui->buttons = ev->buttons;
        break;
    case KEV_MOUSE_DOWN:
        ui->mx = ev->x;
        ui->my = ev->y;
        ui->buttons = ev->buttons;
        ui->down = 1;
        ui->down_x = ev->x;
        ui->down_y = ev->y;
        /* A compositor that reports only the transition leaves buttons
         * empty; treat that as the left button rather than nothing. */
        ui->down_btn = ev->buttons ? ev->buttons : (unsigned)K_MOUSE_LEFT;
        break;
    case KEV_MOUSE_UP: {
        unsigned long now = uptime_ms();
        int dx = ev->x - ui->last_click_x;
        int dy = ev->y - ui->last_click_y;

        ui->mx = ev->x;
        ui->my = ev->y;
        ui->buttons = ev->buttons;
        ui->up = 1;
        ui->up_x = ev->x;
        ui->up_y = ev->y;
        ui->up_btn = ev->buttons ? ev->buttons : ui->down_btn;
        if (ui->up_btn == 0)
            ui->up_btn = (unsigned)K_MOUSE_LEFT;
        if (dx < 0)
            dx = -dx;
        if (dy < 0)
            dy = -dy;
        if (ui->last_click_ms && now - ui->last_click_ms <= DOUBLE_CLICK_MS &&
            dx <= 4 && dy <= 4) {
            ui->dbl = 1;
            ui->last_click_ms = 0;      /* a triple click is not two doubles */
        } else {
            ui->last_click_ms = now ? now : 1;
        }
        ui->last_click_x = ev->x;
        ui->last_click_y = ev->y;
        break;
    }
    case KEV_KEY:
        ui->key = (int)ev->key;
        break;
#ifdef KEV_MOUSE_WHEEL
    case KEV_MOUSE_WHEEL:
        ui->mx = ev->x;
        ui->my = ev->y;
        ui->wheel += (int)ev->key;
        break;
#endif
    default:
        break;
    }
}

int gui_hit(gui_rc r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static int left_up(const gui_ui *ui)
{
    return ui->up && (ui->up_btn & (unsigned)K_MOUSE_LEFT);
}

/* A click "belongs" to a widget only if press and release both landed in
 * it, so dragging out of a button cancels it the way it should. */
static int clicked_in(const gui_ui *ui, gui_rc r)
{
    return left_up(ui) && gui_hit(r, ui->up_x, ui->up_y) &&
           gui_hit(r, ui->down_x, ui->down_y);
}

static int held_in(const gui_ui *ui, gui_rc r)
{
    return (ui->buttons & (unsigned)K_MOUSE_LEFT) &&
           gui_hit(r, ui->down_x, ui->down_y) && gui_hit(r, ui->mx, ui->my);
}

/* -------------------------------------------------------------- buttons */

int gui_button_ex(gui_window *win, gui_rc r, const char *label, gui_ui *ui,
                  int enabled, int active)
{
    uint32_t body, text;
    int hover, held, tx, ty;

    if (!win || !ui || r.w <= 0 || r.h <= 0)
        return 0;
    hover = gui_hit(r, ui->mx, ui->my);
    held = enabled && held_in(ui, r);

    if (!enabled)
        body = gui_shade(GUI_PANEL, -8);
    else if (active)
        body = GUI_ACCENT_DARK;
    else if (held)
        body = gui_shade(GUI_PANEL, -14);
    else if (hover)
        body = GUI_PANEL_HI;
    else
        body = GUI_PANEL;

    text = enabled ? (active ? GUI_WHITE : GUI_TEXT) : GUI_TEXT_DIM;

    gui_rect(win, r.x, r.y, r.w, r.h, body);
    if (held)
        gui_bevel(win, r, gui_shade(body, -25), gui_shade(body, 12));
    else
        gui_bevel(win, r, gui_shade(body, 16), gui_shade(body, -28));
    gui_frame(win, r.x, r.y, r.w, r.h, GUI_BORDER);

    if (label) {
        tx = r.x + (r.w - gui_text_w(label)) / 2;
        ty = r.y + (r.h - GUI_FONT_H) / 2;
        if (held) {
            tx++;
            ty++;
        }
        gui_clip(win, gui_mkrc(r.x + 2, r.y, r.w - 4, r.h));
        gui_text(win, tx, ty, label, text, GUI_TRANSPARENT);
        gui_unclip(win);
    }
    return enabled && clicked_in(ui, r);
}

int gui_button(gui_window *win, gui_rc r, const char *label, gui_ui *ui)
{
    return gui_button_ex(win, r, label, ui, 1, 0);
}

void gui_label(gui_window *win, int x, int y, const char *s, uint32_t fg)
{
    gui_text(win, x, y, s, fg, GUI_TRANSPARENT);
}

/* ------------------------------------------------------------- text box */

static void tb_insert(gui_textbox_state *tb, char c)
{
    int i;

    if (tb->len + 1 >= tb->cap)
        return;
    for (i = tb->len; i > tb->caret; i--)
        tb->buf[i] = tb->buf[i - 1];
    tb->buf[tb->caret] = c;
    tb->len++;
    tb->caret++;
    tb->buf[tb->len] = '\0';
}

static void tb_erase(gui_textbox_state *tb, int at)
{
    int i;

    if (at < 0 || at >= tb->len)
        return;
    for (i = at; i < tb->len - 1; i++)
        tb->buf[i] = tb->buf[i + 1];
    tb->len--;
    tb->buf[tb->len] = '\0';
}

int gui_textbox(gui_window *win, gui_rc r, gui_textbox_state *tb, gui_ui *ui)
{
    int inner_w, visible, entered = 0, i, tx;
    unsigned long now;

    if (!win || !tb || !ui || !tb->buf || tb->cap < 2)
        return 0;
    if (tb->len > tb->cap - 1)
        tb->len = tb->cap - 1;
    tb->buf[tb->len] = '\0';
    if (tb->caret > tb->len)
        tb->caret = tb->len;
    if (tb->caret < 0)
        tb->caret = 0;

    inner_w = r.w - 8;
    if (inner_w < GUI_FONT_W)
        inner_w = GUI_FONT_W;
    visible = inner_w / GUI_FONT_W;

    if (ui->down) {
        if (gui_hit(r, ui->down_x, ui->down_y)) {
            int col = (ui->down_x - r.x - 4 + GUI_FONT_W / 2) / GUI_FONT_W;

            tb->focus = 1;
            tb->caret = tb->scroll + col;
            if (tb->caret > tb->len)
                tb->caret = tb->len;
            if (tb->caret < 0)
                tb->caret = 0;
        } else {
            tb->focus = 0;
        }
    }

    if (tb->focus && ui->key) {
        int k = ui->key;

        switch (k) {
        case '\n':
        case '\r':
            entered = 1;
            break;
        case 8:
        case 127:
            if (tb->caret > 0) {
                tb_erase(tb, tb->caret - 1);
                tb->caret--;
            }
            break;
        case KEY_DELETE:
            tb_erase(tb, tb->caret);
            break;
        case KEY_LEFT:
            if (tb->caret > 0)
                tb->caret--;
            break;
        case KEY_RIGHT:
            if (tb->caret < tb->len)
                tb->caret++;
            break;
        case KEY_HOME:
        case 1:                         /* ctrl-A */
            tb->caret = 0;
            break;
        case KEY_END:
        case 5:                         /* ctrl-E */
            tb->caret = tb->len;
            break;
        case 21:                        /* ctrl-U: clear the line */
            tb->len = 0;
            tb->caret = 0;
            tb->buf[0] = '\0';
            break;
        default:
            if (k >= 32 && k < 127)
                tb_insert(tb, (char)k);
            break;
        }
        ui->key = 0;                    /* consumed */
    }

    if (tb->caret < tb->scroll)
        tb->scroll = tb->caret;
    if (tb->caret > tb->scroll + visible - 1)
        tb->scroll = tb->caret - visible + 1;
    if (tb->scroll < 0)
        tb->scroll = 0;

    gui_rect(win, r.x, r.y, r.w, r.h, GUI_SUNKEN);
    gui_bevel(win, r, gui_shade(GUI_SUNKEN, -30), gui_shade(GUI_SUNKEN, 20));
    gui_frame(win, r.x, r.y, r.w, r.h,
              tb->focus ? GUI_ACCENT : GUI_BORDER);

    gui_clip(win, gui_mkrc(r.x + 3, r.y + 1, r.w - 6, r.h - 2));
    tx = r.x + 4;
    for (i = tb->scroll; i < tb->len && i < tb->scroll + visible; i++)
        tx = gui_char(win, tx, r.y + (r.h - GUI_FONT_H) / 2, tb->buf[i],
                      GUI_TEXT, GUI_TRANSPARENT);

    now = uptime_ms();
    if (tb->focus && ((now / CARET_BLINK_MS) & 1) == 0) {
        int cx = r.x + 4 + (tb->caret - tb->scroll) * GUI_FONT_W;

        gui_rect(win, cx, r.y + (r.h - GUI_FONT_H) / 2, 1, GUI_FONT_H,
                 GUI_ACCENT);
    }
    gui_unclip(win);
    return entered;
}

/* ----------------------------------------------------------- scrollbar */

int gui_scrollbar(gui_window *win, gui_rc r, int total, int visible,
                  int *top, gui_ui *ui)
{
    int max_top, thumb_h, thumb_y, changed = 0, old;

    if (!win || !top || !ui || r.h <= 0)
        return 0;
    if (visible < 1)
        visible = 1;
    max_top = total - visible;
    if (max_top < 0)
        max_top = 0;
    old = *top;
    if (*top > max_top)
        *top = max_top;
    if (*top < 0)
        *top = 0;

    gui_rect(win, r.x, r.y, r.w, r.h, gui_shade(GUI_SUNKEN, -10));
    gui_frame(win, r.x, r.y, r.w, r.h, GUI_BORDER);

    if (max_top == 0) {
        gui_rect(win, r.x + 2, r.y + 2, r.w - 4, r.h - 4, GUI_PANEL);
        return *top != old;
    }

    thumb_h = (r.h * visible) / (total > 0 ? total : 1);
    if (thumb_h < 12)
        thumb_h = 12;
    if (thumb_h > r.h)
        thumb_h = r.h;

    /* Track for as long as the button is down and the press started in
     * the bar, even if the pointer has wandered off it sideways - that
     * is what dragging a scrollbar is supposed to feel like. */
    if ((ui->buttons & (unsigned)K_MOUSE_LEFT) &&
        gui_hit(r, ui->down_x, ui->down_y)) {
        int travel = r.h - thumb_h;
        int pos = ui->my - r.y - thumb_h / 2;

        if (travel < 1)
            travel = 1;
        if (pos < 0)
            pos = 0;
        if (pos > travel)
            pos = travel;
        *top = (pos * max_top + travel / 2) / travel;
        changed = 1;
    }

    thumb_y = r.y + ((r.h - thumb_h) * (*top)) / max_top;
    gui_rect(win, r.x + 2, thumb_y + 1, r.w - 4, thumb_h - 2, GUI_PANEL_HI);
    gui_bevel(win, gui_mkrc(r.x + 2, thumb_y + 1, r.w - 4, thumb_h - 2),
              gui_shade(GUI_PANEL_HI, 18), gui_shade(GUI_PANEL_HI, -25));
    return changed || *top != old;
}

/* ------------------------------------------------------------- list box */

int gui_listbox(gui_window *win, gui_rc r, gui_list *ls, gui_ui *ui)
{
    int row_h, visible, i, activated = -1, bar_w = 12;
    gui_rc rows;

    if (!win || !ls || !ui || r.w <= 0 || r.h <= 0)
        return -1;
    row_h = ls->row_h > 0 ? ls->row_h : GUI_FONT_H + 4;
    ls->row_h = row_h;

    gui_rect(win, r.x, r.y, r.w, r.h, GUI_SUNKEN);
    gui_frame(win, r.x, r.y, r.w, r.h, GUI_BORDER);

    rows = gui_mkrc(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    visible = rows.h / row_h;
    if (visible < 1)
        visible = 1;

    if (ls->count > visible) {
        rows.w -= bar_w;
        gui_scrollbar(win, gui_mkrc(r.x + r.w - bar_w - 1, r.y + 1,
                                    bar_w, r.h - 2),
                      ls->count, visible, &ls->top, ui);
    } else {
        ls->top = 0;
    }

    if (ui->wheel && gui_hit(r, ui->mx, ui->my)) {
        ls->top -= ui->wheel * 3;
        ui->wheel = 0;
    }
    if (ls->top > ls->count - visible)
        ls->top = ls->count - visible;
    if (ls->top < 0)
        ls->top = 0;

    /* Selection and activation. */
    if (ui->down && gui_hit(rows, ui->down_x, ui->down_y)) {
        int idx = ls->top + (ui->down_y - rows.y) / row_h;

        if (idx >= 0 && idx < ls->count) {
            ls->sel = idx;
            if (ui->dbl)
                activated = idx;
        }
    }
    if (ui->dbl && gui_hit(rows, ui->up_x, ui->up_y)) {
        int idx = ls->top + (ui->up_y - rows.y) / row_h;

        if (idx >= 0 && idx < ls->count)
            activated = idx;
    }

    if (ui->key) {
        int handled = 1;

        switch (ui->key) {
        case KEY_UP:
            if (ls->sel > 0)
                ls->sel--;
            break;
        case KEY_DOWN:
            if (ls->sel < ls->count - 1)
                ls->sel++;
            break;
        case KEY_PGUP:
            ls->sel -= visible;
            break;
        case KEY_PGDN:
            ls->sel += visible;
            break;
        case KEY_HOME:
            ls->sel = 0;
            break;
        case KEY_END:
            ls->sel = ls->count - 1;
            break;
        case '\n':
        case '\r':
            if (ls->sel >= 0 && ls->sel < ls->count)
                activated = ls->sel;
            break;
        default:
            handled = 0;
            break;
        }
        if (handled) {
            if (ls->sel < 0)
                ls->sel = 0;
            if (ls->sel > ls->count - 1)
                ls->sel = ls->count - 1;
            ui->key = 0;
        }
    }

    /* Keep the selection on screen. */
    if (ls->sel >= 0) {
        if (ls->sel < ls->top)
            ls->top = ls->sel;
        if (ls->sel >= ls->top + visible)
            ls->top = ls->sel - visible + 1;
        if (ls->top < 0)
            ls->top = 0;
    }

    gui_clip(win, rows);
    for (i = 0; i < visible; i++) {
        int idx = ls->top + i;
        int y = rows.y + i * row_h;
        const char *text;
        uint32_t fg;

        if (idx >= ls->count)
            break;
        text = ls->item ? ls->item(ls->ctx, idx) : "";
        if (!text)
            text = "";
        fg = ls->tint ? ls->tint(ls->ctx, idx) : GUI_TEXT;
        if (idx == ls->sel) {
            gui_rect(win, rows.x, y, rows.w, row_h, GUI_ACCENT_DARK);
            fg = GUI_WHITE;
        } else if (idx & 1) {
            gui_rect(win, rows.x, y, rows.w, row_h,
                     gui_shade(GUI_SUNKEN, 4));
        }
        gui_text(win, rows.x + 4, y + (row_h - GUI_FONT_H) / 2, text, fg,
                 GUI_TRANSPARENT);
    }
    gui_unclip(win);
    return activated;
}

/* -------------------------------------------------------------- checkbox */

int gui_checkbox(gui_window *win, int x, int y, const char *label, int *on,
                 gui_ui *ui)
{
    gui_rc box = gui_mkrc(x, y, 16, 16);
    gui_rc hot;
    int toggled = 0;

    if (!win || !on || !ui)
        return 0;
    hot = gui_mkrc(x, y, 16 + 6 + gui_text_w(label), 16);

    if (clicked_in(ui, hot)) {
        *on = !*on;
        toggled = 1;
    }

    gui_rect(win, box.x, box.y, box.w, box.h, GUI_SUNKEN);
    gui_bevel(win, box, gui_shade(GUI_SUNKEN, -30), gui_shade(GUI_SUNKEN, 20));
    gui_frame(win, box.x, box.y, box.w, box.h, GUI_BORDER);
    if (*on) {
        gui_line_w(win, box.x + 4, box.y + 8, box.x + 7, box.y + 11,
                   GUI_ACCENT, 2);
        gui_line_w(win, box.x + 7, box.y + 11, box.x + 12, box.y + 4,
                   GUI_ACCENT, 2);
    }
    if (label)
        gui_text(win, x + 22, y + (16 - GUI_FONT_H) / 2, label, GUI_TEXT,
                 GUI_TRANSPARENT);
    return toggled;
}

/* ------------------------------------------------------------------ menu */

int gui_menu(gui_window *win, gui_rc r, const char *title,
             const char *const *items, int n, gui_menu_state *st,
             gui_ui *ui)
{
    int i, width = r.w, chosen = -1, item_h = GUI_FONT_H + 6;
    gui_rc drop;

    if (!win || !st || !ui || n < 0)
        return -1;

    if (gui_button_ex(win, r, title, ui, 1, st->open))
        st->open = !st->open;

    if (!st->open)
        return -1;

    for (i = 0; i < n; i++) {
        int w = gui_text_w(items[i]) + 24;

        if (w > width)
            width = w;
    }
    drop = gui_mkrc(r.x, r.y + r.h, width, n * item_h + 4);

    /* A click anywhere else dismisses the menu. */
    if (ui->down && !gui_hit(drop, ui->down_x, ui->down_y) &&
        !gui_hit(r, ui->down_x, ui->down_y))
        st->open = 0;

    gui_rect(win, drop.x + 3, drop.y + 3, drop.w, drop.h,
             gui_shade(GUI_BORDER, -40));
    gui_rect(win, drop.x, drop.y, drop.w, drop.h, GUI_PANEL);
    gui_frame(win, drop.x, drop.y, drop.w, drop.h, GUI_BORDER);
    gui_bevel(win, gui_mkrc(drop.x + 1, drop.y + 1, drop.w - 2, drop.h - 2),
              GUI_EDGE, gui_shade(GUI_PANEL, -20));

    st->hover = -1;
    for (i = 0; i < n; i++) {
        gui_rc row = gui_mkrc(drop.x + 2, drop.y + 2 + i * item_h,
                              drop.w - 4, item_h);
        uint32_t fg = GUI_TEXT;

        if (gui_hit(row, ui->mx, ui->my)) {
            gui_rect(win, row.x, row.y, row.w, row.h, GUI_ACCENT_DARK);
            fg = GUI_WHITE;
            st->hover = i;
        }
        gui_text(win, row.x + 8, row.y + (row.h - GUI_FONT_H) / 2, items[i],
                 fg, GUI_TRANSPARENT);
        if (clicked_in(ui, row)) {
            chosen = i;
            st->open = 0;
        }
    }
    return chosen;
}
