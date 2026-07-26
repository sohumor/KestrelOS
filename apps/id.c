/* id.c - report an account's uid, gid and group membership.
 *
 *   $ id
 *   uid=1000(kestrel) gid=1000(kestrel) groups=100(users),1000(kestrel)
 *
 * With no argument it reports the running process's own credentials as
 * the kernel sees them (SYS_GETUID / SYS_GETGID) and names them from
 * /etc/passwd; with a username it reports what the files say about that
 * account.
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

/* Is `user` named in a comma-separated member list? */
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

/* Look `user` up in /etc/passwd. 1 on success. */
static int find_user(const char *user, int *uid, int *gid)
{
    char buf[DB_MAX];
    char *p;
    char *line;
    char *f[6];

    if (slurp(PASSWD_PATH, buf, sizeof(buf)) < 0)
        return 0;
    p = buf;
    while ((line = db_next(&p)) != 0) {
        if (split_fields(line, f, 6) != 6)
            continue;
        if (strcmp(f[0], user) != 0)
            continue;
        *uid = atoi(f[1]);
        *gid = atoi(f[2]);
        return 1;
    }
    return 0;
}

/* The account owning `uid`; falls back to the number as a string. */
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

/* Print every group the account belongs to: its primary gid plus every
 * group whose member list names it. */
static void print_groups(const char *user, int gid)
{
    char buf[DB_MAX];
    char primary[FIELD_MAX];
    char *p;
    char *line;
    char *f[3];

    /* The primary group always comes first, so every later entry can be
     * printed with a leading comma. */
    name_for_gid(gid, primary, sizeof(primary));
    printf(" groups=%d(%s)", gid, primary);

    if (slurp(GROUP_PATH, buf, sizeof(buf)) < 0) {
        printf("\n");
        return;
    }
    p = buf;
    while ((line = db_next(&p)) != 0) {
        int g;

        if (split_fields(line, f, 3) != 3)
            continue;
        g = atoi(f[1]);
        if (g == gid)
            continue;                       /* already printed as primary */
        if (!in_members(f[2], user))
            continue;
        printf(",%d(%s)", g, f[0]);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    char user[FIELD_MAX];
    char gname[FIELD_MAX];
    int uid;
    int gid;

    argc = strip_cwd_arg(argc, argv);
    if (argc > 2) {
        printf("usage: id [username]\n");
        return 1;
    }

    if (argc == 2) {
        if (!find_user(argv[1], &uid, &gid)) {
            printf("id: unknown user %s\n", argv[1]);
            return 1;
        }
        copy_field(user, argv[1], sizeof(user));
    } else {
        uid = (int)syscall(SYS_GETUID, 0, 0, 0, 0);
        gid = (int)syscall(SYS_GETGID, 0, 0, 0, 0);
        name_for_uid(uid, user, sizeof(user));
    }

    name_for_gid(gid, gname, sizeof(gname));
    printf("uid=%d(%s) gid=%d(%s)", uid, user, gid, gname);
    print_groups(user, gid);
    return 0;
}
