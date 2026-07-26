/* sleep.c - pause for a number of seconds. */

#include <kestrel.h>
#include <stdio.h>
#include <stdlib.h>
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

int main(int argc, char **argv)
{
    long secs;
    const char *s;

    argc = strip_cwd_arg(argc, argv);
    if (argc != 2) {
        printf("usage: sleep <seconds>\n");
        return 1;
    }

    for (s = argv[1]; *s; s++) {
        if (*s < '0' || *s > '9') {
            printf("sleep: invalid interval '%s'\n", argv[1]);
            return 1;
        }
    }

    secs = atol(argv[1]);
    if (secs < 0)
        secs = 0;
    if (secs > 86400)
        secs = 86400;

    sleep_ms((unsigned long)secs * 1000UL);
    return 0;
}
