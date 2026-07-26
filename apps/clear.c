/* clear.c - clear the screen. */

#include <kestrel.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    term_clear();
    return 0;
}
