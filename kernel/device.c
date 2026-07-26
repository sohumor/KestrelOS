#include "kernel.h"
#include "string.h"
#include "device.h"

/* Device model core: three registries and the matching rule between them.
 * See kernel/include/device.h for the contract and docs/drivers.md for how
 * to write a driver against it.
 *
 * Registration order is preserved (append at the tail) so that the boot
 * log, `lsdev` and the kernel monitor all show devices in the order the
 * buses found them, which is the order a human debugging a machine
 * expects.
 */

static struct bus_type *bus_list;
static struct device   *dev_list;
static struct driver   *drv_list;

static int ndevices;

/* probe() is documented not to register drivers, but it may legitimately
 * register a child device (a bridge, a multi-function card). Bound the
 * recursion so a misbehaving driver produces a complaint instead of a
 * silent stack overflow during boot. */
#define BIND_DEPTH_MAX 8
static int bind_depth;

/* ---- helpers --------------------------------------------------------- */

static void name_copy(char *dst, const char *src, int max)
{
    int i = 0;

    if (src)
        for (; i < max - 1 && src[i]; i++)
            dst[i] = src[i];
    dst[i] = '\0';
}

static bool in_dev_list(const struct device *dev)
{
    for (struct device *d = dev_list; d; d = d->next)
        if (d == dev)
            return true;
    return false;
}

static bool in_drv_list(const struct driver *drv)
{
    for (struct driver *d = drv_list; d; d = d->next)
        if (d == drv)
            return true;
    return false;
}

/* ---- buses ----------------------------------------------------------- */

struct bus_type *bus_find(const char *name)
{
    if (!name)
        return NULL;
    for (struct bus_type *b = bus_list; b; b = b->next)
        if (strcmp(b->name, name) == 0)
            return b;
    return NULL;
}

int bus_register(struct bus_type *bus)
{
    struct bus_type **tail;

    if (!bus || bus->name[0] == '\0')
        return -1;

    for (struct bus_type *b = bus_list; b; b = b->next)
        if (b == bus || strcmp(b->name, bus->name) == 0) {
            kprintf("device: bus '%s' already registered\n", bus->name);
            return -1;
        }

    bus->next = NULL;
    bus->enumerated = false;
    for (tail = &bus_list; *tail; tail = &(*tail)->next)
        ;
    *tail = bus;
    return 0;
}

int bus_enumerate_all(void)
{
    int total = 0;

    for (struct bus_type *b = bus_list; b; b = b->next) {
        if (b->enumerated || !b->enumerate)
            continue;
        b->enumerated = true;
        int n = b->enumerate();
        if (n > 0)
            total += n;
    }
    return total;
}

/* ---- binding --------------------------------------------------------- */

/* Try to bind one device to one driver. Returns 1 if the driver claimed
 * the device. dev->drv is set before probe() runs so that probe() can use
 * the device exactly as any later caller would. */
static int try_bind(struct device *dev, struct driver *drv)
{
    if (!dev || !drv || dev->drv)
        return 0;
    if (dev->bus != drv->bus || !dev->bus)
        return 0;
    if (dev->bus->match && !dev->bus->match(dev, drv))
        return 0;

    if (bind_depth >= BIND_DEPTH_MAX) {
        kprintf("device: bind recursion too deep at %s/%s\n",
                dev->name, drv->name);
        return 0;
    }

    dev->drv = drv;
    dev->drv_data = NULL;

    if (drv->probe) {
        bind_depth++;
        int rc = drv->probe(dev);
        bind_depth--;
        if (rc != 0) {
            /* Declined. Leave the device unbound so the next driver on
             * the bus gets its turn. */
            dev->drv = NULL;
            dev->drv_data = NULL;
            return 0;
        }
    }

    kprintf("device: %s bound to %s\n", dev->name, drv->name);
    return 1;
}

static void unbind(struct device *dev)
{
    struct driver *drv = dev->drv;

    if (!drv)
        return;
    /* Clear the link first: remove() may inspect the device, but nothing
     * that runs afterwards should still see it as claimed. */
    dev->drv = NULL;
    if (drv->remove)
        drv->remove(dev);
    dev->drv_data = NULL;
}

/* ---- devices --------------------------------------------------------- */

int device_register(struct device *dev)
{
    struct device **tail;

    if (!dev || !dev->bus || dev->name[0] == '\0')
        return -1;
    if (in_dev_list(dev)) {
        kprintf("device: '%s' already registered\n", dev->name);
        return -1;
    }

    dev->drv = NULL;
    dev->drv_data = NULL;
    dev->next = NULL;
    for (tail = &dev_list; *tail; tail = &(*tail)->next)
        ;
    *tail = dev;
    ndevices++;

    /* Offer the new device to every driver already on its bus. */
    for (struct driver *drv = drv_list; drv; drv = drv->next)
        if (try_bind(dev, drv))
            return 1;

    return 0;
}

void device_unregister(struct device *dev)
{
    struct device **p;

    if (!dev || !in_dev_list(dev))
        return;

    unbind(dev);

    for (p = &dev_list; *p; p = &(*p)->next)
        if (*p == dev) {
            *p = dev->next;
            ndevices--;
            break;
        }
    dev->next = NULL;
}

struct device *device_find(const char *name)
{
    if (!name)
        return NULL;
    for (struct device *d = dev_list; d; d = d->next)
        if (strcmp(d->name, name) == 0)
            return d;
    return NULL;
}

int device_count(void)
{
    return ndevices;
}

/* ---- drivers --------------------------------------------------------- */

int driver_register(struct driver *drv)
{
    struct driver **tail;
    int bound = 0;

    if (!drv || !drv->bus || drv->name[0] == '\0')
        return -1;
    if (in_drv_list(drv)) {
        kprintf("device: driver '%s' already registered\n", drv->name);
        return -1;
    }

    drv->next = NULL;
    for (tail = &drv_list; *tail; tail = &(*tail)->next)
        ;
    *tail = drv;

    /* The other half of the rule: a driver arriving after enumeration --
     * which is every driver in a loadable module -- picks up the hardware
     * that is already sitting there unbound. */
    for (struct device *dev = dev_list; dev; dev = dev->next)
        if (!dev->drv && try_bind(dev, drv))
            bound++;

    return bound;
}

void driver_unregister(struct driver *drv)
{
    struct driver **p;

    if (!drv || !in_drv_list(drv))
        return;

    for (struct device *dev = dev_list; dev; dev = dev->next)
        if (dev->drv == drv)
            unbind(dev);

    for (p = &drv_list; *p; p = &(*p)->next)
        if (*p == drv) {
            *p = drv->next;
            break;
        }
    drv->next = NULL;
}

/* ---- introspection --------------------------------------------------- */

int device_list(int index, struct device_info *out)
{
    struct device *d = dev_list;

    if (!out || index < 0)
        return -1;
    for (int i = 0; d && i < index; i++)
        d = d->next;
    if (!d)
        return -1;

    memset(out, 0, sizeof(*out));
    name_copy(out->bus, d->bus ? d->bus->name : "", BUS_NAME_MAX);
    name_copy(out->name, d->name, DEVICE_NAME_MAX);
    if (d->drv) {
        name_copy(out->driver, d->drv->name, DRIVER_NAME_MAX);
        out->bound = 1;
    }

    /* PCI identity is the one bus-private detail worth surfacing: it is
     * what a user needs in order to say "nothing claims 8086:100e". */
    if (d->bus && d->bus_data && strcmp(d->bus->name, "pci") == 0) {
        const struct pci_devinfo *pd = d->bus_data;
        out->vendor = pd->vendor;
        out->device = pd->device;
        out->class_id = ((uint32_t)pd->class_code << 8) | pd->subclass;
    }
    return 0;
}

void device_dump(void)
{
    struct device_info di;

    kprintf("device: %d device(s)\n", ndevices);
    for (int i = 0; device_list(i, &di) == 0; i++) {
        if (di.vendor)
            kprintf("  %-8s %-16s %04x:%04x %s\n", di.bus, di.name,
                    di.vendor, di.device,
                    di.bound ? di.driver : "(unbound)");
        else
            kprintf("  %-8s %-16s %11s %s\n", di.bus, di.name, "",
                    di.bound ? di.driver : "(unbound)");
    }
}
