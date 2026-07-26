/* su.c - run a shell as another user (root by default).
 *
 * The target account's password is always checked, then SYS_SETUID
 * switches to its uid and a nested /bin/sh is spawned in its home
 * directory; su waits for that shell and exits with its status.
 *
 * KestrelOS has no setuid bit (see abi/kestrel_abi.h), so SYS_SETUID
 * only succeeds for a caller that is already root. su can therefore drop
 * privileges but never gain them; from an unprivileged shell it fails,
 * and it says so plainly instead of pretending otherwise. /etc/shadow is
 * mode 0600 root-owned, so an unprivileged caller cannot even read the
 * hash to check against. See docs/users.md.
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sha256.h>

#define PASSWD_PATH "/etc/passwd"
#define SHADOW_PATH "/etc/shadow"

#define DB_MAX     4096
#define FIELD_MAX  128
#define SPAWN_ARG_MAX 128

struct account {
    char name[FIELD_MAX];
    char home[FIELD_MAX];
    char shell[FIELD_MAX];
    int uid;
    int gid;
};

static int sc_getuid(void)
{
    return (int)syscall(SYS_GETUID, 0, 0, 0, 0);
}

static int sc_setuid(int uid)
{
    return (int)syscall(SYS_SETUID, uid, 0, 0, 0);
}

/* Pull the shell-injected trailing "--cwd=<path>" argument, if any. */
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

/* Next usable line from *p, NUL-terminated in place; blanks and '#'
 * comments skipped, 0 at the end. */
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

/* The name of the account owning `uid`, or the number itself. */
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

/* 1 = password good, 0 = bad or no entry, -1 = /etc/shadow unreadable. */
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

/* Read a line from fd 0 with no echo. -1 on ctrl-C/ctrl-D/read error. */
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

int main(int argc, char **argv)
{
    char pass[FIELD_MAX];
    char cwdarg[FIELD_MAX + 8];
    char caller_name[FIELD_MAX];
    const char *target = "root";
    struct account acct;
    struct k_stat st;
    char *sargv[3];
    int caller;
    int ok;
    int pid;
    int status;

    argc = strip_cwd_arg(argc, argv);
    if (argc > 2) {
        printf("usage: su [username]\n");
        return 1;
    }
    if (argc == 2)
        target = argv[1];

    caller = sc_getuid();
    name_for_uid(caller, caller_name, sizeof(caller_name));

    if (!find_account(target, &acct)) {
        printf("su: unknown user %s\n", target);
        return 1;
    }

    if (caller != acct.uid) {
        if (read_password("password: ", pass, (int)sizeof(pass)) < 0)
            return 1;
        ok = shadow_check(target, pass);
        memset(pass, 0, sizeof(pass));
        if (ok < 0) {
            printf("su: cannot read %s\n", SHADOW_PATH);
            if (caller != 0)
                printf("su: it is mode 0600 and owned by root, and there "
                       "is no setuid bit on KestrelOS\n");
            return 1;
        }
        if (ok == 0) {
            printf("su: authentication failure\n");
            sleep_ms(2000);
            return 1;
        }
        if (sc_setuid(acct.uid) != 0) {
            printf("su: cannot become uid %d\n", acct.uid);
            if (caller != 0)
                printf("su: only root can switch users - KestrelOS has no "
                       "setuid bit, so su cannot raise privilege\n");
            return 1;
        }
    }

    /* Fall back to / if the home directory is gone; a shell that will
     * not start is worse than a shell in the wrong place. */
    if (acct.home[0] != '/' || stat_(acct.home, &st) != 0 || !st.is_dir)
        copy_field(acct.home, "/", sizeof(acct.home));

    snprintf(cwdarg, sizeof(cwdarg), "--cwd=%s", acct.home);
    if (strlen(cwdarg) >= SPAWN_ARG_MAX)
        snprintf(cwdarg, sizeof(cwdarg), "--cwd=/");

    printf("su: %s -> %s (exit the nested shell to come back)\n",
           caller_name, acct.name);

    sargv[0] = acct.shell;
    sargv[1] = cwdarg;
    sargv[2] = 0;
    pid = spawn(acct.shell, sargv);
    if (pid < 0) {
        printf("su: cannot spawn %s\n", acct.shell);
        return 1;
    }
    status = waitpid(pid);
    printf("su: back to %s\n", caller_name);
    return status;
}
