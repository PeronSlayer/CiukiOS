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
    mov dx, msg_help1
    call print_line
    mov dx, msg_help2
    call print_line
    mov dx, msg_help3
    call print_line
    mov dx, msg_help4
    call print_line
    mov dx, msg_help5
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
    mov [dsp_base_current], bx
    call print_found
    call configure_sb16_platform

    mov si, sample_wave
    mov cx, sample_wave_len
    call set_dma_sample
    mov byte [playback_timeconst], 0x83
    call flush_keyboard_buffer

.key_loop:
    xor ah, ah
    int 0x16
    cmp al, 0x11
    jne .not_ctrl_q
    cmp ah, 0x10
    je .exit_ok

.not_ctrl_q:
    cmp al, 0
    je .extended
    cmp al, 0xE0
    je .extended
    cmp al, 'c'
    je .note_c
    cmp al, 'C'
    je .note_c
    cmp al, 'd'
    je .note_d
    cmp al, 'D'
    je .note_d
    cmp al, 'e'
    je .note_e
    cmp al, 'E'
    je .note_e
    cmp al, 'f'
    je .note_f
    cmp al, 'F'
    je .note_f
    cmp al, 'g'
    je .note_g
    cmp al, 'G'
    je .note_g
    cmp al, 'a'
    je .note_a
    cmp al, 'A'
    je .note_a
    cmp al, 'b'
    je .note_b
    cmp al, 'B'
    je .note_b
    cmp al, '+'
    je .fine_up
    cmp al, '='
    je .fine_up
    cmp al, '-'
    je .fine_down
    cmp al, ' '
    je .repeat_last
    jmp .key_loop

.extended:
    cmp ah, 0x4B
    je .left
    cmp ah, 0x50
    je .down
    cmp ah, 0x48
    je .up
    cmp ah, 0x4D
    je .right
    jmp .key_loop

.left:
    mov byte [playback_timeconst], 0x74
    mov dx, msg_left
    call play_selected_tone
    jmp .key_loop

.down:
    mov byte [playback_timeconst], 0x7C
    mov dx, msg_down
    call play_selected_tone
    jmp .key_loop

.up:
    mov byte [playback_timeconst], 0x8C
    mov dx, msg_up
    call play_selected_tone
    jmp .key_loop

.right:
    mov byte [playback_timeconst], 0x9C
    mov dx, msg_right
    call play_selected_tone
    jmp .key_loop

.note_c:
    mov byte [playback_timeconst], 0x6C
    mov dx, msg_note_c
    call play_selected_tone
    jmp .key_loop

.note_d:
    mov byte [playback_timeconst], 0x74
    mov dx, msg_note_d
    call play_selected_tone
    jmp .key_loop

.note_e:
    mov byte [playback_timeconst], 0x7C
    mov dx, msg_note_e
    call play_selected_tone
    jmp .key_loop

.note_f:
    mov byte [playback_timeconst], 0x84
    mov dx, msg_note_f
    call play_selected_tone
    jmp .key_loop

.note_g:
    mov byte [playback_timeconst], 0x8C
    mov dx, msg_note_g
    call play_selected_tone
    jmp .key_loop

.note_a:
    mov byte [playback_timeconst], 0x94
    mov dx, msg_note_a
    call play_selected_tone
    jmp .key_loop

.note_b:
    mov byte [playback_timeconst], 0x9C
    mov dx, msg_note_b
    call play_selected_tone
    jmp .key_loop

.fine_up:
    cmp byte [playback_timeconst], 0xD0
    jae .key_loop
    inc byte [playback_timeconst]
    mov dx, msg_fine_up
    call play_selected_tone
    jmp .key_loop

.fine_down:
    cmp byte [playback_timeconst], 0x40
    jbe .key_loop
    dec byte [playback_timeconst]
    mov dx, msg_fine_down
    call play_selected_tone
    jmp .key_loop

.repeat_last:
    mov dx, msg_repeat
    call play_selected_tone
    jmp .key_loop

.exit_ok:
    mov dx, msg_exit
    call print_line
    mov ax, 0x4C00
    int 0x21

play_selected_tone:
    push dx
    call print_line
    call play_current_sample
    pop dx
    jc .fail
    mov dx, msg_dma_done
    call print_line
    clc
    ret

.fail:
    mov dx, msg_fail
    call print_line
    stc
    ret

flush_keyboard_buffer:
.check:
    mov ah, 0x01
    int 0x16
    jz .done
    xor ah, ah
    int 0x16
    jmp .check
.done:
    ret

set_dma_sample:
    push ax
    push dx

    mov [dma_len], cx

    mov ax, cs
    mov dx, ax
    shl ax, 4
    shr dx, 12
    add ax, si
    adc dl, 0

    mov [dma_offset], ax
    mov [dma_page], dl

    pop dx
    pop ax
    ret

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

play_current_sample:
    call install_irq7_handler
    jc .fail_no_restore

    call program_dma1_playback
    jc .fail

    mov al, 0xD1
    call dsp_write_byte
    jc .fail

    mov al, 0x40
    call dsp_write_byte
    jc .fail

    mov al, [playback_timeconst]
    call dsp_write_byte
    jc .fail

    mov al, 0x14
    call dsp_write_byte
    jc .fail

    mov ax, [dma_len]
    dec ax
    call dsp_write_byte
    jc .fail

    mov al, ah
    call dsp_write_byte
    jc .fail

    call wait_irq7
    jc .fail

    mov al, 0xD3
    call dsp_write_byte

    call restore_irq7_handler
    clc
    ret

.fail:
    call mask_dma1
    call restore_irq7_handler

.fail_no_restore:
    stc
    ret

install_irq7_handler:
    push ax
    push bx
    push dx
    push ds
    push es

    mov byte [irq7_count], 0

    mov ax, 0x350F
    int 0x21
    mov [old_irq7_off], bx
    mov ax, es
    mov [old_irq7_seg], ax

    push cs
    pop ds
    mov dx, irq7_handler
    mov ax, 0x250F
    int 0x21
    mov byte [irq7_installed], 1
    clc

    pop es
    pop ds
    pop dx
    pop bx
    pop ax
    ret

restore_irq7_handler:
    push ax
    push dx
    push ds

    cmp byte [irq7_installed], 0
    je .done

    mov ax, [old_irq7_seg]
    mov dx, [old_irq7_off]
    mov ds, ax
    mov ax, 0x250F
    int 0x21

    pop ds
    mov byte [irq7_installed], 0
    jmp .restored

.done:
    pop ds

.restored:
    pop dx
    pop ax
    ret

program_dma1_playback:
    push ax
    push bx
    push dx

    call mask_dma1

    xor al, al
    out 0x0C, al

    mov al, 0x49
    out 0x0B, al

    mov ax, [dma_offset]
    out 0x02, al
    mov al, ah
    out 0x02, al

    mov ax, [dma_len]
    dec ax
    out 0x03, al
    mov al, ah
    out 0x03, al

    mov al, [dma_page]
    out 0x83, al

    mov al, 0x01
    out 0x0A, al

    clc
    jmp .done

.done:
    pop dx
    pop bx
    pop ax
    ret

mask_dma1:
    push ax
    mov al, 0x05
    out 0x0A, al
    pop ax
    ret

wait_irq7:
    push ax
    push bx
    push cx
    push dx

    sti
    mov cx, 8
    mov ah, 0x00
    int 0x1A
    mov bx, dx

.wait_tick:
    cmp byte [irq7_count], 0
    jne .ok
    mov ah, 0x00
    int 0x1A
    cmp dx, bx
    je .wait_tick
    mov bx, dx
    loop .wait_tick
    stc
    jmp .done

.ok:
    clc

.done:
    pop dx
    pop cx
    pop bx
    pop ax
    ret

irq7_handler:
    push ax
    push dx

    mov dx, [cs:dsp_base_current]
    add dx, 0x0E
    in al, dx
    inc byte [cs:irq7_count]

    mov al, 0x20
    out 0x20, al

    pop dx
    pop ax
    iret

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
dsp_base_current dw 0x0220
old_irq7_off dw 0
old_irq7_seg dw 0
dma_offset dw 0
dma_len dw 0
dma_page db 0
playback_timeconst db 0x83
irq7_installed db 0
irq7_count db 0

sample_wave:
    times 48 db 0x80, 0xA0, 0xC0, 0xE0, 0xFF, 0xE0, 0xC0, 0xA0
    times 48 db 0x80, 0x60, 0x40, 0x20, 0x00, 0x20, 0x40, 0x60
sample_wave_len equ $ - sample_wave

msg_begin db '[AUDIOKEY] BEGIN', 13, 10, '$'
msg_help1 db '[AUDIOKEY] ------------------------------', 13, 10, '$'
msg_help2 db '[AUDIOKEY] ARROWS  preset tones', 13, 10, '$'
msg_help3 db '[AUDIOKEY] C D E F G A B  scale notes', 13, 10, '$'
msg_help4 db '[AUDIOKEY] + / -  fine tune   SPACE replay', 13, 10, '$'
msg_help5 db '[AUDIOKEY] CTRL+Q exit', 13, 10, '$'
msg_probe_prefix db '[AUDIOKEY] PROBE 0x', '$'
msg_found_prefix db '[AUDIOKEY] DSP OK at 0x', '$'
msg_not_found db '[AUDIOKEY] NO DSP FOUND', 13, 10, '$'
msg_left db '[AUDIOKEY] LEFT LOW', 13, 10, '$'
msg_down db '[AUDIOKEY] DOWN MIDLOW', 13, 10, '$'
msg_up db '[AUDIOKEY] UP MIDHIGH', 13, 10, '$'
msg_right db '[AUDIOKEY] RIGHT HIGH', 13, 10, '$'
msg_note_c db '[AUDIOKEY] NOTE C', 13, 10, '$'
msg_note_d db '[AUDIOKEY] NOTE D', 13, 10, '$'
msg_note_e db '[AUDIOKEY] NOTE E', 13, 10, '$'
msg_note_f db '[AUDIOKEY] NOTE F', 13, 10, '$'
msg_note_g db '[AUDIOKEY] NOTE G', 13, 10, '$'
msg_note_a db '[AUDIOKEY] NOTE A', 13, 10, '$'
msg_note_b db '[AUDIOKEY] NOTE B', 13, 10, '$'
msg_fine_up db '[AUDIOKEY] STEP UP', 13, 10, '$'
msg_fine_down db '[AUDIOKEY] STEP DOWN', 13, 10, '$'
msg_repeat db '[AUDIOKEY] REPLAY', 13, 10, '$'
msg_dma_done db '[AUDIOKEY] DMA DONE', 13, 10, '$'
msg_fail db '[AUDIOKEY] FAIL', 13, 10, '$'
msg_exit db '[AUDIOKEY] EXIT', 13, 10, '$'
msg_crlf db 13, 10, '$'
