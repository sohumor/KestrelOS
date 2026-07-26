/* writefile.c - read raw console input until ctrl-D, then write it to
 * a file with a single vfs write. Prints the byte count written. */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
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
    char *buf;
    unsigned long cap = 4096, len = 0;
    unsigned char ch;
    long n;
    int fd;

    argc = strip_cwd_arg(argc, argv);
    if (argc != 2) {
        printf("usage: writefile <path>\n");
        return 1;
    }
    resolve(argv[1], path, sizeof(path));

    buf = malloc(cap);
    if (!buf) {
        printf("writefile: out of memory\n");
        return 1;
    }

    printf("writefile: type text, finish with ctrl-D\n");
    for (;;) {
        n = read(0, &ch, 1);
        if (n <= 0)
            break;
        if (ch == 0x04)          /* ctrl-D ends input */
            break;
        if (ch >= 0x80)          /* ignore special key codes */
            continue;
        if (ch == 8) {           /* backspace */
            if (len > 0) {
                len--;
                write(1, "\b \b", 3);
            }
            continue;
        }
        if (len + 1 > cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) {
                printf("writefile: out of memory\n");
                return 1;
            }
        }
        buf[len++] = (char)ch;
        write(1, &ch, 1);        /* echo */
    }

    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY);
    if (fd < 0) {
        printf("writefile: cannot open %s\n", path);
        free(buf);
        return 1;
    }
    n = write(fd, buf, len);
    close(fd);
    free(buf);

    if (n < 0 || (unsigned long)n != len) {
        printf("\nwritefile: write failed\n");
        return 1;
    }
    printf("\nwrote %lu bytes to %s\n", len, path);
    return 0;
}
