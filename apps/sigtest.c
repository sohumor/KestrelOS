/* sigtest - exercise handler delivery, masking, pending state and return. */

#include <kestrel.h>
#include <stdio.h>

static volatile int seen;

static void on_usr1(int sig)
{
    if (sig == SIGUSR1)
        seen++;
}

int main(void)
{
    uint64_t bit = 1ULL << (SIGUSR1 - 1);

    if (signal(SIGUSR1, on_usr1) == (sighandler_t)-1) {
        printf("sigtest: sigaction failed\n");
        return 1;
    }
    if (sigprocmask_(SIG_BLOCK, &bit, 0) < 0 ||
        kill(getpid(), SIGUSR1) < 0 || seen != 0) {
        printf("sigtest: blocked signal was delivered early\n");
        return 1;
    }
    if (sigprocmask_(SIG_UNBLOCK, &bit, 0) < 0 || seen != 1) {
        printf("sigtest: pending signal was not delivered\n");
        return 1;
    }
    if (kill(getpid(), SIGUSR1) < 0 || seen != 2) {
        printf("sigtest: direct delivery failed\n");
        return 1;
    }
    printf("sigtest: handlers + masks + sigreturn verified\n");
    return 0;
}
