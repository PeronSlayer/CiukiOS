bits 16
%ifdef MZ
org 0
%define O(x) ((x)-32)
 dw 0x5a4d,filesize & 511,(filesize+511)/512,0,2,0x100,0x100,0,0x1000,0,0,0,0x1c,0
 times 32-($-$$) db 0
%else
org 0x100
%define O(x) x
%endif
start:
    push cs
    pop ds
    mov [O(entry_psp)],es
    cli
    push cs
    pop ss
    mov sp,O(stack_top)
    sti
%ifndef MZ
    push cs
    pop es
    mov bx,(stack_top-$$+0x100+15)/16
    mov ah,0x4a
    int 0x21
%endif
    mov ah,0x34
    int 0x21
    mov al,'@'
    out 0xe9,al
    mov al,'L'
    out 0xe9,al
    mov al,ID+'0'
    out 0xe9,al
    mov al,[es:bx]
    add al,'0'
    out 0xe9,al
    mov al,10
    out 0xe9,al
%ifdef NEST
    push cs
    pop es
    mov ax,cs
    mov [O(params)+4],ax
    mov [O(params)+8],ax
    mov [O(params)+12],ax
    mov bx,O(params)
    mov dx,O(path)
    mov ax,0x4b00
    int 0x21
    jc .bad
    mov ah,0x4d
    int 0x21
    cmp ax,0x5a
    jne .bad
    mov ah,0x34
    int 0x21
    mov al,'@'
    out 0xe9,al
    mov al,'N'
    out 0xe9,al
    mov al,[es:bx]
    add al,'0'
    out 0xe9,al
    mov al,10
    out 0xe9,al
    jmp .okay
.bad:
    mov al,'!'
    out 0xe9,al
.okay:
%endif
%if EXIT == 0x4c
    mov ax,0x4c5a
    int 0x21
%elif EXIT == 0x20
%ifdef MZ
    push word [cs:O(entry_psp)]
    push word 0
    retf
%else
    int 0x20
%endif
%elif EXIT == 0x00
    xor ax,ax
    int 0x21
%elif EXIT == 0x31
    mov dx,(stack_top-$$+0x100+15)/16
    mov ax,0x315a
    int 0x21
%elif EXIT == 0xff
    ; Near RET to PSP:0 with the entry COM stack convention restored.
    mov sp,O(stack_top)-2
    mov word [ss:O(stack_top)-2],0
    ret
%endif
entry_psp dw 0
params dw 0,O(tail),0,0x5c,0,0x6c,0
tail db 0,13
path db 'M4C.EXE',0
 times 512 db 0
stack_top:
filesize equ $-$$
