; KestrelOS userspace C runtime entry.
;
; The kernel enters at _start with:
;   rdi = argc
;   rsi = argv (NUL-terminated char* array on the user stack, argv[argc]==0)
;
; We align the stack to 16 bytes at the call site (SysV ABI), call
; main(argc, argv), then pass the return value to SYS_EXIT via int 0x80.

BITS 64

global _start
extern main

SYS_EXIT equ 0

section .text

_start:
    xor rbp, rbp              ; mark the outermost frame
    and rsp, -16              ; 16-byte aligned at the call instruction
    call main
    mov edi, eax              ; exit code = main's return value
    mov eax, SYS_EXIT
    int 0x80
.hang:                        ; SYS_EXIT does not return; just in case
    hlt
    jmp .hang
