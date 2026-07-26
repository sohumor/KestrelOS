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
int waitpid(int pid);                                /* blocks; -> exit code */
int getpid(void);
void sleep_ms(unsigned long ms);
void yield_(void);

/* ---- memory ---- */
void *brk_(void *addr);        /* addr==0 queries; -> current break */

/* ---- system info ---- */
unsigned long uptime_ms(void);
int meminfo(uint64_t *total_kb, uint64_t *free_kb);
int psinfo(int index, struct k_psinfo *out);         /* 0 / -1 end */

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

/* ---- line input ----
 * Read a line from fd 0 with echo, backspace and ctrl-U editing.
 * Stores up to n-1 bytes plus NUL (newline not stored). Returns buf,
 * or 0 (NULL) if ctrl-D was pressed on an empty line. */
char *readline(char *buf, int n);

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
