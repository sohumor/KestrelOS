#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "interrupts.h"
#include "kestrel_abi.h"
#include "smp.h"

#define KSTACK_SIZE 16384
#define TASK_NAME_MAX 32
#define SCHED_QUANTUM 5          /* timer ticks per slice */
#define MAX_OPEN_FILES 16
#define VM_MAX_AREAS 16
#define VM_MAX_FILES 8

struct file;                     /* defined by the VFS layer */

/* A demand-paged ELF PT_LOAD region. File bytes occupy
 * [start, start + file_size); the remainder through end is zero-filled. */
struct vm_area {
    uint64_t start;
    uint64_t end;
    uint64_t file_offset;
    uint64_t file_size;
    uint64_t pte_flags;
    struct file *backing;
};

enum task_state {
    TASK_RUNNABLE,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_STOPPED,
    TASK_ZOMBIE,
};

struct task {
    int pid;
    char name[TASK_NAME_MAX];
    enum task_state state;
    uint32_t uid;                /* 0 = root */
    uint32_t gid;
    uint64_t rsp;                /* saved kernel stack pointer */
    uint8_t *kstack;             /* kernel stack base */
    uint64_t kernel_rsp0;        /* top used by this CPU's TSS */
    void *fpu_state;             /* 16-byte-aligned FXSAVE area */
    void *fpu_alloc;             /* unaligned allocation to free */
    uint64_t *pml4;              /* address space (virtual pointer) */
    bool user;                   /* has a user half to tear down */
    uint64_t sleep_until;
    int exit_code;
    int kill_pending;            /* set by task_kill; acted on at a safe point */
    uint64_t sig_pending;
    uint64_t sig_mask;
    struct k_sigaction sig_actions[K_NSIG];
    int parent_pid;                 /* numeric identity; never a dangling ptr */
    int wait_child_pid;          /* pid being waited on, 0 = none */
    uint64_t user_heap_start;    /* immutable first heap address */
    uint64_t user_brk;           /* heap break for user processes */
    struct vm_area vm_areas[VM_MAX_AREAS];
    int vm_area_count;
    struct file *vm_files[VM_MAX_FILES]; /* unique VMA backing handles */
    int vm_file_count;
    struct file *files[MAX_OPEN_FILES];
    struct task *qnext;          /* circular run queue */
    struct task *allnext;        /* global task list (for ps) */
    struct task *reapnext;       /* deferred SMP-safe stack reclamation */
    int cpu;                     /* running CPU, -1 while not running */
    int affinity;                /* -1 = any CPU; idle tasks are pinned */
    bool idle;
};

extern bool sched_active;
#define current (smp_current_task())

void proc_init(void);
struct task *kthread_create(void (*func)(void *), void *arg, const char *name);
struct task *kthread_create_with_pid(void (*func)(void *), void *arg,
                                     const char *name, int *pid_out);
struct task *proc_prepare_cpu(unsigned cpu, uint64_t *stack_top);
void schedule(void);
void yield(void);
void task_sleep_ticks(uint64_t t);
__attribute__((noreturn)) void task_exit(int code);
void task_wake_sleepers(void);
void task_signal_wake(struct task *t, bool resume_stopped);
bool task_exists(int pid);
int task_psinfo(uint64_t index, struct k_psinfo *out);
int task_child_pids(int parent_pid, int *out, int max);
bool task_address_space_matches(int pid, uint64_t *pml4);
int task_signal_pid(int pid, int sig, uint32_t sender_uid,
                    bool check_permission);

/* Ask a task to die. It is not killed on the spot — that could tear it down
 * mid-syscall while it holds a filesystem lock — instead the flag is checked
 * on the way out of a syscall and before returning to ring 3. */
int task_kill(struct task *t);
void task_check_kill(void);

static inline uint64_t irq_save(void)
{
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f));
    return f;
}

static inline void irq_restore(uint64_t f)
{
    if (f & 0x200)
        __asm__ volatile("sti");
}
