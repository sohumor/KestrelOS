#pragma once

#include <stdint.h>
#include "kestrel_abi.h"

/* Thin VFS over KFS. All paths are absolute ("/bin/sh").
 *
 * Access control lives here, not in KFS: every entry point that names a
 * path checks current->uid / current->gid against the inode's mode, uid
 * and gid. uid 0 is root and bypasses every check. */

/* What a struct file actually refers to. FILE_KFS is 0 so a zeroed
 * (kzalloc'd) struct file is a regular file by default. */
#define FILE_KFS  0
#define FILE_PIPE 1

struct pipe;                    /* defined by pipe.c */

struct file {
    int type;                   /* FILE_KFS or FILE_PIPE */
    uint32_t inum;              /* FILE_KFS: inode number */
    uint32_t pos;               /* FILE_KFS: byte offset */
    int flags;                  /* open() flags */
    int refs;                   /* dup / spawn share a struct file */
    struct pipe *pipe;          /* FILE_PIPE: the shared ring buffer */
    int writable;               /* FILE_PIPE: 1 = write end, 0 = read end */
};

/* Permission bits used with the vfs_* access helpers. */
#define VFS_R 4
#define VFS_W 2
#define VFS_X 1

int  vfs_init(void);   /* mount root fs; returns 0 or -1 */
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

/* Current wall-clock time in seconds since the Unix epoch, 0 if the RTC
 * is unreadable. rtc_unix_time() is the general-purpose entry point (use
 * it for SYS_TIME and for log timestamps); vfs_now() is the same value and
 * exists so filesystem code reads as filesystem code. Both are cached
 * against the timer tick, so calling them per write is cheap. */
uint32_t rtc_unix_time(void);
uint32_t vfs_now(void);
