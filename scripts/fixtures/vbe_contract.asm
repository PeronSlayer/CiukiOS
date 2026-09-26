; Real INT 10h calls: VBE register preservation and banked VRAM round-trip.
bits 16
cpu 386
org 0x100
    cli
    mov ax,cs
    mov ss,ax
    mov sp,end_image
    sti
    mov bx,(end_image-$$+0x100+15)/16
    mov ah,0x4A
    int 0x21
    mov ax,0x4F02
    mov bx,0x103
    mov dx,0xBEEF
    int 0x10
    cmp ax,0x004F
    jne failed
    cmp dx,0xBEEF
    jne bad_dx
    mov ax,0x4F05
    xor bx,bx                 ; BH=set, BL=window A
    mov dx,2
    int 0x10
    cmp ax,0x004F
    jne failed
    mov ax,0xA000
    mov es,ax
    mov byte [es:0],0x5A
    mov ax,0x4F05
    mov bx,0x0100             ; BH=get, BL=window A
    mov dx,0xCAFE
    int 0x10
    cmp ax,0x004F
    jne failed
    cmp dx,2
    jne failed
    mov ax,0x4F05
    xor bx,bx
    xor dx,dx
    int 0x10
    cmp ax,0x004F
    jne failed
    mov byte [es:0],0xA5
    mov ax,0x4F05
    xor bx,bx
    mov dx,2
    int 0x10
    cmp ax,0x004F
    jne failed
    cmp byte [es:0],0x5A
    jne failed
    mov dx,ok
    jmp report
bad_dx:
    mov dx,dx_error
    jmp report
failed:
    mov dx,bank_error
report:
    push dx
    mov ax,3
    int 0x10
    push cs
    pop ds
    pop dx
    mov ah,9
    int 0x21
    mov ax,0x4C00
    int 0x21
ok db '[VBE-CHECK] PASS DX preserved, bank 2 queried, VRAM retained',13,10,'$'
dx_error db '[VBE-CHECK] FAIL mode set changed DX',13,10,'$'
bank_error db '[VBE-CHECK] FAIL bank API or VRAM',13,10,'$'
    times 4096 db 0
end_image:
