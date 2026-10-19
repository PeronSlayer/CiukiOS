; Controller witness executed by the shell after each unchanged COM returns.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
org 100h
    push cs
    pop ds
    mov si, message
.loop:
    lodsb
    test al, al
    jz .done
    out 0e9h, al
    jmp .loop
.done:
    mov ax,4c00h
    int 21h
message db 'D48 SHELL',13,10,0
