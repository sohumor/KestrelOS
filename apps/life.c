/* KestrelOS life - Conway's Game of Life on a toroidal 78x21 board.
 *
 * Starts on a random soup and runs. Newborn cells draw '@', survivors
 * age into 'o', so motion is visible at a glance. Pausing reveals a
 * cursor for editing: arrows move, x toggles a cell, g/G stamp a
 * glider / Gosper glider gun under the cursor.
 *
 *   space  pause/resume        . or n  single step (while paused)
 *   arrows move cursor         x       toggle cell at cursor
 *   g      stamp glider        G       stamp glider gun
 *   r      new random soup     c       clear board
 *   q      quit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kestrel.h>

#define FIELD_W 78            /* interior columns */
#define FIELD_H 21            /* interior rows */
#define TICK_MS 70
#define SOUP_PCT 18           /* random-fill density */

/* interior cell (x 0..77, y 0..20) -> screen row/col */
#define CELL_ROW(y) ((y) + 2)
#define CELL_COL(x) ((x) + 2)

/* cell states: 0 dead, 1 newborn ('@'), 2 settled ('o') */
static unsigned char cur[FIELD_H][FIELD_W];
static unsigned char nxt[FIELD_H][FIELD_W];

static int paused;
static int step_once;
static int cx, cy;            /* cursor position (drawn when paused) */
static unsigned long gen;
static unsigned long pop;

static void draw_border(void)
{
    term_color(TERM_CYAN);
    term_goto(1, 1);
    putchar('+');
    for (int i = 0; i < FIELD_W; i++)
        putchar('-');
    putchar('+');
    for (int y = 0; y < FIELD_H; y++) {
        term_goto(CELL_ROW(y), 1);
        putchar('|');
        term_goto(CELL_ROW(y), FIELD_W + 2);
        putchar('|');
    }
    term_goto(FIELD_H + 2, 1);
    putchar('+');
    for (int i = 0; i < FIELD_W; i++)
        putchar('-');
    putchar('+');
    term_reset();
}

/* Paint one cell. When paused the cursor cell is drawn reverse-video. */
static void draw_cell(int x, int y)
{
    int under_cursor = paused && x == cx && y == cy;

    term_goto(CELL_ROW(y), CELL_COL(x));
    if (under_cursor)
        printf("\033[7m");
    switch (cur[y][x]) {
    case 0:
        putchar(' ');
        break;
    case 1:
        term_color(TERM_YELLOW);
        putchar('@');
        break;
    default:
        term_color(TERM_GREEN);
        putchar('o');
        break;
    }
    term_reset();
}

static void draw_status(void)
{
    term_goto(24, 1);
    printf("\033[K gen %-8lu pop %-5lu %s", gen, pop,
           paused ? "PAUSED" : "running");
    term_goto(25, 1);
    printf("\033[K space pause  . step  arrows move  x toggle"
           "  g glider  G gun  r soup  c clear  q quit");
}

static void draw_all(void)
{
    for (int y = 0; y < FIELD_H; y++)
        for (int x = 0; x < FIELD_W; x++)
            if (cur[y][x])
                draw_cell(x, y);
}

static void recount_pop(void)
{
    pop = 0;
    for (int y = 0; y < FIELD_H; y++)
        for (int x = 0; x < FIELD_W; x++)
            if (cur[y][x])
                pop++;
}

static void random_soup(void)
{
    gen = 0;
    for (int y = 0; y < FIELD_H; y++)
        for (int x = 0; x < FIELD_W; x++)
            cur[y][x] = (rand() % 100 < SOUP_PCT) ? 2 : 0;
    term_clear();
    draw_border();
    draw_all();
    recount_pop();
    draw_status();
    draw_cell(cx, cy);        /* cursor may sit on a dead cell */
}

static void clear_board(void)
{
    memset(cur, 0, sizeof(cur));
    gen = 0;
    pop = 0;
    term_clear();
    draw_border();
    draw_status();
    draw_cell(cx, cy);        /* cursor may sit on a dead cell */
}

static int neighbors(int x, int y)
{
    int n = 0;
    for (int dy = -1; dy <= 1; dy++) {
        int yy = (y + dy + FIELD_H) % FIELD_H;
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0)
                continue;
            n += cur[yy][(x + dx + FIELD_W) % FIELD_W] ? 1 : 0;
        }
    }
    return n;
}

/* Advance one generation, repainting only cells whose state changed. */
static void step(void)
{
    for (int y = 0; y < FIELD_H; y++)
        for (int x = 0; x < FIELD_W; x++) {
            int n = neighbors(x, y);
            int alive = cur[y][x] != 0;
            nxt[y][x] = (alive ? (n == 2 || n == 3) : (n == 3))
                            ? (alive ? 2 : 1)
                            : 0;
        }

    pop = 0;
    for (int y = 0; y < FIELD_H; y++)
        for (int x = 0; x < FIELD_W; x++) {
            if (nxt[y][x] != cur[y][x]) {
                cur[y][x] = nxt[y][x];
                draw_cell(x, y);
            }
            if (cur[y][x])
                pop++;
        }
    gen++;
    draw_status();
}

/* Stamp a pattern ('O' rows) with its top-left corner at the cursor. */
static void stamp(const char *const *rows, int h)
{
    for (int dy = 0; dy < h; dy++) {
        const char *r = rows[dy];
        int yy = (cy + dy) % FIELD_H;
        for (int dx = 0; r[dx]; dx++) {
            if (r[dx] != 'O')
                continue;
            int xx = (cx + dx) % FIELD_W;
            if (!cur[yy][xx]) {
                cur[yy][xx] = 2;
                draw_cell(xx, yy);
            }
        }
    }
    recount_pop();
    draw_status();
    draw_cell(cx, cy);
}

/* Gosper glider gun, 36x9: fires a glider stream toward lower-right. */
static const char *const GUN[] = {
    "........................O...........",
    "......................O.O...........",
    "............OO......OO............OO",
    "...........O...O....OO............OO",
    "OO........O.....O...OO..............",
    "OO........O...O.OO....O.O...........",
    "..........O.....O.......O...........",
    "...........O...O....................",
    "............OO......................",
};

static const char *const GLIDER[] = {
    ".O.",
    "..O",
    "OOO",
};

static void toggle_cell(void)
{
    cur[cy][cx] = cur[cy][cx] ? 0 : 2;
    recount_pop();
    draw_cell(cx, cy);
    draw_status();
}

/* Drain pending input; returns 'q' to quit. Actions repaint in place. */
static int poll_input(void)
{
    unsigned char c;
    while (read_nb(0, &c, 1) > 0) {
        switch (c) {
        case 'q':
        case 'Q':
            return 'q';
        case ' ':
            paused = !paused;
            draw_cell(cx, cy);   /* paint/unpaint cursor */
            draw_status();
            break;
        case '.':
        case 'n':
            if (paused)
                step_once = 1;
            break;
        case 'r':
        case 'R':
            random_soup();
            break;
        case 'c':
        case 'C':
            clear_board();
            break;
        case 'x':
        case 'X':
        case '\r':
        case '\n':
            toggle_cell();
            break;
        case 'g':
            stamp(GLIDER, 3);
            break;
        case 'G':
            stamp(GUN, 9);
            break;
        case KEY_UP:
            if (paused) { int o = cy; cy = (cy + FIELD_H - 1) % FIELD_H;
                          draw_cell(cx, o); draw_cell(cx, cy); }
            break;
        case KEY_DOWN:
            if (paused) { int o = cy; cy = (cy + 1) % FIELD_H;
                          draw_cell(cx, o); draw_cell(cx, cy); }
            break;
        case KEY_LEFT:
            if (paused) { int o = cx; cx = (cx + FIELD_W - 1) % FIELD_W;
                          draw_cell(o, cy); draw_cell(cx, cy); }
            break;
        case KEY_RIGHT:
            if (paused) { int o = cx; cx = (cx + 1) % FIELD_W;
                          draw_cell(o, cy); draw_cell(cx, cy); }
            break;
        default:
            break;
        }
    }
    return 0;
}

int main(int argc, char *argv[])
{
    unsigned int seed = 0;
    (void)argc;
    (void)argv;

    if (getrandom(&seed, sizeof(seed), 0) < 0 || seed == 0)
        seed = (unsigned int)uptime_ms();
    srand(seed);

    paused = 0;
    step_once = 0;
    cx = FIELD_W / 2;
    cy = FIELD_H / 2;

    term_hide_cursor();
    random_soup();

    for (;;) {
        if (poll_input() == 'q')
            break;
        if (!paused || step_once) {
            step();
            step_once = 0;
        }
        sleep_ms(paused ? 30 : TICK_MS);
    }

    term_reset();
    term_show_cursor();
    term_clear();
    printf("life: %lu generations\n", gen);
    return 0;
}
