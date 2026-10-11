; Observe the running personality rather than advertising unconditional RAM.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
org 0x100
start:
    cli
    mov ax, cs
    mov ss, ax
    mov sp, stack_top
    sti
    mov ds, ax
    mov es, ax
    cld
    mov bx, (image_end - $$ + 0x100 + 15) >> 4
    mov ah, 0x4A
    int 0x21
    mov ah, 0x30
    int 0x21
    mov di, message + 9
    call hex_byte
    inc di
    mov al, ah
    call hex_byte
    mov bx, 0xFFFF
    mov ah, 0x48
    int 0x21
    jnc .bad
    cmp ax, 8
    jne .bad
    mov ax, bx
    mov di, message + 20
    mov al, ah
    call hex_byte
    mov ax, bx
    call hex_byte
    mov ah, 0x19
    int 0x21
    add al, 'A'
    mov [message + 25], al
    mov dx, message
    call record_dollar
    mov ax, 0x4C00
    int 0x21
.bad:
    mov ax, 0x4C01
    int 0x21
hex_byte:
    push ax
    push bx
    push cx
    mov bl, al
    mov cl, 4
    shr al, cl
    call .digit
    mov al, bl
    and al, 15
    call .digit
    pop cx
    pop bx
    pop ax
    ret
.digit:
    add al, '0'
    cmp al, '9'
    jbe .store
    add al, 7
.store:
    stosb
    ret
message db 'DOSREADY 00.00 free=0000 C',13,10,'$'
%include "src/probes/doscompat/record.inc"
align 16
stack times 512 db 0
stack_top:
image_end:
