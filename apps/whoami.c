/* whoami.c - print the name of the current account. */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PASSWD_PATH "/etc/passwd"
#define DB_MAX     4096

static int strip_cwd_arg(int argc, char **argv)
{
    if (argc > 1 && strncmp(argv[argc - 1], "--cwd=", 6) == 0)
        return argc - 1;
    return argc;
}

static long slurp(const char *path, char *buf, unsigned long max)
{
    long total = 0;
    long n;
    int fd;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    while ((unsigned long)total < max - 1) {
        n = read(fd, buf + total, max - 1 - (unsigned long)total);
        if (n <= 0)
            break;
        total += n;
    }
    close(fd);
    buf[total] = '\0';
    return total;
}

static char *db_next(char **p)
{
    char *s;
    char *e;

    for (;;) {
        s = *p;
        if (*s == '\0')
            return 0;
        for (e = s; *e != '\0' && *e != '\n'; e++)
            ;
        if (*e == '\n') {
            *e = '\0';
            *p = e + 1;
        } else {
            *p = e;
        }
        if (e > s && e[-1] == '\r')
            e[-1] = '\0';
        if (*s == '\0' || *s == '#')
            continue;
        return s;
    }
}

static int split_fields(char *line, char **f, int maxf)
{
    int n = 0;

    if (maxf <= 0)
        return 0;
    f[n++] = line;
    while (*line != '\0' && n < maxf) {
        if (*line == ':') {
            *line++ = '\0';
            f[n++] = line;
        } else {
            line++;
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    char buf[DB_MAX];
    char *p;
    char *line;
    char *f[6];
    int uid;

    argc = strip_cwd_arg(argc, argv);
    if (argc > 1) {
        printf("usage: whoami\n");
        return 1;
    }

    uid = (int)syscall(SYS_GETUID, 0, 0, 0, 0);
    if (slurp(PASSWD_PATH, buf, sizeof(buf)) >= 0) {
        p = buf;
        while ((line = db_next(&p)) != 0) {
            if (split_fields(line, f, 6) != 6)
                continue;
            if (atoi(f[1]) == uid) {
                printf("%s\n", f[0]);
                return 0;
            }
        }
    }
    /* No /etc/passwd, or nobody owns this uid: the number is still the
     * honest answer. */
    printf("%d\n", uid);
    return 0;
}
