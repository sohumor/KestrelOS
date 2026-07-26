; KestrelOS stage 2 bootloader — loaded at 0x7E00 by stage 1.
;
;  1. enable the A20 line
;  2. collect the E820 memory map into the bootinfo block at 0x6000
;  3. enter "unreal mode" (4 GiB segment limits while staying in real mode)
;  4. load the kernel from disk (LBA 64+) to physical 1 MiB, 16 KiB at a time
;  5. build identity + higher-half page tables
;  6. switch to 64-bit long mode and jump to the kernel
;
; Assembled with: nasm -f bin -DKERNEL_SECTORS=<n>

BITS 16
ORG 0x7E00

%ifndef KERNEL_SECTORS
%error "KERNEL_SECTORS must be defined"
%endif

; Low-memory layout (all free conventional memory):
BOOTINFO     equ 0x6000          ; u16 e820_count; u8 boot_drive; entries at +8
E820_ENTRIES equ BOOTINFO + 8    ; 24 bytes each, max 128
LOADBUF      equ 0x20000         ; 16 KiB kernel read buffer
LOADBUF_SEG  equ 0x2000
PML4         equ 0x70000
PDPT_LO      equ 0x71000
PD_LO        equ 0x72000
PDPT_HI      equ 0x73000
KERNEL_PHYS  equ 0x100000
KERNEL_LBA   equ 64
KERNEL_ENTRY equ 0xFFFFFFFF80100000
CHUNK        equ 32              ; sectors per INT 13h read (16 KiB)

start:
    mov [boot_drive], dl

    ; ---------------- A20 via the fast gate (port 0x92) ----------------
    in  al, 0x92
    test al, 2
    jnz .a20_done
    or  al, 2
    and al, 0xFE                 ; never touch the reset bit
    out 0x92, al
.a20_done:

    ; ---------------- E820 memory map ----------------
    xor ebx, ebx
    xor bp, bp                   ; entry count
    mov di, E820_ENTRIES
.e820_loop:
    mov eax, 0xE820
    mov edx, 0x534D4150          ; 'SMAP'
    mov ecx, 24
    mov dword [di + 20], 1       ; ACPI 3.x "entry valid" default
    int 0x15
    jc  .e820_done
    cmp eax, 0x534D4150
    jne .e820_done
    inc bp
    add di, 24
    cmp bp, 128
    jae .e820_done
    test ebx, ebx
    jnz .e820_loop
.e820_done:
    mov [BOOTINFO], bp
    mov al, [boot_drive]
    mov [BOOTINFO + 2], al

    ; ---------------- unreal mode: 4 GiB limits on DS/ES ----------------
    cli
    push ds
    push es
    lgdt [gdt16_ptr]
    mov eax, cr0
    or  al, 1
    mov cr0, eax
    jmp short $+2
    mov bx, 0x08
    mov ds, bx
    mov es, bx
    and al, 0xFE
    mov cr0, eax
    pop es
    pop ds                       ; real-mode selectors, cached 4 GiB limits
    sti

    ; ---------------- load kernel to 1 MiB ----------------
    mov dword [dest], KERNEL_PHYS
    mov word [remaining], KERNEL_SECTORS
.load_loop:
    mov ax, [remaining]
    test ax, ax
    jz  .load_done
    cmp ax, CHUNK
    jbe .have_count
    mov ax, CHUNK
.have_count:
    mov [dap_num], ax
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc  disk_err
    ; copy chunk from LOADBUF to [dest] with 32-bit addressing
    mov esi, LOADBUF
    mov edi, [dest]
    movzx ecx, word [dap_num]
    shl ecx, 7                   ; sectors * 512 / 4 dwords
    cld
    a32 rep movsd
    movzx eax, word [dap_num]
    shl eax, 9
    add [dest], eax
    movzx eax, word [dap_num]
    add [dap_lba], eax
    mov ax, [remaining]
    sub ax, [dap_num]
    mov [remaining], ax
    jmp .load_loop
.load_done:

    ; ---------------- build initial page tables ----------------
    ; PML4[0]   -> PDPT_LO -> PD_LO: identity map first 1 GiB (2 MiB pages)
    ; PML4[256] -> PDPT_LO         : direct map at 0xFFFF800000000000
    ; PML4[511] -> PDPT_HI[510] -> PD_LO : kernel at 0xFFFFFFFF80000000 -> 0
    cli
    xor eax, eax
    mov edi, PML4
    mov ecx, 4096                ; 4 pages = 4096 dwords
    a32 rep stosd

    mov edi, PML4
    mov eax, PDPT_LO | 3
    mov [edi], eax
    mov [edi + 256*8], eax
    mov eax, PDPT_HI | 3
    mov [edi + 511*8], eax

    mov edi, PDPT_LO
    mov eax, PD_LO | 3
    mov [edi], eax

    mov edi, PDPT_HI
    mov eax, PD_LO | 3
    mov [edi + 510*8], eax

    mov edi, PD_LO
    mov eax, 0x83                ; present | writable | 2 MiB page
    mov ecx, 512
.pd_loop:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, 0x200000
    add edi, 8
    loop .pd_loop

    ; ---------------- enter long mode ----------------
    lgdt [gdt64_ptr]
    mov eax, cr4
    or  eax, 1 << 5              ; PAE
    mov cr4, eax
    mov eax, PML4
    mov cr3, eax
    mov ecx, 0xC0000080          ; EFER
    rdmsr
    or  eax, 1 << 8              ; LME
    wrmsr
    mov eax, cr0
    or  eax, (1 << 31) | 1       ; PG | PE  (real mode -> long mode)
    mov cr0, eax
    jmp 0x08:long_start

disk_err:
    mov si, msg_disk_err
.print:
    lodsb
    test al, al
    jz  .halt
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .print
.halt:
    hlt
    jmp .halt

msg_disk_err db "KestrelOS: stage2 disk error", 13, 10, 0

align 8
gdt16:
    dq 0
    dq 0x00CF92000000FFFF        ; 32-bit data, base 0, limit 4 GiB
gdt16_ptr:
    dw 15
    dd gdt16

align 8
gdt64:
    dq 0
    dq 0x00AF9A000000FFFF        ; 64-bit code
    dq 0x00CF92000000FFFF        ; data
gdt64_ptr:
    dw 23
    dd gdt64

align 4
dap:
    db 0x10, 0
dap_num:
    dw 0
    dw 0, LOADBUF_SEG            ; offset 0, segment
dap_lba:
    dq KERNEL_LBA

dest:      dd 0
remaining: dw 0
boot_drive db 0

BITS 64
long_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov rsp, 0x60000
    mov edi, BOOTINFO            ; first argument to kmain (phys addr)
    mov rax, KERNEL_ENTRY
    jmp rax
