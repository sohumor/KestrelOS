#include "kernel.h"
#include "vm.h"
#include "proc.h"
#include "vmm.h"
#include "pmm.h"
#include "vfs.h"
#include "blockdev.h"
#include "string.h"
#include "kestrel_abi.h"
#include "spinlock.h"

/* tools/mkimage.py writes this header at the fixed end of the 32 MiB root
 * filesystem. The data pages begin one page later, which keeps every swap
 * transfer naturally aligned to eight ATA sectors. */
#define SWAP_START_LBA       (2048ULL + (32ULL * 1024 * 1024 / 512))
#define SWAP_HEADER_SECTORS  8
#define SWAP_SECTORS_PER_PAGE (PAGE_SIZE / 512)
#define SWAP_MAX_PAGES       4096
#define SWAP_VERSION         1
#define VM_PMM_RESERVE       512

struct swap_header {
    char magic[8];               /* "KSWAP01" plus NUL */
    uint32_t version;
    uint32_t pages;
};

static struct blockdev *swap_dev;
static uint8_t swap_bitmap[SWAP_MAX_PAGES / 8];
static uint32_t swap_pages;
static uint32_t swap_used;
static spinlock_t swap_lock = SPINLOCK_INIT;

static int bit_test(uint32_t bit)
{
    return (swap_bitmap[bit >> 3] >> (bit & 7)) & 1;
}

static void bit_set(uint32_t bit)
{
    swap_bitmap[bit >> 3] |= (uint8_t)(1U << (bit & 7));
}

static void bit_clear(uint32_t bit)
{
    swap_bitmap[bit >> 3] &= (uint8_t)~(1U << (bit & 7));
}

void swap_init(void)
{
    uint8_t page[PAGE_SIZE];
    struct swap_header *h = (struct swap_header *)page;

    swap_dev = blockdev_find("hda");
    if (!swap_dev || swap_dev->block_size != 512 ||
        swap_dev->blocks < SWAP_START_LBA + SWAP_HEADER_SECTORS ||
        blockdev_read(swap_dev, SWAP_START_LBA, SWAP_HEADER_SECTORS,
                      page) < 0 ||
        memcmp(h->magic, "KSWAP01", 8) != 0 ||
        h->version != SWAP_VERSION || h->pages == 0 ||
        h->pages > SWAP_MAX_PAGES ||
        h->pages > (swap_dev->blocks - SWAP_START_LBA -
                    SWAP_HEADER_SECTORS) / SWAP_SECTORS_PER_PAGE) {
        swap_dev = NULL;
        kprintf("vm: no valid swap extent; demand paging remains enabled\n");
        return;
    }

    swap_pages = h->pages;
    memset(swap_bitmap, 0, sizeof(swap_bitmap));
    kprintf("vm: demand paging + %u swap pages (%u MiB) online\n",
            swap_pages,
            (unsigned)(swap_pages * (PAGE_SIZE / 1024) / 1024));
}

uint64_t swap_total_pages(void)
{
    return __atomic_load_n(&swap_pages, __ATOMIC_RELAXED);
}

uint64_t swap_used_pages(void)
{
    return __atomic_load_n(&swap_used, __ATOMIC_RELAXED);
}

static int swap_alloc_slot(void)
{
    uint64_t flags = spin_lock_irqsave(&swap_lock);
    for (uint32_t i = 0; i < swap_pages; i++) {
        if (!bit_test(i)) {
            bit_set(i);
            swap_used++;
            spin_unlock_irqrestore(&swap_lock, flags);
            return (int)i;
        }
    }
    spin_unlock_irqrestore(&swap_lock, flags);
    return -1;
}

static void swap_free_slot(uint32_t slot)
{
    uint64_t flags = spin_lock_irqsave(&swap_lock);
    if (slot < swap_pages && bit_test(slot)) {
        bit_clear(slot);
        swap_used--;
    }
    spin_unlock_irqrestore(&swap_lock, flags);
}

static int swap_slot_in_use(uint32_t slot)
{
    uint64_t flags = spin_lock_irqsave(&swap_lock);
    int used = slot < swap_pages && bit_test(slot);
    spin_unlock_irqrestore(&swap_lock, flags);
    return used;
}

static uint64_t swap_lba(uint32_t slot)
{
    return SWAP_START_LBA + SWAP_HEADER_SECTORS +
           (uint64_t)slot * SWAP_SECTORS_PER_PAGE;
}

static int page_is_managed(struct task *t, uint64_t va)
{
    uint64_t end = va + PAGE_SIZE;
    for (int i = 0; i < t->vm_area_count; i++)
        if (va < t->vm_areas[i].end && end > t->vm_areas[i].start)
            return 1;
    if (va < t->user_brk && end > t->user_heap_start)
        return 1;
    if (va >= USER_STACK_TOP - (uint64_t)USER_STACK_PAGES * PAGE_SIZE &&
        va < USER_STACK_TOP)
        return 1;
    return 0;
}

struct victim_search {
    struct task *task;
    struct task *avoid_task;
    uint64_t avoid_va;
    uint64_t va;
    uint64_t *pte;
    int clear_accessed;
};

static int victim_visit(uint64_t va, uint64_t *pte, void *arg)
{
    struct victim_search *s = arg;

    if (!(*pte & PTE_P) || !(*pte & PTE_U) ||
        !page_is_managed(s->task, va) ||
        (s->task == s->avoid_task && va == s->avoid_va))
        return 0;

    if (*pte & PTE_A) {
        if (s->clear_accessed) {
            *pte &= ~PTE_A;
            if (s->task == current)
                invlpg(va);
        }
        return 0;
    }
    s->va = va;
    s->pte = pte;
    return 1;
}

static int find_victim(struct task *avoid_task, uint64_t avoid_va,
                       struct victim_search *out)
{
    /* A remote RUNNING address space would require a TLB shootdown before
     * its frame could be reused. Reclaim from the faulting task itself:
     * it cannot run on another CPU, and this is sufficient for anonymous
     * pressure while preserving strict ownership of every active PML4. */
    struct task *t = avoid_task;
    if (!t || !t->user)
        return -1;
    for (int pass = 0; pass < 2; pass++) {
        struct victim_search s;
        memset(&s, 0, sizeof(s));
        s.task = t;
        s.avoid_task = avoid_task;
        s.avoid_va = avoid_va;
        s.clear_accessed = (pass == 0);
        vmm_for_each_user_pte(t->pml4, victim_visit, &s);
        if (s.pte) {
            *out = s;
            return 0;
        }
    }
    return -1;
}

static int swap_out_one(struct task *avoid_task, uint64_t avoid_va)
{
    struct victim_search v;
    int slot;
    uint64_t old;

    if (!swap_dev || find_victim(avoid_task, avoid_va, &v) < 0)
        return -1;
    slot = swap_alloc_slot();
    if (slot < 0)
        return -1;

    old = *v.pte;
    if (blockdev_write(swap_dev, swap_lba((uint32_t)slot),
                       SWAP_SECTORS_PER_PAGE, P2V(PTE_ADDR(old))) < 0) {
        swap_free_slot((uint32_t)slot);
        return -1;
    }

    *v.pte = ((uint64_t)(slot + 1) << 12) | PTE_SWAP |
             (old & (PTE_U | PTE_W));
    if (v.task == current)
        invlpg(v.va);
    pmm_free(PTE_ADDR(old));
    return 0;
}

static uint64_t alloc_user_frame(struct task *task, uint64_t va)
{
    if (pmm_free_pages() <= VM_PMM_RESERVE &&
        swap_out_one(task, va) < 0)
        return 0;
    return pmm_alloc();
}

static int address_permission(struct task *t, uint64_t address, int write)
{
    for (int i = 0; i < t->vm_area_count; i++) {
        struct vm_area *a = &t->vm_areas[i];
        if (address >= a->start && address < a->end)
            return !write || (a->pte_flags & PTE_W);
    }
    if (address >= t->user_heap_start && address < t->user_brk)
        return 1;
    if (address >= USER_STACK_TOP -
                       (uint64_t)USER_STACK_PAGES * PAGE_SIZE &&
        address < USER_STACK_TOP)
        return 1;
    return 0;
}

static int read_exec_slice(struct file *file, uint64_t file_off,
                           void *dst, uint64_t len)
{
    uint64_t done = 0;
    if (!file || file_off > 0x7FFFFFFFULL ||
        vfs_seek(file, (long)file_off, 0) < 0)
        return -1;
    while (done < len) {
        long n = vfs_read(file, (uint8_t *)dst + done, len - done);
        if (n <= 0)
            return -1;
        done += (uint64_t)n;
    }
    return 0;
}

static int fill_file_page(struct task *t, uint64_t page, uint64_t phys)
{
    for (int i = 0; i < t->vm_area_count; i++) {
        struct vm_area *a = &t->vm_areas[i];
        uint64_t file_end = a->start + a->file_size;
        uint64_t lo = page > a->start ? page : a->start;
        uint64_t hi = page + PAGE_SIZE < file_end
                    ? page + PAGE_SIZE : file_end;
        if (lo >= hi)
            continue;
        if (read_exec_slice(a->backing,
                            a->file_offset + (lo - a->start),
                            (uint8_t *)P2V(phys) + (lo - page),
                            hi - lo) < 0)
            return -1;
    }
    return 0;
}

static int handle_page_fault_locked(struct task *t, uint64_t address,
                                    uint64_t error)
{
    uint64_t page = address & ~(PAGE_SIZE - 1);
    uint64_t *pte;
    uint64_t phys;
    int write = (error & 2) != 0;

    if (!t || !t->user || address >= 0x0000800000000000ULL ||
        (error & 1) || !address_permission(t, address, write))
        return -1;

    pte = vmm_get_pte(t->pml4, page, 0);
    if (pte && (*pte & PTE_SWAP)) {
        uint64_t raw = *pte;
        uint32_t slot = (uint32_t)((PTE_ADDR(raw) >> 12) - 1);
        if (!swap_dev || !swap_slot_in_use(slot))
            return -1;
        phys = alloc_user_frame(t, page);
        if (!phys)
            return -1;
        if (blockdev_read(swap_dev, swap_lba(slot),
                          SWAP_SECTORS_PER_PAGE, P2V(phys)) < 0) {
            pmm_free(phys);
            return -1;
        }
        swap_free_slot(slot);
        vmm_map_page(t->pml4, page, phys,
                     PTE_U | (raw & PTE_W));
        return 0;
    }
    if (pte && (*pte & PTE_P))
        return -1;

    phys = alloc_user_frame(t, page);
    if (!phys)
        return -1;

    uint64_t flags = PTE_U;
    for (int i = 0; i < t->vm_area_count; i++) {
        struct vm_area *a = &t->vm_areas[i];
        if (page < a->end && page + PAGE_SIZE > a->start)
            flags |= a->pte_flags & PTE_W;
    }
    if (address >= t->user_heap_start && address < t->user_brk)
        flags |= PTE_W;
    if (address >= USER_STACK_TOP -
                       (uint64_t)USER_STACK_PAGES * PAGE_SIZE &&
        address < USER_STACK_TOP)
        flags |= PTE_W;

    if (fill_file_page(t, page, phys) < 0) {
        pmm_free(phys);
        return -1;
    }
    vmm_map_page(t->pml4, page, phys, flags);
    return 0;
}

int vm_handle_page_fault(struct task *t, uint64_t address, uint64_t error)
{
    /* A task is RUNNING on at most one CPU, so its VM areas and page tables
     * have a single owner here. The global swap bitmap has its own short
     * lock; never hold a spinlock across executable reads or swap I/O. */
    return handle_page_fault_locked(t, address, error);
}

int vm_fault_in_range(struct task *t, uint64_t address, size_t len, int write)
{
    if (len == 0)
        return 0;
    if (!t || address + len < address)
        return -1;

    uint64_t end = address + len;
    for (uint64_t p = address & ~(PAGE_SIZE - 1); p < end;
         p += PAGE_SIZE) {
        uint64_t probe = p < address ? address : p;
        uint64_t phys = vmm_virt_to_phys(t->pml4, probe);
        if (phys) {
            uint64_t *pte = vmm_get_pte(t->pml4, probe, 0);
            if (write && (!pte || !(*pte & PTE_W)))
                return -1;
            continue;
        }
        if (vm_handle_page_fault(t, probe, write ? 2 : 0) < 0)
            return -1;
    }
    return 0;
}

struct release_ctx {
    int unused;
};

static int release_visit(uint64_t va, uint64_t *pte, void *arg)
{
    (void)va;
    (void)arg;
    if (*pte & PTE_SWAP) {
        uint32_t slot = (uint32_t)((PTE_ADDR(*pte) >> 12) - 1);
        swap_free_slot(slot);
        *pte = 0;
    }
    return 0;
}

void vm_release_task(struct task *t)
{
    if (!t || !t->pml4)
        return;
    struct release_ctx ctx;
    vmm_for_each_user_pte(t->pml4, release_visit, &ctx);
    for (int i = 0; i < t->vm_file_count; i++) {
        if (t->vm_files[i])
            vfs_close(t->vm_files[i]);
        t->vm_files[i] = NULL;
    }
    t->vm_file_count = 0;
}
