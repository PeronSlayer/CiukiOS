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

    mov bx, 150
    mov dx, 90
    mov cx, 20
    mov si, 20
    mov al, 0x0E
    call fill_rect

    mov bx, 158
    mov dx, 40
    mov cx, 4
    mov si, 50
    mov al, 0x0C
    call fill_rect

    mov bx, 158
    mov dx, 110
    mov cx, 4
    mov si, 50
    mov al, 0x0C
    call fill_rect

    mov bx, 100
    mov dx, 98
    mov cx, 58
    mov si, 4
    mov al, 0x0C
    call fill_rect

    mov bx, 162
    mov dx, 98
    mov cx, 58
    mov si, 4
    mov al, 0x0C
    call fill_rect

    mov cx, 41
    xor si, si
.diag:
    mov bx, 120
    add bx, si
    mov dx, 60
    add dx, si
    mov al, 0x0A
    call plot_pixel

    mov bx, 200
    sub bx, si
    mov dx, 60
    add dx, si
    mov al, 0x0A
    call plot_pixel

    mov bx, 120
    add bx, si
    mov dx, 140
    sub dx, si
    mov al, 0x0A
    call plot_pixel

    mov bx, 200
    sub bx, si
    mov dx, 140
    sub dx, si
    mov al, 0x0A
    call plot_pixel

    inc si
    loop .diag

    mov ax, 0x0003
    int 0x10

    mov ax, 0x4C72
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

plot_pixel:
    push di
    push ax
    call calc_offset
    pop ax
    mov [es:di], al
    pop di
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
