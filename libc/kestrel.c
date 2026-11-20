/* KestrelOS libc: thin syscall wrappers and OS helpers.
 *
 * All wrappers forward straight to syscall() (libc/syscall.asm),
 * which issues int 0x80 per the convention in abi/kestrel_abi.h.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>
#include <mission.h>

int mission_spawn(const char *path, char *const argv[], const struct k_mission_grant *grant)
{
    return (int)syscall(SYS_MISSION_SPAWN, (long)path, (long)argv, (long)grant, 0);
}

int mission_spawn_id(const char *path, char *const argv[], const struct k_mission_grant *grant,
                     uint32_t *id)
{
    return (int)syscall(SYS_MISSION_SPAWN, (long)path, (long)argv, (long)grant, (long)id);
}

int mission_ctl(uint32_t id, unsigned op, unsigned arg)
{
    return (int)syscall(SYS_MISSION_CTL, id, op, arg, 0);
}

int mission_info(unsigned index, struct k_mission_info *out)
{
    return (int)syscall(SYS_MISSION_INFO, index, (long)out, 0, 0);
}

/* ---- files ---- */

int open(const char *path, int flags)
{
    return (int)syscall(SYS_OPEN, (long)path, flags, 0, 0);
}

long read(int fd, void *buf, unsigned long len)
{
    return syscall(SYS_READ, fd, (long)buf, (long)len, 0);
}

long write(int fd, const void *buf, unsigned long len)
{
    return syscall(SYS_WRITE, fd, (long)buf, (long)len, 0);
}

int close(int fd)
{
    return (int)syscall(SYS_CLOSE, fd, 0, 0, 0);
}

long seek(int fd, long off, int whence)
{
    return syscall(SYS_SEEK, fd, off, whence, 0);
}

long read_nb(int fd, void *buf, unsigned long len)
{
    return syscall(SYS_READ_NB, fd, (long)buf, (long)len, 0);
}

int readdir_at(const char *path, int index, struct k_dirent *out)
{
    return (int)syscall(SYS_READDIR, (long)path, index, (long)out, 0);
}

int unlink_(const char *path)
{
    return (int)syscall(SYS_UNLINK, (long)path, 0, 0, 0);
}

int mkdir_(const char *path)
{
    return (int)syscall(SYS_MKDIR, (long)path, 0, 0, 0);
}

int stat_(const char *path, struct k_stat *out)
{
    return (int)syscall(SYS_STAT, (long)path, (long)out, 0, 0);
}

/* ---- processes ---- */

int spawn(const char *path, char *const argv[])
{
    return (int)syscall(SYS_SPAWN, (long)path, (long)argv, 0, 0);
}

int spawn_io(const char *path, char *const argv[], const char *in_path,
             const char *out_path, int append)
{
    struct k_spawn_io io;

    memset(&io, 0, sizeof(io));
    if (in_path) {
        if (strlen(in_path) >= sizeof(io.in_path))
            return -1;
        strncpy(io.in_path, in_path, sizeof(io.in_path) - 1);
    }
    if (out_path) {
        if (strlen(out_path) >= sizeof(io.out_path))
            return -1;
        strncpy(io.out_path, out_path, sizeof(io.out_path) - 1);
    }
    io.out_append = append ? 1 : 0;
    return (int)syscall(SYS_SPAWN_IO, (long)path, (long)argv, (long)&io, 0);
}

int spawn_caps(const char *path, char *const argv[],
               const struct k_spawn_caps *caps)
{
    return (int)syscall(SYS_SPAWN_CAPS, (long)path, (long)argv, (long)caps, 0);
}

int pipe_(int fds[2])
{
    return (int)syscall(SYS_PIPE, (long)fds, 0, 0, 0);
}

int dup2_(int oldfd, int newfd)
{
    return (int)syscall(SYS_DUP2, oldfd, newfd, 0, 0);
}

int rename_(const char *oldpath, const char *newpath)
{
    return (int)syscall(SYS_RENAME, (long)oldpath, (long)newpath, 0, 0);
}

int sandbox(uint64_t drop_caps, const char *unveil_prefix)
{
    return (int)syscall(SYS_SANDBOX, (long)drop_caps, (long)unveil_prefix, 0, 0);
}

int waitpid(int pid)
{
    return (int)syscall(SYS_WAITPID, pid, 0, 0, 0);
}

int waitany(int *pid_out)
{
    return (int)syscall(SYS_WAITANY, (long)pid_out, 0, 0, 0);
}

int getpid(void)
{
    return (int)syscall(SYS_GETPID, 0, 0, 0, 0);
}

uint32_t getuid_(void)
{
    return (uint32_t)syscall(SYS_GETUID, 0, 0, 0, 0);
}

uint32_t getgid_(void)
{
    return (uint32_t)syscall(SYS_GETGID, 0, 0, 0, 0);
}

int setuid_(uint32_t uid)
{
    return (int)syscall(SYS_SETUID, (long)uid, 0, 0, 0);
}

int chmod_(const char *path, uint32_t mode)
{
    return (int)syscall(SYS_CHMOD, (long)path, (long)mode, 0, 0);
}

int chown_(const char *path, uint32_t uid, uint32_t gid)
{
    return (int)syscall(SYS_CHOWN, (long)path, (long)uid, (long)gid, 0);
}

unsigned long time_(void)
{
    return (unsigned long)syscall(SYS_TIME, 0, 0, 0, 0);
}

int pulse_poll(struct k_pulse_query *query, struct k_pulse *out)
{
    return (int)syscall(SYS_PULSE_POLL, (long)query, (long)out, 0, 0);
}

int parcel_put(uint32_t type, const char *mime, const char *label,
               const void *data, uint32_t size)
{
    struct k_parcel_put req;

    memset(&req, 0, sizeof(req));
    req.type = type;
    req.size = size;
    req.data = (uint64_t)(unsigned long)data;
    if (mime)
        strncpy(req.mime, mime, sizeof(req.mime) - 1);
    if (label)
        strncpy(req.label, label, sizeof(req.label) - 1);
    return (int)syscall(SYS_PARCEL_PUT, (long)&req, 0, 0, 0);
}

long parcel_get(uint32_t slot_or_id, struct k_parcel_info *info,
                void *buf, uint32_t bufsz)
{
    return syscall(SYS_PARCEL_GET, (long)slot_or_id, (long)info,
                   (long)buf, (long)bufsz);
}

int parcel_list(int index, struct k_parcel_info *info)
{
    return (int)syscall(SYS_PARCEL_LIST, (long)index, (long)info, 0, 0);
}

int wm_theme(const struct k_wm_theme *theme)
{
    return (int)syscall(SYS_WM_THEME, (long)theme, 0, 0, 0);
}

int win_resize(uint32_t wid, int w, int h)
{
    return (int)syscall(SYS_WIN_RESIZE, (long)wid, (long)w, (long)h, 0);
}

void sleep_ms(unsigned long ms)
{
    syscall(SYS_SLEEP_MS, (long)ms, 0, 0, 0);
}

void yield_(void)
{
    syscall(SYS_YIELD, 0, 0, 0, 0);
}

/* ---- memory ---- */

void *brk_(void *addr)
{
    return (void *)syscall(SYS_BRK, (long)addr, 0, 0, 0);
}

long getrandom(void *buf, unsigned long len, unsigned flags)
{
    return syscall(SYS_GETRANDOM, (long)buf, (long)len, (long)flags, 0);
}

int swapinfo(uint64_t *total_kb, uint64_t *used_kb)
{
    return (int)syscall(SYS_SWAPINFO, (long)total_kb, (long)used_kb, 0, 0);
}

/* ---- signals ---- */

extern void __kestrel_sigreturn(void);

int kill(int pid, int sig)
{
    return (int)syscall(SYS_KILL, pid, sig, 0, 0);
}

int sigaction_(int sig, const struct k_sigaction *act,
               struct k_sigaction *oldact)
{
    struct k_sigaction fixed;
    if (act) {
        fixed = *act;
        if (fixed.handler != SIG_DFL && fixed.handler != SIG_IGN &&
            fixed.restorer == 0)
            fixed.restorer = (uint64_t)__kestrel_sigreturn;
        act = &fixed;
    }
    return (int)syscall(SYS_SIGACTION, sig, (long)act, (long)oldact, 0);
}

int sigprocmask_(int how, const uint64_t *set, uint64_t *oldset)
{
    return (int)syscall(SYS_SIGPROCMASK, how, (long)set, (long)oldset, 0);
}

sighandler_t signal(int sig, sighandler_t handler)
{
    struct k_sigaction action, old;
    memset(&action, 0, sizeof(action));
    action.handler = (uint64_t)handler;
    action.flags = SA_RESTART;
    if (sigaction_(sig, &action, &old) < 0)
        return (sighandler_t)-1;
    return (sighandler_t)old.handler;
}

/* ---- system info ---- */

unsigned long uptime_ms(void)
{
    return (unsigned long)syscall(SYS_UPTIME_MS, 0, 0, 0, 0);
}

int meminfo(uint64_t *total_kb, uint64_t *free_kb)
{
    return (int)syscall(SYS_MEMINFO, (long)total_kb, (long)free_kb, 0, 0);
}

int psinfo(int index, struct k_psinfo *out)
{
    return (int)syscall(SYS_PSINFO, index, (long)out, 0, 0);
}

int cpuinfo(struct k_cpuinfo *out)
{
    return (int)syscall(SYS_CPUINFO, (long)out, 0, 0, 0);
}

/* ---- network ---- */

int dns_resolve(const char *name, uint32_t *ip_be)
{
    return (int)syscall(SYS_DNS, (long)name, (long)ip_be, 0, 0);
}

long ping(uint32_t ip_be, int timeout_ms)
{
    return syscall(SYS_PING, (long)ip_be, timeout_ms, 0, 0);
}

int udp_send(uint32_t ip_be, uint16_t dport, uint16_t sport,
             const void *buf, unsigned long len)
{
    long ports = ((long)dport << 16) | (long)sport;

    return (int)syscall(SYS_UDP_SEND, (long)ip_be, ports,
                        (long)buf, (long)len);
}

long udp_recv(uint16_t port, void *buf, unsigned long maxlen, int timeout_ms)
{
    return syscall(SYS_UDP_RECV, port, (long)buf, (long)maxlen, timeout_ms);
}

int netinfo(struct k_netinfo *out)
{
    return (int)syscall(SYS_NETINFO, (long)out, 0, 0, 0);
}

/* Parse a dotted quad into a big-endian (network order) address.
 * Returns 0 on any parse failure (which makes 0.0.0.0 unrepresentable
 * as a success value; documented as acceptable). */
uint32_t ip_aton(const char *s)
{
    uint32_t ip = 0;
    int part;

    for (part = 0; part < 4; part++) {
        unsigned int v = 0;
        int digits = 0;

        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned int)(*s - '0');
            if (v > 255)
                return 0;
            digits++;
            s++;
        }
        if (digits == 0 || digits > 3)
            return 0;
        /* network order: first octet is the lowest-addressed byte */
        ip |= (uint32_t)v << (8 * part);
        if (part < 3) {
            if (*s != '.')
                return 0;
            s++;
        }
    }
    if (*s != '\0')
        return 0;
    return ip;
}

char *ip_ntoa(uint32_t ip_be, char buf[16])
{
    snprintf(buf, 16, "%u.%u.%u.%u",
             (unsigned int)(ip_be & 0xff),
             (unsigned int)((ip_be >> 8) & 0xff),
             (unsigned int)((ip_be >> 16) & 0xff),
             (unsigned int)((ip_be >> 24) & 0xff));
    return buf;
}

/* ---- line input ----
 * Raw getchar() loop with local echo. Printable chars are stored and
 * echoed; backspace erases with "\b \b"; ctrl-U erases the whole line;
 * Enter terminates the buffer, echoes a newline and returns buf.
 * Ctrl-D on an empty line returns NULL (EOF). KEY_* codes (arrows and
 * friends, including KEY_LEFT/KEY_RIGHT) are ignored. At most n-1
 * bytes are stored. */
char *readline(char *buf, int n)
{
    int len = 0;

    if (buf == 0 || n <= 0)
        return 0;

    for (;;) {
        int c = getchar();

        if (c == EOF) {
            /* read error: behave like EOF */
            if (len == 0)
                return 0;
            buf[len] = '\0';
            return buf;
        }
        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            putchar('\n');
            return buf;
        }
        if (c == 4) {                       /* ctrl-D */
            if (len == 0)
                return 0;
            continue;
        }
        if (c == 8 || c == 127) {           /* backspace */
            if (len > 0) {
                len--;
                putchar('\b');
                putchar(' ');
                putchar('\b');
            }
            continue;
        }
        if (c == 21) {                      /* ctrl-U: erase line */
            while (len > 0) {
                len--;
                putchar('\b');
                putchar(' ');
                putchar('\b');
            }
            continue;
        }
        if (c >= 0x80)                      /* KEY_* codes: ignore */
            continue;
        if (c < 32)                         /* other control chars */
            continue;
        if (len < n - 1) {
            buf[len++] = (char)c;
            putchar(c);
        }
    }
}
