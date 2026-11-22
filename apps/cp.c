/* cp.c - copy a file.
 *
 * usage: cp <src> <dst>
 * If <dst> is an existing directory the source basename is appended.
 */

#include <kestrel.h>
#include <kestrel_cwd.h>
#include <stdio.h>
#include <string.h>

#define MAX_PATH 256
#define BUFSZ    512

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

static const char *basename_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Copy src -> dst. Returns 0 on success. */
static int copy_file(const char *src, const char *dst)
{
    char buf[BUFSZ];
    long n, w;
    int in, out;

    in = open(src, O_RDONLY);
    if (in < 0) {
        printf("cp: cannot open %s\n", src);
        return 1;
    }
    out = open_distinct(dst, O_WRONLY | O_CREAT | O_TRUNC, in);
    if (out < 0) {
        if (out == K_OPEN_SAME_FILE)
            printf("cp: %s and %s are the same file\n", src, dst);
        else
            printf("cp: cannot create %s\n", dst);
        close(in);
        return 1;
    }

    while ((n = read(in, buf, sizeof(buf))) > 0) {
        w = write(out, buf, (unsigned long)n);
        if (w != n) {
            printf("cp: write error on %s\n", dst);
            close(in);
            close(out);
            return 1;
        }
    }

    close(in);
    close(out);
    if (n < 0) {
        printf("cp: read error on %s\n", src);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char src[MAX_PATH], dst[MAX_PATH];
    struct k_stat st;

    g_cwd = kestrel_startup_cwd();
    if (argc != 3) {
        printf("usage: cp <src> <dst>\n");
        return 1;
    }

    resolve(argv[1], src, sizeof(src));
    resolve(argv[2], dst, sizeof(dst));

    if (stat_(src, &st) != 0) {
        printf("cp: %s: no such file\n", src);
        return 1;
    }
    if (st.is_dir) {
        printf("cp: %s is a directory\n", src);
        return 1;
    }

    /* Copying into a directory keeps the source name. */
    if (stat_(dst, &st) == 0 && st.is_dir) {
        char tmp[MAX_PATH];
        snprintf(tmp, sizeof(tmp), "%s%s%s", dst,
                 (dst[0] == '/' && dst[1] == '\0') ? "" : "/",
                 basename_of(src));
        snprintf(dst, sizeof(dst), "%s", tmp);
    }

    if (strcmp(src, dst) == 0) {
        printf("cp: %s and %s are the same file\n", src, dst);
        return 1;
    }

    return copy_file(src, dst);
}
