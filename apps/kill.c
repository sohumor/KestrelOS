/* kill.c - terminate a process by pid.
 *
 *   kill <pid>...
 *
 * SYS_KILL only reports success or failure, so the reason for a failure is
 * worked out from SYS_PSINFO: a pid that is not in the table never existed,
 * one owned by another user is a permission problem, and pid 1 is refused
 * by the kernel outright.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

/* Pull the shell-injected "--cwd=<path>" argument, wherever it sits. */
static int strip_cwd_arg(int argc, char **argv)
{
    int i, out = 1;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--cwd=", 6) != 0)
            argv[out++] = argv[i];
    }
    return out;
}

/* Strict non-negative decimal parse. Returns -1 on any junk. */
static int parse_pid(const char *s)
{
    long v = 0;
    int i;

    if (!s || !s[0])
        return -1;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
        if (v > 0x7fffffffL)
            return -1;
    }
    return (int)v;
}

/* Fill *out with the process table entry for pid. Returns 0 when found. */
static int find_proc(int pid, struct k_psinfo *out)
{
    struct k_psinfo pi;
    int i;

    for (i = 0; psinfo(i, &pi) == 0; i++) {
        if (pi.pid == pid) {
            *out = pi;
            return 0;
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    struct k_psinfo pi;
    unsigned int me;
    int i, pid, rc = 0;

    argc = strip_cwd_arg(argc, argv);
    if (argc < 2) {
        printf("usage: kill <pid>...\n");
        return 1;
    }
    me = (unsigned int)syscall(SYS_GETUID, 0, 0, 0, 0);

    for (i = 1; i < argc; i++) {
        pid = parse_pid(argv[i]);
        if (pid < 0) {
            printf("kill: %s: not a valid pid\n", argv[i]);
            rc = 1;
            continue;
        }
        if (pid == 0) {
            printf("kill: 0: not a valid pid\n");
            rc = 1;
            continue;
        }
        if (pid == 1) {
            printf("kill: 1: refusing to kill init\n");
            rc = 1;
            continue;
        }
        if (pid == getpid()) {
            printf("kill: %d: refusing to kill myself\n", pid);
            rc = 1;
            continue;
        }
        if (find_proc(pid, &pi) < 0) {
            printf("kill: %d: no such process\n", pid);
            rc = 1;
            continue;
        }
        if (me != 0 && me != pi.uid) {
            printf("kill: %d (%s): operation not permitted, it belongs to "
                   "uid %u\n", pid, pi.name, pi.uid);
            rc = 1;
            continue;
        }
        if (syscall(SYS_KILL, pid, SIGTERM, 0, 0) < 0) {
            printf("kill: %d (%s): the kernel refused the request\n", pid,
                   pi.name);
            rc = 1;
            continue;
        }
        printf("kill: %d (%s) terminated\n", pid, pi.name);
    }
    return rc;
}
