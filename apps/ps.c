/* ps.c - list processes. */

#include <kestrel.h>
#include <krec.h>
#include <stdio.h>
#include <string.h>

static const char *state_name(int s)
{
    switch (s) {
    case K_STATE_RUNNABLE: return "runnable";
    case K_STATE_RUNNING:  return "running";
    case K_STATE_SLEEPING: return "sleeping";
    case K_STATE_ZOMBIE:   return "zombie";
    case K_STATE_STOPPED:  return "stopped";
    }
    return "?";
}

static void usage(void)
{
    printf("usage: ps [-r]\n");
}

int main(int argc, char **argv)
{
    static const struct krec_col cols[] = {
        { "pid",   KREC_INT },
        { "ppid",  KREC_INT },
        { "uid",   KREC_INT },
        { "state", KREC_STR },
        { "name",  KREC_STR },
    };
    struct k_psinfo pi;
    int rflag = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--krec") == 0) {
            rflag = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else {
            usage();
            return 1;
        }
    }

    if (rflag) {
        if (krec_begin(1, cols, 5) < 0)
            return 1;
        for (i = 0; psinfo(i, &pi) == 0; i++) {
            char pid_s[16], ppid_s[16], uid_s[16];
            pi.name[sizeof(pi.name) - 1] = '\0';
            snprintf(pid_s, sizeof(pid_s), "%d", pi.pid);
            snprintf(ppid_s, sizeof(ppid_s), "%d", pi.ppid);
            snprintf(uid_s, sizeof(uid_s), "%u", pi.uid);
            krec_row(1, 5, pid_s, ppid_s, uid_s, state_name(pi.state), pi.name);
        }
        return 0;
    }

    printf("%5s  %-9s %s\n", "PID", "STATE", "NAME");
    i = 0;
    while (psinfo(i, &pi) == 0) {
        printf("%5d  %-9s %s\n", pi.pid, state_name(pi.state), pi.name);
        i++;
    }
    return 0;
}
