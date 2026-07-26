/* wc.c - count lines, words and bytes.
 *
 * usage: wc [-l] [-w] [-c] [file...]
 * With no file the console (fd 0) is read until ctrl-D.
 */

#include <kestrel.h>
#include <stdio.h>
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

struct counts {
    long lines;
    long words;
    long bytes;
};

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
           c == '\f';
}

/* Count one open stream. Returns 0 on success. */
static int count_fd(int fd, int is_tty, struct counts *c)
{
    char buf[BUFSZ];
    long n;
    int i, in_word = 0;

    for (;;) {
        n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (i = 0; i < (int)n; i++) {
            char ch = buf[i];
            if (is_tty && ch == CTRL_D)
                return 0;
            c->bytes++;
            if (ch == '\n')
                c->lines++;
            if (is_space(ch)) {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                c->words++;
            }
        }
    }
    return n < 0 ? 1 : 0;
}

static void report(const struct counts *c, int want_l, int want_w, int want_c,
                   const char *name)
{
    if (want_l)
        printf("%8ld", c->lines);
    if (want_w)
        printf("%8ld", c->words);
    if (want_c)
        printf("%8ld", c->bytes);
    if (name)
        printf(" %s", name);
    printf("\n");
}

int main(int argc, char **argv)
{
    char path[MAX_PATH];
    struct counts total = { 0, 0, 0 };
    int want_l = 0, want_w = 0, want_c = 0;
    int i, fd, files = 0, rc = 0;

    argc = strip_cwd_arg(argc, argv);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0)
            want_l = 1;
        else if (strcmp(argv[i], "-w") == 0)
            want_w = 1;
        else if (strcmp(argv[i], "-c") == 0)
            want_c = 1;
        else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            printf("usage: wc [-l] [-w] [-c] [file...]\n");
            return 1;
        } else {
            files++;
        }
    }
    if (!want_l && !want_w && !want_c)
        want_l = want_w = want_c = 1;

    if (files == 0) {
        struct counts c = { 0, 0, 0 };
        rc = count_fd(0, 1, &c);
        report(&c, want_l, want_w, want_c, 0);
        return rc;
    }

    for (i = 1; i < argc; i++) {
        struct counts c = { 0, 0, 0 };
        if (argv[i][0] == '-' && argv[i][1] != '\0')
            continue;

        resolve(argv[i], path, sizeof(path));
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            printf("wc: cannot open %s\n", path);
            rc = 1;
            continue;
        }
        if (count_fd(fd, 0, &c) != 0)
            rc = 1;
        close(fd);

        total.lines += c.lines;
        total.words += c.words;
        total.bytes += c.bytes;
        report(&c, want_l, want_w, want_c, argv[i]);
    }

    if (files > 1)
        report(&total, want_l, want_w, want_c, "total");
    return rc;
}
