/* sh.c - the KestrelOS shell.
 *
 * Started by init on the console; the minimal shell it grew out of
 * lives on as /bin/minsh, the fallback. Core behavior: the prompt
 * "kestrel:<cwd>$ ", whitespace tokenizing with double-quote grouping
 * (max 16 tokens), the builtins cd, pwd, exit and help, the
 * "--cwd=<cwd>" argument appended to every child, the "sh: command not
 * found: <name>" diagnostic and the "[exit N]" report for a non-zero
 * child status.
 *
 * On top of that it provides:
 *   - its own line editor (no libc change): insert, backspace, delete,
 *     ctrl-U/ctrl-K kill, LEFT/RIGHT/HOME/END (also ctrl-A/ctrl-E),
 *     UP/DOWN recall over a 32-entry history ring, ctrl-C to cancel the
 *     line, ctrl-D on an empty line to leave the shell;
 *   - tab completion: command names out of /bin for argv[0], otherwise
 *     file and directory names out of the containing directory;
 *   - ';' to separate commands and '#' to start a comment;
 *   - $? and $PWD expansion (there is no environment to speak of);
 *   - I/O redirection: 'cmd > file' truncates, 'cmd >> file' appends,
 *     'cmd < file' feeds the file to the command's stdin. External
 *     commands go through spawn_io()/SYS_SPAWN_IO, which opens the
 *     named files onto the child's fd 0/1 in the kernel before the
 *     program runs, so every app's ordinary read(0)/write(1) is
 *     redirected without knowing it; fd 2 stays on the console.
 *     Builtin output through '>' / '>>' is written by the shell
 *     itself. A quoted ">" or "<" is a literal argument, not an
 *     operator;
 *   - the builtins history, clear, which and set.
 *
 * Screen updates are done by rewriting the line. A line longer than
 * TERM_COLS occupies several physical rows, so the repaint first walks
 * back up to the row the prompt started on (CSI A), rewrites prompt and
 * buffer, erases every row the line used to reach (CSI K per row), then
 * places the cursor with CSI A/B plus '\r' and CSI C. That works on the
 * VGA console (kernel/console.c implements \r, CSI K and CSI A/B/C/D,
 * and wraps to the next row as soon as column 80 is passed) and on a
 * plain serial terminal. CSI J is deliberately not used: the console
 * only implements the "whole screen" form of it. '\b' is never used to
 * move the cursor, only to erase, because the VGA console's backspace
 * is destructive.
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
#define TERM_COLS   80          /* the VGA console is a fixed 80x25 */

/* Kernel spawn limits, from kernel/include/uproc.h. Exceeding either of
 * them is silent truncation - of the argument list past the 16th entry,
 * or of an individual argument past 127 bytes - so the shell checks both
 * before calling spawn() and refuses rather than letting an app run on
 * half an argument. */
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
 * Double quotes group words and are stripped from the token; quoted[i]
 * records whether the FIRST character of token i came from inside
 * quotes, which is what tells a literal ">" from the redirection
 * operator while leaving >"file" a redirection. */
static int tokenize(char *line, char **tokens, int *quoted)
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
        tokens[n] = p;
        quoted[n] = 0;
        n++;
        w = p;
        while (*p && (inq || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') {
                inq = !inq;
                p++;
                continue;
            }
            if (w == tokens[n - 1])         /* first byte kept, if any */
                quoted[n - 1] = inq;
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

/* Where the line editor currently is on screen, in physical rows counted
 * from the row the prompt starts on. line_row is the cursor, line_rows
 * the last row the painted line reaches. Nothing but the line editor
 * writes to the screen while a line is being edited, so tracking these
 * two is enough to repaint a line that wraps over several rows. */
static int line_row;
static int line_rows;

static void line_up(int n)
{
    if (n > 0)
        printf("\033[%dA", n);
}

/* Place the cursor at logical offset cur without repainting. */
static void line_seek(const char *prompt, int cur)
{
    int plen = (int)strlen(prompt);
    int row = (plen + cur) / TERM_COLS;
    int col = (plen + cur) % TERM_COLS;

    if (row < line_row)
        line_up(line_row - row);
    else if (row > line_row)
        printf("\033[%dB", row - line_row);
    putchar('\r');
    if (col > 0)
        printf("\033[%dC", col);
    line_row = row;
}

/* Full repaint: back to the first row of the line, prompt, buffer, erase
 * every row the line used to occupy, then place the cursor. */
static void line_redraw(const char *prompt, const char *buf, int len, int cur)
{
    int plen = (int)strlen(prompt);
    int end = (plen + len) / TERM_COLS;   /* row the line now ends on */
    int i;

    line_up(line_row);
    printf("\r%s%s\033[K", prompt, buf);
    /* CSI K only clears the row the cursor is on and the console has no
     * erase-below, so wipe the rows a longer line left behind by hand. */
    for (i = end; i < line_rows; i++)
        printf("\n\033[K");
    line_up(line_rows - end);
    line_rows = end;
    line_row = end;
    line_seek(prompt, cur);
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

    /* Step below the whole painted line before listing the candidates,
     * or the listing lands on top of a wrapped line's tail. */
    if (line_rows > line_row)
        printf("\033[%dB", line_rows - line_row);
    putchar('\n');
    for (i = 0; i < n; i++)
        printf("%s%s  ", cand[i], cand_dir[i] ? "/" : "");
    putchar('\n');
    line_row = 0;               /* the listing scrolled the line away */
    line_rows = 0;
    line_redraw(prompt, buf, *len, *cur);
}

/* --- readline --------------------------------------------------------- */

/* Read one edited line. Returns 1 with buf filled (possibly empty, when
 * the line was cancelled with ctrl-C), or 0 for end of input. */
static int sh_readline(const char *prompt, char *buf, int max)
{
    char saved[MAX_LINE];
    int len = 0, cur = 0, hpos = 0;
    int plen = (int)strlen(prompt);

    saved[0] = '\0';
    buf[0] = '\0';
    printf("%s", prompt);
    /* A long cwd can wrap the prompt itself, so start from where it ends. */
    line_row = plen / TERM_COLS;
    line_rows = line_row;

    for (;;) {
        int c = getchar();

        if (c == EOF) {
            if (len == 0)
                return 0;
            buf[len] = '\0';
            line_seek(prompt, len);
            putchar('\n');
            return 1;
        }
        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            line_seek(prompt, len);     /* past the tail of a wrapped line */
            putchar('\n');
            return 1;
        }
        if (c == 3) {                   /* ctrl-C: drop the line */
            line_seek(prompt, len);
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
            cur = 0;
            line_seek(prompt, cur);
            continue;
        }
        if (c == 5 || c == KEY_END) {
            cur = len;
            line_seek(prompt, cur);
            continue;
        }
        if (c == KEY_LEFT) {
            if (cur > 0) {
                cur--;
                line_seek(prompt, cur);
            }
            continue;
        }
        if (c == KEY_RIGHT) {
            if (cur < len) {
                cur++;
                line_seek(prompt, cur);
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
            /* Appending: echo the one character, no repaint needed. The
             * console wraps as soon as column 80 is passed, so the row
             * the cursor lands on follows from the new length. */
            buf[len++] = (char)c;
            buf[len] = '\0';
            cur = len;
            putchar(c);
            line_row = (plen + cur) / TERM_COLS;
            line_rows = line_row;
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

/* Truncate the line at an unquoted '#' that starts a word. A ';' ends a
 * word just as whitespace does, so "echo a;# comment" comments too. */
static void strip_comment(char *s)
{
    int inq = 0, i;

    for (i = 0; s[i]; i++) {
        if (s[i] == '"') {
            inq = !inq;
            continue;
        }
        if (!inq && s[i] == '#' &&
            (i == 0 || is_space(s[i - 1]) || s[i - 1] == ';')) {
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

/* Pull '<', '>' and '>>' plus their targets out of the token array.
 * Returns the remaining token count, or -1 on a syntax error. A token
 * that carried double quotes is never an operator, so `echo ">"` passes
 * a literal '>' along the way sh always did. */
static int parse_redirect(int ntok, char **tok, int *quoted, char *ofile,
                          unsigned long ofsz, int *omode, char *ifile,
                          unsigned long ifsz)
{
    char *out[MAX_TOKENS];
    int outq[MAX_TOKENS];
    int i, n = 0;

    *omode = 0;
    ofile[0] = '\0';
    ifile[0] = '\0';
    for (i = 0; i < ntok; i++) {
        if (!quoted[i] && (tok[i][0] == '>' || tok[i][0] == '<')) {
            int input = tok[i][0] == '<';
            const char *rest = tok[i] + 1;
            int append = 0;

            if (!input && *rest == '>') {
                append = 1;
                rest++;
            }
            if (!*rest) {
                if (i + 1 >= ntok ||
                    (!quoted[i + 1] &&
                     (tok[i + 1][0] == '>' || tok[i + 1][0] == '<')))
                    return -1;
                rest = tok[++i];
            }
            if (input) {
                snprintf(ifile, ifsz, "%s", rest);
            } else {
                *omode = append ? 2 : 1;
                snprintf(ofile, ofsz, "%s", rest);
            }
            continue;
        }
        out[n] = tok[i];
        outq[n] = quoted[i];
        n++;
    }
    for (i = 0; i < n; i++) {
        tok[i] = out[i];
        quoted[i] = outq[i];
    }
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
    sh_out("more builtins: history clear which set\n");
    sh_out("editing: left/right/home/end, ctrl-A ctrl-E ctrl-U ctrl-K,\n");
    sh_out("         up/down recalls history, ctrl-C cancels, ctrl-D exits\n");
    sh_out("tab completes /bin names for the command and paths elsewhere\n");
    sh_out("';' separates commands, '#' starts a comment, $? and $PWD expand\n");
    sh_out("'cmd > file' truncates, 'cmd >> file' appends the command's\n");
    sh_out("output; 'cmd < file' feeds the file to its input\n");
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

/* Run an external command. ifile/ofile are the '<' / '>' targets
 * relative to the cwd ("" = none); omode is 2 for '>>', else '>'
 * truncates. Redirections are wired up by the kernel via spawn_io(). */
static void run_external(int ntok, char **tok, const char *ifile,
                         const char *ofile, int omode)
{
    char path[MAX_PATH];
    char ipath[MAX_PATH];
    char opath[MAX_PATH];
    char cwdarg[MAX_PATH + 8];
    char *sargv[SPAWN_MAX_ARGS + 1];
    const char *in = 0, *out = 0;
    struct k_stat st;
    int i, pid, code, argn = 0;

    if (resolve_cmd(tok[0], path, sizeof(path)) != 0) {
        printf("sh: command not found: %s\n", tok[0]);
        last_status = 127;
        return;
    }
    if (ifile[0]) {
        path_join_norm(cwd, ifile, ipath, sizeof(ipath));
        /* A missing input file is the shell's error to report; checking
         * here keeps the kernel's spawn diagnostic off the console. */
        if (stat_(ipath, &st) != 0 || st.is_dir) {
            printf("sh: cannot open %s\n", ipath);
            last_status = 1;
            return;
        }
        in = ipath;
    }
    if (omode) {
        path_join_norm(cwd, ofile, opath, sizeof(opath));
        out = opath;
    }
    /* The kernel copies at most SPAWN_MAX_ARGS argv entries and silently
     * drops the rest, which would eat the --cwd= argument. Refuse instead. */
    if (ntok > SPAWN_MAX_ARGS - 1) {
        printf("sh: too many arguments (max %d)\n", SPAWN_MAX_ARGS - 1);
        last_status = 1;
        return;
    }
    /* An argument longer than SPAWN_ARG_MAX is truncated by the kernel
     * without a word, which hands the app half a path. Refuse instead. */
    for (i = 0; i < ntok; i++) {
        if (strlen(tok[i]) >= SPAWN_ARG_MAX) {
            printf("sh: argument %d too long (max %d)\n", i,
                   SPAWN_ARG_MAX - 1);
            last_status = 1;
            return;
        }
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

    if (in || out)
        pid = spawn_io(path, sargv, in, out, omode == 2);
    else
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
    char ofile[MAX_PATH];
    char ifile[MAX_PATH];
    char rpath[MAX_PATH];
    char *tok[MAX_TOKENS];
    int quoted[MAX_TOKENS];
    int ntok, rmode, fd;

    if (expand_vars(seg, expanded, sizeof(expanded)) != 0) {
        printf("sh: line too long after expansion\n");
        last_status = 1;
        return;
    }
    ntok = tokenize(expanded, tok, quoted);
    if (ntok < 0) {
        printf("sh: too many tokens (max %d)\n", MAX_TOKENS);
        last_status = 1;
        return;
    }
    if (ntok == 0)
        return;

    ntok = parse_redirect(ntok, tok, quoted, ofile, sizeof(ofile), &rmode,
                          ifile, sizeof(ifile));
    if (ntok < 0) {
        printf("sh: syntax error in redirection\n");
        last_status = 1;
        return;
    }
    if (ntok == 0) {
        printf("sh: missing command before redirection\n");
        last_status = 1;
        return;
    }

    if (!is_builtin(tok[0])) {
        run_external(ntok, tok, ifile, ofile, rmode);
        return;
    }

    /* Builtins read no input, so '<' is accepted and ignored; '>' and
     * '>>' capture their output, written by the shell itself. */
    fd = 1;
    if (rmode) {
        path_join_norm(cwd, ofile, rpath, sizeof(rpath));
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
