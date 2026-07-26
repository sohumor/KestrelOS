#include "kernel.h"
#include "bootinfo.h"
#include "console.h"
#include "serial.h"
#include "string.h"

struct bootinfo *boot_info;

static const char *e820_type_name(uint32_t t)
{
    switch (t) {
    case E820_USABLE:   return "usable";
    case E820_RESERVED: return "reserved";
    case E820_ACPI:     return "ACPI reclaim";
    case E820_NVS:      return "ACPI NVS";
    case E820_BAD:      return "bad";
    default:            return "unknown";
    }
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

    kprintf("BIOS boot drive: 0x%02x\n", boot_info->boot_drive);
    kprintf("E820 memory map (%u entries):\n", boot_info->e820_count);

    uint64_t usable = 0;
    for (int i = 0; i < boot_info->e820_count; i++) {
        struct e820_entry *e = &boot_info->e820[i];
        kprintf("  %016lx - %016lx  %s\n",
                e->base, e->base + e->len - 1, e820_type_name(e->type));
        if (e->type == E820_USABLE)
            usable += e->len;
    }
    kprintf("usable RAM: %lu MiB\n\n", usable / (1024 * 1024));

    kprintf("kestrel: nothing more to do yet, halting.\n");
    hang();
}
