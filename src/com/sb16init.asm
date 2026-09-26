bits 16
org 0x0100

start:
%ifdef SB_STARTUP
    cli
    mov ax, cs
    mov ss, ax
    mov sp, startup_stack_top
    sti
    mov ds, ax
    mov es, ax
    mov bx, ((startup_image_end - $$ + 0x100) + 15) >> 4
    mov ah, 0x4A
    int 0x21
    jc .setup_fail
%endif
    cld
    push cs
    pop ds
    push cs
    pop es
    call parse_quiet_switch

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
    mov [dsp_base_current], bx
    call print_found

%ifdef SB_STARTUP
    ; ISA DMA may not cross a physical 64 KiB boundary. Allocate enough room
    ; to align the complete melody, independently of the COM load address.
    mov bx, (65536 + dma_sample_len + 15) >> 4
    mov ah, 0x48
    int 0x21
    jc .setup_fail
    add ax, 0x0FFF
    and ax, 0xF000
    mov [startup_dma_seg], ax
    mov es, ax
    xor di, di
    mov si, dma_sample
    mov cx, dma_sample_len
    rep movsb
    push ds
    pop es
    mov bx, [dsp_base_current]
%endif

    call detect_sb_resources
    call configure_sb16_platform
    call print_config

    mov dx, msg_tone_begin
    call print_line
    call play_tone
    jc .tone_fail

    mov dx, msg_dma_done
    call print_line
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

%ifdef SB_STARTUP
.setup_fail:
    mov ax, 0x4C03
    int 0x21
%endif

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

    mov al, 0x83
    call dsp_write_byte
    jc .fail

    mov al, 0x14
    call dsp_write_byte
    jc .fail

    mov ax, dma_sample_len - 1
    call dsp_write_byte
    jc .fail

    mov al, ah
    call dsp_write_byte
    jc .fail

    call wait_irq7
    jc .fail

    mov al, 0xD3
    call dsp_write_byte

    call mask_dma1
    call restore_irq7_handler

    clc
    ret

.fail:
    mov al, 0xD0
    call dsp_write_byte
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

    call sb_irq_vector
    mov ah, 0x35
    mov al, [irq_vector]
    int 0x21
    mov [old_irq7_off], bx
    mov ax, es
    mov [old_irq7_seg], ax

    push cs
    pop ds
    mov dx, irq7_handler
    mov ah, 0x25
    mov al, [irq_vector]
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
    mov ah, 0x25
    mov al, [cs:irq_vector]
    int 0x21

    pop ds
    mov byte [irq7_installed], 0
    call restore_pic_masks
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
    push cx
    push dx
    push si

%ifdef SB_STARTUP
    mov ax, [startup_dma_seg]
%else
    mov ax, cs
%endif
    mov dx, ax
    shl ax, 4
    shr dx, 12
%ifndef SB_STARTUP
    add ax, dma_sample
    adc dl, 0
%endif

    mov si, ax
    mov cx, ax
    add cx, dma_sample_len - 1
    jc .fail
    mov bl, dl

    call mask_dma1

    xor al, al
    out 0x0C, al

    mov al, 0x48
    or al, [sb_dma]
    out 0x0B, al

    mov ax, si
    mov dx, [dma_addr_port]
    out dx, al
    mov al, ah
    out dx, al

    mov ax, dma_sample_len - 1
    mov dx, [dma_count_port]
    out dx, al
    mov al, ah
    out dx, al

    mov al, bl
    mov dx, [dma_page_port]
    out dx, al

    mov al, [sb_dma]
    out 0x0A, al

    clc
    jmp .done

.fail:
    stc

.done:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

mask_dma1:
    push ax
    mov al, [sb_dma]
    or al, 0x04
    out 0x0A, al
    pop ax
    ret

wait_irq7:
    push ax
    push bx
    push cx
    push dx

    sti
    mov ah, 0x00
    int 0x1A
    mov bx, dx
    mov dword [irq_poll_budget], 0x800000

.wait_tick:
    cmp byte [irq7_count], 0
    jne .ok

    mov ah, 0x00
    int 0x1A
    mov ax, dx
    sub ax, bx
%ifdef SB_STARTUP
    cmp ax, 54
%else
    cmp ax, 6
%endif
    jae .timeout
    in al, 0x80
    dec dword [irq_poll_budget]
    jnz .wait_tick

.timeout:
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

    cmp byte [cs:sb_irq], 8
    jb .master_eoi
    mov al, 0x20
    out 0xA0, al
.master_eoi:
    mov al, 0x20
    out 0x20, al

    pop dx
    pop ax
    iret

; Use the resources already selected by the card/firmware.  Reprogramming
; mixer registers 80h/81h is QEMU-friendly but is unsafe on PnP and laptop
; legacy implementations where routing is owned by PCI/BIOS configuration.
configure_sb16_platform:
    push ax
    mov al, [sb_dma]
    xor ah, ah
    shl ax, 1
    mov si, ax
    mov ax, [dma_addr_ports + si]
    mov [dma_addr_port], ax
    mov ax, [dma_count_ports + si]
    mov [dma_count_port], ax
    mov ax, [dma_page_ports + si]
    mov [dma_page_port], ax

    in al, 0x21
    mov [old_pic1_mask], al
    in al, 0xA1
    mov [old_pic2_mask], al
    mov al, [sb_irq]
    cmp al, 8
    jae .slave_irq
    mov cl, al
    mov ah, 1
    shl ah, cl
    not ah
    in al, 0x21
    and al, ah
    out 0x21, al
    jmp .dma
.slave_irq:
    sub al, 8
    mov cl, al
    mov ah, 1
    shl ah, cl
    not ah
    in al, 0xA1
    and al, ah
    out 0xA1, al
    in al, 0x21
    and al, 0xFB                  ; cascade IRQ2
    out 0x21, al

.dma:
    mov al, [sb_dma]
    out 0x0A, al

    pop ax
    ret

restore_pic_masks:
    push ax
    mov al, [old_pic1_mask]
    out 0x21, al
    mov al, [old_pic2_mask]
    out 0xA1, al
    pop ax
    ret

detect_sb_resources:
    push ax
    push dx
    mov byte [sb_irq], 5
    mov byte [sb_dma], 1

    mov dx, bx
    add dx, 4
    mov al, 0x80
    out dx, al
    inc dx
    in al, dx
    test al, 0x02
    jnz .irq5
    test al, 0x04
    jnz .irq7
    test al, 0x08
    jnz .irq10
    test al, 0x01
    jz .dma
    mov byte [sb_irq], 9
    jmp .dma
.irq5:
    mov byte [sb_irq], 5
    jmp .dma
.irq7:
    mov byte [sb_irq], 7
    jmp .dma
.irq10:
    mov byte [sb_irq], 10
.dma:
    mov dx, bx
    add dx, 4
    mov al, 0x81
    out dx, al
    inc dx
    in al, dx
    test al, 0x02
    jnz .done
    test al, 0x01
    jz .try_dma3
    mov byte [sb_dma], 0
    jmp .done
.try_dma3:
    test al, 0x08
    jz .done
    mov byte [sb_dma], 3
.done:
    pop dx
    pop ax
    ret

sb_irq_vector:
    mov al, [sb_irq]
    cmp al, 8
    jb .master
    add al, 0x68                  ; IRQ8 -> INT70h
    jmp .done
.master:
    add al, 0x08
.done:
    mov [irq_vector], al
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

delay_irq_wait:
    push cx
    mov cx, 0x0100
.loop:
    loop .loop
    pop cx
    ret

parse_quiet_switch:
    push ax
    push cx
    push si

    mov si, 0x0081
    mov cl, [0x0080]
    xor ch, ch

.next_char:
    jcxz .done
    lodsb
    dec cx
    cmp al, ' '
    je .next_char
    cmp al, 9
    je .next_char
    cmp al, '/'
    jne .skip_token

    jcxz .done
    lodsb
    dec cx
    and al, 0xDF
    cmp al, 'Q'
    jne .skip_token
    jcxz .set_quiet

    mov al, [si]
    and al, 0xDF
    cmp al, 'U'
    jne .set_quiet
    cmp cx, 4
    jb .skip_token
    inc si
    dec cx
    mov al, [si]
    and al, 0xDF
    cmp al, 'I'
    jne .skip_token
    inc si
    dec cx
    mov al, [si]
    and al, 0xDF
    cmp al, 'E'
    jne .skip_token
    inc si
    dec cx
    mov al, [si]
    and al, 0xDF
    cmp al, 'T'
    jne .skip_token
    inc si
    dec cx

.set_quiet:
    mov byte [quiet_mode], 1
    jmp .done

.skip_token:
    jcxz .done
    lodsb
    dec cx
    cmp al, ' '
    je .next_char
    cmp al, 9
    jne .skip_token
    jmp .next_char

.done:
    pop si
    pop cx
    pop ax
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

print_config:
    mov dx, msg_cfg_prefix
    call print_line
    mov al, [sb_irq]
    call print_u8_small
    mov dx, msg_cfg_dma
    call print_line
    mov al, [sb_dma]
    call print_u8_small
    mov dx, msg_crlf
    call print_line
    ret

print_u8_small:
    cmp byte [quiet_mode], 0
    jne .done
    push bx
    aam
    mov bl, al
    cmp ah, 0
    je .ones
    mov dl, ah
    add dl, '0'
    mov ah, 0x02
    int 0x21
.ones:
    mov dl, bl
    add dl, '0'
    mov ah, 0x02
    int 0x21
    pop bx
.done:
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
    cmp byte [quiet_mode], 0
    jne .done
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
.done:
    ret

print_line:
    cmp byte [quiet_mode], 0
    jne .done
    mov ah, 0x09
    int 0x21
.done:
    ret

dsp_base_list dw 0x0220, 0x0240, 0x0260, 0x0280
dsp_base_current dw 0x0220
old_irq7_off dw 0
old_irq7_seg dw 0
irq7_installed db 0
irq7_count db 0
quiet_mode db 0
sb_irq db 5
sb_dma db 1
irq_vector db 0x0D
old_pic1_mask db 0xFF
old_pic2_mask db 0xFF
dma_addr_port dw 0x0002
dma_count_port dw 0x0003
dma_page_port dw 0x0083
dma_addr_ports dw 0x0000, 0x0002, 0x0004, 0x0006
dma_count_ports dw 0x0001, 0x0003, 0x0005, 0x0007
dma_page_ports dw 0x0087, 0x0083, 0x0081, 0x0082

dma_sample:
%ifdef SB_STARTUP
    incbin "build/full/obj/BOOTSB.PCM"
%else
    times 32 db 0x80, 0xA0, 0xC0, 0xE0, 0xFF, 0xE0, 0xC0, 0xA0
    times 32 db 0x80, 0x60, 0x40, 0x20, 0x00, 0x20, 0x40, 0x60
%endif
dma_sample_len equ $ - dma_sample

msg_begin db '[SB16INIT] BEGIN', 13, 10, '$'
msg_probe_prefix db '[SB16INIT] PROBE 0x', '$'
msg_found_prefix db '[SB16INIT] DSP OK at 0x', '$'
msg_not_found db '[SB16INIT] NO DSP FOUND', 13, 10, '$'
msg_cfg_prefix db '[SB16INIT] CFG IRQ', '$'
msg_cfg_dma db ' DMA', '$'
msg_tone_begin db '[SB16INIT] TONE START', 13, 10, '$'
msg_dma_done db '[SB16INIT] DMA DONE', 13, 10, '$'
msg_tone_done db '[SB16INIT] TONE DONE', 13, 10, '$'
msg_tone_fail db '[SB16INIT] TONE FAIL', 13, 10, '$'
msg_done db '[SB16INIT] DONE', 13, 10, '$'
msg_crlf db 13, 10, '$'
irq_poll_budget dd 0
%ifdef SB_STARTUP
startup_dma_seg dw 0
align 16
startup_stack times 4096 db 0
startup_stack_top:
startup_image_end:
%endif
