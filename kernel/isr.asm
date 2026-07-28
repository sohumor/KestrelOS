; KestrelOS interrupt stubs: 256 vectors, common register save, C dispatch.

BITS 64
section .text
extern isr_dispatch

%macro MAKE_ISR 1
isr%1:
%if (%1 == 8) || (%1 == 10) || (%1 == 11) || (%1 == 12) || (%1 == 13) || \
    (%1 == 14) || (%1 == 17) || (%1 == 21) || (%1 == 29) || (%1 == 30)
    ; CPU already pushed an error code
    push %1
%else
    push 0
    push %1
%endif
    jmp isr_common
%endmacro

%assign v 0
%rep 256
    MAKE_ISR v
%assign v v+1
%endrep

isr_common:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax
    ; GS names the current CPU in ring 0 and has a zero user base in ring 3.
    ; The saved CS is 144 bytes into struct regs at this point.
    test byte [rsp + 144], 3
    jz .kernel_entry
    swapgs
.kernel_entry:
    mov rdi, rsp
    cld
    call isr_dispatch
global isr_return
isr_return:
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    ; vector, err, rip, cs are now at rsp+0,+8,+16,+24.
    test byte [rsp + 24], 3
    jz .kernel_return
    swapgs
.kernel_return:
    add rsp, 16                  ; vector + error code
    iretq

section .rodata
global isr_table
isr_table:
%assign v 0
%rep 256
    dq isr%+v
%assign v v+1
%endrep
