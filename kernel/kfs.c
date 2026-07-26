#include "kernel.h"
#include "kfs.h"
#include "vfs.h"
#include "mount.h"
#include "blockdev.h"
#include "kheap.h"
#include "proc.h"
#include "string.h"
#include "kestrel_abi.h"

/* KFS driver, on-disk format v2. Write-through: every metadata update goes
 * straight to disk. A handful of static scratch blocks stand in for a
 * buffer cache; they are carefully assigned so no code path aliases two
 * uses of the same buffer (see comments at each declaration).
 *
 * The file has two halves. Below is the driver proper, which knows about
 * blocks, inodes and directories and enforces no access control at all.
 * On top of it sits the `struct fs_ops` layer, which is what the VFS
 * calls: it resolves paths one component at a time so it can check search
 * permission on every directory crossed, using the shared policy in
 * vfs.h so that the uid/gid/mode rules exist in exactly one place.
 *
 * A mount binds an instance (struct kfs_fs) to a block device and an
 * offset. The offset is discovered by probing, not baked in: an image
 * written at sector 0 of its own device mounts as readily as the boot
 * disk's partition at KFS_PART_LBA.
 *
 * ---- locking rule ----------------------------------------------------
 * The scratch buffers below are shared by every instance, and syscalls
 * run preemptible (syscall.c enables interrupts on entry), so:
 *
 *   1. Every public kfs_* entry point takes the whole-driver mutex
 *      fs_lock() on entry and releases it on every return path. Without it
 *      two tasks interleaved inside balloc()/writei() double-allocate
 *      blocks and cross-write each other's file data.
 *   2. Everything below the entry points is a *_locked / lower-case helper
 *      that assumes the lock is already held and never takes it again.
 *      The mutex is recursive by owner anyway (unlink and mkdir reach the
 *      truncate path), but new code should still call the helper.
 *   3. The lock may only be held across disk I/O, which is polled PIO and
 *      always terminates. Nothing under the lock may sleep, wait on the
 *      keyboard, wait on the network, or read the CMOS clock -- that is
 *      why every mutating entry point takes an `mtime` argument instead of
 *      calling the clock itself. The ops layer samples the (cached) time
 *      before it calls in, and never holds the lock across a path walk.
 *   4. Nothing here runs in IRQ context, so kmalloc-free / no-sleep rules
 *      for interrupt handlers do not apply.
 */

/* The on-disk layout is fixed; a padding surprise here would silently
 * shift every inode after the first. */
_Static_assert(sizeof(struct kfs_inode) == 64, "kfs inode must be 64 bytes");
_Static_assert(sizeof(struct kfs_dirent) == 64, "kfs dirent must be 64 bytes");
_Static_assert(sizeof(struct kfs_superblock) == 40, "kfs superblock is 40 bytes");

#define KFS_MAX_MOUNTS 4

static struct kfs_fs instances[KFS_MAX_MOUNTS];

/* Whole-driver mutex. `fs_held` is the flag; `fs_owner` makes it
 * recursive so a public entry point reached from another one (unlink ->
 * truncate) does not deadlock against itself. Waiters back off with
 * task_sleep_ticks(1) rather than spinning. Before proc_init() runs there
 * is exactly one thread of control and `current` is NULL, so the wait loop
 * must never be entered then -- hence the sched_active guard. */
static struct task *fs_owner;
static int fs_held;
static int fs_depth;

static void fs_lock(void)
{
    uint64_t f = irq_save();
    while (fs_held && fs_owner != current) {
        if (!sched_active)
            break;              /* no other task can exist yet */
        irq_restore(f);
        task_sleep_ticks(1);
        f = irq_save();
    }
    fs_held = 1;
    fs_owner = current;
    fs_depth++;
    irq_restore(f);
}

static void fs_unlock(void)
{
    uint64_t f = irq_save();
    if (--fs_depth <= 0) {
        fs_depth = 0;
        fs_held = 0;
        fs_owner = NULL;
    }
    irq_restore(f);
}

static uint8_t sb_block[KFS_BLOCK_SIZE];  /* raw block 0 for writeback */
static uint8_t bitmap_buf[KFS_BLOCK_SIZE]; /* balloc/bfree only */
static uint8_t inode_buf[KFS_BLOCK_SIZE];  /* iget/iput only */
static uint8_t data_buf[KFS_BLOCK_SIZE];   /* readi/writei data blocks */
static uint8_t ind_buf[KFS_BLOCK_SIZE];    /* indirect blocks (bmap, truncate) */
static uint8_t zero_buf[KFS_BLOCK_SIZE];   /* zeroing fresh blocks */

/* ---------- block I/O ----------
 * FS block numbers are relative to the start of the filesystem; the
 * instance turns them into device blocks.
 *
 * Every read goes through a small write-through cache. Without it the cost
 * of walking a directory is quadratic in its length *and* pays a polled
 * PIO transfer per step: listing a 73-entry /bin took 41 seconds, because
 * readdir(index) rescans from the first entry and each rescan re-read the
 * same handful of blocks. The working set for a path walk is tiny — the
 * bitmap block, an inode block, a directory block or two — so a handful of
 * entries removes essentially all of it.
 *
 * Write-through, never write-back: KFS has no journal and no flush on
 * shutdown, so a dirty cache would turn a reset into a corrupt filesystem.
 * The cache is only consulted with the filesystem lock held, which is what
 * makes it safe without any locking of its own. */

#define BCACHE_SLOTS 16

struct bcache_slot {
    struct kfs_fs *fs;              /* NULL when the slot is empty */
    uint32_t blk;
    uint64_t stamp;                 /* for least-recently-used eviction */
    uint8_t data[KFS_BLOCK_SIZE];
};

static struct bcache_slot bcache[BCACHE_SLOTS];
static uint64_t bcache_clock;
static uint64_t bcache_hits, bcache_misses;

static struct bcache_slot *bcache_find(struct kfs_fs *fs, uint32_t blk)
{
    for (int i = 0; i < BCACHE_SLOTS; i++)
        if (bcache[i].fs == fs && bcache[i].blk == blk)
            return &bcache[i];
    return NULL;
}

static struct bcache_slot *bcache_victim(void)
{
    struct bcache_slot *best = &bcache[0];

    for (int i = 0; i < BCACHE_SLOTS; i++) {
        if (bcache[i].fs == NULL)
            return &bcache[i];
        if (bcache[i].stamp < best->stamp)
            best = &bcache[i];
    }
    return best;
}

/* Drop every cached block belonging to `fs` (unmount, or a failed write
 * whose cached copy can no longer be trusted). NULL drops everything. */
static void bcache_invalidate(struct kfs_fs *fs)
{
    for (int i = 0; i < BCACHE_SLOTS; i++)
        if (fs == NULL || bcache[i].fs == fs)
            bcache[i].fs = NULL;
}

static int bread(struct kfs_fs *fs, uint32_t blk, void *buf)
{
    struct bcache_slot *s = bcache_find(fs, blk);

    if (s) {
        s->stamp = ++bcache_clock;
        memcpy(buf, s->data, KFS_BLOCK_SIZE);
        bcache_hits++;
        return 0;
    }

    if (blockdev_read(fs->bd, fs->start_lba + (uint64_t)blk * fs->spb,
                      fs->spb, buf) < 0)
        return -1;
    bcache_misses++;

    s = bcache_victim();
    s->fs = fs;
    s->blk = blk;
    s->stamp = ++bcache_clock;
    memcpy(s->data, buf, KFS_BLOCK_SIZE);
    return 0;
}

static int bwrite(struct kfs_fs *fs, uint32_t blk, const void *buf)
{
    struct bcache_slot *s = bcache_find(fs, blk);

    if (blockdev_write(fs->bd, fs->start_lba + (uint64_t)blk * fs->spb,
                       fs->spb, buf) < 0) {
        /* The device state is now unknown: forget the block rather than
         * serve a copy that may not match what is on the platter. */
        if (s)
            s->fs = NULL;
        return -1;
    }

    if (!s) {
        s = bcache_victim();
        s->fs = fs;
        s->blk = blk;
    }
    s->stamp = ++bcache_clock;
    memcpy(s->data, buf, KFS_BLOCK_SIZE);
    return 0;
}

void kfs_cache_stats(uint64_t *hits, uint64_t *misses)
{
    if (hits)
        *hits = bcache_hits;
    if (misses)
        *misses = bcache_misses;
}

static int sb_sync(struct kfs_fs *fs)
{
    memcpy(sb_block, &fs->sb, sizeof(fs->sb));
    return bwrite(fs, 0, sb_block);
}

/* ---------- block allocator ---------- */

/* Allocate a block, zero it on disk, return its number, or 0 on failure
 * (block 0 is the superblock, never allocatable). */
static uint32_t balloc(struct kfs_fs *fs)
{
    for (uint32_t bb = 0; bb < fs->sb.bitmap_blocks; bb++) {
        if (bread(fs, fs->sb.bitmap_start + bb, bitmap_buf) < 0)
            return 0;
        for (uint32_t i = 0; i < KFS_BLOCK_SIZE; i++) {
            if (bitmap_buf[i] == 0xFF)
                continue;
            for (int bit = 0; bit < 8; bit++) {
                if (bitmap_buf[i] & (1 << bit))
                    continue;
                uint32_t blk = bb * KFS_BLOCK_SIZE * 8 + i * 8 + bit;
                if (blk >= fs->sb.total_blocks)
                    return 0;
                bitmap_buf[i] |= (1 << bit);
                if (bwrite(fs, fs->sb.bitmap_start + bb, bitmap_buf) < 0)
                    return 0;
                fs->sb.free_blocks--;
                sb_sync(fs);
                memset(zero_buf, 0, KFS_BLOCK_SIZE);
                bwrite(fs, blk, zero_buf);
                return blk;
            }
        }
    }
    return 0;
}

static void bfree(struct kfs_fs *fs, uint32_t blk)
{
    if (blk < fs->sb.data_start || blk >= fs->sb.total_blocks) {
        kprintf("kfs: bfree of bad block %u\n", blk);
        return;
    }
    uint32_t bb = blk / (KFS_BLOCK_SIZE * 8);
    uint32_t i = (blk / 8) % KFS_BLOCK_SIZE;
    int bit = blk % 8;
    if (bread(fs, fs->sb.bitmap_start + bb, bitmap_buf) < 0)
        return;
    if (!(bitmap_buf[i] & (1 << bit))) {
        kprintf("kfs: double free of block %u\n", blk);
        return;
    }
    bitmap_buf[i] &= ~(1 << bit);
    bwrite(fs, fs->sb.bitmap_start + bb, bitmap_buf);
    fs->sb.free_blocks++;
    sb_sync(fs);
}

/* ---------- inodes ---------- */

static int ino_valid(struct kfs_fs *fs, uint32_t ino)
{
    return ino >= 1 && ino <= fs->sb.inode_count;
}

/* Unlocked: every caller inside this file already holds the FS lock. */
static int iget(struct kfs_fs *fs, uint32_t ino, struct kfs_inode *out)
{
    if (!fs->mounted || !ino_valid(fs, ino))
        return -1;
    uint32_t blk = fs->sb.inode_start + (ino - 1) / KFS_INODES_PER_BLOCK;
    uint32_t off = ((ino - 1) % KFS_INODES_PER_BLOCK) * sizeof(struct kfs_inode);
    if (bread(fs, blk, inode_buf) < 0)
        return -1;
    memcpy(out, inode_buf + off, sizeof(struct kfs_inode));
    return 0;
}

int kfs_iget(struct kfs_fs *fs, uint32_t ino, struct kfs_inode *out)
{
    if (fs == NULL || out == NULL)
        return -1;
    fs_lock();
    int r = iget(fs, ino, out);
    fs_unlock();
    return r;
}

static int iput(struct kfs_fs *fs, uint32_t ino, const struct kfs_inode *ip)
{
    uint32_t blk = fs->sb.inode_start + (ino - 1) / KFS_INODES_PER_BLOCK;
    uint32_t off = ((ino - 1) % KFS_INODES_PER_BLOCK) * sizeof(struct kfs_inode);
    if (bread(fs, blk, inode_buf) < 0)
        return -1;
    memcpy(inode_buf + off, ip, sizeof(struct kfs_inode));
    return bwrite(fs, blk, inode_buf);
}

/* Allocate a free inode slot, initialized to the given type and owner. */
static uint32_t ialloc(struct kfs_fs *fs, uint16_t type, uint16_t mode,
                       uint32_t uid, uint32_t gid, uint32_t mtime)
{
    struct kfs_inode ino;
    for (uint32_t i = 1; i <= fs->sb.inode_count; i++) {
        if (iget(fs, i, &ino) < 0)
            return 0;
        if (ino.type == KFS_TYPE_FREE) {
            memset(&ino, 0, sizeof(ino));
            ino.type = type;
            ino.mode = (uint16_t)(mode & KFS_MODE_MASK);
            ino.uid = uid;
            ino.gid = gid;
            ino.mtime = mtime;
            if (iput(fs, i, &ino) < 0)
                return 0;
            return i;
        }
    }
    return 0;
}

static void ifree(struct kfs_fs *fs, uint32_t ino)
{
    struct kfs_inode ip;
    memset(&ip, 0, sizeof(ip));
    iput(fs, ino, &ip);
}

/* ---------- block mapping ---------- */

/* Map file block `fbn` of inode `ino` (whose in-memory copy is *ip) to an
 * FS block, allocating if requested. Writes the inode / indirect block
 * back on any change. Returns the block number, or 0. */
static uint32_t bmap(struct kfs_fs *fs, uint32_t ino, struct kfs_inode *ip,
                     uint32_t fbn, int alloc)
{
    if (fbn < KFS_NDIRECT) {
        uint32_t b = ip->direct[fbn];
        if (!b && alloc) {
            b = balloc(fs);
            if (!b)
                return 0;
            ip->direct[fbn] = b;
            iput(fs, ino, ip);
        }
        return b;
    }

    fbn -= KFS_NDIRECT;
    if (fbn >= KFS_NINDIRECT)
        return 0;

    if (!ip->indirect) {
        if (!alloc)
            return 0;
        uint32_t nb = balloc(fs);
        if (!nb)
            return 0;
        ip->indirect = nb;
        iput(fs, ino, ip);
    }

    if (bread(fs, ip->indirect, ind_buf) < 0)
        return 0;
    uint32_t *tab = (uint32_t *)ind_buf;
    uint32_t b = tab[fbn];
    if (!b && alloc) {
        b = balloc(fs);
        if (!b)
            return 0;
        /* balloc does not touch ind_buf; the table is still valid. */
        tab[fbn] = b;
        if (bwrite(fs, ip->indirect, ind_buf) < 0)
            return 0;
    }
    return b;
}

/* ---------- file data I/O (no type checks) ---------- */

static long readi(struct kfs_fs *fs, uint32_t ino, uint32_t off, void *buf,
                  uint32_t n)
{
    struct kfs_inode ip;
    if (iget(fs, ino, &ip) < 0 || ip.type == KFS_TYPE_FREE)
        return -1;
    if (off >= ip.size)
        return 0;
    if (n > ip.size - off)
        n = ip.size - off;

    uint32_t done = 0;
    while (done < n) {
        uint32_t fbn = (off + done) / KFS_BLOCK_SIZE;
        uint32_t boff = (off + done) % KFS_BLOCK_SIZE;
        uint32_t chunk = KFS_BLOCK_SIZE - boff;
        if (chunk > n - done)
            chunk = n - done;
        uint32_t b = bmap(fs, ino, &ip, fbn, 0);
        if (!b || bread(fs, b, data_buf) < 0)
            return done ? (long)done : -1;
        memcpy((uint8_t *)buf + done, data_buf + boff, chunk);
        done += chunk;
    }
    return done;
}

static long writei(struct kfs_fs *fs, uint32_t ino, uint32_t off,
                   const void *buf, uint32_t n, uint32_t mtime)
{
    struct kfs_inode ip;
    if (iget(fs, ino, &ip) < 0 || ip.type == KFS_TYPE_FREE)
        return -1;
    if (n == 0)
        return 0;
    if (off > KFS_MAX_FILE_SIZE || n > KFS_MAX_FILE_SIZE - off)
        return -1;

    uint32_t done = 0;
    while (done < n) {
        uint32_t pos = off + done;
        uint32_t fbn = pos / KFS_BLOCK_SIZE;
        uint32_t boff = pos % KFS_BLOCK_SIZE;
        uint32_t chunk = KFS_BLOCK_SIZE - boff;
        if (chunk > n - done)
            chunk = n - done;
        uint32_t b = bmap(fs, ino, &ip, fbn, 1);
        if (!b)
            break;              /* disk full */
        if (chunk < KFS_BLOCK_SIZE) {
            if (bread(fs, b, data_buf) < 0)
                break;
        }
        memcpy(data_buf + boff, (const uint8_t *)buf + done, chunk);
        if (bwrite(fs, b, data_buf) < 0)
            break;
        done += chunk;
        if (pos + chunk > ip.size) {
            ip.size = pos + chunk;
            iput(fs, ino, &ip);
        }
    }
    if (done) {
        /* One final write-back so the timestamp lands even when the file
         * did not grow (an in-place overwrite). */
        ip.mtime = mtime;
        iput(fs, ino, &ip);
    }
    return done ? (long)done : -1;
}

long kfs_read(struct kfs_fs *fs, uint32_t ino, uint32_t off, void *buf,
              uint32_t n)
{
    if (fs == NULL || !fs->mounted || buf == NULL)
        return -1;
    fs_lock();
    long r = readi(fs, ino, off, buf, n);
    fs_unlock();
    return r;
}

long kfs_write(struct kfs_fs *fs, uint32_t ino, uint32_t off, const void *buf,
               uint32_t n, uint32_t mtime)
{
    struct kfs_inode ip;
    long r = -1;

    if (fs == NULL || !fs->mounted || buf == NULL)
        return -1;
    fs_lock();
    if (iget(fs, ino, &ip) == 0 && ip.type == KFS_TYPE_FILE)
        r = writei(fs, ino, off, buf, n, mtime);
    fs_unlock();
    return r;
}

/* ---------- directories ---------- */

/* Find `name` in directory `dir`. Returns the entry's ino (0 if absent).
 * If slot_out is non-NULL it receives the entry's byte offset. */
static uint32_t dir_lookup(struct kfs_fs *fs, uint32_t dir, const char *name,
                           uint32_t *slot_out)
{
    struct kfs_inode ip;
    struct kfs_dirent de;
    if (iget(fs, dir, &ip) < 0 || ip.type != KFS_TYPE_DIR)
        return 0;
    for (uint32_t off = 0; off + sizeof(de) <= ip.size; off += sizeof(de)) {
        if (readi(fs, dir, off, &de, sizeof(de)) != sizeof(de))
            return 0;
        if (de.ino != 0 && strncmp(de.name, name, sizeof(de.name)) == 0) {
            if (slot_out)
                *slot_out = off;
            return de.ino;
        }
    }
    return 0;
}

/* Add an entry, reusing a free slot or appending at the end. */
static int dir_add(struct kfs_fs *fs, uint32_t dir, const char *name,
                   uint32_t ino, uint32_t mtime)
{
    struct kfs_inode ip;
    struct kfs_dirent de;
    if (iget(fs, dir, &ip) < 0 || ip.type != KFS_TYPE_DIR)
        return -1;

    uint32_t slot = ip.size;
    for (uint32_t off = 0; off + sizeof(de) <= ip.size; off += sizeof(de)) {
        if (readi(fs, dir, off, &de, sizeof(de)) != sizeof(de))
            return -1;
        if (de.ino == 0) {
            slot = off;
            break;
        }
    }

    memset(&de, 0, sizeof(de));
    de.ino = ino;
    strncpy(de.name, name, sizeof(de.name) - 1);
    if (writei(fs, dir, slot, &de, sizeof(de), mtime) != sizeof(de))
        return -1;
    return 0;
}

static int dir_remove(struct kfs_fs *fs, uint32_t dir, const char *name,
                      uint32_t mtime)
{
    struct kfs_dirent de;
    uint32_t slot;
    if (dir_lookup(fs, dir, name, &slot) == 0)
        return -1;
    memset(&de, 0, sizeof(de));
    if (writei(fs, dir, slot, &de, sizeof(de), mtime) != sizeof(de))
        return -1;
    return 0;
}

/* Empty = nothing but "." and "..". */
static int dir_is_empty(struct kfs_fs *fs, uint32_t dir)
{
    struct kfs_inode ip;
    struct kfs_dirent de;
    if (iget(fs, dir, &ip) < 0)
        return 0;
    for (uint32_t off = 0; off + sizeof(de) <= ip.size; off += sizeof(de)) {
        if (readi(fs, dir, off, &de, sizeof(de)) != sizeof(de))
            return 0;
        if (de.ino == 0)
            continue;
        de.name[sizeof(de.name) - 1] = '\0';   /* on-disk name may be junk */
        if (strcmp(de.name, ".") != 0 && strcmp(de.name, "..") != 0)
            return 0;
    }
    return 1;
}

int kfs_lookup_in(struct kfs_fs *fs, uint32_t dir, const char *name)
{
    uint32_t ino;

    if (fs == NULL || !fs->mounted || name == NULL)
        return -1;
    fs_lock();
    ino = dir_lookup(fs, dir, name, NULL);
    fs_unlock();
    return ino ? (int)ino : -1;
}

/* ---------- path resolution ---------- */

/* Pull the next path component (max KFS_NAME_MAX chars) into name[60],
 * advancing *pp past it. Returns 1 on a component, 0 at end of path
 * (a trailing slash is tolerated), -1 on a too-long component. */
static int path_next(const char **pp, char *name)
{
    const char *p = *pp;
    while (*p == '/')
        p++;
    if (*p == '\0') {
        *pp = p;
        return 0;
    }
    int i = 0;
    while (*p && *p != '/') {
        if (i >= KFS_NAME_MAX)
            return -1;
        name[i++] = *p++;
    }
    name[i] = '\0';
    *pp = p;
    return 1;
}

/* Unlocked: callers hold the FS lock. */
static int lookup_path(struct kfs_fs *fs, const char *path)
{
    char name[60];
    if (!fs->mounted || path == NULL || path[0] != '/')
        return -1;

    uint32_t cur = fs->sb.root_ino;
    const char *p = path;
    int r;
    while ((r = path_next(&p, name)) == 1) {
        uint32_t next = dir_lookup(fs, cur, name, NULL);
        if (next == 0)
            return -1;
        cur = next;
    }
    if (r < 0)
        return -1;
    return (int)cur;
}

int kfs_lookup(struct kfs_fs *fs, const char *path)
{
    if (fs == NULL)
        return -1;
    fs_lock();
    int r = lookup_path(fs, path);
    fs_unlock();
    return r;
}

/* Resolve everything but the last component. Returns the parent dir ino
 * (or -1), and copies the final name into name[60]. */
static int lookup_parent(struct kfs_fs *fs, const char *path, char *name)
{
    char comp[60];
    if (!fs->mounted || path == NULL || path[0] != '/')
        return -1;

    uint32_t cur = fs->sb.root_ino;
    const char *p = path;
    if (path_next(&p, comp) != 1)
        return -1;              /* "/" itself has no parent entry */

    for (;;) {
        const char *peek = p;
        while (*peek == '/')
            peek++;
        if (*peek == '\0') {
            strcpy(name, comp);
            return (int)cur;
        }
        uint32_t nino = dir_lookup(fs, cur, comp, NULL);
        if (nino == 0)
            return -1;
        struct kfs_inode ip;
        if (iget(fs, nino, &ip) < 0 || ip.type != KFS_TYPE_DIR)
            return -1;
        cur = nino;
        if (path_next(&p, comp) != 1)
            return -1;
    }
}

/* ---------- public operations ----------
 * Each public entry point takes the whole-driver lock once and does
 * its work through the *_locked helpers, which never take it again. */

static int truncate_locked(struct kfs_fs *fs, uint32_t ino, uint32_t mtime);

static int create_locked(struct kfs_fs *fs, const char *path, uint16_t mode,
                         uint32_t uid, uint32_t gid, uint32_t mtime)
{
    char name[60];
    int parent = lookup_parent(fs, path, name);
    if (parent < 0)
        return -1;
    if (dir_lookup(fs, (uint32_t)parent, name, NULL) != 0)
        return -1;              /* already exists */
    uint32_t ino = ialloc(fs, KFS_TYPE_FILE, mode, uid, gid, mtime);
    if (!ino)
        return -1;
    if (dir_add(fs, (uint32_t)parent, name, ino, mtime) < 0) {
        ifree(fs, ino);
        return -1;
    }
    return (int)ino;
}

int kfs_create(struct kfs_fs *fs, const char *path, uint16_t mode,
               uint32_t uid, uint32_t gid, uint32_t mtime)
{
    if (fs == NULL)
        return -1;
    fs_lock();
    int r = create_locked(fs, path, mode, uid, gid, mtime);
    fs_unlock();
    return r;
}

static int mkdir_locked(struct kfs_fs *fs, const char *path, uint16_t mode,
                        uint32_t uid, uint32_t gid, uint32_t mtime)
{
    char name[60];
    int parent = lookup_parent(fs, path, name);
    if (parent < 0)
        return -1;
    if (dir_lookup(fs, (uint32_t)parent, name, NULL) != 0)
        return -1;
    uint32_t ino = ialloc(fs, KFS_TYPE_DIR, mode, uid, gid, mtime);
    if (!ino)
        return -1;
    if (dir_add(fs, ino, ".", ino, mtime) < 0 ||
        dir_add(fs, ino, "..", (uint32_t)parent, mtime) < 0 ||
        dir_add(fs, (uint32_t)parent, name, ino, mtime) < 0) {
        truncate_locked(fs, ino, mtime);
        ifree(fs, ino);
        return -1;
    }
    return 0;
}

int kfs_mkdir(struct kfs_fs *fs, const char *path, uint16_t mode, uint32_t uid,
              uint32_t gid, uint32_t mtime)
{
    if (fs == NULL)
        return -1;
    fs_lock();
    int r = mkdir_locked(fs, path, mode, uid, gid, mtime);
    fs_unlock();
    return r;
}

static int unlink_locked(struct kfs_fs *fs, const char *path, uint32_t mtime)
{
    char name[60];
    struct kfs_inode ip;

    int parent = lookup_parent(fs, path, name);
    if (parent < 0)
        return -1;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return -1;
    uint32_t ino = dir_lookup(fs, (uint32_t)parent, name, NULL);
    if (!ino || iget(fs, ino, &ip) < 0)
        return -1;
    if (ip.type == KFS_TYPE_DIR && !dir_is_empty(fs, ino))
        return -1;
    /* Detach the name first: a failing dir_remove() must never leave an
     * entry pointing at an inode slot that has already been freed. */
    if (dir_remove(fs, (uint32_t)parent, name, mtime) < 0)
        return -1;
    if (truncate_locked(fs, ino, mtime) < 0)
        return -1;
    ifree(fs, ino);
    return 0;
}

int kfs_unlink(struct kfs_fs *fs, const char *path, uint32_t mtime)
{
    if (fs == NULL)
        return -1;
    fs_lock();
    int r = unlink_locked(fs, path, mtime);
    fs_unlock();
    return r;
}

static int readdir_locked(struct kfs_fs *fs, uint32_t ino, int index,
                          struct kfs_dirent *out)
{
    struct kfs_inode ip;
    struct kfs_dirent de;
    if (iget(fs, ino, &ip) < 0 || ip.type != KFS_TYPE_DIR)
        return -1;
    int seen = 0;
    for (uint32_t off = 0; off + sizeof(de) <= ip.size; off += sizeof(de)) {
        if (readi(fs, ino, off, &de, sizeof(de)) != sizeof(de))
            return -1;
        de.name[sizeof(de.name) - 1] = '\0';   /* on-disk name may be junk */
        if (de.ino == 0)
            continue;
        if (seen == index) {
            memcpy(out, &de, sizeof(de));
            return 0;
        }
        seen++;
    }
    return -1;
}

int kfs_readdir(struct kfs_fs *fs, uint32_t ino, int index,
                struct kfs_dirent *out)
{
    if (fs == NULL || !fs->mounted || index < 0 || out == NULL)
        return -1;
    fs_lock();
    int r = readdir_locked(fs, ino, index, out);
    fs_unlock();
    return r;
}

static int truncate_locked(struct kfs_fs *fs, uint32_t ino, uint32_t mtime)
{
    struct kfs_inode ip;
    if (!fs->mounted || iget(fs, ino, &ip) < 0)
        return -1;

    for (int i = 0; i < KFS_NDIRECT; i++) {
        if (ip.direct[i]) {
            bfree(fs, ip.direct[i]);
            ip.direct[i] = 0;
        }
    }
    if (ip.indirect) {
        if (bread(fs, ip.indirect, ind_buf) == 0) {
            uint32_t *tab = (uint32_t *)ind_buf;
            for (uint32_t i = 0; i < KFS_NINDIRECT; i++) {
                if (tab[i])
                    bfree(fs, tab[i]);  /* bfree does not touch ind_buf */
            }
        }
        bfree(fs, ip.indirect);
        ip.indirect = 0;
    }
    ip.size = 0;
    ip.mtime = mtime;
    return iput(fs, ino, &ip);
}

int kfs_truncate(struct kfs_fs *fs, uint32_t ino, uint32_t mtime)
{
    if (fs == NULL)
        return -1;
    fs_lock();
    int r = truncate_locked(fs, ino, mtime);
    fs_unlock();
    return r;
}

int kfs_chmod(struct kfs_fs *fs, uint32_t ino, uint16_t mode, uint32_t mtime)
{
    struct kfs_inode ip;
    int r = -1;

    if (fs == NULL)
        return -1;
    fs_lock();
    if (fs->mounted && iget(fs, ino, &ip) == 0 && ip.type != KFS_TYPE_FREE) {
        ip.mode = (uint16_t)(mode & KFS_MODE_MASK);
        ip.mtime = mtime;
        r = iput(fs, ino, &ip);
    }
    fs_unlock();
    return r;
}

int kfs_chown(struct kfs_fs *fs, uint32_t ino, uint32_t uid, uint32_t gid,
              uint32_t mtime)
{
    struct kfs_inode ip;
    int r = -1;

    if (fs == NULL)
        return -1;
    fs_lock();
    if (fs->mounted && iget(fs, ino, &ip) == 0 && ip.type != KFS_TYPE_FREE) {
        ip.uid = uid;
        ip.gid = gid;
        ip.mtime = mtime;
        r = iput(fs, ino, &ip);
    }
    fs_unlock();
    return r;
}

/* ---------- mounting ---------- */

/* Sanity-check a superblock just read into fs->sb. Every field is used
 * unchecked as a block index or loop bound below, so a corrupt one must
 * be rejected here, not dereferenced. `verbose` suppresses the messages
 * while probing candidate offsets. */
static int sb_plausible(struct kfs_fs *fs, int verbose)
{
    struct kfs_superblock *sb = &fs->sb;

    if (sb->magic == KFS_MAGIC_V1) {
        /* A v1 inode has nlink where v2 has mode and 12 direct pointers
         * where v2 has 10: reading it as v2 would hand out garbage block
         * numbers. Refuse it by name instead. */
        if (verbose)
            kprintf("kfs: image is KFS v1, this kernel needs v2 "
                    "(rebuild with tools/mkfs.py)\n");
        return 0;
    }
    if (sb->magic != KFS_MAGIC) {
        if (verbose)
            kprintf("kfs: bad magic 0x%x (expected 0x%x)\n",
                    sb->magic, KFS_MAGIC);
        return 0;
    }
    if (sb->root_ino != KFS_ROOT_INO ||
        sb->bitmap_start == 0 ||
        sb->bitmap_blocks == 0 ||
        sb->bitmap_start + sb->bitmap_blocks > sb->inode_start ||
        sb->data_start <= sb->inode_start ||
        sb->inode_start <= sb->bitmap_start ||
        sb->inode_start + sb->inode_blocks > sb->data_start ||
        sb->data_start > sb->total_blocks ||
        sb->free_blocks > sb->total_blocks ||
        sb->inode_count == 0 ||
        sb->inode_count > sb->inode_blocks * KFS_INODES_PER_BLOCK) {
        if (verbose)
            kprintf("kfs: superblock is inconsistent\n");
        return 0;
    }
    /* The filesystem must fit inside the device it claims to live on. */
    uint64_t need = (uint64_t)sb->total_blocks * fs->spb;
    if (fs->start_lba + need > fs->bd->blocks) {
        if (verbose)
            kprintf("kfs: filesystem runs past the end of %s\n", fs->bd->name);
        return 0;
    }
    return 1;
}

/* Try to read a valid superblock at device block `lba`. Locked. */
static int try_offset(struct kfs_fs *fs, uint64_t lba, int verbose)
{
    fs->start_lba = lba;
    if (blockdev_read(fs->bd, lba, fs->spb, sb_block) < 0)
        return 0;
    memcpy(&fs->sb, sb_block, sizeof(fs->sb));
    return sb_plausible(fs, verbose);
}

static int kfs_type_mount(struct blockdev *bd, void **fs_priv)
{
    struct kfs_fs *fs = NULL;
    uint64_t part_lba;
    int ok;

    if (bd == NULL || fs_priv == NULL)
        return -1;
    if (bd->block_size == 0 || KFS_BLOCK_SIZE % bd->block_size != 0) {
        kprintf("kfs: %s has an unusable %u-byte block size\n",
                bd->name, bd->block_size);
        return -1;
    }

    uint64_t f = irq_save();
    for (int i = 0; i < KFS_MAX_MOUNTS; i++) {
        /* Two instances of one device would each keep their own copy of
         * the superblock and write both back, so the block bitmap would
         * diverge on the first allocation. Refuse instead. */
        if (instances[i].used && instances[i].mounted &&
            instances[i].bd == bd) {
            irq_restore(f);
            kprintf("kfs: %s is already mounted\n", bd->name);
            return -1;
        }
    }
    for (int i = 0; i < KFS_MAX_MOUNTS; i++) {
        if (!instances[i].used) {
            fs = &instances[i];
            memset(fs, 0, sizeof(*fs));
            fs->used = 1;
            break;
        }
    }
    irq_restore(f);
    if (fs == NULL) {
        kprintf("kfs: no free mount slot\n");
        return -1;
    }

    fs->bd = bd;
    fs->spb = KFS_BLOCK_SIZE / bd->block_size;

    /* The filesystem may be the whole device, or the boot disk's
     * partition. Probe quietly, then report against the offset that a
     * boot image is expected to use so a real failure still explains
     * itself. KFS_PART_LBA counts 512-byte sectors. */
    part_lba = (uint64_t)KFS_PART_LBA * KFS_SECTOR_SIZE / bd->block_size;

    fs_lock();
    ok = try_offset(fs, 0, 0);
    if (!ok && part_lba != 0)
        ok = try_offset(fs, part_lba, 0);
    if (!ok)
        ok = try_offset(fs, part_lba, 1);   /* again, this time loudly */
    if (ok)
        fs->mounted = 1;
    fs_unlock();

    if (!ok) {
        f = irq_save();
        fs->used = 0;
        irq_restore(f);
        return -1;
    }

    kprintf("kfs: mounted v2 on %s at lba %lu, %u blocks (%u free), "
            "%u inodes\n", bd->name, (unsigned long)fs->start_lba,
            fs->sb.total_blocks, fs->sb.free_blocks, fs->sb.inode_count);
    *fs_priv = fs;
    return 0;
}

/* Retire an instance. Handles still open keep the slot reserved (their
 * reads and writes now fail, since `mounted` is clear) until the last
 * close releases it, so a struct file never points at a recycled slot. */
static void instance_release(struct kfs_fs *fs)
{
    uint64_t f = irq_save();
    if (!fs->mounted && fs->handles == 0)
        fs->used = 0;
    irq_restore(f);
}

static void kfs_type_unmount(void *fs_priv)
{
    struct kfs_fs *fs = fs_priv;

    if (fs == NULL)
        return;
    fs_lock();
    fs->mounted = 0;
    bcache_invalidate(fs);       /* the slot may be reused by another mount */
    fs_unlock();
    instance_release(fs);
}

/* ---------- open-inode table ----------
 * A struct file holds a bare inum, and KFS hands a freed inode slot
 * straight back out to the next create. Unlinking an inode that still has
 * open handles would therefore let an old fd read and write a different
 * file, so a referenced inode is not removable. Keyed by (instance, inum)
 * so two mounts cannot pin each other's inodes. */

#define KFS_OPEN_INODES 64

static struct {
    struct kfs_fs *fs;
    uint32_t inum;
    int count;
} open_inodes[KFS_OPEN_INODES];

static int inode_ref(struct kfs_fs *fs, uint32_t inum)
{
    int slot = -1;
    uint64_t f = irq_save();
    for (int i = 0; i < KFS_OPEN_INODES; i++) {
        if (open_inodes[i].count > 0 && open_inodes[i].fs == fs &&
            open_inodes[i].inum == inum) {
            open_inodes[i].count++;
            fs->handles++;
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
    open_inodes[slot].fs = fs;
    open_inodes[slot].inum = inum;
    open_inodes[slot].count = 1;
    fs->handles++;
    irq_restore(f);
    return 0;
}

static void inode_unref(struct kfs_fs *fs, uint32_t inum)
{
    uint64_t f = irq_save();
    for (int i = 0; i < KFS_OPEN_INODES; i++) {
        if (open_inodes[i].count > 0 && open_inodes[i].fs == fs &&
            open_inodes[i].inum == inum) {
            open_inodes[i].count--;
            break;
        }
    }
    if (fs->handles > 0)
        fs->handles--;
    irq_restore(f);
}

static int inode_is_open(struct kfs_fs *fs, uint32_t inum)
{
    int open = 0;
    uint64_t f = irq_save();
    for (int i = 0; i < KFS_OPEN_INODES; i++) {
        if (open_inodes[i].count > 0 && open_inodes[i].fs == fs &&
            open_inodes[i].inum == inum) {
            open = 1;
            break;
        }
    }
    irq_restore(f);
    return open;
}

/* ---------- the fs_ops layer ----------
 * This is where access control is applied. KFS below enforces nothing;
 * every check here compares the calling task's uid/gid against the inode
 * through vfs_perm_ok(), the single copy of the rule. A path is only
 * usable if the caller has search (x) permission on every directory
 * component, which is why resolve() walks the path one name at a time
 * instead of handing the whole string to kfs_lookup(). */

/* A KFS handle. The struct file must stay first: the VFS casts between
 * the two. Keeping the instance in the handle (rather than reaching
 * through f->mnt) means an open fd survives its mount point going away. */
struct kfs_file {
    struct file f;
    struct kfs_fs *fs;
};

static int perm_ok(const struct kfs_inode *ip, int want)
{
    return vfs_perm_ok(ip->mode & KFS_MODE_MASK, ip->uid, ip->gid, want);
}

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

/* Walk `path` from the filesystem root, requiring search (x) permission
 * on every directory crossed -- including the root and, when want_parent
 * is set, the parent itself.
 *
 * want_parent = 0: returns the inode number of `path`.
 * want_parent = 1: stops one component short, returns the parent
 *                  directory's inode and copies the last component into
 *                  last[] (which must hold KFS_NAME_MAX + 1 bytes).
 * Returns -1 for "not found", "not a directory" and "permission denied"
 * alike; the ABI has no errno to tell them apart.
 *
 * Deliberately built from the locked kfs_* entry points rather than the
 * internal helpers: the FS lock is taken and dropped per step, never held
 * across the whole walk. */
static int resolve(struct kfs_fs *fs, const char *path, int want_parent,
                   char *last)
{
    struct kfs_inode ip;
    char comp[KFS_NAME_MAX + 1];
    uint32_t cur;
    const char *p = path;
    int r;

    if (fs == NULL || !fs->mounted || path == NULL || path[0] != '/')
        return -1;
    cur = fs->sb.root_ino;

    while ((r = comp_next(&p, comp)) == 1) {
        if (kfs_iget(fs, cur, &ip) < 0 || ip.type != KFS_TYPE_DIR)
            return -1;
        if (!perm_ok(&ip, VFS_X))
            return -1;
        if (want_parent && at_end(p)) {
            strcpy(last, comp);
            return (int)cur;
        }
        int next = kfs_lookup_in(fs, cur, comp);
        if (next < 0)
            return -1;
        cur = (uint32_t)next;
    }
    if (r < 0)
        return -1;
    if (want_parent)
        return -1;              /* the root has no parent entry */
    return (int)cur;
}

static struct kfs_fs *mount_fs(struct mount *m)
{
    return m ? (struct kfs_fs *)m->priv : NULL;
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

static struct file *kfs_op_open(struct mount *m, const char *path, int flags)
{
    struct kfs_fs *fs = mount_fs(m);
    struct kfs_inode ip, dirp;
    char name[KFS_NAME_MAX + 1];
    int parent, ino;
    int need_r, need_w;

    if (fs == NULL || !fs->mounted || path == NULL)
        return NULL;

    need_r = vfs_flags_allow_read(flags);
    /* O_CREAT and O_TRUNC both modify the object, so both demand write
     * permission even when the access mode alone would not. */
    need_w = vfs_flags_allow_write(flags) ||
             (flags & (O_CREAT | O_TRUNC)) != 0;

    parent = resolve(fs, path, 1, name);
    if (parent < 0 || kfs_iget(fs, (uint32_t)parent, &dirp) < 0)
        return NULL;

    ino = kfs_lookup_in(fs, (uint32_t)parent, name);
    if (ino < 0) {
        if (!(flags & O_CREAT))
            return NULL;
        /* Creating an entry writes the parent directory. */
        if (!perm_ok(&dirp, VFS_W | VFS_X))
            return NULL;
        ino = kfs_create(fs, path, KFS_DEFAULT_FILE_MODE, vfs_uid(), vfs_gid(),
                         vfs_now());
        if (ino < 0)
            return NULL;
    }
    if (kfs_iget(fs, (uint32_t)ino, &ip) < 0)
        return NULL;
    if (ip.type == KFS_TYPE_DIR && need_w)
        return NULL;            /* directories are read-only via the VFS */
    if (need_r && !perm_ok(&ip, VFS_R))
        return NULL;
    if (need_w && !perm_ok(&ip, VFS_W))
        return NULL;

    if (inode_ref(fs, (uint32_t)ino) < 0)
        return NULL;

    if ((flags & O_TRUNC) && ip.type == KFS_TYPE_FILE) {
        if (kfs_truncate(fs, (uint32_t)ino, vfs_now()) < 0) {
            inode_unref(fs, (uint32_t)ino);
            return NULL;
        }
    }

    struct kfs_file *kf = kzalloc(sizeof(struct kfs_file));
    if (kf == NULL) {
        inode_unref(fs, (uint32_t)ino);
        return NULL;
    }
    kf->fs = fs;
    kf->f.type = FILE_KFS;
    kf->f.inum = (uint32_t)ino;
    kf->f.pos = 0;
    kf->f.flags = flags;
    kf->f.refs = 1;
    return &kf->f;
}

static void kfs_op_close(struct file *f)
{
    struct kfs_file *kf = (struct kfs_file *)f;
    struct kfs_fs *fs;

    if (kf == NULL)
        return;
    fs = kf->fs;
    inode_unref(fs, kf->f.inum);
    kfree(kf);
    instance_release(fs);
}

static long kfs_op_read(struct file *f, void *buf, unsigned long n)
{
    struct kfs_file *kf = (struct kfs_file *)f;
    long r = kfs_read(kf->fs, kf->f.inum, kf->f.pos, buf, (uint32_t)n);

    if (r > 0)
        kf->f.pos += (uint32_t)r;
    return r;
}

static long kfs_op_write(struct file *f, const void *buf, unsigned long n)
{
    struct kfs_file *kf = (struct kfs_file *)f;
    struct kfs_inode ip;
    long r;

    if (kf->f.flags & O_APPEND) {
        if (kfs_iget(kf->fs, kf->f.inum, &ip) < 0)
            return -1;
        kf->f.pos = ip.size;
    }
    r = kfs_write(kf->fs, kf->f.inum, kf->f.pos, buf, (uint32_t)n, vfs_now());
    if (r > 0)
        kf->f.pos += (uint32_t)r;
    return r;
}

static long kfs_op_seek(struct file *f, long off, int whence)
{
    struct kfs_file *kf = (struct kfs_file *)f;
    struct kfs_inode ip;
    long base, pos;

    switch (whence) {
    case 0:                     /* SEEK_SET */
        base = 0;
        break;
    case 1:                     /* SEEK_CUR */
        base = (long)kf->f.pos;
        break;
    case 2:                     /* SEEK_END */
        if (kfs_iget(kf->fs, kf->f.inum, &ip) < 0)
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
    pos = base + off;
    if (pos < 0 || pos > (long)KFS_MAX_FILE_SIZE)
        return -1;
    kf->f.pos = (uint32_t)pos;
    return pos;
}

static int kfs_op_stat(struct mount *m, const char *path, struct k_stat *st)
{
    struct kfs_fs *fs = mount_fs(m);
    struct kfs_inode ip;
    int ino;

    if (fs == NULL || path == NULL || st == NULL)
        return -1;
    ino = resolve(fs, path, 0, NULL);
    if (ino < 0 || kfs_iget(fs, (uint32_t)ino, &ip) < 0)
        return -1;
    fill_stat(&ip, st);
    return 0;
}

static int kfs_op_readdir(struct mount *m, const char *path, int index,
                          struct k_dirent *de)
{
    struct kfs_fs *fs = mount_fs(m);
    struct kfs_dirent kde;
    struct kfs_inode ip;
    int ino;

    if (fs == NULL || path == NULL || de == NULL)
        return -1;
    ino = resolve(fs, path, 0, NULL);
    if (ino < 0 || kfs_iget(fs, (uint32_t)ino, &ip) < 0)
        return -1;
    if (ip.type != KFS_TYPE_DIR || !perm_ok(&ip, VFS_R))
        return -1;              /* listing a directory reads it */
    if (kfs_readdir(fs, (uint32_t)ino, index, &kde) < 0)
        return -1;

    memcpy(de->name, kde.name, sizeof(de->name));
    de->name[sizeof(de->name) - 1] = '\0';
    de->size = 0;
    de->is_dir = 0;
    de->mode = 0;
    de->uid = 0;
    de->gid = 0;
    de->mtime = 0;
    if (kfs_iget(fs, kde.ino, &ip) == 0) {
        de->size = ip.size;
        de->is_dir = (ip.type == KFS_TYPE_DIR);
        de->mode = ip.mode & KFS_MODE_MASK;
        de->uid = ip.uid;
        de->gid = ip.gid;
        de->mtime = ip.mtime;
    }
    return 0;
}

static int kfs_op_unlink(struct mount *m, const char *path)
{
    struct kfs_fs *fs = mount_fs(m);
    struct kfs_inode dirp;
    char name[KFS_NAME_MAX + 1];
    int parent, ino;

    if (fs == NULL || path == NULL)
        return -1;
    parent = resolve(fs, path, 1, name);
    if (parent < 0 || kfs_iget(fs, (uint32_t)parent, &dirp) < 0)
        return -1;
    if (!perm_ok(&dirp, VFS_W | VFS_X))
        return -1;
    ino = kfs_lookup_in(fs, (uint32_t)parent, name);
    if (ino < 0)
        return -1;
    if (inode_is_open(fs, (uint32_t)ino))
        return -1;              /* still open: the slot must not be reused */
    return kfs_unlink(fs, path, vfs_now());
}

static int kfs_op_mkdir(struct mount *m, const char *path)
{
    struct kfs_fs *fs = mount_fs(m);
    struct kfs_inode dirp;
    char name[KFS_NAME_MAX + 1];
    int parent;

    if (fs == NULL || path == NULL)
        return -1;
    parent = resolve(fs, path, 1, name);
    if (parent < 0 || kfs_iget(fs, (uint32_t)parent, &dirp) < 0)
        return -1;
    if (!perm_ok(&dirp, VFS_W | VFS_X))
        return -1;
    return kfs_mkdir(fs, path, KFS_DEFAULT_DIR_MODE, vfs_uid(), vfs_gid(),
                     vfs_now());
}

static int kfs_op_chmod(struct mount *m, const char *path, uint32_t mode)
{
    struct kfs_fs *fs = mount_fs(m);
    struct kfs_inode ip;
    int ino;

    if (fs == NULL || path == NULL)
        return -1;
    ino = resolve(fs, path, 0, NULL);
    if (ino < 0 || kfs_iget(fs, (uint32_t)ino, &ip) < 0)
        return -1;
    /* Only the owner or root may re-permission a file. */
    if (vfs_uid() != 0 && vfs_uid() != ip.uid)
        return -1;
    return kfs_chmod(fs, (uint32_t)ino, (uint16_t)(mode & KFS_MODE_MASK),
                     vfs_now());
}

static int kfs_op_chown(struct mount *m, const char *path, uint32_t uid,
                        uint32_t gid)
{
    struct kfs_fs *fs = mount_fs(m);
    struct kfs_inode ip;
    int ino;

    if (fs == NULL || path == NULL)
        return -1;
    if (vfs_uid() != 0)
        return -1;              /* giving a file away is root-only */
    ino = resolve(fs, path, 0, NULL);
    if (ino < 0 || kfs_iget(fs, (uint32_t)ino, &ip) < 0)
        return -1;
    return kfs_chown(fs, (uint32_t)ino, uid, gid, vfs_now());
}

/* Free inode slots are counted by sweeping the inode table a block at a
 * time; there is no free-inode counter on disk. df is rare enough that
 * 16 block reads is a fair price for an honest number. */
static int kfs_op_statfs(struct mount *m, struct fs_statfs *sf)
{
    struct kfs_fs *fs = mount_fs(m);
    uint32_t free_ino = 0;

    if (fs == NULL || sf == NULL)
        return -1;

    fs_lock();
    if (!fs->mounted) {
        fs_unlock();
        return -1;
    }
    for (uint32_t i = 0; i < fs->sb.inode_count; i += KFS_INODES_PER_BLOCK) {
        uint32_t blk = fs->sb.inode_start + i / KFS_INODES_PER_BLOCK;
        uint32_t here = fs->sb.inode_count - i;
        if (here > KFS_INODES_PER_BLOCK)
            here = KFS_INODES_PER_BLOCK;
        if (bread(fs, blk, inode_buf) < 0)
            break;
        for (uint32_t j = 0; j < here; j++) {
            const struct kfs_inode *ip =
                (const struct kfs_inode *)(inode_buf + j * sizeof(*ip));
            if (ip->type == KFS_TYPE_FREE)
                free_ino++;
        }
    }
    sf->block_size = KFS_BLOCK_SIZE;
    sf->blocks = fs->sb.total_blocks;
    sf->free_blocks = fs->sb.free_blocks;
    sf->files = fs->sb.inode_count;
    sf->free_files = free_ino;
    fs_unlock();
    return 0;
}

static struct fs_ops kfs_ops = {
    .open    = kfs_op_open,
    .close   = kfs_op_close,
    .read    = kfs_op_read,
    .write   = kfs_op_write,
    .seek    = kfs_op_seek,
    .stat    = kfs_op_stat,
    .readdir = kfs_op_readdir,
    .unlink  = kfs_op_unlink,
    .mkdir   = kfs_op_mkdir,
    .chmod   = kfs_op_chmod,
    .chown   = kfs_op_chown,
    .statfs  = kfs_op_statfs,
};

static struct fs_type kfs_type = {
    .name    = "kfs",
    .mount   = kfs_type_mount,
    .unmount = kfs_type_unmount,
    .ops     = &kfs_ops,
};

void kfs_init(void)
{
    static int registered;

    if (registered)
        return;
    if (fs_register(&kfs_type) == 0)
        registered = 1;
}
