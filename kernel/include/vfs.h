#pragma once

#include <stdint.h>
#include "kestrel_abi.h"

/* Thin VFS over KFS. All paths are absolute ("/bin/sh"). */

struct file {
    uint32_t inum;
    uint32_t pos;
    int flags;
    int refs;
};

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
