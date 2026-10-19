; Canonical FreeDOS hard-disk envelope, not a replacement FreeDOS boot sector.
; Relocate before reading the pinned VBR at LBA 63. GPL-2.0-only.
bits 16
org 0x600
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    cld
    mov si, 0x7c00
    mov di, 0x600
    mov cx, 256
    rep movsw
    jmp 0:relocated
relocated:
    sti
    mov si, packet
    mov ah, 0x42
    int 0x13
    jc failed
    mov dl, 0x80
    jmp 0:0x7c00
failed:
    cli
    hlt
    jmp failed
packet:
    db 16,0
    dw 1,0x7c00,0
    dq 63
times 446-($-$$) db 0
    db 0x80,1,1,0,0x0e,1,0x41,4
    dd 63,262144-63
times 510-($-$$) db 0
    dw 0xaa55
