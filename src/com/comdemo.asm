bits 16
org 0x0100

start:
    mov dx, msg
    mov ah, 0x09
    int 0x21

    mov bx, 0x0020
    mov ah, 0x48
    int 0x21
    jc fail
    mov [seg1], ax

    mov bx, 0x0010
    mov ah, 0x48
    int 0x21
    jc cleanup_seg1
    mov [seg2], ax

    mov ax, [seg2]
    mov es, ax
    mov bx, 0x0018
    mov ah, 0x4A
    int 0x21
    jc cleanup_both

    mov ax, [seg2]
    mov es, ax
    mov ah, 0x49
    int 0x21
    jc cleanup_both
    mov word [seg2], 0

    mov bx, 0x0008
    mov ah, 0x48
    int 0x21
    jc cleanup_seg1
    mov [seg2], ax

    mov ax, [seg1]
    mov es, ax
    mov bx, 0x0030
    mov ah, 0x4A
    int 0x21
    jc cleanup_both

    mov ax, [seg2]
    mov es, ax
    mov ah, 0x49
    int 0x21
    jc cleanup_both
    mov word [seg2], 0

    mov ax, [seg1]
    mov es, ax
    mov ah, 0x49
    int 0x21
    jc fail
    mov word [seg1], 0

    mov ax, 0x4C37
    int 0x21
    retf

cleanup_both:
    mov ax, [seg2]
    or ax, ax
    jz cleanup_seg1
    mov es, ax
    mov ah, 0x49
    int 0x21
    mov word [seg2], 0

cleanup_seg1:
    mov ax, [seg1]
    or ax, ax
    jz fail
    mov es, ax
    mov ah, 0x49
    int 0x21
    mov word [seg1], 0

fail:
    mov ax, 0x4CEE
    int 0x21
    retf

msg db "COM demo via INT21h", 13, 10, '$'
seg1 dw 0
seg2 dw 0
