#include "kernel.h"
#include "smp.h"
#include "vmm.h"
#include "pmm.h"
#include "proc.h"
#include "gdt.h"
#include "interrupts.h"
#include "timer.h"
#include "fpu.h"
#include "string.h"

#define AP_TRAMPOLINE_PHYS 0x1000ULL
#define LAPIC_VA           0xFFFFFFFFFEE00000ULL
#define IA32_APIC_BASE     0x0000001B
#define IA32_GS_BASE       0xC0000101

#define LAPIC_ID           0x020
#define LAPIC_EOI          0x0B0
#define LAPIC_SVR          0x0F0
#define LAPIC_ESR          0x280
#define LAPIC_LVT_TIMER    0x320
#define LAPIC_LVT_LINT0    0x350
#define LAPIC_LVT_LINT1    0x360
#define LAPIC_ICR_LOW      0x300
#define LAPIC_ICR_HIGH     0x310
#define LAPIC_ICR_PENDING  (1U << 12)

struct cpu_local {
    struct task *task;          /* GS:0 -- keep first */
    uint32_t index;
    uint32_t slice;
    uint8_t apic_id;
    volatile uint8_t online;
    uint8_t _pad[6];
    struct task *boot_task;
    uint64_t stack_top;
} __attribute__((aligned(64)));

struct acpi_rsdp {
    char signature[8];
    uint8_t checksum;
    char oem[6];
    uint8_t revision;
    uint32_t rsdt;
    uint32_t length;
    uint64_t xsdt;
    uint8_t ext_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

struct acpi_sdt {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_madt {
    struct acpi_sdt header;
    uint32_t lapic_address;
    uint32_t flags;
    uint8_t entries[];
} __attribute__((packed));

static struct cpu_local cpus[SMP_MAX_CPUS];
static unsigned discovered = 1;
static volatile unsigned online_count;
static uint64_t lapic_phys = 0xFEE00000ULL;
static volatile uint32_t *lapic;
static int lapic_available;

extern uint8_t ap_trampoline_start[];
extern uint8_t ap_trampoline_end[];
extern uint8_t ap_trampoline_cr3[];
extern uint8_t ap_trampoline_stack[];
extern uint8_t ap_trampoline_target[];
extern uint8_t ap_trampoline_cpu[];

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                         uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0));
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((uint32_t)value),
                       "d"((uint32_t)(value >> 32))
                     : "memory");
}

static uint8_t checksum8(const void *ptr, uint32_t len)
{
    const uint8_t *p = ptr;
    uint8_t sum = 0;
    while (len--)
        sum = (uint8_t)(sum + *p++);
    return sum;
}

static int phys_range_ok(uint64_t phys, uint32_t len)
{
    uint64_t mapped_top =
        (pmm_max_phys() + 0x1FFFFFULL) & ~0x1FFFFFULL;
    return len >= sizeof(struct acpi_sdt) &&
           len <= 1024 * 1024 &&
           phys < mapped_top &&
           (uint64_t)len <= mapped_top - phys;
}

static struct acpi_rsdp *scan_rsdp_range(uint64_t start, uint64_t end)
{
    start = (start + 15) & ~15ULL;
    for (uint64_t p = start; p + 20 <= end; p += 16) {
        struct acpi_rsdp *r = P2V(p);
        if (memcmp(r->signature, "RSD PTR ", 8) != 0)
            continue;
        if (checksum8(r, 20) != 0)
            continue;
        if (r->revision >= 2) {
            if (r->length < sizeof(*r) || r->length > 4096 ||
                checksum8(r, r->length) != 0)
                continue;
        }
        return r;
    }
    return NULL;
}

static struct acpi_rsdp *find_rsdp(void)
{
    uint16_t ebda_segment = *(volatile uint16_t *)P2V(0x40E);
    uint64_t ebda = (uint64_t)ebda_segment << 4;
    struct acpi_rsdp *r = NULL;
    if (ebda >= 0x400 && ebda < 0xA0000)
        r = scan_rsdp_range(ebda, ebda + 1024);
    if (!r)
        r = scan_rsdp_range(0xE0000, 0x100000);
    return r;
}

static struct acpi_sdt *valid_sdt(uint64_t phys)
{
    uint64_t mapped_top =
        (pmm_max_phys() + 0x1FFFFFULL) & ~0x1FFFFFULL;
    if (phys >= mapped_top ||
        sizeof(struct acpi_sdt) > mapped_top - phys)
        return NULL;
    struct acpi_sdt *sdt = P2V(phys);
    if (!phys_range_ok(phys, sdt->length) ||
        checksum8(sdt, sdt->length) != 0)
        return NULL;
    return sdt;
}

static struct acpi_madt *find_madt(struct acpi_rsdp *rsdp)
{
    uint64_t root_phys = 0;
    unsigned stride = 4;
    if (rsdp->revision >= 2 && rsdp->xsdt) {
        root_phys = rsdp->xsdt;
        stride = 8;
    } else {
        root_phys = rsdp->rsdt;
    }

    struct acpi_sdt *root = valid_sdt(root_phys);
    if (!root && stride == 8 && rsdp->rsdt) {
        root_phys = rsdp->rsdt;
        stride = 4;
        root = valid_sdt(root_phys);
    }
    if (!root)
        return NULL;
    uint32_t count = (root->length - sizeof(*root)) / stride;
    const uint8_t *entries = (const uint8_t *)root + sizeof(*root);
    for (uint32_t i = 0; i < count; i++) {
        uint64_t phys;
        if (stride == 8)
            memcpy(&phys, entries + (uint64_t)i * stride, sizeof(phys));
        else {
            uint32_t p32;
            memcpy(&p32, entries + (uint64_t)i * stride, sizeof(p32));
            phys = p32;
        }
        struct acpi_sdt *sdt = valid_sdt(phys);
        if (sdt && memcmp(sdt->signature, "APIC", 4) == 0 &&
            sdt->length >= sizeof(struct acpi_madt))
            return (struct acpi_madt *)sdt;
    }
    return NULL;
}

static int have_apic_id(uint8_t id)
{
    for (unsigned i = 0; i < discovered; i++)
        if (cpus[i].apic_id == id)
            return 1;
    return 0;
}

static void discover_madt(struct acpi_madt *madt, uint8_t bsp_id)
{
    cpus[0].apic_id = bsp_id;
    discovered = 1;
    lapic_phys = madt->lapic_address;

    uint8_t *p = madt->entries;
    uint8_t *end = (uint8_t *)madt + madt->header.length;
    while (p + 2 <= end) {
        uint8_t type = p[0], len = p[1];
        if (len < 2 || p + len > end)
            break;
        if (type == 0 && len >= 8) {
            uint8_t id = p[3];
            uint32_t flags;
            memcpy(&flags, p + 4, sizeof(flags));
            if ((flags & 3) && !have_apic_id(id) &&
                discovered < SMP_MAX_CPUS)
                cpus[discovered++].apic_id = id;
        } else if (type == 5 && len >= 12) {
            uint64_t override;
            memcpy(&override, p + 4, sizeof(override));
            lapic_phys = override;
        }
        p += len;
    }
}

static inline uint32_t lapic_read(uint32_t reg)
{
    return lapic[reg / 4];
}

static inline void lapic_write(uint32_t reg, uint32_t value)
{
    lapic[reg / 4] = value;
    (void)lapic[reg / 4];
}

static void lapic_wait_icr(void)
{
    while (lapic_read(LAPIC_ICR_LOW) & LAPIC_ICR_PENDING)
        __asm__ volatile("pause");
}

static void lapic_enable(void)
{
    uint64_t base = rdmsr(IA32_APIC_BASE);
    base |= 1ULL << 11;          /* global APIC enable */
    base &= ~(1ULL << 10);       /* use xAPIC MMIO, not x2APIC MSRs */
    wrmsr(IA32_APIC_BASE, base);

    lapic_write(LAPIC_SVR, 0x100 | 0xFF);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_LVT_TIMER, 1U << 16);
    if (smp_cpu_index() != 0) {
        lapic_write(LAPIC_LVT_LINT0, 1U << 16);
        lapic_write(LAPIC_LVT_LINT1, 1U << 16);
    }
}

static void install_cpu_local(unsigned index)
{
    cpus[index].index = index;
    wrmsr(IA32_GS_BASE, (uint64_t)&cpus[index]);
}

unsigned smp_cpu_index(void)
{
    uint32_t index;
    __asm__ volatile("movl %%gs:8, %0" : "=r"(index));
    return index;
}

struct task *smp_current_task(void)
{
    struct task *task;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(task));
    return task;
}

void smp_set_current_task(struct task *task)
{
    __asm__ volatile("movq %0, %%gs:0" : : "r"(task) : "memory");
}

unsigned smp_slice_increment(void)
{
    return ++cpus[smp_cpu_index()].slice;
}

void smp_slice_reset(void)
{
    cpus[smp_cpu_index()].slice = 0;
}

unsigned smp_cpu_count(void)
{
    return __atomic_load_n(&online_count, __ATOMIC_ACQUIRE);
}

unsigned smp_cpu_discovered(void)
{
    return discovered;
}

uint8_t smp_cpu_apic_id(unsigned index)
{
    return index < discovered ? cpus[index].apic_id : 0;
}

void smp_lapic_eoi(void)
{
    if (lapic_available)
        lapic_write(LAPIC_EOI, 0);
}

void smp_handle_reschedule(void)
{
    smp_lapic_eoi();
}

void smp_broadcast_reschedule(void)
{
    if (!lapic_available || smp_cpu_count() < 2)
        return;
    lapic_wait_icr();
    lapic_write(LAPIC_ICR_LOW,
                (3U << 18) | SMP_RESCHEDULE_VECTOR); /* all excluding self */
}

void smp_init(struct bootinfo *bi)
{
    (void)bi;
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    uint8_t bsp_id = (uint8_t)(b >> 24);

    memset(cpus, 0, sizeof(cpus));
    cpus[0].apic_id = bsp_id;
    discovered = 1;
    install_cpu_local(0);

    if (!(d & (1U << 9))) {
        online_count = 1;
        cpus[0].online = 1;
        kprintf("smp: local APIC unavailable; uniprocessor mode\n");
        return;
    }

    struct acpi_rsdp *rsdp = find_rsdp();
    struct acpi_madt *madt = rsdp ? find_madt(rsdp) : NULL;
    if (madt)
        discover_madt(madt, bsp_id);
    else
        kprintf("smp: %s; only BSP available\n",
                rsdp ? "ACPI MADT not found" : "ACPI RSDP not found");

    /* The LAPIC is MMIO near 4 GiB, normally outside the RAM direct map. */
    vmm_map_page(vmm_kernel_pml4(), LAPIC_VA, lapic_phys & ~0xFFFULL,
                 PTE_W | PTE_PCD);
    lapic = (volatile uint32_t *)(LAPIC_VA + (lapic_phys & 0xFFF));
    lapic_available = 1;
    lapic_enable();

    /* The AP executes this page before its first higher-half jump. */
    vmm_map_page(vmm_kernel_pml4(), AP_TRAMPOLINE_PHYS,
                 AP_TRAMPOLINE_PHYS, PTE_W);

    cpus[0].online = 1;
    online_count = 1;
    kprintf("smp: discovered %u CPU%s, BSP APIC id %u\n",
            discovered, discovered == 1 ? "" : "s", bsp_id);
}

static void wait_ticks(uint64_t count)
{
    uint64_t end = timer_ticks() + count;
    while (timer_ticks() < end)
        __asm__ volatile("sti; hlt");
}

static void start_one_ap(unsigned index)
{
    uint8_t id = cpus[index].apic_id;
    lapic_wait_icr();
    lapic_write(LAPIC_ICR_HIGH, (uint32_t)id << 24);
    lapic_write(LAPIC_ICR_LOW, 0x0000C500);  /* INIT assert */
    lapic_wait_icr();
    lapic_write(LAPIC_ICR_HIGH, (uint32_t)id << 24);
    lapic_write(LAPIC_ICR_LOW, 0x00008500);  /* INIT deassert */
    lapic_wait_icr();
    wait_ticks(1);

    for (int attempt = 0; attempt < 2 && !cpus[index].online; attempt++) {
        lapic_write(LAPIC_ESR, 0);
        lapic_write(LAPIC_ICR_HIGH, (uint32_t)id << 24);
        lapic_write(LAPIC_ICR_LOW,
                    0x00000600 | (AP_TRAMPOLINE_PHYS >> 12));
        lapic_wait_icr();
        wait_ticks(1);
    }

    uint64_t deadline = timer_ticks() + 50;
    while (!__atomic_load_n(&cpus[index].online, __ATOMIC_ACQUIRE) &&
           timer_ticks() < deadline)
        __asm__ volatile("sti; hlt");
}

static __attribute__((noreturn)) void smp_ap_main(unsigned index)
{
    /* Loading GS in gdt_flush refreshes its hidden descriptor base. Install
     * the MSR-backed per-CPU base only after that reload. */
    gdt_init_cpu(index, cpus[index].stack_top);
    install_cpu_local(index);
    smp_set_current_task(cpus[index].boot_task);
    idt_load();
    fpu_init_ap();
    lapic_enable();

    __atomic_store_n(&cpus[index].online, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&online_count, 1, __ATOMIC_ACQ_REL);
    sti();
    for (;;) {
        __asm__ volatile("hlt");
        yield();
    }
}

void smp_start_aps(void)
{
    if (!lapic_available || discovered < 2)
        return;

    size_t tramp_size =
        (size_t)(ap_trampoline_end - ap_trampoline_start);
    if (tramp_size > PAGE_SIZE)
        panic("smp: AP trampoline exceeds one page");
    uint8_t *tramp = P2V(AP_TRAMPOLINE_PHYS);
    memcpy(tramp, ap_trampoline_start, tramp_size);

#define PATCH(symbol, value)                                                \
    memcpy(tramp + (size_t)((symbol) - ap_trampoline_start), &(value),       \
           sizeof(value))

    uint64_t cr3 = V2P(vmm_kernel_pml4());
    PATCH(ap_trampoline_cr3, cr3);
    for (unsigned i = 1; i < discovered; i++) {
        cpus[i].boot_task = proc_prepare_cpu(i, &cpus[i].stack_top);
        if (!cpus[i].boot_task) {
            kprintf("smp: CPU %u APIC %u skipped (no bootstrap task)\n",
                    i, cpus[i].apic_id);
            continue;
        }
        uint64_t stack = cpus[i].stack_top;
        uint64_t target = (uint64_t)smp_ap_main;
        uint32_t cpu = i;
        PATCH(ap_trampoline_stack, stack);
        PATCH(ap_trampoline_target, target);
        PATCH(ap_trampoline_cpu, cpu);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        start_one_ap(i);
        if (!cpus[i].online)
            kprintf("smp: CPU %u APIC %u failed to start\n",
                    i, cpus[i].apic_id);
    }
#undef PATCH

    kprintf("smp: %u/%u CPUs online\n", smp_cpu_count(), discovered);
}
