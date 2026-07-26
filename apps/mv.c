/* mv.c - move (rename) a file.
 *
 * usage: mv <src> <dst>
 * KFS has no rename, so this is copy + unlink. Moving a file onto
 * itself is a no-op. If <dst> is a directory the basename is kept.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#define MAX_PATH 256
#define BUFSZ    512

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

static const char *basename_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int copy_file(const char *src, const char *dst)
{
    char buf[BUFSZ];
    long n, w;
    int in, out;

    in = open(src, O_RDONLY);
    if (in < 0) {
        printf("mv: cannot open %s\n", src);
        return 1;
    }
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        printf("mv: cannot create %s\n", dst);
        close(in);
        return 1;
    }

    while ((n = read(in, buf, sizeof(buf))) > 0) {
        w = write(out, buf, (unsigned long)n);
        if (w != n) {
            printf("mv: write error on %s\n", dst);
            close(in);
            close(out);
            return 1;
        }
    }

    close(in);
    close(out);
    if (n < 0) {
        printf("mv: read error on %s\n", src);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char src[MAX_PATH], dst[MAX_PATH];
    struct k_stat st;

    argc = strip_cwd_arg(argc, argv);
    if (argc != 3) {
        printf("usage: mv <src> <dst>\n");
        return 1;
    }

    resolve(argv[1], src, sizeof(src));
    resolve(argv[2], dst, sizeof(dst));

    if (stat_(src, &st) != 0) {
        printf("mv: %s: no such file\n", src);
        return 1;
    }
    if (st.is_dir) {
        printf("mv: %s is a directory\n", src);
        return 1;
    }

    if (stat_(dst, &st) == 0 && st.is_dir) {
        char tmp[MAX_PATH];
        snprintf(tmp, sizeof(tmp), "%s%s%s", dst,
                 (dst[0] == '/' && dst[1] == '\0') ? "" : "/",
                 basename_of(src));
        snprintf(dst, sizeof(dst), "%s", tmp);
    }

    if (strcmp(src, dst) == 0)
        return 0;   /* same path: nothing to do */

    if (copy_file(src, dst) != 0)
        return 1;

    if (unlink_(src) != 0) {
        printf("mv: cannot remove %s\n", src);
        return 1;
    }
    return 0;
}
