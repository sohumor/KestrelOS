/* find.c - walk a directory tree printing paths.
 *
 * usage: find [root] [-name SUBSTRING]
 * The walk is depth-capped and uses a single shared path buffer, so
 * neither the stack nor the heap can run away on a deep tree.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#define MAX_PATH   4096
#define MAX_DEPTH  8
#define MAX_ENTRY  1024

static const char *g_cwd = "/";
static const char *g_name;
static char g_path[MAX_PATH];

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
    return (s && s[1]) ? s + 1 : p;
}

/* Append "/name" to g_path at offset len. Returns the new length, or
 * -1 if it would not fit. */
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

static int matches(const char *name)
{
    return !g_name || strstr(name, g_name) != 0;
}

static void walk(int len, int depth)
{
    struct k_dirent de;
    int i;

    if (depth >= MAX_DEPTH)
        return;

    for (i = 0; i < MAX_ENTRY && readdir_at(g_path, i, &de) == 0; i++) {
        int nlen;

        if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
            continue;

        nlen = path_push(len, de.name);
        if (nlen < 0)
            continue;

        if (matches(de.name))
            printf("%s\n", g_path);
        if (de.is_dir)
            walk(nlen, depth + 1);

        g_path[len] = '\0';
    }
}

int main(int argc, char **argv)
{
    const char *root = 0;
    struct k_stat st;
    int i;

    argc = strip_cwd_arg(argc, argv);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-name") == 0) {
            if (i + 1 >= argc) {
                printf("usage: find [root] [-name SUBSTRING]\n");
                return 1;
            }
            g_name = argv[++i];
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            printf("usage: find [root] [-name SUBSTRING]\n");
            return 1;
        } else if (!root) {
            root = argv[i];
        } else {
            printf("usage: find [root] [-name SUBSTRING]\n");
            return 1;
        }
    }

    if (root)
        resolve(root, g_path, sizeof(g_path));
    else
        snprintf(g_path, sizeof(g_path), "%s", g_cwd);

    if (stat_(g_path, &st) != 0) {
        printf("find: %s: no such file or directory\n", g_path);
        return 1;
    }

    if (matches(basename_of(g_path)))
        printf("%s\n", g_path);

    if (st.is_dir)
        walk((int)strlen(g_path), 0);
    return 0;
}
