/* groups.c - list the groups an account belongs to.
 *
 * That is the account's primary group (the gid in /etc/passwd) plus
 * every group in /etc/group whose member list names it.
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

static int in_members(const char *members, const char *user)
{
    char name[FIELD_MAX];
    unsigned long n = 0;

    for (;;) {
        if (*members != ',' && *members != '\0') {
            if (n + 1 < sizeof(name))
                name[n++] = *members;
            members++;
            continue;
        }
        name[n] = '\0';
        if (name[0] != '\0' && strcmp(name, user) == 0)
            return 1;
        n = 0;
        if (*members == '\0')
            return 0;
        members++;
    }
}

/* Resolve a username to its primary gid; -1 if it is not in /etc/passwd. */
static int primary_gid(const char *user)
{
    char buf[DB_MAX];
    char *p;
    char *line;
    char *f[6];

    if (slurp(PASSWD_PATH, buf, sizeof(buf)) < 0)
        return -1;
    p = buf;
    while ((line = db_next(&p)) != 0) {
        if (split_fields(line, f, 6) != 6)
            continue;
        if (strcmp(f[0], user) == 0)
            return atoi(f[2]);
    }
    return -1;
}

static void name_for_uid(int uid, char *out, unsigned long outsz)
{
    char buf[DB_MAX];
    char *p;
    char *line;
    char *f[6];

    snprintf(out, outsz, "%d", uid);
    if (slurp(PASSWD_PATH, buf, sizeof(buf)) < 0)
        return;
    p = buf;
    while ((line = db_next(&p)) != 0) {
        if (split_fields(line, f, 6) != 6)
            continue;
        if (atoi(f[1]) == uid) {
            copy_field(out, f[0], outsz);
            return;
        }
    }
}

int main(int argc, char **argv)
{
    char buf[DB_MAX];
    char user[FIELD_MAX];
    char *p;
    char *line;
    char *f[3];
    int gid;
    int printed = 0;

    argc = strip_cwd_arg(argc, argv);
    if (argc > 2) {
        printf("usage: groups [username]\n");
        return 1;
    }

    if (argc == 2) {
        copy_field(user, argv[1], sizeof(user));
        gid = primary_gid(user);
        if (gid < 0) {
            printf("groups: unknown user %s\n", user);
            return 1;
        }
    } else {
        name_for_uid((int)syscall(SYS_GETUID, 0, 0, 0, 0), user, sizeof(user));
        gid = (int)syscall(SYS_GETGID, 0, 0, 0, 0);
    }

    if (slurp(GROUP_PATH, buf, sizeof(buf)) < 0) {
        printf("%d\n", gid);
        return 0;
    }
    p = buf;
    while ((line = db_next(&p)) != 0) {
        if (split_fields(line, f, 3) != 3)
            continue;
        if (atoi(f[1]) != gid && !in_members(f[2], user))
            continue;
        printf("%s%s", printed ? " " : "", f[0]);
        printed = 1;
    }
    if (!printed)
        printf("%d", gid);          /* a gid with no group entry */
    printf("\n");
    return 0;
}
