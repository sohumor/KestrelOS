/* KestrelOS snake.
 *
 * Bordered 78x22 playfield (interior), 80ms tick, arrows steer, walls
 * kill, food grows the snake. q quits, r restarts after death.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kestrel.h>

#define FIELD_W 78            /* interior columns */
#define FIELD_H 22            /* interior rows */
#define MAX_LEN (FIELD_W * FIELD_H)

/* interior cell (x 0..77, y 0..21) -> screen row/col */
#define CELL_ROW(y) ((y) + 2)
#define CELL_COL(x) ((x) + 2)

struct pos { int x, y; };

static struct pos body[MAX_LEN];   /* circular buffer, body[head] = head */
static int head, tail, snake_len;
static int dx, dy;
static struct pos food;
static int score;
static int alive;
static char grid[FIELD_H][FIELD_W];   /* 1 if occupied by snake */

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

static void draw_status(const char *extra)
{
    term_goto(25, 1);
    printf("\033[K score: %d   arrows move, q quits%s", score, extra);
}

static void place_food(void)
{
    do {
        food.x = rand() % FIELD_W;
        food.y = rand() % FIELD_H;
    } while (grid[food.y][food.x]);
    term_goto(CELL_ROW(food.y), CELL_COL(food.x));
    term_color(TERM_RED);
    putchar('*');
    term_reset();
}

static void reset_game(void)
{
    memset(grid, 0, sizeof(grid));
    head = tail = 0;
    snake_len = 1;
    body[0].x = FIELD_W / 2;
    body[0].y = FIELD_H / 2;
    grid[body[0].y][body[0].x] = 1;
    dx = 1;
    dy = 0;
    score = 0;
    alive = 1;

    term_clear();
    draw_border();
    term_goto(CELL_ROW(body[0].y), CELL_COL(body[0].x));
    term_color(TERM_GREEN);
    putchar('@');
    term_reset();
    place_food();
    draw_status("");
}

/* Drain pending input; returns 'q' to quit, 'r' for restart request. */
static int poll_input(void)
{
    unsigned char c;
    while (read_nb(0, &c, 1) > 0) {
        switch (c) {
        case KEY_UP:    if (dy != 1)  { dx = 0; dy = -1; } break;
        case KEY_DOWN:  if (dy != -1) { dx = 0; dy = 1;  } break;
        case KEY_LEFT:  if (dx != 1)  { dx = -1; dy = 0; } break;
        case KEY_RIGHT: if (dx != -1) { dx = 1; dy = 0;  } break;
        case 'q':
        case 'Q':
            return 'q';
        case 'r':
        case 'R':
            return 'r';
        default:
            break;
        }
    }
    return 0;
}

static void step(void)
{
    struct pos h = body[head];
    int nx = h.x + dx;
    int ny = h.y + dy;

    /* walls kill */
    if (nx < 0 || nx >= FIELD_W || ny < 0 || ny >= FIELD_H) {
        alive = 0;
        return;
    }
    int ate = (nx == food.x && ny == food.y);

    if (!ate) {
        /* remove tail first so chasing your own tail is legal */
        struct pos t = body[tail];
        grid[t.y][t.x] = 0;
        tail = (tail + 1) % MAX_LEN;
        term_goto(CELL_ROW(t.y), CELL_COL(t.x));
        putchar(' ');
    }

    if (grid[ny][nx]) {   /* self collision */
        alive = 0;
        return;
    }

    term_color(TERM_GREEN);
    /* Old head becomes body. At length 1 the tail we just erased IS the
     * old head, so painting 'o' here would undo that erase and leave a
     * trail behind the snake for the whole game. */
    if (ate || snake_len > 1) {
        term_goto(CELL_ROW(h.y), CELL_COL(h.x));
        putchar('o');
    }

    head = (head + 1) % MAX_LEN;
    body[head].x = nx;
    body[head].y = ny;
    grid[ny][nx] = 1;
    term_goto(CELL_ROW(ny), CELL_COL(nx));
    putchar('@');
    term_reset();

    if (ate) {
        snake_len++;
        score += 10;
        place_food();
        draw_status("");
    }
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    srand((unsigned int)uptime_ms());
    term_hide_cursor();
    reset_game();

    for (;;) {
        int c = poll_input();
        if (c == 'q')
            break;
        if (c == 'r' && !alive) {
            reset_game();
            continue;
        }
        if (alive) {
            step();
            if (!alive) {
                term_goto(11, 32);
                term_color(TERM_RED);
                printf("\033[1m GAME OVER \033[0m");
                draw_status("   r restarts");
            }
        }
        sleep_ms(80);
    }

    term_reset();
    term_show_cursor();
    term_clear();
    printf("snake: final score %d\n", score);
    return 0;
}
