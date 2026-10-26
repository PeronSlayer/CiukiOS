; Mouse-driven DOS Box program, run on the real unified guest/INT33 path.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
org 0x100
start:
    push cs
    pop ds
    xor ax,ax
    int 0x33
    cmp ax,0xFFFF
    jne fail
    mov ax,0x0F
    mov cx,8
    mov dx,8
    int 0x33
    mov ax,4
    mov cx,320
    mov dx,96
    int 0x33
    mov ax,1
    int 0x33
    push cs
    pop es
    mov dx,handler
    mov cx,0x7F
    mov ax,0x0C
    int 0x33
.wait:
    mov ax,[events]
    and ax,7
    cmp ax,7
    je .check
    mov ax,0x1680
    int 0x2F
    jmp .wait
.check:
    mov ax,0x0B
    int 0x33
    cmp cx,37
    jne fail
    cmp dx,21
    jne fail
    mov ax,3
    int 0x33
    test bx,7
    jnz fail
    cmp cx,136
    jne fail
    cmp dx,40
    jne fail
    mov si,passed
    jmp finish
fail:
    mov si,failed
finish:
    xor cx,cx
    mov ax,0x0C
    int 0x33
    mov ax,2
    int 0x33
    mov dx,0xE9
.print:
    lodsb
    test al,al
    jz .exit
    out dx,al
    jmp .print
.exit:
    mov ax,0x4C00
    int 0x21
handler:
    or [cs:events],ax
    retf
events dw 0
passed db 'DOSMOUSE PASS',13,10,0
failed db 'DOSMOUSE FAIL',13,10,0
