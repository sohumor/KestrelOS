#include "kernel.h"
#include "string.h"
#include "device.h"
#include "initcall.h"

/* The platform bus: hardware that cannot be discovered, only assumed.
 *
 * An x86 PC has a PIT at 0x40, an 8042 at 0x60, a UART at 0x3F8 and a
 * CMOS/RTC at 0x70 because the architecture says so, not because anything
 * enumerated them. There is nothing to scan, so "enumeration" is a table
 * -- but putting that table behind the same bus interface as PCI means
 * these devices show up in `lsdev`, can be claimed by a driver in a
 * module, and stop being special cases in kmain().
 *
 * Matching is by name: a platform driver's match_table is a NULL-
 * terminated array of device names, or NULL to match the driver's own
 * name. A name is the honest key here -- there is no vendor id to consult.
 */

static struct bus_type platform_bus;

struct platform_dev {
    struct device dev;
    struct platform_devinfo info;
};

/* The fixed devices, in the order they appear in `lsdev`. Registration
 * order does not imply probe order: drivers bind when they register, at
 * INITCALL_DRIVER. */
static struct platform_dev platform_devs[] = {
    { .dev = { .name = "pit"      }, .info = { 0x0040, 4, 0  } },
    { .dev = { .name = "ps2kbd"   }, .info = { 0x0060, 5, 1  } },
    { .dev = { .name = "ps2mouse" }, .info = { 0x0060, 5, 12 } },
    { .dev = { .name = "serial0"  }, .info = { 0x03F8, 8, 4  } },
    { .dev = { .name = "cmos"     }, .info = { 0x0070, 2, 8  } },
};

#define PLATFORM_NDEVS \
    ((int)(sizeof(platform_devs) / sizeof(platform_devs[0])))

/* ---- matching -------------------------------------------------------- */

static int platform_bus_match(struct device *dev, struct driver *drv)
{
    const char *const *names = drv->match_table;

    if (!names)
        return strcmp(dev->name, drv->name) == 0;

    for (; *names; names++)
        if (strcmp(dev->name, *names) == 0)
            return 1;
    return 0;
}

/* ---- enumeration ----------------------------------------------------- */

static int platform_bus_enumerate(void)
{
    int n = 0;

    for (int i = 0; i < PLATFORM_NDEVS; i++) {
        struct platform_dev *p = &platform_devs[i];

        p->dev.bus = &platform_bus;
        p->dev.bus_data = &p->info;
        if (device_register(&p->dev) >= 0)
            n++;
    }

    kprintf("bus_platform: %d fixed device(s)\n", n);
    return n;
}

static struct bus_type platform_bus = {
    .name = "platform",
    .match = platform_bus_match,
    .enumerate = platform_bus_enumerate,
};

/* ---- registering a platform device at runtime ------------------------ */

/* For a module that owns hardware the table above does not know about.
 * The caller owns both structures for as long as the device is
 * registered. Returns what device_register() returns. */
int platform_device_add(struct device *dev, struct platform_devinfo *info)
{
    if (!dev)
        return -1;
    dev->bus = &platform_bus;
    dev->bus_data = info;
    return device_register(dev);
}

/* ---- init ------------------------------------------------------------ */

static int bus_platform_init(void)
{
    if (bus_register(&platform_bus) < 0)
        return -1;
    platform_bus.enumerated = true;
    platform_bus_enumerate();
    return 0;
}
initcall(early, bus_platform_init);
