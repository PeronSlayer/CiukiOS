bits 16
org 0x0100

start:
    push cs
    pop ds

    mov ax, 0x0013
    int 0x10

    mov ax, 0xA000
    mov es, ax

    mov al, 0x07
    call clear_screen

    mov bx, 0
    mov dx, 0
    mov cx, 320
    mov si, 13
    mov al, 0x01
    call fill_rect

    mov bx, 0
    mov dx, 186
    mov cx, 320
    mov si, 14
    mov al, 0x08
    call fill_rect

    mov bx, 9
    mov dx, 25
    mov cx, 302
    mov si, 129
    mov al, 0x00
    call fill_rect

    mov bx, 8
    mov dx, 24
    mov cx, 302
    mov si, 129
    mov al, 0x0F
    call fill_rect

    mov bx, 10
    mov dx, 26
    mov cx, 298
    mov si, 125
    mov al, 0x07
    call fill_rect

    mov bx, 10
    mov dx, 26
    mov cx, 298
    mov si, 12
    mov al, 0x01
    call fill_rect

    mov bx, 18
    mov dx, 47
    mov cx, 132
    mov si, 88
    mov al, 0x00
    call fill_rect

    mov bx, 17
    mov dx, 46
    mov cx, 132
    mov si, 88
    mov al, 0x0F
    call fill_rect

    mov bx, 19
    mov dx, 48
    mov cx, 128
    mov si, 84
    mov al, 0x07
    call fill_rect

    mov bx, 19
    mov dx, 48
    mov cx, 128
    mov si, 11
    mov al, 0x04
    call fill_rect

    mov bx, 169
    mov dx, 47
    mov cx, 120
    mov si, 58
    mov al, 0x00
    call fill_rect

    mov bx, 168
    mov dx, 46
    mov cx, 120
    mov si, 58
    mov al, 0x0F
    call fill_rect

    mov bx, 170
    mov dx, 48
    mov cx, 116
    mov si, 54
    mov al, 0x07
    call fill_rect

    mov bx, 170
    mov dx, 48
    mov cx, 116
    mov si, 10
    mov al, 0x01
    call fill_rect

    call draw_icons
    call draw_cursor
    call draw_text

    xor ah, ah
    int 0x16

    mov ax, 0x0003
    int 0x10

    mov ax, 0x4C00
    int 0x21

clear_screen:
    xor di, di
    mov ah, al
    mov cx, 32000
    rep stosw
    ret

fill_rect:
    push ax
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
    pop ax
    ret

plot_pixel:
    push ax
    push di
    call calc_offset
    pop di
    pop ax
    mov [es:di], al
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

draw_icons:
    mov bx, 31
    mov dx, 72
    mov cx, 18
    mov si, 15
    mov al, 0x0E
    call fill_rect

    mov bx, 34
    mov dx, 76
    mov cx, 12
    mov si, 8
    mov al, 0x0F
    call fill_rect

    mov bx, 75
    mov dx, 72
    mov cx, 18
    mov si, 15
    mov al, 0x0B
    call fill_rect

    mov bx, 79
    mov dx, 75
    mov cx, 10
    mov si, 9
    mov al, 0x01
    call fill_rect

    mov bx, 119
    mov dx, 72
    mov cx, 18
    mov si, 15
    mov al, 0x0C
    call fill_rect

    mov bx, 123
    mov dx, 76
    mov cx, 10
    mov si, 7
    mov al, 0x0F
    call fill_rect
    ret

draw_cursor:
    mov cx, 14
    xor si, si
.cursor_row:
    push cx
    mov cx, si
    inc cx
    mov bx, 246
    add bx, si
    mov dx, 122
    add dx, si
.cursor_col:
    mov al, 0x00
    call plot_pixel
    inc bx
    loop .cursor_col
    inc si
    pop cx
    loop .cursor_row
    ret

draw_text:
    mov dh, 0
    mov dl, 11
    mov bl, 0x0F
    mov si, txt_title
    call puts_at

    mov dh, 3
    mov dl, 3
    mov bl, 0x0F
    mov si, txt_pm
    call puts_at

    mov dh, 5
    mov dl, 3
    mov bl, 0x0F
    mov si, txt_menu
    call puts_at

    mov dh, 6
    mov dl, 22
    mov bl, 0x0F
    mov si, txt_about
    call puts_at

    mov dh, 8
    mov dl, 22
    mov bl, 0x0F
    mov si, txt_about_body1
    call puts_at

    mov dh, 9
    mov dl, 22
    mov bl, 0x0F
    mov si, txt_about_body2
    call puts_at

    mov dh, 11
    mov dl, 4
    mov bl, 0x00
    mov si, txt_main
    call puts_at

    mov dh, 11
    mov dl, 9
    mov bl, 0x00
    mov si, txt_files
    call puts_at

    mov dh, 11
    mov dl, 14
    mov bl, 0x00
    mov si, txt_setup
    call puts_at

    mov dh, 23
    mov dl, 1
    mov bl, 0x0F
    mov si, txt_status
    call puts_at
    ret

puts_at:
    push ax
    push bx
    push dx

    mov ah, 0x02
    xor bh, bh
    int 0x10

.next:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    xor bh, bh
    int 0x10
    jmp .next

.done:
    pop dx
    pop bx
    pop ax
    ret

txt_title db 'CiukiOS Desktop Preview', 0
txt_pm db 'Program Manager', 0
txt_menu db 'File  Options  Window  Help', 0
txt_about db 'About CiukiOS', 0
txt_about_body1 db 'GUI concept', 0
txt_about_body2 db 'Win 3.x mood', 0
txt_main db 'Main', 0
txt_files db 'Files', 0
txt_setup db 'Setup', 0
txt_status db 'Any key: return to shell', 0
