/* service.c - control and inspect the units supervised by init.
 *
 *   service list                 name, state, pid, restarts
 *   service status <name>        state plus the .svc configuration
 *   service start|stop|restart|reload|reset-failed <name>
 *   service log <name>           lines mentioning <name> in the log
 *
 * init owns /run/services.state and polls /run/init.cmd; the protocol is
 * described in docs/init.md.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#define RUN_DIR    "/run"
#define STATE_PATH "/run/services.state"
#define CMD_PATH   "/run/init.cmd"
#define ACK_PATH   "/run/init.ack"
#define SVC_DIR    "/etc/services"
#define LOG_PATH   "/var/log/messages"

#define STATESZ 2048
#define SVCSZ   1024
#define LINESZ  256

/* Pull the shell-injected "--cwd=<path>" argument, wherever it sits. */
static int strip_cwd_arg(int argc, char **argv)
{
    int i, out = 1;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) != 0)
            argv[out++] = argv[i];
    }
    return out;
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

/* Load the state file. Returns -1 with a message when init has not
 * published one yet. */
static int state_load(char *buf, int max)
{
    if (read_file(STATE_PATH, buf, max) < 0) {
        printf("service: %s is missing (init may not be running)\n",
               STATE_PATH);
        return -1;
    }
    return 0;
}

static int cmd_list(void)
{
    char buf[STATESZ];
    char *cur, *line, *f[5];
    int n, rows = 0;

    if (state_load(buf, sizeof(buf)) < 0)
        return 1;

    printf("%-12s %-9s %6s %8s %5s\n", "NAME", "STATE", "PID", "RESTARTS",
           "EXIT");
    cur = buf;
    while ((line = next_line(&cur)) != 0) {
        line = trim(line);
        if (!*line || line[0] == '#')
            continue;
        n = split_ws(line, f, 5);
        if (n < 5)
            continue;
        printf("%-12s %-9s %6s %8s %5s\n", f[0], f[1], f[2], f[3], f[4]);
        rows++;
    }
    if (rows == 0)
        printf("(no units)\n");
    return 0;
}

/* Copy the state line for `name` into f[]; returns 0 when found. */
static int state_find(char *buf, const char *name, char **f)
{
    char *cur = buf, *line;

    while ((line = next_line(&cur)) != 0) {
        line = trim(line);
        if (!*line || line[0] == '#')
            continue;
        if (split_ws(line, f, 5) < 5)
            continue;
        if (!strcmp(f[0], name))
            return 0;
    }
    return -1;
}

static int cmd_status(const char *name)
{
    char buf[STATESZ];
    char svc[SVCSZ];
    char path[128];
    char *f[5], *cur, *line;
    int have_svc;

    if (state_load(buf, sizeof(buf)) < 0)
        return 1;
    if (state_find(buf, name, f) < 0) {
        printf("service: %s is not a configured unit\n", name);
        return 1;
    }

    printf("%s\n", f[0]);
    printf("  state    %s\n", f[1]);
    printf("  pid      %s\n", f[2]);
    printf("  restarts %s\n", f[3]);
    printf("  exit     %s\n", f[4]);

    snprintf(path, sizeof(path), "%s/%s.svc", SVC_DIR, name);
    have_svc = read_file(path, svc, sizeof(svc)) >= 0;
    if (!have_svc) {
        printf("  source   /etc/inittab (no %s)\n", path);
        return 0;
    }
    printf("  source   %s\n", path);
    cur = svc;
    while ((line = next_line(&cur)) != 0) {
        line = trim(line);
        if (!*line || line[0] == '#')
            continue;
        printf("  %s\n", line);
    }
    return 0;
}

static int cmd_log(const char *name)
{
    char chunk[512];
    char line[LINESZ];
    long n;
    int fd, len = 0, i, hits = 0;

    fd = open(LOG_PATH, O_RDONLY);
    if (fd < 0) {
        printf("service: cannot open %s (is the logger service running?)\n",
               LOG_PATH);
        return 1;
    }
    while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
        for (i = 0; i < (int)n; i++) {
            if (chunk[i] != '\n' && len < LINESZ - 1) {
                line[len++] = chunk[i];
                continue;
            }
            line[len] = '\0';
            if (len && strstr(line, name)) {
                printf("%s\n", line);
                hits++;
            }
            len = 0;
        }
    }
    close(fd);
    if (len) {
        line[len] = '\0';
        if (strstr(line, name)) {
            printf("%s\n", line);
            hits++;
        }
    }
    if (hits == 0)
        printf("service: no log lines mention %s\n", name);
    return 0;
}

/* Hand a command to init and wait for its acknowledgement. */
static int send_cmd(const char *verb, const char *name)
{
    char line[128];
    char ack[160];
    struct k_stat st;
    long n;
    int fd, i;

    ack[0] = '\0';
    if (stat_(RUN_DIR, &st) < 0 || !st.is_dir) {
        printf("service: %s is missing; init is not accepting commands\n",
               RUN_DIR);
        return 1;
    }
    unlink_(ACK_PATH);

    /* Let a command already in flight finish, then overwrite regardless so
     * a stale file from a killed client cannot wedge the interface. */
    for (i = 0; i < 20 && stat_(CMD_PATH, &st) == 0; i++)
        sleep_ms(100);

    snprintf(line, sizeof(line), "%s %s\n", verb, name);
    fd = open(CMD_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        printf("service: cannot write %s\n", CMD_PATH);
        return 1;
    }
    n = write(fd, line, strlen(line));
    close(fd);
    if (n < 0) {
        printf("service: cannot write %s\n", CMD_PATH);
        return 1;
    }

    for (i = 0; i < 50; i++) {           /* up to 5 s */
        if (stat_(CMD_PATH, &st) < 0)
            break;
        sleep_ms(100);
    }
    if (i == 50) {
        printf("service: init did not pick up the request\n");
        return 1;
    }
    for (i = 0; i < 10; i++) {           /* the ack lands just after */
        if (read_file(ACK_PATH, ack, sizeof(ack)) > 0)
            break;
        sleep_ms(50);
    }
    if (ack[0] == '\0') {
        printf("service: no reply from init\n");
        return 1;
    }
    printf("%s", ack);
    if (ack[strlen(ack) - 1] != '\n')
        printf("\n");
    return strncmp(ack, "err", 3) == 0 ? 1 : 0;
}

static void usage(void)
{
    printf("usage: service list\n");
    printf("       service status <name>\n");
    printf("       service start|stop|restart|reload|reset-failed <name>\n");
    printf("       service log <name>\n");
}

int main(int argc, char **argv)
{
    const char *verb;

    argc = strip_cwd_arg(argc, argv);
    if (argc < 2) {
        usage();
        return 1;
    }
    verb = argv[1];

    if (!strcmp(verb, "list"))
        return cmd_list();
    if (argc < 3) {
        usage();
        return 1;
    }
    if (!strcmp(verb, "status"))
        return cmd_status(argv[2]);
    if (!strcmp(verb, "log"))
        return cmd_log(argv[2]);
    if (!strcmp(verb, "start") || !strcmp(verb, "stop") ||
        !strcmp(verb, "restart") || !strcmp(verb, "reload") ||
        !strcmp(verb, "reset-failed"))
        return send_cmd(verb, argv[2]);

    printf("service: unknown command '%s'\n", verb);
    usage();
    return 1;
}
