#include "kernel.h"
#include "vfs.h"
#include "kfs.h"
#include "devfs.h"
#include "pipe.h"
#include "kheap.h"
#include "proc.h"
#include "rtc.h"
#include "timer.h"
#include "string.h"
#include "kestrel_abi.h"

/* Thin VFS over KFS. All paths are absolute.
 *
 * This layer owns access control. KFS stores mode/uid/gid and enforces
 * nothing; every check below compares the calling task's uid/gid against
 * the inode. uid 0 is root and passes everything. A path is only usable if
 * the caller has search (x) permission on every directory component, which
 * is why resolve() walks the path one name at a time instead of handing
 * the whole string to kfs_lookup(). */

static int vfs_ready;

/* --- open-inode table ------------------------------------------------
 * struct file holds a bare inum, and KFS hands a freed inode slot straight
 * back out to the next create. Unlinking an inode that still has open
 * handles would therefore let an old fd read and write a different file,
 * so a referenced inode is not removable. */

#define VFS_OPEN_INODES 64

static struct {
    uint32_t inum;
    int count;
} open_inodes[VFS_OPEN_INODES];

static int inode_ref(uint32_t inum)
{
    int slot = -1;
    uint64_t f = irq_save();
    for (int i = 0; i < VFS_OPEN_INODES; i++) {
        if (open_inodes[i].count > 0 && open_inodes[i].inum == inum) {
            open_inodes[i].count++;
            irq_restore(f);
            return 0;
        }
        if (open_inodes[i].count == 0 && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        irq_restore(f);
        return -1;                  /* too many distinct inodes open */
    }
    open_inodes[slot].inum = inum;
    open_inodes[slot].count = 1;
    irq_restore(f);
    return 0;
}

static void inode_unref(uint32_t inum)
{
    uint64_t f = irq_save();
    for (int i = 0; i < VFS_OPEN_INODES; i++) {
        if (open_inodes[i].count > 0 && open_inodes[i].inum == inum) {
            open_inodes[i].count--;
            break;
        }
    }
    irq_restore(f);
}

static int inode_is_open(uint32_t inum)
{
    int open = 0;
    uint64_t f = irq_save();
    for (int i = 0; i < VFS_OPEN_INODES; i++) {
        if (open_inodes[i].count > 0 && open_inodes[i].inum == inum) {
            open = 1;
            break;
        }
    }
    irq_restore(f);
    return open;
}

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

/* --- permission checks ----------------------------------------------- */

static uint32_t cur_uid(void)
{
    return current ? current->uid : 0;   /* pre-scheduler boot code is root */
}

static uint32_t cur_gid(void)
{
    return current ? current->gid : 0;
}

/* `want` is a bitwise OR of VFS_R / VFS_W / VFS_X. */
static int perm_ok(const struct kfs_inode *ip, int want)
{
    uint32_t mode = ip->mode & KFS_MODE_MASK;
    uint32_t bits;

    if (cur_uid() == 0)
        return 1;               /* root bypasses everything */
    if (cur_uid() == ip->uid)
        bits = (mode >> 6) & 7;
    else if (cur_gid() == ip->gid)
        bits = (mode >> 3) & 7;
    else
        bits = mode & 7;
    return ((int)bits & want) == want;
}

/* --- path walking ---------------------------------------------------- */

/* Pull the next component out of *pp. 1 = got one, 0 = end of path,
 * -1 = component too long. */
static int comp_next(const char **pp, char *name)
{
    const char *p = *pp;
    int i = 0;

    while (*p == '/')
        p++;
    if (*p == '\0') {
        *pp = p;
        return 0;
    }
    while (*p && *p != '/') {
        if (i >= KFS_NAME_MAX)
            return -1;
        name[i++] = *p++;
    }
    name[i] = '\0';
    *pp = p;
    return 1;
}

static int at_end(const char *p)
{
    while (*p == '/')
        p++;
    return *p == '\0';
}

/* Walk `path` from the root, requiring search (x) permission on every
 * directory crossed -- including the root and, when want_parent is set,
 * the parent itself.
 *
 * want_parent = 0: returns the inode number of `path`.
 * want_parent = 1: stops one component short, returns the parent
 *                  directory's inode and copies the last component into
 *                  last[] (which must hold KFS_NAME_MAX + 1 bytes).
 * Returns -1 for "not found", "not a directory" and "permission denied"
 * alike; the ABI has no errno to tell them apart. */
static int resolve(const char *path, int want_parent, char *last)
{
    struct kfs_inode ip;
    char comp[KFS_NAME_MAX + 1];
    uint32_t cur = KFS_ROOT_INO;
    const char *p = path;
    int r;

    if (!vfs_ready || path == NULL || path[0] != '/')
        return -1;

    while ((r = comp_next(&p, comp)) == 1) {
        if (kfs_iget(cur, &ip) < 0 || ip.type != KFS_TYPE_DIR)
            return -1;
        if (!perm_ok(&ip, VFS_X))
            return -1;
        if (want_parent && at_end(p)) {
            strcpy(last, comp);
            return (int)cur;
        }
        int next = kfs_lookup_in(cur, comp);
        if (next < 0)
            return -1;
        cur = (uint32_t)next;
    }
    if (r < 0)
        return -1;
    if (want_parent)
        return -1;              /* "/" has no parent entry */
    return (int)cur;
}

int vfs_init(void)
{
    if (kfs_mount() < 0) {
        kprintf("vfs: no root filesystem, running diskless\n");
        return -1;
    }
    kprintf("vfs: root filesystem mounted\n");
    vfs_ready = 1;
    return 0;
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

struct file *vfs_open(const char *path, int flags)
{
    struct kfs_inode ip, dirp;
    char name[KFS_NAME_MAX + 1];
    int parent, ino;
    int need_r, need_w;

    /* /dev is synthetic and works even on a diskless boot, so it is
     * checked before the mounted-filesystem test. */
    if (path && devfs_claims(path))
        return devfs_open(path, flags);

    if (!vfs_ready || path == NULL)
        return NULL;

    need_r = flags_allow_read(flags);
    /* O_CREAT and O_TRUNC both modify the object, so both demand write
     * permission even when the access mode alone would not. */
    need_w = flags_allow_write(flags) || (flags & (O_CREAT | O_TRUNC)) != 0;

    parent = resolve(path, 1, name);
    if (parent < 0 || kfs_iget((uint32_t)parent, &dirp) < 0)
        return NULL;

    ino = kfs_lookup_in((uint32_t)parent, name);
    if (ino < 0) {
        if (!(flags & O_CREAT))
            return NULL;
        /* Creating an entry writes the parent directory. */
        if (!perm_ok(&dirp, VFS_W | VFS_X))
            return NULL;
        ino = kfs_create(path, KFS_DEFAULT_FILE_MODE, cur_uid(), cur_gid(),
                         vfs_now());
        if (ino < 0)
            return NULL;
    }
    if (kfs_iget((uint32_t)ino, &ip) < 0)
        return NULL;
    if (ip.type == KFS_TYPE_DIR && need_w)
        return NULL;            /* directories are read-only via the VFS */
    if (need_r && !perm_ok(&ip, VFS_R))
        return NULL;
    if (need_w && !perm_ok(&ip, VFS_W))
        return NULL;

    if (inode_ref((uint32_t)ino) < 0)
        return NULL;

    if ((flags & O_TRUNC) && ip.type == KFS_TYPE_FILE) {
        if (kfs_truncate((uint32_t)ino, vfs_now()) < 0) {
            inode_unref((uint32_t)ino);
            return NULL;
        }
    }

    struct file *f = kzalloc(sizeof(struct file));
    if (f == NULL) {
        inode_unref((uint32_t)ino);
        return NULL;
    }
    f->type = FILE_KFS;
    f->inum = (uint32_t)ino;
    f->pos = 0;
    f->flags = flags;
    f->refs = 1;
    return f;
}

void vfs_close(struct file *f)
{
    int gone;
    uint64_t fl;

    if (f == NULL)
        return;
    if (devfs_owns(f)) {
        devfs_close(f);
        return;
    }
    /* fds are shared by dup2 and by spawn, and either holder may close on
     * a different CPU slice, so the refcount drop has to be atomic. */
    fl = irq_save();
    gone = (--f->refs <= 0);
    irq_restore(fl);
    if (!gone)
        return;

    if (f->type == FILE_PIPE)
        pipe_close(f);
    else
        inode_unref(f->inum);
    kfree(f);
}

long vfs_read(struct file *f, void *buf, unsigned long n)
{
    if (devfs_owns(f))
        return devfs_read(f, buf, n);
    if (f == NULL || !flags_allow_read(f->flags))
        return -1;
    if (f->type == FILE_PIPE)
        return pipe_read(f, buf, n);
    if (n > 0x7FFFFFFFUL)
        n = 0x7FFFFFFFUL;
    long r = kfs_read(f->inum, f->pos, buf, (uint32_t)n);
    if (r > 0)
        f->pos += (uint32_t)r;
    return r;
}

long vfs_write(struct file *f, const void *buf, unsigned long n)
{
    struct kfs_inode ip;

    if (devfs_owns(f))
        return devfs_write(f, buf, n);
    if (f == NULL || !flags_allow_write(f->flags))
        return -1;
    if (f->type == FILE_PIPE)
        return pipe_write(f, buf, n);
    if (n > 0x7FFFFFFFUL)
        n = 0x7FFFFFFFUL;
    if (f->flags & O_APPEND) {
        if (kfs_iget(f->inum, &ip) < 0)
            return -1;
        f->pos = ip.size;
    }
    long r = kfs_write(f->inum, f->pos, buf, (uint32_t)n, vfs_now());
    if (r > 0)
        f->pos += (uint32_t)r;
    return r;
}

long vfs_seek(struct file *f, long off, int whence)
{
    struct kfs_inode ip;
    long base;

    if (devfs_owns(f))
        return devfs_seek(f, off, whence);
    if (f == NULL)
        return -1;
    if (f->type == FILE_PIPE)
        return -1;              /* pipes are not seekable */
    switch (whence) {
    case 0:                     /* SEEK_SET */
        base = 0;
        break;
    case 1:                     /* SEEK_CUR */
        base = (long)f->pos;
        break;
    case 2:                     /* SEEK_END */
        if (kfs_iget(f->inum, &ip) < 0)
            return -1;
        base = (long)ip.size;
        break;
    default:
        return -1;
    }
    /* f->pos is 32-bit: narrowing an unchecked 64-bit offset would wrap a
     * huge seek back into range and silently redirect the next write. */
    if (off > 0 && base > (long)0x7FFFFFFFFFFFFFFFLL - off)
        return -1;
    long pos = base + off;
    if (pos < 0 || pos > (long)KFS_MAX_FILE_SIZE)
        return -1;
    f->pos = (uint32_t)pos;
    return pos;
}

static void fill_stat(const struct kfs_inode *ip, struct k_stat *st)
{
    st->size = ip->size;
    st->is_dir = (ip->type == KFS_TYPE_DIR);
    st->mode = ip->mode & KFS_MODE_MASK;
    st->uid = ip->uid;
    st->gid = ip->gid;
    st->mtime = ip->mtime;
}

int vfs_stat(const char *path, struct k_stat *st)
{
    struct kfs_inode ip;

    if (path && devfs_claims(path))
        return devfs_stat(path, st);
    if (!vfs_ready || path == NULL || st == NULL)
        return -1;
    int ino = resolve(path, 0, NULL);
    if (ino < 0 || kfs_iget((uint32_t)ino, &ip) < 0)
        return -1;
    fill_stat(&ip, st);
    return 0;
}

int vfs_readdir(const char *path, int index, struct k_dirent *de)
{
    struct kfs_dirent kde;
    struct kfs_inode ip;

    if (path && devfs_claims(path))
        return devfs_readdir(path, index, de);
    if (!vfs_ready || path == NULL || de == NULL)
        return -1;
    int ino = resolve(path, 0, NULL);
    if (ino < 0 || kfs_iget((uint32_t)ino, &ip) < 0)
        return -1;
    if (ip.type != KFS_TYPE_DIR || !perm_ok(&ip, VFS_R))
        return -1;              /* listing a directory reads it */
    if (kfs_readdir((uint32_t)ino, index, &kde) < 0)
        return -1;

    memcpy(de->name, kde.name, sizeof(de->name));
    de->name[sizeof(de->name) - 1] = '\0';
    de->size = 0;
    de->is_dir = 0;
    de->mode = 0;
    de->uid = 0;
    de->gid = 0;
    de->mtime = 0;
    if (kfs_iget(kde.ino, &ip) == 0) {
        de->size = ip.size;
        de->is_dir = (ip.type == KFS_TYPE_DIR);
        de->mode = ip.mode & KFS_MODE_MASK;
        de->uid = ip.uid;
        de->gid = ip.gid;
        de->mtime = ip.mtime;
    }
    return 0;
}

int vfs_unlink(const char *path)
{
    struct kfs_inode dirp;
    char name[KFS_NAME_MAX + 1];
    int parent, ino;

    if (path && devfs_claims(path))
        return -1;                   /* /dev is immutable */
    if (!vfs_ready || path == NULL)
        return -1;
    parent = resolve(path, 1, name);
    if (parent < 0 || kfs_iget((uint32_t)parent, &dirp) < 0)
        return -1;
    if (!perm_ok(&dirp, VFS_W | VFS_X))
        return -1;
    ino = kfs_lookup_in((uint32_t)parent, name);
    if (ino < 0)
        return -1;
    if (inode_is_open((uint32_t)ino))
        return -1;              /* still open: the slot must not be reused */
    return kfs_unlink(path, vfs_now());
}

int vfs_mkdir(const char *path)
{
    struct kfs_inode dirp;
    char name[KFS_NAME_MAX + 1];
    int parent;

    if (path && devfs_claims(path))
        return -1;                   /* /dev is immutable */
    if (!vfs_ready || path == NULL)
        return -1;
    parent = resolve(path, 1, name);
    if (parent < 0 || kfs_iget((uint32_t)parent, &dirp) < 0)
        return -1;
    if (!perm_ok(&dirp, VFS_W | VFS_X))
        return -1;
    return kfs_mkdir(path, KFS_DEFAULT_DIR_MODE, cur_uid(), cur_gid(),
                     vfs_now());
}

int vfs_chmod(const char *path, uint32_t mode)
{
    struct kfs_inode ip;

    if (!vfs_ready || path == NULL)
        return -1;
    int ino = resolve(path, 0, NULL);
    if (ino < 0 || kfs_iget((uint32_t)ino, &ip) < 0)
        return -1;
    /* Only the owner or root may re-permission a file. */
    if (cur_uid() != 0 && cur_uid() != ip.uid)
        return -1;
    return kfs_chmod((uint32_t)ino, (uint16_t)(mode & KFS_MODE_MASK),
                     vfs_now());
}

int vfs_chown(const char *path, uint32_t uid, uint32_t gid)
{
    struct kfs_inode ip;

    if (!vfs_ready || path == NULL)
        return -1;
    if (cur_uid() != 0)
        return -1;              /* giving a file away is root-only */
    int ino = resolve(path, 0, NULL);
    if (ino < 0 || kfs_iget((uint32_t)ino, &ip) < 0)
        return -1;
    return kfs_chown((uint32_t)ino, uid, gid, vfs_now());
}

int vfs_exec_ok(const char *path)
{
    struct kfs_inode ip;

    if (!vfs_ready || path == NULL)
        return 0;
    int ino = resolve(path, 0, NULL);
    if (ino < 0 || kfs_iget((uint32_t)ino, &ip) < 0)
        return 0;
    if (ip.type != KFS_TYPE_FILE)
        return 0;
    return perm_ok(&ip, VFS_X) ? 1 : 0;
}
