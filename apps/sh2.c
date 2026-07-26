/* sh2.c - the KestrelOS shell, second generation.
 *
 * A drop-in replacement for sh.c. Everything the old shell did is kept
 * byte for byte: the prompt "kestrel:<cwd>$ ", whitespace tokenizing
 * with double-quote grouping (max 16 tokens), the builtins cd, pwd,
 * exit and help, the "--cwd=<cwd>" argument appended to every child,
 * the "sh: command not found: <name>" diagnostic and the "[exit N]"
 * report for a non-zero child status.
 *
 * On top of that sh2 adds:
 *   - its own line editor (no libc change): insert, backspace, delete,
 *     ctrl-U/ctrl-K kill, LEFT/RIGHT/HOME/END (also ctrl-A/ctrl-E),
 *     UP/DOWN recall over a 32-entry history ring, ctrl-C to cancel the
 *     line, ctrl-D on an empty line to leave the shell;
 *   - tab completion: command names out of /bin for argv[0], otherwise
 *     file and directory names out of the containing directory;
 *   - ';' to separate commands and '#' to start a comment;
 *   - $? and $PWD expansion (there is no environment to speak of);
 *   - '>' / '>>' redirection for builtin output (see the note below);
 *   - the builtins history, clear, which and set.
 *
 * Redirection note. The kernel gives fd 0/1/2 straight to the console
 * (kernel/syscall.c: sys_write special-cases fd 1 and 2 before ever
 * looking at current->files[], and fd_file() rejects anything below 3),
 * SYS_SPAWN copies no descriptor table into the child, and there is no
 * dup2. A shell therefore cannot point a child's stdout anywhere. sh2
 * consequently redirects only what it writes itself - the builtins -
 * and, for an external command, says so and runs the command normally
 * rather than inventing a "--stdout=<path>" argument that today's apps
 * would mistake for a filename.
 *
 * Screen updates are done by rewriting the line: '\r', prompt, buffer,
 * CSI K, then CSI <n> D to place the cursor. That works on the VGA
 * console (kernel/console.c implements \r, CSI K and CSI C/D) and on a
 * plain serial terminal. '\b' is never used to move the cursor, only to
 * erase, because the VGA console's backspace is destructive.
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE    512
#define MAX_TOKENS  16
#define MAX_PATH    256
#define MAX_SEGS    16
#define HIST_MAX    32
#define MAX_CAND    64
#define NAME_MAX    64

/* Kernel spawn limits, from kernel/include/uproc.h. Exceeding either of
 * them is silent truncation (too many args) or a failed spawn (an arg
 * that does not fit), so sh2 checks both before calling spawn(). */
#define SPAWN_MAX_ARGS 16
#define SPAWN_ARG_MAX  128

static char cwd[MAX_PATH] = "/";
static int last_status;                 /* $? */
static int out_fd = 1;                  /* builtin output, redirected by > */
static int want_exit;
static int exit_code;

static char hist[HIST_MAX][MAX_LINE];
static int hist_count;                  /* entries currently stored */
static int hist_head;                   /* next slot to write */
static int hist_total;                  /* lifetime count, for numbering */

/* --- small helpers ---------------------------------------------------- */

/* Builtin output. Goes to fd 1 normally, to the redirection target when
 * one is active. Diagnostics that are about the shell itself keep using
 * printf so they stay on the console. */
static void sh_out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sh_out(const char *fmt, ...)
{
    char buf[MAX_LINE];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;       /* vsnprintf returns the full length */
    write(out_fd, buf, (unsigned long)n);
}

static int is_space(int c)
{
    return c == ' ' || c == '\t';
}

static int is_name_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

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

/* Resolve a command name the way the old shell did: absolute as given,
 * otherwise <cwd>/name then /bin/name. Returns 0 and fills out. */
static int resolve_cmd(const char *name, char *out, unsigned long outsz)
{
    struct k_stat st;

    if (name[0] == '/') {
        snprintf(out, outsz, "%s", name);
        return (stat_(out, &st) == 0 && !st.is_dir) ? 0 : -1;
    }
    path_join_norm(cwd, name, out, outsz);
    if (stat_(out, &st) == 0 && !st.is_dir)
        return 0;
    snprintf(out, outsz, "/bin/%s", name);
    if (stat_(out, &st) == 0 && !st.is_dir)
        return 0;
    return -1;
}

/* --- history ---------------------------------------------------------- */

/* k == 1 is the most recent entry; returns NULL when k is out of range. */
static const char *hist_get(int k)
{
    int slot;

    if (k < 1 || k > hist_count)
        return 0;
    slot = (hist_head - k + 2 * HIST_MAX) % HIST_MAX;
    return hist[slot];
}

static void hist_add(const char *line)
{
    const char *p = line;
    const char *last;

    while (is_space(*p))
        p++;
    if (!*p)
        return;                         /* blank lines are not remembered */
    last = hist_get(1);
    if (last && strcmp(last, line) == 0)
        return;                         /* nor immediate duplicates */

    snprintf(hist[hist_head], MAX_LINE, "%s", line);
    hist_head = (hist_head + 1) % HIST_MAX;
    if (hist_count < HIST_MAX)
        hist_count++;
    hist_total++;
}

/* --- line editing ----------------------------------------------------- */

/* Full repaint: carriage return, prompt, buffer, erase to end of line,
 * then walk the cursor back to its logical position. */
static void line_redraw(const char *prompt, const char *buf, int len, int cur)
{
    printf("\r%s%s\033[K", prompt, buf);
    if (cur < len)
        printf("\033[%dD", len - cur);
}

/* Insert s at the cursor. Returns the number of characters inserted. */
static int line_insert(char *buf, int *len, int *cur, int max, const char *s)
{
    int n = (int)strlen(s);
    int i;

    if (n <= 0)
        return 0;
    if (*len + n > max - 1)
        n = max - 1 - *len;
    if (n <= 0)
        return 0;
    for (i = *len; i >= *cur; i--)
        buf[i + n] = buf[i];
    memcpy(buf + *cur, s, (unsigned long)n);
    *len += n;
    *cur += n;
    buf[*len] = '\0';
    return n;
}

static void line_delete_at(char *buf, int *len, int cur)
{
    int i;

    if (cur >= *len)
        return;
    for (i = cur; i < *len; i++)
        buf[i] = buf[i + 1];
    (*len)--;
}

/* --- tab completion --------------------------------------------------- */

static char cand[MAX_CAND][NAME_MAX];
static char cand_dir[MAX_CAND];

/* Collect directory entries under dir whose name starts with pfx. */
static int gather(const char *dir, const char *pfx)
{
    struct k_dirent de;
    unsigned long plen = strlen(pfx);
    int idx = 0, n = 0;

    while (n < MAX_CAND && idx < 4096 && readdir_at(dir, idx, &de) == 0) {
        idx++;
        de.name[sizeof(de.name) - 1] = '\0';
        if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
            continue;
        if (strncmp(de.name, pfx, plen) != 0)
            continue;
        snprintf(cand[n], NAME_MAX, "%s", de.name);
        cand_dir[n] = de.is_dir ? 1 : 0;
        n++;
    }
    return n;
}

/* Length of the longest prefix shared by all n candidates. */
static int common_prefix_len(int n)
{
    int i, k = 0;

    if (n <= 0)
        return 0;
    while (cand[0][k]) {
        for (i = 1; i < n; i++)
            if (cand[i][k] != cand[0][k])
                return k;
        k++;
    }
    return k;
}

static void do_complete(const char *prompt, char *buf, int *len, int *cur,
                        int max)
{
    char tokbuf[MAX_PATH];
    char dir[MAX_PATH];
    char add[NAME_MAX + 2];
    const char *pfx;
    char *slash;
    int start, tlen, i, n, lcp, plen, is_cmd = 1;

    start = *cur;
    while (start > 0 && !is_space(buf[start - 1]))
        start--;
    tlen = *cur - start;
    if (tlen >= (int)sizeof(tokbuf))
        return;
    memcpy(tokbuf, buf + start, (unsigned long)tlen);
    tokbuf[tlen] = '\0';

    for (i = 0; i < start; i++) {
        if (!is_space(buf[i])) {
            is_cmd = 0;
            break;
        }
    }

    slash = strrchr(tokbuf, '/');
    if (slash) {
        int hl = (int)(slash - tokbuf);
        char head[MAX_PATH];

        memcpy(head, tokbuf, (unsigned long)hl);
        head[hl] = '\0';
        if (hl == 0)
            snprintf(dir, sizeof(dir), "/");
        else
            path_join_norm(cwd, head, dir, sizeof(dir));
        pfx = slash + 1;
    } else if (is_cmd) {
        snprintf(dir, sizeof(dir), "/bin");
        pfx = tokbuf;
    } else {
        snprintf(dir, sizeof(dir), "%s", cwd);
        pfx = tokbuf;
    }

    n = gather(dir, pfx);
    if (n == 0)
        return;

    plen = (int)strlen(pfx);
    if (n == 1) {
        snprintf(add, sizeof(add), "%s%s", cand[0] + plen,
                 cand_dir[0] ? "/" : " ");
        line_insert(buf, len, cur, max, add);
        line_redraw(prompt, buf, *len, *cur);
        return;
    }

    lcp = common_prefix_len(n);
    if (lcp > plen) {
        int extra = lcp - plen;

        if (extra > (int)sizeof(add) - 1)
            extra = (int)sizeof(add) - 1;
        memcpy(add, cand[0] + plen, (unsigned long)extra);
        add[extra] = '\0';
        line_insert(buf, len, cur, max, add);
    }

    putchar('\n');
    for (i = 0; i < n; i++)
        printf("%s%s  ", cand[i], cand_dir[i] ? "/" : "");
    putchar('\n');
    line_redraw(prompt, buf, *len, *cur);
}

/* --- readline --------------------------------------------------------- */

/* Read one edited line. Returns 1 with buf filled (possibly empty, when
 * the line was cancelled with ctrl-C), or 0 for end of input. */
static int sh_readline(const char *prompt, char *buf, int max)
{
    char saved[MAX_LINE];
    int len = 0, cur = 0, hpos = 0;

    saved[0] = '\0';
    buf[0] = '\0';
    printf("%s", prompt);

    for (;;) {
        int c = getchar();

        if (c == EOF) {
            if (len == 0)
                return 0;
            buf[len] = '\0';
            putchar('\n');
            return 1;
        }
        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            putchar('\n');
            return 1;
        }
        if (c == 3) {                   /* ctrl-C: drop the line */
            printf("^C\n");
            buf[0] = '\0';
            return 1;
        }
        if (c == 4) {                   /* ctrl-D: EOF, else delete-forward */
            if (len == 0)
                return 0;
            if (cur < len) {
                line_delete_at(buf, &len, cur);
                line_redraw(prompt, buf, len, cur);
            }
            continue;
        }
        if (c == '\t') {
            do_complete(prompt, buf, &len, &cur, max);
            continue;
        }
        if (c == 8 || c == 127) {       /* backspace */
            if (cur > 0) {
                cur--;
                line_delete_at(buf, &len, cur);
                line_redraw(prompt, buf, len, cur);
            }
            continue;
        }
        if (c == 21) {                  /* ctrl-U: erase line */
            len = 0;
            cur = 0;
            buf[0] = '\0';
            line_redraw(prompt, buf, len, cur);
            continue;
        }
        if (c == 11) {                  /* ctrl-K: erase to end of line */
            len = cur;
            buf[len] = '\0';
            line_redraw(prompt, buf, len, cur);
            continue;
        }
        if (c == 1 || c == KEY_HOME) {
            if (cur > 0)
                printf("\033[%dD", cur);
            cur = 0;
            continue;
        }
        if (c == 5 || c == KEY_END) {
            if (cur < len)
                printf("\033[%dC", len - cur);
            cur = len;
            continue;
        }
        if (c == KEY_LEFT) {
            if (cur > 0) {
                cur--;
                printf("\033[1D");
            }
            continue;
        }
        if (c == KEY_RIGHT) {
            if (cur < len) {
                cur++;
                printf("\033[1C");
            }
            continue;
        }
        if (c == KEY_UP || c == KEY_DOWN) {
            const char *e;

            if (c == KEY_UP) {
                if (hpos >= hist_count)
                    continue;
                if (hpos == 0)
                    snprintf(saved, sizeof(saved), "%s", buf);
                hpos++;
                e = hist_get(hpos);
            } else {
                if (hpos == 0)
                    continue;
                hpos--;
                e = hpos == 0 ? saved : hist_get(hpos);
            }
            if (!e)
                e = "";
            snprintf(buf, (unsigned long)max, "%s", e);
            len = (int)strlen(buf);
            cur = len;
            line_redraw(prompt, buf, len, cur);
            continue;
        }
        if (c == KEY_DELETE) {
            if (cur < len) {
                line_delete_at(buf, &len, cur);
                line_redraw(prompt, buf, len, cur);
            }
            continue;
        }
        if (c >= 0x80)                  /* other KEY_* codes: ignore */
            continue;
        if (c < 32)                     /* other control characters */
            continue;
        if (len >= max - 1)
            continue;
        if (cur == len) {
            /* Appending: echo the one character, no repaint needed. */
            buf[len++] = (char)c;
            buf[len] = '\0';
            cur = len;
            putchar(c);
        } else {
            char one[2];

            one[0] = (char)c;
            one[1] = '\0';
            line_insert(buf, &len, &cur, max, one);
            line_redraw(prompt, buf, len, cur);
        }
    }
}

/* --- line preprocessing ----------------------------------------------- */

/* Truncate the line at an unquoted '#' that starts a word. */
static void strip_comment(char *s)
{
    int inq = 0, i;

    for (i = 0; s[i]; i++) {
        if (s[i] == '"') {
            inq = !inq;
            continue;
        }
        if (!inq && s[i] == '#' && (i == 0 || is_space(s[i - 1]))) {
            s[i] = '\0';
            return;
        }
    }
}

/* Cut the line into ';'-separated segments in place. Returns the count,
 * or -1 if there are more segments than maxseg. */
static int split_semi(char *s, char **segs, int maxseg)
{
    int n = 1, inq = 0;
    char *p;

    segs[0] = s;
    for (p = s; *p; p++) {
        if (*p == '"') {
            inq = !inq;
            continue;
        }
        if (*p == ';' && !inq) {
            *p = '\0';
            if (n >= maxseg)
                return -1;
            segs[n++] = p + 1;
        }
    }
    return n;
}

/* Substitute $? and $PWD. Returns -1 if the result does not fit. */
static int expand_vars(const char *in, char *out, unsigned long outsz)
{
    unsigned long o = 0;

    while (*in) {
        const char *rep = 0;
        char num[16];
        int skip = 0;

        if (in[0] == '$' && in[1] == '?') {
            snprintf(num, sizeof(num), "%d", last_status);
            rep = num;
            skip = 2;
        } else if (in[0] == '$' && strncmp(in + 1, "PWD", 3) == 0 &&
                   !is_name_char((unsigned char)in[4])) {
            rep = cwd;
            skip = 4;
        }
        if (rep) {
            unsigned long rl = strlen(rep);

            if (o + rl + 1 > outsz)
                return -1;
            memcpy(out + o, rep, rl);
            o += rl;
            in += skip;
            continue;
        }
        if (o + 1 >= outsz)
            return -1;
        out[o++] = *in++;
    }
    out[o] = '\0';
    return 0;
}

/* Pull '>' / '>>' plus their target out of the token array. Returns the
 * remaining token count, or -1 on a syntax error. */
static int parse_redirect(int ntok, char **tok, char *file,
                          unsigned long fsz, int *mode)
{
    char *out[MAX_TOKENS];
    int i, n = 0;

    *mode = 0;
    file[0] = '\0';
    for (i = 0; i < ntok; i++) {
        if (tok[i][0] == '>') {
            const char *rest = tok[i] + 1;
            int append = 0;

            if (*rest == '>') {
                append = 1;
                rest++;
            }
            if (!*rest) {
                if (i + 1 >= ntok || tok[i + 1][0] == '>')
                    return -1;
                rest = tok[++i];
            }
            *mode = append ? 2 : 1;
            snprintf(file, fsz, "%s", rest);
            continue;
        }
        out[n++] = tok[i];
    }
    for (i = 0; i < n; i++)
        tok[i] = out[i];
    return n;
}

/* --- builtins --------------------------------------------------------- */

static void builtin_cd(int ntok, char **tok)
{
    char target[MAX_PATH];
    struct k_stat st;

    if (ntok < 2)
        strcpy(target, "/");
    else
        path_join_norm(cwd, tok[1], target, sizeof(target));

    if (stat_(target, &st) != 0) {
        sh_out("cd: no such path: %s\n", target);
        last_status = 1;
        return;
    }
    if (!st.is_dir) {
        sh_out("cd: not a directory: %s\n", target);
        last_status = 1;
        return;
    }
    strcpy(cwd, target);
    last_status = 0;
}

static void builtin_help(void)
{
    sh_out("builtins: cd pwd exit help -- run /bin/help for all commands\n");
    sh_out("sh2 builtins: history clear which set\n");
    sh_out("editing: left/right/home/end, ctrl-A ctrl-E ctrl-U ctrl-K,\n");
    sh_out("         up/down recalls history, ctrl-C cancels, ctrl-D exits\n");
    sh_out("tab completes /bin names for the command and paths elsewhere\n");
    sh_out("';' separates commands, '#' starts a comment, $? and $PWD expand\n");
    sh_out("'>' and '>>' redirect builtin output only: the kernel gives\n");
    sh_out("every process the console as fd 1, so redirecting an external\n");
    sh_out("command needs support in that app (or fd inheritance).\n");
    last_status = 0;
}

static void builtin_history(void)
{
    int k;

    for (k = hist_count; k >= 1; k--)
        sh_out("%4d  %s\n", hist_total - k + 1, hist_get(k));
    last_status = 0;
}

static void builtin_which(int ntok, char **tok)
{
    char path[MAX_PATH];
    int i;

    if (ntok < 2) {
        sh_out("usage: which <cmd>\n");
        last_status = 1;
        return;
    }
    last_status = 0;
    for (i = 1; i < ntok; i++) {
        if (strcmp(tok[i], "cd") == 0 || strcmp(tok[i], "pwd") == 0 ||
            strcmp(tok[i], "exit") == 0 || strcmp(tok[i], "help") == 0 ||
            strcmp(tok[i], "history") == 0 || strcmp(tok[i], "clear") == 0 ||
            strcmp(tok[i], "which") == 0 || strcmp(tok[i], "set") == 0) {
            sh_out("%s: shell builtin\n", tok[i]);
            continue;
        }
        if (resolve_cmd(tok[i], path, sizeof(path)) == 0) {
            sh_out("%s\n", path);
        } else {
            sh_out("which: not found: %s\n", tok[i]);
            last_status = 1;
        }
    }
}

static void builtin_set(void)
{
    sh_out("PWD=%s\n", cwd);
    sh_out("?=%d\n", last_status);
    last_status = 0;
}

static int is_builtin(const char *name)
{
    return strcmp(name, "cd") == 0 || strcmp(name, "pwd") == 0 ||
           strcmp(name, "exit") == 0 || strcmp(name, "help") == 0 ||
           strcmp(name, "history") == 0 || strcmp(name, "clear") == 0 ||
           strcmp(name, "which") == 0 || strcmp(name, "set") == 0;
}

static void run_builtin(int ntok, char **tok)
{
    if (strcmp(tok[0], "cd") == 0) {
        builtin_cd(ntok, tok);
    } else if (strcmp(tok[0], "pwd") == 0) {
        sh_out("%s\n", cwd);
        last_status = 0;
    } else if (strcmp(tok[0], "exit") == 0) {
        want_exit = 1;
        exit_code = ntok > 1 ? atoi(tok[1]) : 0;
    } else if (strcmp(tok[0], "help") == 0) {
        builtin_help();
    } else if (strcmp(tok[0], "history") == 0) {
        builtin_history();
    } else if (strcmp(tok[0], "clear") == 0) {
        sh_out("\033[2J\033[H");
        last_status = 0;
    } else if (strcmp(tok[0], "which") == 0) {
        builtin_which(ntok, tok);
    } else if (strcmp(tok[0], "set") == 0) {
        builtin_set();
    }
}

/* --- external commands ------------------------------------------------ */

static void run_external(int ntok, char **tok)
{
    char path[MAX_PATH];
    char cwdarg[MAX_PATH + 8];
    char *sargv[SPAWN_MAX_ARGS + 1];
    int i, pid, code, argn = 0;

    if (resolve_cmd(tok[0], path, sizeof(path)) != 0) {
        printf("sh: command not found: %s\n", tok[0]);
        last_status = 127;
        return;
    }
    /* The kernel copies at most SPAWN_MAX_ARGS argv entries and silently
     * drops the rest, which would eat the --cwd= argument. Refuse instead. */
    if (ntok > SPAWN_MAX_ARGS - 1) {
        printf("sh: too many arguments (max %d)\n", SPAWN_MAX_ARGS - 1);
        last_status = 1;
        return;
    }
    for (i = 0; i < ntok; i++)
        sargv[argn++] = tok[i];

    snprintf(cwdarg, sizeof(cwdarg), "--cwd=%s", cwd);
    /* An argument that does not fit SPAWN_ARG_MAX makes the whole spawn
     * fail, so a very deep cwd costs the hint, not the command. */
    if (strlen(cwdarg) < SPAWN_ARG_MAX) {
        sargv[argn++] = cwdarg;
    } else {
        printf("sh: cwd too long to pass on; %s will resolve against /\n",
               tok[0]);
    }
    sargv[argn] = 0;

    pid = spawn(path, sargv);
    if (pid < 0) {
        printf("sh: spawn failed: %s\n", path);
        last_status = 1;
        return;
    }
    code = waitpid(pid);
    if (code != 0)
        printf("[exit %d]\n", code);
    last_status = code;
}

/* --- one command ------------------------------------------------------ */

static void run_segment(char *seg)
{
    char expanded[MAX_LINE];
    char rfile[MAX_PATH];
    char rpath[MAX_PATH];
    char *tok[MAX_TOKENS];
    int ntok, rmode, fd;

    if (expand_vars(seg, expanded, sizeof(expanded)) != 0) {
        printf("sh: line too long after expansion\n");
        last_status = 1;
        return;
    }
    ntok = tokenize(expanded, tok);
    if (ntok < 0) {
        printf("sh: too many tokens (max %d)\n", MAX_TOKENS);
        last_status = 1;
        return;
    }
    if (ntok == 0)
        return;

    ntok = parse_redirect(ntok, tok, rfile, sizeof(rfile), &rmode);
    if (ntok < 0) {
        printf("sh: syntax error near '>'\n");
        last_status = 1;
        return;
    }
    if (ntok == 0) {
        printf("sh: missing command before '>'\n");
        last_status = 1;
        return;
    }

    if (!is_builtin(tok[0])) {
        if (rmode)
            printf("sh: %s writes to the console: '>' works for builtins "
                   "only\n", tok[0]);
        run_external(ntok, tok);
        return;
    }

    fd = 1;
    if (rmode) {
        path_join_norm(cwd, rfile, rpath, sizeof(rpath));
        fd = open(rpath, O_WRONLY | O_CREAT |
                         (rmode == 2 ? O_APPEND : O_TRUNC));
        if (fd < 0) {
            printf("sh: cannot open %s\n", rpath);
            last_status = 1;
            return;
        }
    }
    out_fd = fd;
    run_builtin(ntok, tok);
    out_fd = 1;
    if (fd != 1)
        close(fd);
}

int main(int argc, char **argv)
{
    char line[MAX_LINE];
    char prompt[MAX_PATH + 16];
    char *segs[MAX_SEGS];
    int nseg, i;

    (void)argc;
    (void)argv;

    for (;;) {
        snprintf(prompt, sizeof(prompt), "kestrel:%s$ ", cwd);
        if (!sh_readline(prompt, line, (int)sizeof(line))) {
            /* ctrl-D on an empty line: leave the shell. */
            putchar('\n');
            return 0;
        }
        hist_add(line);
        strip_comment(line);

        nseg = split_semi(line, segs, MAX_SEGS);
        if (nseg < 0) {
            printf("sh: too many commands (max %d)\n", MAX_SEGS);
            last_status = 1;
            continue;
        }
        for (i = 0; i < nseg; i++) {
            run_segment(segs[i]);
            if (want_exit)
                return exit_code;
        }
    }
}
