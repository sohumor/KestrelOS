/* sigtest - exercise handler delivery, masking, pending state and return. */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int seen;

static void on_usr1(int sig)
{
    if (sig == SIGUSR1)
        seen++;
}

static int interrupt_reader(int pid)
{
    for (int retry = 0; retry < 100; retry++) {
        struct k_psinfo info;
        for (int i = 0; psinfo(i, &info) == 0; i++) {
            if (info.pid == pid && info.state == K_STATE_SLEEPING)
                return kill(pid, SIGUSR1) < 0;
        }
        sleep_ms(10);
    }
    return 1;
}

static int test_pipe_read(void)
{
    enum { PIPE_CHUNK = 4096 };
    static char data[PIPE_CHUNK + 1];
    int fds[2];
    char pid_arg[16];
    char *args[] = { "sigtest", "--interrupt-reader", pid_arg, 0 };

    if (pipe_(fds) < 0 || read_nb(fds[0], data, 1) != 0 ||
        read(fds[1], data, 1) != -1 ||
        write(fds[1], "x", 1) != 1 || read_nb(fds[0], data, 2) != 1 ||
        data[0] != 'x') {
        printf("sigtest: nonblocking pipe read failed\n");
        return 1;
    }
    if (write(fds[1], data, PIPE_CHUNK) != PIPE_CHUNK ||
        read(fds[0], data, sizeof(data)) != PIPE_CHUNK) {
        printf("sigtest: full-chunk pipe read failed\n");
        return 1;
    }

    if (signal(SIGUSR1, on_usr1) == (sighandler_t)-1)
        return 1;
    snprintf(pid_arg, sizeof(pid_arg), "%d", getpid());
    int child = spawn("/bin/sigtest", args);
    if (child < 0 || read(fds[0], data, 1) != -1 || seen != 1 ||
        waitpid(child) != 0 || close(fds[1]) != 0 ||
        read(fds[0], data, 1) != 0) {
        printf("sigtest: pipe read interruption or EOF failed\n");
        return 1;
    }
    close(fds[0]);
    printf("sigtest: pipe read modes + interruption + EOF verified\n");
    return 0;
}

static int test_pipe_lifetime(void)
{
    int fds[2];
    char data[16];
    char *args[] = { "echo", "child", 0 };
    struct k_spawn_caps caps = {0};

    if (pipe_(fds) < 0)
        return 1;
    int alias = open("/dev/null", O_WRONLY);
    if (alias < 0 || dup2_(fds[1], alias) != alias || close(fds[1]) != 0 ||
        write(alias, "x", 1) != 1 || read_nb(fds[0], data, sizeof(data)) != 1 ||
        data[0] != 'x') {
        printf("sigtest: duplicated pipe end did not survive close\n");
        return 1;
    }

    caps.fds[0].child_fd = 1;
    caps.fds[0].parent_fd = alias;
    caps.n_fds = 1;
    caps.flags = K_SPAWN_CLOSE_OTHER;
    int child = spawn_caps("/bin/echo", args, &caps);
    if (child < 0 || close(alias) != 0 || waitpid(child) != 0 ||
        read(fds[0], data, sizeof(data)) != 6 ||
        memcmp(data, "child\n", 6) != 0 ||
        read(fds[0], data, sizeof(data)) != 0 || close(fds[0]) != 0) {
        printf("sigtest: spawned pipe end or final EOF failed\n");
        return 1;
    }

    if (pipe_(fds) < 0 || close(fds[0]) != 0 ||
        write(fds[1], "x", 1) != -1 || close(fds[1]) != 0) {
        printf("sigtest: closed pipe reader was not detected\n");
        return 1;
    }
    printf("sigtest: pipe dup + spawn + peer close verified\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--pipe-read") == 0)
        return test_pipe_read();
    if (argc > 2 && strcmp(argv[1], "--interrupt-reader") == 0)
        return interrupt_reader(atoi(argv[2]));
    if (argc > 1 && strcmp(argv[1], "--pipe-lifetime") == 0)
        return test_pipe_lifetime();

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
