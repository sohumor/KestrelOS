/* lsdev.c - list the kernel device tree: what each bus found, and which
 * driver (if any) claimed it.
 *
 * usage: lsdev [-v] [BUS]
 *   -v    also print the PCI class and the raw vendor:device ids for
 *         every device, not just the ones that have them
 *   BUS   show only devices on that bus ("pci", "platform")
 *
 * A device with "-" in the DRIVER column was enumerated but nothing
 * claimed it. That is the normal state for hardware whose driver has not
 * been converted to the device model yet, and for hardware whose driver
 * lives in a module that is not loaded.
 */

#include <kestrel.h>
#include <stdio.h>
#include <string.h>

/* The kernel walks its device list one entry at a time; there is no
 * "give me everything" call, for the same reason SYS_PSINFO works this
 * way: the list can change between calls and a snapshot of the whole
 * thing would need a bound on its size that nothing can honour. */
static int devinfo_at(int index, struct k_devinfo *out)
{
    return (int)syscall(SYS_DEVLIST, index, (long)out, 0, 0);
}

/* PCI base class codes worth naming; anything else prints as hex. */
static const char *class_name(unsigned class_id)
{
    switch (class_id >> 8) {
    case 0x00: return "legacy";
    case 0x01: return "storage";
    case 0x02: return "network";
    case 0x03: return "display";
    case 0x04: return "multimedia";
    case 0x05: return "memory";
    case 0x06: return "bridge";
    case 0x07: return "comm";
    case 0x08: return "system";
    case 0x09: return "input";
    case 0x0C: return "serialbus";
    case 0x0D: return "wireless";
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct k_devinfo di;
    const char *want_bus = 0;
    int verbose = 0;
    int i, total = 0, shown = 0, bound = 0;

    for (i = 1; i < argc; i++) {
        /* The shell appends "--cwd=<path>" to every command. */
        if (strncmp(argv[i], "--cwd=", 6) == 0)
            continue;
        if (strcmp(argv[i], "-v") == 0)
            verbose = 1;
        else if (argv[i][0] == '-') {
            printf("usage: lsdev [-v] [BUS]\n");
            return 1;
        } else if (!want_bus)
            want_bus = argv[i];
    }

    printf("%-9s %-14s %-10s %s\n", "BUS", "DEVICE", "DRIVER", "ID");

    for (i = 0; devinfo_at(i, &di) == 0; i++) {
        total++;
        if (di.bound)
            bound++;
        if (want_bus && strcmp(want_bus, di.bus) != 0)
            continue;
        shown++;

        int has_id = (di.vendor || di.device);
        const char *drv = di.bound ? di.driver : "-";

        /* Only pad the driver column when a column follows it, so rows
         * with nothing to say about ids do not trail whitespace. */
        if (has_id || verbose)
            printf("%-9s %-14s %-10s", di.bus, di.name, drv);
        else
            printf("%-9s %-14s %s", di.bus, di.name, drv);

        if (has_id) {
            const char *cn = class_name(di.class_id);
            printf(" %04x:%04x", di.vendor, di.device);
            if (cn)
                printf(" %s", cn);
            else if (di.class_id)
                printf(" class %04x", di.class_id);
        } else if (verbose) {
            printf(" -");
        }
        printf("\n");
    }

    if (want_bus)
        printf("%d of %d device%s on bus \"%s\", %d bound\n",
               shown, total, total == 1 ? "" : "s", want_bus, bound);
    else
        printf("%d device%s, %d bound, %d unbound\n",
               total, total == 1 ? "" : "s", bound, total - bound);
    return 0;
}
