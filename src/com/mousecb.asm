bits 16
org 0x0100

; INT 33h callback regression probe.
;
; A conforming DOS mouse driver invokes function 000Ch handlers as FAR
; procedures.  The handler below deliberately returns with RETF, just like a
; normal DOS application callback, so this probe detects a corrupted callback
; frame instead of merely checking that the interrupt vector exists.

start:
    push cs
    pop ds

    xor ax, ax
    int 0x33
    cmp ax, 0xFFFF
    jne mouse_unavailable

    ; Exercise the standard custom graphics cursor entry point as well.  ES:DX
    ; is application-owned source memory and must never be used as a target.
    push cs
    pop es
    mov ax, 0x0009
    xor bx, bx
    xor cx, cx
    mov dx, graphics_cursor_mask
    int 0x33

    mov word [events_seen], 0
    mov byte [callback_error], 0
    push cs
    pop es
    mov ax, 0x000C
    mov cx, 0x0007                 ; movement + left press + left release
    mov dx, mouse_event_callback
    int 0x33

    mov dx, msg_ready
    mov ah, 0x09
    int 0x21

    xor ah, ah
    int 0x1A
    mov [start_tick], dx

.wait_event:
    mov ax, [events_seen]
    and ax, 0x0007
    cmp ax, 0x0007
    je .event_received
    sti
    hlt
    xor ah, ah
    int 0x1A
    sub dx, [start_tick]
    cmp dx, 182                    ; about ten seconds at 18.2 Hz
    jb .wait_event

    call unregister_callback
    mov dx, msg_timeout
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C01
    int 0x21

.event_received:
    call unregister_callback
    cmp byte [callback_error], 0
    jne .bad_event

    ; Functions 0005h/0006h must return the accumulated count once and clear
    ; exactly that button's counter.  DOSNavigator uses this polling API in
    ; addition to callbacks, so a counter which remains set behaves like an
    ; endless click.
    mov ax, 0x0005
    xor bx, bx
    int 0x33
    or bx, bx
    jz .bad_counter
    mov ax, 0x0005
    xor bx, bx
    int 0x33
    or bx, bx
    jnz .bad_counter
    mov ax, 0x0006
    xor bx, bx
    int 0x33
    or bx, bx
    jz .bad_counter
    mov ax, 0x0006
    xor bx, bx
    int 0x33
    or bx, bx
    jnz .bad_counter

    mov dx, msg_pass
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21

.bad_event:
    mov dx, msg_bad_event
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C03
    int 0x21

.bad_counter:
    mov dx, msg_bad_counter
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C04
    int 0x21

mouse_unavailable:
    mov dx, msg_missing
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C02
    int 0x21

unregister_callback:
    push ax
    push cx
    push dx
    push es
    push cs
    pop es
    mov ax, 0x000C
    xor cx, cx
    xor dx, dx
    int 0x33
    pop es
    pop dx
    pop cx
    pop ax
    ret

mouse_event_callback:
    or [cs:events_seen], ax
    mov [cs:last_event_mask], ax
    mov [cs:last_x], cx
    mov [cs:last_y], dx
    test ax, 0x0002
    jz .check_release
    test bx, 0x0001
    jnz .check_release
    mov byte [cs:callback_error], 1
.check_release:
    test ax, 0x0004
    jz .done
    test bx, 0x0001
    jz .done
    mov byte [cs:callback_error], 1
.done:
    retf

events_seen dw 0
callback_error db 0
start_tick dw 0
last_event_mask dw 0
last_x dw 0
last_y dw 0

; 16-word screen mask followed by a 16-word cursor mask.
graphics_cursor_mask:
    dw 0xFFFF, 0x7FFF, 0x3FFF, 0x1FFF
    dw 0x0FFF, 0x07FF, 0x03FF, 0x01FF
    dw 0x00FF, 0x007F, 0x003F, 0x001F
    dw 0x000F, 0x0007, 0x0003, 0x0001
    dw 0x0000, 0x4000, 0x6000, 0x7000
    dw 0x7800, 0x7C00, 0x7E00, 0x7F00
    dw 0x7F80, 0x7C00, 0x6C00, 0x4600
    dw 0x0600, 0x0300, 0x0300, 0x0000

msg_ready db '[MOUSECB] READY', 13, 10, '$'
msg_pass db '[MOUSECB] PASS', 13, 10, '$'
msg_timeout db '[MOUSECB] FAIL TIMEOUT', 13, 10, '$'
msg_bad_event db '[MOUSECB] FAIL BUTTON_STATE', 13, 10, '$'
msg_bad_counter db '[MOUSECB] FAIL BUTTON_COUNTER', 13, 10, '$'
msg_missing db '[MOUSECB] FAIL NO_DRIVER', 13, 10, '$'
