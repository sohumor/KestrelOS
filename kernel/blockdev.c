#include "kernel.h"
#include "blockdev.h"
#include "proc.h"
#include "string.h"

/* The block device registry. See blockdev.h for the contract.
 *
 * The table is a fixed array of pointers to descriptors the drivers own.
 * Registration happens at driver init and, once modules exist, at insmod
 * time -- both preemptible contexts -- while lookups happen from mount
 * and from /dev, so every walk of the table runs under irq_save(). There
 * is one CPU, so masking interrupts is a full mutex. */

static struct blockdev *devices[BLOCKDEV_MAX];

int blockdev_register(struct blockdev *bd)
{
    int slot = -1;

    if (bd == NULL || bd->read == NULL || bd->name[0] == '\0')
        return -1;
    if (bd->block_size == 0 || (bd->block_size & (bd->block_size - 1)) != 0)
        return -1;              /* block size must be a power of two */
    /* A name that is not NUL-terminated would run off the end of the
     * field in every strcmp below. */
    if (bd->name[BLOCKDEV_NAME_MAX - 1] != '\0')
        return -1;

    uint64_t f = irq_save();
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        if (devices[i] == NULL) {
            if (slot < 0)
                slot = i;
            continue;
        }
        if (strcmp(devices[i]->name, bd->name) == 0) {
            irq_restore(f);
            kprintf("blockdev: %s already registered\n", bd->name);
            return -1;
        }
    }
    if (slot < 0) {
        irq_restore(f);
        kprintf("blockdev: table full, cannot add %s\n", bd->name);
        return -1;
    }
    devices[slot] = bd;
    irq_restore(f);

    kprintf("blockdev: %s registered, %u-byte blocks, %lu blocks (%lu MiB)\n",
            bd->name, bd->block_size, (unsigned long)bd->blocks,
            (unsigned long)(bd->blocks * bd->block_size / (1024 * 1024)));
    return 0;
}

void blockdev_unregister(struct blockdev *bd)
{
    if (bd == NULL)
        return;
    uint64_t f = irq_save();
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        if (devices[i] == bd) {
            devices[i] = NULL;
            break;
        }
    }
    irq_restore(f);
}

struct blockdev *blockdev_find(const char *name)
{
    struct blockdev *found = NULL;

    if (name == NULL || name[0] == '\0')
        return NULL;

    uint64_t f = irq_save();
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        if (devices[i] != NULL && strcmp(devices[i]->name, name) == 0) {
            found = devices[i];
            break;
        }
    }
    irq_restore(f);
    return found;
}

int blockdev_list(int index, struct blockdev **out)
{
    int seen = 0;
    int r = -1;

    if (index < 0 || out == NULL)
        return -1;

    uint64_t f = irq_save();
    for (int i = 0; i < BLOCKDEV_MAX; i++) {
        if (devices[i] == NULL)
            continue;
        if (seen == index) {
            *out = devices[i];
            r = 0;
            break;
        }
        seen++;
    }
    irq_restore(f);
    return r;
}

/* A count of 0 is a no-op rather than an error, so a caller that computes
 * a length can pass it straight through. */
static int range_ok(struct blockdev *bd, uint64_t lba, uint32_t count)
{
    if (bd == NULL)
        return 0;
    if (count == 0)
        return 1;
    if (lba >= bd->blocks || count > bd->blocks - lba)
        return 0;
    return 1;
}

int blockdev_read(struct blockdev *bd, uint64_t lba, uint32_t count, void *buf)
{
    if (!range_ok(bd, lba, count) || buf == NULL)
        return -1;
    if (count == 0)
        return 0;
    return bd->read(bd, lba, count, buf);
}

int blockdev_write(struct blockdev *bd, uint64_t lba, uint32_t count,
                   const void *buf)
{
    if (!range_ok(bd, lba, count) || buf == NULL)
        return -1;
    if (bd->write == NULL)
        return -1;              /* read-only device */
    if (count == 0)
        return 0;
    return bd->write(bd, lba, count, buf);
}
