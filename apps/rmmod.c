/* rmmod.c - unload a kernel module by name.
 *
 *   rmmod hello
 *
 * Fails if the module is still referenced; the kernel runs its exit
 * function before freeing it.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

#ifndef SYS_RMMOD
#define SYS_RMMOD 54
#endif

int main(int argc, char **argv)
{
    /* Drop the shell-injected trailing "--cwd=<path>" argument. */
    if (argc > 1 && strncmp(argv[argc - 1], "--cwd=", 6) == 0)
        argc--;

    if (argc != 2) {
        printf("usage: rmmod <name>\n");
        return 1;
    }

    if (syscall(SYS_RMMOD, (long)argv[1], 0, 0, 0) != 0) {
        printf("rmmod: %s: not loaded, or still in use\n", argv[1]);
        return 1;
    }
    return 0;
}
