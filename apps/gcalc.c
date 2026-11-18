/* gcalc.c - KestrelOS 64-bit desktop calculator (Standard & Programmer).
 *
 * Features:
 *   - Standard mode (64-bit decimal arithmetic)
 *   - Programmer mode (live DEC / HEX / BIN readouts, clickable radix
 *     pills, hex digits A-F, bitwise << >> & | ^ ~ and % arithmetic)
 *   - Talon clipboard integration: [Copy] pushes K_PARCEL_NUMBER (Ctrl+C),
 *     [Paste] reads K_PARCEL_NUMBER / K_PARCEL_TEXT (Ctrl+V)
 *   - Dynamic KEV_RESIZE layout reflow
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#include "gui.h"

#define INIT_W 424
#define INIT_H 568

enum calc_mode {
    MODE_STD = 0,
    MODE_PROG = 1
};

enum calc_base {
    BASE_DEC = 10,
    BASE_HEX = 16,
    BASE_BIN = 2
};

struct calc_state {
    char input[80];
    int len;
    long lhs;
    char op;
    int have_lhs;
    int replace;
    int mode;
    int base;
    char history[96];
    char error[48];
    char toast[64];
    unsigned long toast_until;
};

/* ----------------------------------------------------------- conversions */

static int digit_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

static int parse_value_base(const char *s, int base, long *out)
{
    unsigned long value = 0;
    int negative = 0, digits = 0;

    if (!s || !*s)
        return -1;
    if (base == BASE_DEC) {
        unsigned long limit;
        if (*s == '-') {
            negative = 1;
            s++;
        } else if (*s == '+') {
            s++;
        }
        limit = negative ? (unsigned long)__LONG_MAX__ + 1UL
                         : (unsigned long)__LONG_MAX__;
        while (*s) {
            int d = digit_val(*s++);
            if (d < 0 || d >= 10)
                return -1;
            if (value > (limit - (unsigned long)d) / 10UL)
                return -1;
            value = value * 10UL + (unsigned long)d;
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

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && base == BASE_HEX)
        s += 2;
    else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B') && base == BASE_BIN)
        s += 2;

    while (*s) {
        int d = digit_val(*s++);
        if (d < 0 || d >= base)
            return -1;
        if (base == BASE_HEX) {
            if (digits >= 16)
                return -1;
            value = (value << 4) | (unsigned long)d;
        } else {
            if (digits >= 64)
                return -1;
            value = (value << 1) | (unsigned long)d;
        }
        digits++;
    }
    if (!digits)
        return -1;
    *out = (long)value;
    return 0;
}

static void format_value_base(long v, int base, char *out, int cap)
{
    if (base == BASE_DEC) {
        snprintf(out, (unsigned long)cap, "%ld", v);
    } else if (base == BASE_HEX) {
        snprintf(out, (unsigned long)cap, "%lX", (unsigned long)v);
    } else {
        unsigned long u = (unsigned long)v;
        char tmp[68];
        int n = 0, pos = 0;

        if (u == 0) {
            snprintf(out, (unsigned long)cap, "0");
            return;
        }
        while (u > 0 && n < 64) {
            tmp[n++] = (u & 1UL) ? '1' : '0';
            u >>= 1;
        }
        while (n > 0 && pos + 1 < cap)
            out[pos++] = tmp[--n];
        out[pos] = '\0';
    }
}

static void format_bin_grouped(unsigned long u, char *out, int cap)
{
    int bits = 8;
    int pos = 0;

    if (u > 0xFFFFFFFFUL)
        bits = 64;
    else if (u > 0xFFFFUL)
        bits = 32;
    else if (u > 0xFFUL)
        bits = 16;

    if (bits > 32 && cap < 82)
        bits = 32;

    for (int i = bits - 1; i >= 0 && pos + 2 < cap; i--) {
        out[pos++] = ((u >> i) & 1UL) ? '1' : '0';
        if (i > 0 && (i % 4) == 0 && pos + 2 < cap)
            out[pos++] = ' ';
    }
    out[pos] = '\0';
}

static void set_value(struct calc_state *st, long value)
{
    int base = (st->mode == MODE_PROG) ? st->base : BASE_DEC;

    format_value_base(value, base, st->input, (int)sizeof(st->input));
    st->len = (int)strlen(st->input);
}

static const char *op_label(char op)
{
    switch (op) {
    case '<': return "<<";
    case '>': return ">>";
    default: {
        static char buf[2];
        buf[0] = op;
        buf[1] = '\0';
        return buf;
    }
    }
}

/* ----------------------------------------------------------- arithmetic */

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
    case '%':
        if (b == 0)
            return -2;
        if (a == (-__LONG_MAX__ - 1L) && b == -1) {
            *out = 0;
            return 0;
        }
        *out = a % b;
        return 0;
    case '&':
        *out = (long)((uint64_t)a & (uint64_t)b);
        return 0;
    case '|':
        *out = (long)((uint64_t)a | (uint64_t)b);
        return 0;
    case '^':
        *out = (long)((uint64_t)a ^ (uint64_t)b);
        return 0;
    case '<':
        if (b < 0 || b >= 64)
            return -3;
        *out = (long)((uint64_t)a << (unsigned)b);
        return 0;
    case '>':
        if (b < 0 || b >= 64)
            return -3;
        *out = (long)((uint64_t)a >> (unsigned)b);
        return 0;
    default:
        *out = b;
        return 0;
    }
}

static void set_calc_error(struct calc_state *st, int rc)
{
    if (rc == -2)
        strcpy(st->error, "Cannot divide by zero");
    else if (rc == -3)
        strcpy(st->error, "Shift must be 0..63");
    else
        strcpy(st->error, "Integer overflow");
}

static void clear_state(struct calc_state *st, int keep_mode)
{
    int mode = keep_mode ? st->mode : MODE_STD;
    int base = keep_mode ? st->base : BASE_DEC;

    memset(st, 0, sizeof(*st));
    st->mode = mode;
    st->base = base;
    strcpy(st->input, "0");
    st->len = 1;
    strcpy(st->history, "Ready");
}

static void switch_mode(struct calc_state *st, int new_mode)
{
    long cur = 0;
    int cur_base = (st->mode == MODE_PROG) ? st->base : BASE_DEC;

    if (parse_value_base(st->input, cur_base, &cur) != 0)
        cur = 0;
    st->mode = new_mode;
    if (new_mode == MODE_STD)
        st->base = BASE_DEC;
    set_value(st, cur);
    st->error[0] = '\0';
}

static void switch_base(struct calc_state *st, int new_base)
{
    long cur = 0;

    if (st->mode != MODE_PROG || st->base == new_base)
        return;
    if (parse_value_base(st->input, st->base, &cur) != 0)
        cur = 0;
    st->base = new_base;
    set_value(st, cur);
    st->replace = 1;
    st->error[0] = '\0';
}

static void digit_input(struct calc_state *st, char c)
{
    int active_base = (st->mode == MODE_PROG) ? st->base : BASE_DEC;
    int dv = digit_val(c);
    int max_len;

    if (dv < 0 || dv >= active_base)
        return;
    if (c >= 'a' && c <= 'f')
        c = (char)(c - ('a' - 'A'));

    max_len = (active_base == BASE_BIN) ? 64
            : (active_base == BASE_HEX) ? 16 : 22;

    if (st->replace || (st->len == 1 && st->input[0] == '0')) {
        st->len = 0;
        st->input[0] = '\0';
        st->replace = 0;
    }
    if (st->len < max_len && st->len < (int)sizeof(st->input) - 1) {
        st->input[st->len++] = c;
        st->input[st->len] = '\0';
    }
    st->error[0] = '\0';
}

static void apply_operator(struct calc_state *st, char op)
{
    long value, result;
    int active_base = (st->mode == MODE_PROG) ? st->base : BASE_DEC;
    char lhs_s[32];

    if (parse_value_base(st->input, active_base, &value) != 0) {
        strcpy(st->error, "Invalid number");
        return;
    }
    if (st->have_lhs && !st->replace) {
        int rc = calculate(st->lhs, value, st->op, &result);
        if (rc != 0) {
            set_calc_error(st, rc);
            return;
        }
        st->lhs = result;
        set_value(st, result);
    } else {
        st->lhs = value;
    }
    st->op = op;
    st->have_lhs = 1;
    st->replace = 1;
    format_value_base(st->lhs, active_base, lhs_s, (int)sizeof(lhs_s));
    snprintf(st->history, sizeof(st->history), "%s %s", lhs_s, op_label(op));
}

static void equals_op(struct calc_state *st)
{
    long rhs, result;
    int active_base = (st->mode == MODE_PROG) ? st->base : BASE_DEC;
    char lhs_s[32], rhs_s[32];
    int rc;

    if (!st->have_lhs || parse_value_base(st->input, active_base, &rhs) != 0)
        return;
    rc = calculate(st->lhs, rhs, st->op, &result);
    if (rc != 0) {
        set_calc_error(st, rc);
        return;
    }
    format_value_base(st->lhs, active_base, lhs_s, (int)sizeof(lhs_s));
    format_value_base(rhs, active_base, rhs_s, (int)sizeof(rhs_s));
    snprintf(st->history, sizeof(st->history), "%s %s %s =",
             lhs_s, op_label(st->op), rhs_s);
    set_value(st, result);
    st->have_lhs = 0;
    st->op = 0;
    st->replace = 1;
    st->error[0] = '\0';
}

static void press(struct calc_state *st, const char *key)
{
    int active_base = (st->mode == MODE_PROG) ? st->base : BASE_DEC;

    if (((key[0] >= '0' && key[0] <= '9') ||
         (key[0] >= 'A' && key[0] <= 'F')) && !key[1]) {
        digit_input(st, key[0]);
    } else if (!strcmp(key, "00")) {
        digit_input(st, '0');
        digit_input(st, '0');
    } else if (!strcmp(key, "CLR") || (!strcmp(key, "C") && st->mode == MODE_STD)) {
        clear_state(st, 1);
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
        long v = 0;
        if (parse_value_base(st->input, active_base, &v) == 0) {
            if (v == (-__LONG_MAX__ - 1L))
                strcpy(st->error, "Integer overflow");
            else
                set_value(st, -v);
        }
    } else if (!strcmp(key, "~")) {
        long v = 0;
        if (parse_value_base(st->input, active_base, &v) == 0) {
            char prev[80];
            snprintf(prev, sizeof(prev), "%s", st->input);
            set_value(st, (long)(~(uint64_t)v));
            st->replace = 1;
            snprintf(st->history, sizeof(st->history), "~(%s)", prev);
        }
    } else if (!strcmp(key, "=")) {
        equals_op(st);
    } else if (!strcmp(key, "<<")) {
        apply_operator(st, '<');
    } else if (!strcmp(key, ">>")) {
        apply_operator(st, '>');
    } else {
        apply_operator(st, key[0]);
    }
}

/* ------------------------------------------------------- Talon clipboard */

static void copy_to_talon(struct calc_state *st)
{
    long v = 0;
    int active_base = (st->mode == MODE_PROG) ? st->base : BASE_DEC;
    char dec[32], label[48];

    if (parse_value_base(st->input, active_base, &v) != 0) {
        strcpy(st->error, "Invalid number");
        return;
    }
    snprintf(dec, sizeof(dec), "%ld", v);
    if (st->mode == MODE_PROG)
        snprintf(label, sizeof(label), "Calc (0x%lX)", (unsigned long)v);
    else
        snprintf(label, sizeof(label), "Calculator");

    if (parcel_put(K_PARCEL_NUMBER, "text/x-number", label, dec,
                   (uint32_t)strlen(dec)) >= 0) {
        snprintf(st->toast, sizeof(st->toast), "Copied %s to Talon", dec);
        st->toast_until = uptime_ms() + 2500;
    } else {
        strcpy(st->error, "Talon copy failed");
    }
}

static void paste_from_talon(struct calc_state *st)
{
    struct k_parcel_info info;
    char buf[80];
    int found = 0;
    long v = 0;

    for (int slot = 0; slot < K_PARCEL_MAX_SLOTS; slot++) {
        long n;
        if (parcel_list(slot, &info) != 0)
            break;
        if (info.type != K_PARCEL_NUMBER && info.type != K_PARCEL_TEXT)
            continue;
        if (info.size == 0 || info.size >= sizeof(buf))
            continue;
        n = parcel_get(slot, &info, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            found = 1;
            break;
        }
    }
    if (!found) {
        strcpy(st->error, "No number in Talon");
        return;
    }
    /* Trim whitespace */
    {
        char *p = buf;
        int l;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        l = (int)strlen(p);
        while (l > 0 && (p[l - 1] == ' ' || p[l - 1] == '\t' ||
                         p[l - 1] == '\r' || p[l - 1] == '\n'))
            p[--l] = '\0';

        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            if (parse_value_base(p, BASE_HEX, &v) != 0)
                goto invalid;
        } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
            if (parse_value_base(p, BASE_BIN, &v) != 0)
                goto invalid;
        } else {
            if (parse_value_base(p, BASE_DEC, &v) != 0)
                goto invalid;
        }
    }
    set_value(st, v);
    st->replace = 1;
    st->error[0] = '\0';
    snprintf(st->toast, sizeof(st->toast), "Pasted %ld", v);
    st->toast_until = uptime_ms() + 2500;
    return;

invalid:
    strcpy(st->error, "Clipboard is not numeric");
}

/* ------------------------------------------------------------ painting */

static int key_enabled(const struct calc_state *st, const char *k)
{
    int base = (st->mode == MODE_PROG) ? st->base : BASE_DEC;

    if (k[0] >= 'A' && k[0] <= 'F' && !k[1])
        return base == BASE_HEX;
    if (k[0] >= '2' && k[0] <= '9' && !k[1])
        return base != BASE_BIN;
    return 1;
}

static void paint(gui_window *win, gui_ui *ui, struct calc_state *st)
{
    static const char *std_keys[20] = {
        "C",   "Back", "/",  "*",
        "7",   "8",    "9",  "-",
        "4",   "5",    "6",  "+",
        "1",   "2",    "3",  "=",
        "+/-", "0",    "00", "="
    };
    static const char *prog_keys[30] = {
        "A",  "B",  "7",   "8",    "9",   "/",
        "C",  "D",  "4",   "5",    "6",   "*",
        "E",  "F",  "1",   "2",    "3",   "-",
        "<<", ">>", "+/-", "0",    "CLR", "+",
        "&",  "|",  "^",   "~",    "Back","="
    };
    int w = win->w, h = win->h;
    int margin = 12, gap = 6;
    int card_h = (st->mode == MODE_PROG) ? 156 : 104;
    int card_y = 82;
    int grid_y = card_y + card_h + 12;
    int cols = (st->mode == MODE_PROG) ? 6 : 4;
    int rows = 5;
    int bw, bh;
    long cur_val = 0;
    int active_base = (st->mode == MODE_PROG) ? st->base : BASE_DEC;

    if (w < 260) w = 260;
    if (h < 360) h = 360;

    gui_clear(win, GUI_SURFACE);

    /* Top header */
    gui_hgradient(win, gui_mkrc(0, 0, win->w, 40),
                  gui_mix(GUI_PANEL, GUI_ACCENT, 30), GUI_PANEL);
    gui_icon(win, 12, 9, 22, GUI_ICON_CALCULATOR, GUI_ACCENT);
    gui_text(win, 42, 12, "Calculator", GUI_TEXT, GUI_TRANSPARENT);

    if (gui_button_ex(win, gui_mkrc(win->w - 134, 7, 58, 26), "Std", ui,
                      1, st->mode == MODE_STD))
        switch_mode(st, MODE_STD);
    if (gui_button_ex(win, gui_mkrc(win->w - 70, 7, 58, 26), "Prog", ui,
                      1, st->mode == MODE_PROG))
        switch_mode(st, MODE_PROG);

    /* Talon clipboard bar */
    if (gui_button(win, gui_mkrc(margin, 48, 126, 26), "Copy to Talon", ui))
        copy_to_talon(st);
    if (gui_button(win, gui_mkrc(margin + 132, 48, 72, 26), "Paste", ui))
        paste_from_talon(st);

    if (st->toast[0] && uptime_ms() < st->toast_until) {
        gui_push_clip_xy(win, margin + 212, 48, win->w - margin - 220, 26);
        gui_text(win, margin + 212, 53, st->toast, GUI_OK, GUI_TRANSPARENT);
        gui_pop_clip(win);
    }

    /* Display card */
    gui_card(win, gui_mkrc(margin, card_y, win->w - 2 * margin, card_h));
    gui_push_clip_xy(win, margin + 10, card_y + 6,
                     win->w - 2 * margin - 20, card_h - 12);

    gui_text(win, margin + 12, card_y + 10, st->history, GUI_TEXT_DIM,
             GUI_TRANSPARENT);

    {
        int iw = gui_text_w(st->input);
        int ix = win->w - margin - 14 - iw;
        if (ix < margin + 12)
            ix = margin + 12;
        gui_text(win, ix, card_y + 36, st->input, GUI_TEXT, GUI_TRANSPARENT);
    }

    if (st->mode == MODE_PROG) {
        char dec_s[32], hex_s[32], bin_s[88];
        int ry = card_y + 66;

        if (parse_value_base(st->input, active_base, &cur_val) != 0)
            cur_val = 0;
        snprintf(dec_s, sizeof(dec_s), "%ld", cur_val);
        snprintf(hex_s, sizeof(hex_s), "0x%lX", (unsigned long)cur_val);
        format_bin_grouped((unsigned long)cur_val, bin_s, (int)sizeof(bin_s));

        if (gui_button_ex(win, gui_mkrc(margin + 10, ry, 42, 22), "DEC", ui,
                          1, st->base == BASE_DEC))
            switch_base(st, BASE_DEC);
        gui_text(win, margin + 60, ry + 3, dec_s,
                 st->base == BASE_DEC ? GUI_ACCENT : GUI_TEXT, GUI_TRANSPARENT);

        ry += 26;
        if (gui_button_ex(win, gui_mkrc(margin + 10, ry, 42, 22), "HEX", ui,
                          1, st->base == BASE_HEX))
            switch_base(st, BASE_HEX);
        gui_text(win, margin + 60, ry + 3, hex_s,
                 st->base == BASE_HEX ? GUI_ACCENT : GUI_TEXT, GUI_TRANSPARENT);

        ry += 26;
        if (gui_button_ex(win, gui_mkrc(margin + 10, ry, 42, 22), "BIN", ui,
                          1, st->base == BASE_BIN))
            switch_base(st, BASE_BIN);
        gui_text(win, margin + 60, ry + 3, bin_s,
                 st->base == BASE_BIN ? GUI_ACCENT : GUI_TEXT, GUI_TRANSPARENT);
    }
    gui_pop_clip(win);

    if (st->error[0])
        gui_badge(win, gui_mkrc(margin + 10, card_y + card_h - 28,
                                gui_text_w(st->error) + 22, 22),
                  st->error, GUI_ERROR);

    /* Keypad */
    bw = (win->w - 2 * margin - (cols - 1) * gap) / cols;
    bh = (win->h - grid_y - margin - (rows - 1) * gap) / rows;
    if (bw < 28) bw = 28;
    if (bh < 28) bh = 28;

    {
        const char **keys = (st->mode == MODE_PROG) ? prog_keys : std_keys;
        int total = cols * rows;

        for (int i = 0; i < total; i++) {
            int col = i % cols, row = i / cols;
            gui_rc r = gui_mkrc(margin + col * (bw + gap),
                                grid_y + row * (bh + gap), bw, bh);
            const char *k = keys[i];
            int accent = !strcmp(k, "=") || !strcmp(k, "+") ||
                         !strcmp(k, "-") || !strcmp(k, "*") ||
                         !strcmp(k, "/") || !strcmp(k, "<<") ||
                         !strcmp(k, ">>") || !strcmp(k, "&") ||
                         !strcmp(k, "|") || !strcmp(k, "^") ||
                         !strcmp(k, "~");
            int enabled = key_enabled(st, k);

            if (gui_button_ex(win, r, k, ui, enabled, accent))
                press(st, k);
        }
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
    clear_state(&state, 0);

    win = gui_open("Calculator", 300, 110, INIT_W, INIT_H, 0);
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

                if (k == 3) { /* Ctrl+C */
                    copy_to_talon(&state);
                } else if (k == 22) { /* Ctrl+V */
                    paste_from_talon(&state);
                } else if (k == '\t') {
                    switch_mode(&state,
                                state.mode == MODE_STD ? MODE_PROG : MODE_STD);
                } else if (k >= '0' && k <= '9') {
                    char key[2] = { (char)k, '\0' };
                    press(&state, key);
                } else if (state.mode == MODE_PROG &&
                           ((k >= 'a' && k <= 'f') || (k >= 'A' && k <= 'F'))) {
                    char key[2] = { (char)k, '\0' };
                    if (k >= 'a' && k <= 'f')
                        key[0] = (char)(k - ('a' - 'A'));
                    press(&state, key);
                } else if (k == '+' || k == '-' || k == '*' || k == '/' ||
                           (state.mode == MODE_PROG &&
                            (k == '&' || k == '|' || k == '^' || k == '~' ||
                             k == '%'))) {
                    char key[2] = { (char)k, '\0' };
                    press(&state, key);
                } else if (state.mode == MODE_PROG && k == '<') {
                    press(&state, "<<");
                } else if (state.mode == MODE_PROG && k == '>') {
                    press(&state, ">>");
                } else if (k == '\n' || k == '\r' || k == '=') {
                    press(&state, "=");
                } else if (k == 8 || k == 127 || k == KEY_DELETE) {
                    press(&state, "Back");
                } else if (k == 27) {
                    clear_state(&state, 1);
                }
            }
            dirty = 1;
        } else if (state.toast[0] && uptime_ms() >= state.toast_until) {
            state.toast[0] = '\0';
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
