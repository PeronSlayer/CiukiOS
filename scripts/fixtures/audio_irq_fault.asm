; Test-only PCI BIOS fault: leave IRQ0/IRQ1/IRQ12 masked and IF cleared.
; The startup player must time out even when the BIOS tick stops advancing,
; restore its caller's IRQ masks, and return to working keyboard/mouse input.
bits 16
cpu 386
org 0x100
    jmp start
old_time dd 0
armed db 0
timer:
    cmp ax,0xB101
    je .arm
    cmp ah,0
    jne .chain
    cmp byte [cs:armed],1
    je .fault
.chain:
    jmp far [cs:old_time]
.arm:
    ; Preserve the real PCI return status. Mask IRQs only on the first clock
    ; query after discovery, when the player has entered codec initialization.
    mov byte [cs:armed],1
    jmp far [cs:old_time]
.fault:
    mov byte [cs:armed],0
    pushf
    call far [cs:old_time]
    push ax
    mov al,'T'
    out 0xE9,al
    in al,0x21
    or al,3
    out 0x21,al
    in al,0xA1
    or al,0x10
    out 0xA1,al
    pop ax
    push bp
    mov bp,sp
    and word [ss:bp+6],0xFDFF
    pop bp
    iret
start:
    mov ax,0x351A
    int 0x21
    mov [old_time],bx
    mov [old_time+2],es
    mov ax,0x251A
    mov dx,timer
    int 0x21
    mov dx,message
    mov ah,9
    int 0x21
    mov dx,((image_end-$$+0x100)+15)/16
    mov ax,0x3100
    int 0x21
message db '[AUDIO-FAULT] installed',13,10,'$'
image_end:
