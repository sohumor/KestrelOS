#include "kernel.h"
#include "elf.h"
#include "vmm.h"
#include "proc.h"
#include "vfs.h"
#include "vm.h"
#include "kheap.h"
#include "string.h"
#include "kestrel_abi.h"

/* Lowest half of the canonical address space is userland. */
#define USER_VA_LIMIT 0x0000800000000000ULL
#define USER_STACK_LIMIT \
    (USER_STACK_TOP - (uint64_t)USER_STACK_PAGES * PAGE_SIZE)

#define ELF_MAX_PHNUM       64
#define ELF_MAX_IMAGE_PAGES 8192        /* 32 MiB per object */
#define ELF_MAX_FILE_SIZE   (16U * 1024 * 1024)

/* The main PIE and its shared objects receive deterministic, non-overlapping
 * bases. ASLR can randomize these later without changing relocation logic. */
#define ELF_PIE_BASE        0x00400000ULL
#define ELF_DSO_BASE        0x10000000ULL
#define ELF_DSO_STRIDE      0x02000000ULL
#define ELF_MAX_OBJECTS     VM_MAX_FILES
#define ELF_MAX_NEEDED      8

#define ELF_ET_DYN          3
#define ELF_PT_DYNAMIC      2
#define ELF_SHN_UNDEF       0

#define DT_NULL             0
#define DT_NEEDED           1
#define DT_PLTRELSZ         2
#define DT_HASH             4
#define DT_STRTAB           5
#define DT_SYMTAB           6
#define DT_RELA             7
#define DT_RELASZ           8
#define DT_RELAENT          9
#define DT_STRSZ            10
#define DT_SYMENT           11
#define DT_PLTREL           20
#define DT_JMPREL           23

#define R_X86_64_NONE       0
#define R_X86_64_64         1
#define R_X86_64_GLOB_DAT   6
#define R_X86_64_JUMP_SLOT  7
#define R_X86_64_RELATIVE   8

#define STB_WEAK            2

struct elf64_dyn {
    int64_t d_tag;
    uint64_t d_val;
} __attribute__((packed));

struct elf64_sym {
    uint32_t st_name;
    uint8_t st_info;
    uint8_t st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} __attribute__((packed));

struct elf64_rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
} __attribute__((packed));

struct dyn_object {
    const uint8_t *image;
    uint8_t *owned_image;
    size_t size;
    const struct elf64_ehdr *eh;
    struct file *backing;
    uint64_t base;
    char name[64];

    uint64_t strtab;
    uint64_t strsz;
    uint64_t symtab;
    uint64_t syment;
    uint64_t hash;
    uint64_t rela;
    uint64_t relasz;
    uint64_t relaent;
    uint64_t jmprel;
    uint64_t pltrelsz;
    uint64_t pltrel;
    uint64_t needed[ELF_MAX_NEEDED];
    int needed_count;
    uint32_t sym_count;
};

static int ehdr_valid(const struct elf64_ehdr *eh)
{
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return 0;
    if (eh->e_ident[4] != ELF_CLASS64 || eh->e_ident[5] != ELF_DATA2LSB)
        return 0;
    if ((eh->e_type != ELF_ET_EXEC && eh->e_type != ELF_ET_DYN) ||
        eh->e_machine != ELF_EM_X86_64)
        return 0;
    if (eh->e_phentsize != sizeof(struct elf64_phdr))
        return 0;
    if (eh->e_phnum == 0 || eh->e_phnum > ELF_MAX_PHNUM)
        return 0;
    return 1;
}

static const struct elf64_phdr *phdr_at(const struct dyn_object *o, int i)
{
    return (const struct elf64_phdr *)
        (o->image + o->eh->e_phoff + (uint64_t)i * sizeof(struct elf64_phdr));
}

/* Translate an object-relative virtual address into its bytes in the ELF
 * file. Only the file-backed part of PT_LOAD is eligible. */
static const void *object_ptr(const struct dyn_object *o, uint64_t va,
                              uint64_t need)
{
    if (va + need < va)
        return NULL;
    for (int i = 0; i < o->eh->e_phnum; i++) {
        const struct elf64_phdr *ph = phdr_at(o, i);
        if (ph->p_type != ELF_PT_LOAD)
            continue;
        if (va < ph->p_vaddr || va + need > ph->p_vaddr + ph->p_filesz)
            continue;
        uint64_t off = ph->p_offset + (va - ph->p_vaddr);
        if (off <= o->size && need <= o->size - off)
            return o->image + off;
    }
    return NULL;
}

static int register_segments(struct task *task, struct dyn_object *o,
                             uint64_t *max_end)
{
    uint64_t pages = 0;
    int loads = 0;

    if (o->size < sizeof(*o->eh) || !ehdr_valid(o->eh))
        return -1;
    if (o->eh->e_phoff > o->size ||
        (uint64_t)o->eh->e_phnum * sizeof(struct elf64_phdr) >
            o->size - o->eh->e_phoff)
        return -1;

    for (int i = 0; i < o->eh->e_phnum; i++) {
        const struct elf64_phdr *ph = phdr_at(o, i);
        if (ph->p_type != ELF_PT_LOAD || ph->p_memsz == 0)
            continue;
        if (ph->p_filesz > ph->p_memsz ||
            ph->p_offset > o->size ||
            ph->p_filesz > o->size - ph->p_offset)
            return -1;
        if (ph->p_vaddr + ph->p_memsz < ph->p_vaddr ||
            o->base + ph->p_vaddr < o->base)
            return -1;

        uint64_t start = o->base + ph->p_vaddr;
        uint64_t end = start + ph->p_memsz;
        if (start < PAGE_SIZE || end < start || end >= USER_VA_LIMIT ||
            end > USER_STACK_LIMIT)
            return -1;

        pages += ((end + PAGE_SIZE - 1) / PAGE_SIZE) -
                 (start / PAGE_SIZE);
        if (pages > ELF_MAX_IMAGE_PAGES ||
            task->vm_area_count >= VM_MAX_AREAS)
            return -1;

        struct vm_area *area = &task->vm_areas[task->vm_area_count++];
        area->start = start;
        area->end = end;
        area->file_offset = ph->p_offset;
        area->file_size = ph->p_filesz;
        area->pte_flags = (ph->p_flags & ELF_PF_W) ? PTE_W : 0;
        area->backing = o->backing;
        if (end > *max_end)
            *max_end = end;
        loads++;
    }
    return loads ? 0 : -1;
}

static int parse_dynamic(struct dyn_object *o)
{
    const struct elf64_phdr *dynamic = NULL;

    o->syment = sizeof(struct elf64_sym);
    o->relaent = sizeof(struct elf64_rela);
    for (int i = 0; i < o->eh->e_phnum; i++) {
        const struct elf64_phdr *ph = phdr_at(o, i);
        if (ph->p_type == ELF_PT_DYNAMIC) {
            dynamic = ph;
            break;
        }
    }
    if (!dynamic)
        return 0;                       /* ordinary static ET_EXEC */
    if (dynamic->p_offset > o->size ||
        dynamic->p_filesz > o->size - dynamic->p_offset ||
        dynamic->p_filesz % sizeof(struct elf64_dyn))
        return -1;

    const struct elf64_dyn *dyn =
        (const struct elf64_dyn *)(o->image + dynamic->p_offset);
    uint64_t count = dynamic->p_filesz / sizeof(*dyn);
    for (uint64_t i = 0; i < count && dyn[i].d_tag != DT_NULL; i++) {
        switch (dyn[i].d_tag) {
        case DT_NEEDED:
            if (o->needed_count >= ELF_MAX_NEEDED)
                return -1;
            o->needed[o->needed_count++] = dyn[i].d_val;
            break;
        case DT_HASH:     o->hash = dyn[i].d_val; break;
        case DT_STRTAB:   o->strtab = dyn[i].d_val; break;
        case DT_STRSZ:    o->strsz = dyn[i].d_val; break;
        case DT_SYMTAB:   o->symtab = dyn[i].d_val; break;
        case DT_SYMENT:   o->syment = dyn[i].d_val; break;
        case DT_RELA:     o->rela = dyn[i].d_val; break;
        case DT_RELASZ:   o->relasz = dyn[i].d_val; break;
        case DT_RELAENT:  o->relaent = dyn[i].d_val; break;
        case DT_JMPREL:   o->jmprel = dyn[i].d_val; break;
        case DT_PLTRELSZ: o->pltrelsz = dyn[i].d_val; break;
        case DT_PLTREL:   o->pltrel = dyn[i].d_val; break;
        default: break;
        }
    }

    if (o->syment != sizeof(struct elf64_sym) ||
        o->relaent != sizeof(struct elf64_rela))
        return -1;
    if ((o->needed_count || o->symtab) &&
        (!o->strtab || !o->strsz))
        return -1;
    if (o->strtab && !object_ptr(o, o->strtab, o->strsz))
        return -1;
    if (o->hash) {
        const uint32_t *hash = object_ptr(o, o->hash, 8);
        if (!hash)
            return -1;
        o->sym_count = hash[1];          /* SysV hash nchain */
        if (o->sym_count &&
            !object_ptr(o, o->symtab,
                        (uint64_t)o->sym_count * sizeof(struct elf64_sym)))
            return -1;
    }
    if ((o->rela && (!o->relasz ||
                     !object_ptr(o, o->rela, o->relasz))) ||
        (o->jmprel && (!o->pltrelsz ||
                       o->pltrel != DT_RELA ||
                       !object_ptr(o, o->jmprel, o->pltrelsz))))
        return -1;
    return 0;
}

static const char *dyn_string(const struct dyn_object *o, uint64_t off)
{
    const char *strs = object_ptr(o, o->strtab, o->strsz);
    if (!strs || off >= o->strsz)
        return NULL;
    const char *s = strs + off;
    for (uint64_t i = off; i < o->strsz; i++)
        if (strs[i] == '\0')
            return s;
    return NULL;
}

static int slurp_dynamic(const char *path, uint8_t **image_out,
                         size_t *size_out, struct file **file_out)
{
    struct k_stat st;
    struct file *f;
    uint8_t *buf;
    uint32_t done = 0;

    if (vfs_stat(path, &st) < 0 || st.is_dir || st.size == 0 ||
        st.size > ELF_MAX_FILE_SIZE)
        return -1;
    f = vfs_open(path, O_RDONLY);
    if (!f)
        return -1;
    buf = kmalloc(st.size);
    if (!buf) {
        vfs_close(f);
        return -1;
    }
    while (done < st.size) {
        long n = vfs_read(f, buf + done, st.size - done);
        if (n <= 0)
            break;
        done += (uint32_t)n;
    }
    if (done != st.size || vfs_seek(f, 0, 0) < 0) {
        kfree(buf);
        vfs_close(f);
        return -1;
    }
    *image_out = buf;
    *size_out = st.size;
    *file_out = f;
    return 0;
}

static int object_named(struct dyn_object *objects, int count,
                        const char *name)
{
    for (int i = 1; i < count; i++)
        if (strcmp(objects[i].name, name) == 0)
            return i;
    return -1;
}

static const struct elf64_sym *object_sym(const struct dyn_object *o,
                                          uint32_t index)
{
    if (!o->symtab || index >= o->sym_count)
        return NULL;
    return (const struct elf64_sym *)
        object_ptr(o, o->symtab + (uint64_t)index * o->syment,
                   sizeof(struct elf64_sym));
}

static int resolve_symbol(struct dyn_object *objects, int count,
                          struct dyn_object *from, uint32_t index,
                          uint64_t *value)
{
    const struct elf64_sym *wanted = object_sym(from, index);
    if (!wanted)
        return -1;
    if (wanted->st_shndx != ELF_SHN_UNDEF) {
        *value = from->base + wanted->st_value;
        return 0;
    }

    const char *name = dyn_string(from, wanted->st_name);
    if (!name)
        return -1;
    for (int i = 0; i < count; i++) {
        struct dyn_object *o = &objects[i];
        for (uint32_t s = 1; s < o->sym_count; s++) {
            const struct elf64_sym *candidate = object_sym(o, s);
            if (!candidate || candidate->st_shndx == ELF_SHN_UNDEF)
                continue;
            const char *cname = dyn_string(o, candidate->st_name);
            if (cname && strcmp(cname, name) == 0) {
                *value = o->base + candidate->st_value;
                return 0;
            }
        }
    }
    if ((wanted->st_info >> 4) == STB_WEAK) {
        *value = 0;
        return 0;
    }
    kprintf("elf: unresolved dynamic symbol %s\n", name);
    return -1;
}

static int relocate_table(struct task *task, struct dyn_object *objects,
                          int object_count, struct dyn_object *o,
                          uint64_t table_va, uint64_t table_size)
{
    if (!table_va || !table_size)
        return 0;
    if (table_size % sizeof(struct elf64_rela))
        return -1;
    const struct elf64_rela *rela =
        object_ptr(o, table_va, table_size);
    if (!rela)
        return -1;

    uint64_t count = table_size / sizeof(*rela);
    for (uint64_t i = 0; i < count; i++) {
        uint32_t type = (uint32_t)rela[i].r_info;
        uint32_t sym_index = (uint32_t)(rela[i].r_info >> 32);
        uint64_t target = o->base + rela[i].r_offset;
        uint64_t value;

        switch (type) {
        case R_X86_64_NONE:
            continue;
        case R_X86_64_RELATIVE:
            value = o->base + (uint64_t)rela[i].r_addend;
            break;
        case R_X86_64_64:
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT: {
            uint64_t symbol;
            if (resolve_symbol(objects, object_count, o, sym_index,
                               &symbol) < 0)
                return -1;
            value = symbol + (uint64_t)rela[i].r_addend;
            break;
        }
        default:
            kprintf("elf: unsupported x86-64 relocation %u in %s\n",
                    type, o->name);
            return -1;
        }

        if (target + sizeof(uint64_t) < target ||
            vm_fault_in_range(task, target, sizeof(uint64_t), 1) < 0)
            return -1;
        *(uint64_t *)target = value;
    }
    return 0;
}

static int dynamic_link(struct task *task, struct dyn_object *objects,
                        int *object_count, uint64_t *max_end)
{
    int count = *object_count;

    /* Breadth-first DT_NEEDED expansion permits dependencies of libraries,
     * while exact-name deduplication keeps one global instance of each. */
    for (int oi = 0; oi < count; oi++) {
        struct dyn_object *o = &objects[oi];
        if (parse_dynamic(o) < 0)
            return -1;
        for (int ni = 0; ni < o->needed_count; ni++) {
            const char *name = dyn_string(o, o->needed[ni]);
            if (!name || strchr(name, '/') || strlen(name) >= 60)
                return -1;
            if (object_named(objects, count, name) >= 0)
                continue;
            if (count >= ELF_MAX_OBJECTS ||
                task->vm_file_count >= VM_MAX_FILES)
                return -1;

            char path[80] = "/lib/";
            strncpy(path + 5, name, sizeof(path) - 6);
            struct dyn_object *dep = &objects[count];
            memset(dep, 0, sizeof(*dep));
            if (slurp_dynamic(path, &dep->owned_image, &dep->size,
                              &dep->backing) < 0) {
                kprintf("elf: cannot load DT_NEEDED %s\n", path);
                return -1;
            }
            dep->image = dep->owned_image;
            dep->eh = (const struct elf64_ehdr *)dep->image;
            dep->base = ELF_DSO_BASE +
                        (uint64_t)(count - 1) * ELF_DSO_STRIDE;
            strncpy(dep->name, name, sizeof(dep->name) - 1);
            if (!ehdr_valid(dep->eh) || dep->eh->e_type != ELF_ET_DYN)
                return -1;

            task->vm_files[task->vm_file_count++] = dep->backing;
            if (register_segments(task, dep, max_end) < 0)
                return -1;
            count++;
        }
    }

    for (int i = 0; i < count; i++) {
        struct dyn_object *o = &objects[i];
        if (relocate_table(task, objects, count, o,
                           o->rela, o->relasz) < 0 ||
            relocate_table(task, objects, count, o,
                           o->jmprel, o->pltrelsz) < 0)
            return -1;
    }
    *object_count = count;
    return 0;
}

int elf_load(struct task *task, struct file *backing,
             const void *image, size_t size,
             uint64_t *entry_out, uint64_t *brk_out)
{
    struct dyn_object objects[ELF_MAX_OBJECTS];
    int object_count = 1;
    int result = -1;
    uint64_t max_end = 0;

    if (!task || !backing || !image || !entry_out || !brk_out ||
        size < sizeof(struct elf64_ehdr))
        return -1;
    memset(objects, 0, sizeof(objects));
    objects[0].image = image;
    objects[0].size = size;
    objects[0].eh = image;
    objects[0].backing = backing;
    strncpy(objects[0].name, "<main>", sizeof(objects[0].name) - 1);

    if (!ehdr_valid(objects[0].eh))
        goto out;
    objects[0].base = objects[0].eh->e_type == ELF_ET_DYN
                    ? ELF_PIE_BASE : 0;
    if (register_segments(task, &objects[0], &max_end) < 0)
        goto out;

    uint64_t entry = objects[0].base + objects[0].eh->e_entry;
    if (entry < PAGE_SIZE || entry >= USER_VA_LIMIT)
        goto out;
    int entry_mapped = 0;
    for (int i = 0; i < task->vm_area_count; i++)
        if (entry >= task->vm_areas[i].start &&
            entry < task->vm_areas[i].end)
            entry_mapped = 1;
    if (!entry_mapped)
        goto out;

    if (dynamic_link(task, objects, &object_count, &max_end) < 0)
        goto out;

    *entry_out = entry;
    *brk_out = (max_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    kprintf("elf: %s image, %d object%s, %d lazy segment%s\n",
            objects[0].eh->e_type == ELF_ET_DYN ? "dynamic" : "static",
            object_count, object_count == 1 ? "" : "s",
            task->vm_area_count, task->vm_area_count == 1 ? "" : "s");
    result = 0;

out:
    for (int i = 1; i < ELF_MAX_OBJECTS; i++)
        if (objects[i].owned_image)
            kfree(objects[i].owned_image);
    return result;
}
