#include "kernel.h"
#include "devfs.h"
#include "klog.h"
#include "vfs.h"
#include "kheap.h"
#include "string.h"
#include "proc.h"
#include "console.h"
#include "serial.h"
#include "input.h"
#include "timer.h"
#include "kestrel_abi.h"

/* /dev pseudo-filesystem. See devfs.h for the contract. */

extern uint32_t rtc_unix_time(void) __attribute__((weak));

#define DEVFS_MAX_OPEN 32
#define DEVFS_LINE_MAX 192

struct devfile {
    struct file f;              /* MUST stay first: handles are cast to this */
    int dev;
    uint32_t ent;               /* /dev/klog: next log index to render */
    unsigned int lpos;          /* /dev/klog: consumed bytes of `line` */
    unsigned int llen;          /* /dev/klog: valid bytes in `line` */
    char line[DEVFS_LINE_MAX];
};

struct devdesc {
    const char *name;
    int id;
    uint32_t mode;
};

static const struct devdesc devtab[] = {
    { "null",    DEV_NULL,    0666 },
    { "zero",    DEV_ZERO,    0666 },
    { "full",    DEV_FULL,    0666 },
    { "console", DEV_CONSOLE, 0622 },
    { "random",  DEV_RANDOM,  0444 },
    { "klog",    DEV_KLOG,    0644 },
};

#define DEVTAB_COUNT ((int)(sizeof(devtab) / sizeof(devtab[0])))

/* Registry of live handles. Lets devfs_owns() answer without inspecting
 * any field of struct file, whose layout is owned by the VFS. */
static struct devfile *open_handles[DEVFS_MAX_OPEN];

static bool devfs_ready;

/* --- PRNG -------------------------------------------------------------
 * xorshift64* written from scratch: fast, decent equidistribution, and
 * emphatically NOT cryptographic. The state is seeded from the timer tick
 * count and the RTC, both of which an attacker can guess, and the output
 * is trivially invertible from 64 bits of observed stream. /dev/random is
 * here for shuffling and test data, never for keys. */

static uint64_t rng_state;

static void rng_seed(void)
{
    uint64_t s = 0x9E3779B97F4A7C15ULL;

    s ^= timer_ticks() * 0x2545F4914F6CDD1DULL;
    if (rtc_unix_time)
        s ^= (uint64_t)rtc_unix_time() * 0xBF58476D1CE4E5B9ULL;
    s ^= (uint64_t)(uintptr_t)&rng_state;
    if (s == 0)
        s = 0x0123456789ABCDEFULL;
    rng_state = s;
}

uint32_t devfs_random32(void)
{
    uint64_t x;

    uint64_t f = irq_save();
    if (rng_state == 0)
        rng_seed();
    x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    irq_restore(f);

    return (uint32_t)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

void devfs_random_fill(void *buf, unsigned long n)
{
    uint8_t *p = buf;
    unsigned long i = 0;

    while (i < n) {
        uint32_t v = devfs_random32();
        for (int b = 0; b < 4 && i < n; b++, i++)
            p[i] = (uint8_t)(v >> (b * 8));
    }
}

/* --- path handling ---------------------------------------------------- */

bool devfs_claims(const char *path)
{
    if (path == NULL)
        return false;
    if (strcmp(path, "/dev") == 0 || strcmp(path, "/dev/") == 0)
        return true;
    return strncmp(path, "/dev/", 5) == 0;
}

/* Returns the table entry for "/dev/<name>", or NULL. "/dev" itself and
 * any path with a further component are not devices. */
static const struct devdesc *dev_lookup(const char *path)
{
    if (path == NULL || strncmp(path, "/dev/", 5) != 0)
        return NULL;

    const char *name = path + 5;
    if (name[0] == '\0' || strchr(name, '/') != NULL)
        return NULL;

    for (int i = 0; i < DEVTAB_COUNT; i++)
        if (strcmp(name, devtab[i].name) == 0)
            return &devtab[i];
    return NULL;
}

static bool path_is_devroot(const char *path)
{
    return path != NULL &&
           (strcmp(path, "/dev") == 0 || strcmp(path, "/dev/") == 0);
}

static int flags_allow_read(int flags)
{
    int acc = flags & (O_RDONLY | O_WRONLY | O_RDWR);
    return acc == O_RDONLY || acc == O_RDWR;
}

static int flags_allow_write(int flags)
{
    int acc = flags & (O_RDONLY | O_WRONLY | O_RDWR);
    return acc == O_WRONLY || acc == O_RDWR;
}

/* --- handle registry --------------------------------------------------- */

static int handle_register(struct devfile *df)
{
    uint64_t f = irq_save();
    for (int i = 0; i < DEVFS_MAX_OPEN; i++) {
        if (open_handles[i] == NULL) {
            open_handles[i] = df;
            irq_restore(f);
            return 0;
        }
    }
    irq_restore(f);
    return -1;
}

static void handle_unregister(struct devfile *df)
{
    uint64_t f = irq_save();
    for (int i = 0; i < DEVFS_MAX_OPEN; i++) {
        if (open_handles[i] == df) {
            open_handles[i] = NULL;
            break;
        }
    }
    irq_restore(f);
}

bool devfs_owns(struct file *f)
{
    bool found = false;

    if (f == NULL)
        return false;

    uint64_t g = irq_save();
    for (int i = 0; i < DEVFS_MAX_OPEN; i++) {
        if ((struct file *)open_handles[i] == f) {
            found = true;
            break;
        }
    }
    irq_restore(g);
    return found;
}

/* --- lifecycle --------------------------------------------------------- */

void devfs_init(void)
{
    for (int i = 0; i < DEVFS_MAX_OPEN; i++)
        open_handles[i] = NULL;
    rng_seed();
    devfs_ready = true;
    kprintf("devfs: /dev mounted (%d devices)\n", DEVTAB_COUNT);
    klog_printf(K_LOG_INFO, "devfs", "/dev mounted, %d devices", DEVTAB_COUNT);
}

struct file *devfs_open(const char *path, int flags)
{
    const struct devdesc *d = dev_lookup(path);

    if (!devfs_ready || d == NULL)
        return NULL;                /* "/dev" itself is a directory */

    /* Read-only devices reject write access outright. */
    if (d->id == DEV_RANDOM && flags_allow_write(flags))
        return NULL;

    struct devfile *df = kzalloc(sizeof(struct devfile));
    if (df == NULL)
        return NULL;

    df->f.inum = (uint32_t)d->id;   /* synthetic; never touches KFS */
    df->f.pos = 0;
    df->f.flags = flags;
    df->f.refs = 1;
    df->dev = d->id;
    df->ent = 0;
    df->lpos = 0;
    df->llen = 0;

    if (handle_register(df) < 0) {
        kfree(df);
        return NULL;
    }
    return &df->f;
}

void devfs_close(struct file *f)
{
    struct devfile *df = (struct devfile *)f;

    if (df == NULL)
        return;
    if (--df->f.refs > 0)
        return;
    handle_unregister(df);
    kfree(df);
}

/* --- reads ------------------------------------------------------------- */

static long read_console(void *buf, unsigned long n)
{
    uint8_t *p = buf;
    unsigned long i = 0;

    if (n == 0)
        return 0;

    /* Block for the first byte, then drain whatever else is queued. */
    int c = input_getc();
    if (c < 0)
        return 0;
    p[i++] = (uint8_t)c;

    while (i < n) {
        c = input_trygetc();
        if (c < 0)
            break;
        p[i++] = (uint8_t)c;
    }
    return (long)i;
}

static long read_klog(struct devfile *df, void *buf, unsigned long n)
{
    uint8_t *p = buf;
    unsigned long i = 0;
    struct k_logent e;

    while (i < n) {
        if (df->lpos >= df->llen) {
            if (klog_read(df->ent, &e) < 0)
                break;              /* end of the log: EOF */
            df->ent++;
            int len = klog_format_entry(&e, df->line, sizeof(df->line));
            df->llen = len > 0 ? (unsigned int)len : 0;
            df->lpos = 0;
            if (df->llen == 0)
                continue;
        }
        p[i++] = (uint8_t)df->line[df->lpos++];
    }
    df->f.pos += (uint32_t)i;
    return (long)i;
}

long devfs_read(struct file *f, void *buf, unsigned long n)
{
    struct devfile *df = (struct devfile *)f;

    if (df == NULL || buf == NULL)
        return -1;
    if (!flags_allow_read(df->f.flags))
        return -1;
    if (n == 0)
        return 0;
    if (n > 0x7FFFFFFFUL)
        n = 0x7FFFFFFFUL;

    switch (df->dev) {
    case DEV_NULL:
        return 0;                   /* immediate EOF */
    case DEV_ZERO:
    case DEV_FULL:                  /* /dev/full reads as zeros, like Unix */
        memset(buf, 0, n);
        df->f.pos += (uint32_t)n;
        return (long)n;
    case DEV_RANDOM:
        devfs_random_fill(buf, n);
        df->f.pos += (uint32_t)n;
        return (long)n;
    case DEV_CONSOLE:
        return read_console(buf, n);
    case DEV_KLOG:
        return read_klog(df, buf, n);
    default:
        return -1;
    }
}

/* --- writes ------------------------------------------------------------ */

static long write_console(const void *buf, unsigned long n)
{
    const char *p = buf;

    for (unsigned long i = 0; i < n; i++) {
        console_putc(p[i]);
        serial_putc(p[i]);
    }
    return (long)n;
}

/* Writing to /dev/klog files whole lines into the ring. */
static long write_klog(struct devfile *df, const void *buf, unsigned long n)
{
    const char *p = buf;

    for (unsigned long i = 0; i < n; i++) {
        char c = p[i];
        if (c == '\n' || df->llen + 1 >= sizeof(df->line)) {
            df->line[df->llen] = '\0';
            if (df->llen > 0)
                klog_write(K_LOG_INFO, "user", df->line);
            df->llen = 0;
            if (c == '\n')
                continue;
        }
        if (c == '\r' || c == '\0')
            continue;
        df->line[df->llen++] = c;
    }
    df->f.pos += (uint32_t)n;
    return (long)n;
}

long devfs_write(struct file *f, const void *buf, unsigned long n)
{
    struct devfile *df = (struct devfile *)f;

    if (df == NULL || buf == NULL)
        return -1;
    if (!flags_allow_write(df->f.flags))
        return -1;
    if (n == 0)
        return 0;
    if (n > 0x7FFFFFFFUL)
        n = 0x7FFFFFFFUL;

    switch (df->dev) {
    case DEV_NULL:
    case DEV_ZERO:
        df->f.pos += (uint32_t)n;
        return (long)n;             /* discarded */
    case DEV_FULL:
        return -1;                  /* always "out of space" */
    case DEV_CONSOLE:
        return write_console(buf, n);
    case DEV_KLOG:
        return write_klog(df, buf, n);
    default:
        return -1;
    }
}

/* --- seek --------------------------------------------------------------
 * Character devices have no meaningful position. Seeking to absolute 0
 * rewinds /dev/klog so a reader can re-read the ring; everything else
 * reports the running byte count. */
long devfs_seek(struct file *f, long off, int whence)
{
    struct devfile *df = (struct devfile *)f;

    if (df == NULL)
        return -1;
    if (df->dev == DEV_KLOG && whence == 0 && off == 0) {
        df->ent = 0;
        df->lpos = 0;
        df->llen = 0;
        df->f.pos = 0;
        return 0;
    }
    if (whence == 0 && off == 0 && df->dev != DEV_CONSOLE) {
        df->f.pos = 0;
        return 0;
    }
    return (long)df->f.pos;
}

/* --- metadata ----------------------------------------------------------- */

/* The klog device reports the size the log would render to, so that `ls`
 * and `cat` show something sensible. Everything else is zero-length. */
static uint32_t dev_size(int id)
{
    if (id != DEV_KLOG)
        return 0;

    struct k_logent e;
    uint32_t total = 0;
    char line[DEVFS_LINE_MAX];

    for (uint32_t i = 0; ; i++) {
        if (klog_read(i, &e) < 0)
            break;
        int len = klog_format_entry(&e, line, sizeof(line));
        if (len > 0)
            total += (uint32_t)len;
    }
    return total;
}

int devfs_stat(const char *path, struct k_stat *st)
{
    if (st == NULL)
        return -1;

    memset(st, 0, sizeof(*st));
    st->uid = 0;
    st->gid = 0;
    st->mtime = rtc_unix_time ? rtc_unix_time() : 0;

    if (path_is_devroot(path)) {
        st->is_dir = 1;
        st->mode = 0755;
        st->size = 0;
        return 0;
    }

    const struct devdesc *d = dev_lookup(path);
    if (d == NULL)
        return -1;

    st->is_dir = 0;
    st->mode = d->mode;
    st->size = dev_size(d->id);
    return 0;
}

int devfs_readdir(const char *path, int index, struct k_dirent *de)
{
    if (de == NULL || !path_is_devroot(path))
        return -1;
    if (index < 0 || index >= DEVTAB_COUNT)
        return -1;

    const struct devdesc *d = &devtab[index];

    memset(de, 0, sizeof(*de));
    strncpy(de->name, d->name, sizeof(de->name) - 1);
    de->name[sizeof(de->name) - 1] = '\0';
    de->size = dev_size(d->id);
    de->is_dir = 0;
    de->mode = d->mode;
    de->uid = 0;
    de->gid = 0;
    de->mtime = rtc_unix_time ? rtc_unix_time() : 0;
    return 0;
}
