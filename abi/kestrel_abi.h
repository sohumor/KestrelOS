#pragma once

/* KestrelOS user<->kernel ABI. Shared by the kernel and libc.
 *
 * Syscall convention: int 0x80
 *   rax = syscall number, args in rdi, rsi, rdx, r10
 *   return value in rax (negative = error, usually -1)
 */

#include <stdint.h>

#define SYS_EXIT      0   /* (code) */
#define SYS_WRITE     1   /* (fd, buf, len) -> written; fd 1/2 = console */
#define SYS_READ      2   /* (fd, buf, len) -> read; fd 0 = console, blocks */
#define SYS_OPEN      3   /* (path, flags) -> fd */
#define SYS_CLOSE     4   /* (fd) */
#define SYS_SPAWN     5   /* (path, argv) -> pid */
#define SYS_WAITPID   6   /* (pid) -> exit code, blocks */
#define SYS_GETPID    7   /* () -> pid */
#define SYS_SLEEP_MS  8   /* (ms) */
#define SYS_YIELD     9   /* () */
#define SYS_READDIR   10  /* (path, index, struct k_dirent*) -> 0 / -1 end */
#define SYS_UNLINK    11  /* (path) */
#define SYS_MKDIR     12  /* (path) */
#define SYS_STAT      13  /* (path, struct k_stat*) */
#define SYS_BRK       14  /* (addr; 0=query) -> current break */
#define SYS_UPTIME_MS 15  /* () -> ms since boot */
#define SYS_MEMINFO   16  /* (u64* total_kb, u64* free_kb) */
#define SYS_PSINFO    17  /* (index, struct k_psinfo*) -> 0 / -1 end */
#define SYS_DNS       18  /* (name, u32* ip_be) */
#define SYS_PING      19  /* (ip_be, timeout_ms) -> rtt ms / -1 */
#define SYS_UDP_SEND  20  /* (ip_be, dport<<16|sport, buf, len) */
#define SYS_UDP_RECV  21  /* (port, buf, maxlen, timeout_ms) -> len / -1 */
#define SYS_NETINFO   22  /* (struct k_netinfo*) -> 0 / -1 if no nic */
#define SYS_SEEK      23  /* (fd, off, whence 0|1|2) -> new pos */
#define SYS_READ_NB   24  /* (fd, buf, len) -> read, 0 if none ready */
#define SYS_RTC       25  /* (struct k_rtc*) -> 0/-1 */
#define SYS_POWER     26  /* (0 = reboot, 1 = halt) */
#define SYS_SPAWN_IO  27  /* (path, argv, struct k_spawn_io*) -> pid */

/* --- descriptors and processes --- */
#define SYS_PIPE      28  /* (int fd[2]) -> 0/-1; fd[0] read, fd[1] write */
#define SYS_DUP2      29  /* (oldfd, newfd) -> newfd/-1 */
#define SYS_KILL      30  /* (pid, sig) -> 0/-1; sig 0 probes permission */
#define SYS_WAITANY   31  /* (int *pid_out) -> exit code, blocks */
#define SYS_EXEC      32  /* (path, argv) -> does not return on success */

/* --- users and permissions --- */
#define SYS_GETUID    33  /* () -> uid */
#define SYS_GETGID    34  /* () -> gid */
#define SYS_SETUID    35  /* (uid) -> 0/-1; root only */
#define SYS_CHMOD     36  /* (path, mode) -> 0/-1 */
#define SYS_CHOWN     37  /* (path, uid, gid) -> 0/-1 */

/* --- time and logging --- */
#define SYS_TIME      38  /* () -> seconds since the Unix epoch */
#define SYS_LOG       39  /* (level, msg) -> 0; writes the kernel log ring */
#define SYS_LOGREAD   40  /* (index, struct k_logent*) -> 0/-1 at end */

/* --- graphics, input, windows --- */
#define SYS_FBINFO    41  /* (struct k_fbinfo*) -> 0/-1 */
#define SYS_MOUSE     42  /* (struct k_mouse*) -> 0/-1 */
#define SYS_WIN_CREATE  43 /* (struct k_wincreate*, struct k_wininfo*) -> 0/-1 */
#define SYS_WIN_DESTROY 44 /* (wid) -> 0/-1 */
#define SYS_WIN_FLUSH   45 /* (wid) -> 0/-1; publish the window's pixels */
#define SYS_WIN_EVENT   46 /* (wid, struct k_event*, timeout_ms) -> 1/0/-1 */
#define SYS_WIN_MOVE    47 /* (wid, x, y) -> 0/-1 */

/* --- TCP --- */
#define SYS_TCP_CONNECT 48 /* (ip_be, port, timeout_ms) -> handle/-1 */
#define SYS_TCP_SEND    49 /* (handle, buf, len) -> sent/-1 */
#define SYS_TCP_RECV    50 /* (handle, buf, max, timeout_ms) -> n, 0 = closed */
#define SYS_TCP_CLOSE   51 /* (handle) -> 0/-1 */
#define SYS_SYNC        52 /* () -> flush filesystem caches */

/* --- loadable kernel modules --- */
#define SYS_INSMOD      53 /* (path) -> 0/-1; root only */
#define SYS_RMMOD       54 /* (name) -> 0/-1; root only */
#define SYS_MODLIST     55 /* (index, struct k_modinfo*) -> 0 / -1 at end */

/* --- mounts, block devices, device tree --- */
#define SYS_MOUNT       56 /* (path, fstype, devname) -> 0/-1; root only */
#define SYS_UMOUNT      57 /* (path) -> 0/-1; root only */
#define SYS_BLKLIST     58 /* (index, struct k_blkinfo*) -> 0 / -1 at end */
#define SYS_DEVLIST     59 /* (index, struct k_devinfo*) -> 0 / -1 at end */
#define SYS_MOUNTLIST   60 /* (index, struct k_mountinfo*) -> 0 / -1 at end */
#define SYS_GETRANDOM   61 /* (buf, len, flags) -> bytes/-1 */
#define SYS_SWAPINFO    62 /* (u64* total_kb, u64* used_kb) -> 0/-1 */
#define SYS_SIGACTION   63 /* (sig, act, oldact) -> 0/-1 */
#define SYS_SIGPROCMASK 64 /* (how, set, oldset) -> 0/-1 */
#define SYS_SIGRETURN   65 /* internal signal restorer; no ordinary return */
#define SYS_CPUINFO     66 /* (struct k_cpuinfo*) -> 0/-1 */

/* SYS_GETRANDOM flags. Both devices use the same ChaCha20 CSPRNG after its
 * initial seed threshold; GRND_RANDOM requests the blocking random policy. */
#define GRND_NONBLOCK 0x01
#define GRND_RANDOM   0x02

/* Signals. Masks use bit (signal_number - 1). Kestrel supports the
 * traditional 1..31 range; pending signals coalesce. */
#define K_NSIG   32
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGXCPU  24
#define SIGXFSZ  25
#define SIGVTALRM 26
#define SIGPROF  27
#define SIGWINCH 28
#define SIGIO    29
#define SIGSYS   31

#define SIG_DFL 0ULL
#define SIG_IGN 1ULL

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SA_NODEFER   0x01ULL
#define SA_RESETHAND 0x02ULL
#define SA_RESTART   0x04ULL /* accepted; blocking calls are not restarted yet */

struct k_sigaction {
    uint64_t handler;
    uint64_t mask;
    uint64_t flags;
    uint64_t restorer;
};

/* open() flags */
#define O_RDONLY 0x000
#define O_WRONLY 0x001
#define O_RDWR   0x002
#define O_CREAT  0x040
#define O_TRUNC  0x200
#define O_APPEND 0x400

/* task states reported by SYS_PSINFO */
#define K_STATE_RUNNABLE 0
#define K_STATE_RUNNING  1
#define K_STATE_SLEEPING 2
#define K_STATE_ZOMBIE   3
#define K_STATE_STOPPED  4

/* File mode bits (permission bits only; there is no setuid bit). */
#define K_IRUSR 0400
#define K_IWUSR 0200
#define K_IXUSR 0100
#define K_IRGRP 0040
#define K_IWGRP 0020
#define K_IXGRP 0010
#define K_IROTH 0004
#define K_IWOTH 0002
#define K_IXOTH 0001

struct k_stat {
    uint32_t size;
    uint32_t is_dir;
    uint32_t mode;         /* permission bits, e.g. 0755 */
    uint32_t uid;
    uint32_t gid;
    uint32_t mtime;        /* seconds since the Unix epoch, 0 if unknown */
};

struct k_dirent {
    char name[60];
    uint32_t size;
    uint32_t is_dir;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t mtime;
};

struct k_psinfo {
    int32_t pid;
    char name[32];
    int32_t state;
    uint32_t uid;
    int32_t ppid;
};

struct k_cpuinfo {
    uint32_t online;
    uint32_t discovered;
    uint32_t current_cpu;
    uint32_t apic_id;
};

/* SYS_SPAWN_IO: where the child's stdin (fd 0) and stdout (fd 1) go.
 * An empty path leaves that fd on the console; fd 2 always stays on the
 * console. Each path must be NUL-terminated within its field. */
struct k_spawn_io {
    char in_path[128];    /* "" = inherit console */
    char out_path[128];   /* "" = inherit console */
    uint32_t out_append;  /* 0 = truncate, 1 = append */
    uint32_t _pad;
};

/* SYS_POWER actions */
#define K_POWER_REBOOT 0
#define K_POWER_HALT   1

/* Wall-clock time from the CMOS RTC. wday is 0 = Sunday .. 6 = Saturday,
 * computed from the date rather than read from the chip. */
struct k_rtc {
    uint16_t year;
    uint8_t mon, day, hour, min, sec, wday, pad;
};

struct k_netinfo {
    uint32_t ip;      /* all addresses big-endian (network order) */
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    uint8_t mac[6];
    uint8_t up;       /* 1 if the NIC was found and initialized */
    uint8_t _pad;
};

/* Special key codes delivered through SYS_READ on fd 0 (values >= 0x80).
 * Regular keys arrive as ASCII (ctrl-X as 1..26, ESC as 27). */
#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_LEFT   0x82
#define KEY_RIGHT  0x83
#define KEY_HOME   0x84
#define KEY_END    0x85
#define KEY_PGUP   0x86
#define KEY_PGDN   0x87
#define KEY_DELETE 0x88

/* --- kernel log ---------------------------------------------------- */

#define K_LOG_DEBUG 0
#define K_LOG_INFO  1
#define K_LOG_WARN  2
#define K_LOG_ERR   3

struct k_logent {
    uint32_t seq;          /* monotonically increasing */
    uint32_t time;         /* Unix seconds */
    uint32_t level;
    uint32_t pid;
    char tag[16];
    char msg[112];
};

/* --- modules, block devices, mounts, devices ------------------------ */

/* module state reported by SYS_MODLIST */
#define K_MOD_LOADING   0
#define K_MOD_LIVE      1
#define K_MOD_UNLOADING 2

struct k_modinfo {
    char name[32];
    char desc[64];
    uint32_t size;         /* bytes of module memory in use */
    uint32_t refs;
    uint32_t state;        /* K_MOD_* */
};

struct k_blkinfo {
    char name[16];
    uint32_t block_size;
    uint64_t blocks;
};

struct k_mountinfo {
    char path[64];
    char fstype[16];
    char device[16];
    uint64_t blocks;       /* 0 when the filesystem cannot report it */
    uint64_t free_blocks;
    uint32_t block_size;
    uint32_t _pad;
};

struct k_devinfo {
    char bus[16];
    char name[24];
    char driver[24];       /* "" when nothing is bound */
    uint32_t bound;        /* 1 if a driver claimed it */
    uint32_t vendor;       /* PCI vendor id, 0 on other buses */
    uint32_t device;       /* PCI device id, 0 on other buses */
    uint32_t class_id;     /* PCI class << 8 | subclass, else 0 */
};

/* --- framebuffer, mouse, windows ------------------------------------ */

struct k_fbinfo {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;        /* bytes per scanline of the real device */
    uint32_t bpp;
    uint32_t present;      /* 0 = text mode only, no graphics */
    uint32_t _pad;
};

#define K_MOUSE_LEFT   1
#define K_MOUSE_RIGHT  2
#define K_MOUSE_MIDDLE 4

struct k_mouse {
    int32_t x, y;
    uint32_t buttons;      /* bitmask of K_MOUSE_* */
    uint32_t present;
};

/* Window creation. Pixels are 32-bit 0x00RRGGBB, row-major, `width` per
 * row (no padding). The kernel maps the buffer into the caller and keeps
 * it until SYS_WIN_DESTROY or process exit. */
#define K_WIN_NODECOR 1    /* no title bar or border (desktop, panels) */
#define K_WIN_DESKTOP 2    /* full-screen background layer, never focused */

struct k_wincreate {
    int32_t x, y;
    uint32_t width, height;
    uint32_t flags;
    char title[64];
};

struct k_wininfo {
    uint32_t wid;
    uint32_t width, height;   /* client area actually granted */
    uint64_t buffer;          /* user virtual address of the pixel buffer */
};

/* Window event types */
#define KEV_NONE       0
#define KEV_KEY        1   /* key in .key (ASCII or KEY_* code) */
#define KEV_MOUSE_MOVE 2
#define KEV_MOUSE_DOWN 3
#define KEV_MOUSE_UP   4
#define KEV_CLOSE      5   /* the user clicked the close box */
#define KEV_FOCUS      6   /* .key 1 = gained focus, 0 = lost */
#define KEV_RESIZE     7

struct k_event {
    uint32_t type;
    int32_t  x, y;         /* client-relative for mouse events */
    uint32_t key;
    uint32_t buttons;
};

/* Userspace virtual memory layout */
#define USER_LOAD_BASE  0x400000ULL
#define USER_STACK_TOP  0x00007FFFFFFFE000ULL
#define USER_STACK_PAGES 16

/* Window pixel buffers are mapped here, one 16 MiB slot per window, well
 * clear of both the program image and the stack. */
#define USER_WIN_BASE   0x0000200000000000ULL
#define USER_WIN_STRIDE 0x0000000001000000ULL
#define USER_WIN_MAX    16
