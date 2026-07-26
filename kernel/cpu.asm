; CPU support routines: GDT/TSS loading.

BITS 64
section .text

; void gdt_flush(void *gdtr) — load GDT and reload all segment registers.
global gdt_flush
gdt_flush:
    lgdt [rdi]
    mov ax, 0x10                 ; SEL_KDATA
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    push qword 0x08              ; SEL_KCODE
    lea rax, [rel .reload]
    push rax
    retfq
.reload:
    ret

; void tss_flush(uint16_t sel)
global tss_flush
tss_flush:
    ltr di
    ret
