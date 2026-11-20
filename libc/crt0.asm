; KestrelOS userspace C runtime entry.
;
; The kernel enters at _start with:
;   rdi = argc
;   rsi = argv (NUL-terminated char* array on the user stack, argv[argc]==0)
;   rdx = initial cwd string (NULL when none was supplied)
;
; We align the stack to 16 bytes at the call site (SysV ABI), call
; main(argc, argv), then pass the return value to SYS_EXIT via int 0x80.

BITS 64

global _start
global kestrel_startup_cwd
extern main

SYS_EXIT equ 0

section .text

_start:
    xor rbp, rbp              ; mark the outermost frame
    and rsp, -16              ; 16-byte aligned at the call instruction
    mov [rel startup_cwd], rdx
    call main
    mov edi, eax              ; exit code = main's return value
    mov eax, SYS_EXIT
    int 0x80
.hang:                        ; SYS_EXIT does not return; just in case
    hlt
    jmp .hang

kestrel_startup_cwd:
    mov rax, [rel startup_cwd]
    test rax, rax
    jnz .done
    lea rax, [rel root_cwd]
.done:
    ret

section .rodata
root_cwd: db "/", 0

section .bss
align 8
startup_cwd: resq 1
