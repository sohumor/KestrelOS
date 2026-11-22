/* sigtest - exercise handler delivery, masking, pending state and return. */

#include <kestrel.h>
#include <kestrel_cwd.h>
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

static int grant_descendant(void)
{
    struct k_stat st;
    if (mkdir_("/tmp/sigtest-spawn-grant") == 0) {
        unlink_("/tmp/sigtest-spawn-grant");
        return 1;
    }
    return stat_("/bin/sigtest", &st) < 0 ||
           stat_("/dev/zero", &st) == 0;
}

static int grant_child(void)
{
    char byte = 'x';
    struct k_mountinfo info;
    if (grant_descendant() || strcmp(kestrel_startup_cwd(), "/doc") != 0 ||
        read(7, &byte, 1) != 1 || byte != 0 || read(8, &byte, 1) != -1 ||
        syscall(SYS_MOUNTLIST, 0, (long)&info, 0, 0) < 0 ||
        strcmp(info.path, "/") != 0 ||
        syscall(SYS_MOUNTLIST, 1, (long)&info, 0, 0) != -1)
        return 1;

    char *args[] = { "sigtest", "--grant-descendant", 0 };
    struct k_spawn_grant wider = {
        .version = K_SPAWN_GRANT_VERSION,
        .flags = K_SPAWN_GRANT_MOUNT_VIEW,
        .n_mounts = 2,
        .caps.cap_mask = KCAP_ALL,
        .mounts = { "/", "/dev" },
    };
    if (spawn_grant("/bin/sigtest", args, &wider) != -1)
        return 1;
    wider.flags = 0;
    wider.n_mounts = 0;
    int child = spawn_grant("/bin/sigtest", args, &wider);
    return child < 0 || waitpid(child) != 0;
}

static int grant_zero(void)
{
    struct k_stat st;
    char *args[] = { "sigtest", "--grant-descendant", 0 };
    return stat_("/bin/sigtest", &st) != -1 ||
           spawn("/bin/sigtest", args) != -1;
}

static int test_spawn_grant(void)
{
    if (mkdir_("/tmp/sigtest-spawn-grant") != 0 ||
        unlink_("/tmp/sigtest-spawn-grant") != 0)
        return 1;
    int fd = open("/dev/zero", O_RDONLY);
    if (fd < 0 || dup2_(fd, 8) != 8)
        return 1;

    char *args[] = { "sigtest", "--grant-child", 0 };
    struct k_spawn_grant grant = {
        .version = K_SPAWN_GRANT_VERSION,
        .flags = K_SPAWN_GRANT_MOUNT_VIEW,
        .n_mounts = 1,
        .caps = {
            .n_fds = 1,
            .cap_mask = KCAP_FS_READ | KCAP_SPAWN,
            .cwd = "/doc",
            .fds = { { .child_fd = 7, .parent_fd = fd } },
        },
        .mounts = { "/" },
    };
    grant.caps.fds[1] = grant.caps.fds[0];
    grant.caps.n_fds = 2;
    if (spawn_grant("/bin/sigtest", args, &grant) != -1)
        return 1;
    grant.caps.n_fds = 1;
    grant.version++;
    if (spawn_grant("/bin/sigtest", args, &grant) != -1)
        return 1;
    grant.version = K_SPAWN_GRANT_VERSION;
    if (syscall(SYS_SPAWN_GRANT, (long)"/bin/sigtest", (long)args,
                (long)&grant, sizeof(grant) - 1) != -1)
        return 1;

    int child = spawn_grant("/bin/sigtest", args, &grant);
    close(8);
    close(fd);
    if (child < 0 || waitpid(child) != 0)
        return 1;

    args[1] = "--grant-zero";
    grant.caps.n_fds = 0;
    grant.caps.cap_mask = 0;
    child = spawn_grant("/bin/sigtest", args, &grant);
    if (child < 0 || waitpid(child) != 0)
        return 1;

    grant.n_mounts = 0;
    child = spawn_grant("/bin/sigtest", args, &grant);
    if (child < 0 || waitpid(child) != -1)
        return 1;

    printf("sigtest: mount view, exact capabilities, cwd and descriptor grants verified\n");
    return 0;
}

enum { WAITANY_CHILD_STATUS = 37 };

static int test_waitany_bad_pointer(void)
{
    char *args[] = { "sigtest", "--waitany-child", 0 };
    int child = spawn("/bin/sigtest", args);
    if (child < 0) {
        printf("sigtest: waitany child spawn failed\n");
        return 1;
    }

    int bad = waitany((int *)1);
    int pid = 0;
    int status = waitany(&pid);
    if (bad != -1 || pid != child || status != WAITANY_CHILD_STATUS) {
        printf("sigtest: waitany failed: bad=%d retry=(%d, %d) expected=(%d, %d)\n",
               bad, pid, status, child, WAITANY_CHILD_STATUS);
        return 1;
    }
    printf("sigtest: waitany preserved child PID and status after bad pointer\n");
    return 0;
}

static int test_cwd_consumers(void)
{
    static const char expected[] = "--cwd=/literal\n";
    char *sh_args[17] = { "sh", "-c",
                          "cat welcome.md >/dev/null && echo --cwd=/literal" };
    char *cat_args[17] = { "cat" };
    struct k_spawn_caps caps = { .cwd = "/doc" };
    int output[2];
    char data[sizeof(expected)];
    int sink = open("/dev/null", O_WRONLY);

    if (sink < 0 || pipe_(output) != 0)
        return 1;
    caps.fds[0].child_fd = 1;
    caps.fds[0].parent_fd = sink;
    caps.n_fds = 1;
    for (int i = 1; i < 16; i++)
        cat_args[i] = "welcome.md";
    for (int i = 3; i < 16; i++)
        sh_args[i] = "unused";
    int cat = spawn_caps("/bin/cat", cat_args, &caps);
    caps.fds[0].parent_fd = output[1];
    int sh = spawn_caps("/bin/sh", sh_args, &caps);
    close(sink);
    close(output[1]);
    int failed = cat < 0 || sh < 0 || waitpid(cat) != 0 || waitpid(sh) != 0 ||
                 read(output[0], data, sizeof(data)) != sizeof(expected) - 1 ||
                 memcmp(data, expected, sizeof(expected) - 1) != 0;
    close(output[0]);
    return failed;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--startup-cwd") == 0)
        return strcmp(kestrel_startup_cwd(), "/doc") != 0 ||
               !(argc == 2 ||
                 (argc == 16 && strcmp(argv[15], "unused") == 0));
    if (argc > 1 && strcmp(argv[1], "--pipe-read") == 0)
        return test_pipe_read();
    if (argc > 2 && strcmp(argv[1], "--interrupt-reader") == 0)
        return interrupt_reader(atoi(argv[2]));
    if (argc > 1 && strcmp(argv[1], "--pipe-lifetime") == 0)
        return test_pipe_lifetime();
    if (argc > 1 && strcmp(argv[1], "--spawn-grant") == 0)
        return test_spawn_grant();
    if (argc > 1 && strcmp(argv[1], "--grant-child") == 0)
        return grant_child();
    if (argc > 1 && strcmp(argv[1], "--grant-descendant") == 0)
        return grant_descendant();
    if (argc > 1 && strcmp(argv[1], "--grant-zero") == 0)
        return grant_zero();
    if (argc > 1 && strcmp(argv[1], "--waitany-bad-pointer") == 0)
        return test_waitany_bad_pointer();
    if (argc > 1 && strcmp(argv[1], "--waitany-child") == 0)
        return WAITANY_CHILD_STATUS;

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
    struct k_spawn_caps caps = { .cwd = "/doc" };
    char *args[] = { "sigtest", "--startup-cwd", 0 };
    int child = spawn_caps("/bin/sigtest", args, &caps);
    if (child < 0 || waitpid(child) != 0) {
        printf("sigtest: cwd metadata or legacy argument missing\n");
        return 1;
    }
    char *full_args[17] = { "sigtest", "--startup-cwd" };
    for (int i = 2; i < 16; i++)
        full_args[i] = "unused";
    child = spawn_caps("/bin/sigtest", full_args, &caps);
    if (child < 0 || waitpid(child) != 0) {
        printf("sigtest: cwd metadata missing with a full argument list\n");
        return 1;
    }
    if (test_cwd_consumers() != 0) {
        printf("sigtest: shell or cat did not use cwd metadata\n");
        return 1;
    }
    printf("sigtest: handlers + masks + sigreturn verified\n");
    return 0;
}
