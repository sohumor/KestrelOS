; First entry into ring 3. Builds an iretq frame by hand and drops into
; the user program with a clean register file.

BITS 64
section .text

SEL_UCODE equ 0x18
SEL_UDATA equ 0x20

; void enter_usermode(uint64_t entry, uint64_t user_rsp,
;                     uint64_t argc, uint64_t argv)   -- noreturn
;   rdi = entry, rsi = user_rsp, rdx = argc, rcx = argv
global enter_usermode
enter_usermode:
    cli                             ; no interrupts until iretq restores IF

    mov ax, SEL_UDATA | 3
    mov ds, ax
    mov es, ax
    mov fs, ax
    ; Active GS is the kernel's per-CPU base. The paired interrupt path
    ; uses SWAPGS, so exchange it for the zero user base before iretq.
    swapgs

    push qword SEL_UDATA | 3        ; ss
    push rsi                        ; rsp
    push qword 0x202                ; rflags: IF set, reserved bit 1
    push qword SEL_UCODE | 3        ; cs
    push rdi                        ; rip

    mov rdi, rdx                    ; argc -> first user argument
    mov rsi, rcx                    ; argv -> second user argument

    xor eax, eax                    ; scrub everything else
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d

    iretq

; void hangar_enter_user(const struct hangar_regs *r)   -- noreturn
;   Resume a parked process: rdi points at gpr[15] (rax..r15 in struct
;   regs order), rip, rflags, rsp. Selectors are always the user ones.
global hangar_enter_user
hangar_enter_user:
    cli
    mov ax, SEL_UDATA | 3
    mov ds, ax
    mov es, ax
    mov fs, ax
    swapgs

    push qword SEL_UDATA | 3        ; ss
    push qword [rdi + 17*8]         ; rsp
    push qword [rdi + 16*8]         ; rflags
    push qword SEL_UCODE | 3        ; cs
    push qword [rdi + 15*8]         ; rip

    mov rax, [rdi + 0*8]
    mov rbx, [rdi + 1*8]
    mov rcx, [rdi + 2*8]
    mov rdx, [rdi + 3*8]
    mov rsi, [rdi + 4*8]
    mov rbp, [rdi + 6*8]
    mov r8,  [rdi + 7*8]
    mov r9,  [rdi + 8*8]
    mov r10, [rdi + 9*8]
    mov r11, [rdi + 10*8]
    mov r12, [rdi + 11*8]
    mov r13, [rdi + 12*8]
    mov r14, [rdi + 13*8]
    mov r15, [rdi + 14*8]
    mov rdi, [rdi + 5*8]
    iretq
