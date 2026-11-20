#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int alive(int pid)
{
    struct k_psinfo info;
    for (int i = 0; psinfo(i, &info) == 0; i++)
        if (info.pid == pid)
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "child") == 0) {
        sleep_ms(1500);
        return 73;
    }
    if (argc > 3 && strcmp(argv[1], "stranger") == 0) {
        int pid = atoi(argv[2]);
        if (atoi(argv[3]))
            while (alive(pid))
                sleep_ms(10);
        long code = waitpid(pid);
        printf("waittest: stranger %s wait=%ld\n",
               atoi(argv[3]) ? "exited" : "live", code);
        return code == -1 && (atoi(argv[3]) || alive(pid)) ? 0 : 1;
    }

    int failed = 0;
    for (int exited = 0; exited < 2; exited++) {
        char pid_arg[16];
        char *child_args[] = { "waittest", "child", 0 };
        int child = spawn("/bin/waittest", child_args);
        if (child < 0)
            return 1;
        snprintf(pid_arg, sizeof(pid_arg), "%d", child);
        char *stranger_args[] = {
            "waittest", "stranger", pid_arg, exited ? "1" : "0", 0
        };
        int stranger = spawn("/bin/waittest", stranger_args);
        if (stranger < 0)
            return 1;
        long stranger_code = waitpid(stranger);
        long code = waitpid(child);
        printf("waittest: parent %s wait=%ld\n",
               exited ? "exited" : "live", code);
        if (stranger_code != 0 || code != 73 || waitpid(child) != -1)
            failed = 1;
    }
    if (waitpid(0) != -1 || waitpid(-1) != -1 ||
        waitpid(2147483647) != -1)
        failed = 1;
    printf("waittest: %s\n", failed ? "FAIL" : "PASS");
    return failed;
}
