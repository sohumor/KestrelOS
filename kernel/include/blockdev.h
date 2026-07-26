#pragma once

#include <stdint.h>

/* Block device layer.
 *
 * Everything that stores blocks -- a disk, a partition, a ramdisk --
 * registers a `struct blockdev` here under a short name ("hda"). A
 * filesystem then mounts *a block device* rather than reaching for one
 * driver by name, which is what makes a second disk, a second partition
 * or an in-memory root filesystem possible at all.
 *
 * The read/write callbacks address the device in units of block_size
 * bytes, counted from LBA 0 of that device. They are called from
 * preemptible syscall context and may busy-poll hardware, but never
 * sleep and never allocate; nothing here runs in IRQ context.
 *
 * Registration owns no memory: the caller supplies a `struct blockdev`
 * with static (or otherwise permanent) storage and the registry keeps
 * the pointer. */

#define BLOCKDEV_NAME_MAX 16
#define BLOCKDEV_MAX      8

struct blockdev {
    char name[BLOCKDEV_NAME_MAX];   /* "hda", "ram0", ... NUL-terminated */
    uint32_t block_size;            /* bytes per block, a power of two */
    uint64_t blocks;                /* device size in blocks */
    void *priv;                     /* driver's own handle */
    int (*read)(struct blockdev *bd, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blockdev *bd, uint64_t lba, uint32_t count,
                 const void *buf);
};

/* Add / remove a device. register returns 0, or -1 for a bad descriptor,
 * a duplicate name or a full table. */
int  blockdev_register(struct blockdev *bd);
void blockdev_unregister(struct blockdev *bd);

/* Look one up by name, or NULL. */
struct blockdev *blockdev_find(const char *name);

/* Enumerate: index 0, 1, 2 ... until it returns -1. Indices are stable
 * only for as long as nothing registers or unregisters. */
int  blockdev_list(int index, struct blockdev **out);

/* Bounds-checked wrappers: refuse a request that runs off the end of the
 * device instead of handing a wild LBA to the driver. Return 0 or -1. */
int  blockdev_read(struct blockdev *bd, uint64_t lba, uint32_t count,
                   void *buf);
int  blockdev_write(struct blockdev *bd, uint64_t lba, uint32_t count,
                    const void *buf);
