; f9-02 incomplete-install MBR (cd-setup-plan.md 6.2). libdisk writes these
; code bytes with the real partition table and disk signature during P1; P5
; replaces only bytes 0..439 with the canonical mbr.asm code. It never reads
; the disk: it prints one line through INT 10h AH=0Eh teletype and halts.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
org 0x7c00
    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7c00
    sti
    cld
    mov si, message
.next:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0e
    mov bx, 0x0007
    int 0x10
    jmp .next
.halt:
    cli
    hlt
    jmp .halt
message db 'CiukiOS Setup did not complete. Insert the Setup CD and restart.', 13, 10, 0
%if $-$$ > 440
%error "stub MBR code overlaps the disk signature"
%endif
times 440-($-$$) db 0
dd 0 ; Disk signature, written by libdisk with the planned table.
dw 0
times 64 db 0 ; Partition table, written by libdisk.
dw 0xaa55
