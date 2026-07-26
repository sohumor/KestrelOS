#include "kernel.h"
#include "bootinfo.h"
#include "console.h"
#include "serial.h"
#include "string.h"
#include "gdt.h"
#include "interrupts.h"
#include "timer.h"
#include "keyboard.h"
#include "input.h"
#include "pmm.h"
#include "vmm.h"
#include "kheap.h"
#include "proc.h"
#include "ata.h"
#include "vfs.h"
#include "net.h"
#include "uproc.h"
#include "fpu.h"
#include "rtc.h"
#include "kmon.h"

struct bootinfo *boot_info;

/* Start userspace. uproc_spawn only creates the task — the ELF load
 * happens on that task, so a missing or corrupt /bin/init shows up as an
 * immediate exit rather than a spawn failure. Either way we land in the
 * kernel rescue console instead of a dead machine. */
static void init_launcher(void *arg)
{
    (void)arg;
    char *argv[] = { "/bin/init", NULL };
    struct k_stat st;
    int pid;

    if (vfs_stat("/bin/init", &st) < 0 || st.is_dir) {
        kprintf("init: /bin/init is missing\n");
        kmon_run(NULL);
    }

    pid = uproc_spawn("/bin/init", argv, 1);
    if (pid < 0) {
        kprintf("init: cannot spawn /bin/init\n");
        kmon_run(NULL);
    }

    long code = uproc_waitpid(pid);
    kprintf("init: /bin/init exited (%ld)\n", code);
    kmon_run(NULL);
}

void kmain(uint64_t bootinfo_phys)
{
    serial_init();
    console_init();

    console_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("\n  KestrelOS %s (x86-64)\n", KERNEL_VERSION);
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("  from-scratch kernel booted in long mode\n\n");

    boot_info = P2V(bootinfo_phys);

    uint64_t usable = 0;
    for (int i = 0; i < boot_info->e820_count; i++)
        if (boot_info->e820[i].type == E820_USABLE)
            usable += boot_info->e820[i].len;
    kprintf("mem: %lu MiB usable (%u E820 entries)\n",
            usable / (1024 * 1024), boot_info->e820_count);

    gdt_init();
    idt_init();
    pic_init();
    timer_init(TIMER_HZ);
    keyboard_init();
    serial_init_irq();
    sti();
    kprintf("irq: gdt/idt/pic up, timer %u Hz, keyboard + serial input\n",
            TIMER_HZ);

    fpu_init();
    pmm_init(boot_info);
    vmm_init();
    kheap_init();
    kprintf("mem: pmm %lu pages free, paging rebuilt, heap ready\n",
            pmm_free_pages());

    proc_init();
    kprintf("proc: scheduler online\n");

    ata_init();
    if (vfs_init() == 0)
        kprintf("vfs: root filesystem mounted\n");
    else
        kprintf("vfs: WARNING: no root filesystem\n");

    net_init();
    syscall_init();

    char when[40];
    if (rtc_format(when, sizeof(when)) == 0)
        kprintf("rtc: %s\n", when);

    kprintf("\nKESTREL READY\n\n");

    kthread_create(init_launcher, NULL, "launcher");

    /* Boot task parks; all life now happens via IRQs and the scheduler. */
    for (;;) {
        __asm__ volatile("sti; hlt");
        yield();
    }
}
