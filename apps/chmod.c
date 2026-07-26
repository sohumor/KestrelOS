/* chmod.c - change file permission bits.
 *
 *   chmod 0644 notes.txt
 *   chmod u+x,go-w script
 *
 * The mode is either octal (1..4 digits, masked to 0777 - KestrelOS has
 * no setuid/setgid/sticky bits) or a comma-separated list of symbolic
 * clauses [ugoa...][+-=][rwx...]. Symbolic clauses are applied to the
 * file's current mode, so each file is read with stat() first.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#define MAX_PATH 256

static const char *g_cwd = "/";

static int sc_chmod(const char *path, int mode)
{
    return (int)syscall(SYS_CHMOD, (long)path, mode, 0, 0);
}

/* Pull the shell-injected trailing "--cwd=<path>" argument, if any. */
static int strip_cwd_arg(int argc, char **argv)
{
    if (argc > 1 && strncmp(argv[argc - 1], "--cwd=", 6) == 0) {
        g_cwd = argv[argc - 1] + 6;
        return argc - 1;
    }
    return argc;
}

/* Join a possibly relative path with the cwd. */
static void resolve(const char *tok, char *out, unsigned long outsz)
{
    if (tok[0] == '/')
        snprintf(out, outsz, "%s", tok);
    else if (g_cwd[0] == '/' && g_cwd[1] == '\0')
        snprintf(out, outsz, "/%s", tok);
    else
        snprintf(out, outsz, "%s/%s", g_cwd, tok);
}

/* libc's printf has no %o, so spell the three permission digits out. */
static void mode_str(int mode, char out[5])
{
    out[0] = '0';
    out[1] = (char)('0' + ((mode >> 6) & 7));
    out[2] = (char)('0' + ((mode >> 3) & 7));
    out[3] = (char)('0' + (mode & 7));
    out[4] = '\0';
}

/* "755", "0644" -> 0..0777. Returns 0 if this is not an octal mode. */
static int parse_octal(const char *s, int *out)
{
    int value = 0;
    int digits = 0;

    if (*s == '\0')
        return 0;
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '7')
            return 0;
        value = value * 8 + (*s - '0');
        if (++digits > 4)
            return 0;
    }
    *out = value & 0777;
    return 1;
}

/* Apply symbolic clauses to `mode`. Returns 0 on a syntax error.
 *
 * An empty "who" means all three classes; an empty permission list is
 * legal and makes '+'/'-' a no-op and '=' a clear. */
static int parse_symbolic(const char *s, int mode, int *out)
{
    for (;;) {
        int who = 0;
        int bits = 0;
        int mask;
        char op;

        while (*s == 'u' || *s == 'g' || *s == 'o' || *s == 'a') {
            if (*s == 'u')
                who |= 0700;
            else if (*s == 'g')
                who |= 0070;
            else if (*s == 'o')
                who |= 0007;
            else
                who |= 0777;
            s++;
        }
        if (who == 0)
            who = 0777;

        if (*s != '+' && *s != '-' && *s != '=')
            return 0;
        op = *s++;

        while (*s == 'r' || *s == 'w' || *s == 'x') {
            if (*s == 'r')
                bits |= 0444;
            else if (*s == 'w')
                bits |= 0222;
            else
                bits |= 0111;
            s++;
        }

        mask = who & bits;
        if (op == '+')
            mode |= mask;
        else if (op == '-')
            mode &= ~mask;
        else
            mode = (mode & ~who) | mask;

        if (*s == '\0')
            break;
        if (*s != ',')
            return 0;
        s++;
    }
    *out = mode & 0777;
    return 1;
}

int main(int argc, char **argv)
{
    char path[MAX_PATH];
    struct k_stat st;
    const char *spec;
    int verbose = 0;
    int first;
    int octal;
    int have_octal;
    int i;
    int rc = 0;

    argc = strip_cwd_arg(argc, argv);

    first = 1;
    if (first < argc && strcmp(argv[first], "-v") == 0) {
        verbose = 1;
        first++;
    }
    if (argc - first < 2) {
        printf("usage: chmod [-v] <mode> <file>...\n");
        printf("       mode is octal (0755) or symbolic (u+x,go-w)\n");
        return 1;
    }

    spec = argv[first];
    have_octal = parse_octal(spec, &octal);

    for (i = first + 1; i < argc; i++) {
        int mode;

        resolve(argv[i], path, sizeof(path));
        if (have_octal) {
            mode = octal;
        } else {
            if (stat_(path, &st) != 0) {
                printf("chmod: cannot stat %s\n", path);
                rc = 1;
                continue;
            }
            if (!parse_symbolic(spec, (int)(st.mode & 0777), &mode)) {
                printf("chmod: bad mode %s\n", spec);
                return 1;
            }
        }
        if (sc_chmod(path, mode) != 0) {
            printf("chmod: cannot change %s\n", path);
            rc = 1;
            continue;
        }
        if (verbose) {
            char text[5];

            mode_str(mode, text);
            printf("chmod: %s -> %s\n", path, text);
        }
    }
    return rc;
}
