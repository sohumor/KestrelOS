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

struct bootinfo *boot_info;

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
    kprintf("gdt: loaded (kernel/user segments + TSS)\n");
    idt_init();
    pic_init();
    kprintf("idt: 256 gates, PIC remapped\n");
    timer_init(TIMER_HZ);
    keyboard_init();
    serial_init_irq();
    sti();
    kprintf("irq: timer %u Hz, keyboard, serial online\n", TIMER_HZ);

    __asm__ volatile("int3");      /* exception path self-test */

    timer_sleep(10);
    kprintf("timer: ticks=%lu after 100ms sleep\n", timer_ticks());

    pmm_init(boot_info);
    kprintf("pmm: %lu/%lu pages free (%lu MiB)\n",
            pmm_free_pages(), pmm_total_pages(),
            (uint64_t)(pmm_free_pages() * PAGE_SIZE / (1024 * 1024)));

    vmm_init();
    kprintf("vmm: kernel page tables rebuilt, direct map to %lu MiB\n",
            pmm_max_phys() / (1024 * 1024));
    uint64_t kp = vmm_virt_to_phys(vmm_kernel_pml4(), KERNEL_OFFSET + 0x100000);
    if (kp != 0x100000)
        panic("vmm: kernel translation wrong: %lx", kp);

    kheap_init();
    char *a = kmalloc(64);
    char *b = kmalloc(100000);
    memset(a, 0xAA, 64);
    memset(b, 0x55, 100000);
    if ((uint8_t)a[63] != 0xAA || (uint8_t)b[99999] != 0x55)
        panic("kheap: data corruption");
    kfree(a);
    kfree(b);
    kprintf("kheap: small+large alloc/free ok\n");

    proc_init();
    kprintf("proc: scheduler online (pid %d + idle)\n", current->pid);
    extern void sched_selftest(void);
    sched_selftest();

    kprintf("\nKESTREL READY\n");
    kprintf("echo test — type on keyboard or serial:\n> ");
    for (;;) {
        int c = input_getc();
        if (c >= 0x80) {
            kprintf("[key %02x]", c);
            continue;
        }
        kprintf("%c", c);
        if (c == '\n')
            kprintf("> ");
    }
}
