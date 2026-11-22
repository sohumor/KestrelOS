/* tail.c - print the last lines of a file.
 *
 * usage: tail [-n N] [file...]   (default 10 lines, stdin if no file)
 * The last N lines are kept in a ring buffer.
 */

#include <kestrel.h>
#include <kestrel_cwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PATH  256
#define BUFSZ     512
#define MAX_LINES 128
#define CTRL_D    4

static const char *g_cwd = "/";

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

static int lr_line(struct lreader *r, char **out, unsigned long *cap)
{
    unsigned long n = 0;
    int got = 0;

    for (;;) {
        char c;
        if (n + 1 >= *cap) {
            unsigned long size = *cap ? *cap * 2 : BUFSZ;
            char *p = size > *cap ? realloc(*out, size) : 0;
            if (!p) {
                printf("tail: out of memory\n");
                exit(1);
            }
            *out = p;
            *cap = size;
        }
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
        (*out)[n++] = c;
    }
    (*out)[n] = '\0';
    return got;
}

static void tail_fd(int fd, int tty, int nlines)
{
    struct lreader r;
    char *line = 0;
    char *ring[MAX_LINES] = {0};
    unsigned long cap = 0;
    long total = 0;
    int i, first, count;

    if (nlines == 0)
        return;

    lr_init(&r, fd, tty);
    while (lr_line(&r, &line, &cap)) {
        free(ring[total % nlines]);
        ring[total % nlines] = line;
        line = 0;
        cap = 0;
        total++;
    }
    free(line);

    count = (int)(total < nlines ? total : nlines);
    first = (int)(total - count);
    for (i = 0; i < count; i++) {
        char *p = ring[(first + i) % nlines];
        printf("%s\n", p);
        free(p);
    }
}

int main(int argc, char **argv)
{
    char path[MAX_PATH];
    int nlines = 10;
    int i, fd, files = 0, rc = 0;

    g_cwd = kestrel_startup_cwd();

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 >= argc) {
                printf("usage: tail [-n N] [file...]\n");
                return 1;
            }
            nlines = atoi(argv[++i]);
            if (nlines < 0)
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
