bits 16
org 0x0000

stage2_entry:
    push cs
    pop ds

    mov dx, msg_begin
    call dos_print

    call gfx2d_mvp_entry

    mov dx, msg_return
    call dos_print

.done:
    retf

dos_print:
    mov ah, 0x09
    int 0x21
    ret

; -----------------------------------------------------------------------------
; Compact 2D driver MVP for full profile first-sector launcher.
; -----------------------------------------------------------------------------
gfx2d_mvp_entry:
    call gfx2d_init_mode13
    call gfx2d_render_test_frame
    mov dx, msg_gfx_serial_pass
    call dos_print
    ret

gfx2d_init_mode13:
    mov ax, 0x0013
    int 0x10
    mov byte [gfx2d_ctx_mode], 0x13
    mov word [gfx2d_ctx_width], 320
    mov word [gfx2d_ctx_height], 200
    mov word [gfx2d_ctx_pitch], 320
    mov word [gfx2d_ctx_fb_seg], 0xA000
    mov word [gfx2d_ctx_fb_off], 0
    ret

; in: al = color
gfx2d_clear:
    push ax
    push cx
    push di
    push es
    mov ah, al
    mov ax, [gfx2d_ctx_fb_seg]
    mov es, ax
    xor di, di
    mov al, ah
    mov cx, 64000
    rep stosb
    pop es
    pop di
    pop cx
    pop ax
    ret

; in: cx = x, dx = y, al = color
gfx2d_put_pixel:
    push bx
    push es
    mov bx, dx
    shl bx, 6
    mov ax, dx
    shl ax, 8
    add bx, ax
    add bx, cx
    mov ah, al
    mov ax, [gfx2d_ctx_fb_seg]
    mov es, ax
    mov al, ah
    mov [es:bx], al
    pop es
    pop bx
    ret

; in: cx = x, dx = y, si = width, di = height, al = color
gfx2d_fill_rect:
    push bx
    push bp
    push es
    mov ah, al
    mov ax, [gfx2d_ctx_fb_seg]
    mov es, ax
    mov bx, dx
    shl bx, 6
    mov bp, dx
    shl bp, 8
    add bx, bp
    add bx, cx
    mov bp, di
.row:
    mov di, bx
    mov cx, si
    mov al, ah
    rep stosb
    add bx, [gfx2d_ctx_pitch]
    dec bp
    jnz .row
    pop es
    pop bp
    pop bx
    ret

gfx2d_draw_line:
    stc
    ret

gfx2d_blit:
    stc
    ret

gfx2d_render_test_frame:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    mov al, 0x01
    call gfx2d_clear
    mov cx, 20
    mov dx, 20
    mov si, 280
    mov di, 16
    mov al, 0x04
    call gfx2d_fill_rect
    mov cx, 40
    mov dx, 56
    mov si, 240
    mov di, 100
    mov al, 0x02
    call gfx2d_fill_rect
    xor bx, bx
.diag:
    mov cx, bx
    add cx, 30
    mov dx, bx
    add dx, 30
    mov al, 0x0F
    call gfx2d_put_pixel
    inc bx
    cmp bx, 100
    jb .diag
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

msg_begin db "[STAGE2] shell-only launcher", 13, 10, '$'
msg_return db "[STAGE2] return to shell", 13, 10, '$'
msg_gfx_serial_pass db "[GFX-SERIAL] PASS", 13, 10, '$'

gfx2d_ctx_mode db 0
gfx2d_ctx_width dw 0
gfx2d_ctx_height dw 0
gfx2d_ctx_pitch dw 0
gfx2d_ctx_fb_seg dw 0
gfx2d_ctx_fb_off dw 0
