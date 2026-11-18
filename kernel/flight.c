#include "kernel.h"
#include "flight.h"
#include "proc.h"
#include "smp.h"
#include "timer.h"
#include "spinlock.h"
#include "string.h"

/* Fixed-size BSS ring buffer; NOLOAD so kernel.bin size is unaffected. */
static struct flight_entry flight_ring[FLIGHT_RING_SIZE];
static uint64_t flight_total_events;
static spinlock_t flight_lock = SPINLOCK_INIT;

static void copy_bounded(char *dst, size_t cap, const char *src)
{
    size_t i = 0;

    if (!dst || cap == 0)
        return;
    if (src) {
        for (; i + 1 < cap && src[i]; i++)
            dst[i] = src[i];
    }
    dst[i] = '\0';
}

const char *flight_cat_name(uint8_t cat)
{
    switch (cat) {
    case FLIGHT_CAT_PROC:  return "proc";
    case FLIGHT_CAT_FS:    return "fs";
    case FLIGHT_CAT_VM:    return "vm";
    case FLIGHT_CAT_PULSE: return "pulse";
    case FLIGHT_CAT_TALON: return "talon";
    case FLIGHT_CAT_NET:   return "net";
    default:               return "unknown";
    }
}

void flight_init(void)
{
    uint64_t f = spin_lock_irqsave(&flight_lock);
    memset(flight_ring, 0, sizeof(flight_ring));
    flight_total_events = 0;
    spin_unlock_irqrestore(&flight_lock, f);
}

void flight_record(uint8_t cat, const char *event, int16_t pid,
                   uint64_t arg0, uint64_t arg1, const char *detail)
{
    struct task *cur = current;
    uint64_t tick = timer_ticks();
    uint8_t cpu = (uint8_t)smp_cpu_index();
    int16_t rec_pid = pid;

    if (rec_pid < 0)
        rec_pid = cur ? (int16_t)cur->pid : 0;

    uint64_t f = spin_lock_irqsave(&flight_lock);
    uint64_t seq = flight_total_events++;
    struct flight_entry *e = &flight_ring[seq % FLIGHT_RING_SIZE];

    e->tick = tick;
    e->arg0 = arg0;
    e->arg1 = arg1;
    e->pid = rec_pid;
    e->cpu = cpu;
    e->cat = cat;
    copy_bounded(e->event, sizeof(e->event),
                 (event && event[0]) ? event : "event");
    copy_bounded(e->detail, sizeof(e->detail), detail);
    spin_unlock_irqrestore(&flight_lock, f);
}

uint64_t flight_total(void)
{
    return __atomic_load_n(&flight_total_events, __ATOMIC_RELAXED);
}

/* --- bounded @krec formatter ----------------------------------------- */

struct fbuf {
    char *out;
    uint32_t len;
    uint32_t cap;
    bool overflow;
};

static void fb_putc(struct fbuf *b, char c)
{
    if (b->len + 1 < b->cap) {
        if (b->out)
            b->out[b->len] = c;
        b->len++;
    } else {
        b->overflow = true;
    }
}

static void fb_puts(struct fbuf *b, const char *s)
{
    while (*s)
        fb_putc(b, *s++);
}

static void fb_escape(struct fbuf *b, const char *s)
{
    if (!s || !s[0]) {
        fb_putc(b, '-');
        return;
    }
    for (; *s; s++) {
        unsigned char uc = (unsigned char)*s;
        switch (*s) {
        case '\\': fb_putc(b, '\\'); fb_putc(b, '\\'); break;
        case '\t': fb_putc(b, '\\'); fb_putc(b, 't');  break;
        case '\n': fb_putc(b, '\\'); fb_putc(b, 'n');  break;
        case '\r': fb_putc(b, '\\'); fb_putc(b, 'r');  break;
        default:
            fb_putc(b, (uc < 0x20 || uc >= 0x7f) ? '?' : *s);
            break;
        }
    }
}

static void fb_put_u64(struct fbuf *b, uint64_t v)
{
    char tmp[24];
    int i = 0;

    if (v == 0) {
        fb_putc(b, '0');
        return;
    }
    while (v > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0)
        fb_putc(b, tmp[--i]);
}

static void fb_put_i64(struct fbuf *b, int64_t v)
{
    if (v < 0) {
        fb_putc(b, '-');
        fb_put_u64(b, (uint64_t)(-(v + 1)) + 1ULL);
    } else {
        fb_put_u64(b, (uint64_t)v);
    }
}

static void fb_put_hex(struct fbuf *b, uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    char tmp[16];
    int i = 0;

    fb_puts(b, "0x");
    if (v == 0) {
        fb_putc(b, '0');
        return;
    }
    while (v > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = hex[v & 0xFULL];
        v >>= 4;
    }
    while (i > 0)
        fb_putc(b, tmp[--i]);
}

uint32_t flight_render_krec(char *buf, uint32_t cap)
{
    struct fbuf b = { buf, 0, cap, false };

    if (cap == 0)
        return 0;

    fb_puts(&b, "@krec tick:u64 cpu:int pid:int cat:str event:str arg0:hex detail:str\n");
    if (b.overflow) {
        b.len = 0;
        if (buf)
            buf[0] = '\0';
        return 0;
    }

    uint64_t f = spin_lock_irqsave(&flight_lock);
    uint64_t total = flight_total_events;
    uint64_t count = total > FLIGHT_RING_SIZE ? FLIGHT_RING_SIZE : total;
    uint64_t oldest = total - count;

    for (uint64_t i = 0; i < count; i++) {
        const struct flight_entry *e =
            &flight_ring[(oldest + i) % FLIGHT_RING_SIZE];
        uint32_t mark = b.len;

        fb_put_u64(&b, e->tick);
        fb_putc(&b, '\t');
        fb_put_u64(&b, e->cpu);
        fb_putc(&b, '\t');
        fb_put_i64(&b, e->pid);
        fb_putc(&b, '\t');
        fb_puts(&b, flight_cat_name(e->cat));
        fb_putc(&b, '\t');
        fb_escape(&b, e->event);
        fb_putc(&b, '\t');
        fb_put_hex(&b, e->arg0);
        fb_putc(&b, '\t');
        fb_escape(&b, e->detail);
        fb_putc(&b, '\n');

        if (b.overflow) {
            /* Never leave a truncated final row in the @krec stream. */
            b.len = mark;
            break;
        }
    }
    spin_unlock_irqrestore(&flight_lock, f);

    if (buf)
        buf[b.len] = '\0';
    return b.len;
}
