/* halt.c - power off / halt the machine via the kernel power syscall. */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

/* Provided by abi/kestrel_abi.h once the power syscall lands. */
#ifndef SYS_POWER
#define SYS_POWER 26   /* (0 = reboot, 1 = halt) */
#endif

#define POWER_HALT 1

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

int main(int argc, char **argv)
{
    argc = strip_cwd_arg(argc, argv);
    if (argc > 1) {
        printf("usage: halt\n");
        return 1;
    }

    printf("System halted. It is safe to power off.\n");
    sleep_ms(200);

    syscall(SYS_POWER, POWER_HALT, 0, 0, 0);

    /* Only reached if the kernel could not stop the machine. */
    printf("halt: failed\n");
    return 1;
}
