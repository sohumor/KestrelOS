#include "kernel.h"
#include "module.h"

struct probe_ops {
    unsigned int (*read)(void);
};

#ifdef PROBE_PROVIDER

static unsigned int value;

static unsigned int probe_read(void)
{
    return value;
}

static const struct probe_ops ops = { .read = probe_read };

static int provider_init(void)
{
    value = 42;
    return 0;
}

MODULE_NAME("service-provider");
MODULE_INIT(provider_init);
MODULE_SERVICE(1, ops);

#else

#ifndef PROBE_VERSION
#define PROBE_VERSION 1
#endif
#ifndef PROBE_EXTRA_SIZE
#define PROBE_EXTRA_SIZE 0
#endif

static struct module_service_ref ref;

static int reader_init(void)
{
    int rc = module_service_acquire(&ref, "service-provider", PROBE_VERSION,
                                    sizeof(struct probe_ops) + PROBE_EXTRA_SIZE);
    if (rc < 0) {
        kprintf("service probe: acquire %d\n", rc);
        return rc;
    }
    const struct probe_ops *ops = ref.ops;
    unsigned int result = ops->read();
    if (result != 42) {
        module_service_release(&ref);
        return -1;
    }
    kprintf("service probe: read %u\n", result);
    return 0;
}

static void reader_exit(void)
{
    module_service_release(&ref);
}

MODULE_NAME("service-reader");
MODULE_INIT(reader_init);
MODULE_EXIT(reader_exit);

#endif
