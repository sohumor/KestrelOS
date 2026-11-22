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

#elif defined(PROBE_MANAGED)

#ifndef PROBE_FIRST_VERSION
#define PROBE_FIRST_VERSION 1
#endif

MODULE_REQUIRE_SERVICE(provider, "service-provider", PROBE_FIRST_VERSION, struct probe_ops);
#ifdef PROBE_SECOND_VERSION
MODULE_REQUIRE_SERVICE(second, "service-provider", PROBE_SECOND_VERSION, struct probe_ops);
#endif

static int managed_init(void)
{
    const struct probe_ops *ops = module_required_service(&provider);
    kprintf("managed probe: init entered\n");
    if (!ops || ops->read() != 42)
        return -1;
    struct module_service_requirement copy = provider;
    if (module_required_service(&copy))
        return -1;
    kprintf("managed probe: init read 42\n");
#ifdef PROBE_FAIL_INIT
    return -1;
#else
    return 0;
#endif
}

static void managed_exit(void)
{
    const struct probe_ops *ops = module_required_service(&provider);
    kprintf("managed probe: exit read %u\n", ops ? ops->read() : 0);
}

MODULE_NAME("service-managed");
MODULE_INIT(managed_init);
MODULE_EXIT(managed_exit);

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
