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
    int 0x13
    jc disk_error

    mov dl, [RELOC_BASE + (boot_drive - $$)]
    jmp 0x0000:0x7C00

disk_error:
    mov si, RELOC_BASE + (msg_disk_error - $$)
.print:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .print
.halt:
    cli
    hlt
    jmp .halt

boot_drive db 0
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
