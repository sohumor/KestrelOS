/* logger.c - append new kernel-log entries to /var/log/messages.
 *
 * Runs as a supervised service. Every tick it walks the kernel ring with
 * SYS_LOGREAD, appends every entry newer than the last one it wrote, and
 * remembers the highest sequence number in /var/log/.messages.seq so a
 * restart does not duplicate what is already on disk. When the file grows
 * past the cap the oldest half is dropped.
 *
 *   logger [-f <path>] [-i <interval ms>] [-c <cap KiB>]
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DEF_PATH   "/var/log/messages"
#define SEQ_PATH   "/var/log/.messages.seq"
#define DEF_CAP    (64 * 1024)
#define DEF_TICK   500
#define BATCHSZ    4096
#define LINESZ     192
#define COPYSZ     1024

static const char *log_path = DEF_PATH;
static const char *seq_path = SEQ_PATH;
static unsigned long cap_bytes = DEF_CAP;
static unsigned long tick_ms = DEF_TICK;

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

static const char *level_name(unsigned int level)
{
    switch (level) {
    case K_LOG_DEBUG: return "debug";
    case K_LOG_INFO:  return "info";
    case K_LOG_WARN:  return "warn";
    case K_LOG_ERR:   return "error";
    default:          return "?";
    }
}

/* Append `len` bytes to the log file. Returns 0 or -1. */
static int append(const char *data, unsigned long len)
{
    long n;
    int fd;

    if (len == 0)
        return 0;
    fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND);
    if (fd < 0)
        return -1;
    seek(fd, 0, SEEK_END);
    n = write(fd, data, len);
    close(fd);
    return n < 0 ? -1 : 0;
}

static void seq_save(uint32_t seq)
{
    char buf[32];
    int fd;

    snprintf(buf, sizeof(buf), "%u\n", seq);
    fd = open(seq_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return;
    write(fd, buf, strlen(buf));
    close(fd);
}

/* The next sequence number to write; 0 when nothing is on disk yet. */
static uint32_t seq_load(void)
{
    char buf[32];
    long n;
    int fd;

    fd = open(seq_path, O_RDONLY);
    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    return (uint32_t)atol(buf);
}

/* Copy `count` bytes from src at `off` into dst, truncating dst first. */
static int copy_range(const char *src, long off, unsigned long count,
                      const char *dst)
{
    char buf[COPYSZ];
    long n;
    unsigned long left = count;
    int in, out;

    in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        close(in);
        return -1;
    }
    if (off > 0)
        seek(in, off, SEEK_SET);
    while (left > 0) {
        unsigned long want = left < sizeof(buf) ? left : sizeof(buf);
        n = read(in, buf, want);
        if (n <= 0)
            break;
        if (write(out, buf, (unsigned long)n) < 0)
            break;
        left -= (unsigned long)n;
    }
    close(in);
    close(out);
    return left == 0 ? 0 : -1;
}

/* Drop the oldest half of the log once it passes the cap. The tail is
 * staged in a sibling file first so a crash mid-rotation loses at most the
 * half that was already destined for deletion. */
static void rotate(void)
{
    char tmp[128];
    struct k_stat st;
    char probe[COPYSZ];
    long cut;
    long n;
    int fd, i;

    if (stat_(log_path, &st) < 0 || st.size <= cap_bytes)
        return;

    cut = (long)(st.size / 2);
    /* Advance to the start of the next whole line. */
    fd = open(log_path, O_RDONLY);
    if (fd < 0)
        return;
    seek(fd, cut, SEEK_SET);
    n = read(fd, probe, sizeof(probe));
    close(fd);
    for (i = 0; i < (int)n; i++) {
        if (probe[i] == '\n') {
            cut += i + 1;
            break;
        }
    }
    if (cut >= (long)st.size)
        cut = (long)st.size;

    snprintf(tmp, sizeof(tmp), "%s.old", log_path);
    if (copy_range(log_path, cut, st.size - (unsigned long)cut, tmp) < 0) {
        unlink_(tmp);
        logf(K_LOG_WARN, "logger: rotation of %s failed", log_path);
        return;
    }
    if (stat_(tmp, &st) < 0) {
        unlink_(tmp);
        return;
    }
    if (copy_range(tmp, 0, st.size, log_path) < 0)
        logf(K_LOG_WARN, "logger: could not rewrite %s", log_path);
    unlink_(tmp);
    logf(K_LOG_INFO, "logger: %s trimmed to %u bytes", log_path, st.size);
}

/* Write every ring entry from sequence *next onwards, then advance it. */
static void drain(uint32_t *next)
{
    char batch[BATCHSZ];
    char line[LINESZ];
    struct k_logent e;
    unsigned long used = 0, len;
    uint32_t after = *next;
    int i;

    for (i = 0; syscall(SYS_LOGREAD, i, (long)&e, 0, 0) == 0; i++) {
        if (e.seq < *next)
            continue;
        e.tag[sizeof(e.tag) - 1] = '\0';
        e.msg[sizeof(e.msg) - 1] = '\0';
        len = (unsigned long)snprintf(line, sizeof(line),
                                      "[%10u] %-5s %-8s (%u) %s\n",
                                      e.time, level_name(e.level),
                                      e.tag[0] ? e.tag : "-", e.pid, e.msg);
        if (len >= sizeof(line))
            len = sizeof(line) - 1;
        if (used + len >= sizeof(batch)) {
            if (append(batch, used) < 0)
                return;
            used = 0;
        }
        memcpy(batch + used, line, len);
        used += len;
        if (e.seq >= after)
            after = e.seq + 1;
    }

    if (used > 0 && append(batch, used) < 0)
        return;
    if (after != *next) {
        *next = after;
        seq_save(after);
    }
}

static void ensure_dirs(void)
{
    struct k_stat st;
    char dir[128];
    char *slash;

    snprintf(dir, sizeof(dir), "%s", log_path);
    slash = strrchr(dir, '/');
    if (!slash || slash == dir)
        return;
    *slash = '\0';
    /* Create every component: /var then /var/log. */
    for (slash = dir + 1; *slash; slash++) {
        if (*slash != '/')
            continue;
        *slash = '\0';
        if (stat_(dir, &st) < 0)
            mkdir_(dir);
        *slash = '/';
    }
    if (stat_(dir, &st) < 0)
        mkdir_(dir);
}

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

int main(int argc, char **argv)
{
    char seqbuf[128];
    uint32_t next;
    int i, custom = 0;

    argc = strip_cwd_arg(argc, argv);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            log_path = argv[++i];
            custom = 1;
        } else if (!strcmp(argv[i], "-i") && i + 1 < argc) {
            tick_ms = (unsigned long)atol(argv[++i]);
            if (tick_ms < 50)
                tick_ms = 50;
        } else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            cap_bytes = (unsigned long)atol(argv[++i]) * 1024UL;
            if (cap_bytes < 4096)
                cap_bytes = 4096;
        } else {
            printf("usage: logger [-f path] [-i ms] [-c cap-kib]\n");
            return 1;
        }
    }
    if (custom) {
        snprintf(seqbuf, sizeof(seqbuf), "%s.seq", log_path);
        seq_path = seqbuf;
    }

    ensure_dirs();
    next = seq_load();
    logf(K_LOG_INFO, "logger: writing %s from seq %u", log_path, next);

    for (;;) {
        drain(&next);
        rotate();
        sleep_ms(tick_ms);
    }
}
