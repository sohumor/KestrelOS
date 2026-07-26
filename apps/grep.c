/* grep.c - print lines containing a literal substring.
 *
 * usage: grep [-i] [-n] <pattern> <file>...
 * Literal matching only: no regular expressions.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#define MAX_PATH 256
#define BUFSZ    512
#define LINESZ   512
#define CTRL_D   4

static const char *g_cwd = "/";
static int g_icase;

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

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Literal substring search, honouring the -i flag. */
static int contains(const char *hay, const char *needle)
{
    unsigned long nlen = strlen(needle);
    unsigned long hlen = strlen(hay);
    unsigned long i, j;

    if (nlen == 0)
        return 1;
    if (nlen > hlen)
        return 0;

    for (i = 0; i + nlen <= hlen; i++) {
        for (j = 0; j < nlen; j++) {
            char a = hay[i + j], b = needle[j];
            if (g_icase) {
                a = lower(a);
                b = lower(b);
            }
            if (a != b)
                break;
        }
        if (j == nlen)
            return 1;
    }
    return 0;
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

/* Returns the number of matching lines. */
static int grep_fd(int fd, int tty, const char *pat, const char *label,
                   int show_num)
{
    struct lreader r;
    char line[LINESZ];
    int lineno = 0, hits = 0;

    lr_init(&r, fd, tty);
    while (lr_line(&r, line, sizeof(line))) {
        lineno++;
        if (!contains(line, pat))
            continue;
        hits++;
        if (label)
            printf("%s:", label);
        if (show_num)
            printf("%d:", lineno);
        printf("%s\n", line);
    }
    return hits;
}

int main(int argc, char **argv)
{
    char path[MAX_PATH];
    const char *pat = 0;
    int show_num = 0;
    int i, fd, files = 0, hits = 0, rc = 0;

    argc = strip_cwd_arg(argc, argv);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0) {
            g_icase = 1;
        } else if (strcmp(argv[i], "-n") == 0) {
            show_num = 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            printf("usage: grep [-i] [-n] <pattern> <file>...\n");
            return 1;
        } else if (!pat) {
            pat = argv[i];
        } else {
            files++;
        }
    }

    if (!pat) {
        printf("usage: grep [-i] [-n] <pattern> <file>...\n");
        return 1;
    }

    if (files == 0)
        return grep_fd(0, 1, pat, 0, show_num) > 0 ? 0 : 1;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0')
            continue;
        if (argv[i] == pat)
            continue;

        resolve(argv[i], path, sizeof(path));
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            printf("grep: cannot open %s\n", path);
            rc = 2;
            continue;
        }
        hits += grep_fd(fd, 0, pat, files > 1 ? argv[i] : 0, show_num);
        close(fd);
    }

    if (rc != 0)
        return rc;
    return hits > 0 ? 0 : 1;
}
