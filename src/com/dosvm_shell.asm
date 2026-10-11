; Minimal boot wrapper for the existing COMMAND_COMPAT text interpreter.
; Execute AUTOEXEC lines through COMMAND /C, then an interactive COMMAND.
; Shell return requests one clean VM exit without changing CIUKIDOS INT21.
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
    mov [params + 4], ax
    mov [params + 8], ax
    mov [params + 12], ax
    mov bx, (image_end - $$ + 0x100 + 15) >> 4
    mov ah, 0x4A
    int 0x21
    jc fatal
    mov dx, autoexec
    mov ax, 0x3D00
    int 0x21
    jc fatal
    mov bx, ax
    mov dx, batch
    mov cx, 1023
    mov ah, 0x3F
    int 0x21
    jc fatal
    mov di, batch
    add di, ax
    mov byte [di], 0
    mov ah, 0x3E
    int 0x21
    mov si, batch
.line:
    mov di, tail + 4
    mov byte [tail + 1], ' '
    mov byte [tail + 2], '/'
    mov byte [tail + 3], 'C'
    mov byte [di], ' '
    inc di
    mov cx, 4
.copy:
    lodsb
    test al, al
    jz .end
    cmp al, 13
    je .copy
    cmp al, 10
    je .run
    cmp cx, 125
    jae fatal
    stosb
    inc cx
    jmp .copy
.end:
    dec si
.run:
    cmp cx, 4
    je .next
    mov [tail], cl
    mov byte [di], 13
    push si
    call execute
    pop si
    jc fatal
.next:
    cmp byte [si], 0
    jne .line
    mov byte [tail], 0
    mov byte [tail + 1], 13
    call execute
    jc fatal
    mov dx, 0xEA
    xor al, al
    out dx, al
    mov ax, 0x4C00
    int 0x21
fatal:
    mov dx, 0xFFFF
    xor al, al
    out dx, al
    hlt
    jmp fatal
execute:
    push cs
    pop ds
    push cs
    pop es
    mov dx, command
    mov bx, params
    mov ax, 0x4B00
    int 0x21
    pushf
    push cs
    pop ds
    push cs
    pop es
    popf
    ret
command db '\COMMAND.COM',0
autoexec db '\AUTOEXEC.BAT',0
params dw 0,tail,0,0x5C,0,0x6C,0
tail times 128 db 0
batch times 1024 db 0
align 16
stack times 1024 db 0
stack_top:
image_end:
