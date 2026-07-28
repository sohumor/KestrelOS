#pragma once

#include <stdint.h>
#include "bootinfo.h"

#define SMP_MAX_CPUS 16
#define SMP_RESCHEDULE_VECTOR 0xF0

struct task;
struct regs;

/* Discover processors, map/enable the bootstrap local APIC and install the
 * BSP's GS-based per-CPU area. Requires the rebuilt kernel page tables. */
void smp_init(struct bootinfo *bi);

/* Allocate per-CPU bootstrap tasks and issue INIT/SIPI to discovered APs.
 * Requires the heap and process scheduler. */
void smp_start_aps(void);

unsigned smp_cpu_index(void);
unsigned smp_cpu_count(void);
unsigned smp_cpu_discovered(void);
uint8_t smp_cpu_apic_id(unsigned index);

struct task *smp_current_task(void);
void smp_set_current_task(struct task *task);
unsigned smp_slice_increment(void);
void smp_slice_reset(void);

void smp_broadcast_reschedule(void);
void smp_handle_reschedule(void);
void smp_lapic_eoi(void);
