#pragma once

#include <stdint.h>
#include <stdbool.h>

/* devfs: the /dev pseudo-filesystem.
 *
 * A filesystem type like any other. It owns no disk blocks -- every path
 * under it is synthesised -- and it is mounted at /dev by vfs_init(),
 * where longest-prefix resolution puts it in front of the root
 * filesystem without anyone special-casing the path.
 *
 * What it publishes:
 *   the character devices below (null, zero, full, console, random,
 *     urandom, klog);
 *   one entry per registered block device (hda, ...), readable and
 *     writable at any byte offset, so `hexdump /dev/hda` shows the boot
 *     sector;
 *   two text views of the kernel's own tables, `mounts` and `blocks`,
 *     which is how mount(1), df(1) and lsblk(1) read the system without
 *     a syscall of their own.
 */

/* Device ids, also used as the synthetic inode numbers in k_stat. */
#define DEV_NULL     1
#define DEV_ZERO     2
#define DEV_FULL     3
#define DEV_CONSOLE  4
#define DEV_RANDOM   5
#define DEV_URANDOM  6
#define DEV_KLOG     7
#define DEV_MOUNTS   8
#define DEV_BLOCKS   9
#define DEV_BLOCKDEV 10         /* any registered struct blockdev */

/* Register the "devfs" filesystem type. Idempotent. */
void devfs_init(void);

/* Compatibility helpers backed by the kernel ChaCha20 CSPRNG. */
uint32_t devfs_random32(void);
void devfs_random_fill(void *buf, unsigned long n);
