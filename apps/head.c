/* head.c - print the first lines of a file.
 *
 * usage: head [-n N] [file...]   (default 10 lines, stdin if no file)
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PATH 256
#define BUFSZ    512
#define CTRL_D   4

static const char *g_cwd = "/";

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

/* Read one line (newline stripped), growing out as needed. */
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
                printf("head: out of memory\n");
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

static void head_fd(int fd, int tty, int nlines)
{
    struct lreader r;
    char *line = 0;
    unsigned long cap = 0;
    int i;

    lr_init(&r, fd, tty);
    for (i = 0; i < nlines; i++) {
        if (!lr_line(&r, &line, &cap))
            break;
        printf("%s\n", line);
    }
    free(line);
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
                printf("usage: head [-n N] [file...]\n");
                return 1;
            }
            nlines = atoi(argv[++i]);
            if (nlines < 0)
                nlines = 0;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            printf("usage: head [-n N] [file...]\n");
            return 1;
        } else {
            files++;
        }
    }

    if (files == 0) {
        head_fd(0, 1, nlines);
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
            printf("head: cannot open %s\n", path);
            rc = 1;
            continue;
        }
        if (files > 1)
            printf("==> %s <==\n", argv[i]);
        head_fd(fd, 0, nlines);
        close(fd);
    }
    return rc;
}
