bits 16
org 0x0100

; Minimal OpenGEM stub for Phase 3 testing
; Usage: OPENGEM.COM or loaded via INT21/AH=4Bh
; Provides minimal desktop with VDI wrapper and INT33h mouse support

entry:
    mov ax, cs
    mov ds, ax
    mov es, ax

    mov si, msg_opengem_startup
    call print_string

    ; Set VGA mode 13h (320x200, 256 colors)
    mov ax, 0x0013
    int 0x10

    ; Initialize minimal desktop
    call draw_opengem_desktop

    ; Install minimal event loop
    call opengem_event_loop

    ; Return to DOS
    mov ax, 0x4C00
    int 0x21

draw_opengem_desktop:
    ; Draw window frame (top menu bar)
    mov ax, 0
    mov bx, 0
    mov cx, 319
    mov dx, 19
    mov byte [gfx_draw_color], 0x0F
    call draw_rectangle

    ; Draw desktop (turquoise background)
    mov byte [gfx_draw_color], 0x03
    xor ax, ax
    xor bx, bx
    call vdi_clear_screen

    ; Draw status bar at bottom
    mov ax, 0
    mov bx, 190
    mov cx, 319
    mov dx, 199
    mov byte [gfx_draw_color], 0x0F
    call draw_rectangle

    ; Print desktop labels
    mov ax, 10
    mov bx, 5
    mov dl, 0x0F
    mov si, msg_opengem_menu
    call vdi_gtext

    mov ax, 10
    mov bx, 192
    mov dl, 0x0F
    mov si, msg_opengem_status
    call vdi_gtext

    ret

opengem_event_loop:
    mov al, 0                   ; Max 3 seconds of loop
    mov byte [countdown], 30
.loop:
    ; Check keyboard
    mov ah, 0x01
    int 0x16
    jz .check_mouse

    ; Key pressed - exit
    mov ax, 0x4C00
    int 0x21

.check_mouse:
    ; INT33h AH=3 - get mouse status
    mov ax, 0x0003
    int 0x33
    ; BX = button state, CX = X, DX = Y

    dec byte [countdown]
    jnz .loop

    ret

draw_rectangle:
    ; Simple rectangle: AX=x0, BX=y0, CX=x1, DX=y1
    push ax
    push bx
    push cx
    push dx

    ; Fill using VDI bar (simplified)
    call vdi_bar

    pop dx
    pop cx
    pop bx
    pop ax
    ret

vdi_gtext:
    ; Placeholder for graphics text (stub)
    ret

vdi_bar:
    ; Placeholder for VDI bar fill (stub)
    ret

vdi_clear_screen:
    ; Clear VGA buffer
    xor ax, ax
    mov es, ax
    mov es, 0xA000
    xor di, di
.clear_loop:
    cmp di, 0x8000
    jae .clear_done
    mov byte [es:di], 0x00
    inc di
    jmp .clear_loop
.clear_done:
    ret

print_string:
    ; Minimal string print for debugging
    lodsb
    test al, al
    jz .done
    mov ah, 0x02
    mov dl, al
    int 0x21
    jmp print_string
.done:
    ret

msg_opengem_startup db "OpenGEM stub initializing...", 13, 10, 0
msg_opengem_menu db "File  Edit  View  Window  Help", 0
msg_opengem_status db "Ready", 0

gfx_draw_color db 0
countdown db 0
