/* gcalc.c - a compact 64-bit integer desktop calculator. */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#include "gui.h"

#define WIN_W 356
#define WIN_H 474

struct calc_state {
    char input[48];
    int len;
    long lhs;
    char op;
    int have_lhs;
    int replace;
    char history[80];
    char error[48];
};

static int parse_value(const char *s, long *out)
{
    unsigned long value = 0;
    unsigned long limit;
    int negative = 0, digits = 0;

    if (*s == '-') {
        negative = 1;
        s++;
    }
    limit = negative ? (unsigned long)__LONG_MAX__ + 1UL
                     : (unsigned long)__LONG_MAX__;
    while (*s) {
        unsigned digit;

        if (*s < '0' || *s > '9')
            return -1;
        digit = (unsigned)(*s++ - '0');
        if (value > (limit - digit) / 10)
            return -1;
        value = value * 10 + digit;
        digits++;
    }
    if (!digits)
        return -1;
    if (negative && value == (unsigned long)__LONG_MAX__ + 1UL)
        *out = (-__LONG_MAX__ - 1L);
    else
        *out = negative ? -(long)value : (long)value;
    return 0;
}

static void set_value(struct calc_state *st, long value)
{
    snprintf(st->input, sizeof(st->input), "%ld", value);
    st->len = (int)strlen(st->input);
}

static int calculate(long a, long b, char op, long *out)
{
    switch (op) {
    case '+':
        return __builtin_add_overflow(a, b, out) ? -1 : 0;
    case '-':
        return __builtin_sub_overflow(a, b, out) ? -1 : 0;
    case '*':
        return __builtin_mul_overflow(a, b, out) ? -1 : 0;
    case '/':
        if (b == 0)
            return -2;
        if (a == (-__LONG_MAX__ - 1L) && b == -1)
            return -1;
        *out = a / b;
        return 0;
    default:
        *out = b;
        return 0;
    }
}

static void clear(struct calc_state *st)
{
    memset(st, 0, sizeof(*st));
    strcpy(st->input, "0");
    st->len = 1;
    strcpy(st->history, "Ready");
}

static void digit(struct calc_state *st, char c)
{
    if (st->replace || (st->len == 1 && st->input[0] == '0')) {
        st->len = 0;
        st->input[0] = '\0';
        st->replace = 0;
    }
    if (st->len < (int)sizeof(st->input) - 1) {
        st->input[st->len++] = c;
        st->input[st->len] = '\0';
    }
    st->error[0] = '\0';
}

static void apply_operator(struct calc_state *st, char op)
{
    long value, result;
    int rc;

    if (parse_value(st->input, &value) != 0) {
        strcpy(st->error, "Invalid number");
        return;
    }
    if (st->have_lhs && !st->replace) {
        rc = calculate(st->lhs, value, st->op, &result);
        if (rc != 0) {
            strcpy(st->error, rc == -2 ? "Cannot divide by zero"
                                      : "Integer overflow");
            return;
        }
        snprintf(st->history, sizeof(st->history), "%ld %c %ld",
                 st->lhs, st->op, value);
        st->lhs = result;
        set_value(st, result);
    } else {
        st->lhs = value;
    }
    st->op = op;
    st->have_lhs = 1;
    st->replace = 1;
    snprintf(st->history, sizeof(st->history), "%ld %c", st->lhs, op);
}

static void equals(struct calc_state *st)
{
    long rhs, result;
    int rc;

    if (!st->have_lhs || parse_value(st->input, &rhs) != 0)
        return;
    rc = calculate(st->lhs, rhs, st->op, &result);
    if (rc != 0) {
        strcpy(st->error, rc == -2 ? "Cannot divide by zero"
                                  : "Integer overflow");
        return;
    }
    snprintf(st->history, sizeof(st->history), "%ld %c %ld =",
             st->lhs, st->op, rhs);
    set_value(st, result);
    st->have_lhs = 0;
    st->op = 0;
    st->replace = 1;
    st->error[0] = '\0';
}

static void press(struct calc_state *st, const char *key)
{
    if (key[0] >= '0' && key[0] <= '9' && !key[1]) {
        digit(st, key[0]);
    } else if (!strcmp(key, "00")) {
        digit(st, '0');
        digit(st, '0');
    } else if (!strcmp(key, "C")) {
        clear(st);
    } else if (!strcmp(key, "Back")) {
        if (st->replace) {
            strcpy(st->input, "0");
            st->len = 1;
            st->replace = 0;
        } else if (st->len > 1) {
            st->input[--st->len] = '\0';
        } else {
            strcpy(st->input, "0");
        }
    } else if (!strcmp(key, "+/-")) {
        if (st->input[0] == '-') {
            memmove(st->input, st->input + 1, (unsigned long)st->len);
            st->len--;
        } else if (strcmp(st->input, "0") && st->len + 1 <
                   (int)sizeof(st->input)) {
            memmove(st->input + 1, st->input,
                    (unsigned long)st->len + 1);
            st->input[0] = '-';
            st->len++;
        }
    } else if (!strcmp(key, "=")) {
        equals(st);
    } else {
        apply_operator(st, key[0]);
    }
}

static void paint(gui_window *win, gui_ui *ui, struct calc_state *st)
{
    static const char *keys[20] = {
        "C", "Back", "/", "*",
        "7", "8", "9", "-",
        "4", "5", "6", "+",
        "1", "2", "3", "=",
        "+/-", "0", "00", "="
    };
    int margin = 14, gap = 8;
    int bw = (WIN_W - 2 * margin - 3 * gap) / 4;
    int bh = 48;

    gui_clear(win, GUI_SURFACE);
    gui_hgradient(win, gui_mkrc(0, 0, WIN_W, 42),
                  gui_mix(GUI_PANEL, GUI_ACCENT, 30), GUI_PANEL);
    gui_icon(win, 14, 10, 22, GUI_ICON_CALCULATOR, GUI_ACCENT);
    gui_text(win, 46, 13, "Calculator", GUI_TEXT, GUI_TRANSPARENT);

    gui_card(win, gui_mkrc(margin, 56, WIN_W - 2 * margin, 104));
    gui_clip(win, gui_mkrc(margin + 14, 62, WIN_W - 2 * margin - 28, 84));
    gui_text(win, margin + 14, 70, st->history, GUI_TEXT_DIM,
             GUI_TRANSPARENT);
    gui_text(win, WIN_W - margin - 14 - gui_text_w(st->input), 108,
             st->input, GUI_TEXT, GUI_TRANSPARENT);
    gui_unclip(win);
    if (st->error[0])
        gui_badge(win, gui_mkrc(margin + 12, 164,
                                 gui_text_w(st->error) + 24, 24),
                  st->error, GUI_ERROR);

    for (int i = 0; i < 20; i++) {
        int col = i % 4, row = i / 4;
        gui_rc r = gui_mkrc(margin + col * (bw + gap),
                            184 + row * (bh + 7), bw, bh);
        int accent = !strcmp(keys[i], "=") || !strcmp(keys[i], "+") ||
                     !strcmp(keys[i], "-") || !strcmp(keys[i], "*") ||
                     !strcmp(keys[i], "/");

        /* The second equals occupies a normal cell; both deliberately invoke
         * the same operation so the grid remains keyboard-like and regular. */
        if (gui_button_ex(win, r, keys[i], ui, 1, accent))
            press(st, keys[i]);
    }
}

int main(int argc, char **argv)
{
    gui_window *win;
    gui_ui ui;
    struct k_event ev;
    struct calc_state state;
    int dirty = 1;

    (void)argc;
    (void)argv;
    memset(&ui, 0, sizeof(ui));
    clear(&state);
    win = gui_open("Calculator", 320, 140, WIN_W, WIN_H, 0);
    if (!win) {
        printf("gcalc: no window manager available\n");
        return 1;
    }

    while (win->open) {
        int got = gui_next_event(win, &ev, 80);

        if (got < 0)
            break;
        gui_ui_begin(&ui);
        if (got > 0) {
            gui_ui_event(&ui, &ev);
            if (ev.type == KEV_CLOSE)
                break;
            if (ev.type == KEV_KEY) {
                int k = (int)ev.key;

                if (k >= '0' && k <= '9') {
                    char key[2] = { (char)k, '\0' };
                    press(&state, key);
                } else if (k == '+' || k == '-' || k == '*' || k == '/') {
                    char key[2] = { (char)k, '\0' };
                    press(&state, key);
                } else if (k == '\n' || k == '\r' || k == '=') {
                    press(&state, "=");
                } else if (k == 8 || k == 127 || k == KEY_DELETE) {
                    press(&state, "Back");
                } else if (k == 27) {
                    clear(&state);
                }
            }
            dirty = 1;
        }
        if (!dirty)
            continue;
        paint(win, &ui, &state);
        if (gui_flush(win) != 0)
            break;
        dirty = 0;
    }
    gui_close(win);
    return 0;
}
