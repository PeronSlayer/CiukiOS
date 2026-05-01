bits 16
org 0x0100

start:
    mov ax, 0x0013
    int 0x10

    mov ax, 0xA000
    mov es, ax

    xor di, di
    xor ax, ax
    mov cx, 32000
    rep stosw

    mov bx, 0
    mov dx, 0
    mov cx, 320
    mov si, 100
    mov al, 0x01
    call fill_rect

    mov bx, 0
    mov dx, 100
    mov cx, 320
    mov si, 100
    mov al, 0x02
    call fill_rect

    mov bx, 90
    mov dx, 60
    mov cx, 140
    mov si, 80
    mov al, 0x0C
    call fill_rect

    mov bx, 120
    mov dx, 80
    mov cx, 80
    mov si, 40
    mov al, 0x0E
    call fill_rect

    mov ax, 0x0003
    int 0x10

    mov ax, 0x4C71
    int 0x21

fill_rect:
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    mov bp, cx

.row:
    push ax
    call calc_offset
    pop ax
    mov cx, bp
    rep stosb
    inc dx
    dec si
    jnz .row

    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

calc_offset:
    mov ax, dx
    shl ax, 8
    mov di, ax
    mov ax, dx
    shl ax, 6
    add di, ax
    add di, bx
    ret
