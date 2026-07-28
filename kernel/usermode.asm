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
