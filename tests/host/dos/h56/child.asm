; H56 child: behaviour selected by the first command-tail character.
;   X exit 42h   T TSR (31h) 07h   K TSR (INT 27h)   A INT 20h
;   N EXEC CHILD.COM X and return its code   H inherited-handle check
;   D DTA at PSP:80h and parent PSP check   F EXEC must fail (missing)
; SPDX-License-Identifier: GPL-2.0-only
bits 16
org 0x100
start:
    mov sp, stack_top
    mov bx, (image_end - $$ + 0x100 + 15) >> 4
    mov ah, 0x4a
    int 0x21
    mov si, 0x81
.skip:
    lodsb
    cmp al, ' '
    je .skip
    cmp al, 'X'
    je exit42
    cmp al, 'T'
    je tsr
    cmp al, 'K'
    je int27
    cmp al, 'A'
    je abort
    cmp al, 'N'
    je nested
    cmp al, 'H'
    je handles
    cmp al, 'D'
    je dta
    mov al, 0xee
    jmp exit
exit42:
    mov al, 0x42
exit:
    mov ah, 0x4c
    int 0x21
tsr:
    mov dx, (image_end - $$ + 0x100 + 15) >> 4
    mov ax, 0x3107
    int 0x21
int27:
    mov dx, image_end
    int 0x27
abort:
    int 0x20
nested:
    mov word [block+2], grand_tail
    mov [block+4], cs
    mov word [block+6], 0x5c
    mov [block+8], cs
    mov word [block+10], 0x6c
    mov [block+12], cs
    mov [save_sp], sp
    mov dx, self
    mov bx, block
    mov ax, 0x4b00
    int 0x21
    cli
    mov ax, cs
    mov ss, ax
    mov sp, [cs:save_sp]
    sti
    push cs
    pop ds
    push cs
    pop es
    mov al, 0xe1
    jc exit
    mov ah, 0x4d
    int 0x21
    jmp exit
; Handle 5 must be inherited with a shared SFT (count >= 2); handle 6 was
; opened O_NOINHERIT and must be absent; entries 20.. do not exist.
handles:
    mov bx, 5
    mov ax, 0x1220
    int 0x2f
    mov al, 0xe2
    jc exit
    mov bl, [es:di]
    cmp bl, 0xff
    je exit
    xor bh, bh
    mov ax, 0x1216
    int 0x2f
    mov al, 0xe3
    jc exit
    cmp word [es:di], 2
    jb exit
    mov bx, 6
    mov ax, 0x1220
    int 0x2f
    mov al, 0xe4
    jc exit
    cmp byte [es:di], 0xff
    jne exit
    push cs
    pop es
    mov ah, 0x51
    int 0x21
    mov es, bx
    mov al, 0xe5
    cmp word [es:0x32], 20
    jne exit
    xor al, al
    jmp exit
; The child's DTA is its PSP:80h and its parent field names the caller.
dta:
    mov ah, 0x2f
    int 0x21
    mov al, 0xe6
    cmp bx, 0x80
    jne exit
    mov ax, es
    mov dx, cs
    cmp ax, dx
    mov al, 0xe7
    jne exit
    xor al, al
    jmp exit
self db 'C:\CHILD.COM', 0
grand_tail db 2, ' X', 13
save_sp dw 0
block times 14 db 0
    times 256 db 0
stack_top:
image_end:
