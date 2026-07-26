#include "kernel.h"
#include "devfs.h"
#include "klog.h"
#include "vfs.h"
#include "mount.h"
#include "blockdev.h"
#include "kheap.h"
#include "string.h"
#include "proc.h"
#include "console.h"
#include "serial.h"
#include "input.h"
#include "timer.h"
#include "kestrel_abi.h"

/* /dev pseudo-filesystem. See devfs.h for what it publishes.
 *
 * Paths arriving here are relative to the mount point: "/" is the
 * directory itself and "/null" is a device, so this file never mentions
 * the string "/dev" and would work just as well mounted somewhere else.
 *
 * Everything is synthesised on demand except the two table views, which
 * are rendered into a private buffer at open() so that a reader sees one
 * consistent snapshot rather than a table mutating under it. */

#define DEVFS_MAX_OPEN 32
#define DEVFS_LINE_MAX 192
#define DEVFS_TEXT_MAX 1024

struct devfile {
    struct file f;              /* MUST stay first: handles are cast to this */
    int dev;
    struct blockdev *bd;        /* DEV_BLOCKDEV: the device */
    uint8_t *bounce;            /* DEV_BLOCKDEV: one block of scratch */
    char *text;                 /* DEV_MOUNTS / DEV_BLOCKS: the snapshot */
    uint32_t textlen;
    uint32_t ent;               /* klog: next log index to render */
    unsigned int lpos;          /* klog: consumed bytes of `line` */
    unsigned int llen;          /* klog: valid bytes in `line` */
    char line[DEVFS_LINE_MAX];
};

struct devdesc {
    const char *name;
    int id;
    uint32_t mode;
};

/* The fixed devices. Block devices are appended to this list at runtime
 * from the block layer's registry; everything in /dev is owned by
 * root:root. */
static const struct devdesc devtab[] = {
    { "null",    DEV_NULL,    0666 },
    { "zero",    DEV_ZERO,    0666 },
    { "full",    DEV_FULL,    0666 },
    { "console", DEV_CONSOLE, 0622 },
    { "random",  DEV_RANDOM,  0444 },
    { "klog",    DEV_KLOG,    0644 },
    { "mounts",  DEV_MOUNTS,  0444 },
    { "blocks",  DEV_BLOCKS,  0444 },
};

#define DEVTAB_COUNT ((int)(sizeof(devtab) / sizeof(devtab[0])))

/* Disks are root-only, as they bypass every filesystem permission. */
#define DEVFS_BLOCK_MODE 0600

/* Largest block size devfs will hand out a bounce buffer for. */
#define DEVFS_BLOCK_MAX 4096

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

/* --- path handling ----------------------------------------------------
 * Paths are mount-relative: "/" is the directory, "/name" is an entry,
 * anything deeper is not ours. */

static bool path_is_root(const char *path)
{
    return path != NULL && path[0] == '/' && path[1] == '\0';
}

/* Returns the single name in "/name", or NULL for the directory itself
 * and for any path with a further component. */
static const char *path_name(const char *path)
{
    if (path == NULL || path[0] != '/' || path[1] == '\0')
        return NULL;
    if (strchr(path + 1, '/') != NULL)
        return NULL;
    return path + 1;
}

static const struct devdesc *dev_lookup(const char *path)
{
    const char *name = path_name(path);

    if (name == NULL)
        return NULL;
    for (int i = 0; i < DEVTAB_COUNT; i++)
        if (strcmp(name, devtab[i].name) == 0)
            return &devtab[i];
    return NULL;
}

static struct blockdev *blk_lookup(const char *path)
{
    const char *name = path_name(path);

    return name ? blockdev_find(name) : NULL;
}

/* --- text views -------------------------------------------------------
 * The kernel has no snprintf, so the two table views are built with a
 * bounded appender. Both are small and fixed-width in practice. */

struct sbuf {
    char *p;
    uint32_t len;
    uint32_t cap;
};

static void sb_putc(struct sbuf *b, char c)
{
    if (b->len + 1 < b->cap)
        b->p[b->len++] = c;
}

static void sb_puts(struct sbuf *b, const char *s)
{
    while (*s)
        sb_putc(b, *s++);
}

static void sb_putu(struct sbuf *b, uint64_t v)
{
    char tmp[24];
    int i = 0;

    if (v == 0) {
        sb_putc(b, '0');
        return;
    }
    while (v > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0)
        sb_putc(b, tmp[--i]);
}

/* "<device> <mount point> <type> <block size> <blocks> <free blocks>"
 * one line per mount; "-" where a filesystem has no device. */
static uint32_t render_mounts(char *out, uint32_t cap)
{
    struct sbuf b = { out, 0, cap };
    struct mount *m;

    for (int i = 0; mount_list(i, &m) == 0; i++) {
        struct fs_statfs sf;

        sb_puts(&b, m->bd ? m->bd->name : "-");
        sb_putc(&b, ' ');
        sb_puts(&b, m->path);
        sb_putc(&b, ' ');
        sb_puts(&b, m->type->name);
        sb_putc(&b, ' ');
        memset(&sf, 0, sizeof(sf));
        if (m->type->ops->statfs == NULL || m->type->ops->statfs(m, &sf) < 0)
            memset(&sf, 0, sizeof(sf));
        sb_putu(&b, sf.block_size);
        sb_putc(&b, ' ');
        sb_putu(&b, sf.blocks);
        sb_putc(&b, ' ');
        sb_putu(&b, sf.free_blocks);
        sb_putc(&b, '\n');
    }
    b.p[b.len] = '\0';
    return b.len;
}

/* "<name> <block size> <blocks>" one line per registered block device. */
static uint32_t render_blocks(char *out, uint32_t cap)
{
    struct sbuf b = { out, 0, cap };
    struct blockdev *bd;

    for (int i = 0; blockdev_list(i, &bd) == 0; i++) {
        sb_puts(&b, bd->name);
        sb_putc(&b, ' ');
        sb_putu(&b, bd->block_size);
        sb_putc(&b, ' ');
        sb_putu(&b, bd->blocks);
        sb_putc(&b, '\n');
    }
    b.p[b.len] = '\0';
    return b.len;
}

/* --- sizes ------------------------------------------------------------
 * The klog device reports the size the log would render to, and a block
 * device its capacity, so that `ls` and `cat` show something sensible.
 * Everything else is zero-length. */

static uint32_t klog_size(void)
{
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

static uint32_t text_size(int id)
{
    char buf[DEVFS_TEXT_MAX];

    if (id == DEV_MOUNTS)
        return render_mounts(buf, sizeof(buf));
    return render_blocks(buf, sizeof(buf));
}

/* struct k_stat carries a 32-bit size, so a device larger than 4 GiB
 * reports the clamp rather than a wrapped value. */
static uint32_t blk_size_bytes(struct blockdev *bd)
{
    uint64_t bytes = bd->blocks * (uint64_t)bd->block_size;

    return bytes > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)bytes;
}

static uint32_t dev_size(int id)
{
    switch (id) {
    case DEV_KLOG:
        return klog_size();
    case DEV_MOUNTS:
    case DEV_BLOCKS:
        return text_size(id);
    default:
        return 0;
    }
}

/* --- handle registry --------------------------------------------------
 * Only used to bound how many devices may be open at once; dispatch is
 * by the ops vector the VFS stamps on the handle. */

static struct devfile *open_handles[DEVFS_MAX_OPEN];

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

/* --- open / close ------------------------------------------------------ */

static void devfile_free(struct devfile *df)
{
    if (df->bounce)
        kfree(df->bounce);
    if (df->text)
        kfree(df->text);
    kfree(df);
}

static struct devfile *devfile_new(int id, int flags)
{
    struct devfile *df = kzalloc(sizeof(struct devfile));

    if (df == NULL)
        return NULL;
    df->dev = id;
    df->f.inum = (uint32_t)id;  /* synthetic; never touches a disk inode */
    df->f.pos = 0;
    df->f.flags = flags;
    df->f.refs = 1;
    return df;
}

/* O_CREAT and O_TRUNC are accepted and ignored: nothing under /dev is
 * created or grown, and truncating a device is meaningless -- but
 * `cmd > /dev/null` passes both, and refusing them would break the most
 * common thing anyone does with this filesystem. */
static struct file *devfs_op_open(struct mount *m, const char *path, int flags)
{
    const struct devdesc *d;
    struct blockdev *bd;
    struct devfile *df;

    (void)m;
    if (!devfs_ready)
        return NULL;

    d = dev_lookup(path);
    if (d != NULL) {
        if (!vfs_perm_ok(d->mode, 0, 0,
                         (vfs_flags_allow_read(flags) ? VFS_R : 0) |
                         (vfs_flags_allow_write(flags) ? VFS_W : 0)))
            return NULL;

        df = devfile_new(d->id, flags);
        if (df == NULL)
            return NULL;
        if (d->id == DEV_MOUNTS || d->id == DEV_BLOCKS) {
            df->text = kmalloc(DEVFS_TEXT_MAX);
            if (df->text == NULL) {
                devfile_free(df);
                return NULL;
            }
            df->textlen = (d->id == DEV_MOUNTS)
                            ? render_mounts(df->text, DEVFS_TEXT_MAX)
                            : render_blocks(df->text, DEVFS_TEXT_MAX);
        }
        if (handle_register(df) < 0) {
            devfile_free(df);
            return NULL;
        }
        return &df->f;
    }

    bd = blk_lookup(path);
    if (bd == NULL)
        return NULL;            /* "/" itself is a directory, not a file */
    if (bd->block_size > DEVFS_BLOCK_MAX)
        return NULL;
    if (!vfs_perm_ok(DEVFS_BLOCK_MODE, 0, 0,
                     (vfs_flags_allow_read(flags) ? VFS_R : 0) |
                     (vfs_flags_allow_write(flags) ? VFS_W : 0)))
        return NULL;

    df = devfile_new(DEV_BLOCKDEV, flags);
    if (df == NULL)
        return NULL;
    df->bd = bd;
    df->bounce = kmalloc(bd->block_size);
    if (df->bounce == NULL || handle_register(df) < 0) {
        devfile_free(df);
        return NULL;
    }
    return &df->f;
}

static void devfs_op_close(struct file *f)
{
    struct devfile *df = (struct devfile *)f;

    if (df == NULL)
        return;
    handle_unregister(df);
    devfile_free(df);
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

static long read_text(struct devfile *df, void *buf, unsigned long n)
{
    uint32_t pos = df->f.pos;

    if (df->text == NULL || pos >= df->textlen)
        return 0;
    if (n > df->textlen - pos)
        n = df->textlen - pos;
    memcpy(buf, df->text + pos, n);
    df->f.pos = pos + (uint32_t)n;
    return (long)n;
}

/* Byte-granular access to a block device, one block at a time through
 * the handle's bounce buffer. */
static long read_blockdev(struct devfile *df, void *buf, unsigned long n)
{
    struct blockdev *bd = df->bd;
    uint64_t size = bd->blocks * (uint64_t)bd->block_size;
    uint64_t pos = df->f.pos;
    uint8_t *out = buf;
    unsigned long done = 0;

    if (pos >= size)
        return 0;
    if (n > size - pos)
        n = (unsigned long)(size - pos);

    while (done < n) {
        uint64_t lba = pos / bd->block_size;
        uint32_t off = (uint32_t)(pos % bd->block_size);
        uint32_t chunk = bd->block_size - off;
        if (chunk > n - done)
            chunk = (uint32_t)(n - done);
        if (blockdev_read(bd, lba, 1, df->bounce) < 0)
            return done ? (long)done : -1;
        memcpy(out + done, df->bounce + off, chunk);
        done += chunk;
        pos += chunk;
    }
    df->f.pos = (uint32_t)pos;
    return (long)done;
}

static long devfs_op_read(struct file *f, void *buf, unsigned long n)
{
    struct devfile *df = (struct devfile *)f;

    if (df == NULL || buf == NULL)
        return -1;
    if (!vfs_flags_allow_read(df->f.flags))
        return -1;
    if (n == 0)
        return 0;

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
    case DEV_MOUNTS:
    case DEV_BLOCKS:
        return read_text(df, buf, n);
    case DEV_BLOCKDEV:
        return read_blockdev(df, buf, n);
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

/* A partial block is read back before it is patched, so writing one byte
 * to /dev/hda does not zero the other 511. */
static long write_blockdev(struct devfile *df, const void *buf,
                           unsigned long n)
{
    struct blockdev *bd = df->bd;
    uint64_t size = bd->blocks * (uint64_t)bd->block_size;
    uint64_t pos = df->f.pos;
    const uint8_t *in = buf;
    unsigned long done = 0;

    if (pos >= size)
        return -1;                  /* past the end of the device */
    if (n > size - pos)
        n = (unsigned long)(size - pos);

    while (done < n) {
        uint64_t lba = pos / bd->block_size;
        uint32_t off = (uint32_t)(pos % bd->block_size);
        uint32_t chunk = bd->block_size - off;
        if (chunk > n - done)
            chunk = (uint32_t)(n - done);
        if (chunk < bd->block_size) {
            if (blockdev_read(bd, lba, 1, df->bounce) < 0)
                return done ? (long)done : -1;
        }
        memcpy(df->bounce + off, in + done, chunk);
        if (blockdev_write(bd, lba, 1, df->bounce) < 0)
            return done ? (long)done : -1;
        done += chunk;
        pos += chunk;
    }
    df->f.pos = (uint32_t)pos;
    return (long)done;
}

static long devfs_op_write(struct file *f, const void *buf, unsigned long n)
{
    struct devfile *df = (struct devfile *)f;

    if (df == NULL || buf == NULL)
        return -1;
    if (!vfs_flags_allow_write(df->f.flags))
        return -1;
    if (n == 0)
        return 0;

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
    case DEV_BLOCKDEV:
        return write_blockdev(df, buf, n);
    default:
        return -1;                  /* the table views are read-only */
    }
}

/* --- seek --------------------------------------------------------------
 * Block devices and the table views seek for real. Character devices have
 * no meaningful position: seeking to absolute 0 rewinds /dev/klog so a
 * reader can re-read the ring, and everything else reports the running
 * byte count. */

static long seek_bounded(struct devfile *df, long off, int whence,
                         uint64_t end)
{
    long base, pos;

    switch (whence) {
    case 0: base = 0;                   break;
    case 1: base = (long)df->f.pos;     break;
    case 2: base = (long)end;           break;
    default: return -1;
    }
    if (off > 0 && base > (long)0x7FFFFFFFFFFFFFFFLL - off)
        return -1;
    pos = base + off;
    if (pos < 0 || (uint64_t)pos > end || pos > 0xFFFFFFFFL)
        return -1;
    df->f.pos = (uint32_t)pos;
    return pos;
}

static long devfs_op_seek(struct file *f, long off, int whence)
{
    struct devfile *df = (struct devfile *)f;

    if (df == NULL)
        return -1;
    if (df->dev == DEV_BLOCKDEV)
        return seek_bounded(df, off, whence, blk_size_bytes(df->bd));
    if (df->dev == DEV_MOUNTS || df->dev == DEV_BLOCKS)
        return seek_bounded(df, off, whence, df->textlen);
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

static int devfs_op_stat(struct mount *m, const char *path, struct k_stat *st)
{
    const struct devdesc *d;
    struct blockdev *bd;

    (void)m;
    if (st == NULL)
        return -1;

    memset(st, 0, sizeof(*st));
    st->uid = 0;
    st->gid = 0;
    st->mtime = rtc_unix_time();

    if (path_is_root(path)) {
        st->is_dir = 1;
        st->mode = 0755;
        st->size = 0;
        return 0;
    }

    d = dev_lookup(path);
    if (d != NULL) {
        st->mode = d->mode;
        st->size = dev_size(d->id);
        return 0;
    }

    bd = blk_lookup(path);
    if (bd == NULL)
        return -1;
    st->mode = DEVFS_BLOCK_MODE;
    st->size = blk_size_bytes(bd);
    return 0;
}

/* The directory is the fixed table followed by the block devices, so an
 * index past DEVTAB_COUNT selects from the block registry. */
static int devfs_op_readdir(struct mount *m, const char *path, int index,
                            struct k_dirent *de)
{
    struct blockdev *bd;

    (void)m;
    if (de == NULL || index < 0 || !path_is_root(path))
        return -1;

    memset(de, 0, sizeof(*de));
    de->mtime = rtc_unix_time();

    if (index < DEVTAB_COUNT) {
        const struct devdesc *d = &devtab[index];
        strncpy(de->name, d->name, sizeof(de->name) - 1);
        de->size = dev_size(d->id);
        de->mode = d->mode;
        return 0;
    }
    if (blockdev_list(index - DEVTAB_COUNT, &bd) < 0)
        return -1;
    strncpy(de->name, bd->name, sizeof(de->name) - 1);
    de->size = blk_size_bytes(bd);
    de->mode = DEVFS_BLOCK_MODE;
    return 0;
}

/* devfs stores nothing, so it has no blocks to report. */
static int devfs_op_statfs(struct mount *m, struct fs_statfs *sf)
{
    (void)m;
    if (sf == NULL)
        return -1;
    memset(sf, 0, sizeof(*sf));
    return 0;
}

/* No unlink, mkdir, chmod or chown: /dev is synthesised from the
 * kernel's own tables and is not editable, so the VFS answers -1. */
static struct fs_ops devfs_ops = {
    .open    = devfs_op_open,
    .close   = devfs_op_close,
    .read    = devfs_op_read,
    .write   = devfs_op_write,
    .seek    = devfs_op_seek,
    .stat    = devfs_op_stat,
    .readdir = devfs_op_readdir,
    .statfs  = devfs_op_statfs,
};

/* One instance, whatever it is mounted on: there is only one set of
 * kernel tables to publish. */
static int devfs_type_mount(struct blockdev *bd, void **fs_priv)
{
    (void)bd;
    if (fs_priv == NULL)
        return -1;
    *fs_priv = (void *)&devfs_ops;   /* a non-NULL instance token */
    kprintf("devfs: /dev mounted (%d devices)\n", DEVTAB_COUNT);
    klog_printf(K_LOG_INFO, "devfs", "/dev mounted, %d devices", DEVTAB_COUNT);
    return 0;
}

static void devfs_type_unmount(void *fs_priv)
{
    (void)fs_priv;
}

static struct fs_type devfs_type = {
    .name    = "devfs",
    .mount   = devfs_type_mount,
    .unmount = devfs_type_unmount,
    .ops     = &devfs_ops,
};

void devfs_init(void)
{
    if (devfs_ready)
        return;
    for (int i = 0; i < DEVFS_MAX_OPEN; i++)
        open_handles[i] = NULL;
    rng_seed();
    devfs_ready = true;
    fs_register(&devfs_type);
}
