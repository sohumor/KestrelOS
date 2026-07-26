/* uptime.c - print time since boot as h:mm:ss. */

#include <kestrel.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    unsigned long ms, s, h, m;

    (void)argc;
    (void)argv;

    ms = uptime_ms();
    s = ms / 1000;
    h = s / 3600;
    m = (s / 60) % 60;
    printf("up %lu:%02lu:%02lu\n", h, m, s % 60);
    return 0;
}
