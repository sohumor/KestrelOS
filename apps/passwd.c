/* passwd.c - change an account password.
 *
 * Reads the old password (skipped when root is running), reads the new
 * one twice with echo off, draws a fresh 16-hex-digit salt from
 * /dev/random and rewrites that account's /etc/shadow line as
 * "user:salt:iterations:hash". Comments and every other account in the
 * file are preserved byte for byte.
 *
 * /etc/shadow is mode 0600 and owned by root, and KestrelOS has no
 * setuid bit, so in practice only root can complete a change; an
 * unprivileged run fails at the file and says why. See docs/users.md.
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sha256.h>

#define PASSWD_PATH "/etc/passwd"
#define SHADOW_PATH "/etc/shadow"
#define RANDOM_PATH "/dev/random"

#define DB_MAX      4096
#define FIELD_MAX   128
#define SALT_DIGITS 16          /* hex digits, so 8 random bytes */
#define ITERATIONS  4096

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

/* Does this raw line name `user` in its first ':' field? */
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

/* Append s + '\n' to out. Returns -1 if it will not fit. */
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

/* Rewrite `path` with `user`'s line replaced by `repl` (appended if the
 * user has no line yet). Everything else, comments included, survives
 * untouched. Returns 0 on success. */
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
    /* O_TRUNC keeps the inode, but a freshly created file would not have
     * the right bits, so set them either way. */
    sc_chmod(path, mode);
    return 0;
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

static int user_exists(const char *user)
{
    char buf[DB_MAX];
    char *p;
    char *line;
    char *f[6];

    if (slurp(PASSWD_PATH, buf, sizeof(buf)) < 0)
        return 0;
    p = buf;
    while ((line = db_next(&p)) != 0) {
        if (split_fields(line, f, 6) < 1)
            continue;
        if (strcmp(f[0], user) == 0)
            return 1;
    }
    return 0;
}

static int hash_equal(const char *a, const char *b)
{
    unsigned int diff = 0;
    int i;

    if (strlen(a) != SHA256_HEX_LEN || strlen(b) != SHA256_HEX_LEN)
        return 0;
    for (i = 0; i < SHA256_HEX_LEN; i++)
        diff |= (unsigned int)((unsigned char)a[i] ^ (unsigned char)b[i]);
    return diff == 0;
}

/* 1 = good, 0 = bad or no entry, -1 = /etc/shadow unreadable. */
static int shadow_check(const char *user, const char *pw)
{
    char buf[DB_MAX];
    char want[SHA256_HEX_LEN + 1];
    char *p;
    char *line;
    char *f[4];
    long iters;

    if (slurp(SHADOW_PATH, buf, sizeof(buf)) < 0)
        return -1;
    p = buf;
    while ((line = db_next(&p)) != 0) {
        if (split_fields(line, f, 4) != 4)
            continue;
        if (strcmp(f[0], user) != 0)
            continue;
        iters = atol(f[2]);
        if (f[1][0] == '\0' || iters < 1 || iters > 1000000)
            return 0;
        sha256_password(f[1], pw, (unsigned long)iters, want);
        return hash_equal(want, f[3]);
    }
    return 0;
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

/* Fill out[] with SALT_DIGITS hex digits. /dev/random is preferred; if
 * it is missing we fall back to mixing the clock, the uptime and the pid,
 * which is even weaker. Neither source is cryptographic - docs/users.md
 * spells out what that costs. */
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

/* Passwords live in a ':'-separated file, so ':' is out, and control
 * characters cannot be typed back at the prompt. */
static int password_ok(const char *pw)
{
    unsigned long i;

    if (pw[0] == '\0') {
        printf("passwd: an empty password is not allowed\n");
        return 0;
    }
    for (i = 0; pw[i] != '\0'; i++) {
        if (pw[i] == ':') {
            printf("passwd: ':' separates shadow fields and cannot be used\n");
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    char me[FIELD_MAX];
    char old[FIELD_MAX];
    char pw1[FIELD_MAX];
    char pw2[FIELD_MAX];
    char salt[SALT_DIGITS + 1];
    char hash[SHA256_HEX_LEN + 1];
    char line[FIELD_MAX + SALT_DIGITS + SHA256_HEX_LEN + 32];
    const char *target;
    int caller;
    int ok;

    argc = strip_cwd_arg(argc, argv);
    if (argc > 2) {
        printf("usage: passwd [username]\n");
        return 1;
    }

    caller = sc_getuid();
    name_for_uid(caller, me, sizeof(me));
    target = (argc == 2) ? argv[1] : me;

    if (!user_exists(target)) {
        printf("passwd: unknown user %s\n", target);
        return 1;
    }
    if (caller != 0 && strcmp(target, me) != 0) {
        printf("passwd: only root can change another user's password\n");
        return 1;
    }

    if (caller != 0) {
        if (read_password("current password: ", old, (int)sizeof(old)) < 0)
            return 1;
        ok = shadow_check(target, old);
        memset(old, 0, sizeof(old));
        if (ok < 0) {
            printf("passwd: cannot read %s (mode 0600, owned by root)\n",
                   SHADOW_PATH);
            return 1;
        }
        if (ok == 0) {
            printf("passwd: authentication failure\n");
            sleep_ms(2000);
            return 1;
        }
    }

    printf("changing password for %s\n", target);
    if (read_password("new password: ", pw1, (int)sizeof(pw1)) < 0)
        return 1;
    if (read_password("retype new password: ", pw2, (int)sizeof(pw2)) < 0) {
        memset(pw1, 0, sizeof(pw1));
        return 1;
    }
    if (strcmp(pw1, pw2) != 0) {
        memset(pw1, 0, sizeof(pw1));
        memset(pw2, 0, sizeof(pw2));
        printf("passwd: the two passwords do not match\n");
        return 1;
    }
    memset(pw2, 0, sizeof(pw2));
    if (!password_ok(pw1)) {
        memset(pw1, 0, sizeof(pw1));
        return 1;
    }

    make_salt(salt, sizeof(salt));
    sha256_password(salt, pw1, ITERATIONS, hash);
    memset(pw1, 0, sizeof(pw1));

    snprintf(line, sizeof(line), "%s:%s:%d:%s", target, salt, ITERATIONS, hash);
    if (db_replace(SHADOW_PATH, target, line, 0600) != 0) {
        printf("passwd: cannot write %s\n", SHADOW_PATH);
        if (caller != 0)
            printf("passwd: it is mode 0600 and owned by root, and there is "
                   "no setuid bit on KestrelOS, so only root can do this\n");
        return 1;
    }
    printf("password updated\n");
    return 0;
}
