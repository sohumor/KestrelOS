/* userdel.c - remove an account (root only).
 *
 * Drops the account's line from /etc/passwd and /etc/shadow, drops its
 * own group from /etc/group and takes its name out of every other
 * group's member list. With -r the home directory is removed too.
 *
 * uid 0 is refused outright: an OS with no root account cannot be
 * repaired from inside itself.
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PASSWD_PATH "/etc/passwd"
#define SHADOW_PATH "/etc/shadow"
#define GROUP_PATH  "/etc/group"

#define DB_MAX     4096
#define FIELD_MAX  128
#define LINE_MAX   512
#define MAX_PATH   256
#define RM_DEPTH   4          /* how deep -r will recurse */
#define RM_ENTRIES 32         /* names collected per directory */

static int sc_getuid(void)
{
    return (int)syscall(SYS_GETUID, 0, 0, 0, 0);
}

static int sc_chmod(const char *path, int mode)
{
    return (int)syscall(SYS_CHMOD, (long)path, mode, 0, 0);
}

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

static int line_is_user(const char *line, const char *user)
{
    unsigned long i;

    if (line[0] == '#')
        return 0;
    for (i = 0; user[i] != '\0'; i++)
        if (line[i] != user[i])
            return 0;
    return line[i] == ':';
}

static int out_line(char *out, unsigned long max, unsigned long *len,
                    const char *s)
{
    while (*s != '\0') {
        if (*len + 2 > max)
            return -1;
        out[(*len)++] = *s++;
    }
    if (*len + 2 > max)
        return -1;
    out[(*len)++] = '\n';
    out[*len] = '\0';
    return 0;
}

static int write_file(const char *path, const char *data, unsigned long len,
                      int mode)
{
    long n;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);

    if (fd < 0)
        return -1;
    n = write(fd, data, len);
    close(fd);
    if (n < 0 || (unsigned long)n != len)
        return -1;
    sc_chmod(path, mode);
    return 0;
}

/* Drop `user`'s line from `path`. Returns 1 if a line went away. */
static int db_delete(const char *path, const char *user, int mode)
{
    char buf[DB_MAX];
    char out[DB_MAX];
    unsigned long len = 0;
    char *p;
    char *e;
    int removed = 0;

    if (slurp(path, buf, sizeof(buf)) < 0)
        return 0;
    out[0] = '\0';

    p = buf;
    while (*p != '\0') {
        char *line = p;

        for (e = p; *e != '\0' && *e != '\n'; e++)
            ;
        if (*e == '\n') {
            *e = '\0';
            p = e + 1;
        } else {
            p = e;
        }
        if (e > line && e[-1] == '\r')
            e[-1] = '\0';
        if (line_is_user(line, user)) {
            removed = 1;
            continue;
        }
        if (out_line(out, sizeof(out), &len, line) != 0)
            return 0;
    }
    if (!removed)
        return 0;
    return write_file(path, out, len, mode) == 0;
}

/* Copy a comma-separated member list, dropping `user`. */
static void members_without(const char *members, const char *user,
                            char *out, unsigned long outsz)
{
    char name[FIELD_MAX];
    unsigned long len = 0;
    unsigned long n = 0;

    out[0] = '\0';
    for (;;) {
        if (*members != ',' && *members != '\0') {
            if (n + 1 < sizeof(name))
                name[n++] = *members;
            members++;
            continue;
        }
        name[n] = '\0';
        if (name[0] != '\0' && strcmp(name, user) != 0) {
            unsigned long i;

            if (len != 0 && len + 1 < outsz)
                out[len++] = ',';
            for (i = 0; name[i] != '\0' && len + 1 < outsz; i++)
                out[len++] = name[i];
            out[len] = '\0';
        }
        n = 0;
        if (*members == '\0')
            break;
        members++;
    }
    out[len] = '\0';
}

/* Delete the group named `user` and remove `user` from every other
 * group's member list. */
static int group_purge(const char *user)
{
    char buf[DB_MAX];
    char out[DB_MAX];
    char work[LINE_MAX];
    char members[LINE_MAX];
    char rebuilt[LINE_MAX];
    unsigned long len = 0;
    char *p;
    char *e;
    char *f[3];

    if (slurp(GROUP_PATH, buf, sizeof(buf)) < 0)
        return 0;
    out[0] = '\0';

    p = buf;
    while (*p != '\0') {
        char *line = p;

        for (e = p; *e != '\0' && *e != '\n'; e++)
            ;
        if (*e == '\n') {
            *e = '\0';
            p = e + 1;
        } else {
            p = e;
        }
        if (e > line && e[-1] == '\r')
            e[-1] = '\0';

        if (line[0] == '#' || line[0] == '\0') {
            if (out_line(out, sizeof(out), &len, line) != 0)
                return 0;
            continue;
        }
        if (line_is_user(line, user))
            continue;                       /* the account's own group */

        copy_field(work, line, sizeof(work));
        if (split_fields(work, f, 3) != 3) {
            /* malformed: keep it as it is rather than mangle it */
            if (out_line(out, sizeof(out), &len, line) != 0)
                return 0;
            continue;
        }
        members_without(f[2], user, members, sizeof(members));
        snprintf(rebuilt, sizeof(rebuilt), "%s:%s:%s", f[0], f[1], members);
        if (out_line(out, sizeof(out), &len, rebuilt) != 0)
            return 0;
    }
    return write_file(GROUP_PATH, out, len, 0644) == 0;
}

/* Remove a file, or a directory and everything under it. */
static int rm_rf(const char *path, int depth)
{
    char names[RM_ENTRIES][60];
    char child[MAX_PATH];
    struct k_dirent de;
    struct k_stat st;
    int n = 0;
    int i;
    int rc = 0;

    if (depth > RM_DEPTH)
        return -1;
    if (stat_(path, &st) != 0)
        return -1;
    if (st.is_dir) {
        for (i = 0; n < RM_ENTRIES && readdir_at(path, i, &de) == 0; i++) {
            if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
                continue;
            copy_field(names[n], de.name, sizeof(names[n]));
            n++;
        }
        for (i = 0; i < n; i++) {
            snprintf(child, sizeof(child), "%s/%s", path, names[i]);
            if (rm_rf(child, depth + 1) != 0)
                rc = -1;
        }
    }
    if (unlink_(path) != 0)
        rc = -1;
    return rc;
}

/* Find `user`'s uid and home in /etc/passwd. 1 on success. */
static int lookup(const char *user, int *uid, char *home, unsigned long homesz)
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
        copy_field(home, f[4], homesz);
        return 1;
    }
    return 0;
}

/* Refuse to hand rm_rf anything that is not plausibly a home. */
static int home_is_safe(const char *home)
{
    int slashes = 0;
    unsigned long i;

    if (home[0] != '/' || home[1] == '\0')
        return 0;
    for (i = 0; home[i] != '\0'; i++)
        if (home[i] == '/')
            slashes++;
    /* at least two components, e.g. /home/ana or /root/ana - never /etc */
    return slashes >= 2;
}

int main(int argc, char **argv)
{
    char home[FIELD_MAX];
    const char *name = 0;
    struct k_stat st;
    int remove_home = 0;
    int uid = -1;
    int i;
    int rc = 0;

    argc = strip_cwd_arg(argc, argv);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            remove_home = 1;
        } else if (argv[i][0] == '-' || name != 0) {
            printf("usage: userdel [-r] <username>\n");
            return 1;
        } else {
            name = argv[i];
        }
    }
    if (name == 0) {
        printf("usage: userdel [-r] <username>\n");
        return 1;
    }

    if (sc_getuid() != 0) {
        printf("userdel: only root can remove accounts\n");
        return 1;
    }
    if (!lookup(name, &uid, home, sizeof(home))) {
        printf("userdel: unknown user %s\n", name);
        return 1;
    }
    if (uid == 0) {
        printf("userdel: refusing to remove uid 0 (%s)\n", name);
        return 1;
    }

    if (!db_delete(PASSWD_PATH, name, 0644)) {
        printf("userdel: cannot update %s\n", PASSWD_PATH);
        return 1;
    }
    if (!db_delete(SHADOW_PATH, name, 0600))
        printf("userdel: warning: no %s entry removed\n", SHADOW_PATH);
    if (!group_purge(name))
        printf("userdel: warning: cannot update %s\n", GROUP_PATH);

    if (remove_home) {
        if (!home_is_safe(home)) {
            printf("userdel: refusing to remove %s\n", home);
            rc = 1;
        } else if (stat_(home, &st) != 0) {
            printf("userdel: %s does not exist\n", home);
        } else if (rm_rf(home, 0) != 0) {
            printf("userdel: could not fully remove %s\n", home);
            rc = 1;
        }
    }

    printf("userdel: removed %s (uid %d)\n", name, uid);
    return rc;
}
