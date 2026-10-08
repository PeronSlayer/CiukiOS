; Unchanging DOSVM client: no animation can conceal a compositor overwrite.
bits 16
org 100h
start:
    mov ax,13h
    int 10h
    mov ax,0A000h
    mov es,ax
    xor di,di
    xor dx,dx
.row:
    xor bx,bx
.pixel:
    mov ax,bx
    shr ax,4
    mov cx,dx
    shr cx,4
    add ax,cx
    and al,15
    stosb
    inc bx
    cmp bx,320
    jb .pixel
    inc dx
    cmp dx,200
    jb .row
    mov dx,ready
    mov ah,9
    int 21h
.idle:
    xor ax,ax
    int 16h
    cmp al,27
    jne .idle
    mov ax,4C00h
    int 21h
ready db '[OVERLAY-GUEST] static VGA ready',13,10,'$'
