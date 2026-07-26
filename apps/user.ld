/* KestrelOS userspace link script. Load base matches USER_LOAD_BASE
 * in abi/kestrel_abi.h. Output: static ELF64, entry _start. */

ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text : ALIGN(0x1000) {
        *(.text .text.*)
    }

    .rodata : ALIGN(0x1000) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(0x1000) {
        *(.data .data.*)
    }

    .bss : ALIGN(0x1000) {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.comment)
        *(.note*)
    }
}
