/* login.c - console login for KestrelOS.
 *
 * Prompts for a username and a password (the password is read raw from
 * fd 0 and never echoed), checks it against /etc/shadow, and on success
 * drops to the account's uid with SYS_SETUID before SYS_EXEC'ing the
 * account's shell in its home directory.
 *
 * SYS_SETUID only works while the caller is still root, so login has to
 * do it before handing over. That also means login cannot recover its
 * privileges once it has switched: an exec failure after setuid is fatal
 * and we exit so that init starts a fresh login.
 *
 * Everything about the account database is best-effort: a missing
 * /etc/shadow, a truncated or malformed line, an unknown user and an
 * over-long field all end in "login incorrect", never in a crash.
 * See docs/users.md.
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sha256.h>

#define PASSWD_PATH "/etc/passwd"
#define SHADOW_PATH "/etc/shadow"

#define DB_MAX     4096       /* whole-file buffer for an account file */
#define FIELD_MAX  128        /* longest field we keep */
#define MAX_TRIES  3          /* failures before the banner starts over */
#define FAIL_DELAY 2000       /* ms of penalty after a bad password */
#define SPAWN_ARG_MAX 128     /* kernel limit, from kernel/include/uproc.h */

struct account {
    char name[FIELD_MAX];
    char home[FIELD_MAX];
    char shell[FIELD_MAX];
    int uid;
    int gid;
};

/* ---- syscalls libc does not wrap yet ---- */

static int sc_getuid(void)
{
    return (int)syscall(SYS_GETUID, 0, 0, 0, 0);
}

static int sc_setuid(int uid)
{
    return (int)syscall(SYS_SETUID, uid, 0, 0, 0);
}

static int sc_chown(const char *path, int uid, int gid)
{
    return (int)syscall(SYS_CHOWN, (long)path, uid, gid, 0);
}

static int sc_chmod(const char *path, int mode)
{
    return (int)syscall(SYS_CHMOD, (long)path, mode, 0, 0);
}

static int sc_exec(const char *path, char *const argv[])
{
    return (int)syscall(SYS_EXEC, (long)path, (long)argv, 0, 0);
}

/* ---- account file parsing ---- */

/* Read a whole file into buf and NUL-terminate it. Returns the length,
 * or -1 if the file cannot be opened. A file larger than the buffer is
 * truncated, which can only cost us a trailing account. */
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

/* Hand back the next usable line from *p, NUL-terminated in place, and
 * advance *p past it. Blank lines and '#' comments are skipped. Returns
 * 0 at the end of the buffer. */
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
        /* a stray '\r' from a host-edited file is not part of the data */
        if (e > s && e[-1] == '\r')
            e[-1] = '\0';
        if (*s == '\0' || *s == '#')
            continue;
        return s;
    }
}

/* Split a line on ':' in place. At most maxf fields; anything after the
 * last separator we look at stays in the final field. */
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

/* Look up `user` in /etc/passwd. Returns 1 on success. */
static int find_account(const char *user, struct account *out)
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
        copy_field(out->name, f[0], sizeof(out->name));
        out->uid = atoi(f[1]);
        out->gid = atoi(f[2]);
        copy_field(out->home, f[4], sizeof(out->home));
        copy_field(out->shell, f[5], sizeof(out->shell));
        if (out->uid < 0 || out->gid < 0)
            return 0;
        if (out->home[0] != '/')
            copy_field(out->home, "/", sizeof(out->home));
        if (out->shell[0] != '/')
            copy_field(out->shell, "/bin/sh", sizeof(out->shell));
        return 1;
    }
    return 0;
}

/* Compare two hex digests without an early exit, so the time taken does
 * not leak how many leading digits matched. Not that our threat model
 * really supports this - see docs/users.md - but it costs nothing. */
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

/* Check `pw` against `user`'s /etc/shadow entry. 1 = good, 0 = no. */
static int shadow_check(const char *user, const char *pw)
{
    char buf[DB_MAX];
    char want[SHA256_HEX_LEN + 1];
    char *p;
    char *line;
    char *f[4];
    long iters;

    if (slurp(SHADOW_PATH, buf, sizeof(buf)) < 0)
        return 0;
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

/* ---- terminal ---- */

/* Read a line from fd 0 with no echo at all. Backspace and ctrl-U edit
 * the buffer silently. Returns the length, or -1 on ctrl-C / ctrl-D /
 * read error. */
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
        if (c == 3 || c == 4) {              /* ctrl-C / ctrl-D */
            putchar('\n');
            return -1;
        }
        if (c == 8 || c == 127) {            /* backspace */
            if (len > 0)
                len--;
            continue;
        }
        if (c == 21) {                       /* ctrl-U */
            len = 0;
            continue;
        }
        if (c >= 0x80 || c < 32)             /* KEY_* and other controls */
            continue;
        if (len < n - 1)
            buf[len++] = (char)c;
    }
}

/* ---- home directory ---- */

/* mkdir every missing component of an absolute path. Best effort. */
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

/* Make sure the account's home exists and belongs to it. Must run while
 * we are still root; failures are reported but not fatal, because a
 * shell in / beats no shell at all. */
static void ensure_home(const struct account *a)
{
    struct k_stat st;

    if (a->home[0] != '/' || strcmp(a->home, "/") == 0)
        return;
    if (stat_(a->home, &st) == 0) {
        if (!st.is_dir)
            printf("login: %s is not a directory\n", a->home);
        return;
    }
    mkdir_p(a->home);
    if (stat_(a->home, &st) != 0) {
        printf("login: cannot create %s\n", a->home);
        return;
    }
    sc_chown(a->home, a->uid, a->gid);
    sc_chmod(a->home, 0755);
}

/* ---- main ---- */

static void banner(void)
{
    printf("\n");
    printf("  +--------------------------------------------------+\n");
    printf("  |  KestrelOS login                                 |\n");
    printf("  |                                                  |\n");
    printf("  |  demo accounts (this is a demo OS, the passwords  |\n");
    printf("  |  are published on purpose):                      |\n");
    printf("  |      root     / root                             |\n");
    printf("  |      kestrel  / kestrel                          |\n");
    printf("  +--------------------------------------------------+\n");
    printf("\n");
}

/* Replace login with the account's shell. Only returns on failure. */
static void start_shell(const struct account *a)
{
    char cwdarg[FIELD_MAX + 8];
    char *sargv[3];

    snprintf(cwdarg, sizeof(cwdarg), "--cwd=%s", a->home);
    if (strlen(cwdarg) >= SPAWN_ARG_MAX)
        snprintf(cwdarg, sizeof(cwdarg), "--cwd=/");

    sargv[0] = (char *)a->shell;
    sargv[1] = cwdarg;
    sargv[2] = 0;
    sc_exec(a->shell, sargv);
}

int main(int argc, char **argv)
{
    char user[FIELD_MAX];
    char pass[FIELD_MAX];
    struct account acct;
    int tries;
    int have;

    (void)argc;
    (void)argv;

    for (;;) {
        banner();
        for (tries = 0; tries < MAX_TRIES; tries++) {
            printf("login: ");
            if (readline(user, (int)sizeof(user)) == 0) {
                putchar('\n');
                continue;
            }
            if (user[0] == '\0')
                continue;
            if (read_password("password: ", pass, (int)sizeof(pass)) < 0)
                continue;

            /* Look the account up first, but always run the password
             * check path so an unknown name costs the same as a bad
             * password. */
            have = find_account(user, &acct);
            if (shadow_check(user, pass) && have) {
                memset(pass, 0, sizeof(pass));
                printf("\nwelcome, %s\n", acct.name);
                ensure_home(&acct);
                if (sc_getuid() != acct.uid && sc_setuid(acct.uid) != 0) {
                    printf("login: cannot become uid %d\n", acct.uid);
                    return 1;
                }
                start_shell(&acct);
                printf("login: cannot exec %s\n", acct.shell);
                return 1;
            }
            memset(pass, 0, sizeof(pass));
            printf("login incorrect\n");
            sleep_ms(FAIL_DELAY);
        }
        printf("\ntoo many failures, starting over\n");
    }
}
