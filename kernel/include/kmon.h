#pragma once

/* Kernel rescue console; runs as a kernel thread when userspace can't
 * be started. Never returns. */
void kmon_run(void *arg);
