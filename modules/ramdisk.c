/* ramdisk.c - a RAM-backed block device, loadable.
 *
 * The second module, and the one that shows the loader carrying real
 * weight: it allocates through the exported heap, registers a device with
 * a subsystem that is compiled into the kernel, and unregisters it again
 * on rmmod. Registering an in-memory disk from a module is exactly the
 * case docs/MODULARITY.md section 4 exists for: KFS mounts *a block
 * device*, so a root filesystem can come from something that was not
 * compiled in.
 *
 * Depends on the block device layer (kernel/include/blockdev.h). If that
 * subsystem is not in the build, blockdev_register is a weak export with
 * no provider (see kernel/ksyms.c) and insmod refuses this module by
 * name, which is the correct outcome rather than a silent failure.
 */

#include "kernel.h"
#include "module.h"
#include "blockdev.h"
#include "kheap.h"
#include "string.h"
#include "klog.h"

#define RD_BLOCK_SIZE 512
#define RD_BLOCKS     512               /* 256 KiB */

static uint8_t *rd_data;

/* Both entry points reject a range that runs off the end or wraps, which
 * matters more here than on a real disk: the backing store is the kernel
 * heap, so an unchecked lba would corrupt whatever is next to it. */
static int rd_range_ok(uint64_t lba, uint32_t count)
{
    if (!rd_data || count == 0)
        return 0;
    if (lba >= RD_BLOCKS || (uint64_t)count > RD_BLOCKS - lba)
        return 0;
    return 1;
}

static int rd_read(struct blockdev *bd, uint64_t lba, uint32_t count,
                   void *buf)
{
    (void)bd;
    if (!buf || !rd_range_ok(lba, count))
        return -1;
    memcpy(buf, rd_data + lba * RD_BLOCK_SIZE,
           (size_t)count * RD_BLOCK_SIZE);
    return 0;
}

static int rd_write(struct blockdev *bd, uint64_t lba, uint32_t count,
                    const void *buf)
{
    (void)bd;
    if (!buf || !rd_range_ok(lba, count))
        return -1;
    memcpy(rd_data + lba * RD_BLOCK_SIZE, buf,
           (size_t)count * RD_BLOCK_SIZE);
    return 0;
}

static struct blockdev rd_dev = {
    .name = "ram0",
    .block_size = RD_BLOCK_SIZE,
    .blocks = RD_BLOCKS,
    .read = rd_read,
    .write = rd_write,
};

static int ramdisk_init(void)
{
    rd_data = kmalloc((size_t)RD_BLOCK_SIZE * RD_BLOCKS);
    if (!rd_data) {
        kprintf("ramdisk: cannot allocate %u KiB\n",
                (unsigned)(RD_BLOCK_SIZE * RD_BLOCKS / 1024));
        return -1;
    }
    memset(rd_data, 0, (size_t)RD_BLOCK_SIZE * RD_BLOCKS);

    if (blockdev_register(&rd_dev) != 0) {
        kprintf("ramdisk: blockdev_register(ram0) failed\n");
        kfree(rd_data);
        rd_data = NULL;
        return -1;
    }

    kprintf("ramdisk: ram0 registered, %u blocks of %u bytes\n",
            (unsigned)RD_BLOCKS, (unsigned)RD_BLOCK_SIZE);
    klog_printf(K_LOG_INFO, "ramdisk", "ram0 registered (%u KiB)",
                (unsigned)(RD_BLOCK_SIZE * RD_BLOCKS / 1024));
    return 0;
}

static void ramdisk_exit(void)
{
    blockdev_unregister(&rd_dev);
    kfree(rd_data);
    rd_data = NULL;
    kprintf("ramdisk: ram0 unregistered\n");
    klog_write(K_LOG_INFO, "ramdisk", "ram0 unregistered");
}

MODULE_NAME("ramdisk");
MODULE_DESC("256 KiB RAM-backed block device (ram0)");
MODULE_INIT(ramdisk_init);
MODULE_EXIT(ramdisk_exit);
