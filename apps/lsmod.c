/* lsmod.c - list the loaded kernel modules. */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

/* Falls away once abi/kestrel_abi.h carries the module calls. */
#ifndef SYS_MODLIST
#define SYS_MODLIST 55

struct k_modinfo {
    char name[32];
    char desc[64];
    uint32_t size;
    uint32_t refs;
    uint32_t state;
};

#define K_MOD_LOADING   0
#define K_MOD_LIVE      1
#define K_MOD_UNLOADING 2
#endif

static const char *state_name(uint32_t state)
{
    switch (state) {
    case K_MOD_LOADING:   return "loading";
    case K_MOD_LIVE:      return "live";
    case K_MOD_UNLOADING: return "unloading";
    default:              return "?";
    }
}

int main(void)
{
    struct k_modinfo mi;
    int i = 0;

    printf("%-16s %8s %5s  %-10s %s\n",
           "NAME", "SIZE", "REFS", "STATE", "DESCRIPTION");

    while (syscall(SYS_MODLIST, i, (long)&mi, 0, 0) == 0) {
        mi.name[sizeof(mi.name) - 1] = '\0';
        mi.desc[sizeof(mi.desc) - 1] = '\0';
        printf("%-16s %8u %5u  %-10s %s\n", mi.name, mi.size, mi.refs,
               state_name(mi.state), mi.desc);
        i++;
    }

    if (i == 0)
        printf("(no modules loaded)\n");
    return 0;
}
