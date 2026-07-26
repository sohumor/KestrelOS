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
