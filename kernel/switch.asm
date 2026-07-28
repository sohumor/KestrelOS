; Context switch and new-task bootstrap.

BITS 64
section .text

; void ctx_switch(uint64_t *save_rsp, uint64_t new_rsp)
; Saves callee-saved registers on the current stack, stores RSP through
; save_rsp, switches to new_rsp and pops the same frame.
global ctx_switch
ctx_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; SMP scheduler variant. The scheduler lock remains held until RSP no longer
; references the outgoing task; releasing it here closes the window in which
; another CPU could select that task while it still executes on this stack.
; void ctx_switch_unlock(u64 *save_rsp, u64 new_rsp, u32 *lock)
global ctx_switch_unlock
ctx_switch_unlock:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    mov dword [rdx], 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; First code a fresh task runs. ctx_switch pops the initial frame where
; r12 = entry function and r13 = argument, then returns here.
global task_bootstrap
extern task_exit_from_bootstrap
task_bootstrap:
    sti
    mov rdi, r13
    call r12
    xor edi, edi
    call task_exit_from_bootstrap
