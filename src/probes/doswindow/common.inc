; Shared real-mode helpers. No kernel-private calls or window-manager hooks.
; BIOS_ONLY=1 emits no B8000 access; otherwise put_cell writes that aperture.
serial_char:
    push ax
    push cx
    push dx
    out 0xE9, al
    mov ah, al
    mov cx, 2048
    mov dx, 0x3FD
.wait:
    in al, dx
    test al, 0x20
    jnz .send
    loop .wait
    jmp .done
.send:
    mov dx, 0x3F8
    mov al, ah
    out dx, al
.done:
    pop dx
    pop cx
    pop ax
    ret

serial_string:
    push ax
    push si
.next:
    lodsb
    test al, al
    jz .done
    call serial_char
    jmp .next
.done:
    pop si
    pop ax
    ret

serial_hex:
    push ax
    push bx
    push cx
    mov bx, ax
    mov cx, 4
.digit:
    rol bx, 4
    mov al, bl
    and al, 15
    add al, '0'
    cmp al, '9'
    jbe .emit
    add al, 7
.emit:
    call serial_char
    loop .digit
    pop cx
    pop bx
    pop ax
    ret

serial_newline:
    push ax
    mov al, 13
    call serial_char
    mov al, 10
    call serial_char
    pop ax
    ret

; Return the actual BIOS timer in EAX. The caller handles the midnight wrap.
read_ticks:
    push cx
    push dx
    push ds
    push es
    xor ah, ah
    int 0x1A
    movzx eax, cx
    shl eax, 16
    mov ax, dx
    pop es
    pop ds
    pop dx
    pop cx
    ret

; DI is a byte offset in the logical 80-column text page; AX=attribute/char.
; Preserves registers. BIOS variant uses only standard cursor/write functions.
put_cell:
    push ax
    push bx
    push cx
    push dx
    push di
    push es
%if BIOS_ONLY
    mov bx, ax
    mov ax, di
    xor dx, dx
    mov cx, 160
    div cx
    mov dh, al
    shr dl, 1
    push bx
    xor bh, bh
    mov ah, 0x02
    int 0x10
    pop bx
    mov ax, bx
    mov bl, ah
    xor bh, bh
    mov ah, 0x09
    mov cx, 1
    int 0x10
%else
    mov bx, 0xB800
    mov es, bx
    mov [es:di], ax
%endif
    pop es
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; DS:SI ASCIIZ, AH attribute, DI text byte offset.
screen_string:
    push ax
    push si
    push di
.next:
    lodsb
    test al, al
    jz .done
    call put_cell
    add di, 2
    jmp .next
.done:
    pop di
    pop si
    pop ax
    ret

; AX number, DI text byte offset, fixed bright-green text attribute.
screen_hex:
    push ax
    push bx
    push cx
    push di
    mov bx, ax
    mov cx, 4
.digit:
    rol bx, 4
    mov al, bl
    and al, 15
    add al, '0'
    cmp al, '9'
    jbe .emit
    add al, 7
.emit:
    mov ah, 0x1A
    call put_cell
    add di, 2
    loop .digit
    pop di
    pop cx
    pop bx
    pop ax
    ret
