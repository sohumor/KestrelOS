/* ps.c - list processes. */

#include <kestrel.h>
#include <stdio.h>

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

int main(int argc, char **argv)
{
    struct k_psinfo pi;
    int i = 0;

    (void)argc;
    (void)argv;

    printf("%5s  %-9s %s\n", "PID", "STATE", "NAME");
    while (psinfo(i, &pi) == 0) {
        printf("%5d  %-9s %s\n", pi.pid, state_name(pi.state), pi.name);
        i++;
    }
    return 0;
}
