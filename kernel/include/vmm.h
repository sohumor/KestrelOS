#pragma once

#include <stdint.h>

#define PTE_P  0x001ULL
#define PTE_W  0x002ULL
#define PTE_U  0x004ULL
#define PTE_PCD 0x010ULL
#define PTE_A  0x020ULL
#define PTE_D  0x040ULL
#define PTE_PS 0x080ULL
#define PTE_G  0x100ULL
/* Available-to-software leaf bit. A non-present PTE with this bit set
 * contains a one-based swap slot number in its address field. */
#define PTE_SWAP 0x200ULL

#define PTE_ADDR(e) ((e) & 0x000FFFFFFFFFF000ULL)

/* All pml4 arguments are *virtual* pointers (via the direct map). */
void      vmm_init(void);
uint64_t *vmm_kernel_pml4(void);
uint64_t *vmm_new_pml4(void);                 /* kernel half pre-populated */
void      vmm_map_page(uint64_t *pml4, uint64_t virt, uint64_t phys,
                       uint64_t flags);
uint64_t  vmm_virt_to_phys(uint64_t *pml4, uint64_t virt); /* 0 if unmapped */
uint64_t *vmm_get_pte(uint64_t *pml4, uint64_t virt, int create);
void      vmm_for_each_user_pte(uint64_t *pml4,
                                int (*visit)(uint64_t va, uint64_t *pte,
                                             void *arg),
                                void *arg);
void      vmm_destroy_user(uint64_t *pml4);   /* free user half + tables */
void      vmm_switch(uint64_t *pml4);

static inline void invlpg(uint64_t va)
{
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}
