; Test-only firmware faults. Never included in a release image.
bits 16
cpu 386
org 0x100
%ifndef TEST_FAULT
%define TEST_FAULT 1
%endif
    jmp start
old_video dd 0
video:
%if TEST_FAULT = 1
    cmp ax,0x4F01            ; stale advertised pitch, actual pitch unchanged
%elif TEST_FAULT = 2
    cmp ax,0x4F03            ; set returned success but another mode is active
%elif TEST_FAULT = 3
    cmp ax,0x4F15            ; corrupt EDID must not authorize a large desktop
%elif TEST_FAULT = 4
    cmp ax,0x4F05            ; broken firmware returns with interrupts disabled
%else
    cmp ax,0x4F06            ; active scanline cannot hold the visible width
%endif
    je .fault
    jmp far [cs:old_video]
.fault:
    pushf
    call far [cs:old_video]
    cmp ax,0x004F
    jne .done
%if TEST_FAULT = 1
    shl word [es:di+16],1
%elif TEST_FAULT = 2
    mov bx,3
%elif TEST_FAULT = 3
    xor byte [es:di+127],1
%elif TEST_FAULT = 4
    push bp
    mov bp,sp
    and word [ss:bp+6],0xFDFF
    pop bp
%else
    mov cx,1
%endif
.done:
    iret
start:
    mov ax,0x3510
    int 0x21
    mov [old_video],bx
    mov [old_video+2],es
    mov ax,0x2510
    mov dx,video
    int 0x21
    mov dx,message
    mov ah,9
    int 0x21
    mov dx,((image_end-$$+0x100)+15)/16
    mov ax,0x3100
    int 0x21
message db '[VBE-FAULT] installed',13,10,'$'
image_end:
