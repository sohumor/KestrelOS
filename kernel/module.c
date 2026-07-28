/* module.c - the loadable kernel module loader.
 *
 * The ET_REL sibling of elf.c. Where elf.c maps a finished ET_EXEC image
 * into a fresh user address space, this takes a relocatable object,
 * places its sections in kernel memory, binds every undefined symbol
 * against the export table (kernel/ksyms.c), rewrites the relocations,
 * and runs the module's init.
 *
 * The design rule throughout: refuse loudly. A module that cannot be
 * fully bound is not loaded at all, and the reason names the symbol,
 * section or relocation involved. Late binding is only worth having if
 * its failures are legible.
 */

#include "kernel.h"
#include "module.h"
#include "export.h"
#include "elf.h"
#include "kheap.h"
#include "pmm.h"
#include "vmm.h"
#include "vfs.h"
#include "klog.h"
#include "proc.h"
#include "string.h"
#include "kestrel_abi.h"
#include "spinlock.h"

/* --- ET_REL definitions -------------------------------------------------
 * kernel/include/elf.h covers the ET_EXEC program-header view and belongs
 * to the user-program loader; the section/symbol/relocation view is only
 * ever needed here, so it lives here. */

#define ELF_ET_REL 1

#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_NOBITS   8

#define SHF_ALLOC 0x2

#define SHN_UNDEF   0
#define SHN_ABS     0xFFF1
#define SHN_COMMON  0xFFF2
#define SHN_XINDEX  0xFFFF

#define R_X86_64_NONE  0
#define R_X86_64_64    1
#define R_X86_64_PC32  2
#define R_X86_64_PLT32 4
#define R_X86_64_32    10
#define R_X86_64_32S   11

struct elf64_shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} __attribute__((packed));

struct elf64_sym {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} __attribute__((packed));

struct elf64_rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} __attribute__((packed));

#define MODULE_MAX_SECTIONS 96
#define MODULE_MAX_SYMS     4096
#define MODULE_MAX_IMAGE    (1024 * 1024)   /* relocated bytes per module */
#define MODULE_MAX_FILE     (1024 * 1024)   /* .kmod file size */

#define MOD_S32_MIN (-2147483648LL)
#define MOD_S32_MAX (2147483647LL)

/* --- module memory ------------------------------------------------------
 *
 * Modules cannot live in kmalloc memory, and this is the one genuinely
 * hard problem in the subsystem. kmalloc returns direct-map addresses at
 * 0xFFFF800000000000; the kernel text is at 0xFFFFFFFF80100000. That is
 * roughly 128 TiB apart, so *every* R_X86_64_PC32 / PLT32 call from a
 * module into an exported kernel function would overflow its signed
 * 32-bit displacement, and the R_X86_64_32S references that
 * -mcmodel=kernel emits for the module's own data would not sign-extend
 * back to the right address either. A loader that ignored this would
 * produce modules that appear to load and then jump into nothing.
 *
 * So module memory comes from a dedicated 4 MiB virtual window placed
 * immediately above the 16 MiB that vmm_init() maps for the kernel image,
 * at KERNEL_OFFSET + 16 MiB. Any two addresses inside the top 2 GiB are
 * within PC32 reach of each other, and any address in that window
 * truncates to a 32-bit value that sign-extends back exactly, so both
 * relocation families are correct by construction rather than by luck.
 * The 32-bit forms are still range-checked on every entry: an oversized
 * module or a bad object must be refused, never silently truncated.
 *
 * Backing frames come from the PMM and are mapped into the kernel PML4.
 * Entry 511 of every address space is a copy of the kernel's (see
 * vmm_new_pml4), and it points at the same PDPT page, so a mapping made
 * here becomes visible to every process without any further work. Frames
 * are mapped on first use and then kept: unmapping would mean tearing
 * down page tables shared with every live address space, for no gain at
 * a 4 MiB ceiling. Only the arena's page accounting is reclaimed.
 */

#define MOD_ARENA_BASE  (KERNEL_OFFSET + 0x01000000ULL)   /* +16 MiB */
#define MOD_ARENA_PAGES 1024                              /* 4 MiB */
#define MOD_PMM_RESERVE 256      /* frames kept back so a load cannot panic */

static uint8_t  arena_used[MOD_ARENA_PAGES];
static uint8_t  arena_mapped[MOD_ARENA_PAGES];
static uint16_t arena_run[MOD_ARENA_PAGES];
static spinlock_t arena_lock = SPINLOCK_INIT;

/* The page bitmap is the only shared state, so claiming a run runs with
 * under the arena lock; mapping the frames and zeroing up to 4 MiB does
 * not, because the run belongs to this caller by then. */
static void *arena_alloc(unsigned long size)
{
    unsigned long need = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    unsigned long start = 0, run = 0, want = 0, i;
    uint64_t base, f;

    if (need == 0 || need > MOD_ARENA_PAGES)
        return NULL;

    f = spin_lock_irqsave(&arena_lock);
    for (i = 0; i < MOD_ARENA_PAGES; i++) {
        if (arena_used[i]) {
            run = 0;
            continue;
        }
        if (run == 0)
            start = i;
        if (++run == need)
            break;
    }
    if (run != need) {
        spin_unlock_irqrestore(&arena_lock, f);
        return NULL;
    }

    /* Count the frames this run still needs before claiming anything:
     * pmm_alloc() panics on exhaustion, and a module failing to load must
     * never take the kernel down with it. */
    for (i = start; i < start + need; i++)
        if (!arena_mapped[i])
            want++;
    if (want + MOD_PMM_RESERVE > pmm_free_pages()) {
        spin_unlock_irqrestore(&arena_lock, f);
        return NULL;
    }

    for (i = start; i < start + need; i++) {
        arena_used[i] = 1;
        arena_run[i] = 0;
    }
    arena_run[start] = (uint16_t)need;
    spin_unlock_irqrestore(&arena_lock, f);

    base = MOD_ARENA_BASE + (uint64_t)start * PAGE_SIZE;
    for (i = start; i < start + need; i++) {
        if (!arena_mapped[i]) {
            uint64_t phys = pmm_alloc();
            vmm_map_page(vmm_kernel_pml4(),
                         MOD_ARENA_BASE + (uint64_t)i * PAGE_SIZE,
                         phys, PTE_W);
            arena_mapped[i] = 1;
        }
    }
    memset((void *)base, 0, need * PAGE_SIZE);
    return (void *)base;
}

static void arena_free(void *p)
{
    uint64_t a = (uint64_t)p, f;
    unsigned long idx, n, i;

    if (!p)
        return;
    if (a < MOD_ARENA_BASE ||
        a >= MOD_ARENA_BASE + (uint64_t)MOD_ARENA_PAGES * PAGE_SIZE)
        panic("module: arena_free(%p) outside the module window", p);
    idx = (unsigned long)((a - MOD_ARENA_BASE) / PAGE_SIZE);

    f = spin_lock_irqsave(&arena_lock);
    n = arena_run[idx];
    if (n == 0) {
        spin_unlock_irqrestore(&arena_lock, f);
        panic("module: arena_free(%p) is not an allocation start", p);
    }
    for (i = idx; i < idx + n; i++)
        arena_used[i] = 0;
    arena_run[idx] = 0;
    spin_unlock_irqrestore(&arena_lock, f);
}

/* --- loaded module registry -------------------------------------------- */

struct module {
    char name[MODULE_NAME_MAX];
    char desc[MODULE_DESC_MAX];
    void *base;
    unsigned long size;
    int refs;
    int state;
    int (*init)(void);
    void (*exit)(void);
    struct module *next;
};

static struct module *modules;      /* newest first */
static spinlock_t module_registry_lock = SPINLOCK_INIT;

/* insmod/rmmod are rare administrative operations, so one flag is enough
 * to keep two of them from interleaving arena allocation and the name
 * check. It is never held across the module's own init/exit. */
static volatile int module_busy;

static int module_lock(void)
{
    int expected = 0;
    return __atomic_compare_exchange_n(&module_busy, &expected, 1, false,
                                       __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
}

static void module_unlock(void)
{
    __atomic_store_n(&module_busy, 0, __ATOMIC_RELEASE);
}

static struct module *module_find(const char *name)
{
    struct module *m;

    for (m = modules; m; m = m->next)
        if (strcmp(m->name, name) == 0)
            return m;
    return NULL;
}

/* --- ELF helpers -------------------------------------------------------- */

struct load_ctx {
    const uint8_t *img;
    unsigned long size;
    const struct elf64_shdr *sh;
    int shnum;
    const struct elf64_sym *sym;
    int nsym;
    const char *strtab;
    unsigned long strtab_size;
    const char *shstr;
    unsigned long shstr_size;
    uint64_t secaddr[MODULE_MAX_SECTIONS];   /* 0 = section not allocated */
    char modname[MODULE_NAME_MAX];           /* best name known so far */
};

/* A string table entry, or NULL if the offset escapes the table. Callers
 * may rely on the result being NUL-terminated: the table itself is
 * checked for a trailing NUL when it is adopted. */
static const char *str_at(const char *tab, unsigned long size, uint32_t off)
{
    if (!tab || (unsigned long)off >= size)
        return NULL;
    return tab + off;
}

static const char *sec_name(const struct load_ctx *c, int i)
{
    const char *s = str_at(c->shstr, c->shstr_size, c->sh[i].sh_name);
    return s ? s : "?";
}

static int ehdr_ok(const struct elf64_ehdr *eh)
{
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return 0;
    if (eh->e_ident[4] != ELF_CLASS64 || eh->e_ident[5] != ELF_DATA2LSB)
        return 0;
    if (eh->e_type != ELF_ET_REL || eh->e_machine != ELF_EM_X86_64)
        return 0;
    if (eh->e_shentsize != sizeof(struct elf64_shdr))
        return 0;
    if (eh->e_shnum == 0 || eh->e_shnum > MODULE_MAX_SECTIONS)
        return 0;
    if (eh->e_shstrndx == SHN_XINDEX || eh->e_shstrndx >= eh->e_shnum)
        return 0;
    return 1;
}

/* Adopt a string table section: bounds-checked and required to end in a
 * NUL so str_at() results are safe to hand to strcmp/strncpy. */
static int take_strtab(const struct load_ctx *c, int idx,
                       const char **tab, unsigned long *size)
{
    const struct elf64_shdr *s;

    if (idx <= 0 || idx >= c->shnum)
        return -1;
    s = &c->sh[idx];
    if (s->sh_type != SHT_STRTAB || s->sh_size == 0)
        return -1;
    if (c->img[s->sh_offset + s->sh_size - 1] != '\0')
        return -1;
    *tab = (const char *)(c->img + s->sh_offset);
    *size = (unsigned long)s->sh_size;
    return 0;
}

/* Parse and validate the section table, symbol table and string tables.
 * Every offset in the file is range-checked here so nothing downstream
 * has to. */
static int parse_headers(struct load_ctx *c)
{
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)c->img;
    int i, symidx = -1;

    if (c->size < sizeof(*eh) || !ehdr_ok(eh))
        return MODERR_BADELF;
    if (eh->e_shoff > c->size ||
        (uint64_t)eh->e_shnum * sizeof(struct elf64_shdr) >
            c->size - eh->e_shoff)
        return MODERR_BADELF;

    c->sh = (const struct elf64_shdr *)(c->img + eh->e_shoff);
    c->shnum = eh->e_shnum;

    /* No section may claim file bytes the image does not have. */
    for (i = 0; i < c->shnum; i++) {
        const struct elf64_shdr *s = &c->sh[i];
        if (s->sh_type == SHT_NOBITS)
            continue;
        if (s->sh_offset > c->size || s->sh_size > c->size - s->sh_offset)
            return MODERR_BADELF;
    }

    if (take_strtab(c, eh->e_shstrndx, &c->shstr, &c->shstr_size) < 0)
        return MODERR_BADELF;

    for (i = 0; i < c->shnum; i++) {
        if (c->sh[i].sh_type == SHT_SYMTAB) {
            symidx = i;
            break;
        }
    }
    if (symidx < 0)
        return MODERR_BADELF;
    if (c->sh[symidx].sh_entsize != sizeof(struct elf64_sym) ||
        c->sh[symidx].sh_size == 0)
        return MODERR_BADELF;
    if (take_strtab(c, (int)c->sh[symidx].sh_link,
                    &c->strtab, &c->strtab_size) < 0)
        return MODERR_BADELF;

    c->sym = (const struct elf64_sym *)(c->img + c->sh[symidx].sh_offset);
    c->nsym = (int)(c->sh[symidx].sh_size / sizeof(struct elf64_sym));
    if (c->nsym > MODULE_MAX_SYMS)
        return MODERR_BADELF;
    return 0;
}

/* Lay out every SHF_ALLOC section in one contiguous block, honouring each
 * section's alignment. Returns the total size, or 0 if the module is too
 * big or malformed. Section offsets are stashed in secaddr[] and turned
 * into addresses once the block exists. */
static unsigned long plan_layout(struct load_ctx *c)
{
    unsigned long off = 0;
    int i;

    for (i = 0; i < c->shnum; i++) {
        const struct elf64_shdr *s = &c->sh[i];
        unsigned long align;

        c->secaddr[i] = 0;
        if (!(s->sh_flags & SHF_ALLOC) || s->sh_size == 0)
            continue;
        if (s->sh_type != SHT_PROGBITS && s->sh_type != SHT_NOBITS &&
            s->sh_type != 7 /* SHT_NOTE */)
            continue;

        align = s->sh_addralign ? (unsigned long)s->sh_addralign : 1;
        if (align > PAGE_SIZE)
            align = PAGE_SIZE;      /* the block itself is page aligned */
        if (align & (align - 1))
            return 0;               /* alignment must be a power of two */

        off = (off + align - 1) & ~(align - 1);
        c->secaddr[i] = off + 1;    /* +1: distinguish "offset 0" from "none" */
        if (s->sh_size > MODULE_MAX_IMAGE ||
            off > MODULE_MAX_IMAGE - s->sh_size)
            return 0;
        off += (unsigned long)s->sh_size;
    }
    return off;
}

/* Turn the stashed offsets into addresses and fill the block in: copy
 * PROGBITS, leave NOBITS as the zeroes arena_alloc() already wrote. */
static void place_sections(struct load_ctx *c, uint64_t base)
{
    int i;

    for (i = 0; i < c->shnum; i++) {
        if (!c->secaddr[i])
            continue;
        c->secaddr[i] = base + (c->secaddr[i] - 1);
        if (c->sh[i].sh_type != SHT_NOBITS)
            memcpy((void *)c->secaddr[i], c->img + c->sh[i].sh_offset,
                   (size_t)c->sh[i].sh_size);
    }
}

/* --- symbol resolution -------------------------------------------------- */

/* A module built with `ld -r` from several objects can carry a symbol
 * that is SHN_UNDEF in one entry and defined in another. Look inside the
 * module before falling back to the kernel's export table. */
static uint64_t local_define(const struct load_ctx *c, const char *name)
{
    int i;

    for (i = 0; i < c->nsym; i++) {
        const struct elf64_sym *s = &c->sym[i];
        const char *nm;

        if (s->st_shndx == SHN_UNDEF || s->st_shndx >= (uint16_t)c->shnum)
            continue;
        if (!c->secaddr[s->st_shndx])
            continue;
        nm = str_at(c->strtab, c->strtab_size, s->st_name);
        if (nm && *nm && strcmp(nm, name) == 0)
            return c->secaddr[s->st_shndx] + s->st_value;
    }
    return 0;
}

static int resolve_syms(struct load_ctx *c, uint64_t *symval)
{
    int i;

    for (i = 0; i < c->nsym; i++) {
        const struct elf64_sym *s = &c->sym[i];
        const char *nm = str_at(c->strtab, c->strtab_size, s->st_name);
        uint16_t shndx = s->st_shndx;

        symval[i] = 0;

        if (shndx == SHN_ABS) {
            symval[i] = s->st_value;
            continue;
        }
        if (shndx == SHN_COMMON) {
            kprintf("module: %s: common symbol '%s', build with -fno-common\n",
                    c->modname, nm ? nm : "?");
            return MODERR_BADELF;
        }
        if (shndx == SHN_UNDEF) {
            uint64_t a;

            if (!nm || !*nm)
                continue;                   /* the null symbol, index 0 */
            a = local_define(c, nm);
            if (!a)
                a = (uint64_t)ksym_lookup(nm);
            if (!a) {
                kprintf("module: %s: unresolved symbol '%s'\n", c->modname, nm);
                klog_printf(K_LOG_ERR, "module",
                            "%s: unresolved symbol '%s'", c->modname, nm);
                return MODERR_UNDEF;
            }
            symval[i] = a;
            continue;
        }
        if (shndx >= (uint16_t)c->shnum)
            return MODERR_BADELF;
        /* Symbols in sections we did not allocate (debug info) stay 0;
         * that is only a problem if a relocation actually uses one, which
         * apply_relocs() catches by name. */
        if (c->secaddr[shndx])
            symval[i] = c->secaddr[shndx] + s->st_value;
    }
    return 0;
}

/* --- relocation --------------------------------------------------------- */

static int apply_relocs(struct load_ctx *c, const uint64_t *symval)
{
    int i;

    for (i = 0; i < c->shnum; i++) {
        const struct elf64_shdr *rs = &c->sh[i];
        const struct elf64_shdr *ts;
        const struct elf64_rela *ra;
        unsigned long n, j;
        uint64_t tbase;

        if (rs->sh_type != SHT_RELA || rs->sh_size == 0)
            continue;
        if (rs->sh_entsize != sizeof(struct elf64_rela))
            return MODERR_BADELF;
        if (rs->sh_info >= (uint32_t)c->shnum)
            return MODERR_BADELF;

        tbase = c->secaddr[rs->sh_info];
        if (!tbase)
            continue;               /* relocations for .debug_* and friends */

        ts = &c->sh[rs->sh_info];
        ra = (const struct elf64_rela *)(c->img + rs->sh_offset);
        n = (unsigned long)(rs->sh_size / sizeof(struct elf64_rela));

        for (j = 0; j < n; j++) {
            uint32_t type = (uint32_t)(ra[j].r_info & 0xFFFFFFFFU);
            uint64_t si = ra[j].r_info >> 32;
            uint64_t off = ra[j].r_offset;
            unsigned long width = (type == R_X86_64_64) ? 8 : 4;
            const char *nm;
            uint64_t S, P;
            int64_t A, v;

            if (type == R_X86_64_NONE)
                continue;
            if (si >= (uint64_t)c->nsym)
                return MODERR_BADELF;
            if (off > ts->sh_size || width > ts->sh_size - off)
                return MODERR_BADELF;

            /* Section symbols carry no name; report the section instead,
             * which is what makes a bad .rodata reference readable. */
            nm = str_at(c->strtab, c->strtab_size, c->sym[si].st_name);
            if (!nm || !*nm) {
                uint16_t sx = c->sym[si].st_shndx;
                nm = sx < (uint16_t)c->shnum ? sec_name(c, (int)sx) : "?";
            }

            S = symval[si];
            A = ra[j].r_addend;
            P = tbase + off;

            if (!S && c->sym[si].st_shndx != SHN_ABS) {
                kprintf("module: %s: relocation against unplaced symbol "
                        "'%s' in %s\n", c->modname, nm, sec_name(c, (int)rs->sh_info));
                return MODERR_RELOC;
            }

            switch (type) {
            case R_X86_64_64:
                *(uint64_t *)P = S + (uint64_t)A;
                break;

            /* PLT32 has no PLT to go through here: the module is bound
             * directly, so it is exactly PC32. */
            case R_X86_64_PC32:
            case R_X86_64_PLT32:
                v = (int64_t)(S + (uint64_t)A) - (int64_t)P;
                if (v < MOD_S32_MIN || v > MOD_S32_MAX) {
                    kprintf("module: %s: PC32 overflow for '%s' "
                            "(displacement %ld)\n", c->modname, nm, (long)v);
                    return MODERR_RELOC;
                }
                *(int32_t *)P = (int32_t)v;
                break;

            case R_X86_64_32:
                if (S + (uint64_t)A > 0xFFFFFFFFULL) {
                    kprintf("module: %s: 32-bit relocation overflow for "
                            "'%s'\n", c->modname, nm);
                    return MODERR_RELOC;
                }
                *(uint32_t *)P = (uint32_t)(S + (uint64_t)A);
                break;

            case R_X86_64_32S:
                v = (int64_t)(S + (uint64_t)A);
                if (v < MOD_S32_MIN || v > MOD_S32_MAX) {
                    kprintf("module: %s: 32S relocation overflow for "
                            "'%s' (%lx)\n", c->modname, nm,
                            (unsigned long)v);
                    return MODERR_RELOC;
                }
                *(int32_t *)P = (int32_t)v;
                break;

            default:
                kprintf("module: %s: unsupported relocation type %u "
                        "against '%s'\n", c->modname, type, nm);
                return MODERR_RELOC;
            }
        }
    }
    return 0;
}

/* --- the module's own declaration --------------------------------------- */

/* Copy a NUL-terminated string that lives inside the module's block. A
 * pointer that escapes the block is a corrupt or hostile object, not a
 * string; refuse it rather than reading kernel memory. */
static int copy_mod_string(const struct module *m, const void *p,
                           char *dst, unsigned long dstsz)
{
    uint64_t a = (uint64_t)p;
    uint64_t lo = (uint64_t)m->base, hi = lo + m->size;
    const char *s = p;
    unsigned long i;

    if (a < lo || a >= hi)
        return -1;
    for (i = 0; i < dstsz - 1 && a + i < hi; i++) {
        if (!s[i])
            break;
        dst[i] = s[i];
    }
    dst[i] = '\0';
    return i ? 0 : -1;
}

static int inside_module(const struct module *m, const void *p)
{
    uint64_t a = (uint64_t)p;
    return a >= (uint64_t)m->base && a < (uint64_t)m->base + m->size;
}

static int read_decl(struct load_ctx *c, struct module *m)
{
    const struct module_decl *d = NULL;
    unsigned long n = 0, j;
    int i;

    for (i = 0; i < c->shnum; i++) {
        if (!c->secaddr[i])
            continue;
        if (strcmp(sec_name(c, i), MODULE_DECL_SECTION) != 0)
            continue;
        d = (const struct module_decl *)c->secaddr[i];
        n = (unsigned long)(c->sh[i].sh_size / sizeof(struct module_decl));
        break;
    }
    if (!d || n == 0) {
        kprintf("module: %s: no %s section, not a kestrel module\n",
                c->modname, MODULE_DECL_SECTION);
        return MODERR_NODECL;
    }

    for (j = 0; j < n; j++) {
        switch (d[j].tag) {
        case MODDECL_NAME:
            if (copy_mod_string(m, d[j].value, m->name, sizeof(m->name)) < 0)
                return MODERR_NODECL;
            break;
        case MODDECL_DESC:
            copy_mod_string(m, d[j].value, m->desc, sizeof(m->desc));
            break;
        case MODDECL_INIT:
            if (!inside_module(m, d[j].value))
                return MODERR_NODECL;
            m->init = (int (*)(void))d[j].value;
            break;
        case MODDECL_EXIT:
            if (!inside_module(m, d[j].value))
                return MODERR_NODECL;
            m->exit = (void (*)(void))d[j].value;
            break;
        default:
            /* A newer module declaring something this loader predates. */
            kprintf("module: %s: ignoring unknown decl tag %u\n",
                    c->modname, d[j].tag);
            break;
        }
    }

    if (!m->name[0]) {
        kprintf("module: %s: no MODULE_NAME\n", c->modname);
        return MODERR_NODECL;
    }
    strncpy(c->modname, m->name, sizeof(c->modname) - 1);
    c->modname[sizeof(c->modname) - 1] = '\0';
    return 0;
}

/* --- public API ---------------------------------------------------------- */

int module_load(const void *image, unsigned long size, const char *name)
{
    struct load_ctx c;
    struct module *m = NULL;
    uint64_t *symval = NULL;
    unsigned long total;
    uint64_t f;
    int rc;

    if (!image || size < sizeof(struct elf64_ehdr) || size > MODULE_MAX_FILE)
        return MODERR_BADELF;

    memset(&c, 0, sizeof(c));
    c.img = image;
    c.size = size;
    strncpy(c.modname, name && *name ? name : "(anonymous)",
            sizeof(c.modname) - 1);

    if (!module_lock())
        return MODERR_BUSY;

    rc = parse_headers(&c);
    if (rc < 0) {
        kprintf("module: %s: not a usable x86-64 relocatable object\n",
                c.modname);
        goto out;
    }

    total = plan_layout(&c);
    if (total == 0) {
        kprintf("module: %s: no loadable sections, or too large\n", c.modname);
        rc = MODERR_BADELF;
        goto out;
    }

    m = kzalloc(sizeof(*m));
    symval = kzalloc((size_t)c.nsym * sizeof(uint64_t));
    if (!m || !symval) {
        rc = MODERR_NOMEM;
        goto out;
    }

    m->base = arena_alloc(total);
    if (!m->base) {
        kprintf("module: %s: no room in the %lu KiB module arena\n",
                c.modname, (unsigned long)(MOD_ARENA_PAGES * PAGE_SIZE / 1024));
        rc = MODERR_NOMEM;
        goto out;
    }
    m->size = total;
    m->state = MODULE_STATE_LOADING;

    place_sections(&c, (uint64_t)m->base);

    rc = resolve_syms(&c, symval);
    if (rc < 0)
        goto out;
    rc = apply_relocs(&c, symval);
    if (rc < 0)
        goto out;
    rc = read_decl(&c, m);
    if (rc < 0)
        goto out;

    f = spin_lock_irqsave(&module_registry_lock);
    if (module_find(m->name)) {
        spin_unlock_irqrestore(&module_registry_lock, f);
        kprintf("module: %s is already loaded\n", m->name);
        rc = MODERR_EXISTS;
        goto out;
    }
    m->next = modules;
    modules = m;
    spin_unlock_irqrestore(&module_registry_lock, f);

    /* The module is registered and visible before init runs, so that init
     * can hand out pointers into itself and have them reference-counted. */
    module_unlock();
    kfree(symval);
    symval = NULL;

    if (m->init && m->init() != 0) {
        kprintf("module: %s: init failed, unloading\n", m->name);
        klog_printf(K_LOG_ERR, "module", "%s: init failed", m->name);
        f = spin_lock_irqsave(&module_registry_lock);
        struct module **failed_link;
        for (failed_link = &modules; *failed_link;
             failed_link = &(*failed_link)->next) {
            if (*failed_link == m) {
                *failed_link = m->next;
                break;
            }
        }
        spin_unlock_irqrestore(&module_registry_lock, f);
        arena_free(m->base);
        kfree(m);
        return MODERR_INIT;
    }

    f = spin_lock_irqsave(&module_registry_lock);
    m->state = MODULE_STATE_LIVE;
    spin_unlock_irqrestore(&module_registry_lock, f);
    kprintf("module: loaded %s (%lu bytes at %p)%s%s\n", m->name, m->size,
            m->base, m->desc[0] ? " - " : "", m->desc);
    klog_printf(K_LOG_INFO, "module", "loaded %s (%lu bytes)",
                m->name, m->size);
    return 0;

out:
    if (m) {
        arena_free(m->base);
        kfree(m);
    }
    kfree(symval);
    module_unlock();
    return rc;
}

/* Read `path` through the VFS into a kmalloc buffer and load it. */
int module_load_path(const char *path)
{
    struct k_stat st;
    struct file *fp;
    uint8_t *buf;
    const char *base;
    uint32_t got = 0;
    int rc;

    if (!path || !*path)
        return MODERR_IO;
    if (vfs_stat(path, &st) < 0 || st.is_dir || st.size == 0 ||
        st.size > MODULE_MAX_FILE) {
        kprintf("module: cannot read %s\n", path);
        return MODERR_IO;
    }
    buf = kmalloc(st.size);
    if (!buf)
        return MODERR_NOMEM;
    fp = vfs_open(path, O_RDONLY);
    if (!fp) {
        kfree(buf);
        kprintf("module: cannot open %s\n", path);
        return MODERR_IO;
    }
    while (got < st.size) {
        long n = vfs_read(fp, buf + got, st.size - got);
        if (n <= 0)
            break;
        got += (uint32_t)n;
    }
    vfs_close(fp);
    if (got != st.size) {
        kfree(buf);
        kprintf("module: short read on %s\n", path);
        return MODERR_IO;
    }

    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    rc = module_load(buf, st.size, base);
    kfree(buf);
    return rc;
}

int module_unload(const char *name)
{
    struct module *m, **pp;
    void (*fn)(void);
    uint64_t f;

    if (!name || !*name)
        return MODERR_NOTFOUND;
    if (!module_lock())
        return MODERR_BUSY;

    f = spin_lock_irqsave(&module_registry_lock);
    m = module_find(name);
    if (!m) {
        spin_unlock_irqrestore(&module_registry_lock, f);
        module_unlock();
        return MODERR_NOTFOUND;
    }
    if (m->refs > 0) {
        int refs = m->refs;
        spin_unlock_irqrestore(&module_registry_lock, f);
        kprintf("module: %s is in use (%d references)\n", m->name, refs);
        module_unlock();
        return MODERR_BUSY;
    }
    if (m->state != MODULE_STATE_LIVE) {
        spin_unlock_irqrestore(&module_registry_lock, f);
        module_unlock();
        return MODERR_BUSY;
    }

    m->state = MODULE_STATE_UNLOADING;
    fn = m->exit;
    spin_unlock_irqrestore(&module_registry_lock, f);
    module_unlock();

    if (fn)
        fn();

    f = spin_lock_irqsave(&module_registry_lock);
    for (pp = &modules; *pp; pp = &(*pp)->next) {
        if (*pp == m) {
            *pp = m->next;
            break;
        }
    }
    spin_unlock_irqrestore(&module_registry_lock, f);

    kprintf("module: unloaded %s\n", m->name);
    klog_printf(K_LOG_INFO, "module", "unloaded %s", m->name);
    arena_free(m->base);
    kfree(m);
    return 0;
}

int module_list(int index, struct module_info *out)
{
    struct module *m;
    int i = 0;
    uint64_t f;

    if (index < 0 || !out)
        return -1;

    f = spin_lock_irqsave(&module_registry_lock);
    for (m = modules; m; m = m->next, i++) {
        if (i != index)
            continue;
        memset(out, 0, sizeof(*out));
        strncpy(out->name, m->name, sizeof(out->name) - 1);
        strncpy(out->desc, m->desc, sizeof(out->desc) - 1);
        out->size = m->size;
        out->refs = m->refs;
        out->state = m->state;
        spin_unlock_irqrestore(&module_registry_lock, f);
        return 0;
    }
    spin_unlock_irqrestore(&module_registry_lock, f);
    return -1;
}

int module_get(const char *name)
{
    struct module *m;
    uint64_t f = spin_lock_irqsave(&module_registry_lock);

    m = name ? module_find(name) : NULL;
    if (m && m->state != MODULE_STATE_UNLOADING)
        m->refs++;
    else
        m = NULL;
    spin_unlock_irqrestore(&module_registry_lock, f);
    return m ? 0 : MODERR_NOTFOUND;
}

int module_put(const char *name)
{
    struct module *m;
    uint64_t f = spin_lock_irqsave(&module_registry_lock);

    m = name ? module_find(name) : NULL;
    if (m && m->refs > 0)
        m->refs--;
    spin_unlock_irqrestore(&module_registry_lock, f);
    return m ? 0 : MODERR_NOTFOUND;
}
