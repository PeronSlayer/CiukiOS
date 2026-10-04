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

%ifdef BOOT_DISK_LOG
    call bootlog_begin
%endif

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
%ifdef BOOT_DISK_LOG
    mov al, 0xE0
    push cs
    call bootlog_update
%endif
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

%ifdef BOOT_DISK_LOG
; Four raw sectors in the unused pre-partition track (LBA 1..4).  A record
; survives a reset without touching FAT.  Calls are best-effort: a BIOS that
; refuses CHS writes can still boot through its EDD read path.
%define BOOTLOG_BUFFER 0x7000
%define BOOTLOG_API    0x06E0
times (BOOTLOG_API - RELOC_BASE) - ($ - $$) db 0
bootlog_update:
    pushf
    pushad
    push ds
    push es
    xor bx, bx
    mov ds, bx
    mov es, bx
    mov [BOOTLOG_BUFFER+6], al
    mov byte [BOOTLOG_BUFFER+7], 0
    call bootlog_write
    pop es
    pop ds
    popad
    popf
    retf

bootlog_begin:
    mov word [RELOC_BASE + (bootlog_sequence - $$)], 0
    mov byte [RELOC_BASE + (bootlog_slot - $$)], 0
    mov byte [RELOC_BASE + (bootlog_found - $$)], 0
    mov bp, 4
.scan:
    mov cx, bp
    inc cl                         ; slots 1..4 are CHS sectors 2..5
    mov bx, BOOTLOG_BUFFER
    mov ax, 0x0201
    xor dh, dh
    mov dl, [RELOC_BASE + (boot_drive - $$)]
    call bios_disk
    jc .next
    cmp dword [BOOTLOG_BUFFER], 'CKLG'
    jne .next
    mov ax, [BOOTLOG_BUFFER+4]
    cmp ax, [RELOC_BASE + (bootlog_sequence - $$)]
    jbe .next
    mov [RELOC_BASE + (bootlog_sequence - $$)], ax
    mov ax, bp
    mov [RELOC_BASE + (bootlog_slot - $$)], al
    mov byte [RELOC_BASE + (bootlog_found - $$)], 1
.next:
    dec bp
    jnz .scan
    cmp byte [RELOC_BASE + (bootlog_found - $$)], 0
    je .first
    mov al, [RELOC_BASE + (bootlog_slot - $$)]
    and al, 3
    inc al
    mov [RELOC_BASE + (bootlog_slot - $$)], al
    jmp .prepare
.first:
    mov byte [RELOC_BASE + (bootlog_slot - $$)], 1
.prepare:
    inc word [RELOC_BASE + (bootlog_sequence - $$)]
    xor ax, ax
    mov di, BOOTLOG_BUFFER
    mov cx, 256
    rep stosw
    mov dword [BOOTLOG_BUFFER], 'CKLG'
    mov ax, [RELOC_BASE + (bootlog_sequence - $$)]
    mov [BOOTLOG_BUFFER+4], ax
    mov byte [BOOTLOG_BUFFER+6], 1
    mov al, [RELOC_BASE + (boot_drive - $$)]
    mov [BOOTLOG_BUFFER+8], al
    call bootlog_write
    ret

bootlog_write:
    mov ax, 0x0301
    mov bx, BOOTLOG_BUFFER
    mov cl, [RELOC_BASE + (bootlog_slot - $$)]
    inc cl
    xor ch, ch
    xor dh, dh
    mov dl, [RELOC_BASE + (boot_drive - $$)]
    call bios_disk
    ret

bootlog_sequence dw 0
bootlog_slot db 0
bootlog_found db 0
%endif

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
