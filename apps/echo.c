/* echo.c - print arguments joined by spaces. */

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (i > 1)
            putchar(' ');
        printf("%s", argv[i]);
    }
    putchar('\n');
    return 0;
}
