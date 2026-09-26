; Test only: a BIOS which stalls during discovery, or rejects mode info.
bits 16
org 0x100
    jmp start
old_video dd 0
video:
%ifdef REJECT_PREVIEW
    cmp ax,0x4F01
    jne .chain
    mov ax,0x014F
    iret
%else
    cmp ax,0x4F00
    je .stall
    cmp ax,0x4F15
    jne .chain
.stall:
    mov al,'P'
    out 0xE9,al
    cli
.halt:
    hlt
    jmp .halt
%endif
.chain:
    jmp far [cs:old_video]
start:
    mov ax,0x3510
    int 0x21
    mov [old_video],bx
    mov [old_video+2],es
    push cs
    pop ds
    mov dx,video
    mov ax,0x2510
    int 0x21
    mov dx,message
    mov ah,9
    int 0x21
    mov dx,(image_end-$$+0x100+15)/16
    mov ax,0x3100
    int 0x21
message db '[SETUP-PROBE-FAULT] installed',13,10,'$'
image_end:
