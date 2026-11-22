#include "kernel.h"
#include "vm.h"
#include "proc.h"
#include "vmm.h"
#include "pmm.h"
#include "vfs.h"
#include "kfs.h"
#include "blockdev.h"
#include "string.h"
#include "kestrel_abi.h"
#include "spinlock.h"
#include "flight.h"
#include "hangar_fmt.h"

/* tools/mkimage.py writes this header immediately after the root KFS
 * filesystem partition. Derive the starting LBA from the KFS superblock at
 * KFS_PART_LBA when present (falling back to the legacy 32 MiB offset) so
 * resizing the filesystem image keeps swap naturally aligned to 8 sectors. */
#define SWAP_DEFAULT_START_LBA (KFS_PART_LBA + (32ULL * 1024 * 1024 / 512))
#define SWAP_HEADER_SECTORS    8
#define SWAP_SECTORS_PER_PAGE  (PAGE_SIZE / 512)
#define SWAP_MAX_PAGES         4096
#define SWAP_VERSION           1
#define VM_PMM_RESERVE         512
#define SWAP_RAM_SLOTS         128
#define SWAP_RAM_WORDS         128
#define SWAP_RAM_BASE          (SWAP_MAX_PAGES + 1)

struct swap_header {
    char magic[8];               /* "KSWAP01" plus NUL */
    uint32_t version;
    uint32_t pages;
};

static struct blockdev *swap_dev;
static uint8_t swap_bitmap[SWAP_MAX_PAGES / 8];
static uint64_t swap_start_lba = SWAP_DEFAULT_START_LBA;
static uint32_t swap_pages;
static uint32_t swap_used;
static spinlock_t swap_lock = SPINLOCK_INIT;

/* Swap token 0 is an exact zero page; 1..SWAP_MAX_PAGES are disk slots. */
static struct swap_ram_page {
    uint16_t size;
    uint32_t words[SWAP_RAM_WORDS];
} swap_ram[SWAP_RAM_SLOTS];

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
    struct kfs_superblock *sb = (struct kfs_superblock *)page;
    uint64_t start = SWAP_DEFAULT_START_LBA;

    swap_dev = blockdev_find("hda");
    if (!swap_dev)
        swap_dev = blockdev_find("sda");
    if (!swap_dev)
        swap_dev = blockdev_find("vda");
    if (!swap_dev || swap_dev->block_size != 512) {
        swap_dev = NULL;
        kprintf("vm: no valid swap extent; demand paging remains enabled\n");
        return;
    }
    if (swap_dev->blocks >= (uint64_t)KFS_PART_LBA + SWAP_HEADER_SECTORS &&
        blockdev_read(swap_dev, KFS_PART_LBA, SWAP_HEADER_SECTORS, page) == 0 &&
        sb->magic == KFS_MAGIC && sb->total_blocks > 0) {
        start = (uint64_t)KFS_PART_LBA +
                (uint64_t)sb->total_blocks * KFS_SECTORS_PER_BLOCK;
    }
    swap_start_lba = start;
    if (swap_dev->blocks < swap_start_lba + SWAP_HEADER_SECTORS ||
        blockdev_read(swap_dev, swap_start_lba, SWAP_HEADER_SECTORS,
                      page) < 0 ||
        memcmp(h->magic, "KSWAP01", 8) != 0 ||
        h->version != SWAP_VERSION || h->pages == 0 ||
        h->pages > SWAP_MAX_PAGES ||
        h->pages > (swap_dev->blocks - swap_start_lba -
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
    return swap_start_lba + SWAP_HEADER_SECTORS +
           (uint64_t)slot * SWAP_SECTORS_PER_PAGE;
}

static int store_ram_page(const uint32_t *page)
{
    uint32_t packed[SWAP_RAM_WORDS];
    size_t words = hangar_rle_encode(page, PAGE_SIZE / sizeof(*page),
                                     packed, SWAP_RAM_WORDS);
    if (!words)
        return -1;
    if (words == 2 && packed[0] == (HANGAR_RLE_RUN | (PAGE_SIZE / sizeof(*page))) &&
        packed[1] == 0)
        return 0;

    uint64_t f = spin_lock_irqsave(&swap_lock);
    for (int i = 0; i < SWAP_RAM_SLOTS; i++) {
        struct swap_ram_page *slot = &swap_ram[i];
        if (slot->size)
            continue;
        memcpy(slot->words, packed, words * sizeof(*packed));
        slot->size = (uint16_t)words;
        spin_unlock_irqrestore(&swap_lock, f);
        return SWAP_RAM_BASE + i;
    }
    spin_unlock_irqrestore(&swap_lock, f);
    return -1;
}

static bool is_ram_token(uint64_t token)
{
    return token >= SWAP_RAM_BASE && token < SWAP_RAM_BASE + SWAP_RAM_SLOTS;
}

static int restore_swap_page(uint64_t token, void *page)
{
    if (token == 0) {
        memset(page, 0, PAGE_SIZE);
        return 0;
    }
    if (is_ram_token(token)) {
        const struct swap_ram_page *slot = &swap_ram[token - SWAP_RAM_BASE];
        uint32_t packed[SWAP_RAM_WORDS];
        uint64_t f = spin_lock_irqsave(&swap_lock);
        size_t words = slot->size;
        memcpy(packed, slot->words, words * sizeof(*packed));
        spin_unlock_irqrestore(&swap_lock, f);
        return hangar_rle_decode(packed, words, page, PAGE_SIZE / sizeof(*packed));
    }
    if (token > SWAP_MAX_PAGES || !swap_dev || !swap_slot_in_use((uint32_t)token - 1))
        return -1;
    return blockdev_read(swap_dev, swap_lba((uint32_t)token - 1),
                         SWAP_SECTORS_PER_PAGE, page);
}

static void release_swap_page(uint64_t token)
{
    if (is_ram_token(token)) {
        uint64_t f = spin_lock_irqsave(&swap_lock);
        swap_ram[token - SWAP_RAM_BASE].size = 0;
        spin_unlock_irqrestore(&swap_lock, f);
    } else if (token > 0 && token <= SWAP_MAX_PAGES) {
        swap_free_slot((uint32_t)token - 1);
    }
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

    if (find_victim(avoid_task, avoid_va, &v) < 0)
        return -1;

    uint64_t old = *v.pte;
    const void *data = P2V(PTE_ADDR(old));
    int token = store_ram_page(data);
    if (token < 0) {
        int slot = swap_dev ? swap_alloc_slot() : -1;
        if (slot < 0)
            return -1;
        if (blockdev_write(swap_dev, swap_lba((uint32_t)slot),
                           SWAP_SECTORS_PER_PAGE, data) < 0) {
            swap_free_slot((uint32_t)slot);
            return -1;
        }
        token = slot + 1;
    }

    *v.pte = ((uint64_t)token << 12) | PTE_SWAP | (old & (PTE_U | PTE_W));
    if (v.task == current)
        invlpg(v.va);
    pmm_free(PTE_ADDR(old));
    const char *event = !token ? "swap_zero" :
                        is_ram_token(token) ? "swap_pack" : "swap_out";
    flight_record(FLIGHT_CAT_VM, event, (int16_t)v.task->pid, v.va,
                  token ? (uint64_t)token - 1 : 0, v.task->name);
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
        uint64_t token = PTE_ADDR(raw) >> 12;
        phys = alloc_user_frame(t, page);
        if (!phys)
            return -1;
        if (restore_swap_page(token, P2V(phys)) < 0) {
            pmm_free(phys);
            return -1;
        }
        vmm_map_page(t->pml4, page, phys,
                     PTE_U | (raw & PTE_W));
        release_swap_page(token);
        const char *event = !token ? "swap_refill" :
                            is_ram_token(token) ? "swap_unpack" : "swap_in";
        flight_record(FLIGHT_CAT_VM, event, (int16_t)t->pid,
                      page, token ? token - 1 : 0, t->name);
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
    flight_record(FLIGHT_CAT_VM, "page_fault", (int16_t)t->pid,
                  page, error, t->name);
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
        release_swap_page(PTE_ADDR(*pte) >> 12);
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

int vm_page_is_managed(struct task *t, uint64_t va)
{
    return t ? page_is_managed(t, va & ~(PAGE_SIZE - 1)) : 0;
}
