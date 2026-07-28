# Symmetric multiprocessing

KestrelOS discovers processors from the ACPI MADT and starts application
processors with the xAPIC INIT/SIPI sequence. The default QEMU target boots two
CPUs; `-smp 4` is used by the SMP stress run, and the kernel accepts up to 16.

## Startup

1. The BSP finds the RSDP in the EBDA or BIOS ROM area, validates the
   RSDT/XSDT and MADT checksums, and records enabled local-APIC IDs.
2. The local APIC is mapped uncached and enabled in xAPIC mode.
3. A relocation-free trampoline is copied to physical `0x1000`. For each AP,
   the BSP patches its CR3, stack, C entry point, and logical CPU index, then
   sends INIT and two SIPIs.
4. The AP installs its per-CPU GDT/TSS, IDT, FPU state and GS base, enables its
   LAPIC, publishes `online`, and enters its pinned idle task.

`/bin/nproc` reports the discovered and online counts, current logical CPU and
APIC ID through `SYS_CPUINFO`.

## Per-CPU and scheduling state

Kernel GS points at a cache-line-aligned CPU-local record whose first member is
the current task. It also carries the logical index and scheduling slice.
Ring-3 interrupt entry uses `swapgs`; return restores the user GS state.

The run ring and task list are global and protected by `sched_lock`. A task is
`RUNNING` on at most one CPU. The assembly context-switch handoff releases the
scheduler lock only after RSP has moved to the incoming stack, closing the
otherwise fatal window where another CPU could reclaim or run the outgoing
task. Idle tasks are pinned; ordinary tasks may migrate.

The BSP receives the PIT and legacy device interrupts. Each timer tick sends a
reschedule IPI to the other online CPUs, so every CPU gets a preemption
checkpoint even though Kestrel still uses the 8259 PIC rather than an IOAPIC.

## Locking rules

- IRQ-visible state uses IRQ-safe spin locks.
- Long task-context operations such as KFS and the compositor use sleepable
  owner mutexes and never make other CPUs spin across disk I/O or a frame pass.
- Address-space ownership is per running task. Only the swap-slot bitmap is
  globally locked; page faults never hold a spin lock across filesystem I/O.
- PID-based process helpers inspect the task list while holding the scheduler
  lock, so callers never retain reclaimable task pointers.

## Current boundary

Kestrel uses xAPIC, not x2APIC, and has no IOAPIC routing or NUMA support.
External IRQ work remains BSP-centric. There are no CPU hotplug or topology
policies, and the scheduler is one global round-robin queue rather than a
per-CPU load balancer.
