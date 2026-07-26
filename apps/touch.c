/* touch.c - create empty files.
 *
 * KFS has no timestamps, so an existing file is simply left alone.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#define MAX_PATH 256

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

int main(int argc, char **argv)
{
    char path[MAX_PATH];
    struct k_stat st;
    int i, fd, rc = 0;

    argc = strip_cwd_arg(argc, argv);
    if (argc < 2) {
        printf("usage: touch <file>...\n");
        return 1;
    }

    for (i = 1; i < argc; i++) {
        resolve(argv[i], path, sizeof(path));
        if (stat_(path, &st) == 0)
            continue;   /* already exists */

        fd = open(path, O_WRONLY | O_CREAT);
        if (fd < 0) {
            printf("touch: cannot create %s\n", path);
            rc = 1;
            continue;
        }
        close(fd);
    }
    return rc;
}
