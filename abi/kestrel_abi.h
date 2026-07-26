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
#define SYS_POWER     26  /* (0=reboot, 1=halt) */
#define SYS_SPAWN_IO  27  /* (path, argv, struct k_spawn_io *) -> pid */

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

struct k_stat {
    uint32_t size;
    uint32_t is_dir;
};

struct k_dirent {
    char name[60];
    uint32_t size;
    uint32_t is_dir;
};

struct k_psinfo {
    int32_t pid;
    char name[32];
    int32_t state;
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

/* Userspace virtual memory layout */
#define USER_LOAD_BASE  0x400000ULL
#define USER_STACK_TOP  0x00007FFFFFFFE000ULL
#define USER_STACK_PAGES 16
