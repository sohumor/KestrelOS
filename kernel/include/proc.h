#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "interrupts.h"

#define KSTACK_SIZE 16384
#define TASK_NAME_MAX 32
#define SCHED_QUANTUM 5          /* timer ticks per slice */
#define MAX_OPEN_FILES 16

enum task_state {
    TASK_RUNNABLE,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_ZOMBIE,
};

struct file;                     /* defined by the VFS layer */

struct task {
    int pid;
    char name[TASK_NAME_MAX];
    enum task_state state;
    uint64_t rsp;                /* saved kernel stack pointer */
    uint8_t *kstack;             /* kernel stack base */
    void *fpu_state;             /* 16-byte-aligned FXSAVE area */
    void *fpu_alloc;             /* unaligned allocation to free */
    uint64_t *pml4;              /* address space (virtual pointer) */
    bool user;                   /* has a user half to tear down */
    uint64_t sleep_until;
    int exit_code;
    struct task *parent;
    int wait_child_pid;          /* pid being waited on, 0 = none */
    uint64_t user_brk;           /* heap break for user processes */
    struct file *files[MAX_OPEN_FILES];
    struct task *qnext;          /* circular run queue */
    struct task *allnext;        /* global task list (for ps) */
};

extern struct task *current;
extern bool sched_active;

void proc_init(void);
struct task *kthread_create(void (*func)(void *), void *arg, const char *name);
void schedule(void);
void yield(void);
void task_sleep_ticks(uint64_t t);
__attribute__((noreturn)) void task_exit(int code);
void task_wake_sleepers(void);
struct task *task_all_list(void);
struct task *task_find(int pid);

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
