#include "kernel.h"
#include "kfs.h"
#include "ata.h"
#include "proc.h"
#include "string.h"

/* KFS driver. Write-through: every metadata update goes straight to disk.
 * A handful of static scratch blocks stand in for a buffer cache; they are
 * carefully assigned so no code path aliases two uses of the same buffer
 * (see comments at each declaration).
 *
 * Those buffers, the superblock and the singleton ATA controller are all
 * shared state, but syscalls run preemptible (syscall.c enables interrupts
 * on entry), so every public entry point takes a whole-filesystem lock.
 * Without it two tasks interleaved inside balloc()/writei() double-allocate
 * blocks and cross-write each other's file data. */

static struct kfs_superblock sb;
static int kfs_mounted;

/* Recursive: kfs_unlink()/kfs_mkdir() call the public kfs_truncate(). */
static struct task *fs_owner;
static int fs_depth;

static void fs_lock(void)
{
    uint64_t f = irq_save();
    while (fs_owner && fs_owner != current) {
        irq_restore(f);
        yield();
        f = irq_save();
    }
    fs_owner = current;
    fs_depth++;
    irq_restore(f);
}

static void fs_unlock(void)
{
    uint64_t f = irq_save();
    if (--fs_depth == 0)
        fs_owner = NULL;
    irq_restore(f);
}

static uint8_t sb_block[KFS_BLOCK_SIZE];  /* raw block 0 for writeback */
static uint8_t bitmap_buf[KFS_BLOCK_SIZE]; /* balloc/bfree only */
static uint8_t inode_buf[KFS_BLOCK_SIZE];  /* iget/iput only */
static uint8_t data_buf[KFS_BLOCK_SIZE];   /* readi/writei data blocks */
static uint8_t ind_buf[KFS_BLOCK_SIZE];    /* indirect blocks (bmap, truncate) */
static uint8_t zero_buf[KFS_BLOCK_SIZE];   /* zeroing fresh blocks */

/* ---------- block I/O ---------- */

static int bread(uint32_t blk, void *buf)
{
    return ata_read(FS_START_LBA + blk * KFS_SECTORS_PER_BLOCK,
                    KFS_SECTORS_PER_BLOCK, buf);
}

static int bwrite(uint32_t blk, const void *buf)
{
    return ata_write(FS_START_LBA + blk * KFS_SECTORS_PER_BLOCK,
                     KFS_SECTORS_PER_BLOCK, buf);
}

static int sb_sync(void)
{
    memcpy(sb_block, &sb, sizeof(sb));
    return bwrite(0, sb_block);
}

/* ---------- block allocator ---------- */

/* Allocate a block, zero it on disk, return its number, or 0 on failure
 * (block 0 is the superblock, never allocatable). */
static uint32_t balloc(void)
{
    for (uint32_t bb = 0; bb < sb.bitmap_blocks; bb++) {
        if (bread(sb.bitmap_start + bb, bitmap_buf) < 0)
            return 0;
        for (uint32_t i = 0; i < KFS_BLOCK_SIZE; i++) {
            if (bitmap_buf[i] == 0xFF)
                continue;
            for (int bit = 0; bit < 8; bit++) {
                if (bitmap_buf[i] & (1 << bit))
                    continue;
                uint32_t blk = bb * KFS_BLOCK_SIZE * 8 + i * 8 + bit;
                if (blk >= sb.total_blocks)
                    return 0;
                bitmap_buf[i] |= (1 << bit);
                if (bwrite(sb.bitmap_start + bb, bitmap_buf) < 0)
                    return 0;
                sb.free_blocks--;
                sb_sync();
                memset(zero_buf, 0, KFS_BLOCK_SIZE);
                bwrite(blk, zero_buf);
                return blk;
            }
        }
    }
    return 0;
}

static void bfree(uint32_t blk)
{
    if (blk < sb.data_start || blk >= sb.total_blocks) {
        kprintf("kfs: bfree of bad block %u\n", blk);
        return;
    }
    uint32_t bb = blk / (KFS_BLOCK_SIZE * 8);
    uint32_t i = (blk / 8) % KFS_BLOCK_SIZE;
    int bit = blk % 8;
    if (bread(sb.bitmap_start + bb, bitmap_buf) < 0)
        return;
    if (!(bitmap_buf[i] & (1 << bit))) {
        kprintf("kfs: double free of block %u\n", blk);
        return;
    }
    bitmap_buf[i] &= ~(1 << bit);
    bwrite(sb.bitmap_start + bb, bitmap_buf);
    sb.free_blocks++;
    sb_sync();
}

/* ---------- inodes ---------- */

static int ino_valid(uint32_t ino)
{
    return ino >= 1 && ino <= sb.inode_count;
}

/* Unlocked: every caller inside this file already holds the FS lock. */
static int iget(uint32_t ino, struct kfs_inode *out)
{
    if (!kfs_mounted || !ino_valid(ino))
        return -1;
    uint32_t blk = sb.inode_start + (ino - 1) / KFS_INODES_PER_BLOCK;
    uint32_t off = ((ino - 1) % KFS_INODES_PER_BLOCK) * sizeof(struct kfs_inode);
    if (bread(blk, inode_buf) < 0)
        return -1;
    memcpy(out, inode_buf + off, sizeof(struct kfs_inode));
    return 0;
}

int kfs_iget(uint32_t ino, struct kfs_inode *out)
{
    if (out == NULL)
        return -1;
    fs_lock();
    int r = iget(ino, out);
    fs_unlock();
    return r;
}

static int iput(uint32_t ino, const struct kfs_inode *ip)
{
    uint32_t blk = sb.inode_start + (ino - 1) / KFS_INODES_PER_BLOCK;
    uint32_t off = ((ino - 1) % KFS_INODES_PER_BLOCK) * sizeof(struct kfs_inode);
    if (bread(blk, inode_buf) < 0)
        return -1;
    memcpy(inode_buf + off, ip, sizeof(struct kfs_inode));
    return bwrite(blk, inode_buf);
}

/* Allocate a free inode slot, initialized to the given type. */
static uint32_t ialloc(uint16_t type)
{
    struct kfs_inode ino;
    for (uint32_t i = 1; i <= sb.inode_count; i++) {
        if (iget(i, &ino) < 0)
            return 0;
        if (ino.type == KFS_TYPE_FREE) {
            memset(&ino, 0, sizeof(ino));
            ino.type = type;
            ino.nlink = (type == KFS_TYPE_DIR) ? 2 : 1;
            if (iput(i, &ino) < 0)
                return 0;
            return i;
        }
    }
    return 0;
}

static void ifree(uint32_t ino)
{
    struct kfs_inode ip;
    memset(&ip, 0, sizeof(ip));
    iput(ino, &ip);
}

/* ---------- block mapping ---------- */

/* Map file block `fbn` of inode `ino` (whose in-memory copy is *ip) to an
 * FS block, allocating if requested. Writes the inode / indirect block
 * back on any change. Returns the block number, or 0. */
static uint32_t bmap(uint32_t ino, struct kfs_inode *ip, uint32_t fbn, int alloc)
{
    if (fbn < KFS_NDIRECT) {
        uint32_t b = ip->direct[fbn];
        if (!b && alloc) {
            b = balloc();
            if (!b)
                return 0;
            ip->direct[fbn] = b;
            iput(ino, ip);
        }
        return b;
    }

    fbn -= KFS_NDIRECT;
    if (fbn >= KFS_NINDIRECT)
        return 0;

    if (!ip->indirect) {
        if (!alloc)
            return 0;
        uint32_t nb = balloc();
        if (!nb)
            return 0;
        ip->indirect = nb;
        iput(ino, ip);
    }

    if (bread(ip->indirect, ind_buf) < 0)
        return 0;
    uint32_t *tab = (uint32_t *)ind_buf;
    uint32_t b = tab[fbn];
    if (!b && alloc) {
        b = balloc();
        if (!b)
            return 0;
        /* balloc does not touch ind_buf; the table is still valid. */
        tab[fbn] = b;
        if (bwrite(ip->indirect, ind_buf) < 0)
            return 0;
    }
    return b;
}

/* ---------- file data I/O (no type checks) ---------- */

static long readi(uint32_t ino, uint32_t off, void *buf, uint32_t n)
{
    struct kfs_inode ip;
    if (iget(ino, &ip) < 0 || ip.type == KFS_TYPE_FREE)
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
        uint32_t b = bmap(ino, &ip, fbn, 0);
        if (!b || bread(b, data_buf) < 0)
            return done ? (long)done : -1;
        memcpy((uint8_t *)buf + done, data_buf + boff, chunk);
        done += chunk;
    }
    return done;
}

static long writei(uint32_t ino, uint32_t off, const void *buf, uint32_t n)
{
    struct kfs_inode ip;
    if (iget(ino, &ip) < 0 || ip.type == KFS_TYPE_FREE)
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
        uint32_t b = bmap(ino, &ip, fbn, 1);
        if (!b)
            break;              /* disk full */
        if (chunk < KFS_BLOCK_SIZE) {
            if (bread(b, data_buf) < 0)
                break;
        }
        memcpy(data_buf + boff, (const uint8_t *)buf + done, chunk);
        if (bwrite(b, data_buf) < 0)
            break;
        done += chunk;
        if (pos + chunk > ip.size) {
            ip.size = pos + chunk;
            iput(ino, &ip);
        }
    }
    return done ? (long)done : -1;
}

long kfs_read(uint32_t ino, uint32_t off, void *buf, uint32_t n)
{
    if (!kfs_mounted || buf == NULL)
        return -1;
    fs_lock();
    long r = readi(ino, off, buf, n);
    fs_unlock();
    return r;
}

long kfs_write(uint32_t ino, uint32_t off, const void *buf, uint32_t n)
{
    struct kfs_inode ip;
    long r = -1;

    if (!kfs_mounted || buf == NULL)
        return -1;
    fs_lock();
    if (iget(ino, &ip) == 0 && ip.type == KFS_TYPE_FILE)
        r = writei(ino, off, buf, n);
    fs_unlock();
    return r;
}

/* ---------- directories ---------- */

/* Find `name` in directory `dir`. Returns the entry's ino (0 if absent).
 * If slot_out is non-NULL it receives the entry's byte offset. */
static uint32_t dir_lookup(uint32_t dir, const char *name, uint32_t *slot_out)
{
    struct kfs_inode ip;
    struct kfs_dirent de;
    if (iget(dir, &ip) < 0 || ip.type != KFS_TYPE_DIR)
        return 0;
    for (uint32_t off = 0; off + sizeof(de) <= ip.size; off += sizeof(de)) {
        if (readi(dir, off, &de, sizeof(de)) != sizeof(de))
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
static int dir_add(uint32_t dir, const char *name, uint32_t ino)
{
    struct kfs_inode ip;
    struct kfs_dirent de;
    if (iget(dir, &ip) < 0 || ip.type != KFS_TYPE_DIR)
        return -1;

    uint32_t slot = ip.size;
    for (uint32_t off = 0; off + sizeof(de) <= ip.size; off += sizeof(de)) {
        if (readi(dir, off, &de, sizeof(de)) != sizeof(de))
            return -1;
        if (de.ino == 0) {
            slot = off;
            break;
        }
    }

    memset(&de, 0, sizeof(de));
    de.ino = ino;
    strncpy(de.name, name, sizeof(de.name) - 1);
    if (writei(dir, slot, &de, sizeof(de)) != sizeof(de))
        return -1;
    return 0;
}

static int dir_remove(uint32_t dir, const char *name)
{
    struct kfs_dirent de;
    uint32_t slot;
    if (dir_lookup(dir, name, &slot) == 0)
        return -1;
    memset(&de, 0, sizeof(de));
    if (writei(dir, slot, &de, sizeof(de)) != sizeof(de))
        return -1;
    return 0;
}

/* Empty = nothing but "." and "..". */
static int dir_is_empty(uint32_t dir)
{
    struct kfs_inode ip;
    struct kfs_dirent de;
    if (iget(dir, &ip) < 0)
        return 0;
    for (uint32_t off = 0; off + sizeof(de) <= ip.size; off += sizeof(de)) {
        if (readi(dir, off, &de, sizeof(de)) != sizeof(de))
            return 0;
        if (de.ino == 0)
            continue;
        de.name[sizeof(de.name) - 1] = '\0';   /* on-disk name may be junk */
        if (strcmp(de.name, ".") != 0 && strcmp(de.name, "..") != 0)
            return 0;
    }
    return 1;
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
static int lookup_path(const char *path)
{
    char name[60];
    if (!kfs_mounted || path == NULL || path[0] != '/')
        return -1;

    uint32_t cur = sb.root_ino;
    const char *p = path;
    int r;
    while ((r = path_next(&p, name)) == 1) {
        uint32_t next = dir_lookup(cur, name, NULL);
        if (next == 0)
            return -1;
        cur = next;
    }
    if (r < 0)
        return -1;
    return (int)cur;
}

int kfs_lookup(const char *path)
{
    fs_lock();
    int r = lookup_path(path);
    fs_unlock();
    return r;
}

/* Resolve everything but the last component. Returns the parent dir ino
 * (or -1), and copies the final name into name[60]. */
static int lookup_parent(const char *path, char *name)
{
    char comp[60];
    if (!kfs_mounted || path == NULL || path[0] != '/')
        return -1;

    uint32_t cur = sb.root_ino;
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
        uint32_t nino = dir_lookup(cur, comp, NULL);
        if (nino == 0)
            return -1;
        struct kfs_inode ip;
        if (iget(nino, &ip) < 0 || ip.type != KFS_TYPE_DIR)
            return -1;
        cur = nino;
        if (path_next(&p, comp) != 1)
            return -1;
    }
}

/* ---------- public operations ----------
 * Each public entry point takes the whole-filesystem lock once and does
 * its work through the *_locked helpers, which never take it again. */

static int truncate_locked(uint32_t ino);

static int create_locked(const char *path)
{
    char name[60];
    int parent = lookup_parent(path, name);
    if (parent < 0)
        return -1;
    if (dir_lookup((uint32_t)parent, name, NULL) != 0)
        return -1;              /* already exists */
    uint32_t ino = ialloc(KFS_TYPE_FILE);
    if (!ino)
        return -1;
    if (dir_add((uint32_t)parent, name, ino) < 0) {
        ifree(ino);
        return -1;
    }
    return (int)ino;
}

int kfs_create(const char *path)
{
    fs_lock();
    int r = create_locked(path);
    fs_unlock();
    return r;
}

static int mkdir_locked(const char *path)
{
    char name[60];
    int parent = lookup_parent(path, name);
    if (parent < 0)
        return -1;
    if (dir_lookup((uint32_t)parent, name, NULL) != 0)
        return -1;
    uint32_t ino = ialloc(KFS_TYPE_DIR);
    if (!ino)
        return -1;
    if (dir_add(ino, ".", ino) < 0 ||
        dir_add(ino, "..", (uint32_t)parent) < 0 ||
        dir_add((uint32_t)parent, name, ino) < 0) {
        truncate_locked(ino);
        ifree(ino);
        return -1;
    }
    return 0;
}

int kfs_mkdir(const char *path)
{
    fs_lock();
    int r = mkdir_locked(path);
    fs_unlock();
    return r;
}

static int unlink_locked(const char *path)
{
    char name[60];
    struct kfs_inode ip;

    int parent = lookup_parent(path, name);
    if (parent < 0)
        return -1;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return -1;
    uint32_t ino = dir_lookup((uint32_t)parent, name, NULL);
    if (!ino || iget(ino, &ip) < 0)
        return -1;
    if (ip.type == KFS_TYPE_DIR && !dir_is_empty(ino))
        return -1;
    /* Detach the name first: a failing dir_remove() must never leave an
     * entry pointing at an inode slot that has already been freed. */
    if (dir_remove((uint32_t)parent, name) < 0)
        return -1;
    if (truncate_locked(ino) < 0)
        return -1;
    ifree(ino);
    return 0;
}

int kfs_unlink(const char *path)
{
    fs_lock();
    int r = unlink_locked(path);
    fs_unlock();
    return r;
}

static int readdir_locked(uint32_t ino, int index, struct kfs_dirent *out)
{
    struct kfs_inode ip;
    struct kfs_dirent de;
    if (iget(ino, &ip) < 0 || ip.type != KFS_TYPE_DIR)
        return -1;
    int seen = 0;
    for (uint32_t off = 0; off + sizeof(de) <= ip.size; off += sizeof(de)) {
        if (readi(ino, off, &de, sizeof(de)) != sizeof(de))
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

int kfs_readdir(uint32_t ino, int index, struct kfs_dirent *out)
{
    if (!kfs_mounted || index < 0 || out == NULL)
        return -1;
    fs_lock();
    int r = readdir_locked(ino, index, out);
    fs_unlock();
    return r;
}

static int truncate_locked(uint32_t ino)
{
    struct kfs_inode ip;
    if (!kfs_mounted || iget(ino, &ip) < 0)
        return -1;

    for (int i = 0; i < KFS_NDIRECT; i++) {
        if (ip.direct[i]) {
            bfree(ip.direct[i]);
            ip.direct[i] = 0;
        }
    }
    if (ip.indirect) {
        if (bread(ip.indirect, ind_buf) == 0) {
            uint32_t *tab = (uint32_t *)ind_buf;
            for (uint32_t i = 0; i < KFS_NINDIRECT; i++) {
                if (tab[i])
                    bfree(tab[i]);  /* bfree does not touch ind_buf */
            }
        }
        bfree(ip.indirect);
        ip.indirect = 0;
    }
    ip.size = 0;
    return iput(ino, &ip);
}

int kfs_truncate(uint32_t ino)
{
    fs_lock();
    int r = truncate_locked(ino);
    fs_unlock();
    return r;
}

int kfs_mount(void)
{
    fs_lock();
    if (bread(0, sb_block) < 0) {
        kprintf("kfs: cannot read superblock (no disk?)\n");
        fs_unlock();
        return -1;
    }
    memcpy(&sb, sb_block, sizeof(sb));
    if (sb.magic != KFS_MAGIC) {
        kprintf("kfs: bad magic 0x%x (expected 0x%x)\n", sb.magic, KFS_MAGIC);
        fs_unlock();
        return -1;
    }
    /* Every field below is used unchecked as a block index or loop bound,
     * so a corrupt superblock must be rejected here, not dereferenced. */
    if (sb.root_ino != KFS_ROOT_INO ||
        sb.bitmap_start == 0 ||
        sb.bitmap_blocks == 0 ||
        sb.bitmap_start + sb.bitmap_blocks > sb.inode_start ||
        sb.data_start <= sb.inode_start ||
        sb.inode_start <= sb.bitmap_start ||
        sb.inode_start + sb.inode_blocks > sb.data_start ||
        sb.data_start > sb.total_blocks ||
        sb.free_blocks > sb.total_blocks ||
        sb.inode_count == 0 ||
        sb.inode_count > sb.inode_blocks * KFS_INODES_PER_BLOCK) {
        kprintf("kfs: superblock is inconsistent\n");
        fs_unlock();
        return -1;
    }
    kfs_mounted = 1;
    kprintf("kfs: mounted, %u blocks (%u free), %u inodes\n",
            sb.total_blocks, sb.free_blocks, sb.inode_count);
    fs_unlock();
    return 0;
}
