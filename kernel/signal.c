#include "kernel.h"
#include "signal.h"
#include "proc.h"
#include "interrupts.h"
#include "uproc.h"
#include "vm.h"
#include "gdt.h"
#include "string.h"
#include "kestrel_abi.h"
#include "spinlock.h"

#define SIGFRAME_MAGIC 0x4B5349474652414DULL /* "KSIGFRAM" */
#define USER_VA_LIMIT  0x0000800000000000ULL

struct signal_frame {
    uint64_t magic;
    uint64_t old_mask;
    uint64_t signal;
    struct regs saved;
};

static spinlock_t signal_lock = SPINLOCK_INIT;

static uint64_t sig_bit(int sig)
{
    return 1ULL << (sig - 1);
}

static uint64_t unblockable_mask(void)
{
    return sig_bit(SIGKILL) | sig_bit(SIGSTOP);
}

static int signal_valid(int sig)
{
    return sig > 0 && sig < K_NSIG;
}

int signal_queue_task(struct task *t, int sig)
{
    if (!t || !signal_valid(sig) || t->state == TASK_ZOMBIE)
        return -1;

    uint64_t f = spin_lock_irqsave(&signal_lock);
    t->sig_pending |= sig_bit(sig);
    if (sig == SIGCONT) {
        t->sig_pending &= ~(sig_bit(SIGSTOP) | sig_bit(SIGTSTP) |
                            sig_bit(SIGTTIN) | sig_bit(SIGTTOU));
    }
    spin_unlock_irqrestore(&signal_lock, f);
    return 0;
}

int signal_send(struct task *t, int sig)
{
    int rc = signal_queue_task(t, sig);
    if (rc < 0)
        return rc;
    task_signal_wake(t, sig == SIGCONT);
    return 0;
}

static int default_ignored(int sig)
{
    return sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH ||
           sig == SIGCONT;
}

static int default_stops(int sig)
{
    return sig == SIGSTOP || sig == SIGTSTP ||
           sig == SIGTTIN || sig == SIGTTOU;
}

static void terminate_for_signal(struct task *t, int sig)
{
    kprintf("signal: %s (pid %d) terminated by signal %d\n",
            t->name, t->pid, sig);
    uproc_record_exit(t->pid, 128 + sig);
    task_exit(128 + sig);
}

static int next_pending(struct task *t)
{
    uint64_t ready = t->sig_pending &
                     (~t->sig_mask | unblockable_mask());
    if (!ready)
        return 0;
    for (int sig = 1; sig < K_NSIG; sig++)
        if (ready & sig_bit(sig))
            return sig;
    return 0;
}

void signal_deliver_pending(struct task *t, struct regs *r)
{
    if (!t || !t->user || !r || (r->cs & 3) != 3)
        return;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&signal_lock);
        int sig = next_pending(t);
        if (!sig) {
            spin_unlock_irqrestore(&signal_lock, flags);
            return;
        }
        t->sig_pending &= ~sig_bit(sig);

        struct k_sigaction action = t->sig_actions[sig];
        spin_unlock_irqrestore(&signal_lock, flags);
        if (sig == SIGKILL)
            action.handler = SIG_DFL;
        if (sig == SIGSTOP)
            action.handler = SIG_DFL;

        if (action.handler == SIG_IGN ||
            (action.handler == SIG_DFL && default_ignored(sig)))
            continue;

        if (action.handler == SIG_DFL) {
            if (default_stops(sig)) {
                t->state = TASK_STOPPED;
                schedule();
                continue;           /* resumes here after SIGCONT */
            }
            terminate_for_signal(t, sig);
        }

        if (action.handler >= USER_VA_LIMIT ||
            action.restorer < PAGE_SIZE ||
            action.restorer >= USER_VA_LIMIT)
            terminate_for_signal(t, SIGSEGV);

        uint64_t frame_sp =
            (r->rsp - sizeof(struct signal_frame)) & ~0xFULL;
        uint64_t return_sp = frame_sp - sizeof(uint64_t);
        if (frame_sp > r->rsp ||
            vm_fault_in_range(t, return_sp,
                              r->rsp - return_sp, 1) < 0)
            terminate_for_signal(t, SIGSEGV);

        struct signal_frame frame;
        frame.magic = SIGFRAME_MAGIC;
        frame.old_mask = t->sig_mask;
        frame.signal = (uint64_t)sig;
        frame.saved = *r;
        if (copy_to_user((void *)frame_sp, &frame, sizeof(frame)) < 0 ||
            copy_to_user((void *)return_sp, &action.restorer,
                         sizeof(action.restorer)) < 0)
            terminate_for_signal(t, SIGSEGV);

        t->sig_mask |= action.mask;
        if (!(action.flags & SA_NODEFER))
            t->sig_mask |= sig_bit(sig);
        t->sig_mask &= ~unblockable_mask();
        if (action.flags & SA_RESETHAND)
            memset(&t->sig_actions[sig], 0,
                   sizeof(t->sig_actions[sig]));

        r->rip = action.handler;
        r->rsp = return_sp;
        r->rdi = (uint64_t)sig;
        r->rax = 0;
        return;
    }
}

long signal_sys_sigaction(uint64_t usig, uint64_t uact, uint64_t uold)
{
    int sig = (int)usig;
    struct k_sigaction action;

    if (!signal_valid(sig) || sig == SIGKILL || sig == SIGSTOP)
        return -1;
    if (uold &&
        copy_to_user((void *)uold, &current->sig_actions[sig],
                     sizeof(struct k_sigaction)) < 0)
        return -1;
    if (!uact)
        return 0;
    if (copy_from_user(&action, (const void *)uact, sizeof(action)) < 0)
        return -1;
    if (action.flags & ~(SA_NODEFER | SA_RESETHAND | SA_RESTART))
        return -1;
    if (action.handler != SIG_DFL && action.handler != SIG_IGN &&
        (action.handler < PAGE_SIZE || action.handler >= USER_VA_LIMIT ||
         action.restorer < PAGE_SIZE ||
         action.restorer >= USER_VA_LIMIT))
        return -1;
    action.mask &= ~unblockable_mask();
    current->sig_actions[sig] = action;
    return 0;
}

long signal_sys_sigprocmask(uint64_t how, uint64_t uset, uint64_t uold)
{
    uint64_t set;
    if (uold && copy_to_user((void *)uold, &current->sig_mask,
                             sizeof(current->sig_mask)) < 0)
        return -1;
    if (!uset)
        return 0;
    if (copy_from_user(&set, (const void *)uset, sizeof(set)) < 0)
        return -1;
    set &= ~unblockable_mask();
    switch (how) {
    case SIG_BLOCK:
        current->sig_mask |= set;
        break;
    case SIG_UNBLOCK:
        current->sig_mask &= ~set;
        break;
    case SIG_SETMASK:
        current->sig_mask = set;
        break;
    default:
        return -1;
    }
    return 0;
}

int signal_sigreturn(struct task *t, struct regs *r)
{
    struct signal_frame frame;
    if (!t || !r ||
        copy_from_user(&frame, (const void *)r->rsp, sizeof(frame)) < 0 ||
        frame.magic != SIGFRAME_MAGIC ||
        frame.saved.cs != (SEL_UCODE | 3) ||
        frame.saved.ss != (SEL_UDATA | 3) ||
        frame.saved.rip < PAGE_SIZE ||
        frame.saved.rip >= USER_VA_LIMIT ||
        frame.saved.rsp < PAGE_SIZE ||
        frame.saved.rsp >= USER_VA_LIMIT)
        return -1;

    /* The frame lives in writable user memory. Never let it restore IOPL,
     * NT, VM or other privileged RFLAGS bits through a ring-0 iretq. */
    frame.saved.rflags &= 0x240FD5ULL;
    frame.saved.rflags |= 0x202ULL;       /* reserved bit + interrupts on */
    t->sig_mask = frame.old_mask & ~unblockable_mask();
    *r = frame.saved;
    return 0;
}

void signal_user_exception(struct task *t, struct regs *r)
{
    int sig;
    switch (r->vector) {
    case 0:  sig = SIGFPE;  break;
    case 1:
    case 3:  sig = SIGTRAP; break;
    case 6:  sig = SIGILL;  break;
    case 14: sig = SIGSEGV; break;
    default: sig = SIGBUS;  break;
    }
    signal_send(t, sig);
    signal_deliver_pending(t, r);
}
