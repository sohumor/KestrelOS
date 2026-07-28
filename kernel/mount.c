#include "kernel.h"
#include "mount.h"
#include "blockdev.h"
#include "proc.h"
#include "string.h"
#include "spinlock.h"

/* Filesystem registry and mount table. See mount.h for the contract.
 *
 * Both tables are fixed arrays of descriptors owned by their registrants
 * (fs types) or by this file (mounts). Registration and mounting run in
 * preemptible context and lookups happen on every path syscall, so every
 * walk of either table is done under an IRQ-safe spin lock shared by all
 * CPUs.
 *
 * The type's mount()/unmount() callbacks do real disk I/O, so they are
 * deliberately called with interrupts on and the table lock dropped. The
 * slot is reserved first (used = 1, priv = NULL) so no second mounter can
 * claim the same path underneath, and released again if mount() fails. */

static struct fs_type *types[FS_TYPE_MAX];
static struct mount mounts[MOUNT_MAX];
static spinlock_t mount_lock = SPINLOCK_INIT;

/* ---------- filesystem registry ---------- */

int fs_register(struct fs_type *t)
{
    int slot = -1;

    if (t == NULL || t->ops == NULL || t->mount == NULL || t->name[0] == '\0')
        return -1;
    if (t->name[FS_TYPE_NAME_MAX - 1] != '\0')
        return -1;              /* an unterminated name breaks every strcmp */

    uint64_t f = spin_lock_irqsave(&mount_lock);
    for (int i = 0; i < FS_TYPE_MAX; i++) {
        if (types[i] == NULL) {
            if (slot < 0)
                slot = i;
            continue;
        }
        if (strcmp(types[i]->name, t->name) == 0) {
            spin_unlock_irqrestore(&mount_lock, f);
            return -1;          /* already registered */
        }
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&mount_lock, f);
        kprintf("fs: type table full, cannot add %s\n", t->name);
        return -1;
    }
    types[slot] = t;
    spin_unlock_irqrestore(&mount_lock, f);
    kprintf("fs: filesystem type '%s' registered\n", t->name);
    return 0;
}

void fs_unregister(struct fs_type *t)
{
    if (t == NULL)
        return;
    uint64_t f = spin_lock_irqsave(&mount_lock);
    for (int i = 0; i < FS_TYPE_MAX; i++) {
        if (types[i] == t) {
            types[i] = NULL;
            break;
        }
    }
    spin_unlock_irqrestore(&mount_lock, f);
}

struct fs_type *fs_find(const char *name)
{
    struct fs_type *found = NULL;

    if (name == NULL || name[0] == '\0')
        return NULL;

    uint64_t f = spin_lock_irqsave(&mount_lock);
    for (int i = 0; i < FS_TYPE_MAX; i++) {
        if (types[i] != NULL && strcmp(types[i]->name, name) == 0) {
            found = types[i];
            break;
        }
    }
    spin_unlock_irqrestore(&mount_lock, f);
    return found;
}

int fs_list(int index, struct fs_type **out)
{
    int seen = 0;
    int r = -1;

    if (index < 0 || out == NULL)
        return -1;

    uint64_t f = spin_lock_irqsave(&mount_lock);
    for (int i = 0; i < FS_TYPE_MAX; i++) {
        if (types[i] == NULL)
            continue;
        if (seen == index) {
            *out = types[i];
            r = 0;
            break;
        }
        seen++;
    }
    spin_unlock_irqrestore(&mount_lock, f);
    return r;
}

/* ---------- mount points ---------- */

/* Copy `path` into `out` as a canonical mount point: absolute, no
 * trailing slash, no empty components. "/" stays "/". Returns 0 or -1. */
static int canon_path(const char *path, char *out)
{
    size_t len;

    if (path == NULL || path[0] != '/')
        return -1;
    len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        len--;
    if (len >= MOUNT_PATH_MAX)
        return -1;
    /* "//x" and "/a//b" would never match a resolved path, so reject
     * them here rather than create a mount nothing can reach. */
    for (size_t i = 1; i < len; i++)
        if (path[i] == '/' && path[i - 1] == '/')
            return -1;
    memcpy(out, path, len);
    out[len] = '\0';
    return 0;
}

/* True if mount point `mp` (canonical) covers `path`. */
static int covers(const char *mp, const char *path, size_t *mplen_out)
{
    size_t mplen = strlen(mp);

    if (mplen == 1)             /* the root mount covers everything */
        *mplen_out = 1;
    else if (strncmp(path, mp, mplen) == 0 &&
             (path[mplen] == '\0' || path[mplen] == '/'))
        *mplen_out = mplen;
    else
        return 0;
    return 1;
}

struct mount *mount_resolve(const char *path, const char **rel_out)
{
    struct mount *best = NULL;
    size_t bestlen = 0;

    if (path == NULL || path[0] != '/')
        return NULL;

    uint64_t f = spin_lock_irqsave(&mount_lock);
    for (int i = 0; i < MOUNT_MAX; i++) {
        size_t mplen;
        if (!mounts[i].used || mounts[i].priv == NULL)
            continue;           /* a slot mid-mount is not usable yet */
        if (!covers(mounts[i].path, path, &mplen))
            continue;
        if (best == NULL || mplen > bestlen) {
            best = &mounts[i];
            bestlen = mplen;
        }
    }
    spin_unlock_irqrestore(&mount_lock, f);

    if (best == NULL)
        return NULL;
    if (rel_out != NULL) {
        const char *rel = (bestlen == 1) ? path : path + bestlen;
        /* "/dev" under the /dev mount is the mount point itself. */
        *rel_out = (rel[0] == '\0') ? "/" : rel;
    }
    return best;
}

int mount_list(int index, struct mount **out)
{
    int seen = 0;
    int r = -1;

    if (index < 0 || out == NULL)
        return -1;

    uint64_t f = spin_lock_irqsave(&mount_lock);
    for (int i = 0; i < MOUNT_MAX; i++) {
        if (!mounts[i].used || mounts[i].priv == NULL)
            continue;           /* a slot mid-mount is not usable yet */
        if (seen == index) {
            *out = &mounts[i];
            r = 0;
            break;
        }
        seen++;
    }
    spin_unlock_irqrestore(&mount_lock, f);
    return r;
}

int mount_add(const char *path, const char *fstype, const char *devname)
{
    char mp[MOUNT_PATH_MAX];
    struct fs_type *type;
    struct blockdev *bd = NULL;
    struct mount *m = NULL;
    void *priv = NULL;

    if (canon_path(path, mp) < 0)
        return -1;
    type = fs_find(fstype);
    if (type == NULL) {
        kprintf("mount: no filesystem type '%s'\n",
                fstype ? fstype : "(null)");
        return -1;
    }
    if (devname != NULL && devname[0] != '\0') {
        bd = blockdev_find(devname);
        if (bd == NULL) {
            kprintf("mount: no block device '%s'\n", devname);
            return -1;
        }
    }

    /* Reserve the slot with priv still NULL: mount_resolve() skips such
     * a slot, so nothing can traverse a half-built mount. */
    uint64_t f = spin_lock_irqsave(&mount_lock);
    for (int i = 0; i < MOUNT_MAX; i++) {
        if (mounts[i].used && strcmp(mounts[i].path, mp) == 0) {
            spin_unlock_irqrestore(&mount_lock, f);
            kprintf("mount: %s is already a mount point\n", mp);
            return -1;
        }
    }
    for (int i = 0; i < MOUNT_MAX; i++) {
        if (!mounts[i].used) {
            m = &mounts[i];
            break;
        }
    }
    if (m == NULL) {
        spin_unlock_irqrestore(&mount_lock, f);
        kprintf("mount: table full\n");
        return -1;
    }
    memset(m, 0, sizeof(*m));
    strcpy(m->path, mp);
    m->type = type;
    m->bd = bd;
    m->priv = NULL;
    m->used = 1;
    spin_unlock_irqrestore(&mount_lock, f);

    /* Real disk I/O: interrupts on, table lock dropped. */
    if (type->mount(bd, &priv) < 0 || priv == NULL) {
        f = spin_lock_irqsave(&mount_lock);
        memset(m, 0, sizeof(*m));
        spin_unlock_irqrestore(&mount_lock, f);
        return -1;
    }

    f = spin_lock_irqsave(&mount_lock);
    m->priv = priv;
    spin_unlock_irqrestore(&mount_lock, f);
    return 0;
}

int mount_remove(const char *path)
{
    char mp[MOUNT_PATH_MAX];
    struct mount *m = NULL;
    struct fs_type *type;
    void *priv;

    if (canon_path(path, mp) < 0)
        return -1;
    if (strcmp(mp, "/") == 0)
        return -1;              /* the root mount is not removable */

    uint64_t f = spin_lock_irqsave(&mount_lock);
    for (int i = 0; i < MOUNT_MAX; i++) {
        if (mounts[i].used && strcmp(mounts[i].path, mp) == 0) {
            m = &mounts[i];
            break;
        }
    }
    if (m == NULL || m->priv == NULL) {
        spin_unlock_irqrestore(&mount_lock, f);
        return -1;
    }
    /* Detach first: from here no new path can resolve into it, so the
     * unmount callback cannot race a fresh open. Handles already open
     * keep their own reference to the instance and are unaffected. */
    type = m->type;
    priv = m->priv;
    memset(m, 0, sizeof(*m));
    spin_unlock_irqrestore(&mount_lock, f);

    if (type->unmount != NULL)
        type->unmount(priv);
    return 0;
}
