#pragma once

/* Kernel symbol export table.
 *
 * EXPORT_SYMBOL(sym) emits one { name, address } record into the .ksyms
 * output section (see kernel/linker.ld, which brackets it with
 * __ksyms_start / __ksyms_end). The module loader resolves every SHN_UNDEF
 * symbol in a .kmod against this table and against nothing else, so the
 * export list *is* the kernel's module-facing API: a function that is not
 * exported cannot be reached from a module, however global its linkage.
 *
 * The declarations live next to each function's own header include in
 * kernel/ksyms.c rather than being scattered through the subsystems, so
 * the whole API surface stays reviewable in one file. EXPORT_SYMBOL only
 * needs the symbol declared, never defined here.
 *
 * A weak extern may be exported: if the provider is not compiled in, the
 * record's address is NULL, ksym_lookup() skips it, and a module that
 * needs it fails to load with "unresolved symbol" — which is the right
 * answer, not a silent jump to zero.
 */

struct ksym {
    const char *name;
    void *addr;
};

#define EXPORT_SYMBOL(sym)                                              \
    static const struct ksym __ksym_ent_##sym                           \
        __attribute__((section(".ksyms"), used, aligned(16))) =         \
        { #sym, (void *)&sym }

/* Address of an exported symbol, or NULL if it is not exported (or is a
 * weak export whose provider was left out of the build). */
void *ksym_lookup(const char *name);

/* Number of records in the export table. */
int ksym_count(void);
