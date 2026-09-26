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

    ; /LEAK is a negative interoperability fixture: it deliberately exits
    ; without unregistering the callback so the Windows same-boot test can
    ; prove that normal EXEC return releases the transient client before
    ; WIN386 takes exclusive BIOS ownership.  Normal invocations retain the
    ; full stress test below.
    mov si, 0x0081
    xor cx, cx
    mov cl, [0x0080]
.arg_skip_spaces:
    jcxz .arg_done
    lodsb
    dec cx
    cmp al, ' '
    je .arg_skip_spaces
    cmp al, '/'
    jne .arg_done
    cmp cx, 4
    jb .arg_done
    lodsb
    and al, 0xDF
    cmp al, 'L'
    jne .arg_done
    lodsb
    and al, 0xDF
    cmp al, 'E'
    jne .arg_done
    lodsb
    and al, 0xDF
    cmp al, 'A'
    jne .arg_done
    lodsb
    and al, 0xDF
    cmp al, 'K'
    jne .arg_done
    mov byte [leak_mode], 1
.arg_done:

    xor ax, ax
    int 0x33
    cmp ax, 0xFFFF
    jne mouse_unavailable

    cmp byte [leak_mode], 1
    je install_leaked_callback

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
    mov byte [delay_first_callback], 1
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

install_leaked_callback:
    push cs
    pop es
    mov ax, 0x000C
    mov cx, 0x0001                 ; movement callback only
    mov dx, mouse_event_callback
    int 0x33
    mov dx, msg_leak_ready
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00                 ; deliberately do not unregister
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
    ; Model a real GUI callback that is still drawing while more hardware
    ; packets arrive.  Timer/keyboard IRQs remain enabled by the resident
    ; driver, so the following delay lets the test inject a nested IRQ12 and
    ; verifies that the pending event is drained after this callback returns.
    cmp byte [cs:delay_first_callback], 0
    je .record
    mov byte [cs:delay_first_callback], 0
    push ax
    push bx
    push es
    mov ax, 0x0040
    mov es, ax
    mov bx, [es:0x006C]
.delay:
    sti
    hlt
    mov ax, [es:0x006C]
    sub ax, bx
    cmp ax, 6                     ; keep callback busy across press + release
    jb .delay
    pop es
    pop bx
    pop ax
.record:
    or [cs:events_seen], ax
    mov [cs:last_event_mask], ax
    mov [cs:last_x], cx
    mov [cs:last_y], dx
    ; Coalescing may report press+release together.  BX is the final button
    ; state, so a combined transition must be released; an isolated press must
    ; be down.
    test ax, 0x0004
    jnz .expect_released
    test ax, 0x0002
    jz .done
    test bx, 0x0001
    jnz .done
    mov byte [cs:callback_error], 1
    jmp .done
.expect_released:
    test bx, 0x0001
    jz .done
    mov byte [cs:callback_error], 1
.done:
    retf

events_seen dw 0
callback_error db 0
delay_first_callback db 0
leak_mode db 0
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

msg_ready db '[MOUSECB] READY NESTED_IRQ_STRESS', 13, 10, '$'
msg_leak_ready db '[MOUSECB] READY STALE_CALLBACK_FIXTURE', 13, 10, '$'
msg_pass db '[MOUSECB] PASS', 13, 10, '$'
msg_timeout db '[MOUSECB] FAIL TIMEOUT', 13, 10, '$'
msg_bad_event db '[MOUSECB] FAIL BUTTON_STATE', 13, 10, '$'
msg_bad_counter db '[MOUSECB] FAIL BUTTON_COUNTER', 13, 10, '$'
msg_missing db '[MOUSECB] FAIL NO_DRIVER', 13, 10, '$'
