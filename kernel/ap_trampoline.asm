; Secondary-processor startup trampoline. smp.c copies this blob to physical
; 0x1000 and patches the data fields at the end before each INIT/SIPI.

BITS 16
section .rodata
align 16

%define AP_PHYS 0x1000
%define OFF(x) (x - ap_trampoline_start)

global ap_trampoline_start
global ap_trampoline_end
global ap_trampoline_cr3
global ap_trampoline_stack
global ap_trampoline_target
global ap_trampoline_cpu

ap_trampoline_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x0FF0

    lgdt [cs:OFF(ap_gdtr)]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:(AP_PHYS + OFF(ap_protected))

BITS 32
ap_protected:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov eax, [AP_PHYS + OFF(ap_trampoline_cr3)]
    mov cr3, eax
    mov eax, cr4
    or eax, 1 << 5               ; PAE
    mov cr4, eax

    mov ecx, 0xC0000080          ; EFER
    rdmsr
    or eax, 1 << 8               ; LME
    wrmsr

    mov eax, cr0
    or eax, (1 << 31) | 1        ; paging + protected mode
    mov cr0, eax
    jmp dword 0x18:(AP_PHYS + OFF(ap_long))

BITS 64
ap_long:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax

    mov rsp, [rel ap_trampoline_stack]
    and rsp, -16
    sub rsp, 8                    ; SysV function-entry alignment
    mov edi, [rel ap_trampoline_cpu]
    mov rax, [rel ap_trampoline_target]
    jmp rax

align 8
ap_gdt:
    dq 0
    dq 0x00CF9A000000FFFF         ; 32-bit code
    dq 0x00CF92000000FFFF         ; data
    dq 0x00AF9A000000FFFF         ; 64-bit code
ap_gdt_end:

ap_gdtr:
    dw ap_gdt_end - ap_gdt - 1
    dd AP_PHYS + OFF(ap_gdt)

align 8
ap_trampoline_cr3:
    dq 0
ap_trampoline_stack:
    dq 0
ap_trampoline_target:
    dq 0
ap_trampoline_cpu:
    dd 0

ap_trampoline_end:
