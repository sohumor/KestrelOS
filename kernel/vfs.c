#include "kernel.h"
#include "vfs.h"
#include "kfs.h"
#include "kheap.h"
#include "proc.h"
#include "string.h"
#include "kestrel_abi.h"

/* Thin VFS over KFS. All paths are absolute. */

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
    struct kfs_inode ip;

    if (!vfs_ready || path == NULL)
        return NULL;

    int ino = kfs_lookup(path);
    if (ino < 0) {
        if (!(flags & O_CREAT))
            return NULL;
        ino = kfs_create(path);
        if (ino < 0)
            return NULL;
    }
    if (kfs_iget((uint32_t)ino, &ip) < 0)
        return NULL;
    if (ip.type == KFS_TYPE_DIR && flags_allow_write(flags))
        return NULL;            /* directories are read-only via the VFS */

    if (inode_ref((uint32_t)ino) < 0)
        return NULL;

    if ((flags & O_TRUNC) && flags_allow_write(flags) &&
        ip.type == KFS_TYPE_FILE) {
        if (kfs_truncate((uint32_t)ino) < 0) {
            inode_unref((uint32_t)ino);
            return NULL;
        }
    }

    struct file *f = kzalloc(sizeof(struct file));
    if (f == NULL) {
        inode_unref((uint32_t)ino);
        return NULL;
    }
    f->inum = (uint32_t)ino;
    f->pos = 0;
    f->flags = flags;
    f->refs = 1;
    return f;
}

void vfs_close(struct file *f)
{
    if (f == NULL)
        return;
    if (--f->refs <= 0) {
        inode_unref(f->inum);
        kfree(f);
    }
}

long vfs_read(struct file *f, void *buf, unsigned long n)
{
    if (f == NULL || !flags_allow_read(f->flags))
        return -1;
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

    if (f == NULL || !flags_allow_write(f->flags))
        return -1;
    if (n > 0x7FFFFFFFUL)
        n = 0x7FFFFFFFUL;
    if (f->flags & O_APPEND) {
        if (kfs_iget(f->inum, &ip) < 0)
            return -1;
        f->pos = ip.size;
    }
    long r = kfs_write(f->inum, f->pos, buf, (uint32_t)n);
    if (r > 0)
        f->pos += (uint32_t)r;
    return r;
}

long vfs_seek(struct file *f, long off, int whence)
{
    struct kfs_inode ip;
    long base;

    if (f == NULL)
        return -1;
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

int vfs_stat(const char *path, struct k_stat *st)
{
    struct kfs_inode ip;

    if (!vfs_ready || path == NULL || st == NULL)
        return -1;
    int ino = kfs_lookup(path);
    if (ino < 0 || kfs_iget((uint32_t)ino, &ip) < 0)
        return -1;
    st->size = ip.size;
    st->is_dir = (ip.type == KFS_TYPE_DIR);
    return 0;
}

int vfs_readdir(const char *path, int index, struct k_dirent *de)
{
    struct kfs_dirent kde;
    struct kfs_inode ip;

    if (!vfs_ready || path == NULL || de == NULL)
        return -1;
    int ino = kfs_lookup(path);
    if (ino < 0)
        return -1;
    if (kfs_readdir((uint32_t)ino, index, &kde) < 0)
        return -1;

    memcpy(de->name, kde.name, sizeof(de->name));
    de->name[sizeof(de->name) - 1] = '\0';
    de->size = 0;
    de->is_dir = 0;
    if (kfs_iget(kde.ino, &ip) == 0) {
        de->size = ip.size;
        de->is_dir = (ip.type == KFS_TYPE_DIR);
    }
    return 0;
}

int vfs_unlink(const char *path)
{
    if (!vfs_ready || path == NULL)
        return -1;
    int ino = kfs_lookup(path);
    if (ino >= 0 && inode_is_open((uint32_t)ino))
        return -1;              /* still open: the slot must not be reused */
    return kfs_unlink(path);
}

int vfs_mkdir(const char *path)
{
    if (!vfs_ready || path == NULL)
        return -1;
    return kfs_mkdir(path);
}
