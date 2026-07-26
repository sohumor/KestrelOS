/* minsh.c - the minimal fallback shell.
 *
 * The original KestrelOS shell, kept as /bin/minsh now that the
 * full-featured shell (apps/sh.c: history, tab completion, redirection)
 * is /bin/sh. No line editing beyond libc readline(), no history, no
 * redirection - useful when the real shell misbehaves.
 *
 * Prompt: "kestrel:<cwd>$ ". Tokenizes on whitespace (max 16 tokens,
 * double quotes group words). Builtins: cd, pwd, exit, help. Anything
 * else is resolved to a binary (absolute, then <cwd>/name, then
 * /bin/name) and spawned. The shell appends one extra argv element
 * "--cwd=<cwd>" so path-taking apps can resolve relative arguments
 * themselves; apps that ignore argv are unaffected. The hint is dropped
 * (with a warning) when it would not fit the kernel's per-argument limit.
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE   256
/* One below the kernel's argv limit: run_external appends --cwd=<path> as
 * an extra element, and an argv[16] would be dropped by the copy-in loop. */
#define MAX_TOKENS 15
#define MAX_PATH   256
/* Kernel per-argument limit, from kernel/include/uproc.h (UPROC_ARG_MAX).
 * The kernel copies each argv entry with strncpy into a slot this big, so
 * a longer argument reaches the child silently truncated. */
#define SPAWN_ARG_MAX 128

static char cwd[MAX_PATH] = "/";

/* Split line in place. Returns token count, or -1 on too many tokens.
 * Double quotes group words and are stripped from the token. */
static int tokenize(char *line, char **tokens)
{
    int n = 0;
    char *p = line;

    while (*p) {
        char *w;
        int inq = 0;

        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        if (n >= MAX_TOKENS)
            return -1;
        tokens[n++] = p;
        w = p;
        while (*p && (inq || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') {
                inq = !inq;
                p++;
                continue;
            }
            *w++ = *p++;
        }
        if (*p)
            p++;
        *w = '\0';
    }
    return n;
}

/* Join base + rel (rel may be absolute) and normalize "." and ".."
 * components. Result always starts with '/'. */
static void path_join_norm(const char *base, const char *rel,
                           char *out, unsigned long outsz)
{
    char tmp[2 * MAX_PATH];
    char *parts[64];
    char *p;
    int nparts = 0, i;
    unsigned long off = 0;

    if (rel[0] == '/')
        snprintf(tmp, sizeof(tmp), "%s", rel);
    else if (base[0] == '/' && base[1] == '\0')
        snprintf(tmp, sizeof(tmp), "/%s", rel);
    else
        snprintf(tmp, sizeof(tmp), "%s/%s", base, rel);

    p = tmp;
    while (*p) {
        char *start;

        while (*p == '/')
            p++;
        if (!*p)
            break;
        start = p;
        while (*p && *p != '/')
            p++;
        if (*p)
            *p++ = '\0';
        if (strcmp(start, ".") == 0)
            continue;
        if (strcmp(start, "..") == 0) {
            if (nparts > 0)
                nparts--;
            continue;
        }
        if (nparts < 64)
            parts[nparts++] = start;
    }

    if (nparts == 0) {
        snprintf(out, outsz, "/");
        return;
    }
    out[0] = '\0';
    for (i = 0; i < nparts; i++) {
        off += (unsigned long)snprintf(out + off, outsz - off, "/%s", parts[i]);
        if (off >= outsz) {
            off = outsz - 1;
            break;
        }
    }
}

static void builtin_cd(int ntok, char **tok)
{
    char target[MAX_PATH];
    struct k_stat st;

    if (ntok < 2)
        strcpy(target, "/");
    else
        path_join_norm(cwd, tok[1], target, sizeof(target));

    if (stat_(target, &st) != 0) {
        printf("cd: no such path: %s\n", target);
        return;
    }
    if (!st.is_dir) {
        printf("cd: not a directory: %s\n", target);
        return;
    }
    strcpy(cwd, target);
}

static void run_external(int ntok, char **tok)
{
    char path[MAX_PATH];
    char cwdarg[MAX_PATH + 8];
    char *sargv[MAX_TOKENS + 2];
    struct k_stat st;
    int i, pid, code;

    if (tok[0][0] == '/') {
        snprintf(path, sizeof(path), "%s", tok[0]);
        if (stat_(path, &st) != 0 || st.is_dir) {
            printf("minsh: command not found: %s\n", tok[0]);
            return;
        }
    } else {
        path_join_norm(cwd, tok[0], path, sizeof(path));
        if (stat_(path, &st) != 0 || st.is_dir) {
            snprintf(path, sizeof(path), "/bin/%s", tok[0]);
            if (stat_(path, &st) != 0 || st.is_dir) {
                printf("minsh: command not found: %s\n", tok[0]);
                return;
            }
        }
    }

    for (i = 0; i < ntok; i++)
        sargv[i] = tok[i];

    snprintf(cwdarg, sizeof(cwdarg), "--cwd=%s", cwd);
    /* A hint the kernel would truncate points at a directory that does not
     * exist, which is worse than no hint at all: apps then resolve against /. */
    if (strlen(cwdarg) < SPAWN_ARG_MAX) {
        sargv[i++] = cwdarg;
    } else {
        printf("minsh: cwd too long to pass on; %s will resolve against /\n",
               tok[0]);
    }
    sargv[i] = 0;

    pid = spawn(path, sargv);
    if (pid < 0) {
        printf("minsh: spawn failed: %s\n", path);
        return;
    }
    code = waitpid(pid);
    if (code != 0)
        printf("[exit %d]\n", code);
}

/* The banner promises "help" lists every command, so the builtin names the
 * builtins and then runs /bin/help, falling back to a pointer if it cannot. */
static void builtin_help(void)
{
    char cwdarg[MAX_PATH + 8];
    char arg0[] = "help";
    char *hargv[3];
    struct k_stat st;
    int n = 0, pid;

    puts("builtins: cd pwd exit help");
    if (stat_("/bin/help", &st) == 0 && !st.is_dir) {
        hargv[n++] = arg0;
        snprintf(cwdarg, sizeof(cwdarg), "--cwd=%s", cwd);
        if (strlen(cwdarg) < SPAWN_ARG_MAX)
            hargv[n++] = cwdarg;
        hargv[n] = 0;
        pid = spawn("/bin/help", hargv);
        if (pid >= 0) {
            waitpid(pid);
            return;
        }
    }
    puts("run /bin/help for all commands");
}

int main(int argc, char **argv)
{
    char line[MAX_LINE];
    char *tok[MAX_TOKENS];
    int ntok;

    (void)argc;
    (void)argv;

    for (;;) {
        printf("kestrel:%s$ ", cwd);
        if (!readline(line, (int)sizeof(line))) {
            /* ctrl-D on an empty line: leave the shell. */
            putchar('\n');
            return 0;
        }
        ntok = tokenize(line, tok);
        if (ntok < 0) {
            printf("minsh: too many tokens (max %d)\n", MAX_TOKENS);
            continue;
        }
        if (ntok == 0)
            continue;

        if (strcmp(tok[0], "cd") == 0)
            builtin_cd(ntok, tok);
        else if (strcmp(tok[0], "pwd") == 0)
            puts(cwd);
        else if (strcmp(tok[0], "exit") == 0)
            return ntok > 1 ? atoi(tok[1]) : 0;
        else if (strcmp(tok[0], "help") == 0)
            builtin_help();
        else
            run_external(ntok, tok);
    }
}
