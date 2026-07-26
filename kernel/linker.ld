/* KestrelOS kernel linker script.
 * Linked at the higher half (0xFFFFFFFF80100000), loaded at phys 1 MiB.
 * objcopy -O binary uses the LMA (VMA - 0xFFFFFFFF80000000). */

ENTRY(_start)

KERNEL_OFFSET = 0xFFFFFFFF80000000;

SECTIONS
{
    . = KERNEL_OFFSET + 0x100000;

    .text : AT(ADDR(.text) - KERNEL_OFFSET)
    {
        *(.text.entry)
        *(.text*)
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_OFFSET)
    {
        *(.rodata*)
    }

    /* Exported symbol table (kernel/include/export.h). Read-only, but kept
     * in its own output section so the loader can walk it: *(.rodata*)
     * would swallow it and lose the bounds. KEEP() because nothing in the
     * kernel references these records — the module loader finds them by
     * address, not by name. */
    .ksyms : AT(ADDR(.ksyms) - KERNEL_OFFSET)
    {
        . = ALIGN(16);
        __ksyms_start = .;
        KEEP(*(.ksyms))
        __ksyms_end = .;
    }

    /* Initcall levels (kernel/include/initcall.h). Each level's function
     * pointers are collected between a start/end pair so kmain() can run
     * a level instead of naming every subsystem in it. Order within a
     * level is link order and is deliberately not depended on. */
    .initcall : AT(ADDR(.initcall) - KERNEL_OFFSET)
    {
        . = ALIGN(8);
        __initcall_early_start = .;
        KEEP(*(.initcall.early.init))
        __initcall_early_end = .;

        __initcall_core_start = .;
        KEEP(*(.initcall.core.init))
        __initcall_core_end = .;

        __initcall_driver_start = .;
        KEEP(*(.initcall.driver.init))
        __initcall_driver_end = .;

        __initcall_late_start = .;
        KEEP(*(.initcall.late.init))
        __initcall_late_end = .;
    }

    .data : AT(ADDR(.data) - KERNEL_OFFSET)
    {
        *(.data*)
    }

    .bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_OFFSET)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss*)
        . = ALIGN(8);
        __bss_end = .;
    }

    __kernel_end = .;

    /DISCARD/ :
    {
        *(.eh_frame*)
        *(.comment)
        *(.note*)
    }
}
