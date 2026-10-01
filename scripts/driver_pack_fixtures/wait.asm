; WAIT.COM - test fixture: announces itself on COM1 and waits about 12
; seconds (BIOS ticks, interrupts enabled) so a gate can exercise a driver
; loaded just before it, before the memory manager starts.
bits 16
org 0x100
    mov dx,msg
    mov ah,9
    int 0x21
    push 0x40
    pop es
    mov bx,[es:0x6C]
.wait:
    sti
    hlt
    mov ax,[es:0x6C]
    sub ax,bx
    cmp ax,218
    jb .wait
    mov ax,0x4C00
    int 0x21
msg db '[FIXTURE] WAIT start',13,10,'$'
