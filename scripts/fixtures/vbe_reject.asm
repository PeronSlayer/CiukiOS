bits 16
org 0x100

    jmp install
old_int10 dd 0
reject_mode db 1
handler:
    cmp ax, 0x4F02
    jne .bank
    cmp byte [cs:reject_mode], 0
    je .chain
    mov byte [cs:reject_mode], 0
    jmp .reject
.bank:
    cmp ax, 0x4F05
    jne .chain
.reject:
    mov ax, 0x014F
    iret
.chain:
    jmp far [cs:old_int10]
resident_end:

install:
    mov ax, 0x3510
    int 0x21
    mov [old_int10], bx
    mov [old_int10 + 2], es
    mov dx, handler
    mov ax, 0x2510
    int 0x21
    mov dx, message
    mov ah, 9
    int 0x21
    mov dx, (resident_end - $$ + 0x100 + 15) >> 4
    mov ax, 0x3100
    int 0x21
message db '[VBEREJ] Installed: refuse first modeset, then bank requests',13,10,'$'
