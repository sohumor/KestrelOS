/* ls.c - list directory contents.
 *
 * Always long-style: "TYPE  SIZE  NAME", directories first (marked
 * 'd'), aligned columns, trailing entry count. With no arguments the
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

static int list_path(const char *path)
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
    int i, rc = 0;

    argc = strip_cwd_arg(argc, argv);

    if (argc < 2)
        return list_path(g_cwd);

    for (i = 1; i < argc; i++) {
        resolve(argv[i], path, sizeof(path));
        if (argc > 2)
            printf("%s:\n", path);
        if (list_path(path) != 0)
            rc = 1;
    }
    return rc;
}
