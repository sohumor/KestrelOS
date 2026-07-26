; KestrelOS raw syscall entry from C.
;
;   long syscall(long n, long a, long b, long c, long d);
;
; SysV in-args arrive in rdi, rsi, rdx, rcx, r8. The kernel expects
; rax = n and syscall args in rdi, rsi, rdx, r10, so shuffle left and
; move arg4 (r8) into r10. Return value comes back in rax.

BITS 64

global syscall

section .text

syscall:
    mov rax, rdi              ; syscall number
    mov rdi, rsi              ; arg0
    mov rsi, rdx              ; arg1
    mov rdx, rcx              ; arg2
    mov r10, r8               ; arg3 (rcx is clobbered by nothing here,
                              ;  but the kernel convention uses r10)
    int 0x80
    ret
