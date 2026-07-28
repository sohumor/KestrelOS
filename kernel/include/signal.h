#pragma once

#include <stdint.h>

struct task;
struct regs;

/* Queue one traditional signal. Signals coalesce by number. */
int signal_send(struct task *task, int sig);
/* Queue only the signal bits. The caller must keep the task alive and is
 * responsible for waking it; used by the scheduler's PID-safe path. */
int signal_queue_task(struct task *task, int sig);

/* Called only at safe ring-3 return points. It may rewrite the interrupt
 * frame, stop the task, or terminate it for a default action. */
void signal_deliver_pending(struct task *task, struct regs *regs);

long signal_sys_sigaction(uint64_t sig, uint64_t uact, uint64_t uold);
long signal_sys_sigprocmask(uint64_t how, uint64_t uset, uint64_t uold);
int signal_sigreturn(struct task *task, struct regs *regs);

/* Convert a CPU exception into its conventional process signal. */
void signal_user_exception(struct task *task, struct regs *regs);
