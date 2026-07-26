/* ksyms.c - the kernel's module-facing API, made explicit.
 *
 * Everything a loadable module is allowed to call is listed here and
 * nowhere else. Adding an entry is a deliberate act: it widens the
 * kernel's public surface and commits it to that signature. Removing one
 * breaks every module that used it, loudly, at load time.
 *
 * Deliberately *not* exported: the scheduler internals, the VFS, the
 * syscall layer, the window manager. Modules exist to decouple drivers at
 * the edges (docs/MODULARITY.md), not to make the core swappable.
 *
 * Note on port I/O: inb/outb/inw/outw/inl/outl are static inline in
 * kernel/include/io.h, so they have no address to export and none is
 * needed — a module that includes io.h gets the instructions inlined.
 * The same is true of irq_save/irq_restore, sti/cli and invlpg.
 */

#include "kernel.h"
#include "export.h"
#include "kheap.h"
#include "pmm.h"
#include "string.h"
#include "interrupts.h"
#include "timer.h"
#include "proc.h"
#include "vmm.h"
#include "klog.h"

/* Linker-provided bounds of the .ksyms output section. */
extern const struct ksym __ksyms_start[];
extern const struct ksym __ksyms_end[];

/* --- console and failure ---------------------------------------------- */

EXPORT_SYMBOL(kprintf);
EXPORT_SYMBOL(kvprintf);
EXPORT_SYMBOL(panic);

/* --- kernel heap ------------------------------------------------------- */

EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kzalloc);
EXPORT_SYMBOL(kfree);

/* --- physical memory --------------------------------------------------- */

EXPORT_SYMBOL(pmm_alloc);
EXPORT_SYMBOL(pmm_free);
EXPORT_SYMBOL(pmm_alloc_contig);
EXPORT_SYMBOL(pmm_free_contig);
EXPORT_SYMBOL(pmm_free_pages);

/* --- virtual memory ---------------------------------------------------- */

EXPORT_SYMBOL(vmm_map_page);
EXPORT_SYMBOL(vmm_kernel_pml4);
EXPORT_SYMBOL(vmm_virt_to_phys);

/* --- string and memory helpers ----------------------------------------- */

EXPORT_SYMBOL(memcpy);
EXPORT_SYMBOL(memset);
EXPORT_SYMBOL(memmove);
EXPORT_SYMBOL(memcmp);
EXPORT_SYMBOL(strlen);
EXPORT_SYMBOL(strcmp);
EXPORT_SYMBOL(strncmp);
EXPORT_SYMBOL(strcpy);
EXPORT_SYMBOL(strncpy);
EXPORT_SYMBOL(strchr);

/* --- interrupts -------------------------------------------------------- */

EXPORT_SYMBOL(irq_install_handler);
EXPORT_SYMBOL(pic_set_mask);
EXPORT_SYMBOL(pic_clear_mask);
EXPORT_SYMBOL(pic_send_eoi);

/* --- time and scheduling ----------------------------------------------- */

EXPORT_SYMBOL(timer_ticks);
EXPORT_SYMBOL(timer_sleep);
EXPORT_SYMBOL(kthread_create);
EXPORT_SYMBOL(task_sleep_ticks);
EXPORT_SYMBOL(yield);

/* --- kernel log -------------------------------------------------------- */

EXPORT_SYMBOL(klog_write);
EXPORT_SYMBOL(klog_printf);

/* --- block device layer (optional) --------------------------------------
 * Re-declared weak on purpose: the block layer is a separate subsystem
 * and may legitimately be left out of a build. If it is, these records
 * carry a NULL address, ksym_lookup() refuses them, and a block driver
 * module fails to load naming the symbol it wanted — which is the right
 * answer, and much better than resolving to zero and jumping there. */
#include "blockdev.h"

extern int  blockdev_register(struct blockdev *bd) __attribute__((weak));
extern void blockdev_unregister(struct blockdev *bd) __attribute__((weak));

EXPORT_SYMBOL(blockdev_register);
EXPORT_SYMBOL(blockdev_unregister);

/* --- lookup ------------------------------------------------------------ */

void *ksym_lookup(const char *name)
{
    const struct ksym *s;

    if (!name || !*name)
        return NULL;
    for (s = __ksyms_start; s < __ksyms_end; s++) {
        if (!s->addr || !s->name)
            continue;               /* weak export with no provider */
        if (strcmp(s->name, name) == 0)
            return s->addr;
    }
    return NULL;
}

int ksym_count(void)
{
    return (int)(__ksyms_end - __ksyms_start);
}
