bits 16
org 0x0100

start:
    cld
    push cs
    pop ds
    push cs
    pop es

    mov dx, msg_begin
    call print_line

    mov si, dsp_base_list
    mov cx, 4

.probe_next:
    mov bx, [si]
    push si
    call print_probe
    pop si
    push si
    call dsp_reset_handshake
    pop si
    jnc .found
    add si, 2
    loop .probe_next

    mov dx, msg_not_found
    call print_line
    mov ax, 0x4C01
    int 0x21

.found:
    call print_found

    call configure_sb16_platform
    mov dx, msg_cfg_done
    call print_line

    mov dx, msg_tone_begin
    call print_line
    call play_tone
    jc .tone_fail

    mov dx, msg_tone_done
    call print_line
    mov dx, msg_done
    call print_line
    mov ax, 0x4C00
    int 0x21

.tone_fail:
    mov dx, msg_tone_fail
    call print_line
    mov ax, 0x4C02
    int 0x21

; IN: BX = DSP base port
; OUT: CF clear on reset ACK (0xAA), set on timeout/mismatch
dsp_reset_handshake:
    push ax
    push cx
    push dx

    mov dx, bx
    add dx, 0x06
    mov al, 0x01
    out dx, al
    call delay_reset
    xor al, al
    out dx, al
    call delay_reset

    mov dx, bx
    add dx, 0x0E
    mov cx, 0xFFFF

.wait_ready:
    in al, dx
    test al, 0x80
    jnz .read_ack
    loop .wait_ready
    stc
    jmp .done

.read_ack:
    mov dx, bx
    add dx, 0x0A
    in al, dx
    cmp al, 0xAA
    jne .ack_fail
    clc
    jmp .done

.ack_fail:
    stc

.done:
    pop dx
    pop cx
    pop ax
    ret

; IN: BX = DSP base port
; OUT: CF clear on success, set on write timeout
play_tone:
    push ax
    push cx

    mov al, 0xD1
    call dsp_write_byte
    jc .fail

    mov cx, 700
.tone_loop:
    mov al, 0x10
    call dsp_write_byte
    jc .fail
    mov al, 0x20
    call dsp_write_byte
    jc .fail

    mov al, 0x10
    call dsp_write_byte
    jc .fail
    mov al, 0xE0
    call dsp_write_byte
    jc .fail

    call delay_tone
    loop .tone_loop

    mov al, 0xD3
    call dsp_write_byte
    jc .fail

    clc
    jmp .done

.fail:
    stc

.done:
    pop cx
    pop ax
    ret

; Configure the QEMU SB16-compatible mixer and legacy PIC/DMA masks.
configure_sb16_platform:
    push ax
    push dx

    mov dx, bx
    add dx, 0x04
    mov al, 0x80
    out dx, al
    inc dx
    mov al, 0x04
    out dx, al

    mov dx, bx
    add dx, 0x04
    mov al, 0x81
    out dx, al
    inc dx
    mov al, 0x22
    out dx, al

    in al, 0x21
    and al, 0x7F
    out 0x21, al

    mov al, 0x01
    out 0x0A, al

    pop dx
    pop ax
    ret

dsp_write_byte:
    push ax
    push cx
    push dx

    mov ah, al
    mov dx, bx
    add dx, 0x0C
    mov cx, 0xFFFF

.wait:
    in al, dx
    test al, 0x80
    jz .ready
    loop .wait
    stc
    jmp .done

.ready:
    mov al, ah
    out dx, al
    clc

.done:
    pop dx
    pop cx
    pop ax
    ret

delay_reset:
    push cx
    mov cx, 0x0800
.loop:
    loop .loop
    pop cx
    ret

delay_tone:
    push cx
    mov cx, 0x0040
.loop:
    loop .loop
    pop cx
    ret

print_probe:
    mov dx, msg_probe_prefix
    call print_line
    mov ax, bx
    call print_hex_word
    mov dx, msg_crlf
    call print_line
    ret

print_found:
    mov dx, msg_found_prefix
    call print_line
    mov ax, bx
    call print_hex_word
    mov dx, msg_crlf
    call print_line
    ret

print_hex_word:
    push ax
    mov al, ah
    call print_hex_byte
    pop ax
    call print_hex_byte
    ret

print_hex_byte:
    push ax
    push bx
    mov bl, al
    shr al, 4
    call print_hex_nibble
    mov al, bl
    and al, 0x0F
    call print_hex_nibble
    pop bx
    pop ax
    ret

print_hex_nibble:
    cmp al, 10
    jb .digit
    add al, 0x37
    jmp .emit

.digit:
    add al, 0x30

.emit:
    mov dl, al
    mov ah, 0x02
    int 0x21
    ret

print_line:
    mov ah, 0x09
    int 0x21
    ret

dsp_base_list dw 0x0220, 0x0240, 0x0260, 0x0280

msg_begin db '[SB16INIT] BEGIN', 13, 10, '$'
msg_probe_prefix db '[SB16INIT] PROBE 0x', '$'
msg_found_prefix db '[SB16INIT] DSP OK at 0x', '$'
msg_not_found db '[SB16INIT] NO DSP FOUND', 13, 10, '$'
msg_cfg_done db '[SB16INIT] CFG IRQ7 DMA1', 13, 10, '$'
msg_tone_begin db '[SB16INIT] TONE START', 13, 10, '$'
msg_tone_done db '[SB16INIT] TONE DONE', 13, 10, '$'
msg_tone_fail db '[SB16INIT] TONE FAIL', 13, 10, '$'
msg_done db '[SB16INIT] DONE', 13, 10, '$'
msg_crlf db 13, 10, '$'
