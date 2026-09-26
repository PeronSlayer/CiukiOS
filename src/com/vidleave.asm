bits 16
org 0x0100

start:
    push cs
    pop ds
    mov dx, msg_mode13
    mov ah, 0x09
    int 0x21

    mov ax, 0x0013
    int 0x10

    mov ax, 0xA000
    mov es, ax
    xor di, di
    mov cx, 32000
    mov ax, 0x2A15
    rep stosw

    ; Deliberately leave graphics mode active.  The parent shell must restore
    ; its own complete text-mode contract after the standard DOS termination.
    mov ax, 0x4C00
    int 0x21

msg_mode13 db '[VIDLEAVE] MODE13', 13, 10, '$'
