#pragma once

/*
 * Host seam for tools/test_browser_stack.c.
 *
 * Keep the ABI structures and constants real, but redirect the handful of
 * Kestrel syscalls used by apps/browser.c to deterministic host shims.
 * Standard C/POSIX declarations come from the host headers included by the
 * harness before browser.c.
 */

/* The ABI publishes Kestrel's open flags too.  The host has already supplied
 * equivalent names through <fcntl.h>; discard those macros before importing
 * the real ABI values so -Werror remains useful for everything else. */
#undef O_RDONLY
#undef O_WRONLY
#undef O_RDWR
#undef O_CREAT
#undef O_TRUNC
#undef O_APPEND
#include "../../abi/kestrel_abi.h"

int browser_host_stat(const char *path, struct k_stat *out);
int browser_host_netinfo(struct k_netinfo *out);
long browser_host_syscall(long n, long a, long b, long c, long d);

#define stat_   browser_host_stat
#define netinfo browser_host_netinfo
#define syscall browser_host_syscall
