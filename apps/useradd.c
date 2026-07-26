/* useradd.c - create an account (root only).
 *
 * Appends a line to /etc/passwd, /etc/group and /etc/shadow, then makes
 * the home directory and hands it to the new owner. The password is read
 * twice with echo off and hashed exactly the way login checks it.
 *
 * Nothing is written until every check has passed and both password
 * prompts agree, so an aborted run leaves the account files as they were.
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sha256.h>

#define PASSWD_PATH "/etc/passwd"
#define SHADOW_PATH "/etc/shadow"
#define GROUP_PATH  "/etc/group"
#define RANDOM_PATH "/dev/random"

#define DB_MAX      4096
#define FIELD_MAX   128
#define NAME_MAX    32
#define SALT_DIGITS 16
#define ITERATIONS  4096
#define UID_MIN     1000
#define UID_MAX     60000

static int sc_getuid(void)
{
    return (int)syscall(SYS_GETUID, 0, 0, 0, 0);
}

static int sc_chown(const char *path, int uid, int gid)
{
    return (int)syscall(SYS_CHOWN, (long)path, uid, gid, 0);
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

/* Rewrite `path`, replacing `user`'s line with `repl` or appending it if
 * there is none. Comments and other entries are preserved verbatim. */
static int db_replace(const char *path, const char *user, const char *repl,
                      int mode)
{
    char buf[DB_MAX];
    char out[DB_MAX];
    unsigned long len = 0;
    char *p;
    char *e;
    int replaced = 0;
    int fd;
    long n;

    if (slurp(path, buf, sizeof(buf)) < 0)
        buf[0] = '\0';
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
            replaced = 1;
            if (out_line(out, sizeof(out), &len, repl) != 0)
                return -1;
        } else if (out_line(out, sizeof(out), &len, line) != 0) {
            return -1;
        }
    }
    if (!replaced && out_line(out, sizeof(out), &len, repl) != 0)
        return -1;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    n = write(fd, out, len);
    close(fd);
    if (n < 0 || (unsigned long)n != len)
        return -1;
    sc_chmod(path, mode);
    return 0;
}

/* Is `name` the first field of any line in `path`? */
static int db_has(const char *path, const char *name)
{
    char buf[DB_MAX];
    char *p;
    char *line;
    char *f[2];

    if (slurp(path, buf, sizeof(buf)) < 0)
        return 0;
    p = buf;
    while ((line = db_next(&p)) != 0) {
        split_fields(line, f, 2);
        if (strcmp(f[0], name) == 0)
            return 1;
    }
    return 0;
}

/* One past the highest ordinary uid in /etc/passwd, floored at UID_MIN. */
static int next_uid(void)
{
    char buf[DB_MAX];
    char *p;
    char *line;
    char *f[6];
    int best = UID_MIN - 1;

    if (slurp(PASSWD_PATH, buf, sizeof(buf)) >= 0) {
        p = buf;
        while ((line = db_next(&p)) != 0) {
            int uid;

            if (split_fields(line, f, 6) != 6)
                continue;
            uid = atoi(f[1]);
            if (uid >= UID_MIN && uid < UID_MAX && uid > best)
                best = uid;
        }
    }
    return best + 1;
}

static int read_password(const char *prompt, char *buf, int n)
{
    int len = 0;

    printf("%s", prompt);
    for (;;) {
        unsigned char c;
        long r = read(0, &c, 1);

        if (r <= 0) {
            putchar('\n');
            return -1;
        }
        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            putchar('\n');
            return len;
        }
        if (c == 3 || c == 4) {
            putchar('\n');
            return -1;
        }
        if (c == 8 || c == 127) {
            if (len > 0)
                len--;
            continue;
        }
        if (c == 21) {
            len = 0;
            continue;
        }
        if (c >= 0x80 || c < 32)
            continue;
        if (len < n - 1)
            buf[len++] = (char)c;
    }
}

/* See the note in passwd.c: this RNG is not cryptographic. */
static void make_salt(char *out, unsigned long outsz)
{
    static const char digits[] = "0123456789abcdef";
    unsigned char raw[SALT_DIGITS / 2];
    unsigned long got = 0;
    unsigned long i;
    int fd;

    fd = open(RANDOM_PATH, O_RDONLY);
    if (fd >= 0) {
        while (got < sizeof(raw)) {
            long n = read(fd, raw + got, sizeof(raw) - got);

            if (n <= 0)
                break;
            got += (unsigned long)n;
        }
        close(fd);
    }
    if (got < sizeof(raw)) {
        unsigned long seed = (unsigned long)syscall(SYS_TIME, 0, 0, 0, 0);

        seed ^= uptime_ms() * 2654435761UL;
        seed ^= (unsigned long)getpid() << 17;
        for (i = got; i < sizeof(raw); i++) {
            seed = seed * 6364136223846793005UL + 1442695040888963407UL;
            raw[i] = (unsigned char)(seed >> 33);
        }
    }
    for (i = 0; i < sizeof(raw) && i * 2 + 2 < outsz; i++) {
        out[i * 2] = digits[(raw[i] >> 4) & 0xf];
        out[i * 2 + 1] = digits[raw[i] & 0xf];
    }
    out[i * 2] = '\0';
}

/* Account names have to survive being a ':'-separated field, a path
 * component and a shell token, so keep them boring. */
static int name_ok(const char *name)
{
    unsigned long i;

    if (name[0] == '\0' || strlen(name) >= NAME_MAX) {
        printf("useradd: the name must be 1..%d characters\n", NAME_MAX - 1);
        return 0;
    }
    if (!((name[0] >= 'a' && name[0] <= 'z') ||
          (name[0] >= 'A' && name[0] <= 'Z'))) {
        printf("useradd: the name must start with a letter\n");
        return 0;
    }
    for (i = 0; name[i] != '\0'; i++) {
        char c = name[i];

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')
            continue;
        printf("useradd: '%c' is not allowed in an account name\n", c);
        return 0;
    }
    return 1;
}

/* Reject anything that would corrupt the record it is stored in. */
static int field_ok(const char *what, const char *s)
{
    unsigned long i;

    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] == ':' || s[i] == '\n' || s[i] == '\r') {
            printf("useradd: %s cannot contain ':' or a newline\n", what);
            return 0;
        }
    }
    return 1;
}

static void mkdir_p(const char *path)
{
    char part[FIELD_MAX];
    unsigned long i;

    if (path[0] != '/')
        return;
    for (i = 1; path[i] != '\0' && i + 1 < sizeof(part); i++) {
        if (path[i] != '/')
            continue;
        copy_field(part, path, i + 1);
        mkdir_(part);
    }
    mkdir_(path);
}

static void usage(void)
{
    printf("usage: useradd [-u UID] [-g GID] [-r REALNAME] [-h HOME] "
           "[-s SHELL] <username>\n");
}

int main(int argc, char **argv)
{
    char home[FIELD_MAX];
    char salt[SALT_DIGITS + 1];
    char hash[SHA256_HEX_LEN + 1];
    char pw1[FIELD_MAX];
    char pw2[FIELD_MAX];
    char line[FIELD_MAX * 4];
    const char *name = 0;
    const char *realname = 0;
    const char *shell = "/bin/sh";
    const char *want_home = 0;
    struct k_stat st;
    int uid = -1;
    int gid = -1;
    int i;

    argc = strip_cwd_arg(argc, argv);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (a[0] == '-' && a[1] != '\0' && a[2] == '\0') {
            if (i + 1 >= argc) {
                printf("useradd: %s needs an argument\n", a);
                return 1;
            }
            switch (a[1]) {
            case 'u': uid = atoi(argv[++i]); break;
            case 'g': gid = atoi(argv[++i]); break;
            case 'r': realname = argv[++i]; break;
            case 'h': want_home = argv[++i]; break;
            case 's': shell = argv[++i]; break;
            default:
                usage();
                return 1;
            }
        } else if (a[0] == '-') {
            usage();
            return 1;
        } else if (name == 0) {
            name = a;
        } else {
            usage();
            return 1;
        }
    }
    if (name == 0) {
        usage();
        return 1;
    }

    if (sc_getuid() != 0) {
        printf("useradd: only root can create accounts\n");
        return 1;
    }
    if (!name_ok(name))
        return 1;
    if (realname == 0)
        realname = name;
    if (!field_ok("the real name", realname) || !field_ok("the shell", shell))
        return 1;
    if (want_home != 0) {
        if (!field_ok("the home directory", want_home))
            return 1;
        if (want_home[0] != '/') {
            printf("useradd: the home directory must be an absolute path\n");
            return 1;
        }
        copy_field(home, want_home, sizeof(home));
    } else {
        snprintf(home, sizeof(home), "/home/%s", name);
    }
    if (shell[0] != '/') {
        printf("useradd: the shell must be an absolute path\n");
        return 1;
    }
    if (db_has(PASSWD_PATH, name)) {
        printf("useradd: %s already exists\n", name);
        return 1;
    }
    if (uid < 0)
        uid = next_uid();
    if (uid < 1 || uid >= UID_MAX) {
        printf("useradd: uid must be 1..%d (0 is root)\n", UID_MAX - 1);
        return 1;
    }
    if (gid < 0)
        gid = uid;
    if (gid < 0 || gid >= UID_MAX) {
        printf("useradd: gid out of range\n");
        return 1;
    }

    if (read_password("password: ", pw1, (int)sizeof(pw1)) < 0)
        return 1;
    if (read_password("retype password: ", pw2, (int)sizeof(pw2)) < 0) {
        memset(pw1, 0, sizeof(pw1));
        return 1;
    }
    if (strcmp(pw1, pw2) != 0) {
        memset(pw1, 0, sizeof(pw1));
        memset(pw2, 0, sizeof(pw2));
        printf("useradd: the two passwords do not match\n");
        return 1;
    }
    memset(pw2, 0, sizeof(pw2));
    if (pw1[0] == '\0' || strchr(pw1, ':') != 0) {
        memset(pw1, 0, sizeof(pw1));
        printf("useradd: the password must be non-empty and free of ':'\n");
        return 1;
    }

    make_salt(salt, sizeof(salt));
    sha256_password(salt, pw1, ITERATIONS, hash);
    memset(pw1, 0, sizeof(pw1));

    snprintf(line, sizeof(line), "%s:%d:%d:%s:%s:%s",
             name, uid, gid, realname, home, shell);
    if (db_replace(PASSWD_PATH, name, line, 0644) != 0) {
        printf("useradd: cannot write %s\n", PASSWD_PATH);
        return 1;
    }
    snprintf(line, sizeof(line), "%s:%s:%d:%s", name, salt, ITERATIONS, hash);
    if (db_replace(SHADOW_PATH, name, line, 0600) != 0) {
        printf("useradd: cannot write %s (the account has no password yet)\n",
               SHADOW_PATH);
        return 1;
    }
    if (!db_has(GROUP_PATH, name)) {
        snprintf(line, sizeof(line), "%s:%d:%s", name, gid, name);
        if (db_replace(GROUP_PATH, name, line, 0644) != 0)
            printf("useradd: warning: cannot write %s\n", GROUP_PATH);
    }

    mkdir_p(home);
    if (stat_(home, &st) != 0 || !st.is_dir) {
        printf("useradd: warning: cannot create %s\n", home);
    } else {
        sc_chown(home, uid, gid);
        sc_chmod(home, 0755);
    }

    printf("useradd: created %s (uid %d, gid %d, home %s, shell %s)\n",
           name, uid, gid, home, shell);
    return 0;
}
