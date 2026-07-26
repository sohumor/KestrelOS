#include "kernel.h"
#include "vfs.h"
#include "mount.h"
#include "kfs.h"
#include "devfs.h"
#include "pipe.h"
#include "kheap.h"
#include "proc.h"
#include "rtc.h"
#include "timer.h"
#include "string.h"
#include "kestrel_abi.h"

/* The VFS. All paths are absolute.
 *
 * There is nothing filesystem-specific below this comment. Every path
 * entry point resolves the path to a mount and a path relative to it
 * (longest prefix wins, so "/dev" beats "/") and calls that mount's
 * operations; every handle entry point calls the operations the handle
 * carries. Pipes are not a special case -- they are simply a handle with
 * a different ops vector and no mount.
 *
 * What does live here is the *policy* half of access control:
 * vfs_perm_ok() is the single place the uid/gid/mode rule is written
 * down. The walk that applies it belongs to each filesystem, which is
 * the only thing that can cross its own directories in one pass. */

/* --- wall clock ------------------------------------------------------
 * rtc_read() spins on the CMOS update-in-progress flag and re-reads the
 * register file until two sweeps agree, so it costs far too much to run on
 * every file write. Sample it rarely and interpolate from the timer tick
 * in between: the PIT is exact enough over a minute, and mtime only has
 * one-second resolution anyway.
 *
 * The cache is read and written from preemptible syscall context, so the
 * three fields are updated together under irq_save(). rtc_read() itself is
 * deliberately called with interrupts on and with no filesystem lock held
 * (see the locking rule at the top of kfs.c). */

#define CLK_REFRESH_TICKS ((uint64_t)TIMER_HZ * 60)

static uint32_t clk_base;       /* Unix seconds sampled at clk_tick */
static uint64_t clk_tick;       /* timer_ticks() when clk_base was taken */
static int clk_valid;

static int is_leap(uint32_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

/* Leap years in the closed-open range [1, y). Every 4th year is a leap
 * year, minus every 100th, plus every 400th, counted over the years
 * strictly before y. */
static uint32_t leaps_before(uint32_t y)
{
    uint32_t p = y - 1;
    return p / 4 - p / 100 + p / 400;
}

/* Days elapsed from 1970-01-01 to y-mm-dd, Gregorian, y >= 1970.
 * Whole years first (365 each plus one per intervening leap year), then
 * whole months from a running total, then the days inside the month. */
static uint32_t days_from_epoch(uint32_t y, uint32_t mon, uint32_t day)
{
    static const uint16_t month_start[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    uint32_t days;

    if (y < 1970 || mon < 1 || mon > 12 || day < 1)
        return 0;
    days = (y - 1970) * 365;
    days += leaps_before(y) - leaps_before(1970);
    days += month_start[mon - 1];
    if (mon > 2 && is_leap(y))
        days++;                 /* this year's Feb 29 is already behind us */
    days += day - 1;
    return days;
}

/* The CMOS clock is taken to be UTC; KestrelOS has no timezone database. */
static uint32_t rtc_to_unix(const struct k_rtc *t)
{
    uint32_t days = days_from_epoch(t->year, t->mon, t->day);
    return days * 86400u + (uint32_t)t->hour * 3600u +
           (uint32_t)t->min * 60u + (uint32_t)t->sec;
}

static uint32_t clk_extrapolate(uint32_t base, uint64_t tick, uint64_t now)
{
    if (now < tick)
        return base;
    return base + (uint32_t)((now - tick) / TIMER_HZ);
}

uint32_t rtc_unix_time(void)
{
    struct k_rtc t;
    uint64_t now = timer_ticks();
    uint32_t base, secs;
    uint64_t tick;
    int valid;
    uint64_t f;

    f = irq_save();
    valid = clk_valid;
    base = clk_base;
    tick = clk_tick;
    irq_restore(f);

    if (valid && now - tick < CLK_REFRESH_TICKS)
        return clk_extrapolate(base, tick, now);

    if (rtc_read(&t) < 0) {
        /* No usable chip: keep extrapolating from whatever we last had,
         * and report 0 ("unknown") if we never had anything. */
        return valid ? clk_extrapolate(base, tick, now) : 0;
    }
    secs = rtc_to_unix(&t);

    f = irq_save();
    clk_base = secs;
    clk_tick = timer_ticks();
    clk_valid = 1;
    irq_restore(f);
    return secs;
}

uint32_t vfs_now(void)
{
    return rtc_unix_time();
}

/* --- access-control policy -------------------------------------------
 * The one copy of the rule. Filesystems call it; nothing else decides
 * who may do what. */

uint32_t vfs_uid(void)
{
    return current ? current->uid : 0;   /* pre-scheduler boot code is root */
}

uint32_t vfs_gid(void)
{
    return current ? current->gid : 0;
}

int vfs_perm_ok(uint32_t mode, uint32_t uid, uint32_t gid, int want)
{
    uint32_t bits;

    if (vfs_uid() == 0)
        return 1;               /* root bypasses everything */
    if (vfs_uid() == uid)
        bits = (mode >> 6) & 7;
    else if (vfs_gid() == gid)
        bits = (mode >> 3) & 7;
    else
        bits = mode & 7;
    return ((int)bits & want) == want;
}

int vfs_flags_allow_read(int flags)
{
    int acc = flags & (O_RDONLY | O_WRONLY | O_RDWR);
    return acc == O_RDONLY || acc == O_RDWR;
}

int vfs_flags_allow_write(int flags)
{
    int acc = flags & (O_RDONLY | O_WRONLY | O_RDWR);
    return acc == O_WRONLY || acc == O_RDWR;
}

/* --- pipes as ordinary handles ---------------------------------------
 * pipe.c owns the ring buffer; the ops vector below is what lets a pipe
 * end travel the same read/write path as a file. Seek is absent, so
 * vfs_seek() answers -1 for a pipe without knowing what a pipe is. */

static void pipe_op_close(struct file *f)
{
    pipe_close(f);
    kfree(f);
}

static struct fs_ops pipe_ops = {
    .read  = pipe_read,
    .write = pipe_write,
    .close = pipe_op_close,
};

int vfs_pipe(struct file **read_end, struct file **write_end)
{
    if (pipe_create(read_end, write_end) < 0)
        return -1;
    (*read_end)->ops = &pipe_ops;
    (*write_end)->ops = &pipe_ops;
    return 0;
}

/* A handle created by pipe_create() directly (rather than through
 * vfs_pipe) carries no ops vector; bind it on first use so the old entry
 * point keeps working. Nothing else can produce a handle without ops:
 * vfs_open() stamps every one it hands out. */
static const struct fs_ops *file_ops(struct file *f)
{
    if (f->ops == NULL && f->type == FILE_PIPE)
        f->ops = &pipe_ops;
    return f->ops;
}

/* --- mounting -------------------------------------------------------- */

int vfs_init(void)
{
    int rooted;

    /* Filesystem types first, then the mounts. devfs_init() only
     * registers the type and seeds its PRNG; it is idempotent, so
     * kmain() calling it again afterwards costs nothing. */
    kfs_init();
    devfs_init();

    rooted = (mount_add("/", "kfs", "hda") == 0);
    if (!rooted)
        kprintf("vfs: no root filesystem, running diskless\n");
    else
        kprintf("vfs: root filesystem mounted\n");

    /* /dev is synthetic and works even on a diskless boot, so it is
     * mounted either way. It is a longer prefix than "/", so it wins
     * every path under it without any special case. */
    if (mount_add("/dev", "devfs", NULL) < 0)
        kprintf("vfs: cannot mount /dev\n");

    return rooted ? 0 : -1;
}

/* --- path dispatch ----------------------------------------------------
 * Every one of these is the same three lines: resolve, check the mount
 * implements the operation, call it. */

struct file *vfs_open(const char *path, int flags)
{
    const char *rel;
    struct mount *m;
    struct file *f;

    if (path == NULL)
        return NULL;
    m = mount_resolve(path, &rel);
    if (m == NULL || m->type->ops->open == NULL)
        return NULL;
    f = m->type->ops->open(m, rel, flags);
    if (f == NULL)
        return NULL;
    f->ops = m->type->ops;
    f->mnt = m;
    return f;
}

int vfs_stat(const char *path, struct k_stat *st)
{
    const char *rel;
    struct mount *m;

    if (path == NULL || st == NULL)
        return -1;
    m = mount_resolve(path, &rel);
    if (m == NULL || m->type->ops->stat == NULL)
        return -1;
    return m->type->ops->stat(m, rel, st);
}

int vfs_readdir(const char *path, int index, struct k_dirent *de)
{
    const char *rel;
    struct mount *m;

    if (path == NULL || de == NULL || index < 0)
        return -1;
    m = mount_resolve(path, &rel);
    if (m == NULL || m->type->ops->readdir == NULL)
        return -1;
    return m->type->ops->readdir(m, rel, index, de);
}

int vfs_unlink(const char *path)
{
    const char *rel;
    struct mount *m;

    if (path == NULL)
        return -1;
    m = mount_resolve(path, &rel);
    if (m == NULL || m->type->ops->unlink == NULL)
        return -1;              /* e.g. /dev, which is immutable */
    return m->type->ops->unlink(m, rel);
}

int vfs_mkdir(const char *path)
{
    const char *rel;
    struct mount *m;

    if (path == NULL)
        return -1;
    m = mount_resolve(path, &rel);
    if (m == NULL || m->type->ops->mkdir == NULL)
        return -1;
    return m->type->ops->mkdir(m, rel);
}

int vfs_chmod(const char *path, uint32_t mode)
{
    const char *rel;
    struct mount *m;

    if (path == NULL)
        return -1;
    m = mount_resolve(path, &rel);
    if (m == NULL || m->type->ops->chmod == NULL)
        return -1;
    return m->type->ops->chmod(m, rel, mode);
}

int vfs_chown(const char *path, uint32_t uid, uint32_t gid)
{
    const char *rel;
    struct mount *m;

    if (path == NULL)
        return -1;
    m = mount_resolve(path, &rel);
    if (m == NULL || m->type->ops->chown == NULL)
        return -1;
    return m->type->ops->chown(m, rel, uid, gid);
}

/* Executability is stat plus the shared policy, so it needs no operation
 * of its own: the filesystem's stat already refused the path if the
 * caller could not search its way there. */
int vfs_exec_ok(const char *path)
{
    struct k_stat st;

    if (vfs_stat(path, &st) < 0 || st.is_dir)
        return 0;
    return vfs_perm_ok(st.mode, st.uid, st.gid, VFS_X) ? 1 : 0;
}

/* --- handle dispatch -------------------------------------------------- */

void vfs_close(struct file *f)
{
    const struct fs_ops *ops;
    int gone;
    uint64_t fl;

    if (f == NULL)
        return;
    /* fds are shared by dup2 and by spawn, and either holder may close on
     * a different CPU slice, so the refcount drop has to be atomic. */
    fl = irq_save();
    gone = (--f->refs <= 0);
    irq_restore(fl);
    if (!gone)
        return;

    ops = file_ops(f);
    if (ops != NULL && ops->close != NULL)
        ops->close(f);
    else
        kfree(f);
}

long vfs_read(struct file *f, void *buf, unsigned long n)
{
    const struct fs_ops *ops;

    if (f == NULL || buf == NULL)
        return -1;
    if (!vfs_flags_allow_read(f->flags))
        return -1;
    ops = file_ops(f);
    if (ops == NULL || ops->read == NULL)
        return -1;
    if (n > 0x7FFFFFFFUL)
        n = 0x7FFFFFFFUL;
    return ops->read(f, buf, n);
}

long vfs_write(struct file *f, const void *buf, unsigned long n)
{
    const struct fs_ops *ops;

    if (f == NULL || buf == NULL)
        return -1;
    if (!vfs_flags_allow_write(f->flags))
        return -1;
    ops = file_ops(f);
    if (ops == NULL || ops->write == NULL)
        return -1;
    if (n > 0x7FFFFFFFUL)
        n = 0x7FFFFFFFUL;
    return ops->write(f, buf, n);
}

long vfs_seek(struct file *f, long off, int whence)
{
    const struct fs_ops *ops;

    if (f == NULL)
        return -1;
    ops = file_ops(f);
    if (ops == NULL || ops->seek == NULL)
        return -1;              /* pipes are not seekable */
    return ops->seek(f, off, whence);
}
