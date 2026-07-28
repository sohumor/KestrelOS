#include "kernel.h"
#include "klog.h"
#include "console.h"
#include "serial.h"
#include "string.h"
#include "proc.h"
#include "kestrel_abi.h"
#include "spinlock.h"

/* Kernel message ring. See klog.h for the contract. */

/* Provided by the RTC/filesystem layer. Declared weak so that a kernel
 * built before that symbol exists still links; the timestamp is simply 0
 * until it is present. */
extern uint32_t rtc_unix_time(void) __attribute__((weak));

static struct k_logent ring[KLOG_RING_ENTRIES];
static uint32_t log_total;          /* entries ever written */
static bool log_ready;
static bool log_mirror;
static bool log_in_sink;            /* re-entrancy guard for the kprintf hook */
static spinlock_t log_lock = SPINLOCK_INIT;

void (*klog_kprintf_sink)(char c);

/* --- tiny freestanding snprintf -------------------------------------- */

struct sbuf {
    char *buf;
    unsigned long size;             /* total capacity including the NUL */
    unsigned long n;                /* characters produced so far */
};

static void sb_putc(struct sbuf *s, char c)
{
    if (s->n + 1 < s->size)
        s->buf[s->n] = c;
    s->n++;
}

static void sb_puts(struct sbuf *s, const char *p)
{
    while (*p)
        sb_putc(s, *p++);
}

static void sb_pad(struct sbuf *s, int count, char pad)
{
    while (count-- > 0)
        sb_putc(s, pad);
}

static void sb_num(struct sbuf *s, uint64_t val, int base, bool negative,
                   int width, char pad, bool upper, bool left)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int i = 0;

    if (val == 0)
        tmp[i++] = '0';
    while (val && i < (int)sizeof(tmp)) {
        tmp[i++] = digits[val % (uint64_t)base];
        val /= (uint64_t)base;
    }
    if (negative && i < (int)sizeof(tmp))
        tmp[i++] = '-';

    if (left) {
        int n = i;
        while (i--)
            sb_putc(s, tmp[i]);
        sb_pad(s, width - n, ' ');
        return;
    }
    sb_pad(s, width - i, pad);
    while (i--)
        sb_putc(s, tmp[i]);
}

int klog_vsnprintf(char *buf, unsigned long size, const char *fmt, va_list ap)
{
    struct sbuf s = { buf, size, 0 };

    if (fmt == NULL)
        fmt = "";

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            sb_putc(&s, *fmt);
            continue;
        }
        fmt++;

        char pad = ' ';
        int width = 0;
        int longs = 0;
        bool left = false;

        for (;;) {
            if (*fmt == '-')
                left = true;
            else if (*fmt == '0')
                pad = '0';
            else
                break;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        while (*fmt == 'l') {
            longs++;
            fmt++;
        }
        if (*fmt == 'z') {
            longs = 2;
            fmt++;
        }

        switch (*fmt) {
        case 'c':
            sb_putc(&s, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *p = va_arg(ap, const char *);
            if (p == NULL)
                p = "(null)";
            int len = (int)strlen(p);
            if (!left)
                sb_pad(&s, width - len, ' ');
            sb_puts(&s, p);
            if (left)
                sb_pad(&s, width - len, ' ');
            break;
        }
        case 'd':
        case 'i': {
            int64_t v = longs ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int32_t);
            bool neg = v < 0;
            sb_num(&s, neg ? (uint64_t)(-v) : (uint64_t)v, 10, neg, width, pad,
                   false, left);
            break;
        }
        case 'u': {
            uint64_t v = longs ? va_arg(ap, uint64_t)
                               : (uint64_t)va_arg(ap, uint32_t);
            sb_num(&s, v, 10, false, width, pad, false, left);
            break;
        }
        case 'x': {
            uint64_t v = longs ? va_arg(ap, uint64_t)
                               : (uint64_t)va_arg(ap, uint32_t);
            sb_num(&s, v, 16, false, width, pad, false, left);
            break;
        }
        case 'X': {
            uint64_t v = longs ? va_arg(ap, uint64_t)
                               : (uint64_t)va_arg(ap, uint32_t);
            sb_num(&s, v, 16, false, width, pad, true, left);
            break;
        }
        case 'p':
            sb_puts(&s, "0x");
            sb_num(&s, (uint64_t)va_arg(ap, void *), 16, false, 16, '0',
                   false, false);
            break;
        case '%':
            sb_putc(&s, '%');
            break;
        case '\0':
            fmt--;
            break;
        default:
            sb_putc(&s, '%');
            sb_putc(&s, *fmt);
        }
    }

    if (size > 0) {
        unsigned long end = s.n < size - 1 ? s.n : size - 1;
        buf[end] = '\0';
        return (int)end;
    }
    return 0;
}

int klog_snprintf(char *buf, unsigned long size, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = klog_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

/* --- the ring --------------------------------------------------------- */

const char *klog_level_name(int level)
{
    switch (level) {
    case K_LOG_DEBUG: return "debug";
    case K_LOG_INFO:  return "info";
    case K_LOG_WARN:  return "warn";
    case K_LOG_ERR:   return "error";
    default:          return "?";
    }
}

void klog_init(void)
{
    uint64_t f = spin_lock_irqsave(&log_lock);
    memset(ring, 0, sizeof(ring));
    log_total = 0;
    log_mirror = false;
    log_ready = true;
    spin_unlock_irqrestore(&log_lock, f);
    klog_write(K_LOG_INFO, "klog", "kernel log ring online");
}

void klog_set_console(bool on)
{
    log_mirror = on;
}

uint32_t klog_count(void)
{
    return __atomic_load_n(&log_total, __ATOMIC_RELAXED);
}

uint32_t klog_retained(void)
{
    uint32_t t = __atomic_load_n(&log_total, __ATOMIC_RELAXED);
    return t < KLOG_RING_ENTRIES ? t : (uint32_t)KLOG_RING_ENTRIES;
}

uint32_t klog_oldest_seq(void)
{
    uint32_t t = __atomic_load_n(&log_total, __ATOMIC_RELAXED);
    return t > KLOG_RING_ENTRIES ? t - KLOG_RING_ENTRIES : 0;
}

/* Copy at most max-1 bytes and always NUL-terminate. */
static void copy_field(char *dst, unsigned long max, const char *src)
{
    unsigned long i = 0;

    if (src != NULL)
        for (; i + 1 < max && src[i]; i++)
            dst[i] = src[i];
    dst[i] = '\0';
}

/* Mirrors an entry straight to the devices rather than through kprintf,
 * so that the kprintf sink can never recurse back into the ring. */
static void mirror_entry(const struct k_logent *e)
{
    char line[sizeof(e->tag) + sizeof(e->msg) + 32];
    klog_format_entry(e, line, sizeof(line));
    for (const char *p = line; *p; p++) {
        console_putc(*p);
        serial_putc(*p);
    }
}

void klog_write(int level, const char *tag, const char *msg)
{
    struct k_logent snapshot;
    bool mirror;

    if (level < K_LOG_DEBUG || level > K_LOG_ERR)
        level = K_LOG_INFO;

    uint64_t f = spin_lock_irqsave(&log_lock);

    struct k_logent *e = &ring[log_total % KLOG_RING_ENTRIES];
    memset(e, 0, sizeof(*e));
    e->seq = log_total;
    e->time = rtc_unix_time ? rtc_unix_time() : 0;
    e->level = (uint32_t)level;
    e->pid = (sched_active && current) ? (uint32_t)current->pid : 0;
    copy_field(e->tag, sizeof(e->tag), tag);
    copy_field(e->msg, sizeof(e->msg), msg);

    log_total++;
    mirror = log_mirror && !log_in_sink;
    if (mirror)
        snapshot = *e;

    spin_unlock_irqrestore(&log_lock, f);

    if (mirror)
        mirror_entry(&snapshot);
}

void klog_printf(int level, const char *tag, const char *fmt, ...)
{
    char msg[112];
    va_list ap;

    va_start(ap, fmt);
    klog_vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    klog_write(level, tag, msg);
}

int klog_read(uint32_t index, struct k_logent *out)
{
    if (out == NULL)
        return -1;

    uint64_t f = spin_lock_irqsave(&log_lock);
    uint32_t retained = klog_retained();
    if (index >= retained) {
        spin_unlock_irqrestore(&log_lock, f);
        return -1;
    }
    uint32_t seq = klog_oldest_seq() + index;
    *out = ring[seq % KLOG_RING_ENTRIES];
    spin_unlock_irqrestore(&log_lock, f);

    /* The slot may have been recycled between the bounds check and the
     * copy on another CPU; on this uniprocessor kernel interrupts are off
     * for the whole window, so a mismatch means the caller raced a wrap. */
    return out->seq == seq ? 0 : -1;
}

int klog_format_entry(const struct k_logent *e, char *buf, unsigned long size)
{
    if (e == NULL)
        return klog_snprintf(buf, size, "\n");
    return klog_snprintf(buf, size, "[%10u] %-5s %-8s (%u) %s\n",
                         e->time, klog_level_name((int)e->level),
                         e->tag[0] ? e->tag : "-", e->pid, e->msg);
}

/* --- kprintf mirroring ------------------------------------------------
 * Assembles the character stream kprintf emits into whole lines and files
 * each one as an entry. ANSI escape sequences from the console driver are
 * stripped so the ring holds plain text. */

static char sink_line[112];
static unsigned int sink_len;
static int sink_esc;                /* 0 = text, 1 = saw ESC, 2 = in CSI */

static void sink_flush(void)
{
    if (sink_len == 0)
        return;
    sink_line[sink_len] = '\0';
    sink_len = 0;
    log_in_sink = true;
    klog_write(K_LOG_INFO, "kernel", sink_line);
    log_in_sink = false;
}

static void klog_sink_char(char c)
{
    if (!log_ready)
        return;

    if (sink_esc == 1) {
        sink_esc = (c == '[') ? 2 : 0;
        return;
    }
    if (sink_esc == 2) {
        if ((c >= '@' && c <= '~'))
            sink_esc = 0;
        return;
    }
    if (c == 0x1B) {
        sink_esc = 1;
        return;
    }

    if (c == '\n') {
        sink_flush();
        return;
    }
    if (c == '\r' || c == '\0')
        return;
    if (c == '\b') {
        if (sink_len > 0)
            sink_len--;
        return;
    }
    if (sink_len + 1 >= sizeof(sink_line)) {
        sink_flush();               /* truncate rather than overflow */
        return;
    }
    sink_line[sink_len++] = c;
}

void klog_hook_kprintf(void)
{
    sink_len = 0;
    sink_esc = 0;
    klog_kprintf_sink = klog_sink_char;
}

void klog_unhook_kprintf(void)
{
    klog_kprintf_sink = NULL;
    sink_len = 0;
    sink_esc = 0;
}
