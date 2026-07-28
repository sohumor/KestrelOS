; User signal-handler return trampoline. The kernel places this address as
; the handler's return address; SIGRETURN restores the complete saved frame.

BITS 64
section .text

global __kestrel_sigreturn

SYS_SIGRETURN equ 65

__kestrel_sigreturn:
    mov eax, SYS_SIGRETURN
    int 0x80
.bad:
    hlt
    jmp .bad
