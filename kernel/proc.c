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
#include "vm.h"
#include "signal.h"
#include "uproc.h"
#include "smp.h"
#include "spinlock.h"
#include "flight.h"
#include "mission.h"

bool sched_active;

static struct task *runq;        /* any node of the circular run ring */
static struct task *all_tasks;
static struct task *reap_list;   /* stacks safe after scheduler handoff */
static int next_pid = 1;
static spinlock_t sched_lock = SPINLOCK_INIT;

struct task_event_waiter {
    struct task_event_waiter *next;
    struct task *task;
    uint32_t mask;
};

extern void ctx_switch(uint64_t *save_rsp, uint64_t new_rsp);
extern void ctx_switch_unlock(uint64_t *save_rsp, uint64_t new_rsp,
                              volatile uint32_t *lock);
extern void task_bootstrap(void);
extern void (*irq_preempt_hook)(struct regs *r);
extern uint8_t kernel_stack_top[];

static struct task *task_find_locked(int pid)
{
    for (struct task *t = all_tasks; t; t = t->allnext)
        if (t->pid == pid)
            return t;
    return NULL;
}

bool task_exists(int pid)
{
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    bool found = task_find_locked(pid) != NULL;
    spin_unlock_irqrestore(&sched_lock, flags);
    return found;
}

bool task_is_child(int parent_pid, int pid)
{
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct task *t = task_find_locked(pid);
    bool found = t && t->parent_pid == parent_pid && t->pid != parent_pid;
    spin_unlock_irqrestore(&sched_lock, flags);
    return found;
}

/* Whether pid can still wait for its children's exit statuses. */
bool task_collects_exits(int pid)
{
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct task *t = task_find_locked(pid);
    bool collects = t && !t->exits_released;
    spin_unlock_irqrestore(&sched_lock, flags);
    return collects;
}

int task_child_pids(int parent_pid, int *out, int max)
{
    if (!out || max <= 0)
        return 0;
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    int count = 0;
    for (struct task *t = all_tasks; t && count < max; t = t->allnext)
        if (t->parent_pid == parent_pid && t->pid != parent_pid)
            out[count++] = t->pid;
    spin_unlock_irqrestore(&sched_lock, flags);
    return count;
}

bool task_address_space_matches(int pid, uint64_t *pml4)
{
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct task *t = task_find_locked(pid);
    bool matches = t && t->pml4 == pml4 &&
                   t->pml4 != vmm_kernel_pml4();
    spin_unlock_irqrestore(&sched_lock, flags);
    return matches;
}

int task_signal_pid(int pid, int sig, uint32_t sender_uid,
                    bool check_permission)
{
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct task *t = task_find_locked(pid);
    if (!t || t->state == TASK_ZOMBIE ||
        (check_permission && sender_uid != 0 && sender_uid != t->uid)) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return -1;
    }
    if (sig == 0) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return 0;
    }
    int rc = signal_queue_task(t, sig);
    if (rc == 0 &&
        (t->state == TASK_SLEEPING ||
         ((sig == SIGCONT || sig == SIGKILL) && t->state == TASK_STOPPED)))
        t->state = TASK_RUNNABLE;
    spin_unlock_irqrestore(&sched_lock, flags);
    return rc;
}

int task_psinfo(uint64_t index, struct k_psinfo *pi)
{
    if (!pi)
        return -1;
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct task *t = all_tasks;
    for (uint64_t i = 0; t && i < index; i++)
        t = t->allnext;
    if (!t) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return -1;
    }
    memset(pi, 0, sizeof(*pi));
    pi->pid = t->pid;
    pi->uid = t->uid;
    pi->ppid = t->parent_pid;
    strncpy(pi->name, t->name, sizeof(pi->name) - 1);
    switch (t->state) {
    case TASK_RUNNABLE: pi->state = K_STATE_RUNNABLE; break;
    case TASK_RUNNING:  pi->state = K_STATE_RUNNING;  break;
    case TASK_SLEEPING: pi->state = K_STATE_SLEEPING; break;
    case TASK_STOPPED:  pi->state = K_STATE_STOPPED;  break;
    case TASK_ZOMBIE:   pi->state = K_STATE_ZOMBIE;   break;
    default:            pi->state = K_STATE_ZOMBIE;   break;
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return 0;
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
    /* Callers promise t is a ring member. If that is ever violated, either
     * the wrong head gets dropped (singleton case) or the search below
     * spins forever with interrupts off — a panic diagnoses it instead. */
    if (!runq || (t->qnext == t && runq != t))
        panic("sched: runq_remove: task %d not in run ring", t->pid);
    struct task *p = runq;
    if (t->qnext == t) {
        runq = NULL;
        t->qnext = NULL;
        return;
    }
    while (p->qnext != t) {
        p = p->qnext;
        if (p == runq)
            panic("sched: runq_remove: task %d not in run ring", t->pid);
    }
    p->qnext = t->qnext;
    if (runq == t)
        runq = p;
    t->qnext = NULL;
}

static void wake_sleepers_locked(void)
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

void task_wake_sleepers(void)
{
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    wake_sleepers_locked();
    spin_unlock_irqrestore(&sched_lock, flags);
}

void task_signal_wake(struct task *t, bool resume_stopped)
{
    if (!t)
        return;
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    if (t->state == TASK_SLEEPING ||
        (resume_stopped && t->state == TASK_STOPPED))
        t->state = TASK_RUNNABLE;
    spin_unlock_irqrestore(&sched_lock, flags);
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

static void reap_locked(void)
{
    struct task **link = &reap_list;
    while (*link) {
        struct task *t = *link;
        /* task_exit queues itself before ctx_switch_unlock has stopped using
         * its stack. The current task is therefore never reclaimable by its
         * own scheduling invocation; a later invocation (or another CPU)
         * will remove it after the assembly handoff. */
        if (t == current) {
            link = &t->reapnext;
            continue;
        }
        *link = t->reapnext;
        all_tasks_remove(t);
        kfree(t->kstack);
        kfree(t->fpu_alloc);
        kfree(t);
    }
}

/* Next runnable task strictly round-robin after `from` (which must be in
 * the ring). Per-CPU idle tasks are considered only on their assigned CPU. */
static struct task *pick_next(struct task *from, unsigned cpu)
{
    struct task *t = from;
    struct task *fallback = NULL;
    do {
        t = t->qnext;
        if (t->state != TASK_RUNNABLE)
            continue;
        if (t->affinity >= 0 && (unsigned)t->affinity != cpu)
            continue;
        if (t->idle)
            fallback = fallback ? fallback : t;
        else
            return t;
    } while (t != from);
    return fallback;
}

/* Entered with local interrupts off and sched_lock held. The assembly switch
 * releases sched_lock only after it has stopped using the outgoing stack. */
static void schedule_locked(uint64_t flags)
{
    reap_locked();
    wake_sleepers_locked();

    struct task *prev = current;
    struct task *from = prev->qnext ? prev : runq;
    unsigned cpu = smp_cpu_index();
    struct task *pick = from ? pick_next(from, cpu) : NULL;
    if (!pick && prev->state == TASK_RUNNING)
        pick = prev;
    if (!pick)
        panic("scheduler: nothing runnable");

    smp_slice_reset();
    if (pick == prev) {
        spin_unlock(&sched_lock);
        irq_restore(flags);
        return;
    }

    if (prev->state == TASK_RUNNING)
        prev->state = TASK_RUNNABLE;
    if (prev->state != TASK_RUNNING)
        prev->cpu = -1;
    pick->state = TASK_RUNNING;
    pick->cpu = (int)cpu;
    smp_set_current_task(pick);

    tss_set_rsp0(pick->kernel_rsp0);
    if (pick->pml4 != prev->pml4)
        vmm_switch(pick->pml4);

    fpu_save(prev->fpu_state);
    fpu_restore(pick->fpu_state);
    ctx_switch_unlock(&prev->rsp, pick->rsp, &sched_lock.value);
    /* This invocation resumes only when its original task is selected again.
     * The assembly handoff already released sched_lock. */
    irq_restore(flags);
}

void schedule(void)
{
    uint64_t flags = irq_save();
    spin_lock(&sched_lock);
    schedule_locked(flags);
}

void yield(void)
{
    schedule();
}

void task_sleep_ticks(uint64_t t)
{
    uint64_t f = irq_save();
    spin_lock(&sched_lock);
    current->state = TASK_SLEEPING;
    current->cpu = -1;
    current->sleep_until = timer_ticks() + t;
    schedule_locked(f);
}

static bool event_wait_cancelled(const struct task *task)
{
    return task->kill_pending ||
           (__atomic_load_n(&task->sig_pending, __ATOMIC_SEQ_CST) &
            (1ULL << (SIGKILL - 1)));
}

static int event_wait_locked(struct task_event_channel *channel, uint32_t mask,
                             uint64_t deadline, spinlock_t *lock,
                             uint64_t flags, bool interruptible)
{
    struct task_event_waiter waiter = { .task = current, .mask = mask };

    spin_lock(&sched_lock);
    if (event_wait_cancelled(waiter.task) ||
        (interruptible && signal_interrupt_pending(waiter.task))) {
        spin_unlock(&sched_lock);
        spin_unlock_irqrestore(lock, flags);
        return -1;
    }
    waiter.next = channel->waiters;
    channel->waiters = &waiter;
    waiter.task->state = TASK_SLEEPING;
    waiter.task->cpu = -1;
    waiter.task->sleep_until = deadline;
    spin_unlock(lock);
    schedule_locked(flags);

    uint64_t f = spin_lock_irqsave(&sched_lock);
    struct task_event_waiter **link = &channel->waiters;
    while (*link != &waiter) {
        if (!*link)
            panic("sched: event waiter already unlinked");
        link = &(*link)->next;
    }
    *link = waiter.next;
    bool cancelled = !interruptible && event_wait_cancelled(waiter.task);
    spin_unlock_irqrestore(&sched_lock, f);
    return cancelled ? -1 : 0;
}

int task_event_wait_locked(struct task_event_channel *channel, uint32_t mask,
                           uint64_t deadline, spinlock_t *lock, uint64_t flags)
{
    return event_wait_locked(channel, mask, deadline, lock, flags, false);
}

int task_event_wait_interruptible_locked(struct task_event_channel *channel,
                                         uint32_t mask, spinlock_t *lock,
                                         uint64_t flags)
{
    return event_wait_locked(channel, mask, UINT64_MAX, lock, flags, true);
}

void task_event_notify(struct task_event_channel *channel, uint32_t mask)
{
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    for (struct task_event_waiter *w = channel->waiters; w; w = w->next)
        if ((w->mask & mask) && w->task->state == TASK_SLEEPING)
            w->task->state = TASK_RUNNABLE;
    spin_unlock_irqrestore(&sched_lock, flags);
}

void task_stop_current(void)
{
    uint64_t flags = irq_save();
    spin_lock(&sched_lock);
    if (!current || current->kill_pending ||
        (__atomic_load_n(&current->sig_pending, __ATOMIC_SEQ_CST) &
         ((1ULL << (SIGKILL - 1)) | (1ULL << (SIGCONT - 1))))) {
        spin_unlock(&sched_lock);
        irq_restore(flags);
        return;
    }
    current->state = TASK_STOPPED;
    current->cpu = -1;
    schedule_locked(flags);
}

void task_exit(int code)
{
    flight_record(FLIGHT_CAT_PROC, "exit", (int16_t)current->pid,
                  (uint64_t)(long)code, (uint64_t)current->parent_pid,
                  current->name);

    /* Before the address space goes away: the compositor clears its own
     * PTEs and returns the pixel frames itself. */
    wm_cleanup_task(current->pid);
    uproc_release_exits();

    /* Nothing else references these handles, and the task struct itself is
     * about to be freed — a ring-3 fault must not leak them. */
    for (int fd = 0; fd < MAX_OPEN_FILES; fd++) {
        if (current->files[fd]) {
            vfs_close(current->files[fd]);
            current->files[fd] = NULL;
        }
    }

    /* ELF backing handles can wait for the filesystem mutex. Tear the user
     * address space down while this task is still its sole RUNNING owner,
     * before taking sched_lock; sleeping while holding sched_lock would
     * deadlock every CPU trying to select work. */
    if (current->user) {
        vm_release_task(current);
        vmm_switch(vmm_kernel_pml4());
        uint64_t *upml4 = current->pml4;
        current->pml4 = vmm_kernel_pml4();
        current->user = false;
        vmm_destroy_user(upml4);
    }

    /* Queue by numeric identity so an already-exited parent can never leave
     * a dangling task pointer in a long-lived child. */
    if (current->parent_pid > 0)
        task_signal_pid(current->parent_pid, SIGCHLD, 0, false);

    uint64_t flags = irq_save();
    spin_lock(&sched_lock);
    int mission_id = current->mission_id;
    current->mission_id = 0;
    spin_unlock_irqrestore(&sched_lock, flags);
    mission_put(mission_id);

    flags = irq_save();
    spin_lock(&sched_lock);
    current->state = TASK_ZOMBIE;
    current->cpu = -1;
    current->exit_code = code;

    runq_remove(current);
    current->reapnext = reap_list;
    reap_list = current;
    schedule_locked(flags);
    panic("task_exit: zombie returned");
}

void task_exit_from_bootstrap(int code)
{
    task_exit(code);
}

struct task *kthread_create_mission(void (*func)(void *), void *arg,
                                   const char *name, int *pid_out, int mission_id)
{
    struct task *t = kzalloc(sizeof(*t));
    if (!t)
        return NULL;
    strncpy(t->name, name, TASK_NAME_MAX - 1);
    t->state = TASK_RUNNABLE;
    t->kstack = kmalloc(KSTACK_SIZE);
    t->kernel_rsp0 = (uint64_t)t->kstack + KSTACK_SIZE;
    t->pml4 = vmm_kernel_pml4();
    t->parent_pid = current ? current->pid : 0;
    t->cpu = -1;
    t->affinity = -1;
    t->cap_mask = KCAP_ALL;
    t->unveil_prefix[0] = '\0';
    /* Children run as their parent, so the VFS permission checks mean
     * something once login drops privileges. */
    if (current) {
        t->uid = current->uid;
        t->gid = current->gid;
        if (current->user) {
            t->cap_mask = current->cap_mask;
            strncpy(t->unveil_prefix, current->unveil_prefix,
                    sizeof(t->unveil_prefix) - 1);
            t->unveil_prefix[sizeof(t->unveil_prefix) - 1] = '\0';
        }
        if (t->uid != 0)
            t->cap_mask &= ~KCAP_ADMIN;
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
    spin_lock(&sched_lock);
    t->mission_id = mission_id;
    mission_inherit(t, current);
    t->pid = next_pid++;
    t->allnext = all_tasks;
    all_tasks = t;
    runq_insert(t);
    if (pid_out)
        *pid_out = t->pid;
    spin_unlock_irqrestore(&sched_lock, f);
    return t;
}

struct task *kthread_create_with_pid(void (*func)(void *), void *arg,
                                     const char *name, int *pid_out)
{
    return kthread_create_mission(func, arg, name, pid_out, 0);
}

void proc_mission_ctl(int id, uint64_t drop, int end)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    for (struct task *t = all_tasks; t; t = t->allnext) {
        if (t->mission_id != id || t->state == TASK_ZOMBIE) continue;
        __atomic_fetch_and(&t->cap_mask, ~drop, __ATOMIC_SEQ_CST);
        if (end) {
            t->kill_pending = 1;
            if (t->state == TASK_SLEEPING || t->state == TASK_STOPPED) {
                t->state = TASK_RUNNABLE;
            }
        }
    }
    spin_unlock_irqrestore(&sched_lock, f);
}

void proc_mission_pids(struct k_mission_info *out)
{
    out->task_count = out->pid_count = 0;
    uint64_t f = spin_lock_irqsave(&sched_lock);
    for (struct task *t = all_tasks; t; t = t->allnext) {
        if (t->mission_id != (int)out->id || t->state == TASK_ZOMBIE) continue;
        out->task_count++;
        if (out->pid_count < K_MISSION_PIDS) out->pids[out->pid_count++] = t->pid;
    }
    spin_unlock_irqrestore(&sched_lock, f);
}

struct task *kthread_create(void (*func)(void *), void *arg, const char *name)
{
    return kthread_create_with_pid(func, arg, name, NULL);
}

struct task *proc_prepare_cpu(unsigned cpu, uint64_t *stack_top)
{
    struct task *t = kzalloc(sizeof(*t));
    if (!t)
        return NULL;
    t->kstack = kmalloc(KSTACK_SIZE);
    t->pml4 = vmm_kernel_pml4();
    t->state = TASK_RUNNING;
    t->idle = true;
    t->affinity = (int)cpu;
    t->cpu = (int)cpu;
    t->cap_mask = KCAP_ALL;
    t->unveil_prefix[0] = '\0';
    strncpy(t->name, "idle/ap", TASK_NAME_MAX - 1);
    fpu_attach(t);
    if (!t->kstack || !t->fpu_alloc) {
        kfree(t->kstack);
        kfree(t->fpu_alloc);
        kfree(t);
        return NULL;
    }
    t->kernel_rsp0 = (uint64_t)t->kstack + KSTACK_SIZE;
    t->rsp = t->kernel_rsp0;
    if (stack_top)
        *stack_top = t->kernel_rsp0;

    uint64_t f = irq_save();
    spin_lock(&sched_lock);
    t->pid = next_pid++;
    t->allnext = all_tasks;
    all_tasks = t;
    runq_insert(t);
    spin_unlock_irqrestore(&sched_lock, f);
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
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    if (!t || t->state == TASK_ZOMBIE)
    {
        spin_unlock_irqrestore(&sched_lock, flags);
        return -1;
    }
    t->kill_pending = 1;
    if (t->state == TASK_SLEEPING || t->state == TASK_STOPPED)
        t->state = TASK_RUNNABLE;    /* so it reaches its next checkpoint */
    spin_unlock_irqrestore(&sched_lock, flags);
    signal_send(t, SIGKILL);
    return 0;
}

/* Safe points only: on the way out of a syscall, and just before returning
 * to ring 3. Anywhere else the task might hold the filesystem lock. */
void task_check_kill(void)
{
    if (current && current->kill_pending) {
        uproc_record_exit(current->pid, 128 + SIGKILL);
        task_exit(128 + SIGKILL);
    }
}

int task_park_request(int pid, uint32_t caller_uid)
{
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct task *t = task_find_locked(pid);
    int rc = 0;

    if (!t || !t->user || t->state == TASK_ZOMBIE || t->kill_pending ||
        (caller_uid != 0 && caller_uid != t->uid))
        rc = -1;
    else if (t->mission_id != 0)
        rc = -3;   /* parking would drop the mission confinement on resume */
    else if (t->park_state != 0)
        rc = -2;
    else {
        t->park_state = 1;
        if (t->state == TASK_SLEEPING)
            t->state = TASK_RUNNABLE;   /* reach the checkpoint promptly */
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return rc;
}

int task_park_cancel(int pid)
{
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct task *t = task_find_locked(pid);
    int rc = 0;

    if (t && t->park_state == 1) {
        t->park_state = 0;
        rc = 1;
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return rc;
}

int task_park_begin(void)
{
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    int rc = 0;

    if (current && current->park_state == 1) {
        current->park_state = 2;
        rc = 1;
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return rc;
}

static void preempt(struct regs *r)
{
    if (!sched_active)
        return;
    if (r->vector == 32 || r->vector == SMP_RESCHEDULE_VECTOR) {
        /* A kill only takes effect where the victim was running usermode,
         * so it can never be torn down inside a kernel critical section. */
        if (current && current->kill_pending && (r->cs & 3) == 3)
            uproc_record_exit(current->pid, 128 + SIGKILL);
        if (current && current->kill_pending && (r->cs & 3) == 3)
            task_exit(128 + SIGKILL);
        if (smp_slice_increment() >= SCHED_QUANTUM)
            schedule();
        if (current && (r->cs & 3) == 3)
            signal_deliver_pending(current, r);
    }
}

void proc_init(void)
{
    struct task *t = kzalloc(sizeof(*t));
    t->pid = 0;
    strncpy(t->name, "kernel", TASK_NAME_MAX - 1);
    t->state = TASK_RUNNING;
    t->kstack = NULL;                   /* boot stack, never freed */
    t->kernel_rsp0 = (uint64_t)kernel_stack_top;
    t->pml4 = vmm_kernel_pml4();
    t->idle = false;
    t->affinity = 0;
    t->cpu = 0;
    t->cap_mask = KCAP_ALL;
    t->unveil_prefix[0] = '\0';
    fpu_attach(t);
    smp_set_current_task(t);
    all_tasks = t;
    runq_insert(t);

    struct task *idle = kthread_create(idle_thread, NULL, "idle/0");
    if (!idle)
        panic("scheduler: cannot allocate BSP idle task");
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    idle->idle = true;
    idle->affinity = 0;
    spin_unlock_irqrestore(&sched_lock, flags);

    irq_preempt_hook = preempt;
    sched_active = true;
}

void proc_mark_current_idle(void)
{
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    if (current)
        current->idle = true;
    spin_unlock_irqrestore(&sched_lock, flags);
}

/* --- shared @krec kernel table formatter ------------------------------ */

void krec_kbuf_init(struct krec_kbuf *b, char *out, uint32_t cap)
{
    b->out = out;
    b->len = 0;
    b->cap = cap;
    b->overflow = false;
}

void krec_kbuf_putc(struct krec_kbuf *b, char c)
{
    if (b->len + 1 < b->cap) {
        if (b->out)
            b->out[b->len] = c;
        b->len++;
    } else {
        b->overflow = true;
    }
}

void krec_kbuf_puts(struct krec_kbuf *b, const char *s)
{
    if (!s)
        return;
    while (*s)
        krec_kbuf_putc(b, *s++);
}

void krec_kbuf_escape(struct krec_kbuf *b, const char *s)
{
    if (!s || !s[0]) {
        krec_kbuf_putc(b, '-');
        return;
    }
    for (; *s; s++) {
        unsigned char uc = (unsigned char)*s;
        switch (*s) {
        case '\\': krec_kbuf_putc(b, '\\'); krec_kbuf_putc(b, '\\'); break;
        case '\t': krec_kbuf_putc(b, '\\'); krec_kbuf_putc(b, 't');  break;
        case '\n': krec_kbuf_putc(b, '\\'); krec_kbuf_putc(b, 'n');  break;
        case '\r': krec_kbuf_putc(b, '\\'); krec_kbuf_putc(b, 'r');  break;
        default:
            krec_kbuf_putc(b, (uc < 0x20 || uc >= 0x7f) ? '?' : *s);
            break;
        }
    }
}

void krec_kbuf_put_u64(struct krec_kbuf *b, uint64_t v)
{
    char tmp[24];
    int i = 0;

    if (v == 0) {
        krec_kbuf_putc(b, '0');
        return;
    }
    while (v > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0)
        krec_kbuf_putc(b, tmp[--i]);
}

void krec_kbuf_put_i64(struct krec_kbuf *b, int64_t v)
{
    if (v < 0) {
        krec_kbuf_putc(b, '-');
        krec_kbuf_put_u64(b, (uint64_t)(-(v + 1)) + 1ULL);
    } else {
        krec_kbuf_put_u64(b, (uint64_t)v);
    }
}

void krec_kbuf_put_hex(struct krec_kbuf *b, uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    char tmp[16];
    int i = 0;

    krec_kbuf_puts(b, "0x");
    if (v == 0) {
        krec_kbuf_putc(b, '0');
        return;
    }
    while (v > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = hex[v & 0xFULL];
        v >>= 4;
    }
    while (i > 0)
        krec_kbuf_putc(b, tmp[--i]);
}

void krec_kbuf_put_ip4(struct krec_kbuf *b, uint32_t ip_be)
{
    krec_kbuf_put_u64(b, ip_be & 0xFFU);
    krec_kbuf_putc(b, '.');
    krec_kbuf_put_u64(b, (ip_be >> 8) & 0xFFU);
    krec_kbuf_putc(b, '.');
    krec_kbuf_put_u64(b, (ip_be >> 16) & 0xFFU);
    krec_kbuf_putc(b, '.');
    krec_kbuf_put_u64(b, (ip_be >> 24) & 0xFFU);
}

uint32_t krec_kbuf_finish(struct krec_kbuf *b)
{
    if (b->out && b->cap > 0)
        b->out[b->len] = '\0';
    return b->len;
}

/* --- /dev/procs & /dev/caps @krec snapshots --------------------------- */

struct proc_snap_row {
    int pid;
    int ppid;
    uint32_t uid;
    int cpu;
    enum task_state state;
    char name[TASK_NAME_MAX];
    uint64_t cap_mask;
    char unveil_prefix[64];
};

static const char *task_state_name(enum task_state s)
{
    switch (s) {
    case TASK_RUNNABLE: return "runnable";
    case TASK_RUNNING:  return "running";
    case TASK_SLEEPING: return "sleeping";
    case TASK_STOPPED:  return "stopped";
    case TASK_ZOMBIE:   return "zombie";
    default:            return "unknown";
    }
}

static struct proc_snap_row *snapshot_all_tasks(int *count_out)
{
    int cap = 63; /* 63 * 128 + 8 <= 8192 (2 pages) */

    *count_out = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        struct proc_snap_row *arr = kmalloc((size_t)cap * sizeof(*arr));
        if (!arr)
            return NULL;

        uint64_t flags = spin_lock_irqsave(&sched_lock);
        int total = 0;
        int n = 0;
        for (struct task *t = all_tasks; t; t = t->allnext) {
            total++;
            if (n < cap) {
                arr[n].pid = t->pid;
                arr[n].ppid = t->parent_pid;
                arr[n].uid = t->uid;
                arr[n].cpu = t->cpu;
                arr[n].state = t->state;
                arr[n].cap_mask = t->cap_mask;
                memcpy(arr[n].name, t->name, TASK_NAME_MAX);
                arr[n].name[TASK_NAME_MAX - 1] = '\0';
                memcpy(arr[n].unveil_prefix, t->unveil_prefix,
                       sizeof(arr[n].unveil_prefix));
                arr[n].unveil_prefix[sizeof(arr[n].unveil_prefix) - 1] = '\0';
                n++;
            }
        }
        spin_unlock_irqrestore(&sched_lock, flags);

        if (total <= cap || attempt == 1) {
            *count_out = n;
            return arr;
        }
        kfree(arr);
        cap = total + 16;
    }
    return NULL;
}

uint32_t proc_snapshot_krec(char *buf, uint32_t cap)
{
    struct krec_kbuf b;

    if (cap == 0)
        return 0;

    krec_kbuf_init(&b, buf, cap);
    krec_kbuf_puts(&b, "@krec pid:int ppid:int uid:int cpu:int state:str name:str caps:hex\n");
    if (b.overflow) {
        b.len = 0;
        return krec_kbuf_finish(&b);
    }

    int count = 0;
    struct proc_snap_row *rows = snapshot_all_tasks(&count);
    if (!rows)
        return krec_kbuf_finish(&b);

    for (int i = 0; i < count; i++) {
        uint32_t mark = b.len;
        krec_kbuf_put_i64(&b, rows[i].pid);
        krec_kbuf_putc(&b, '\t');
        krec_kbuf_put_i64(&b, rows[i].ppid);
        krec_kbuf_putc(&b, '\t');
        krec_kbuf_put_u64(&b, rows[i].uid);
        krec_kbuf_putc(&b, '\t');
        krec_kbuf_put_i64(&b, rows[i].cpu);
        krec_kbuf_putc(&b, '\t');
        krec_kbuf_puts(&b, task_state_name(rows[i].state));
        krec_kbuf_putc(&b, '\t');
        krec_kbuf_escape(&b, rows[i].name);
        krec_kbuf_putc(&b, '\t');
        krec_kbuf_put_hex(&b, rows[i].cap_mask);
        krec_kbuf_putc(&b, '\n');
        if (b.overflow) {
            b.len = mark;
            break;
        }
    }

    kfree(rows);
    return krec_kbuf_finish(&b);
}

static void format_caps_str(struct krec_kbuf *b, uint64_t cap_mask)
{
    static const struct {
        uint64_t bit;
        const char *name;
    } cap_tab[] = {
        { KCAP_FS_READ,  "fs_read"  },
        { KCAP_FS_WRITE, "fs_write" },
        { KCAP_NET,      "net"      },
        { KCAP_WM,       "wm"       },
        { KCAP_SPAWN,    "spawn"    },
        { KCAP_IPC,      "ipc"      },
        { KCAP_ADMIN,    "admin"    },
    };
    uint64_t m = cap_mask & KCAP_ALL;
    bool first = true;

    if (m == KCAP_ALL) {
        krec_kbuf_puts(b, "all");
        return;
    }
    if (m == 0) {
        krec_kbuf_puts(b, "none");
        return;
    }
    for (size_t i = 0; i < sizeof(cap_tab) / sizeof(cap_tab[0]); i++) {
        if (m & cap_tab[i].bit) {
            if (!first)
                krec_kbuf_putc(b, ',');
            krec_kbuf_puts(b, cap_tab[i].name);
            first = false;
        }
    }
}

uint32_t proc_caps_snapshot_krec(char *buf, uint32_t cap)
{
    struct krec_kbuf b;

    if (cap == 0)
        return 0;

    krec_kbuf_init(&b, buf, cap);
    krec_kbuf_puts(&b, "@krec pid:int uid:int cap_mask:hex caps:str unveil:str\n");
    if (b.overflow) {
        b.len = 0;
        return krec_kbuf_finish(&b);
    }

    int count = 0;
    struct proc_snap_row *rows = snapshot_all_tasks(&count);
    if (!rows)
        return krec_kbuf_finish(&b);

    for (int i = 0; i < count; i++) {
        uint32_t mark = b.len;
        krec_kbuf_put_i64(&b, rows[i].pid);
        krec_kbuf_putc(&b, '\t');
        krec_kbuf_put_u64(&b, rows[i].uid);
        krec_kbuf_putc(&b, '\t');
        krec_kbuf_put_hex(&b, rows[i].cap_mask);
        krec_kbuf_putc(&b, '\t');
        format_caps_str(&b, rows[i].cap_mask);
        krec_kbuf_putc(&b, '\t');
        krec_kbuf_escape(&b, rows[i].unveil_prefix);
        krec_kbuf_putc(&b, '\n');
        if (b.overflow) {
            b.len = mark;
            break;
        }
    }

    kfree(rows);
    return krec_kbuf_finish(&b);
}
