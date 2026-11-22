#pragma once

/* KestrelOS libc: system call wrappers and OS-specific helpers.
 * Ground truth for numbers/structs/flags: abi/kestrel_abi.h.
 */

#include <stdint.h>
#include <kestrel_abi.h>
#include <stdio.h>   /* term_* macros emit ANSI via printf */

/* Raw syscall: int 0x80, rax=n, args in rdi rsi rdx r10, ret in rax.
 * Negative return means error (usually -1). */
long syscall(long n, long a, long b, long c, long d);

/* ---- files ---- */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int open(const char *path, int flags);               /* -> fd or -1 */
/* Like open(), but returns K_OPEN_SAME_FILE if path resolves to exclude_fd's file. */
int open_distinct(const char *path, int flags, int exclude_fd);
long read(int fd, void *buf, unsigned long len);     /* blocks; -> bytes or -1 */
long write(int fd, const void *buf, unsigned long len);
int close(int fd);
long seek(int fd, long off, int whence);             /* -> new pos or -1 */
long read_nb(int fd, void *buf, unsigned long len);  /* -> bytes, 0 if none ready */

int readdir_at(const char *path, int index, struct k_dirent *out); /* 0 / -1 end */
int unlink_(const char *path);
int mkdir_(const char *path);
int stat_(const char *path, struct k_stat *out);

/* ---- processes ---- */
int spawn(const char *path, char *const argv[]);     /* -> pid or -1 */
/* spawn with redirected stdio: the child's fd 0 reads in_path, its fd 1
 * writes out_path (created; truncated unless append != 0), fd 2 stays
 * on the console. NULL inherits the console. A path that does not fit
 * struct k_spawn_io's 128-byte fields returns -1. -> pid or -1 */
int spawn_io(const char *path, char *const argv[], const char *in_path,
             const char *out_path, int append);
int spawn_caps(const char *path, char *const argv[],
               const struct k_spawn_caps *caps);
int spawn_grant(const char *path, char *const argv[],
                const struct k_spawn_grant *grant);
int pipe_(int fds[2]);
int dup2_(int oldfd, int newfd);
int rename_(const char *oldpath, const char *newpath);
int sandbox(uint64_t drop_caps, const char *unveil_prefix);
int waitpid(int pid);                                /* blocks; -> exit code */
int waitany(int *pid_out);                           /* blocks; -> exit code */
int getpid(void);
uint32_t getuid_(void);
uint32_t getgid_(void);
int setuid_(uint32_t uid);
int chmod_(const char *path, uint32_t mode);
int chown_(const char *path, uint32_t uid, uint32_t gid);
unsigned long time_(void);
void sleep_ms(unsigned long ms);
void yield_(void);

/* ---- memory ---- */
void *brk_(void *addr);        /* addr==0 queries; -> current break */
long getrandom(void *buf, unsigned long len, unsigned flags);
int swapinfo(uint64_t *total_kb, uint64_t *used_kb);

/* ---- signals ---- */
typedef void (*sighandler_t)(int);
int kill(int pid, int sig);
int sigaction_(int sig, const struct k_sigaction *act,
               struct k_sigaction *oldact);
int sigprocmask_(int how, const uint64_t *set, uint64_t *oldset);
sighandler_t signal(int sig, sighandler_t handler);

/* ---- system info ---- */
unsigned long uptime_ms(void);
int meminfo(uint64_t *total_kb, uint64_t *free_kb);
int psinfo(int index, struct k_psinfo *out);         /* 0 / -1 end */
int cpuinfo(struct k_cpuinfo *out);                  /* 0 / -1 */

/* ---- network (all addresses big-endian / network order) ---- */
int dns_resolve(const char *name, uint32_t *ip_be);  /* 0 / -1 */
long ping(uint32_t ip_be, int timeout_ms);           /* -> rtt ms or -1 */
int udp_send(uint32_t ip_be, uint16_t dport, uint16_t sport,
             const void *buf, unsigned long len);
long udp_recv(uint16_t port, void *buf, unsigned long maxlen,
              int timeout_ms);                       /* -> len or -1 timeout */
int netinfo(struct k_netinfo *out);                  /* 0 / -1 no nic */

/* Parse dotted quad -> big-endian ip, 0 on parse failure. */
uint32_t ip_aton(const char *s);
/* Format big-endian ip into buf (>= 16 bytes), returns buf. */
char *ip_ntoa(uint32_t ip_be, char buf[16]);

/* ---- IPv6: addresses are 16 network-order bytes ---- */
int dns_resolve6(const char *name, uint8_t ip6[16]); /* AAAA or literal */
long ping6(const uint8_t ip6[16], int timeout_ms);   /* -> rtt ms or -1 */
int udp_send6(const uint8_t ip6[16], uint16_t dport, uint16_t sport,
              const void *buf, unsigned long len);
int netinfo6(struct k_netinfo6 *out);                /* 0 / -1 no nic */
int ip6_aton(const char *s, uint8_t out[16]);        /* 0 / -1; "[..]" ok */
char *ip6_ntoa(const uint8_t ip6[16], char buf[46]);

/* Family-agnostic client helpers: IPv4 and IPv6 literals, then DNS A,
 * then AAAA (NET_AF_ANY), or only one family. */
#define NET_AF_ANY   0
#define NET_AF_INET  4
#define NET_AF_INET6 6
struct net_addr {
    int family;           /* NET_AF_INET or NET_AF_INET6 */
    uint32_t ip4;         /* network order */
    uint8_t ip6[16];
};
int net_resolve(const char *host, int family, struct net_addr *out);
int net_tcp_connect(const struct net_addr *a, int port, int timeout_ms);
char *net_addr_str(const struct net_addr *a, char *buf, unsigned long n);

/* ---- line input ----
 * Read a line from fd 0 with echo, backspace and ctrl-U editing.
 * Stores up to n-1 bytes plus NUL (newline not stored). Returns buf,
 * or 0 (NULL) if ctrl-D was pressed on an empty line. */
char *readline(char *buf, int n);

/* ---- Kestrel Pulses, Talon Parcels & Compositor ---- */
int  pulse_poll(struct k_pulse_query *query, struct k_pulse *out);
int  parcel_put(uint32_t type, const char *mime, const char *label,
                const void *data, uint32_t size);
long parcel_get(uint32_t slot_or_id, struct k_parcel_info *info,
                void *buf, uint32_t bufsz);
int  parcel_list(int index, struct k_parcel_info *info);
int  wm_theme(const struct k_wm_theme *theme);
int  win_resize(uint32_t wid, int w, int h);

/* ---- Hangar: park running apps to disk, resume them later ---- */
/* reason (K_HANGAR_REASON_MAX bytes, may be 0) explains a -1. */
int  hangar_park(int pid, char *reason);                 /* -> bay / -1 */
int  hangar_list(int index, struct k_hangar_info *out);  /* 0 / -1 end */
int  hangar_thumb(int bay, uint32_t *pixels);            /* W*H ARGB */
int  hangar_resume(int bay, char *reason);               /* -> pid / -1 */
int  hangar_discard(int bay);

/* ---- terminal helpers (ANSI escapes on fd 1) ---- */
#define TERM_BLACK   30
#define TERM_RED     31
#define TERM_GREEN   32
#define TERM_YELLOW  33
#define TERM_BLUE    34
#define TERM_MAGENTA 35
#define TERM_CYAN    36
#define TERM_WHITE   37

#define term_clear()       printf("\033[2J\033[H")
#define term_goto(r, c)    printf("\033[%d;%dH", (int)(r), (int)(c))
#define term_color(fg)     printf("\033[%dm", (int)(fg))
#define term_reset()       printf("\033[0m")
#define term_hide_cursor() printf("\033[?25l")
#define term_show_cursor() printf("\033[?25h")
