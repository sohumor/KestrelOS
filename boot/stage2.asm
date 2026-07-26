; KestrelOS stage 2 bootloader — loaded at 0x7E00 by stage 1.
;
;  1. enable the A20 line
;  2. collect the E820 memory map into the bootinfo block at 0x6000
;  3. enter "unreal mode" (4 GiB segment limits while staying in real mode)
;  4. load the kernel from disk (LBA 64+) to physical 1 MiB, 16 KiB at a time
;  5. build identity + higher-half page tables
;  6. ask the VBE BIOS for a 32-bpp linear framebuffer (optional)
;  7. switch to 64-bit long mode and jump to the kernel
;
; Assembled with: nasm -f bin -DKERNEL_SECTORS=<n>

BITS 16
ORG 0x7E00

%ifndef KERNEL_SECTORS
%error "KERNEL_SECTORS must be defined"
%endif

; Low-memory layout. Stage 1 reads 63 sectors to 0x7E00, so everything
; from 0x7E00 to 0xFDFF belongs to stage 2; the only conventional memory
; we are free to scribble on below the bootinfo block is 0x5000-0x5FFF.
;
;   0x5000  VBE controller info block   (512 bytes)
;   0x5200  VBE mode info block         (256 bytes)
;   0x5400  our copy of the mode list   (255 modes + terminator, 512 bytes)
;   0x6000  bootinfo header + E820 array (128 entries max, ends at 0x6C20)
;   0x7C00  stage 1 / real-mode stack
;   0x7E00  stage 2
;   0x20000 kernel read buffer
;   0x70000 page tables
;
; bootinfo layout (kernel/include/bootinfo.h):
;   +0  u16 e820_count   +2  u8 boot_drive   +3  u8 fb_present
;   +4  u32 fb_width     +8  u32 fb_height   +12 u32 fb_pitch
;   +16 u32 fb_bpp       +20 u32 reserved    +24 u64 fb_phys
;   +32 e820 entries, 24 bytes each
BOOTINFO     equ 0x6000
E820_ENTRIES equ BOOTINFO + 32
VBE_INFO     equ 0x5000
VBE_MODE     equ 0x5200
VBE_MODES    equ 0x5400
VBE_MAX_MODES equ 255
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
    cld

    ; ---------------- clear the bootinfo header ----------------
    xor ax, ax
    mov di, BOOTINFO
    mov cx, 16                   ; 32 bytes: counts, drive, framebuffer
    rep stosw

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
    ;
    ; Built while unreal mode still gives us 32-bit addressing; the VBE
    ; calls below go through the BIOS and reload DS/ES, which destroys the
    ; cached 4 GiB limits, so nothing above 64 KiB may be touched after
    ; this point.
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

    ; ---------------- VBE linear framebuffer ----------------
    ; Deliberately last: once a graphics mode is set, INT 10h AH=0Eh no
    ; longer produces readable text, so every error path that prints has
    ; already run. Any failure in here leaves fb_present = 0 and the
    ; kernel comes up on the VGA text console.
    sti                          ; the BIOS wants interrupts for INT 10h
    call vbe_setup
    cli

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

; ------------------------------------------------------------------
; VBE mode selection
; ------------------------------------------------------------------
; Fills the framebuffer fields of the bootinfo block and sets
; fb_present, or leaves both alone (zeroed at entry) on any failure.
; Clobbers the general registers; DS is preserved for the caller.
vbe_setup:
    push ds
    xor ax, ax
    mov ds, ax
    mov es, ax
    cld

    ; -------- controller info --------
    mov di, VBE_INFO
    mov cx, 256                  ; 512 bytes
    xor ax, ax
    rep stosw
    mov dword [VBE_INFO], 'VBE2' ; request the VBE 2.0+ fields

    mov di, VBE_INFO
    mov ax, 0x4F00
    int 0x10
    call vbe_reseg
    cmp ax, 0x004F
    jne .fail
    cmp dword [VBE_INFO], 'VESA'
    jne .fail

    ; -------- copy the mode list somewhere we own --------
    ; VideoModePtr is a far pointer that usually lands in the video ROM,
    ; and some BIOSes point it back into the info block we are about to
    ; overwrite with per-mode data, so take a private copy first.
    mov si, [VBE_INFO + 0x0E]    ; offset
    mov ax, [VBE_INFO + 0x10]    ; segment
    mov di, VBE_MODES
    xor cx, cx
    mov ds, ax
.copy_modes:
    lodsw
    stosw
    cmp ax, 0xFFFF
    je  .modes_done
    inc cx
    cmp cx, VBE_MAX_MODES
    jb  .copy_modes
    mov ax, 0xFFFF               ; truncate an absurdly long list
    stosw
.modes_done:
    xor ax, ax
    mov ds, ax

    ; -------- first preferred resolution that exists wins --------
    mov word [vbe_pref], pref_list
.pref_loop:
    mov si, [vbe_pref]
    mov ax, [si]
    test ax, ax
    jz  .fail                    ; ran out of preferences
    mov [vbe_w], ax
    mov ax, [si + 2]
    mov [vbe_h], ax
    add word [vbe_pref], 4
    call vbe_find
    cmp ax, 0xFFFF
    je  .pref_loop
    mov [vbe_mode], ax

    ; -------- stage the geometry (fb_present stays 0 for now) --------
    movzx eax, word [VBE_MODE + 0x12]   ; XResolution
    mov [BOOTINFO + 4], eax
    movzx eax, word [VBE_MODE + 0x14]   ; YResolution
    mov [BOOTINFO + 8], eax
    call vbe_pitch
    mov [BOOTINFO + 12], eax
    mov eax, 32
    mov [BOOTINFO + 16], eax
    mov eax, [VBE_MODE + 0x28]          ; PhysBasePtr
    mov [BOOTINFO + 24], eax
    xor eax, eax
    mov [BOOTINFO + 28], eax

    ; -------- set the mode, linear framebuffer --------
    mov bx, [vbe_mode]
    or  bx, 0x4000               ; bit 14: linear/flat frame buffer
    xor di, di                   ; ES:DI = 0: no user CRTC block
    mov ax, 0x4F02
    int 0x10
    call vbe_reseg
    cmp ax, 0x004F
    jne .fail

    mov byte [BOOTINFO + 3], 1
    pop ds
    ret

.fail:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov byte [BOOTINFO + 3], 0
    mov dword [BOOTINFO + 4], 0
    mov dword [BOOTINFO + 8], 0
    mov dword [BOOTINFO + 12], 0
    mov dword [BOOTINFO + 16], 0
    mov dword [BOOTINFO + 24], 0
    mov dword [BOOTINFO + 28], 0
    pop ds
    ret

; Restore DS/ES to 0 after a BIOS call without disturbing AX or the flags
; that the caller is about to test.
vbe_reseg:
    push ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    pop ax
    ret

; Scan the copied mode list for a supported 32-bpp direct-colour mode with
; a linear framebuffer at [vbe_w] x [vbe_h]. Returns the mode number in AX
; or 0xFFFF; on success VBE_MODE holds that mode's info block.
vbe_find:
    mov word [vbe_idx], 0
.next:
    mov si, [vbe_idx]
    shl si, 1
    add si, VBE_MODES
    mov ax, [si]
    cmp ax, 0xFFFF
    je  .none
    inc word [vbe_idx]
    mov [vbe_cur], ax

    mov di, VBE_MODE
    mov cx, ax
    mov ax, 0x4F01
    int 0x10
    call vbe_reseg
    cmp ax, 0x004F
    jne .next

    mov ax, [VBE_MODE]                  ; ModeAttributes
    and ax, 0x0081                      ; bit 0 supported, bit 7 linear fb
    cmp ax, 0x0081
    jne .next
    cmp byte [VBE_MODE + 0x19], 32      ; BitsPerPixel
    jne .next
    cmp byte [VBE_MODE + 0x1B], 6       ; MemoryModel: direct colour
    jne .next
    mov ax, [VBE_MODE + 0x12]           ; XResolution
    cmp ax, [vbe_w]
    jne .next
    mov ax, [VBE_MODE + 0x14]           ; YResolution
    cmp ax, [vbe_h]
    jne .next

    mov ax, [vbe_cur]
    ret
.none:
    mov ax, 0xFFFF
    ret

; Bytes per scan line of the chosen mode, in EAX. VBE 3.0 reports the
; linear-mode pitch separately (LinBytesPerScanLine at 0x32); older
; BIOSes only fill the banked value at 0x10.
vbe_pitch:
    movzx eax, word [VBE_MODE + 0x10]
    cmp word [VBE_INFO + 4], 0x0300     ; VbeVersion
    jb  .done
    movzx ecx, word [VBE_MODE + 0x32]
    movzx edx, word [VBE_MODE + 0x12]
    shl edx, 2                          ; width * 4: the smallest sane pitch
    cmp ecx, edx
    jb  .done
    mov eax, ecx
.done:
    ret

align 2
pref_list:                       ; width, height; 0 terminates
    dw 1024, 768
    dw 1280, 1024
    dw 1280, 720
    dw 800, 600
    dw 640, 480
    dw 0, 0

vbe_pref  dw 0                   ; cursor into pref_list
vbe_idx   dw 0                   ; cursor into the copied mode list
vbe_cur   dw 0                   ; mode being probed
vbe_mode  dw 0                   ; mode chosen
vbe_w     dw 0
vbe_h     dw 0

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
