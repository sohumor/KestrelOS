#include "kernel.h"
#include "gdt.h"
#include "string.h"
#include "smp.h"

struct tss64 {
    uint32_t rsv0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t rsv1;
    uint64_t ist[7];
    uint64_t rsv2;
    uint16_t rsv3, iopb;
} __attribute__((packed));

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Every logical processor executes LTR, which marks its descriptor busy, and
 * needs an independent RSP0. Keep a complete GDT/TSS pair per CPU. */
static uint64_t gdts[SMP_MAX_CPUS][7] __attribute__((aligned(16)));
static struct tss64 tsses[SMP_MAX_CPUS] __attribute__((aligned(16)));
static struct gdtr gdtrs[SMP_MAX_CPUS];

extern void gdt_flush(void *gdtr);
extern void tss_flush(uint16_t sel);
extern uint8_t kernel_stack_top[];

static uint64_t make_desc(uint8_t access, uint8_t flags)
{
    /* base/limit are ignored in long mode for code/data */
    return ((uint64_t)access << 40) | ((uint64_t)flags << 52) | 0xFFFFULL;
}

void gdt_init_cpu(unsigned cpu, uint64_t rsp0)
{
    if (cpu >= SMP_MAX_CPUS)
        panic("gdt: CPU index %u out of range", cpu);
    uint64_t *gdt = gdts[cpu];
    struct tss64 *tss = &tsses[cpu];
    struct gdtr *gdtr = &gdtrs[cpu];

    gdt[0] = 0;
    gdt[1] = make_desc(0x9A, 0x2);   /* kernel code: L=1 */
    gdt[2] = make_desc(0x92, 0x0);   /* kernel data */
    gdt[3] = make_desc(0xFA, 0x2);   /* user code: DPL=3, L=1 */
    gdt[4] = make_desc(0xF2, 0x0);   /* user data: DPL=3 */

    memset(tss, 0, sizeof(*tss));
    tss->rsp0 = rsp0;
    tss->iopb = sizeof(*tss);

    uint64_t base = (uint64_t)tss;
    uint32_t limit = sizeof(*tss) - 1;
    gdt[5] = (limit & 0xFFFF)
           | ((base & 0xFFFFFF) << 16)
           | (0x89ULL << 40)            /* available 64-bit TSS */
           | (((uint64_t)(limit >> 16) & 0xF) << 48)
           | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;

    gdtr->limit = sizeof(gdts[cpu]) - 1;
    gdtr->base = (uint64_t)gdt;
    gdt_flush(gdtr);
    tss_flush(SEL_TSS);
}

void gdt_init(void)
{
    gdt_init_cpu(0, (uint64_t)kernel_stack_top);
}

void tss_set_rsp0(uint64_t rsp0)
{
    tsses[smp_cpu_index()].rsp0 = rsp0;
}
