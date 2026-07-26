/* KestrelOS full-screen text editor.
 *
 * Usage: edit [file]
 * Rows 1-23: text (with horizontal scroll), row 24: inverse status bar,
 * row 25: message line. Ctrl-S saves, Ctrl-Q quits (twice if modified).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kestrel.h>

#define MAX_LINES 2000
#define MAX_COLS  256          /* buffer size per line, incl. NUL */
#define TEXT_ROWS 23
#define TEXT_COLS 80

static char *lines[MAX_LINES];
static int nlines = 0;

static char filename[128];
static int modified = 0;
static int cy = 0;             /* cursor line (0-based) */
static int cx = 0;             /* cursor column (0-based) */
static int top = 0;            /* first visible line */
static int leftcol = 0;        /* horizontal scroll offset */
static char message[80];
static int quit_pending = 0;
static int save_pending = 0;   /* confirm overwrite of a truncated buffer */
static int truncated = 0;      /* file did not fit in the buffer */
static int had_final_nl = 1;   /* source file ended with a newline */

static char *alloc_line(const char *text)
{
    char *l = malloc(MAX_COLS);
    if (!l) {
        printf("edit: out of memory\n");
        exit(1);
    }
    l[0] = 0;
    if (text)
        strncpy(l, text, MAX_COLS - 1);
    l[MAX_COLS - 1] = 0;
    return l;
}

static void set_message(const char *m)
{
    strncpy(message, m, sizeof(message) - 1);
    message[sizeof(message) - 1] = 0;
}

/* Resolve path against --cwd (shell-injected) when relative. */
static void resolve_path(const char *path, const char *cwd, char *out,
                         unsigned long outsz)
{
    if (path[0] == '/' || !cwd || !cwd[0]) {
        strncpy(out, path, outsz - 1);
        out[outsz - 1] = 0;
    } else {
        snprintf(out, outsz, "%s/%s", strcmp(cwd, "/") == 0 ? "" : cwd, path);
    }
}

static void load_file(void)
{
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        nlines = 1;
        lines[0] = alloc_line(0);
        set_message("new file");
        return;
    }

    char cur[MAX_COLS];
    int curlen = 0;
    char buf[512];
    long n;
    int last = -1;
    nlines = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            last = (unsigned char)c;
            if (c == '\n') {
                if (nlines < MAX_LINES) {
                    cur[curlen] = 0;
                    lines[nlines++] = alloc_line(cur);
                } else {
                    truncated = 1;
                }
                curlen = 0;
            } else if (c != '\r') {
                if (curlen < MAX_COLS - 1)
                    cur[curlen++] = c;
                else
                    truncated = 1;      /* the rest of this line is lost */
            }
        }
    }
    had_final_nl = (last < 0 || last == '\n');
    if (curlen > 0 || nlines == 0) {
        if (nlines < MAX_LINES) {
            cur[curlen] = 0;
            lines[nlines++] = alloc_line(cur);
        } else {
            truncated = 1;
        }
    }
    close(fd);
    /* Saving writes the buffer back with O_TRUNC, so a partial load would
     * silently destroy the rest of the file. Say so, loudly. */
    set_message(truncated ? "loaded, TRUNCATED - saving will destroy data"
                          : "loaded");
}

static void save_file(void)
{
    if (truncated && !save_pending) {
        save_pending = 1;
        set_message("buffer is TRUNCATED - ctrl-S again to overwrite anyway");
        return;
    }
    save_pending = 0;

    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        set_message("save failed: cannot open file");
        return;
    }
    long total = 0;
    for (int i = 0; i < nlines; i++) {
        unsigned long len = strlen(lines[i]);
        if (len > 0)
            write(fd, lines[i], len);
        /* Separator between lines, plus the file's own terminator if it
         * had one — otherwise every save shaves a byte off the file. */
        if (i < nlines - 1 || (had_final_nl && (nlines > 1 || len > 0))) {
            write(fd, "\n", 1);
            total++;
        }
        total += (long)len;
    }
    close(fd);
    modified = 0;
    char m[64];
    snprintf(m, sizeof(m), "saved %d lines", nlines);
    set_message(m);
    (void)total;
}

static void draw(void)
{
    term_hide_cursor();

    for (int row = 0; row < TEXT_ROWS; row++) {
        term_goto(row + 1, 1);
        printf("\033[K");
        int li = top + row;
        if (li < nlines) {
            const char *l = lines[li];
            unsigned long len = strlen(l);
            if ((unsigned long)leftcol < len) {
                const char *p = l + leftcol;
                unsigned long vis = len - (unsigned long)leftcol;
                if (vis > TEXT_COLS)
                    vis = TEXT_COLS;
                write(1, p, vis);
            }
        } else {
            term_color(TERM_CYAN);
            printf("~");
            term_reset();
        }
    }

    /* status bar, row 24, inverse video */
    char status[96];
    snprintf(status, sizeof(status),
             " %s%s%s  ln %d/%d col %d  ^S save ^Q quit",
             filename[0] ? filename : "(no name)", modified ? " *" : "",
             truncated ? " [TRUNCATED]" : "", cy + 1, nlines, cx + 1);
    term_goto(24, 1);
    printf("\033[7m");
    int slen = (int)strlen(status);
    write(1, status, slen > TEXT_COLS ? TEXT_COLS : (unsigned long)slen);
    for (int i = slen; i < TEXT_COLS; i++)
        putchar(' ');
    term_reset();

    /* message line, row 25 */
    term_goto(25, 1);
    printf("\033[K%s", message);

    term_goto(cy - top + 1, cx - leftcol + 1);
    term_show_cursor();
}

static void clamp_scroll(void)
{
    if (cy < top)
        top = cy;
    if (cy >= top + TEXT_ROWS)
        top = cy - TEXT_ROWS + 1;
    if (cx < leftcol)
        leftcol = cx;
    if (cx >= leftcol + TEXT_COLS)
        leftcol = cx - TEXT_COLS + 1;
}

static void insert_char(int c)
{
    char *l = lines[cy];
    int len = (int)strlen(l);
    if (len >= MAX_COLS - 1) {
        set_message("line full");
        return;
    }
    if (cx > len)
        cx = len;
    memmove(l + cx + 1, l + cx, (unsigned long)(len - cx + 1));
    l[cx] = (char)c;
    cx++;
    modified = 1;
}

static void delete_at(void)
{
    char *l = lines[cy];
    int len = (int)strlen(l);
    if (cx < len) {
        memmove(l + cx, l + cx + 1, (unsigned long)(len - cx));
        modified = 1;
    } else if (cy < nlines - 1) {
        /* join next line up */
        char *next = lines[cy + 1];
        int nl = (int)strlen(next);
        int room = MAX_COLS - 1 - len;
        if (nl > room) {
            /* Truncating here would silently drop the overflow, the way
             * insert_char refuses when a line is full. */
            set_message("lines too long to join");
            return;
        }
        memcpy(l + len, next, (unsigned long)nl);
        l[len + nl] = 0;
        free(next);
        for (int i = cy + 1; i < nlines - 1; i++)
            lines[i] = lines[i + 1];
        nlines--;
        modified = 1;
    }
}

static void backspace(void)
{
    if (cx > 0) {
        cx--;
        delete_at();
    } else if (cy > 0) {
        cy--;
        cx = (int)strlen(lines[cy]);
        delete_at();
    }
}

static void split_line(void)
{
    if (nlines >= MAX_LINES) {
        set_message("buffer full");
        return;
    }
    char *l = lines[cy];
    int len = (int)strlen(l);
    if (cx > len)
        cx = len;
    for (int i = nlines; i > cy + 1; i--)
        lines[i] = lines[i - 1];
    lines[cy + 1] = alloc_line(l + cx);
    l[cx] = 0;
    nlines++;
    cy++;
    cx = 0;
    modified = 1;
}

static void clamp_cx(void)
{
    int len = (int)strlen(lines[cy]);
    if (cx > len)
        cx = len;
}

int main(int argc, char *argv[])
{
    const char *cwd = "";
    const char *arg = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) == 0)
            cwd = argv[i] + 6;
        else if (!arg)
            arg = argv[i];
    }

    if (arg) {
        resolve_path(arg, cwd, filename, sizeof(filename));
        load_file();
    } else {
        strcpy(filename, "");
        nlines = 1;
        lines[0] = alloc_line(0);
        set_message("empty buffer (no filename: save will fail)");
    }

    term_clear();

    for (;;) {
        clamp_scroll();
        draw();
        int c = getchar();
        if (c == EOF)
            break;

        if (c != 17)
            quit_pending = 0;
        if (c != 19)
            save_pending = 0;

        switch (c) {
        case KEY_UP:
            if (cy > 0) cy--;
            clamp_cx();
            break;
        case KEY_DOWN:
            if (cy < nlines - 1) cy++;
            clamp_cx();
            break;
        case KEY_LEFT:
            if (cx > 0) cx--;
            else if (cy > 0) { cy--; cx = (int)strlen(lines[cy]); }
            break;
        case KEY_RIGHT:
            if (cx < (int)strlen(lines[cy])) cx++;
            else if (cy < nlines - 1) { cy++; cx = 0; }
            break;
        case KEY_HOME:
            cx = 0;
            break;
        case KEY_END:
            cx = (int)strlen(lines[cy]);
            break;
        case KEY_PGUP:
            cy -= TEXT_ROWS;
            if (cy < 0) cy = 0;
            clamp_cx();
            break;
        case KEY_PGDN:
            cy += TEXT_ROWS;
            if (cy > nlines - 1) cy = nlines - 1;
            clamp_cx();
            break;
        case KEY_DELETE:
            delete_at();
            break;
        case 8:   /* backspace */
        case 127:
            backspace();
            break;
        case '\n':
        case '\r':
            split_line();
            break;
        case 19:  /* ctrl-S */
            if (filename[0])
                save_file();
            else
                set_message("no filename to save to");
            break;
        case 17:  /* ctrl-Q */
            if (modified && !quit_pending) {
                quit_pending = 1;
                set_message("unsaved changes! ctrl-Q again to quit");
            } else {
                term_clear();
                term_show_cursor();
                return 0;
            }
            break;
        default:
            if (c >= 32 && c < 127)
                insert_char(c);
            break;
        }
    }

    term_clear();
    term_show_cursor();
    return 0;
}
