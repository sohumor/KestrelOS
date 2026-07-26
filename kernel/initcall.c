#include "kernel.h"
#include "initcall.h"

/* Walks the initcall levels. See kernel/include/initcall.h for the two
 * collection mechanisms; this file implements both, selected at compile
 * time by INITCALL_STATIC_TABLE.
 *
 * Nothing here panics. An initcall that fails is a subsystem that is not
 * available, which the rest of the kernel already has to cope with (there
 * has always been the possibility of no NIC and no disk). A failed boot is
 * strictly worse than a missing driver.
 */

static const char *const level_names[INITCALL_LEVELS] = {
    "early", "core", "driver", "late"
};

const char *initcall_level_name(int level)
{
    if (level < 0 || level >= INITCALL_LEVELS)
        return "?";
    return level_names[level];
}

#ifndef INITCALL_STATIC_TABLE

/* ---- linker-section collection (the expected mechanism) --------------- */

/* Provided by kernel/linker.ld. Each pair brackets an array of function
 * pointers emitted by initcall() into .initcall.<level>.init. */
extern initcall_fn __initcall_early_start[],  __initcall_early_end[];
extern initcall_fn __initcall_core_start[],   __initcall_core_end[];
extern initcall_fn __initcall_driver_start[], __initcall_driver_end[];
extern initcall_fn __initcall_late_start[],   __initcall_late_end[];

struct initcall_range {
    initcall_fn *start;
    initcall_fn *end;
};

static const struct initcall_range ranges[INITCALL_LEVELS] = {
    { __initcall_early_start,  __initcall_early_end  },
    { __initcall_core_start,   __initcall_core_end   },
    { __initcall_driver_start, __initcall_driver_end },
    { __initcall_late_start,   __initcall_late_end   },
};

int initcall_count(int level)
{
    if (level < 0 || level >= INITCALL_LEVELS)
        return 0;
    return (int)(ranges[level].end - ranges[level].start);
}

int initcall_run_level(int level)
{
    int failed = 0;

    if (level < 0 || level >= INITCALL_LEVELS)
        return 0;

    for (initcall_fn *p = ranges[level].start; p < ranges[level].end; p++) {
        if (!*p)
            continue;
        if ((*p)() != 0) {
            failed++;
            kprintf("initcall: %s call %d failed\n",
                    initcall_level_name(level),
                    (int)(p - ranges[level].start));
        }
    }
    return failed;
}

#else

/* ---- static table fallback (no linker script support needed) ---------- */

/* Every initcall in the kernel must be named here, and the function's
 * defining file must still carry its initcall() declaration -- that is
 * what creates the __initcall_p_<fn> pointer this table refers to.
 *
 * Referring to the *pointer* rather than the function means the function
 * itself may stay static in its own file, exactly as in the linker-section
 * build, so no driver needs conditional code.
 *
 * The pointers are declared weak, so a row naming a subsystem that is not
 * compiled into this kernel resolves to a NULL address and is skipped
 * instead of failing the link. Without that, the table would be a second
 * place that has to agree with the build configuration, and disagreeing
 * would break the build rather than lose a driver.
 */

#define INITCALL_EXTERN(fn) \
    extern initcall_fn const __initcall_p_##fn __attribute__((weak))
#define INITCALL_ENTRY(lvl, fn) { (lvl), &__initcall_p_##fn, #fn }

INITCALL_EXTERN(bus_platform_init);
INITCALL_EXTERN(bus_pci_init);
INITCALL_EXTERN(ps2kbd_drv_init);
INITCALL_EXTERN(ps2mouse_drv_init);
INITCALL_EXTERN(rtl8139_drv_init);
INITCALL_EXTERN(e1000_drv_init);

struct initcall_static {
    int level;
    initcall_fn const *slot;
    const char *name;
};

static const struct initcall_static static_table[] = {
    INITCALL_ENTRY(INITCALL_EARLY,  bus_platform_init),
    INITCALL_ENTRY(INITCALL_CORE,   bus_pci_init),
    /* Order within a level is this table's order here, which is the one
     * place the fallback is better behaved than the linker sections:
     * ps2kbd before ps2mouse is written down rather than inherited from
     * the alphabet. */
    INITCALL_ENTRY(INITCALL_DRIVER, ps2kbd_drv_init),
    INITCALL_ENTRY(INITCALL_DRIVER, ps2mouse_drv_init),
    INITCALL_ENTRY(INITCALL_DRIVER, rtl8139_drv_init),
    INITCALL_ENTRY(INITCALL_DRIVER, e1000_drv_init),
};

#define STATIC_TABLE_LEN \
    ((int)(sizeof(static_table) / sizeof(static_table[0])))

int initcall_count(int level)
{
    int n = 0;

    for (int i = 0; i < STATIC_TABLE_LEN; i++)
        if (static_table[i].level == level && static_table[i].slot)
            n++;
    return n;
}

int initcall_run_level(int level)
{
    int failed = 0;

    if (level < 0 || level >= INITCALL_LEVELS)
        return 0;

    for (int i = 0; i < STATIC_TABLE_LEN; i++) {
        if (static_table[i].level != level)
            continue;
        /* NULL slot: the weak symbol is unresolved, i.e. that subsystem
         * is not part of this kernel. Not an error. */
        if (!static_table[i].slot)
            continue;
        initcall_fn fn = *static_table[i].slot;
        if (!fn)
            continue;
        if (fn() != 0) {
            failed++;
            kprintf("initcall: %s call %s failed\n",
                    initcall_level_name(level), static_table[i].name);
        }
    }
    return failed;
}

#endif /* INITCALL_STATIC_TABLE */

int initcall_run_all(void)
{
    int failed = 0;

    for (int lvl = 0; lvl < INITCALL_LEVELS; lvl++)
        failed += initcall_run_level(lvl);
    return failed;
}
