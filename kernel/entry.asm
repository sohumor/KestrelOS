; KestrelOS kernel entry — first bytes of the kernel binary.
; Stage 2 jumps here at 0xFFFFFFFF80100000 in 64-bit long mode with
; RDI = physical address of the bootinfo block.

BITS 64
section .text.entry

global _start
extern kmain
extern __bss_start
extern __bss_end

_start:
    mov r12, rdi                 ; save bootinfo pointer

    ; zero .bss
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    add rcx, 7
    shr rcx, 3
    xor eax, eax
    cld
    rep stosq

    mov rdi, r12
    mov rsp, kernel_stack_top
    xor ebp, ebp
    call kmain
.halt:
    cli
    hlt
    jmp .halt

section .bss
align 16
global kernel_stack_top
kernel_stack:
    resb 16384
kernel_stack_top:
