#pragma once

#include <stdint.h>
#include <stddef.h>

/* Ring-3 process support: spawning ELF64 executables as user tasks,
 * exit-code bookkeeping for waitpid, and the int 0x80 syscall layer. */

#define UPROC_MAX_ARGS 16
/* Must be >= sh.c's MAX_LINE (256) so a single token typed at the shell can
 * never be truncated on the way into a process: a shortened argv[] element
 * makes tools like calc compute a wrong answer instead of failing. */
#define UPROC_ARG_MAX  256
#define UPROC_PATH_MAX 256

/* syscall.c: install the int 0x80 dispatcher + ring-3 fault handler. */
void syscall_init(void);

/* Spawn `path` as a user process. `path` and every argv[] string must
 * already live in kernel memory; they are copied into a private package
 * before this returns, so the caller keeps ownership. Returns pid or -1. */
int uproc_spawn(const char *path, char *const argv[], int argc);

/* Same, but `upath`/`uargv` are userspace pointers from the current
 * process (SYS_SPAWN). Performs bounded copy-in (UPROC_MAX_ARGS args of
 * UPROC_ARG_MAX bytes, path UPROC_PATH_MAX) then calls uproc_spawn. */
int uproc_spawn_from_user(const char *upath, char *const *uargv);

/* Exit-code ring for waitpid; record is called right before task_exit. */
void uproc_record_exit(int pid, long code);
long uproc_waitpid(int pid);   /* blocks; -1 if pid unknown/forgotten */

/* Bounded user-memory copies through current->pml4 (defined in
 * syscall.c). All return -1 on a bad/unmapped user pointer, never
 * panic. copy_str_from_user returns the string length (excluding NUL)
 * or -1 if unterminated within `max`. */
int  user_range_ok(const void *uptr, size_t len);
int  copy_from_user(void *dst, const void *usrc, size_t len);
int  copy_to_user(void *udst, const void *src, size_t len);
long copy_str_from_user(char *dst, const void *usrc, size_t max);
