#pragma once

#include <stdint.h>
#include "kestrel_abi.h"

/* The VFS: a thin dispatcher over mounted filesystems. All paths are
 * absolute ("/bin/sh").
 *
 * Every entry point resolves the path to (mount, path within that mount)
 * by longest prefix (see mount.h) and calls that mount's operations. It
 * knows nothing about KFS, about /dev or about pipes; a handle carries
 * its own operations vector, so a pipe, a device and a regular file all
 * travel the same read/write path.
 *
 * Access control *policy* lives here -- vfs_perm_ok() below is the one
 * place the uid/gid/mode rules are written down -- while the walk that
 * applies it belongs to each filesystem, which is the only thing that
 * can traverse its own directories in one pass. */

/* What a struct file refers to. FILE_KFS is 0 so a zeroed (kzalloc'd)
 * struct file is a regular file by default. The tag survives for the
 * benefit of pipe.c, which checks it; dispatch no longer uses it. */
#define FILE_KFS  0
#define FILE_PIPE 1

struct pipe;                    /* defined by pipe.c */
struct fs_ops;                  /* defined by mount.h */
struct mount;                   /* defined by mount.h */

struct file {
    const struct fs_ops *ops;   /* how to read/write/seek/close this */
    struct mount *mnt;          /* the mount it came from, NULL for pipes */
    int type;                   /* FILE_KFS or FILE_PIPE */
    uint32_t inum;              /* filesystem's own handle id */
    uint32_t pos;               /* byte offset */
    int flags;                  /* open() flags */
    int refs;                   /* dup / spawn share a struct file */
    struct pipe *pipe;          /* FILE_PIPE: the shared ring buffer */
    int writable;               /* FILE_PIPE: 1 = write end, 0 = read end */
};

/* Permission bits used with the vfs_* access helpers. */
#define VFS_R 4
#define VFS_W 2
#define VFS_X 1

int  vfs_init(void);   /* register the filesystems and mount / and /dev */
struct file *vfs_open(const char *path, int flags);
void vfs_close(struct file *f);
long vfs_read(struct file *f, void *buf, unsigned long n);
long vfs_write(struct file *f, const void *buf, unsigned long n);
long vfs_seek(struct file *f, long off, int whence);
int  vfs_stat(const char *path, struct k_stat *st);
int  vfs_readdir(const char *path, int index, struct k_dirent *de);
int  vfs_unlink(const char *path);
int  vfs_mkdir(const char *path);

/* Owner / permission changes. chmod needs the caller to own the file (or
 * be root); chown is root-only. Both return 0 or -1. */
int  vfs_chmod(const char *path, uint32_t mode);
int  vfs_chown(const char *path, uint32_t uid, uint32_t gid);

/* 1 if the current task may execute `path` (a regular file with an x bit
 * the caller matches, reachable through directories it may search),
 * else 0. The ELF loader calls this before reading the image. */
int  vfs_exec_ok(const char *path);

/* Create a pipe whose two ends carry this layer's pipe operations, so
 * they read and write through the same dispatcher as everything else.
 * Returns 0, or -1 with nothing allocated. Close each end with
 * vfs_close(). */
int  vfs_pipe(struct file **read_end, struct file **write_end);

/* --- shared access-control policy ------------------------------------
 * The calling task's identity, and the one rule that decides whether it
 * may do `want` (a bitwise OR of VFS_R / VFS_W / VFS_X) to an object
 * with these owners and permission bits. uid 0 is root and passes
 * everything; pre-scheduler boot code counts as root. Filesystems call
 * these instead of writing the comparison out again. */
uint32_t vfs_uid(void);
uint32_t vfs_gid(void);
int  vfs_perm_ok(uint32_t mode, uint32_t uid, uint32_t gid, int want);

/* Access-mode tests on open() flags. */
int  vfs_flags_allow_read(int flags);
int  vfs_flags_allow_write(int flags);

/* Current wall-clock time in seconds since the Unix epoch, 0 if the RTC
 * is unreadable. rtc_unix_time() is the general-purpose entry point (use
 * it for SYS_TIME and for log timestamps); vfs_now() is the same value and
 * exists so filesystem code reads as filesystem code. Both are cached
 * against the timer tick, so calling them per write is cheap. */
uint32_t rtc_unix_time(void);
uint32_t vfs_now(void);
