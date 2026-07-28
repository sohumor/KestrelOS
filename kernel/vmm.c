#include "kernel.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "spinlock.h"

static uint64_t *kernel_pml4;   /* virtual (direct-map) pointer */
static spinlock_t page_table_lock = SPINLOCK_INIT;

static void write_cr3(uint64_t phys)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(phys) : "memory");
}

uint64_t *vmm_kernel_pml4(void)
{
    return kernel_pml4;
}

/* Get (or create) the next-level table for entry `idx` of `table`. */
static uint64_t *walk_create(uint64_t *table, int idx, uint64_t flags)
{
    if (!(table[idx] & PTE_P)) {
        uint64_t phys = pmm_alloc();
        table[idx] = phys | PTE_P | PTE_W | (flags & PTE_U);
        return P2V(phys);
    }
    /* Propagate the U bit if a user mapping goes through an existing table */
    if (flags & PTE_U)
        table[idx] |= PTE_U;
    return P2V(PTE_ADDR(table[idx]));
}

void vmm_map_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t irq = spin_lock_irqsave(&page_table_lock);
    int i4 = (virt >> 39) & 511;
    int i3 = (virt >> 30) & 511;
    int i2 = (virt >> 21) & 511;
    int i1 = (virt >> 12) & 511;

    uint64_t *pdpt = walk_create(pml4, i4, flags);
    uint64_t *pd = walk_create(pdpt, i3, flags);
    uint64_t *pt = walk_create(pd, i2, flags);
    pt[i1] = phys | flags | PTE_P;
    invlpg(virt);
    spin_unlock_irqrestore(&page_table_lock, irq);
}

static void map_2mb(uint64_t *pml4, uint64_t virt, uint64_t phys,
                    uint64_t flags)
{
    int i4 = (virt >> 39) & 511;
    int i3 = (virt >> 30) & 511;
    int i2 = (virt >> 21) & 511;

    uint64_t *pdpt = walk_create(pml4, i4, flags);
    uint64_t *pd = walk_create(pdpt, i3, flags);
    pd[i2] = phys | flags | PTE_P | PTE_PS;
}

uint64_t vmm_virt_to_phys(uint64_t *pml4, uint64_t virt)
{
    uint64_t *t = pml4;
    int idx[4] = { (int)((virt >> 39) & 511), (int)((virt >> 30) & 511),
                   (int)((virt >> 21) & 511), (int)((virt >> 12) & 511) };
    for (int level = 0; level < 4; level++) {
        uint64_t e = t[idx[level]];
        if (!(e & PTE_P))
            return 0;
        if (level == 2 && (e & PTE_PS))
            return PTE_ADDR(e) + (virt & 0x1FFFFF);
        if (level == 3)
            return PTE_ADDR(e) + (virt & 0xFFF);
        t = P2V(PTE_ADDR(e));
    }
    return 0;
}

uint64_t *vmm_get_pte(uint64_t *pml4, uint64_t virt, int create)
{
    uint64_t *table = pml4;
    int idx[4] = { (int)((virt >> 39) & 511), (int)((virt >> 30) & 511),
                   (int)((virt >> 21) & 511), (int)((virt >> 12) & 511) };

    for (int level = 0; level < 3; level++) {
        uint64_t e = table[idx[level]];
        if (!(e & PTE_P)) {
            if (!create)
                return NULL;
            table = walk_create(table, idx[level], PTE_U);
            continue;
        }
        if (e & PTE_PS)
            return NULL;
        if (create)
            table[idx[level]] |= PTE_U;
        table = P2V(PTE_ADDR(e));
    }
    return &table[idx[3]];
}

/* Walk allocated user page tables only. The callback may rewrite leaf PTEs
 * and returns non-zero to stop early. This is used by swap without exposing
 * the page-table hierarchy outside this module. */
void vmm_for_each_user_pte(uint64_t *pml4,
                           int (*visit)(uint64_t, uint64_t *, void *),
                           void *arg)
{
    if (!pml4 || !visit)
        return;
    for (uint64_t i4 = 0; i4 < 256; i4++) {
        if (!(pml4[i4] & PTE_P) || (pml4[i4] & PTE_PS))
            continue;
        uint64_t *pdpt = P2V(PTE_ADDR(pml4[i4]));
        for (uint64_t i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PTE_P) || (pdpt[i3] & PTE_PS))
                continue;
            uint64_t *pd = P2V(PTE_ADDR(pdpt[i3]));
            for (uint64_t i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PTE_P) || (pd[i2] & PTE_PS))
                    continue;
                uint64_t *pt = P2V(PTE_ADDR(pd[i2]));
                for (uint64_t i1 = 0; i1 < 512; i1++) {
                    if (pt[i1] == 0)
                        continue;
                    uint64_t va = (i4 << 39) | (i3 << 30) |
                                  (i2 << 21) | (i1 << 12);
                    if (visit(va, &pt[i1], arg))
                        return;
                }
            }
        }
    }
}

void vmm_init(void)
{
    uint64_t phys = pmm_alloc();
    kernel_pml4 = P2V(phys);

    /* Direct map of all physical memory at PHYS_MAP_BASE (2 MiB pages).
     * Round up to cover at least 16 MiB even on tiny systems. */
    uint64_t top = pmm_max_phys();
    if (top < 16 * 1024 * 1024)
        top = 16 * 1024 * 1024;
    for (uint64_t a = 0; a < top; a += 0x200000)
        map_2mb(kernel_pml4, PHYS_MAP_BASE + a, a, PTE_W | PTE_G);

    /* Higher-half kernel image: KERNEL_OFFSET + phys for the first 16 MiB
     * (covers the kernel and leaves room to grow). */
    for (uint64_t a = 0; a < 16 * 1024 * 1024; a += 0x200000)
        map_2mb(kernel_pml4, KERNEL_OFFSET + a, a, PTE_W | PTE_G);

    vmm_switch(kernel_pml4);
}

uint64_t *vmm_new_pml4(void)
{
    uint64_t phys = pmm_alloc();
    uint64_t *pml4 = P2V(phys);
    /* Kernel half is shared with the kernel address space. */
    for (int i = 256; i < 512; i++)
        pml4[i] = kernel_pml4[i];
    return pml4;
}

/* Recursively free user-half page tables and the frames they map. */
static void free_level(uint64_t *table, int level)
{
    for (int i = 0; i < 512; i++) {
        uint64_t e = table[i];
        if (!(e & PTE_P))
            continue;
        if (level < 3 && !(e & PTE_PS))
            free_level(P2V(PTE_ADDR(e)), level + 1);
        if (level == 3 || (e & PTE_PS))
            pmm_free(PTE_ADDR(e));
    }
    if (level > 0)
        pmm_free(V2P(table));
}

void vmm_destroy_user(uint64_t *pml4)
{
    for (int i = 0; i < 256; i++) {
        if (pml4[i] & PTE_P) {
            free_level(P2V(PTE_ADDR(pml4[i])), 1);
            pml4[i] = 0;
        }
    }
    pmm_free(V2P(pml4));
}

void vmm_switch(uint64_t *pml4)
{
    write_cr3(V2P(pml4));
}
