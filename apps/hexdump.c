/* hexdump.c - canonical hex + ascii dump, 16 bytes per row. */

#include <kestrel.h>
#include <kestrel_cwd.h>
#include <stdio.h>
#include <string.h>

#define MAX_PATH 256

static const char *g_cwd;

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

static void dump_row(unsigned long off, const unsigned char *row, int n)
{
    int i;

    printf("%08lx  ", off);
    for (i = 0; i < 16; i++) {
        if (i == 8)
            putchar(' ');
        if (i < n)
            printf("%02x ", row[i]);
        else
            printf("   ");
    }
    printf(" |");
    for (i = 0; i < n; i++)
        putchar((row[i] >= 32 && row[i] < 127) ? row[i] : '.');
    printf("|\n");
}

static int dump_file(const char *path)
{
    unsigned char buf[512];
    unsigned char row[16];
    unsigned long off = 0;
    long n, i;
    int rowlen = 0, fd;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("hexdump: cannot open %s\n", path);
        return 1;
    }

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < n; i++) {
            row[rowlen++] = buf[i];
            if (rowlen == 16) {
                dump_row(off, row, 16);
                off += 16;
                rowlen = 0;
            }
        }
    }
    if (rowlen > 0) {
        dump_row(off, row, rowlen);
        off += (unsigned long)rowlen;
    }
    printf("%08lx\n", off);
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    char path[MAX_PATH];
    int i, rc = 0;

    g_cwd = kestrel_startup_cwd();
    if (argc < 2) {
        printf("usage: hexdump <file>...\n");
        return 1;
    }

    for (i = 1; i < argc; i++) {
        resolve(argv[i], path, sizeof(path));
        if (argc > 2)
            printf("%s:\n", path);
        if (dump_file(path) != 0)
            rc = 1;
    }
    return rc;
}
