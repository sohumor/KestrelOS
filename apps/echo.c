/* echo.c - print arguments joined by spaces. */

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    int i;

    /* Drop the shell-injected trailing "--cwd=<path>" argument. */
    if (argc > 1 && strncmp(argv[argc - 1], "--cwd=", 6) == 0)
        argc--;

    for (i = 1; i < argc; i++) {
        if (i > 1)
            putchar(' ');
        printf("%s", argv[i]);
    }
    putchar('\n');
    return 0;
}
