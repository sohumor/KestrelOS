/* lsusers.c - list the accounts in /etc/passwd.
 *
 *   $ lsusers
 *   UID    GID    USER      GROUP     HOME            SHELL      NAME
 *   0      0      root      root      /root           /bin/sh    System Administrator
 *
 * With -g it lists /etc/group instead. Nothing here reads /etc/shadow,
 * so it works for any user.
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PASSWD_PATH "/etc/passwd"
#define GROUP_PATH  "/etc/group"
#define DB_MAX     4096
#define FIELD_MAX  128

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

static void copy_field(char *dst, const char *src, unsigned long dstsz)
{
    unsigned long i;

    for (i = 0; i + 1 < dstsz && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static void name_for_gid(int gid, char *out, unsigned long outsz)
{
    char buf[DB_MAX];
    char *p;
    char *line;
    char *f[3];

    snprintf(out, outsz, "%d", gid);
    if (slurp(GROUP_PATH, buf, sizeof(buf)) < 0)
        return;
    p = buf;
    while ((line = db_next(&p)) != 0) {
        if (split_fields(line, f, 3) != 3)
            continue;
        if (atoi(f[1]) == gid) {
            copy_field(out, f[0], outsz);
            return;
        }
    }
}

static int list_groups(void)
{
    char buf[DB_MAX];
    char *p;
    char *line;
    char *f[3];

    if (slurp(GROUP_PATH, buf, sizeof(buf)) < 0) {
        printf("lsusers: cannot read %s\n", GROUP_PATH);
        return 1;
    }
    printf("%-6s %-10s %s\n", "GID", "GROUP", "MEMBERS");
    p = buf;
    while ((line = db_next(&p)) != 0) {
        if (split_fields(line, f, 3) != 3)
            continue;
        printf("%-6d %-10s %s\n", atoi(f[1]), f[0], f[2]);
    }
    return 0;
}

static int list_users(void)
{
    char buf[DB_MAX];
    char gname[FIELD_MAX];
    char *p;
    char *line;
    char *f[6];

    if (slurp(PASSWD_PATH, buf, sizeof(buf)) < 0) {
        printf("lsusers: cannot read %s\n", PASSWD_PATH);
        return 1;
    }
    printf("%-6s %-6s %-10s %-10s %-16s %-10s %s\n",
           "UID", "GID", "USER", "GROUP", "HOME", "SHELL", "NAME");
    p = buf;
    while ((line = db_next(&p)) != 0) {
        int gid;

        if (split_fields(line, f, 6) != 6)
            continue;                       /* malformed: skip quietly */
        gid = atoi(f[2]);
        name_for_gid(gid, gname, sizeof(gname));
        printf("%-6d %-6d %-10s %-10s %-16s %-10s %s\n",
               atoi(f[1]), gid, f[0], gname, f[4], f[5], f[3]);
    }
    return 0;
}

int main(int argc, char **argv)
{
    argc = strip_cwd_arg(argc, argv);
    if (argc == 2 && strcmp(argv[1], "-g") == 0)
        return list_groups();
    if (argc > 1) {
        printf("usage: lsusers [-g]\n");
        return 1;
    }
    return list_users();
}
