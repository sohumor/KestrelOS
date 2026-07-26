/* ls.c - list directory contents.
 *
 * usage: ls [-a] [path...]
 * Always long-style: "TYPE  SIZE  NAME", directories first (marked
 * 'd'), aligned columns, trailing entry count. "." and ".." are hidden
 * (and not counted) unless -a is given. With no path arguments the
 * shell-provided cwd is listed.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#define MAX_ENTRIES 256
#define MAX_PATH    256

static const char *g_cwd = "/";

/* Pull the shell-injected trailing "--cwd=<path>" argument, if any. */
static int strip_cwd_arg(int argc, char **argv)
{
    if (argc > 1 && strncmp(argv[argc - 1], "--cwd=", 6) == 0) {
        g_cwd = argv[argc - 1] + 6;
        return argc - 1;
    }
    return argc;
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

static int is_dot(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static int list_path(const char *path, int show_all)
{
    static struct k_dirent ents[MAX_ENTRIES];
    struct k_stat st;
    int n = 0, i, pass, count = 0;

    while (n < MAX_ENTRIES && readdir_at(path, n, &ents[n]) == 0)
        n++;

    if (n == 0) {
        if (stat_(path, &st) != 0) {
            printf("ls: cannot access %s\n", path);
            return 1;
        }
        if (!st.is_dir) {
            printf("- %8u  %s\n", st.size, path);
            printf("1 entry\n");
            return 0;
        }
    }

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < n; i++) {
            if ((pass == 0) != (ents[i].is_dir != 0))
                continue;
            if (!show_all && is_dot(ents[i].name))
                continue;
            printf("%c %8u  %s\n", ents[i].is_dir ? 'd' : '-',
                   ents[i].size, ents[i].name);
            count++;
        }
    }
    printf("%d entr%s\n", count, count == 1 ? "y" : "ies");
    return 0;
}

int main(int argc, char **argv)
{
    char path[MAX_PATH];
    int i, rc = 0, show_all = 0, paths = 0;

    argc = strip_cwd_arg(argc, argv);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            show_all = 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            printf("usage: ls [-a] [path...]\n");
            return 1;
        } else {
            paths++;
        }
    }

    if (paths == 0)
        return list_path(g_cwd, show_all);

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0')
            continue;
        resolve(argv[i], path, sizeof(path));
        if (paths > 1)
            printf("%s:\n", path);
        if (list_path(path, show_all) != 0)
            rc = 1;
    }
    return rc;
}
