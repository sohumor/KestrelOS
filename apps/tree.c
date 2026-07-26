/* tree.c - indented recursive directory listing.
 *
 * usage: tree [path]
 * Depth-capped like find/du. Drawn with plain ASCII so the output reads
 * the same on the VGA console and on a serial terminal.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#define MAX_PATH   4096
#define MAX_DEPTH  8
#define MAX_ENTRY  1024
#define PREFIX_SZ  (MAX_DEPTH * 4 + 1)

#define BOX_TEE    "|-- "
#define BOX_ELBOW  "`-- "
#define BOX_BAR    "|   "
#define BOX_GAP    "    "

static const char *g_cwd = "/";
static char g_path[MAX_PATH];
static char g_prefix[PREFIX_SZ];
static int g_dirs, g_files;

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

static int is_dot(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

/* Highest readdir index that is not "." or ".."; -1 if the dir is empty.
 * Recursing into the dot entries would walk the tree forever. */
static int last_real_index(void)
{
    struct k_dirent de;
    int i, last = -1;

    for (i = 0; i < MAX_ENTRY && readdir_at(g_path, i, &de) == 0; i++)
        if (!is_dot(de.name))
            last = i;
    return last;
}

static void walk(int len, int depth, int plen)
{
    struct k_dirent de;
    int n, i;

    if (depth >= MAX_DEPTH)
        return;

    n = last_real_index();
    for (i = 0; i <= n; i++) {
        int nlen, last;

        if (readdir_at(g_path, i, &de) != 0)
            break;
        if (is_dot(de.name))
            continue;
        last = (i == n);

        printf("%s%s%s\n", g_prefix, last ? BOX_ELBOW : BOX_TEE, de.name);

        if (de.is_dir)
            g_dirs++;
        else
            g_files++;

        if (!de.is_dir)
            continue;

        nlen = path_push(len, de.name);
        if (nlen < 0)
            continue;

        if (plen + 4 < PREFIX_SZ) {
            memcpy(g_prefix + plen, last ? BOX_GAP : BOX_BAR, 4);
            g_prefix[plen + 4] = '\0';
            walk(nlen, depth + 1, plen + 4);
            g_prefix[plen] = '\0';
        }
        g_path[len] = '\0';
    }
}

int main(int argc, char **argv)
{
    struct k_stat st;
    const char *root = 0;
    int i;

    argc = strip_cwd_arg(argc, argv);

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            printf("usage: tree [path]\n");
            return 1;
        }
        if (root) {
            printf("usage: tree [path]\n");
            return 1;
        }
        root = argv[i];
    }

    if (root)
        resolve(root, g_path, sizeof(g_path));
    else
        snprintf(g_path, sizeof(g_path), "%s", g_cwd);

    if (stat_(g_path, &st) != 0) {
        printf("tree: %s: no such file or directory\n", g_path);
        return 1;
    }

    printf("%s\n", g_path);
    if (st.is_dir) {
        g_prefix[0] = '\0';
        walk((int)strlen(g_path), 0, 0);
    }

    printf("\n%d director%s, %d file%s\n", g_dirs,
           g_dirs == 1 ? "y" : "ies", g_files, g_files == 1 ? "" : "s");
    return 0;
}
