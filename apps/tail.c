/* tail.c - print the last lines of a file.
 *
 * usage: tail [-n N] [file...]   (default 10 lines, stdin if no file)
 * The last N lines are kept in a fixed ring buffer, so memory use is
 * bounded no matter how large the input is.
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PATH  256
#define BUFSZ     512
#define LINESZ    512
#define MAX_LINES 128
#define CTRL_D    4

static const char *g_cwd = "/";
static char g_ring[MAX_LINES][LINESZ];

/* Pull the shell-injected "--cwd=<path>" argument, wherever it sits. */
static int strip_cwd_arg(int argc, char **argv)
{
    int i, out = 1;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) == 0)
            g_cwd = argv[i] + 6;
        else
            argv[out++] = argv[i];
    }
    return out;
}

/* Join a possibly relative path with the cwd. */
static void resolve(const char *tok, char *out, unsigned long outsz)
{
    if (tok[0] == '/')
        snprintf(out, outsz, "%s", tok);
    else if (g_cwd[0] == '/' && g_cwd[1] == '\0')
        snprintf(out, outsz, "/%s", tok);
    else
        snprintf(out, outsz, "%s/%s", g_cwd, tok);
}

/* Buffered line reader over one fd. */
struct lreader {
    int fd;
    int len;
    int pos;
    int tty;
    char buf[BUFSZ];
};

static void lr_init(struct lreader *r, int fd, int tty)
{
    r->fd = fd;
    r->len = 0;
    r->pos = 0;
    r->tty = tty;
}

static int lr_line(struct lreader *r, char *out, int outsz)
{
    int n = 0, got = 0;

    for (;;) {
        char c;
        if (r->pos >= r->len) {
            long g = read(r->fd, r->buf, sizeof(r->buf));
            if (g <= 0)
                break;
            r->len = (int)g;
            r->pos = 0;
        }
        c = r->buf[r->pos++];
        if (r->tty && c == CTRL_D)
            break;
        got = 1;
        if (c == '\n')
            break;
        if (n < outsz - 1)
            out[n++] = c;
    }
    out[n] = '\0';
    return got;
}

static void tail_fd(int fd, int tty, int nlines)
{
    struct lreader r;
    char line[LINESZ];
    long total = 0;
    int i, first, count;

    /* Read into a scratch buffer: lr_line terminates "out" even on the
     * final call that returns 0 at EOF, so writing straight into the
     * ring would blank the oldest slot we are about to print. */
    lr_init(&r, fd, tty);
    while (lr_line(&r, line, LINESZ)) {
        memcpy(g_ring[total % nlines], line, strlen(line) + 1);
        total++;
    }

    count = (int)(total < nlines ? total : nlines);
    first = (int)(total - count);
    for (i = 0; i < count; i++)
        printf("%s\n", g_ring[(first + i) % nlines]);
}

int main(int argc, char **argv)
{
    char path[MAX_PATH];
    int nlines = 10;
    int i, fd, files = 0, rc = 0;

    argc = strip_cwd_arg(argc, argv);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 >= argc) {
                printf("usage: tail [-n N] [file...]\n");
                return 1;
            }
            nlines = atoi(argv[++i]);
            if (nlines < 1)
                nlines = 1;
            if (nlines > MAX_LINES)
                nlines = MAX_LINES;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            printf("usage: tail [-n N] [file...]\n");
            return 1;
        } else {
            files++;
        }
    }

    if (files == 0) {
        tail_fd(0, 1, nlines);
        return 0;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            i++;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0')
            continue;

        resolve(argv[i], path, sizeof(path));
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            printf("tail: cannot open %s\n", path);
            rc = 1;
            continue;
        }
        if (files > 1)
            printf("==> %s <==\n", argv[i]);
        tail_fd(fd, 0, nlines);
        close(fd);
    }
    return rc;
}
