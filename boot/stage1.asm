; KestrelOS stage 1 bootloader — lives in the MBR (sector 0).
; BIOS loads us at 0x7C00 with DL = boot drive. We load stage 2 from
; LBA 1..63 to 0x7E00 using INT 13h extended reads, then jump to it.

BITS 16
ORG 0x7C00

STAGE2_LBA     equ 1
STAGE2_SECTORS equ 63
STAGE2_ADDR    equ 0x7E00

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [boot_drive], dl

    ; require INT 13h extensions (LBA reads)
    mov ah, 0x41
    mov bx, 0x55AA
    int 0x13
    jc  err
    cmp bx, 0xAA55
    jne err

    ; read stage 2
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc  err

    mov dl, [boot_drive]
    jmp 0x0000:STAGE2_ADDR

err:
    mov si, msg_err
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

msg_err    db "KestrelOS: boot disk error", 13, 10, 0
boot_drive db 0

align 4
dap:
    db 0x10, 0
    dw STAGE2_SECTORS
    dw 0, 0x07E0                ; offset 0, segment 0x07E0 -> 0x7E00
    dq STAGE2_LBA

times 510-($-$$) db 0
dw 0xAA55
