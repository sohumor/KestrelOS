#pragma once

#include <stdint.h>
#include "bootinfo.h"

void     pmm_init(const struct boot_memory_range *ranges, size_t count);
uint64_t pmm_alloc(void);              /* one zeroed 4 KiB page (phys addr) */
uint64_t pmm_alloc_contig(int npages); /* contiguous zeroed pages */
uint64_t pmm_try_alloc_contig(int npages); /* same, or 0 if unavailable */
void     pmm_free(uint64_t phys);
void     pmm_free_contig(uint64_t phys, int npages);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages(void);
uint64_t pmm_max_phys(void);
