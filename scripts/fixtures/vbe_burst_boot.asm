bits 16
org 0x7C00
    cli
    xor ax,ax
    mov ss,ax
    mov sp,0x7C00
    mov ds,ax
    mov ax,0x1000
    mov es,ax
    mov bx,0x100
    mov ax,0x0211
    mov cx,2
    xor dh,dh
    sti
    int 0x13
    jc $
    jmp 0x1000:0x100
times 510-($-$$) db 0
dw 0xAA55
