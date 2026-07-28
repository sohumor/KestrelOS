/* free.c - report physical memory usage. */

#include <kestrel.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    uint64_t total_kb = 0, free_kb = 0, used_kb;
    uint64_t swap_total_kb = 0, swap_used_kb = 0;

    (void)argc;
    (void)argv;

    if (meminfo(&total_kb, &free_kb) != 0) {
        printf("free: meminfo failed\n");
        return 1;
    }
    used_kb = total_kb - free_kb;

    printf("%-7s %10s %10s\n", "", "KiB", "MiB");
    printf("%-7s %10llu %10llu\n", "total:",
           (unsigned long long)total_kb,
           (unsigned long long)(total_kb / 1024));
    printf("%-7s %10llu %10llu\n", "used:",
           (unsigned long long)used_kb,
           (unsigned long long)(used_kb / 1024));
    printf("%-7s %10llu %10llu\n", "free:",
           (unsigned long long)free_kb,
           (unsigned long long)(free_kb / 1024));
    if (swapinfo(&swap_total_kb, &swap_used_kb) == 0) {
        printf("%-7s %10llu %10llu\n", "swap:",
               (unsigned long long)swap_total_kb,
               (unsigned long long)(swap_total_kb / 1024));
        printf("%-7s %10llu %10llu\n", "swapuse:",
               (unsigned long long)swap_used_kb,
               (unsigned long long)(swap_used_kb / 1024));
    }
    return 0;
}
