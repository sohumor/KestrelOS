/* chown.c - change the owner and/or group of a file.
 *
 *   chown kestrel notes.txt
 *   chown kestrel:users notes.txt
 *   chown :wheel notes.txt
 *   chown 1000:1000 notes.txt
 *
 * Names are resolved through /etc/passwd and /etc/group; a plain number
 * is taken as the id itself, so this still works on a system whose
 * account files are missing. SYS_CHOWN always takes both ids, so the
 * half that is not being changed is read back with stat() first.
 *
 * The kernel refuses the call for anyone but root; that error is
 * reported, not worked around.
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PASSWD_PATH "/etc/passwd"
#define GROUP_PATH  "/etc/group"
#define DB_MAX     4096
#define FIELD_MAX  128
#define MAX_PATH   256

static const char *g_cwd = "/";

static int sc_chown(const char *path, int uid, int gid)
{
    return (int)syscall(SYS_CHOWN, (long)path, uid, gid, 0);
}

static int strip_cwd_arg(int argc, char **argv)
{
    if (argc > 1 && strncmp(argv[argc - 1], "--cwd=", 6) == 0) {
        g_cwd = argv[argc - 1] + 6;
        return argc - 1;
    }
    return argc;
}

static void resolve(const char *tok, char *out, unsigned long outsz)
{
    if (tok[0] == '/')
        snprintf(out, outsz, "%s", tok);
    else if (g_cwd[0] == '/' && g_cwd[1] == '\0')
        snprintf(out, outsz, "/%s", tok);
    else
        snprintf(out, outsz, "%s/%s", g_cwd, tok);
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

static int all_digits(const char *s)
{
    if (*s == '\0')
        return 0;
    for (; *s != '\0'; s++)
        if (*s < '0' || *s > '9')
            return 0;
    return 1;
}

/* Resolve a name (or a number) against `path`, taking field `idfield`
 * as the id. Returns the id, or -1 if it cannot be resolved. */
static int lookup_id(const char *path, const char *name, int nfields,
                     int idfield)
{
    char buf[DB_MAX];
    char *p;
    char *line;
    char *f[6];

    if (all_digits(name))
        return atoi(name);
    if (nfields > (int)(sizeof(f) / sizeof(f[0])))
        return -1;
    if (slurp(path, buf, sizeof(buf)) < 0)
        return -1;
    p = buf;
    while ((line = db_next(&p)) != 0) {
        if (split_fields(line, f, nfields) != nfields)
            continue;
        if (strcmp(f[0], name) == 0)
            return atoi(f[idfield]);
    }
    return -1;
}

int main(int argc, char **argv)
{
    char spec[FIELD_MAX];
    char path[MAX_PATH];
    struct k_stat st;
    char *colon;
    const char *user;
    const char *group;
    int uid = -1;
    int gid = -1;
    int i;
    int rc = 0;

    argc = strip_cwd_arg(argc, argv);
    if (argc < 3) {
        printf("usage: chown <user>[:<group>] <file>...\n");
        printf("       chown :<group> <file>...\n");
        return 1;
    }

    snprintf(spec, sizeof(spec), "%s", argv[1]);
    colon = strchr(spec, ':');
    if (colon != 0) {
        *colon = '\0';
        group = colon + 1;
    } else {
        group = "";
    }
    user = spec;

    if (user[0] != '\0') {
        uid = lookup_id(PASSWD_PATH, user, 6, 1);
        if (uid < 0) {
            printf("chown: unknown user %s\n", user);
            return 1;
        }
    }
    if (group[0] != '\0') {
        gid = lookup_id(GROUP_PATH, group, 3, 1);
        if (gid < 0) {
            printf("chown: unknown group %s\n", group);
            return 1;
        }
    }
    if (uid < 0 && gid < 0) {
        printf("chown: nothing to change\n");
        return 1;
    }

    for (i = 2; i < argc; i++) {
        int new_uid = uid;
        int new_gid = gid;

        resolve(argv[i], path, sizeof(path));
        if (new_uid < 0 || new_gid < 0) {
            /* Fill in whichever half is staying put. */
            if (stat_(path, &st) != 0) {
                printf("chown: cannot stat %s\n", path);
                rc = 1;
                continue;
            }
            if (new_uid < 0)
                new_uid = (int)st.uid;
            if (new_gid < 0)
                new_gid = (int)st.gid;
        }
        if (sc_chown(path, new_uid, new_gid) != 0) {
            printf("chown: cannot change %s (only root may)\n", path);
            rc = 1;
        }
    }
    return rc;
}
