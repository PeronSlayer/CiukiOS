bits 16
org 0x0100

    mov dx, msg
    mov ah, 0x09
    int 0x21

    mov ax, 0x4C37
    int 0x21

    retf

msg db "COM demo via INT21h", 13, 10, '$'
