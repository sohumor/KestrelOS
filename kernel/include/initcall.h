#pragma once

/* Ordered initialization without a hand-maintained list in kmain().
 *
 * A subsystem declares when it wants to run:
 *
 *     static int rtl8139_drv_init(void)
 *     {
 *         return driver_register(&rtl8139_driver) < 0 ? -1 : 0;
 *     }
 *     initcall(driver, rtl8139_drv_init);
 *
 * and kmain() only says "now run the driver level". Adding a subsystem
 * stops being an edit to main.c.
 *
 * Levels, in the order they run:
 *
 *   early    the device core and the buses that need no memory allocator.
 *            Runs once paging, the heap and the framebuffer are up.
 *   core     bus enumeration: walking PCI configuration space, declaring
 *            the fixed platform devices.
 *   driver   driver_register() calls. Every device the buses found already
 *            exists, so drivers bind during this level.
 *   late     things that want a fully populated device tree.
 *
 * Order *within* a level is link order and therefore incidental. Two
 * initcalls that must run in a fixed order belong in different levels, or
 * in one initcall that calls both. The only in-level ordering the kernel
 * currently leans on is ps2kbd before ps2mouse (kernel/keyboard.c sorts
 * before kernel/mouse.c), and the PS/2 mouse probe copes either way.
 *
 * An initcall returns 0 for success. A non-zero return is logged and
 * ignored: a driver that finds no hardware is not a reason to stop
 * booting. Nothing here may panic.
 *
 * ---- the two collection mechanisms ----
 *
 * By default each initcall() drops a function pointer into a per-level
 * linker section and kernel/initcall.c walks the section between the
 * start/end symbols kernel/linker.ld provides. This is the mechanism the
 * kernel is expected to use, and the only one that lets a file declare an
 * initcall without anything else knowing it exists.
 *
 * Building with -DINITCALL_STATIC_TABLE selects the fallback: initcall()
 * emits a plain global pointer variable and kernel/initcall.c holds an
 * explicit table naming every one of them. No linker script support is
 * needed, at the cost of a second place to edit. The table's references
 * are weak, so a row naming a subsystem that is not compiled in is
 * skipped rather than failing the link. It exists so that a deferred or
 * conflicting linker.ld change cannot leave the kernel unbootable.
 */

#define INITCALL_EARLY   0
#define INITCALL_CORE    1
#define INITCALL_DRIVER  2
#define INITCALL_LATE    3
#define INITCALL_LEVELS  4

typedef int (*initcall_fn)(void);

/* initcall(level, fn) where level is the bare token early|core|driver|late.
 * Use it at file scope, terminated with a semicolon. */
#ifdef INITCALL_STATIC_TABLE

/* Fallback: a global pointer with a predictable name. kernel/initcall.c
 * declares it extern and takes its address, so `fn` itself may stay
 * static in the file that defines it. */
#define initcall(level, fn) \
    initcall_fn const __initcall_p_##fn = (fn)

#else

#define initcall(level, fn) \
    static initcall_fn const __initcall_p_##fn \
        __attribute__((used, section(".initcall." #level ".init"))) = (fn)

#endif

#define early_initcall(fn)   initcall(early, fn)
#define core_initcall(fn)    initcall(core, fn)
#define driver_initcall(fn)  initcall(driver, fn)
#define late_initcall(fn)    initcall(late, fn)

/* Run every initcall at one level, in collection order. Returns the number
 * that reported failure. */
int initcall_run_level(int level);

/* Run all four levels in order. Returns the total number of failures. */
int initcall_run_all(void);

/* "early" / "core" / "driver" / "late", or "?" for a bad level. */
const char *initcall_level_name(int level);

/* How many initcalls are registered at a level (0 if the level is empty). */
int initcall_count(int level);
