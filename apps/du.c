/* du.c - recursive byte totals per directory.
 *
 * usage: du [path...]
 * Prints "<bytes>  <path>" for every directory visited (deepest first)
 * and a grand total. The walk is depth-capped.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#define MAX_PATH   4096
#define MAX_DEPTH  8
#define MAX_ENTRY  1024

static const char *g_cwd = "/";
static char g_path[MAX_PATH];
static int g_rc;

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

/* Collapse "//", "." and ".." segments of an absolute path in place. */
static void canonicalize(char *p)
{
    int r = 0, w = 0;

    if (p[0] != '/')
        return;

    while (p[r]) {
        int seg, len;

        while (p[r] == '/')
            r++;
        if (!p[r])
            break;
        seg = r;
        while (p[r] && p[r] != '/')
            r++;
        len = r - seg;

        if (len == 1 && p[seg] == '.')
            continue;
        if (len == 2 && p[seg] == '.' && p[seg + 1] == '.') {
            while (w > 0 && p[w - 1] != '/')
                w--;
            if (w > 0)
                w--;
            continue;
        }
        /* w <= seg always holds, so this forward copy cannot clobber. */
        p[w++] = '/';
        while (len--)
            p[w++] = p[seg++];
    }
    if (w == 0)
        p[w++] = '/';
    p[w] = '\0';
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
    canonicalize(out);
}

/* Append "/name" to g_path at offset len; -1 if it would not fit. */
static int path_push(int len, const char *name)
{
    int n = (int)strlen(name);

    if (len + n + 2 > MAX_PATH)
        return -1;
    if (!(len == 1 && g_path[0] == '/'))
        g_path[len++] = '/';
    memcpy(g_path + len, name, (unsigned long)n);
    len += n;
    g_path[len] = '\0';
    return len;
}

/* Sum the bytes below g_path (which names a directory) and print the
 * running totals. Returns the byte total. */
static unsigned long walk(int len, int depth)
{
    struct k_dirent de;
    unsigned long total = 0;
    int i;

    /* Say so when the cap bites: a silent stop reports a wrong total. */
    if (depth >= MAX_DEPTH) {
        printf("du: %s: max depth %d reached, not descending\n",
               g_path, MAX_DEPTH);
        printf("%10lu  %s\n", total, g_path);
        return 0;
    }

    for (i = 0; i < MAX_ENTRY && readdir_at(g_path, i, &de) == 0; i++) {
        int nlen;

        if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
            continue;

        if (!de.is_dir) {
            total += de.size;
            continue;
        }

        nlen = path_push(len, de.name);
        if (nlen < 0)
            continue;
        total += walk(nlen, depth + 1);
        g_path[len] = '\0';
    }

    printf("%10lu  %s\n", total, g_path);
    return total;
}

static unsigned long du_one(const char *arg)
{
    struct k_stat st;

    resolve(arg, g_path, sizeof(g_path));
    if (stat_(g_path, &st) != 0) {
        printf("du: %s: no such file or directory\n", g_path);
        g_rc = 1;
        return 0;
    }
    if (!st.is_dir) {
        printf("%10u  %s\n", st.size, g_path);
        return st.size;
    }
    return walk((int)strlen(g_path), 0);
}

int main(int argc, char **argv)
{
    unsigned long total = 0;
    int i, paths = 0;

    argc = strip_cwd_arg(argc, argv);

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            printf("usage: du [path...]\n");
            return 1;
        }
        paths++;
    }

    if (paths == 0) {
        du_one(g_cwd);
        return g_rc;
    }

    for (i = 1; i < argc; i++)
        total += du_one(argv[i]);

    if (paths > 1)
        printf("%10lu  total\n", total);
    return g_rc;
}
