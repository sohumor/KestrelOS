/* cat.c - concatenate files to the console. */

#include <kestrel.h>
#include <kestrel_cwd.h>
#include <stdio.h>
#include <string.h>

#define MAX_PATH 256

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

int main(int argc, char **argv)
{
    char buf[512];
    char path[MAX_PATH];
    struct k_stat st;
    long n;
    int i, fd, rc = 0;

    g_cwd = kestrel_startup_cwd();
    if (argc < 2) {
        printf("usage: cat <file>...\n");
        return 1;
    }

    for (i = 1; i < argc; i++) {
        resolve(argv[i], path, sizeof(path));
        /* Dumping raw directory entries would spray escape bytes at the
         * console, so refuse directories the way cp/mv do. */
        if (stat_(path, &st) == 0 && st.is_dir) {
            printf("cat: %s: is a directory\n", path);
            rc = 1;
            continue;
        }
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            printf("cat: cannot open %s\n", path);
            rc = 1;
            continue;
        }
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, (unsigned long)n);
        close(fd);
    }
    return rc;
}
