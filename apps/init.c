/* init.c - PID 1 for KestrelOS: configuration-driven service supervisor.
 *
 * init reads /etc/inittab, runs the sysinit directives to completion, then
 * starts services described by /etc/services/<name>.svc respecting their
 * `after=` ordering, then the once/respawn directives. From there it runs
 * a poll loop that reaps dead children, restarts respawning units with a
 * backoff, serves control requests from /run/init.cmd (written by
 * `service`) and performs an orderly shutdown when /run/shutdown appears
 * (written by `reboot` / `halt`).
 *
 * The file formats and the /run protocol are documented in docs/init.md.
 *
 * Reaping note: SYS_WAITANY is not usable as this loop's reaper. It blocks
 * for as long as any child is alive (init must keep polling /run), and the
 * kernel unlinks a zombie from the task list on the next schedule(), so a
 * sleeping supervisor usually misses the exit and then waits forever on
 * the surviving children. init therefore detects exits with SYS_PSINFO and
 * collects the status with SYS_WAITPID, which reads the same exit ring and
 * always returns immediately for a process that is already gone. See
 * docs/init.md for the kernel change that would let init use SYS_WAITANY.
 */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- tunables --------------------------------------------------------- */

#define POLL_MS         200        /* supervisor tick */
#define FAIL_WINDOW_MS  30000UL    /* crash-loop detection window */
#define FAIL_MAX        5          /* deaths in the window before failing */
#define SETTLED_MS      1000UL     /* uptime after which exit 0 is normal */
#define STOP_GRACE_MS   3000UL     /* wait for children during shutdown */
#define START_TIMEOUT_MS 5000UL     /* ready= deadline */

#define MAX_UNITS       24
#define MAX_SVCARGS     8
#define MAX_DEPS        8
#define NAMESZ          24
#define PATHSZ          96
#define DEPSZ           128
#define ARGSZ           128
#define TABSZ           2048
#define SVCSZ           1024
#define STATESZ         2048

#define INITTAB_PATH    "/etc/inittab"
#define SVC_DIR         "/etc/services"
#define RUN_DIR         "/run"
#define STATE_PATH      "/run/services.state"
#define CMD_PATH        "/run/init.cmd"
#define ACK_PATH        "/run/init.ack"
#define SHUTDOWN_PATH   "/run/shutdown"
#define CONSOLE_DEFAULT "/bin/login|/bin/sh"

/* Restart backoff, indexed by consecutive failures in the window. */
static const unsigned long backoff_ms[4] = { 0, 1000, 2000, 5000 };

/* --- units ------------------------------------------------------------ */

#define K_SYSINIT 0
#define K_SERVICE 1
#define K_RESPAWN 2
#define K_ONCE    3

#define S_STOPPED  0
#define S_STARTING 1
#define S_RUNNING  2
#define S_WAITING  3
#define S_EXITED   4
#define S_FAILED   5
#define S_DISABLED 6

static const char *const state_name[] = {
    "stopped", "starting", "running", "waiting", "exited", "failed",
    "disabled"
};

#define R_NEVER      0
#define R_ON_FAILURE 1
#define R_ALWAYS     2

struct unit {
    char name[NAMESZ];
    char exec[PATHSZ];
    char args[ARGSZ];
    char out[PATHSZ];         /* stdout redirection, "" = console */
    char after[DEPSZ];        /* ordering dependencies, services only */
    char requires[DEPSZ];     /* hard runtime dependencies */
    char ready[PATHSZ];       /* optional readiness marker */
    int  kind;
    int  respawn;             /* R_* policy; nonzero remains compat true */
    int  enabled;
    int  state;
    int  pid;
    int  restarts;
    int  exit_code;
    int  fails;               /* deaths inside the current window */
    int  win_open;            /* a failure window is being counted */
    int  backoff;             /* index into backoff_ms */
    int  stopping;            /* an operator stop is in flight */
    int  restart_pending;     /* start again as soon as it is reaped */
    int  dependency_failed;   /* stopped because a required unit went away */
    int  start_guard;         /* recursive dependency-start cycle guard */
    int  seq;                 /* start order, for reverse shutdown */
    int  order;               /* resolved dependency order (services) */
    unsigned long win_start;  /* uptime ms the failure window opened */
    unsigned long next_start; /* uptime ms a backoff restart is due */
    unsigned long start_ms;   /* uptime ms this instance was spawned */
    unsigned long start_timeout_ms;
};

static struct unit units[MAX_UNITS];
static int nunits;
static int seq_counter;
static int state_dirty;
static int have_run;          /* /run exists and is writable */

/* --- small helpers ---------------------------------------------------- */

static void logf(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void logf(int level, const char *fmt, ...)
{
    char msg[112];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    syscall(SYS_LOG, level, (long)msg, 0, 0);
}

/* snprintf that always fits and always terminates. */
static void setstr(char *dst, unsigned long size, const char *src)
{
    snprintf(dst, size, "%s", src);
}

/* Append to a NUL-terminated buffer, never overflowing it. */
static void appendf(char *buf, unsigned long size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void appendf(char *buf, unsigned long size, const char *fmt, ...)
{
    unsigned long used = strlen(buf);
    va_list ap;

    if (used + 1 >= size)
        return;
    va_start(ap, fmt);
    vsnprintf(buf + used, size - used, fmt, ap);
    va_end(ap);
}

static int read_file(const char *path, char *buf, int max)
{
    long n;
    int fd, total = 0;

    buf[0] = '\0';
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    while (total < max - 1) {
        n = read(fd, buf + total, (unsigned long)(max - 1 - total));
        if (n <= 0)
            break;
        total += (int)n;
    }
    close(fd);
    buf[total] = '\0';
    return total;
}

static int write_file(const char *path, const char *data)
{
    long n;
    int fd;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    n = write(fd, data, strlen(data));
    close(fd);
    return n < 0 ? -1 : 0;
}

static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t' || *s == '\r')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                     e[-1] == '\n'))
        *--e = '\0';
    return s;
}

/* A '#' starts a comment at the start of a line or after whitespace, so
 * that a value like args=--tag=#1 survives. */
static void strip_comment(char *s)
{
    int i;

    for (i = 0; s[i]; i++) {
        if (s[i] == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
            s[i] = '\0';
            return;
        }
    }
}

/* Consume one line from *cur, NUL-terminating it in place. */
static char *next_line(char **cur)
{
    char *s = *cur, *nl;

    if (s == 0 || *s == '\0')
        return 0;
    nl = strchr(s, '\n');
    if (nl) {
        *nl = '\0';
        *cur = nl + 1;
    } else {
        *cur = s + strlen(s);
    }
    return s;
}

/* Split on runs of spaces/tabs, in place. Stops after `max` tokens. */
static int split_ws(char *s, char **out, int max)
{
    int n = 0;

    while (*s && n < max) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s)
            break;
        out[n++] = s;
        while (*s && *s != ' ' && *s != '\t')
            s++;
        if (*s)
            *s++ = '\0';
    }
    return n;
}

static const char *base_of(const char *p)
{
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

static int parse_bool(const char *v, int dflt)
{
    if (!strcmp(v, "yes") || !strcmp(v, "1") || !strcmp(v, "true") ||
        !strcmp(v, "on"))
        return 1;
    if (!strcmp(v, "no") || !strcmp(v, "0") || !strcmp(v, "false") ||
        !strcmp(v, "off"))
        return 0;
    return dflt;
}

static int is_bool(const char *v)
{
    return parse_bool(v, 0) == parse_bool(v, 1);
}

static int parse_restart(const char *v)
{
    if (!strcmp(v, "always"))
        return R_ALWAYS;
    if (!strcmp(v, "on-failure"))
        return R_ON_FAILURE;
    if (!strcmp(v, "never") || !strcmp(v, "no"))
        return R_NEVER;
    return -1;
}

static unsigned long parse_timeout(const char *v, unsigned long dflt,
                                   const char *name)
{
    const char *p;
    unsigned long n;

    if (!v[0])
        return dflt;
    for (p = v; *p; p++) {
        if (*p < '0' || *p > '9') {
            logf(K_LOG_WARN, "%s.svc: timeout_ms=%s is not numeric", name, v);
            return dflt;
        }
    }
    if (strlen(v) > 5) {
        logf(K_LOG_WARN, "%s.svc: timeout_ms=%s outside 100..60000", name, v);
        return dflt;
    }
    n = (unsigned long)atol(v);
    if (n < 100 || n > 60000) {
        logf(K_LOG_WARN, "%s.svc: timeout_ms=%s outside 100..60000", name, v);
        return dflt;
    }
    return n;
}

static struct unit *find_unit(const char *name)
{
    int i;

    for (i = 0; i < nunits; i++)
        if (!strcmp(units[i].name, name))
            return &units[i];
    return 0;
}

static struct unit *unit_alloc(void)
{
    struct unit *u;

    if (nunits >= MAX_UNITS)
        return 0;
    u = &units[nunits++];
    memset(u, 0, sizeof(*u));
    u->enabled = 1;
    u->order = -1;
    u->start_timeout_ms = START_TIMEOUT_MS;
    return u;
}

/* Give the unit a unique name derived from `want`. */
static void unit_name(struct unit *u, const char *want)
{
    char cand[NAMESZ];
    int n;

    setstr(cand, sizeof(cand), want);
    if (!cand[0])
        setstr(cand, sizeof(cand), "unit");
    for (n = 2; find_unit(cand) && n < 100; n++)
        snprintf(cand, sizeof(cand), "%.*s%d", NAMESZ - 4, want, n);
    setstr(u->name, sizeof(u->name), cand);
}

/* Choose the first existing path from a '|'-separated alternatives list.
 * Returns 0 on success; -1 when none exist (out holds the first one so the
 * caller can report it); -2 when the spec was empty. */
static int pick_exec(const char *spec, char *out, unsigned long size)
{
    char tmp[PATHSZ];
    struct k_stat st;
    const char *p = spec;
    unsigned long i;
    int any = 0;

    setstr(out, size, "");
    while (*p) {
        for (i = 0; *p && *p != '|'; p++)
            if (i + 1 < sizeof(tmp))
                tmp[i++] = *p;
        tmp[i] = '\0';
        if (*p == '|')
            p++;
        if (!tmp[0])
            continue;
        if (!any) {
            setstr(out, size, tmp);
            any = 1;
        }
        if (stat_(tmp, &st) == 0 && !st.is_dir) {
            setstr(out, size, tmp);
            return 0;
        }
    }
    return any ? -1 : -2;
}

/* Join argv[from..argc) with single spaces into dst. */
static void join_args(char *dst, unsigned long size, char **argv, int from,
                      int argc)
{
    int i;

    dst[0] = '\0';
    for (i = from; i < argc; i++)
        appendf(dst, size, "%s%s", i > from ? " " : "", argv[i]);
}

/* --- .svc parsing ----------------------------------------------------- */

/* Fill `u` from /etc/services/<name>.svc. Returns 0, or -1 if the file is
 * unreadable or has no exec= (the unit is then marked failed). */
static int svc_load(struct unit *u, const char *name)
{
    char path[PATHSZ + 32];
    char buf[SVCSZ];
    char stderr_path[PATHSZ];
    char *cur, *line, *eq, *key, *val;

    unit_name(u, name);
    u->kind = K_SERVICE;
    u->respawn = R_ALWAYS;
    u->enabled = 1;
    u->start_timeout_ms = START_TIMEOUT_MS;
    stderr_path[0] = '\0';

    snprintf(path, sizeof(path), "%s/%s.svc", SVC_DIR, name);
    if (read_file(path, buf, sizeof(buf)) < 0) {
        logf(K_LOG_ERR, "service %s: cannot read %s", name, path);
        u->state = S_FAILED;
        return -1;
    }

    cur = buf;
    while ((line = next_line(&cur)) != 0) {
        strip_comment(line);
        line = trim(line);
        if (!*line)
            continue;
        eq = strchr(line, '=');
        if (!eq) {
            logf(K_LOG_WARN, "%s.svc: ignoring line without '=': %.60s",
                 name, line);
            continue;
        }
        *eq = '\0';
        key = trim(line);
        val = trim(eq + 1);

        if (!strcmp(key, "name")) {
            /* Informational: the unit name always follows the filename so
             * that `service <name>` and the .svc path cannot disagree. */
            if (strcmp(val, name) != 0)
                logf(K_LOG_WARN, "%s.svc: name=%s ignored, using %s",
                     name, val, name);
        } else if (!strcmp(key, "exec")) {
            setstr(u->exec, sizeof(u->exec), val);
        } else if (!strcmp(key, "args")) {
            setstr(u->args, sizeof(u->args), val);
        } else if (!strcmp(key, "respawn")) {
            if (!is_bool(val))
                logf(K_LOG_WARN, "%s.svc: respawn=%s is not yes/no", name, val);
            u->respawn = parse_bool(val, 1) ? R_ALWAYS : R_NEVER;
        } else if (!strcmp(key, "restart")) {
            int policy = parse_restart(val);
            if (policy < 0)
                logf(K_LOG_WARN,
                     "%s.svc: restart=%s wants always|on-failure|never",
                     name, val);
            else
                u->respawn = policy;
        } else if (!strcmp(key, "enabled")) {
            if (!is_bool(val))
                logf(K_LOG_WARN, "%s.svc: enabled=%s is not yes/no", name, val);
            u->enabled = parse_bool(val, 1);
        } else if (!strcmp(key, "after")) {
            setstr(u->after, sizeof(u->after), val);
        } else if (!strcmp(key, "requires")) {
            setstr(u->requires, sizeof(u->requires), val);
        } else if (!strcmp(key, "ready")) {
            setstr(u->ready, sizeof(u->ready), val);
        } else if (!strcmp(key, "timeout_ms")) {
            u->start_timeout_ms = parse_timeout(val, START_TIMEOUT_MS, name);
        } else if (!strcmp(key, "stdout")) {
            setstr(u->out, sizeof(u->out), val);
        } else if (!strcmp(key, "stderr")) {
            setstr(stderr_path, sizeof(stderr_path), val);
        } else {
            logf(K_LOG_WARN, "%s.svc: unknown key '%.20s'", name, key);
        }
    }

    if (!u->exec[0]) {
        logf(K_LOG_ERR, "service %s: no exec=, not starting", name);
        u->state = S_FAILED;
        return -1;
    }
    if (u->ready[0] && u->ready[0] != '/') {
        logf(K_LOG_ERR, "service %s: ready= must be an absolute path", name);
        u->state = S_FAILED;
        return -1;
    }
    /* SYS_SPAWN_IO redirects fd 0 and fd 1 only; fd 2 always stays on the
     * console, so a distinct stderr= cannot be honoured. */
    if (stderr_path[0] && strcmp(stderr_path, u->out) != 0)
        logf(K_LOG_WARN, "service %s: stderr=%s unsupported, using console",
             name, stderr_path);
    if (!u->enabled)
        u->state = S_DISABLED;
    return 0;
}

/* --- inittab parsing -------------------------------------------------- */

/* Add a path-based unit (sysinit / respawn / once). */
static void add_path_unit(int kind, char **argv, int argc, const char *what)
{
    struct unit *u;
    char exec[PATHSZ];
    int rc;

    if (argc < 1) {
        logf(K_LOG_WARN, "inittab: %s needs a path", what);
        return;
    }
    u = unit_alloc();
    if (!u) {
        logf(K_LOG_WARN, "inittab: too many units, ignoring %s %.40s",
             what, argv[0]);
        return;
    }
    rc = pick_exec(argv[0], exec, sizeof(exec));
    setstr(u->exec, sizeof(u->exec), exec);
    unit_name(u, base_of(exec));
    u->kind = kind;
    u->respawn = (kind == K_RESPAWN) ? R_ALWAYS : R_NEVER;
    join_args(u->args, sizeof(u->args), argv, 1, argc);
    if (rc != 0) {
        logf(K_LOG_ERR, "inittab: %s %.40s: no such program", what, argv[0]);
        u->state = S_FAILED;
    }
}

/* Parse one already-trimmed, comment-stripped directive line. */
static void inittab_line(char *line)
{
    char *tok[MAX_SVCARGS + 4];
    struct k_stat st;
    struct unit *u;
    int n, at = 0;

    n = split_ws(line, tok, MAX_SVCARGS + 4);
    if (n <= 0)
        return;

    /* Optional condition prefix: if-exists/if-missing <path> <directive> */
    if (!strcmp(tok[0], "if-exists") || !strcmp(tok[0], "if-missing")) {
        int want = (tok[0][3] == 'e');
        if (n < 3) {
            logf(K_LOG_WARN, "inittab: %s needs a path and a directive",
                 tok[0]);
            return;
        }
        if ((stat_(tok[1], &st) == 0) != want)
            return;                          /* condition false: skip */
        at = 2;
    }

    if (!strcmp(tok[at], "sysinit")) {
        add_path_unit(K_SYSINIT, tok + at + 1, n - at - 1, "sysinit");
    } else if (!strcmp(tok[at], "respawn")) {
        add_path_unit(K_RESPAWN, tok + at + 1, n - at - 1, "respawn");
    } else if (!strcmp(tok[at], "once")) {
        add_path_unit(K_ONCE, tok + at + 1, n - at - 1, "once");
    } else if (!strcmp(tok[at], "service")) {
        if (n - at < 2) {
            logf(K_LOG_WARN, "inittab: service needs a name");
            return;
        }
        if (find_unit(tok[at + 1])) {
            logf(K_LOG_WARN, "inittab: service %s listed twice", tok[at + 1]);
            return;
        }
        u = unit_alloc();
        if (!u) {
            logf(K_LOG_WARN, "inittab: too many units, ignoring service %s",
                 tok[at + 1]);
            return;
        }
        svc_load(u, tok[at + 1]);
    } else {
        logf(K_LOG_WARN, "inittab: unknown directive '%.20s'", tok[at]);
    }
}

/* Returns the number of directives parsed, or -1 if the file is missing. */
static int inittab_parse(const char *path)
{
    char buf[TABSZ];
    char *cur, *line;
    int seen = 0;

    if (read_file(path, buf, sizeof(buf)) < 0)
        return -1;
    cur = buf;
    while ((line = next_line(&cur)) != 0) {
        strip_comment(line);
        line = trim(line);
        if (!*line)
            continue;
        seen++;
        inittab_line(line);
    }
    return seen;
}

/* Last-resort configuration so the machine is never unbootable. */
static void inittab_default(void)
{
    char *tok[1];
    char spec[PATHSZ];

    setstr(spec, sizeof(spec), CONSOLE_DEFAULT);
    tok[0] = spec;
    add_path_unit(K_RESPAWN, tok, 1, "respawn");
}

/* --- dependency ordering ---------------------------------------------- */

/* Commas and whitespace are both accepted between dependency names. */
static int split_deps(const char *spec, char *scratch, char **out)
{
    int i;

    setstr(scratch, DEPSZ, spec);
    for (i = 0; scratch[i]; i++)
        if (scratch[i] == ',')
            scratch[i] = ' ';
    return split_ws(scratch, out, MAX_DEPS);
}

static int deps_placed(struct unit *u, const char *spec, int required)
{
    char scratch[DEPSZ];
    char *dep[MAX_DEPS];
    struct unit *d;
    int n, i;

    n = split_deps(spec, scratch, dep);
    for (i = 0; i < n; i++) {
        d = find_unit(dep[i]);
        if (d == u || !d || d->kind != K_SERVICE) {
            if (required) {
                logf(K_LOG_ERR, "service %s: requires=%s is not a service",
                     u->name, dep[i]);
                u->state = S_FAILED;
            } else {
                logf(K_LOG_WARN, "service %s: after=%s ignored",
                     u->name, dep[i]);
            }
            continue;
        }
        if (d->order < 0)
            return 0;
    }
    return 1;
}

/* Order services so every name in after= and requires= is placed first.
 * Missing after= targets are ordering hints and are ignored; missing hard
 * requirements fail the unit. Anything left unplaced is a real cycle. */
static void order_services(void)
{
    struct unit *u;
    int placed = 0, changed, i;

    for (i = 0; i < nunits; i++)
        if (units[i].kind == K_SERVICE)
            units[i].order = -1;

    do {
        changed = 0;
        for (i = 0; i < nunits; i++) {
            u = &units[i];
            if (u->kind != K_SERVICE || u->order >= 0)
                continue;
            if (!deps_placed(u, u->after, 0) ||
                !deps_placed(u, u->requires, 1))
                continue;
            u->order = placed++;
            changed = 1;
        }
    } while (changed);

    for (i = 0; i < nunits; i++) {
        u = &units[i];
        if (u->kind == K_SERVICE && u->order < 0) {
            logf(K_LOG_ERR,
                 "service %s: dependency cycle (after=%s requires=%s)",
                 u->name, u->after[0] ? u->after : "-",
                 u->requires[0] ? u->requires : "-");
            u->state = S_FAILED;
            u->order = placed++;
        }
    }
}

/* --- state file ------------------------------------------------------- */

static void state_write(void)
{
    char buf[STATESZ];
    long n;
    int fd;
    int i;

    if (!have_run)
        return;
    state_dirty = 1;
    /*
     * Publish the snapshot with one fixed-size in-place write.  Opening with
     * O_TRUNC made publication two filesystem transactions (truncate, then
     * write), so `service status` could observe an empty or prefix-only unit
     * table between them.  KFS journals one write syscall atomically and
     * serializes readers against it; zero-filling the fixed-size file also
     * removes any tail left by a previously longer snapshot.
     */
    memset(buf, 0, sizeof(buf));
    setstr(buf, sizeof(buf),
           "# kestrel init state: name state pid restarts exit\n");
    for (i = 0; i < nunits; i++)
        appendf(buf, sizeof(buf), "%s %s %d %d %d\n", units[i].name,
                state_name[units[i].state], units[i].pid,
                units[i].restarts, units[i].exit_code);

    fd = open(STATE_PATH, O_WRONLY | O_CREAT);
    if (fd < 0)
        return;
    n = write(fd, buf, sizeof(buf));
    close(fd);
    if (n == (long)sizeof(buf))
        state_dirty = 0;
}

/* --- starting and reaping --------------------------------------------- */

static int pid_alive(int pid);
static void unit_start(struct unit *u);

static int requirement_satisfied(const struct unit *u)
{
    if (u->state == S_RUNNING)
        return 1;
    return u->state == S_EXITED && u->exit_code == 0 &&
           u->respawn == R_NEVER;
}

static int ensure_requirements(struct unit *u)
{
    char scratch[DEPSZ];
    char *dep[MAX_DEPS];
    struct unit *d;
    int n, i;

    if (u->kind != K_SERVICE || !u->requires[0])
        return 0;
    n = split_deps(u->requires, scratch, dep);
    for (i = 0; i < n; i++) {
        d = find_unit(dep[i]);
        if (!d || d == u || d->kind != K_SERVICE ||
            d->state == S_FAILED) {
            logf(K_LOG_ERR, "%s: required service %s is unavailable",
                 u->name, dep[i]);
            return -1;
        }
        /* enabled=no controls automatic boot, not whether another unit may
         * explicitly pull this requirement in. */
        if (d->state == S_DISABLED)
            d->enabled = 1;
        if (!requirement_satisfied(d))
            unit_start(d);
        if (!requirement_satisfied(d)) {
            logf(K_LOG_ERR, "%s: required service %s did not become ready",
                 u->name, dep[i]);
            return -1;
        }
    }
    return 0;
}

static void unit_spawn(struct unit *u)
{
    char scratch[ARGSZ];
    char *av[MAX_SVCARGS];
    char *argv[MAX_SVCARGS + 2];
    struct k_stat st;
    int n, i, pid;

    state_dirty = 1;
    u->next_start = 0;
    u->stopping = 0;
    u->restart_pending = 0;
    u->dependency_failed = 0;

    if (!u->enabled) {
        u->state = S_DISABLED;
        return;
    }
    if (!u->exec[0] || stat_(u->exec, &st) < 0 || st.is_dir) {
        logf(K_LOG_ERR, "%s: %.60s is not an executable", u->name, u->exec);
        u->state = S_FAILED;
        return;
    }

    setstr(scratch, sizeof(scratch), u->args);
    n = split_ws(scratch, av, MAX_SVCARGS);
    argv[0] = u->exec;
    for (i = 0; i < n; i++)
        argv[i + 1] = av[i];
    argv[n + 1] = 0;

    if (u->ready[0])
        unlink_(u->ready);

    if (u->out[0])
        pid = spawn_io(u->exec, argv, 0, u->out, 1);
    else
        pid = spawn(u->exec, argv);

    if (pid < 0) {
        logf(K_LOG_ERR, "%s: spawn %.50s failed", u->name, u->exec);
        u->state = S_FAILED;
        return;
    }
    u->pid = pid;
    u->state = u->ready[0] ? S_STARTING : S_RUNNING;
    u->start_ms = uptime_ms();
    if (!u->seq)
        u->seq = ++seq_counter;
    logf(K_LOG_INFO, "started %s (pid %d)", u->name, pid);

    if (u->ready[0]) {
        unsigned long deadline = uptime_ms() + u->start_timeout_ms;

        state_write();
        while (uptime_ms() < deadline) {
            if (stat_(u->ready, &st) == 0) {
                u->state = S_RUNNING;
                state_dirty = 1;
                logf(K_LOG_INFO, "%s ready (%s)", u->name, u->ready);
                return;
            }
            if (!pid_alive(pid)) {
                u->exit_code = waitpid(pid);
                u->pid = 0;
                u->state = S_FAILED;
                state_dirty = 1;
                logf(K_LOG_ERR, "%s exited before readiness (code %d)",
                     u->name, u->exit_code);
                return;
            }
            sleep_ms(50);
        }
        logf(K_LOG_ERR, "%s readiness timed out after %lu ms",
             u->name, u->start_timeout_ms);
        syscall(SYS_KILL, pid, SIGTERM, 0, 0);
        for (i = 0; i < 20 && pid_alive(pid); i++)
            sleep_ms(10);
        u->exit_code = waitpid(pid);
        u->pid = 0;
        u->state = S_FAILED;
        state_dirty = 1;
    }
}

static void unit_start(struct unit *u)
{
    if (u->state == S_RUNNING || u->state == S_STARTING)
        return;
    if (u->start_guard) {
        logf(K_LOG_ERR, "%s: recursive requirement cycle", u->name);
        u->state = S_FAILED;
        state_dirty = 1;
        return;
    }
    u->start_guard = 1;
    if (ensure_requirements(u) < 0) {
        u->state = S_FAILED;
        state_dirty = 1;
    } else {
        unit_spawn(u);
    }
    u->start_guard = 0;
}

/* Run a sysinit unit to completion. */
static void unit_run_sync(struct unit *u)
{
    int code;

    unit_start(u);
    if (u->state != S_RUNNING)
        return;
    code = waitpid(u->pid);
    u->exit_code = code;
    u->pid = 0;
    u->state = (code == 0) ? S_EXITED : S_FAILED;
    state_dirty = 1;
    if (code != 0)
        logf(K_LOG_WARN, "sysinit %s exited with %d", u->name, code);
}

static int pid_alive(int pid)
{
    struct k_psinfo pi;
    int i;

    for (i = 0; psinfo(i, &pi) == 0; i++)
        if (pi.pid == pid)
            return pi.state != K_STATE_ZOMBIE;
    return 0;
}

static void unit_died(struct unit *u, int code)
{
    unsigned long now = uptime_ms();

    u->pid = 0;
    u->exit_code = code;
    state_dirty = 1;
    if (u->ready[0])
        unlink_(u->ready);
    logf(K_LOG_INFO, "%s exited with %d", u->name, code);

    if (u->restart_pending) {
        u->restart_pending = 0;
        u->stopping = 0;
        /* The old instance is gone. unit_start() deliberately refuses a
         * running unit, so publish the transition before dispatching the
         * replacement rather than leaving the stale pre-kill state in
         * place. */
        u->state = S_STOPPED;
        u->fails = 0;
        u->backoff = 0;
        u->win_start = 0;
        u->win_open = 0;
        unit_start(u);
        return;
    }
    if (u->stopping) {
        u->stopping = 0;
        u->state = u->dependency_failed ? S_FAILED :
                   (u->enabled ? S_STOPPED : S_DISABLED);
        u->dependency_failed = 0;
        return;
    }
    if (u->respawn == R_NEVER ||
        (u->respawn == R_ON_FAILURE && code == 0) || !u->enabled) {
        u->state = u->enabled ? S_EXITED : S_DISABLED;
        return;
    }

    /* Leaving the console with ctrl-D must not look like a crash loop: a
     * clean exit from an instance that stayed up counts as a normal
     * restart. Anything that fails, or dies immediately, is a crash. */
    if (code == 0 && now - u->start_ms >= SETTLED_MS) {
        u->restarts++;
        u->next_start = now;
        u->state = S_WAITING;
        return;
    }

    if (!u->win_open || now - u->win_start > FAIL_WINDOW_MS) {
        u->win_open = 1;
        u->win_start = now;
        u->fails = 1;
        u->backoff = 0;
    } else {
        u->fails++;
    }

    if (u->fails >= FAIL_MAX) {
        u->state = S_FAILED;
        logf(K_LOG_ERR,
             "%s died %d times in %lu s: marking it failed, no more restarts",
             u->name, u->fails, FAIL_WINDOW_MS / 1000);
        return;
    }

    u->restarts++;
    u->next_start = now + backoff_ms[u->backoff];
    if (u->backoff < (int)(sizeof(backoff_ms) / sizeof(backoff_ms[0])) - 1)
        u->backoff++;
    u->state = S_WAITING;
}

/* Collect every supervised child that is no longer running. */
static void reap_children(void)
{
    struct unit *u;
    int i;

    for (i = 0; i < nunits; i++) {
        u = &units[i];
        if ((u->state != S_RUNNING && u->state != S_STARTING &&
             !u->stopping) || u->pid <= 0)
            continue;
        if (pid_alive(u->pid))
            continue;
        unit_died(u, waitpid(u->pid));
    }
}

static void run_backoff(void)
{
    unsigned long now = uptime_ms();
    int i;

    for (i = 0; i < nunits; i++)
        if (units[i].state == S_WAITING && now >= units[i].next_start)
            unit_start(&units[i]);
}

/* --- control protocol ------------------------------------------------- */

static void unit_stop(struct unit *u)
{
    u->stopping = 1;
    if (u->pid > 0)
        syscall(SYS_KILL, u->pid, SIGTERM, 0, 0);
}

static int reload_service(struct unit *u)
{
    struct unit fresh;
    struct unit old;
    int saved_state[MAX_UNITS];
    int saved_order[MAX_UNITS];
    int runtime_enabled = u->enabled;
    int i;

    if (u->kind != K_SERVICE)
        return -1;
    memset(&fresh, 0, sizeof(fresh));
    fresh.enabled = 1;
    fresh.order = -1;
    fresh.start_timeout_ms = START_TIMEOUT_MS;
    if (svc_load(&fresh, u->name) < 0)
        return -1;

    memcpy(&old, u, sizeof(old));
    for (i = 0; i < nunits; i++) {
        saved_state[i] = units[i].state;
        saved_order[i] = units[i].order;
    }
    setstr(u->exec, sizeof(u->exec), fresh.exec);
    setstr(u->args, sizeof(u->args), fresh.args);
    setstr(u->out, sizeof(u->out), fresh.out);
    setstr(u->after, sizeof(u->after), fresh.after);
    setstr(u->requires, sizeof(u->requires), fresh.requires);
    setstr(u->ready, sizeof(u->ready), fresh.ready);
    u->respawn = fresh.respawn;
    /* enabled= is a boot policy. A live `service reload` must not undo an
     * operator's earlier `service start` of a boot-disabled unit. */
    u->enabled = runtime_enabled;
    u->start_timeout_ms = fresh.start_timeout_ms;
    if (u->state == S_FAILED)
        u->state = u->enabled ? S_STOPPED : S_DISABLED;
    order_services();
    if (u->state != S_FAILED)
        return 0;

    /* Dependency validation is part of reload's transaction: a bad edit
     * must not replace the last working config or fail neighbouring units
     * that were only pulled into its temporary cycle. */
    memcpy(u, &old, sizeof(old));
    for (i = 0; i < nunits; i++) {
        units[i].state = saved_state[i];
        units[i].order = saved_order[i];
    }
    return -1;
}

static int unit_requires(const struct unit *u, const char *name)
{
    char scratch[DEPSZ];
    char *dep[MAX_DEPS];
    int n = split_deps(u->requires, scratch, dep);

    for (int i = 0; i < n; i++)
        if (!strcmp(dep[i], name))
            return 1;
    return 0;
}

/* requires= is a lifecycle relationship, not just startup ordering. Stop
 * dependents when a required service is no longer healthy. */
static void enforce_dependencies(void)
{
    int i, j;

    for (i = 0; i < nunits; i++) {
        struct unit *u = &units[i];
        if (u->state != S_RUNNING && u->state != S_STARTING &&
            u->state != S_WAITING)
            continue;
        for (j = 0; j < nunits; j++) {
            struct unit *d = &units[j];
            if (!unit_requires(u, d->name) || requirement_satisfied(d))
                continue;
            logf(K_LOG_ERR, "%s stopped: required service %s is not ready",
                 u->name, d->name);
            u->dependency_failed = 1;
            u->next_start = 0;
            if (u->pid > 0) {
                unit_stop(u);
                /*
                 * Dependency loss is already a failed service transition;
                 * do not keep publishing "running" until SIGTERM happens
                 * to be reaped on a later supervisor tick.  The stopping
                 * flag keeps the child in reap_children() while its PID is
                 * still live.
                 */
                u->state = S_FAILED;
                state_dirty = 1;
            } else {
                u->state = S_FAILED;
                u->dependency_failed = 0;
                state_dirty = 1;
            }
            break;
        }
    }
}

static void run_command(char *verb, char *name, char *ack, unsigned long size)
{
    struct unit *u = find_unit(name);

    if (!u) {
        snprintf(ack, size, "err no such service: %s\n", name);
        return;
    }
    logf(K_LOG_INFO, "control: %s %s", verb, name);

    if (!strcmp(verb, "start")) {
        if (u->state == S_RUNNING || u->state == S_STARTING) {
            snprintf(ack, size, "ok %s already running (pid %d)\n", name,
                     u->pid);
            return;
        }
        u->enabled = 1;
        u->restart_pending = 0;
        u->dependency_failed = 0;
        u->fails = 0;
        u->backoff = 0;
        u->win_start = 0;
        u->win_open = 0;
        unit_start(u);
        snprintf(ack, size, "%s start %s (%s)\n",
                 u->state == S_RUNNING ? "ok" : "err", name,
                 state_name[u->state]);
    } else if (!strcmp(verb, "stop")) {
        u->restart_pending = 0;
        u->dependency_failed = 0;
        if ((u->state != S_RUNNING && u->state != S_STARTING) || u->pid <= 0) {
            u->state = S_STOPPED;
            u->next_start = 0;
            state_dirty = 1;
            snprintf(ack, size, "ok stop %s (was not running)\n", name);
            return;
        }
        unit_stop(u);
        snprintf(ack, size, "ok stop %s (pid %d)\n", name, u->pid);
    } else if (!strcmp(verb, "restart")) {
        u->enabled = 1;
        if ((u->state == S_RUNNING || u->state == S_STARTING) && u->pid > 0) {
            u->restart_pending = 1;      /* checked before stopping in reap */
            unit_stop(u);
            snprintf(ack, size, "ok restart %s (pid %d)\n", name, u->pid);
            return;
        }
        u->fails = 0;
        u->backoff = 0;
        u->win_start = 0;
        u->win_open = 0;
        unit_start(u);
        snprintf(ack, size, "%s restart %s (%s)\n",
                 u->state == S_RUNNING ? "ok" : "err", name,
                 state_name[u->state]);
    } else if (!strcmp(verb, "reload")) {
        if (reload_service(u) < 0) {
            snprintf(ack, size, "err reload %s (invalid configuration)\n",
                     name);
            return;
        }
        if (!u->enabled) {
            if ((u->state == S_RUNNING || u->state == S_STARTING) &&
                u->pid > 0)
                unit_stop(u);
            else
                u->state = S_DISABLED;
            snprintf(ack, size, "ok reload %s (disabled)\n", name);
            return;
        }
        if ((u->state == S_RUNNING || u->state == S_STARTING) && u->pid > 0) {
            u->restart_pending = 1;
            unit_stop(u);
            snprintf(ack, size, "ok reload %s (restarting pid %d)\n",
                     name, u->pid);
        } else
            snprintf(ack, size, "ok reload %s (%s)\n", name,
                     state_name[u->state]);
    } else if (!strcmp(verb, "reset-failed")) {
        u->fails = 0;
        u->backoff = 0;
        u->win_open = 0;
        u->next_start = 0;
        if (u->state == S_FAILED)
            u->state = u->enabled ? S_STOPPED : S_DISABLED;
        state_dirty = 1;
        snprintf(ack, size, "ok reset-failed %s (%s)\n", name,
                 state_name[u->state]);
    } else {
        snprintf(ack, size, "err unknown command: %.20s\n", verb);
    }
}

/* /run/init.cmd holds one newline-terminated "<verb> <name>" line. The
 * newline is the commit marker, so a half-written file is ignored until
 * the writer finishes. */
static void poll_command(void)
{
    char buf[128];
    char ack[128];
    char *tok[3];
    int n;

    if (!have_run)
        return;
    n = read_file(CMD_PATH, buf, sizeof(buf));
    if (n < 0)
        return;
    if (n == 0 || !strchr(buf, '\n'))
        return;                          /* still being written */

    *strchr(buf, '\n') = '\0';
    strip_comment(buf);
    n = split_ws(trim(buf), tok, 3);
    ack[0] = '\0';
    if (n < 2)
        setstr(ack, sizeof(ack),
               "err usage: <start|stop|restart|reload|reset-failed> <name>\n");
    else
        run_command(tok[0], tok[1], ack, sizeof(ack));

    /* Apply lifecycle effects of a stop/reload before committing the
     * command acknowledgement and its state snapshot. */
    enforce_dependencies();
    state_dirty = 1;
    /*
     * An acknowledgement commits the control operation to the client.
     * Publish its resulting state first so an immediate `service status`
     * cannot race behind a successful start/stop/reload reply.
     */
    state_write();
    unlink_(CMD_PATH);
    write_file(ACK_PATH, ack);
}

/* --- shutdown --------------------------------------------------------- */

static int any_running(void)
{
    int i;

    for (i = 0; i < nunits; i++)
        if ((units[i].state == S_RUNNING || units[i].state == S_STARTING) &&
            units[i].pid > 0)
            return 1;
    return 0;
}

/* Stop everything in reverse start order, then power down. Never returns
 * unless the kernel refuses the power operation. */
static void do_shutdown(int action)
{
    unsigned long deadline;
    struct unit *u;
    int i, best;

    logf(K_LOG_INFO, "shutdown requested (%s)",
         action == K_POWER_HALT ? "halt" : "reboot");

    for (;;) {
        best = -1;
        for (i = 0; i < nunits; i++) {
            u = &units[i];
            if ((u->state != S_RUNNING && u->state != S_STARTING) ||
                u->pid <= 0 || u->stopping)
                continue;
            if (best < 0 || u->seq > units[best].seq)
                best = i;
        }
        if (best < 0)
            break;
        units[best].respawn = 0;
        logf(K_LOG_INFO, "stopping %s (pid %d)", units[best].name,
             units[best].pid);
        unit_stop(&units[best]);
    }

    deadline = uptime_ms() + STOP_GRACE_MS;
    while (uptime_ms() < deadline && any_running()) {
        reap_children();
        sleep_ms(50);
    }
    reap_children();
    if (any_running())
        logf(K_LOG_WARN, "shutdown: some services did not stop in time");

    state_write();
    unlink_(SHUTDOWN_PATH);
    unlink_(CMD_PATH);
    logf(K_LOG_INFO, "system going down now");
    syscall(SYS_POWER, action, 0, 0, 0);

    logf(K_LOG_ERR, "SYS_POWER failed; halting the supervisor");
    for (;;)
        sleep_ms(1000);
}

/* /run/shutdown holds one newline-terminated word: reboot | halt |
 * poweroff. */
static void poll_shutdown(void)
{
    char buf[64];
    char *tok[2];
    int n;

    if (!have_run)
        return;
    n = read_file(SHUTDOWN_PATH, buf, sizeof(buf));
    if (n < 0)
        return;
    if (n == 0 || !strchr(buf, '\n'))
        return;                          /* still being written */

    *strchr(buf, '\n') = '\0';
    n = split_ws(trim(buf), tok, 2);
    if (n < 1) {
        unlink_(SHUTDOWN_PATH);
        return;
    }
    if (!strcmp(tok[0], "reboot"))
        do_shutdown(K_POWER_REBOOT);
    else if (!strcmp(tok[0], "halt") || !strcmp(tok[0], "poweroff"))
        do_shutdown(K_POWER_HALT);
    else {
        logf(K_LOG_WARN, "shutdown: ignoring request '%.20s'", tok[0]);
        unlink_(SHUTDOWN_PATH);
    }
}

/* --- startup ---------------------------------------------------------- */

static void run_setup(void)
{
    struct k_stat st;

    if (stat_(RUN_DIR, &st) < 0)
        mkdir_(RUN_DIR);
    have_run = (stat_(RUN_DIR, &st) == 0 && st.is_dir);
    if (!have_run) {
        logf(K_LOG_WARN, "cannot create %s: service control is disabled",
             RUN_DIR);
        return;
    }
    /* /run is on the real filesystem, so clear anything a previous boot
     * left behind - a stale shutdown request would power the machine off
     * again the moment it came up. */
    unlink_(SHUTDOWN_PATH);
    unlink_(CMD_PATH);
    unlink_(ACK_PATH);
}

static void start_all(void)
{
    int i, o;

    for (i = 0; i < nunits; i++)
        if (units[i].kind == K_SYSINIT && units[i].state != S_FAILED)
            unit_run_sync(&units[i]);

    order_services();
    for (o = 0; o < nunits; o++)
        for (i = 0; i < nunits; i++)
            if (units[i].kind == K_SERVICE && units[i].order == o &&
                units[i].state != S_FAILED)
                unit_start(&units[i]);

    for (i = 0; i < nunits; i++)
        if ((units[i].kind == K_RESPAWN || units[i].kind == K_ONCE) &&
            units[i].state != S_FAILED)
            unit_start(&units[i]);
}

int main(int argc, char **argv)
{
    int n;

    (void)argc;
    (void)argv;

    logf(K_LOG_INFO, "init starting (pid %d)", getpid());
    run_setup();

    n = inittab_parse(INITTAB_PATH);
    if (n < 0) {
        logf(K_LOG_WARN, "%s is missing; falling back to a console only",
             INITTAB_PATH);
        inittab_default();
    } else if (nunits == 0) {
        logf(K_LOG_WARN, "%s configured nothing; adding a console",
             INITTAB_PATH);
        inittab_default();
    }

    start_all();
    state_write();

    for (;;) {
        reap_children();
        enforce_dependencies();
        run_backoff();
        poll_command();
        poll_shutdown();
        if (state_dirty)
            state_write();
        sleep_ms(POLL_MS);
    }
}
