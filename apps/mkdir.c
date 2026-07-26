/* mkdir.c - create directories. */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#define MAX_PATH 256

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

int main(int argc, char **argv)
{
    char path[MAX_PATH];
    int i, rc = 0;

    argc = strip_cwd_arg(argc, argv);
    if (argc < 2) {
        printf("usage: mkdir <path>...\n");
        return 1;
    }

    for (i = 1; i < argc; i++) {
        resolve(argv[i], path, sizeof(path));
        if (mkdir_(path) != 0) {
            printf("mkdir: cannot create %s\n", path);
            rc = 1;
        }
    }
    return rc;
}
