#pragma once

#include <stdint.h>
#include <stddef.h>

struct task;

/* The generated boot image carries a self-describing raw swap extent after
 * KFS. A missing or invalid extent simply disables swap. */
void swap_init(void);
uint64_t swap_total_pages(void);
uint64_t swap_used_pages(void);

/* Resolve a non-present userspace page. `error` is the x86 page-fault error
 * code. Returns 0 if the instruction may be retried, -1 for an invalid or
 * unrecoverable access. */
int vm_handle_page_fault(struct task *task, uint64_t address, uint64_t error);

/* Materialize a valid userspace range before the kernel directly copies it.
 * `write` requests write permission. */
int vm_fault_in_range(struct task *task, uint64_t address, size_t len,
                      int write);

/* Release swap slots owned by a task before its page tables are destroyed. */
void vm_release_task(struct task *task);
