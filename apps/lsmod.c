/* lsmod.c - list the loaded kernel modules. */

#include <kestrel.h>
#include <krec.h>
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

int main(int argc, char **argv)
{
    static const struct krec_col cols[] = {
        { "name",  KREC_STR },
        { "size",  KREC_U64 },
        { "refs",  KREC_INT },
        { "state", KREC_STR },
        { "desc",  KREC_STR },
    };
    struct k_modinfo mi;
    int rflag = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--krec") == 0) {
            rflag = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("usage: lsmod [-r]\n");
            return 0;
        } else {
            printf("usage: lsmod [-r]\n");
            return 1;
        }
    }

    if (rflag) {
        if (krec_begin(1, cols, 5) < 0)
            return 1;
        i = 0;
        while (syscall(SYS_MODLIST, i, (long)&mi, 0, 0) == 0) {
            char size_s[24], refs_s[16];
            mi.name[sizeof(mi.name) - 1] = '\0';
            mi.desc[sizeof(mi.desc) - 1] = '\0';
            snprintf(size_s, sizeof(size_s), "%u", mi.size);
            snprintf(refs_s, sizeof(refs_s), "%u", mi.refs);
            krec_row(1, 5, mi.name, size_s, refs_s,
                     state_name(mi.state), mi.desc);
            i++;
        }
        return 0;
    }

    printf("%-16s %8s %5s  %-10s %s\n",
           "NAME", "SIZE", "REFS", "STATE", "DESCRIPTION");

    i = 0;
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
