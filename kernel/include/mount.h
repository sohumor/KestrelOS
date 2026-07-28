#pragma once

#include <stdint.h>
#include "kestrel_abi.h"

/* Filesystem registry and mount table.
 *
 * A filesystem registers a `struct fs_type` under a name ("kfs",
 * "devfs"); mounting it binds that type to a block device (or to
 * nothing, for a synthetic filesystem) at a path. The VFS then resolves
 * every path to (mount, path-relative-to-that-mount) by LONGEST PREFIX
 * and calls the mount's ops -- so "/", "/dev" and anything mounted later
 * all work by one rule instead of by a special case in front of the
 * root filesystem.
 *
 * Everything here is called from preemptible syscall context. The table
 * itself is walked under an IRQ-safe spin lock; the operations below are
 * called with it dropped and must do their own locking (see kfs.c). */

struct blockdev;
struct file;
struct mount;

#define FS_TYPE_NAME_MAX  16
#define MOUNT_PATH_MAX    64
#define MOUNT_MAX          8
#define FS_TYPE_MAX        8

/* What a filesystem reports about itself for `df`. A synthetic
 * filesystem that stores nothing reports every field as 0. */
struct fs_statfs {
    uint32_t block_size;
    uint64_t blocks;            /* total blocks in the filesystem */
    uint64_t free_blocks;
    uint32_t files;             /* total inode slots */
    uint32_t free_files;
};

/* The operations the VFS needs. Path operations take the mount and a
 * path relative to its mount point (always absolute within the
 * filesystem: "/bin/sh" for a root mount, "/null" for /dev/null, "/" for
 * the mount point itself). Handle operations take the struct file that
 * open() returned; the VFS stamps `ops` and `mnt` on it, so a handle
 * always knows where it came from.
 *
 * A NULL entry means "not supported" and the VFS answers -1 / NULL. Each
 * filesystem enforces its own access control, using the shared policy
 * helpers in vfs.h so that the rules are written down exactly once.
 *
 * close() owns the handle: it releases whatever the filesystem attached
 * and frees the struct file (or the larger allocation it is embedded
 * in). The VFS does the reference counting and never frees a handle. */
struct fs_ops {
    struct file *(*open)(struct mount *m, const char *path, int flags);
    void (*close)(struct file *f);
    long (*read)(struct file *f, void *buf, unsigned long n);
    long (*write)(struct file *f, const void *buf, unsigned long n);
    long (*seek)(struct file *f, long off, int whence);
    int  (*stat)(struct mount *m, const char *path, struct k_stat *st);
    int  (*readdir)(struct mount *m, const char *path, int index,
                    struct k_dirent *de);
    int  (*unlink)(struct mount *m, const char *path);
    int  (*mkdir)(struct mount *m, const char *path);
    int  (*chmod)(struct mount *m, const char *path, uint32_t mode);
    int  (*chown)(struct mount *m, const char *path, uint32_t uid,
                  uint32_t gid);
    int  (*statfs)(struct mount *m, struct fs_statfs *sf);
};

/* A filesystem implementation. mount() binds an instance to a block
 * device and hands back its private state; bd is NULL for a synthetic
 * filesystem. Registration keeps the pointer, so the descriptor must
 * have permanent storage. */
struct fs_type {
    char name[FS_TYPE_NAME_MAX];
    int (*mount)(struct blockdev *bd, void **fs_priv);
    void (*unmount)(void *fs_priv);
    struct fs_ops *ops;
};

/* One mounted instance. */
struct mount {
    char path[MOUNT_PATH_MAX];  /* "/" or "/dev"; no trailing slash */
    struct fs_type *type;
    struct blockdev *bd;        /* may be NULL */
    void *priv;                 /* the instance handed back by mount() */
    int used;
};

/* --- filesystem registry --- */

int fs_register(struct fs_type *t);
void fs_unregister(struct fs_type *t);
struct fs_type *fs_find(const char *name);
int fs_list(int index, struct fs_type **out);

/* --- mount table --- */

/* Mount `fstype` from block device `devname` (NULL or "" for a
 * filesystem that needs none) at `path`. Returns 0 or -1. */
int mount_add(const char *path, const char *fstype, const char *devname);

/* Unmount the filesystem mounted exactly at `path`. Returns 0 or -1.
 * The root mount cannot be removed. */
int mount_remove(const char *path);

/* Longest-prefix resolution. Returns the mount covering `path` and, in
 * *rel_out, the path relative to it (always starting with '/', pointing
 * either into `path` or at a literal "/"), or NULL if nothing covers it. */
struct mount *mount_resolve(const char *path, const char **rel_out);

/* Enumerate mounts: index 0, 1, 2 ... until it returns -1. */
int mount_list(int index, struct mount **out);
