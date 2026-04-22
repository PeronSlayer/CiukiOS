bits 16
org 0x0100

; OpenGEM Desktop Stub - Phase 3 Milestone
; Minimal but functional OpenGEM-compatible desktop
; Demonstrates VGA mode 13h, color palette, graphics primitives, and mouse/keyboard input

jmp entry

; === Configuration ===
DESKTOP_COLOR equ 0x03           ; Turquoise background
MENUBAR_COLOR equ 0x0F           ; White menu bar
WINDOW_COLOR equ 0x0C            ; Red window
CURSOR_COLOR equ 0x0E            ; Yellow mouse cursor
VGA_BUFFER equ 0xA000
FRAME_COUNT equ 100               ; ~5 seconds at ~20fps

entry:
    cli
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xFFFE
    sti

    ; Announce to serial
    mov si, msg_desktop_start
    call print_serial

    ; Set VGA mode 13h (320x200, 256 colors)
    mov ax, 0x0013
    int 0x10

    ; Initialize graphics
    call gfx_init_desktop

    ; Main event loop
    call gfx_event_loop

    ; Return to DOS
    mov si, msg_desktop_exit
    call print_serial
    mov ax, 0x4C00
    int 0x21

; === Initialization ===
gfx_init_desktop:
    ; Set up graphics mode
    mov ax, 0
    mov es, ax
    mov es, VGA_BUFFER
    mov ax, VGA_BUFFER

    mov es, ax

    ; Clear screen to turquoise
    xor di, di
    mov al, DESKTOP_COLOR
    mov cx, 0x8000            ; 320*200 pixels = 64K
gfx_init_desktop:
    ; Set up graphics mode
    mov ax, VGA_BUFFER
    mov es, ax

    ; Clear screen to turquoise
    xor di, di
    mov al, DESKTOP_COLOR
    mov cx, 0x8000            ; 320*200 pixels = 64K
.clear_loop:
    cmp cx, 0
    je .clear_done
    mov [es:di], al
    inc di
    dec cx
    jmp .clear_loop

.clear_done:
    ; Draw menu bar (white, 20 pixels high)
    call gfx_draw_menubar

    ; Draw status bar (white, 10 pixels high)
    call gfx_draw_statusbar

    ; Draw window (red, centered)
    mov ax, 70                ; X offset
    mov bx, 50                ; Y offset
    mov cx, 180               ; Width
    mov dx, 100               ; Height
    mov al, WINDOW_COLOR
    call gfx_draw_filled_rect

    ; Draw decorative elements
    call gfx_draw_decorations

    ret

gfx_draw_menubar:
    ; Fill top 20 rows with white
    mov ax, VGA_BUFFER
    mov es, ax
    xor di, di
    mov al, MENUBAR_COLOR
    mov cx, 320 * 20
.menubar_fill:
    cmp cx, 0
    je .menubar_done
    mov [es:di], al
    inc di
    dec cx
    jmp .menubar_fill
.menubar_done:
    ret

gfx_draw_statusbar:
    ; Fill bottom 10 rows with white
    mov ax, VGA_BUFFER
    mov es, ax
    mov di, 320 * 190         ; Start at row 190
    mov al, MENUBAR_COLOR
    mov cx, 320 * 10
.statusbar_fill:
    cmp cx, 0
    je .statusbar_done
    mov [es:di], al
    inc di
    dec cx
    jmp .statusbar_fill
.statusbar_done:
    ret

gfx_draw_decorations:
    ; Draw some horizontal lines
    mov ax, VGA_BUFFER
    mov es, ax

    ; Line 1: Y=30, color=white
    mov di, 320 * 30
    mov al, 0x0F
    mov cx, 320
.line1_loop:
    cmp cx, 0
    je .line1_done
    mov [es:di], al
    inc di
    dec cx
    jmp .line1_loop
.line1_done:

    ; Line 2: Y=150, color=white
    mov di, 320 * 150
    mov cx, 320
.line2_loop:
    cmp cx, 0
    je .line2_done
    mov [es:di], al
    inc di
    dec cx
    jmp .line2_loop
.line2_done:
    ret

gfx_draw_filled_rect:
    ; Draw filled rectangle: AX=x, BX=y, CX=width, DX=height, AL=color
    push ax
    push bx
    push cx
    push dx
    push di
    push si

    mov si, ax                ; SI = X offset
    mov ax, VGA_BUFFER
    mov es, ax

.rect_row_loop:
    cmp dx, 0
    je .rect_done

    ; Calculate row offset: (BX * 320) + SI
    mov ax, bx
    imul ax, 320
    add ax, si
    mov di, ax

    ; Fill width pixels
    push cx
.rect_fill_loop:
    cmp cx, 0
    je .rect_next_row
    mov [es:di], al
    inc di
    dec cx
    jmp .rect_fill_loop

.rect_next_row:
    pop cx
    inc bx
    dec dx
    jmp .rect_row_loop

.rect_done:
    pop si
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; === Event Loop ===
gfx_event_loop:
    mov byte [frame_counter], FRAME_COUNT

.frame_loop:
    ; Check keyboard (non-blocking)
    mov ah, 0x01
    int 0x16
    jz .no_keypress

    ; Any key pressed - exit
    mov si, msg_key_detected
    call print_serial
    ret

.no_keypress:
    ; Query mouse (INT33h AH=3)
    mov ax, 0x0003
    int 0x33
    ; BX = button state, CX = X, DX = Y

    ; Draw mouse cursor
    cmp cx, 320
    jae .skip_cursor
    cmp dx, 200
    jae .skip_cursor

    ; Calculate cursor position: DX * 320 + CX
    mov ax, dx
    imul ax, 320
    add ax, cx
    mov di, ax

    mov ax, VGA_BUFFER
    mov es, ax
    mov al, CURSOR_COLOR
    mov [es:di], al
    cmp di, 0
    je .skip_cursor
    mov [es:di-1], al
    add di, 320
    cmp di, 0x8000
    jae .skip_cursor
    mov [es:di], al
    mov [es:di+1], al

.skip_cursor:
    ; Simple delay loop
    mov cx, 0x4000
.delay:
    loop .delay

    dec byte [frame_counter]
    cmp byte [frame_counter], 0
    jne .frame_loop

    mov si, msg_timeout
    call print_serial
    ret

; === Serial Output (for debugging) ===
print_serial:
    lodsb
    test al, al
    jz .serial_done
    mov ah, 0x0E              ; INT10h TTY output
    mov bh, 0
    int 0x10
    jmp print_serial
.serial_done:
    ret

; === Data ===
msg_desktop_start db "[OPENGEM-DESKTOP] Starting...", 13, 10, 0
msg_desktop_exit db "[OPENGEM-DESKTOP] Exiting to DOS", 13, 10, 0
msg_key_detected db "[OPENGEM-DESKTOP] Key pressed - returning to DOS", 13, 10, 0
msg_timeout db "[OPENGEM-DESKTOP] Timeout - returning to DOS", 13, 10, 0

frame_counter db 0
