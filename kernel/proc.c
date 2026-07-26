#include "kernel.h"
#include "proc.h"
#include "kheap.h"
#include "string.h"
#include "gdt.h"
#include "timer.h"
#include "vmm.h"
#include "vfs.h"
#include "fpu.h"
#include "wm.h"

struct task *current;
bool sched_active;

static struct task *runq;        /* any node of the circular run ring */
static struct task *idle_task;   /* always runnable, never exits */
static struct task *all_tasks;
static struct task *reap_me;     /* zombie awaiting stack free */
static int next_pid = 1;
static unsigned slice;

extern void ctx_switch(uint64_t *save_rsp, uint64_t new_rsp);
extern void task_bootstrap(void);
extern void (*irq_preempt_hook)(struct regs *r);

struct task *task_all_list(void) { return all_tasks; }

struct task *task_find(int pid)
{
    for (struct task *t = all_tasks; t; t = t->allnext)
        if (t->pid == pid)
            return t;
    return NULL;
}

/* --- run ring management (callers hold interrupts off) --- */

static void runq_insert(struct task *t)
{
    if (!runq) {
        runq = t;
        t->qnext = t;
    } else {
        t->qnext = runq->qnext;
        runq->qnext = t;
    }
}

static void runq_remove(struct task *t)
{
    struct task *p = runq;
    if (t->qnext == t) {
        runq = NULL;
        t->qnext = NULL;
        return;
    }
    while (p->qnext != t)
        p = p->qnext;
    p->qnext = t->qnext;
    if (runq == t)
        runq = p;
    t->qnext = NULL;
}

void task_wake_sleepers(void)
{
    if (!runq)
        return;
    uint64_t now = timer_ticks();
    struct task *t = runq;
    do {
        if (t->state == TASK_SLEEPING && t->sleep_until <= now)
            t->state = TASK_RUNNABLE;
        t = t->qnext;
    } while (t != runq);
}

static void fpu_attach(struct task *t)
{
    t->fpu_alloc = kmalloc(FPU_STATE_SIZE + 16);
    if (!t->fpu_alloc)
        return;
    t->fpu_state = (void *)(((uint64_t)t->fpu_alloc + 15) & ~15ULL);
    fpu_state_init(t->fpu_state);
}

static void all_tasks_remove(struct task *t)
{
    if (all_tasks == t) {
        all_tasks = t->allnext;
        return;
    }
    for (struct task *p = all_tasks; p; p = p->allnext) {
        if (p->allnext == t) {
            p->allnext = t->allnext;
            return;
        }
    }
}

static void reap(void)
{
    if (reap_me && reap_me != current) {
        all_tasks_remove(reap_me);
        kfree(reap_me->kstack);
        kfree(reap_me->fpu_alloc);
        kfree(reap_me);
        reap_me = NULL;
    }
}

/* Next runnable task strictly round-robin after `from` (which must be in
 * the ring). The idle task is only chosen if nothing else is runnable. */
static struct task *pick_next(struct task *from)
{
    struct task *t = from;
    struct task *fallback = NULL;
    do {
        t = t->qnext;
        if (t->state == TASK_RUNNABLE || t->state == TASK_RUNNING) {
            if (t == idle_task)
                fallback = fallback ? fallback : t;
            else
                return t;
        }
    } while (t != from);
    return fallback;
}

void schedule(void)
{
    uint64_t f = irq_save();
    reap();
    task_wake_sleepers();

    struct task *prev = current;
    struct task *from = prev->qnext ? prev : runq;
    struct task *pick = from ? pick_next(from) : NULL;
    if (!pick)
        panic("scheduler: nothing runnable");

    slice = 0;
    if (pick == prev) {
        irq_restore(f);
        return;
    }

    if (prev->state == TASK_RUNNING)
        prev->state = TASK_RUNNABLE;
    pick->state = TASK_RUNNING;
    current = pick;

    tss_set_rsp0((uint64_t)pick->kstack + KSTACK_SIZE);
    if (pick->pml4 != prev->pml4)
        vmm_switch(pick->pml4);

    fpu_save(prev->fpu_state);
    fpu_restore(pick->fpu_state);
    ctx_switch(&prev->rsp, pick->rsp);
    /* running as `prev` again */
    reap();
    irq_restore(f);
}

void yield(void)
{
    schedule();
}

void task_sleep_ticks(uint64_t t)
{
    uint64_t f = irq_save();
    current->state = TASK_SLEEPING;
    current->sleep_until = timer_ticks() + t;
    schedule();
    irq_restore(f);
}

void task_exit(int code)
{
    /* Before the address space goes away: the compositor clears its own
     * PTEs and returns the pixel frames itself. */
    wm_cleanup_task(current->pid);

    cli();
    current->state = TASK_ZOMBIE;
    current->exit_code = code;

    /* Nothing else references these handles, and the task struct itself is
     * about to be freed — a ring-3 fault must not leak them. */
    for (int fd = 0; fd < MAX_OPEN_FILES; fd++) {
        if (current->files[fd]) {
            vfs_close(current->files[fd]);
            current->files[fd] = NULL;
        }
    }

    if (current->parent && current->parent->state == TASK_SLEEPING &&
        current->parent->wait_child_pid == current->pid)
        current->parent->state = TASK_RUNNABLE;

    runq_remove(current);
    if (current->user) {
        vmm_switch(vmm_kernel_pml4());
        uint64_t *upml4 = current->pml4;
        current->pml4 = vmm_kernel_pml4();
        vmm_destroy_user(upml4);
        current->user = false;
    }
    if (reap_me) {
        all_tasks_remove(reap_me);
        kfree(reap_me->kstack);
        kfree(reap_me->fpu_alloc);
        kfree(reap_me);
    }
    reap_me = current;
    schedule();
    panic("task_exit: zombie returned");
}

void task_exit_from_bootstrap(int code)
{
    task_exit(code);
}

struct task *kthread_create(void (*func)(void *), void *arg, const char *name)
{
    struct task *t = kzalloc(sizeof(*t));
    if (!t)
        return NULL;
    strncpy(t->name, name, TASK_NAME_MAX - 1);
    t->state = TASK_RUNNABLE;
    t->kstack = kmalloc(KSTACK_SIZE);
    t->pml4 = vmm_kernel_pml4();
    t->parent = current;
    /* Children run as their parent, so the VFS permission checks mean
     * something once login drops privileges. */
    if (current) {
        t->uid = current->uid;
        t->gid = current->gid;
    }
    fpu_attach(t);
    if (!t->kstack || !t->fpu_alloc) {
        kfree(t->kstack);
        kfree(t->fpu_alloc);
        kfree(t);
        return NULL;
    }

    /* Initial frame popped by ctx_switch: r15 r14 r13 r12 rbx rbp | rip.
     * task_bootstrap expects r12 = entry function, r13 = argument. */
    uint64_t *sp = (uint64_t *)(t->kstack + KSTACK_SIZE);
    *--sp = (uint64_t)task_bootstrap;   /* rip */
    *--sp = 0;                          /* rbp */
    *--sp = 0;                          /* rbx */
    *--sp = (uint64_t)func;             /* r12 */
    *--sp = (uint64_t)arg;              /* r13 */
    *--sp = 0;                          /* r14 */
    *--sp = 0;                          /* r15 */
    t->rsp = (uint64_t)sp;

    uint64_t f = irq_save();
    t->pid = next_pid++;
    t->allnext = all_tasks;
    all_tasks = t;
    runq_insert(t);
    irq_restore(f);
    return t;
}

static void idle_thread(void *arg)
{
    (void)arg;
    for (;;) {
        __asm__ volatile("sti; hlt");
        yield();
    }
}

int task_kill(struct task *t)
{
    if (!t || t->state == TASK_ZOMBIE)
        return -1;
    t->kill_pending = 1;
    if (t->state == TASK_SLEEPING)
        t->state = TASK_RUNNABLE;    /* so it reaches its next checkpoint */
    return 0;
}

/* Safe points only: on the way out of a syscall, and just before returning
 * to ring 3. Anywhere else the task might hold the filesystem lock. */
void task_check_kill(void)
{
    if (current && current->kill_pending)
        task_exit(-1);
}

static void preempt(struct regs *r)
{
    if (!sched_active)
        return;
    if (r->vector == 32) {
        task_wake_sleepers();
        /* A kill only takes effect where the victim was running usermode,
         * so it can never be torn down inside a kernel critical section. */
        if (current && current->kill_pending && (r->cs & 3) == 3)
            task_exit(-1);
        if (++slice >= SCHED_QUANTUM)
            schedule();
    }
}

void proc_init(void)
{
    struct task *t = kzalloc(sizeof(*t));
    t->pid = 0;
    strncpy(t->name, "kernel", TASK_NAME_MAX - 1);
    t->state = TASK_RUNNING;
    t->kstack = NULL;                   /* boot stack, never freed */
    t->pml4 = vmm_kernel_pml4();
    fpu_attach(t);
    current = t;
    all_tasks = t;
    runq_insert(t);

    idle_task = kthread_create(idle_thread, NULL, "idle");

    irq_preempt_hook = preempt;
    sched_active = true;
}
