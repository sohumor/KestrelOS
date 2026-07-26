#pragma once

#include <stdint.h>

/* Loadable kernel modules.
 *
 * A module is a relocatable ELF64 object (ET_REL) compiled separately from
 * the kernel and loaded at runtime by kernel/module.c. It reaches the
 * kernel only through the exported symbol table (kernel/include/export.h);
 * anything it references that is not exported is a hard load failure.
 *
 * Build a module exactly like the kernel (-mcmodel=kernel, -fno-pic,
 * -fno-common, no SSE, no unwind tables) and do not link it — the .o *is*
 * the .kmod. See docs/modules.md.
 */

#define MODULE_NAME_MAX 32
#define MODULE_DESC_MAX 64

/* --- what a module declares about itself ------------------------------ */

/* One tagged field of a module's declaration. MODULE_NAME/_DESC/_INIT/
 * _EXIT each emit one of these into the .kmod_decl section; the loader
 * finds that section by name and merges the records it holds. Splitting
 * the declaration into tagged records instead of one fixed struct makes
 * the four macros order-independent, lets a module omit any of them, and
 * keeps a future MODULE_VERSION readable by today's loader (which simply
 * ignores tags it does not know). */
struct module_decl {
    uint32_t tag;
    uint32_t reserved;
    void *value;                 /* string or function pointer, per tag */
};

#define MODDECL_NAME 1
#define MODDECL_DESC 2
#define MODDECL_INIT 3
#define MODDECL_EXIT 4

#define MODULE_DECL_SECTION ".kmod_decl"

#define __KMOD_DECL(id, dtag, val)                                        \
    static const struct module_decl __kmod_decl_##id                      \
        __attribute__((section(MODULE_DECL_SECTION), used, aligned(16)))  \
        = { (dtag), 0, (void *)(val) }

/* Module name as insmod/lsmod/rmmod see it. Required. */
#define MODULE_NAME(s) __KMOD_DECL(name, MODDECL_NAME, (s))

/* One-line human description. Optional. */
#define MODULE_DESC(s) __KMOD_DECL(desc, MODDECL_DESC, (s))

/* int fn(void): run after relocation. Non-zero aborts the load and the
 * module is torn down immediately. Optional. */
#define MODULE_INIT(f) __KMOD_DECL(init, MODDECL_INIT, (f))

/* void fn(void): run by rmmod before the module's memory is freed. A
 * module without one can still be unloaded. Optional. */
#define MODULE_EXIT(f) __KMOD_DECL(exit, MODDECL_EXIT, (f))

/* --- what the kernel reports about a module --------------------------- */

#define MODULE_STATE_LOADING   0    /* relocated, init has not returned */
#define MODULE_STATE_LIVE      1    /* init returned 0 */
#define MODULE_STATE_UNLOADING 2    /* exit is running */

struct module_info {
    char name[MODULE_NAME_MAX];
    char desc[MODULE_DESC_MAX];
    unsigned long size;          /* bytes of module memory in use */
    int refs;
    int state;                   /* MODULE_STATE_* */
};

/* --- loader API -------------------------------------------------------- */

/* Load a relocatable object already resident in kernel memory. `name` is
 * a fallback used only for diagnostics and only if the image carries no
 * MODULE_NAME. Returns 0, or a negative MODERR_* code. Never panics. */
int module_load(const void *image, unsigned long size, const char *name);

/* Read `path` through the VFS and load it. Returns 0 or a MODERR_* code. */
int module_load_path(const char *path);

/* Run the module's exit function and free it. Fails if refs > 0. */
int module_unload(const char *name);

/* Snapshot of the `index`'th loaded module, newest first. Returns 0, or
 * -1 once `index` is past the end (so callers can just count up). */
int module_list(int index, struct module_info *out);

/* Reference counting. A subsystem that hands out a pointer into a module
 * takes a reference; rmmod refuses while any are outstanding. */
int module_get(const char *name);
int module_put(const char *name);

/* --- failure codes -----------------------------------------------------
 * Every one of these is also reported to the kernel log with the offending
 * symbol / section / relocation named, because "insmod: failed" without a
 * reason is useless when the whole point of the subsystem is late binding. */
#define MODERR_BADELF    -1    /* not a usable x86-64 ET_REL object */
#define MODERR_NOMEM     -2    /* out of module arena or physical memory */
#define MODERR_UNDEF     -3    /* unresolved symbol */
#define MODERR_RELOC     -4    /* unsupported or out-of-range relocation */
#define MODERR_NODECL    -5    /* no .kmod_decl / no MODULE_NAME */
#define MODERR_EXISTS    -6    /* a module of that name is already loaded */
#define MODERR_INIT      -7    /* module_init returned non-zero */
#define MODERR_NOTFOUND  -8    /* no such module loaded */
#define MODERR_BUSY      -9    /* refs > 0 */
#define MODERR_IO       -10    /* could not read the file */
