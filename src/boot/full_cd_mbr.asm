bits 16
org 0x7C00

%define RELOC_BASE 0x0600
%ifndef PARTITION_LBA
%define PARTITION_LBA 63
%endif
%ifndef PARTITION_SECTORS
%define PARTITION_SECTORS 262144
%endif

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl

    cld
    mov si, 0x7C00
    mov di, RELOC_BASE
    mov cx, 256
    rep movsw
    jmp 0x0000:RELOC_BASE + (relocated - $$)

relocated:
    xor ax, ax
    mov ds, ax
    mov es, ax

    mov si, RELOC_BASE + (vbr_dap - $$)
    mov dl, [RELOC_BASE + (boot_drive - $$)]
    mov ah, 0x42
    call bios_disk
    jnc boot_vbr

%if PARTITION_LBA == 63
    xor ax, ax
    mov es, ax
    mov bx, 0x7C00
    mov ax, 0x0201
    xor ch, ch
    mov cl, 0x01
    mov dh, 0x01
    mov dl, [RELOC_BASE + (boot_drive - $$)]
    call bios_disk
    jc disk_error
%else
    jmp disk_error              ; the fixed CHS fallback describes LBA 63 only
%endif

boot_vbr:
    cmp word [0x7DFE], 0xAA55
    jne disk_error
    mov dl, [RELOC_BASE + (boot_drive - $$)]
    jmp 0x0000:0x7C00

; Keep the relocated MBR's segments and pointers across both disk services.
; These calls consume only the BIOS carry result.
bios_disk:
    pushad
    push ds
    push es
    push fs
    push gs
    stc
    sti
    int 0x13
    pop gs
    pop fs
    pop es
    pop ds
    popad
    cld
    sti
    ret

disk_error:
    mov si, RELOC_BASE + (msg_disk_error - $$)
.print:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0E
    mov bx, 0x0007
    pushad
    push ds
    push es
    int 0x10
    pop es
    pop ds
    popad
    cld
    jmp .print
.halt:
    cli
    hlt
    jmp .halt

boot_drive db 0
align 4, db 0
vbr_dap:
    db 0x10
    db 0
    dw 1
    dw 0x7C00
    dw 0x0000
    dd PARTITION_LBA
    dd 0
msg_disk_error db "[CD-MBR] Disk read error", 13, 10, 0

times 446 - ($ - $$) db 0

; Active FAT16 partition containing the CiukiOS full profile image.
db 0x80
db 0x01, 0x01, 0x00
db 0x06
db 0xFE, 0xFF, 0xFF
dd PARTITION_LBA
dd PARTITION_SECTORS

times 510 - ($ - $$) db 0
dw 0xAA55
