/* sh.c - the KestrelOS shell.
 *
 * Started by init/login on the console or by terminal over pipes; the
 * minimal shell it grew out of lives on as /bin/minsh, the fallback.
 * Core behavior: the prompt "kestrel:<cwd>$ ", whitespace tokenizing
 * with double-quote grouping (max 16 tokens), the builtins cd, pwd,
 * exit, help, history, clear, which and set, the "--cwd=<cwd>" argument
 * both honored at startup and forwarded to every child, the
 * "sh: command not found: <name>" diagnostic and the "[exit N]" report
 * for a non-zero child status.
 *
 * Features:
 *   - line editor: insert, backspace, delete, ctrl-U/ctrl-K kill,
 *     LEFT/RIGHT/HOME/END (also ctrl-A/ctrl-E), UP/DOWN recall over a
 *     32-entry history ring, ctrl-C to cancel the line, ctrl-D on an
 *     empty line to leave the shell, and tab completion;
 *   - command lists: ';', '&&', '||', trailing '&' background jobs, and
 *     '#' comments;
 *   - variable expansion: $? and $PWD;
 *   - I/O redirection ('<', '>', '>>') and multi-stage pipelines
 *     ('cmd1 | cmd2 | ...') built on pipe_() and spawn_caps(), with
 *     support for both external commands and shell builtins inside
 *     pipelines;
 *   - interactive mode (-i or console TTY) vs non-interactive pipe/file
 *     input without spurious prompts or control sequences.
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE        512
#define MAX_TOKENS      16
#define MAX_PATH        256
#define MAX_SEGS        16
#define MAX_PIPE_STAGES 8
#define MAX_BG          16
#define HIST_MAX        32
#define MAX_CAND        64
#define NAME_MAX        64
#define TERM_COLS       80      /* the VGA console is a fixed 80x25 */

/* Kernel spawn limits, from kernel/include/uproc.h. */
#define SPAWN_MAX_ARGS  16
#define SPAWN_ARG_MAX   128

enum list_op {
    LOP_SEQ = 0,
    LOP_AND,
    LOP_OR
};

struct list_item {
    char *text;
    int op;                     /* operator following this item */
    int bg;                     /* trailing '&' */
};

struct pipe_stage {
    char expanded[MAX_LINE];
    char ofile[MAX_PATH];
    char ifile[MAX_PATH];
    char ipath[MAX_PATH];
    char opath[MAX_PATH];
    char *tok[MAX_TOKENS];
    int quoted[MAX_TOKENS];
    int ntok;
    int omode;
};

static char cwd[MAX_PATH] = "/";
static int last_status;                 /* $? */
static int out_fd = 1;                  /* builtin output fd */
static int interactive = 1;
static int want_exit;
static int exit_code;

static int cap_active;
static char *cap_buf;
static unsigned long cap_len;
static unsigned long cap_cap;

static int bg_pids[MAX_BG];
static int nbg;

static char hist[HIST_MAX][MAX_LINE];
static int hist_count;                  /* entries currently stored */
static int hist_head;                   /* next slot to write */
static int hist_total;                  /* lifetime count, for numbering */

static struct pipe_stage stages[MAX_PIPE_STAGES];

/* --- small helpers ---------------------------------------------------- */

static void write_all(int fd, const char *buf, unsigned long len)
{
    unsigned long off = 0;

    while (off < len) {
        long w = write(fd, buf + off, len - off);
        if (w <= 0)
            break;
        off += (unsigned long)w;
    }
}

static void cap_reset(void)
{
    free(cap_buf);
    cap_buf = 0;
    cap_len = 0;
    cap_cap = 0;
    cap_active = 0;
}

static void cap_append(const char *s, unsigned long n)
{
    if (n == 0)
        return;
    if (cap_len + n > cap_cap) {
        unsigned long ncap = cap_cap ? cap_cap * 2 : 512;
        char *nb;

        while (ncap < cap_len + n)
            ncap *= 2;
        nb = malloc(ncap);
        if (!nb)
            return;
        if (cap_buf && cap_len > 0)
            memcpy(nb, cap_buf, cap_len);
        free(cap_buf);
        cap_buf = nb;
        cap_cap = ncap;
    }
    memcpy(cap_buf + cap_len, s, n);
    cap_len += n;
}

/* Builtin output. Goes to cap_buf when capturing a pipeline stage, or
 * to out_fd normally. */
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
        n = (int)sizeof(buf) - 1;
    if (cap_active)
        cap_append(buf, (unsigned long)n);
    else
        write_all(out_fd, buf, (unsigned long)n);
}

static int is_space(int c)
{
    return c == ' ' || c == '\t';
}

static int is_blank(const char *s)
{
    while (*s) {
        if (!is_space((unsigned char)*s))
            return 0;
        s++;
    }
    return 1;
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
            if (w == tokens[n - 1])
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

/* Resolve a command name: absolute as given, otherwise <cwd>/name then
 * /bin/name. Returns 0 and fills out. */
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
        return;
    last = hist_get(1);
    if (last && strcmp(last, line) == 0)
        return;

    snprintf(hist[hist_head], MAX_LINE, "%s", line);
    hist_head = (hist_head + 1) % HIST_MAX;
    if (hist_count < HIST_MAX)
        hist_count++;
    hist_total++;
}

/* --- line editing ----------------------------------------------------- */

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
    int end = (plen + len) / TERM_COLS;
    int i;

    line_up(line_row);
    printf("\r%s%s\033[K", prompt, buf);
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

    for (i = start - 1; i >= 0; i--) {
        if (is_space(buf[i]))
            continue;
        if (buf[i] == ';' || buf[i] == '|' || buf[i] == '&')
            is_cmd = 1;
        else
            is_cmd = 0;
        break;
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

    if (line_rows > line_row)
        printf("\033[%dB", line_rows - line_row);
    putchar('\n');
    for (i = 0; i < n; i++)
        printf("%s%s  ", cand[i], cand_dir[i] ? "/" : "");
    putchar('\n');
    line_row = 0;
    line_rows = 0;
    line_redraw(prompt, buf, *len, *cur);
}

/* --- readline --------------------------------------------------------- */

/* Read one edited line in interactive mode. Returns 1 with buf filled
 * (possibly empty, when cancelled with ctrl-C), or 0 for end of input. */
static int sh_readline(const char *prompt, char *buf, int max)
{
    char saved[MAX_LINE];
    int len = 0, cur = 0, hpos = 0;
    int plen = (int)strlen(prompt);

    saved[0] = '\0';
    buf[0] = '\0';
    printf("%s", prompt);
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
            line_seek(prompt, len);
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
        if (c >= 0x80)
            continue;
        if (c < 32)
            continue;
        if (len >= max - 1)
            continue;
        if (cur == len) {
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

/* Read one plain line from a non-interactive pipe/file (no prompt, no
 * echo, no ANSI escapes). */
static int read_plain_line(char *buf, int max)
{
    int len = 0;

    for (;;) {
        unsigned char c;
        long r = read(0, &c, 1);

        if (r <= 0) {
            if (len == 0)
                return 0;
            buf[len] = '\0';
            return 1;
        }
        if (c == '\n') {
            if (len > 0 && buf[len - 1] == '\r')
                len--;
            buf[len] = '\0';
            return 1;
        }
        if (len < max - 1)
            buf[len++] = (char)c;
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
        if (!inq && s[i] == '#' &&
            (i == 0 || is_space(s[i - 1]) || s[i - 1] == ';')) {
            s[i] = '\0';
            return;
        }
    }
}

/* Cut line into items separated by ';', '&&', '||', and '&' in place.
 * Returns item count, -1 on syntax error, -2 on too many items. */
static int split_list(char *s, struct list_item *items, int maxitems)
{
    int n = 0, inq = 0;
    char *start = s, *p = s;

    while (*p) {
        if (*p == '"') {
            inq = !inq;
            p++;
            continue;
        }
        if (inq) {
            p++;
            continue;
        }
        if (*p == ';') {
            *p++ = '\0';
            if (n >= maxitems)
                return -2;
            items[n].text = start;
            items[n].op = LOP_SEQ;
            items[n].bg = 0;
            n++;
            start = p;
            continue;
        }
        if (p[0] == '&' && p[1] == '&') {
            if (p[2] == '&')
                return -1;
            *p = '\0';
            p += 2;
            if (is_blank(start))
                return -1;
            if (n >= maxitems)
                return -2;
            items[n].text = start;
            items[n].op = LOP_AND;
            items[n].bg = 0;
            n++;
            start = p;
            if (is_blank(start))
                return -1;
            continue;
        }
        if (*p == '&') {
            *p++ = '\0';
            if (is_blank(start))
                return -1;
            if (n >= maxitems)
                return -2;
            items[n].text = start;
            items[n].op = LOP_SEQ;
            items[n].bg = 1;
            n++;
            start = p;
            continue;
        }
        if (p[0] == '|' && p[1] == '|') {
            if (p[2] == '|')
                return -1;
            *p = '\0';
            p += 2;
            if (is_blank(start))
                return -1;
            if (n >= maxitems)
                return -2;
            items[n].text = start;
            items[n].op = LOP_OR;
            items[n].bg = 0;
            n++;
            start = p;
            if (is_blank(start))
                return -1;
            continue;
        }
        p++;
    }
    if (!is_blank(start)) {
        if (n >= maxitems)
            return -2;
        items[n].text = start;
        items[n].op = LOP_SEQ;
        items[n].bg = 0;
        n++;
    }
    return n;
}

/* Split an item on single unquoted '|'. Returns stage count, -1 on
 * syntax error, -2 on too many stages. */
static int split_pipe(char *s, char **out, int maxstages)
{
    int n = 0, inq = 0;
    char *start = s, *p = s;

    while (*p) {
        if (*p == '"') {
            inq = !inq;
            p++;
            continue;
        }
        if (!inq && *p == '|') {
            *p++ = '\0';
            if (is_blank(start))
                return -1;
            if (n >= maxstages)
                return -2;
            out[n++] = start;
            start = p;
            if (is_blank(start))
                return -1;
            continue;
        }
        p++;
    }
    if (is_blank(start))
        return n > 0 ? -1 : 0;
    if (n >= maxstages)
        return -2;
    out[n++] = start;
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
 * Returns the remaining token count, or -1 on a syntax error. */
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
    sh_out("operators: ';', '&&', '||', '&' background, '#' comments,\n");
    sh_out("           $? and $PWD, '|', '<', '>', '>>'\n");
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

static void run_builtin(int ntok, char **tok, int in_pipeline)
{
    if (strcmp(tok[0], "cd") == 0) {
        if (in_pipeline) {
            char saved_cwd[MAX_PATH];
            snprintf(saved_cwd, sizeof(saved_cwd), "%s", cwd);
            builtin_cd(ntok, tok);
            snprintf(cwd, sizeof(cwd), "%s", saved_cwd);
        } else {
            builtin_cd(ntok, tok);
        }
    } else if (strcmp(tok[0], "pwd") == 0) {
        sh_out("%s\n", cwd);
        last_status = 0;
    } else if (strcmp(tok[0], "exit") == 0) {
        int code = ntok > 1 ? atoi(tok[1]) : 0;
        if (!in_pipeline) {
            want_exit = 1;
            exit_code = code;
        }
        last_status = code;
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

/* --- background reaping ----------------------------------------------- */

static void bg_track(int pid)
{
    if (nbg < MAX_BG)
        bg_pids[nbg++] = pid;
}

static void reap_bg(void)
{
    int i = 0;

    while (i < nbg) {
        struct k_psinfo pi;
        int alive = 0;
        int j;

        for (j = 0; psinfo(j, &pi) == 0; j++) {
            if (pi.pid == bg_pids[i]) {
                alive = (pi.state != K_STATE_ZOMBIE);
                break;
            }
        }
        if (!alive) {
            int code = waitpid(bg_pids[i]);
            if (interactive)
                printf("[bg done %d: exit %d]\n", bg_pids[i], code);
            bg_pids[i] = bg_pids[--nbg];
            continue;
        }
        i++;
    }
}

/* --- pipeline and external execution ---------------------------------- */

static int parse_stage(const char *raw, struct pipe_stage *st)
{
    struct k_stat kst;

    if (expand_vars(raw, st->expanded, sizeof(st->expanded)) != 0) {
        printf("sh: line too long after expansion\n");
        return -1;
    }
    st->ntok = tokenize(st->expanded, st->tok, st->quoted);
    if (st->ntok < 0) {
        printf("sh: too many tokens (max %d)\n", MAX_TOKENS);
        return -1;
    }
    if (st->ntok == 0)
        return 0;

    st->ntok = parse_redirect(st->ntok, st->tok, st->quoted, st->ofile,
                              sizeof(st->ofile), &st->omode, st->ifile,
                              sizeof(st->ifile));
    if (st->ntok < 0) {
        printf("sh: syntax error in redirection\n");
        return -1;
    }
    if (st->ntok == 0) {
        printf("sh: missing command before redirection\n");
        return -1;
    }
    if (st->ifile[0]) {
        path_join_norm(cwd, st->ifile, st->ipath, sizeof(st->ipath));
        if (stat_(st->ipath, &kst) != 0 || kst.is_dir) {
            printf("sh: cannot open %s\n", st->ipath);
            return -1;
        }
    }
    if (st->omode)
        path_join_norm(cwd, st->ofile, st->opath, sizeof(st->opath));
    return 1;
}

static int spawn_stage_external(const struct pipe_stage *st, int in_fd,
                                int out_fd_stage, int *status_out)
{
    char path[MAX_PATH];
    char cwdarg[MAX_PATH + 8];
    char *sargv[SPAWN_MAX_ARGS + 1];
    struct k_spawn_caps caps;
    int i, pid, argn = 0;

    if (resolve_cmd(st->tok[0], path, sizeof(path)) != 0) {
        printf("sh: command not found: %s\n", st->tok[0]);
        *status_out = 127;
        return -1;
    }
    if (st->ntok > SPAWN_MAX_ARGS - 1) {
        printf("sh: too many arguments (max %d)\n", SPAWN_MAX_ARGS - 1);
        *status_out = 1;
        return -1;
    }
    for (i = 0; i < st->ntok; i++) {
        if (strlen(st->tok[i]) >= SPAWN_ARG_MAX) {
            printf("sh: argument %d too long (max %d)\n", i,
                   SPAWN_ARG_MAX - 1);
            *status_out = 1;
            return -1;
        }
    }
    for (i = 0; i < st->ntok; i++)
        sargv[argn++] = st->tok[i];

    snprintf(cwdarg, sizeof(cwdarg), "--cwd=%s", cwd);
    if (strlen(cwdarg) < SPAWN_ARG_MAX) {
        sargv[argn++] = cwdarg;
    } else {
        printf("sh: cwd too long to pass on; %s will resolve against /\n",
               st->tok[0]);
    }
    sargv[argn] = 0;

    memset(&caps, 0, sizeof(caps));
    caps.fds[0].child_fd = 0;
    caps.fds[0].parent_fd = in_fd;
    caps.fds[1].child_fd = 1;
    caps.fds[1].parent_fd = out_fd_stage;
    caps.fds[2].child_fd = 2;
    caps.fds[2].parent_fd = 2;
    caps.n_fds = 3;
    caps.flags = K_SPAWN_CLOSE_OTHER;

    pid = spawn_caps(path, sargv, &caps);
    if (pid < 0) {
        printf("sh: spawn failed: %s\n", path);
        *status_out = 1;
        return -1;
    }
    return pid;
}

static void run_pipeline(char *seg, int bg)
{
    char *raw_stages[MAX_PIPE_STAGES];
    int pids[MAX_PIPE_STAGES];
    int nstages, i, s;
    int prev_read = -1;
    int pending_wr = -1;
    int npids = 0, last_pid = -1;
    int last_stage_status = 0;

    nstages = split_pipe(seg, raw_stages, MAX_PIPE_STAGES);
    if (nstages == -1) {
        printf("sh: syntax error near '|'\n");
        last_status = 1;
        return;
    }
    if (nstages == -2) {
        printf("sh: too many pipeline stages (max %d)\n", MAX_PIPE_STAGES);
        last_status = 1;
        return;
    }
    if (nstages == 0)
        return;

    for (s = 0; s < nstages; s++) {
        int pr = parse_stage(raw_stages[s], &stages[s]);
        if (pr < 0) {
            last_status = 1;
            return;
        }
        if (pr == 0) {
            if (nstages > 1) {
                printf("sh: syntax error near '|'\n");
                last_status = 1;
            }
            return;
        }
    }

    /* Single-stage builtin without background runs directly in the shell. */
    if (nstages == 1 && is_builtin(stages[0].tok[0]) && !bg) {
        int fd = 1;
        if (stages[0].omode) {
            fd = open(stages[0].opath, O_WRONLY | O_CREAT |
                      (stages[0].omode == 2 ? O_APPEND : O_TRUNC));
            if (fd < 0) {
                printf("sh: cannot open %s\n", stages[0].opath);
                last_status = 1;
                return;
            }
        }
        out_fd = fd;
        run_builtin(stages[0].ntok, stages[0].tok, 0);
        out_fd = 1;
        if (fd != 1)
            close(fd);
        return;
    }

    for (s = 0; s < nstages; s++) {
        int pnext[2] = { -1, -1 };
        int opened_in = -1, opened_out = -1;
        int in_fd, out_fd_stage;
        int stage_is_builtin = is_builtin(stages[s].tok[0]);

        if (s < nstages - 1) {
            if (pipe_(pnext) != 0) {
                printf("sh: pipe failed\n");
                last_stage_status = 1;
                break;
            }
        }

        if (stages[s].ifile[0]) {
            opened_in = open(stages[s].ipath, O_RDONLY);
            if (opened_in < 0) {
                printf("sh: cannot open %s\n", stages[s].ipath);
                if (pnext[0] >= 0) close(pnext[0]);
                if (pnext[1] >= 0) close(pnext[1]);
                last_stage_status = 1;
                break;
            }
        } else if (bg && s == 0 && prev_read < 0) {
            opened_in = open("/dev/null", O_RDONLY);
        }

        if (stages[s].omode) {
            opened_out = open(stages[s].opath, O_WRONLY | O_CREAT |
                              (stages[s].omode == 2 ? O_APPEND : O_TRUNC));
            if (opened_out < 0) {
                printf("sh: cannot open %s\n", stages[s].opath);
                if (opened_in >= 0) close(opened_in);
                if (pnext[0] >= 0) close(pnext[0]);
                if (pnext[1] >= 0) close(pnext[1]);
                last_stage_status = 1;
                break;
            }
        }

        in_fd = (opened_in >= 0) ? opened_in : (prev_read >= 0 ? prev_read : 0);
        out_fd_stage = (opened_out >= 0) ? opened_out
                     : (s < nstages - 1 ? pnext[1] : 1);

        if (stage_is_builtin) {
            /* A builtin reads no stdin, so drop any upstream builtin/pipe. */
            if (pending_wr >= 0) {
                close(pending_wr);
                pending_wr = -1;
            }
            cap_reset();
            if (prev_read >= 0) {
                close(prev_read);
                prev_read = -1;
            }
            if (opened_in >= 0) {
                close(opened_in);
                opened_in = -1;
            }

            if (s < nstages - 1 && opened_out < 0) {
                /* Capture output in memory until stage s+1's reader spawns,
                 * avoiding pipe-buffer deadlock on >4 KiB output. */
                cap_active = 1;
                run_builtin(stages[s].ntok, stages[s].tok, nstages > 1);
                cap_active = 0;
                pending_wr = pnext[1];
                prev_read = pnext[0];
            } else {
                out_fd = out_fd_stage;
                run_builtin(stages[s].ntok, stages[s].tok, nstages > 1);
                out_fd = 1;
                if (opened_out >= 0)
                    close(opened_out);
                if (pnext[1] >= 0)
                    close(pnext[1]);
                prev_read = pnext[0];
            }
            last_pid = -1;
            last_stage_status = last_status;
            continue;
        }

        int st_err = 0;
        int pid = spawn_stage_external(&stages[s], in_fd, out_fd_stage, &st_err);

        /* Close parent-side FDs handed off to the child immediately. */
        if (prev_read >= 0) {
            close(prev_read);
            prev_read = -1;
        }
        if (opened_in >= 0)
            close(opened_in);
        if (opened_out >= 0)
            close(opened_out);
        if (pnext[1] >= 0)
            close(pnext[1]);

        if (pid < 0) {
            if (pending_wr >= 0) {
                close(pending_wr);
                pending_wr = -1;
            }
            cap_reset();
            last_pid = -1;
            last_stage_status = st_err;
            prev_read = pnext[0];
            continue;
        }

        pids[npids++] = pid;
        last_pid = pid;

        if (pending_wr >= 0) {
            if (cap_buf && cap_len > 0)
                write_all(pending_wr, cap_buf, cap_len);
            close(pending_wr);
            pending_wr = -1;
            cap_reset();
        }
        prev_read = pnext[0];
    }

    if (prev_read >= 0)
        close(prev_read);
    if (pending_wr >= 0)
        close(pending_wr);
    cap_reset();

    if (bg) {
        for (i = 0; i < npids; i++)
            bg_track(pids[i]);
        if (npids > 0 && interactive)
            printf("[bg] %d\n", pids[npids - 1]);
        last_status = 0;
        return;
    }

    {
        int code = last_stage_status;
        for (i = 0; i < npids; i++) {
            int w = waitpid(pids[i]);
            if (pids[i] == last_pid)
                code = w;
        }
        if (code != 0 && last_pid >= 0)
            printf("[exit %d]\n", code);
        last_status = code;
    }
}

static void run_line(char *line)
{
    struct list_item items[MAX_SEGS];
    int nitems, i, prev_op;

    strip_comment(line);
    nitems = split_list(line, items, MAX_SEGS);
    if (nitems == -1) {
        printf("sh: syntax error near unexpected token\n");
        last_status = 1;
        return;
    }
    if (nitems == -2) {
        printf("sh: too many commands (max %d)\n", MAX_SEGS);
        last_status = 1;
        return;
    }

    prev_op = LOP_SEQ;
    for (i = 0; i < nitems; i++) {
        int should_run = 1;

        if (prev_op == LOP_AND && last_status != 0)
            should_run = 0;
        else if (prev_op == LOP_OR && last_status == 0)
            should_run = 0;

        if (should_run && !is_blank(items[i].text)) {
            run_pipeline(items[i].text, items[i].bg);
            if (want_exit)
                return;
        }
        prev_op = items[i].op;
    }
}

static int stdin_is_tty(void)
{
    int d = dup2_(0, 3);

    if (d >= 0) {
        close(d);
        return 0;
    }
    return 1;
}

static void apply_cwd_arg(const char *arg)
{
    char norm[MAX_PATH];
    struct k_stat st;

    if (!arg || arg[0] != '/') {
        snprintf(cwd, sizeof(cwd), "/");
        return;
    }
    path_join_norm("/", arg, norm, sizeof(norm));
    if (stat_(norm, &st) == 0 && st.is_dir)
        snprintf(cwd, sizeof(cwd), "%s", norm);
    else
        snprintf(cwd, sizeof(cwd), "/");
}

int main(int argc, char **argv)
{
    char line[MAX_LINE];
    char prompt[MAX_PATH + 16];
    const char *cmd_str = 0;
    int force_interactive = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) == 0) {
            apply_cwd_arg(argv[i] + 6);
        } else if (strcmp(argv[i], "-i") == 0) {
            force_interactive = 1;
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            cmd_str = argv[++i];
        }
    }

    if (cmd_str) {
        interactive = 0;
        snprintf(line, sizeof(line), "%s", cmd_str);
        run_line(line);
        reap_bg();
        return want_exit ? exit_code : last_status;
    }

    interactive = force_interactive || stdin_is_tty();
    if (interactive)
        signal(SIGINT, (sighandler_t)SIG_IGN);

    for (;;) {
        reap_bg();
        if (interactive) {
            snprintf(prompt, sizeof(prompt), "kestrel:%s$ ", cwd);
            if (!sh_readline(prompt, line, (int)sizeof(line))) {
                putchar('\n');
                return last_status;
            }
            hist_add(line);
        } else {
            if (!read_plain_line(line, (int)sizeof(line)))
                return last_status;
        }
        run_line(line);
        if (want_exit)
            return exit_code;
    }
}
