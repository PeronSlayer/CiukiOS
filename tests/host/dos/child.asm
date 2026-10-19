; EXEC child for inheritance/SHARE/critical-abort probes. GPL-2.0-only.
bits 16
org 0x100
start:
    push cs
    pop ds
    cmp byte [0x80], 0
    jne abort_test
    ; The parent asserts handles 5/6 before EXEC. 5 is inherited, 6 private.
    mov bx, 5
    mov cx, 1
    mov dx, buffer
    mov ah, 0x3f
    int 0x21
    jc fail
    cmp ax, 1
    jne fail
    cmp byte [buffer], 'B'
    jne fail
    mov bx, 6
    mov ah, 0x3f
    int 0x21
    jnc fail
    cmp ax, 6
    jne fail
    mov dx, file
    mov ax, 0x3d42
    int 0x21
    jc fail
    mov bx, ax
    mov cx, 1
    mov dx, 7
    xor si, si
    mov di, 16
    mov ax, 0x5c00
    int 0x21
    jnc fail
    cmp ax, 33
    jne fail
    mov ah, 0x3e
    int 0x21
    jc fail
    mov ax, 0x4c00
    int 0x21
abort_test:
    mov dx, critical
    mov ax, 0x2524
    int 0x21
    mov dx, fault
    mov ax, 0x3d00
    int 0x21
    jc fail
    mov bx, ax
    mov dx, buffer
    mov cx, 4096
    mov ah, 0x3f
    int 0x21
    ; INT24's abort must terminate us, never return here.
fail:
    mov ax, 0x4c01
    int 0x21
critical:
    mov al, 2
    iret
file db 'D48FILE.DAT',0
fault db 'E:\D48FAULT.DAT',0
buffer times 4096 db 0
