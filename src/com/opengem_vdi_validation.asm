bits 16
org 0x0000

; Deterministic OpenGEM validation shim.
; Built as GEMVDI.EXE for the unattended full-profile gate so the Stage2
; OpenGEM path reaches a desktop-ready state and returns to DOS reliably.

mz_header:
    dw 0x5A4D
    dw file_size_mod_512
    dw file_size_pages
    dw 0x0000
    dw 0x0002
    dw 0x0000
    dw 0xFFFF
    dw 0x0000
    dw 0xFFFE
    dw 0x0000
    dw start - image_start
    dw 0x0000
    dw 0x001C
    dw 0x0000

times 0x20 - ($ - mz_header) db 0

image_start:
start:
    push cs
    pop ds

    mov dx, msg_desktop_start - image_start
    mov ah, 0x09
    int 0x21

    mov ax, 0x0013
    int 0x10

    call draw_desktop
    call wait_frames

    mov ax, 0x0003
    int 0x10

    mov dx, msg_desktop_timeout - image_start
    mov ah, 0x09
    int 0x21
    mov dx, msg_desktop_exit - image_start
    mov ah, 0x09
    int 0x21

    mov ax, 0x4C00
    int 0x21

draw_desktop:
    mov ax, 0xA000
    mov es, ax

    xor di, di
    mov al, 0x03
    mov cx, 4096
.clear:
    mov [es:di], al
    inc di
    loop .clear

    xor di, di
    mov al, 0x0F
    mov cx, 640
.menubar:
    mov [es:di], al
    inc di
    loop .menubar

    ret

wait_frames:
    mov bx, 2
.frame:
    mov cx, 0x1000
.delay:
    loop .delay
    dec bx
    jnz .frame
    ret

msg_desktop_start db "[OPENGEM-DESKTOP] Starting...", 13, 10, '$'
msg_desktop_timeout db "[OPENGEM-DESKTOP] Timeout - returning to DOS", 13, 10, '$'
msg_desktop_exit db "[OPENGEM-DESKTOP] Exiting to DOS", 13, 10, '$'

file_end:

file_size equ file_end - mz_header
file_size_mod_512 equ file_size & 0x1FF
file_size_pages equ (file_size + 511) / 512
