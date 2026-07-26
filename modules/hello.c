/* hello.c - the module loader's smoke test.
 *
 * Does nothing useful on purpose. What it proves is the whole binding
 * path: a PLT32 call into an exported kernel function (kprintf), 32S
 * references to its own .rodata strings, a .bss variable that the loader
 * has to zero, and R_X86_64_64 relocations inside .kmod_decl for the
 * name/description strings and the init/exit function pointers.
 *
 * Build: see docs/modules.md. Load: insmod hello
 */

#include "kernel.h"
#include "module.h"
#include "klog.h"
#include "timer.h"

/* .bss: the loader must place and zero this, and 32S-relocate the
 * references to it. */
static uint64_t loaded_at;
static int hello_calls;

static int hello_init(void)
{
    loaded_at = timer_ticks();
    hello_calls++;
    kprintf("hello: module init ran (tick %lu, call %d)\n",
            (unsigned long)loaded_at, hello_calls);
    klog_write(K_LOG_INFO, "hello", "loadable module init ran");
    return 0;
}

static void hello_exit(void)
{
    kprintf("hello: module exit ran after %lu ticks\n",
            (unsigned long)(timer_ticks() - loaded_at));
    klog_write(K_LOG_INFO, "hello", "loadable module exit ran");
}

MODULE_NAME("hello");
MODULE_DESC("loader smoke test: logs on load and unload");
MODULE_INIT(hello_init);
MODULE_EXIT(hello_exit);
