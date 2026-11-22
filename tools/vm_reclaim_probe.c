#include "../kernel/vm.c"
#include "initcall.h"

#define PROBE_VA 0x12000000ULL

static struct task probe_task;

static uint64_t evicted_token(void)
{
    uint64_t *pte = vmm_get_pte(probe_task.pml4, PROBE_VA, 0);
    if (!pte || (*pte & (PTE_P | PTE_SWAP)) != PTE_SWAP)
        return UINT64_MAX;
    return PTE_ADDR(*pte) >> 12;
}

static void fill_page(uint32_t *data, int pattern)
{
    for (unsigned i = 0; i < PAGE_SIZE / sizeof(*data); i++)
        data[i] = pattern == 0 ? 0 : pattern == 1 ? (i / 512 + 17) : i + 1;
}

static bool evict_page(int pattern, uint64_t perms)
{
    uint64_t phys = pmm_alloc();
    fill_page(P2V(phys), pattern);
    vmm_map_page(probe_task.pml4, PROBE_VA, phys, perms);
    return swap_out_one(&probe_task, PROBE_VA + PAGE_SIZE) == 0;
}

static bool fault_and_check(int pattern, uint64_t perms)
{
    if (vm_fault_in_range(&probe_task, PROBE_VA, PAGE_SIZE, !!(perms & PTE_W)) < 0)
        return false;
    uint64_t *pte = vmm_get_pte(probe_task.pml4, PROBE_VA, 0);
    if (!pte || (*pte & (PTE_P | PTE_W | PTE_U)) != (PTE_P | perms))
        return false;
    const uint32_t *data = P2V(PTE_ADDR(*pte));
    bool valid = true;
    for (unsigned i = 0; i < PAGE_SIZE / sizeof(*data); i++) {
        uint32_t expected = pattern == 0 ? 0 : pattern == 1 ? (i / 512 + 17) : i + 1;
        if (data[i] != expected)
            valid = false;
    }
    pmm_free(PTE_ADDR(*pte));
    *pte = 0;
    return valid;
}

static bool try_disk_page(int pattern, uint64_t disk_used)
{
    if (!evict_page(pattern, PTE_U | PTE_W))
        return false;
    uint64_t token = evicted_token();
    return token && token <= SWAP_MAX_PAGES &&
           swap_used_pages() == disk_used + 1 &&
           fault_and_check(pattern, PTE_U | PTE_W) &&
           swap_used_pages() == disk_used;
}

static int check_reclaim(void)
{
    struct blockdev *disk = swap_dev;
    uint64_t disk_used = swap_used_pages();
    uint64_t packed;
    const char *stage = "zero without disk";
    uint32_t repeated[PAGE_SIZE / sizeof(uint32_t)];

    probe_task.user = true;
    probe_task.pml4 = vmm_new_pml4();
    probe_task.user_heap_start = PROBE_VA;
    probe_task.user_brk = PROBE_VA + PAGE_SIZE;

    swap_dev = NULL;
    if (!evict_page(0, PTE_U | PTE_W) || evicted_token() != 0 ||
        !fault_and_check(0, PTE_U | PTE_W))
        goto fail;
    stage = "packed restore without disk";
    if (!evict_page(1, PTE_U))
        goto fail;
    packed = evicted_token();
    if (!is_ram_token(packed) || !fault_and_check(1, PTE_U) ||
        swap_ram[packed - SWAP_RAM_BASE].size || swap_used_pages() != disk_used)
        goto fail;

    stage = "packed teardown";
    if (!evict_page(1, PTE_U | PTE_W) || evicted_token() != packed)
        goto fail;
    vm_release_task(&probe_task);
    if (swap_ram[packed - SWAP_RAM_BASE].size ||
        evicted_token() != UINT64_MAX)
        goto fail;

    stage = "disk fallback for noncompressible page";
    swap_dev = disk;
    if (!disk || !try_disk_page(2, disk_used))
        goto fail;
    stage = "disk fallback when packed pool is full";
    fill_page(repeated, 1);
    for (unsigned i = 0; i < SWAP_RAM_SLOTS; i++)
        if (store_ram_page(repeated) != (int)(SWAP_RAM_BASE + i))
            goto fail;
    if (!try_disk_page(1, disk_used) || !evict_page(0, PTE_U | PTE_W) ||
        evicted_token() != 0 || !fault_and_check(0, PTE_U | PTE_W))
        goto fail;
    stage = "disk teardown";
    if (!evict_page(2, PTE_U | PTE_W) || swap_used_pages() != disk_used + 1)
        goto fail;
    vm_release_task(&probe_task);
    if (swap_used_pages() != disk_used)
        goto fail;

    for (unsigned i = 0; i < SWAP_RAM_SLOTS; i++)
        release_swap_page(SWAP_RAM_BASE + i);
    vmm_destroy_user(probe_task.pml4);
    kprintf("PASS tiered VM reclaim: zero, packed RAM, disk fallback and slot release\n");
    return 0;
fail:
    swap_dev = disk;
    kprintf("FAIL tiered VM reclaim: %s\n", stage);
    return -1;
}
late_initcall(check_reclaim);
