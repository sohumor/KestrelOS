#include "kernel.h"
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "kestrel_abi.h"

/* Lowest half of the canonical address space is userland. */
#define USER_VA_LIMIT 0x0000800000000000ULL

#define ELF_MAX_PHNUM 64

/* p_memsz is attacker-controlled, so the mapped image needs a hard cap and
 * a free-frame margin: pmm_alloc() panics on exhaustion, and elf.h promises
 * elf_load never panics. */
#define ELF_MAX_IMAGE_PAGES 8192          /* 32 MiB per executable */
#define ELF_PMM_RESERVE     512

/* Segments must stay clear of the stack: uproc.c maps the stack pages
 * afterwards, and vmm_map_page overwrites a live PTE without freeing it,
 * so an overlapping segment silently orphans its frames. */
#define USER_STACK_LIMIT \
    (USER_STACK_TOP - (uint64_t)USER_STACK_PAGES * PAGE_SIZE)

static int ehdr_valid(const struct elf64_ehdr *eh)
{
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return 0;
    if (eh->e_ident[4] != ELF_CLASS64 || eh->e_ident[5] != ELF_DATA2LSB)
        return 0;
    if (eh->e_type != ELF_ET_EXEC || eh->e_machine != ELF_EM_X86_64)
        return 0;
    if (eh->e_phentsize != sizeof(struct elf64_phdr))
        return 0;
    if (eh->e_phnum == 0 || eh->e_phnum > ELF_MAX_PHNUM)
        return 0;
    return 1;
}

/* Map every page of a PT_LOAD segment into pml4 and copy the file-backed
 * portion in through the direct map. Freshly allocated frames come back
 * zeroed from pmm_alloc(), which takes care of the p_memsz > p_filesz
 * (bss) tail for free. Pages shared between two segments are reused. */
static int load_segment(uint64_t *pml4, const uint8_t *img,
                        const struct elf64_phdr *ph)
{
    uint64_t seg_lo = ph->p_vaddr;
    uint64_t seg_hi = ph->p_vaddr + ph->p_memsz;
    uint64_t file_hi = ph->p_vaddr + ph->p_filesz;
    uint64_t page = seg_lo & ~(PAGE_SIZE - 1);

    for (; page < seg_hi; page += PAGE_SIZE) {
        uint64_t phys = vmm_virt_to_phys(pml4, page);
        if (!phys) {
            phys = pmm_alloc();
            if (!phys)
                return -1;
            vmm_map_page(pml4, page, phys, PTE_U | PTE_W);
        }

        /* Slice of [seg_lo, file_hi) that falls inside this page. */
        uint64_t lo = seg_lo > page ? seg_lo : page;
        uint64_t hi = file_hi < page + PAGE_SIZE ? file_hi : page + PAGE_SIZE;
        if (lo < hi)
            memcpy((uint8_t *)P2V(phys) + (lo - page),
                   img + ph->p_offset + (lo - seg_lo), hi - lo);
    }
    return 0;
}

int elf_load(uint64_t *pml4, const void *image, size_t size,
             uint64_t *entry_out, uint64_t *brk_out)
{
    const uint8_t *img = image;
    const struct elf64_ehdr *eh = image;
    uint64_t max_end = 0;
    uint64_t total_pages = 0;

    if (!image || size < sizeof(*eh) || !ehdr_valid(eh))
        return -1;
    if (eh->e_phoff > size ||
        (uint64_t)eh->e_phnum * sizeof(struct elf64_phdr) > size - eh->e_phoff)
        return -1;
    if (eh->e_entry == 0 || eh->e_entry >= USER_VA_LIMIT)
        return -1;

    for (int i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = (const struct elf64_phdr *)
            (img + eh->e_phoff + (uint64_t)i * sizeof(*ph));

        if (ph->p_type != ELF_PT_LOAD || ph->p_memsz == 0)
            continue;
        if (ph->p_filesz > ph->p_memsz)
            return -1;
        if (ph->p_offset > size || ph->p_filesz > size - ph->p_offset)
            return -1;
        /* Keep segments off the zero page and inside the user half. */
        if (ph->p_vaddr < PAGE_SIZE)
            return -1;
        if (ph->p_vaddr + ph->p_memsz < ph->p_vaddr ||
            ph->p_vaddr + ph->p_memsz >= USER_VA_LIMIT)
            return -1;
        if (ph->p_vaddr + ph->p_memsz > USER_STACK_LIMIT)
            return -1;

        /* Bound what this image may map, and leave the allocator a margin
         * so a huge-but-legal binary fails cleanly instead of panicking. */
        total_pages += ((ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1) / PAGE_SIZE)
                       - (ph->p_vaddr / PAGE_SIZE);
        if (total_pages > ELF_MAX_IMAGE_PAGES)
            return -1;
        if (total_pages + ELF_PMM_RESERVE > pmm_free_pages())
            return -1;

        if (load_segment(pml4, img, ph) < 0)
            return -1;

        if (ph->p_vaddr + ph->p_memsz > max_end)
            max_end = ph->p_vaddr + ph->p_memsz;
    }

    if (max_end == 0)
        return -1;                        /* no loadable segments */

    *entry_out = eh->e_entry;
    *brk_out = (max_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    return 0;
}
