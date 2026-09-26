; Test-only cold-boot option ROM. No production payload is modified.
; Apply a configurable total scratch footprint (including INT frame) only
; to calls from the shipped native shell, then tail-chain real video BIOS.
; 1024 bytes is a stress model informed by VBE 3.0's protected-mode stack
; allowance and upstream AUXSTACK; it is not a measured T23 requirement.
bits 16
cpu 386
org 0
%ifndef STACK_BYTES
%define STACK_BYTES 1024
%endif
%ifndef SHELL_STACK_TOP
%error "Pass the actual shipped SHELL entry's stack pointer"
%endif
%if STACK_BYTES < 18 || STACK_BYTES > 8192 || STACK_BYTES & 1
%error "STACK_BYTES must be even and between 18 and 8192"
%endif
    db 0x55,0xAA,4
    jmp init
blob:
    db 'C10STACK'
    dd 0                       ; original INT10 vector, offset 8
    dd 0                       ; native shell call count, offset 12
    dw 0,0xffff,0xffff          ; caller CS, lowest entry SP, scratch bottom
    dw 0                       ; SS!=CS count
    dw STACK_BYTES
video:
    pushf
    push ax
    push cx
    push di
    push es
    push bp
    mov bp,sp
    mov ax,[ss:bp+14]           ; original CS, behind six saved words + IP
    mov es,ax
    cmp dword [es:0x100],0x8ec88cfa
    jne .other
    cmp word [es:0x104],0xbcd0
    jne .other
    cmp word [es:0x106],SHELL_STACK_TOP
    jne .other
    inc dword [cs:12]
    mov [cs:16],ax
    mov cx,ss
    cmp ax,cx
    je .same_stack
    inc word [cs:22]
.same_stack:
    lea ax,[bp+12]              ; entry SP, includes 6-byte interrupt frame
    cmp ax,[cs:18]
    jae .scratch
    mov [cs:18],ax
.scratch:
    push ss
    pop es
    sub sp,STACK_BYTES-18
    mov di,sp
    cmp di,[cs:20]
    jae .fill
    mov [cs:20],di
.fill:
    mov cx,(STACK_BYTES-18)/2
    mov ax,0xA55A
    cld
    rep stosw
    mov sp,bp
.other:
    pop bp
    pop es
    pop di
    pop cx
    pop ax
    popf
    jmp far [cs:8]
blob_end:
init:
    pushf
    pushad
    push ds
    push es
    cli
    xor ax,ax
    mov ds,ax
    dec word [0x413]
    mov ax,[0x413]
    shl ax,6
    mov es,ax
    push cs
    pop ds
    mov si,blob
    xor di,di
    mov cx,blob_end-blob
    cld
    rep movsb
    xor ax,ax
    mov ds,ax
    mov eax,[0x10*4]
    mov [es:8],eax
    mov word [0x10*4],video-blob
    mov [0x10*4+2],es
    pop es
    pop ds
    popad
    popf
    retf
    times 2047-($-$$) db 0
    db 0                       ; ROM checksum patched by runner
