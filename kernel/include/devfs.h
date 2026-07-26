#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "vfs.h"
#include "kestrel_abi.h"

/* /dev pseudo-filesystem.
 *
 * A self-contained module the VFS consults before it reaches KFS. It owns
 * no disk blocks: every path under /dev is synthesised. The handles it
 * returns are `struct devfile` allocations whose first member is a plain
 * struct file, so a `struct file *` from devfs_open() can be stored in the
 * per-task fd table exactly like a KFS handle.
 *
 * Dispatch: devfs_owns(f) answers "did this handle come from here?" using
 * a private registry, so it does not depend on any field inside struct
 * file. If struct file gains a `type` tag, DEVFS_FILE_TYPE is the value
 * reserved for these handles and may be used instead. */

#define DEVFS_FILE_TYPE 1

/* Device ids, also used as the synthetic inode numbers in k_stat. */
#define DEV_NULL     1
#define DEV_ZERO     2
#define DEV_FULL     3
#define DEV_CONSOLE  4
#define DEV_RANDOM   5
#define DEV_KLOG     6

void devfs_init(void);

/* true for exactly "/dev" and for anything under "/dev/". */
bool devfs_claims(const char *path);

/* true if this handle was produced by devfs_open(). */
bool devfs_owns(struct file *f);

struct file *devfs_open(const char *path, int flags);
long devfs_read(struct file *f, void *buf, unsigned long n);
long devfs_write(struct file *f, const void *buf, unsigned long n);
long devfs_seek(struct file *f, long off, int whence);
void devfs_close(struct file *f);
int  devfs_stat(const char *path, struct k_stat *st);
int  devfs_readdir(const char *path, int index, struct k_dirent *de);

/* Non-cryptographic PRNG behind /dev/random. Do NOT use for keys. */
uint32_t devfs_random32(void);
void devfs_random_fill(void *buf, unsigned long n);
