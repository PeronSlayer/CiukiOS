; f3-05 DOS-side Sound Blaster 16 / OPL3 / 8237 probe (VMAUDIO.COM).
; Mailbox at 0000:0600: the kernel controller (f3_probes_audio_state.c)
; writes the command word, this program runs the stage and publishes it.
; It reports raw readings only; the controller judges them. Detection uses
; the BLASTER variable of the CIUKIDOS environment, as DOS programs do.
; Wait loops test their condition with IF clear and sleep with STI;HLT
; (f3-01 lesson); command waits spin, never HLT, since no IRQ announces them.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
org 0x100

%define M_MAGIC   0x600
%define M_STAGE   0x602
%define M_COMMAND 0x604
%define M_IRQS    0x606
%define M_LAST82  0x608
%define M_ERROR   0x60A
%define M_VERSION 0x60C
%define M_BASE    0x60E
%define M_IRQ     0x610
%define M_DMA     0x612
%define M_HDMA    0x614
%define M_MIX80   0x616
%define M_MIX81   0x618
%define M_IDENT   0x61A
%define M_TEST    0x61C
%define M_SPEAKER 0x61E
%define M_RESET   0x620
%define M_MASTER  0x622
%define M_COMPAT  0x624
%define M_DEFAULT 0x626
%define M_STATUS  0x628
%define M_DMATC   0x62A
%define M_COUNTA  0x62C
%define M_COUNTB  0x62E
%define M_ISRBAD  0x630
%define M_STI82   0x632
%define M_IRQSB   0x634
%define M_S1      0x636      ; six OPL status words: S1 S2 T1 T2 S3 S4
%define M_FIRQS   0x642
%define M_FRESET  0x644
%define M_BUFFER  0x646      ; dword: linear address of the DMA buffer
%define M_WORDS   0x26       ; 0600h-064Bh
%define BUF_SIZE  1024

start:
    cld
    cli
    xor ax, ax
    mov es, ax
    mov di, M_MAGIC
    mov cx, M_WORDS
    rep stosw
    mov ds, ax                  ; DS = 0: mailbox; program data through CS:
    sti
    call parse_blaster
    call setup_buffer
    call detect
    call install_isr
    mov word [M_MAGIC], 0D305h
    mov word [M_STAGE], 1
    mov bx, 2
.next:
    call wait_command
    push bx
    mov si, bx
    sub si, 2
    shl si, 1
    call [cs:stage_table + si]
    pop bx
    mov [M_STAGE], bx
    inc bx
    cmp bx, 11
    jb .next
    call wait_command           ; command 11: clean up and return to DOS
    jmp stage11

wait_command:                   ; BX = command; IF left as the caller set it
    cmp [M_COMMAND], bx
    jne wait_command
    ret

wait_irqs:                      ; until [M_IRQS] >= AX; returns with IF set
    push bx
    mov bx, ax
.loop:
    cli
    cmp [M_IRQS], bx
    jae .done
    sti
    hlt                         ; STI shadow covers HLT: a pending IRQ wakes it
    jmp .loop
.done:
    sti
    pop bx
    ret

; ---------------------------------------------------------------- BLASTER
parse_blaster:
    push es
    mov word [cs:sb_base], 220h
    mov byte [cs:irq], 7
    mov byte [cs:dma8], 1
    mov byte [cs:dma16], 5
    mov es, [cs:2Ch]            ; environment segment of this PSP
    xor di, di
.var:
    cmp byte [es:di], 0
    je .missing
    mov si, blaster_name
    mov cx, 8
    push di
.cmp:
    mov al, [cs:si]
    cmp al, [es:di]
    jne .skip
    inc si
    inc di
    loop .cmp
    add sp, 2                   ; matched: ES:DI is the value
    jmp .parse
.skip:
    pop di
.skip_var:
    cmp byte [es:di], 0
    je .after
    inc di
    jmp .skip_var
.after:
    inc di
    jmp .var
.missing:
    mov word [M_ERROR], 1
    jmp .store
.parse:
    mov al, [es:di]
    inc di
    test al, al
    jz .store
    and al, 0DFh
    cmp al, 'A'
    je .base
    cmp al, 'I'
    je .irq
    cmp al, 'D'
    je .dma
    cmp al, 'H'
    je .hdma
    jmp .parse
.base:
    call read_number16
    mov [cs:sb_base], ax
    jmp .parse
.irq:
    call read_number10
    mov [cs:irq], al
    jmp .parse
.dma:
    call read_number10
    mov [cs:dma8], al
    jmp .parse
.hdma:
    call read_number10
    mov [cs:dma16], al
    jmp .parse
.store:
    mov ax, [cs:sb_base]
    mov [M_BASE], ax
    xor ah, ah
    mov al, [cs:irq]
    mov [M_IRQ], ax
    mov al, [cs:dma8]
    mov [M_DMA], ax
    mov al, [cs:dma16]
    mov [M_HDMA], ax
    pop es
    ret

read_number16:                  ; hex digits at ES:DI -> AX
    xor ax, ax
.digit:
    mov dl, [es:di]
    cmp dl, '0'
    jb .done
    cmp dl, '9'
    jbe .dec
    and dl, 0DFh
    cmp dl, 'A'
    jb .done
    cmp dl, 'F'
    ja .done
    sub dl, 'A' - 10
    jmp .add
.dec:
    sub dl, '0'
.add:
    shl ax, 4
    or al, dl
    inc di
    jmp .digit
.done:
    ret

read_number10:                  ; decimal digits at ES:DI -> AL
    xor ax, ax
.digit:
    mov dl, [es:di]
    cmp dl, '0'
    jb .done
    cmp dl, '9'
    ja .done
    sub dl, '0'
    mov dh, 10
    mul dh
    add al, dl
    inc di
    jmp .digit
.done:
    ret

; ---------------------------------------------------------------- buffer
setup_buffer:                   ; BUF_SIZE bytes inside one 64 KiB DMA page
    mov ax, cs
    mov dx, ax
    mov cl, 12
    shr dx, cl
    mov cl, 4
    shl ax, cl
    add ax, dma_area
    adc dx, 0                   ; DX:AX = linear address
    mov bx, ax
    add bx, BUF_SIZE - 1
    mov cx, dx
    adc cx, 0
    cmp cx, dx
    je .ok
    mov dx, cx                  ; crossing: start at the next page (inside the area)
    xor ax, ax
.ok:
    mov [cs:buf_lin], ax
    mov [cs:buf_lin + 2], dx
    mov [M_BUFFER], ax
    mov [M_BUFFER + 2], dx
    mov bx, ax
    and bx, 15
    mov [cs:buf_off], bx
    mov cl, 4
    shr ax, cl
    mov cl, 12
    shl dx, cl
    or ax, dx
    mov [cs:buf_seg], ax
    ret

; ---------------------------------------------------------------- DSP, mixer, DMA
dsp_write:                      ; AL
    push ax
    push cx
    push dx
    mov ah, al
    mov dx, [cs:sb_base]
    add dx, 0Ch
    mov cx, 0FFFFh
.wait:
    in al, dx
    test al, 80h
    jz .ready
    loop .wait
.ready:
    mov al, ah
    out dx, al
    pop dx
    pop cx
    pop ax
    ret

dsp_read:                       ; -> AL (FFh on timeout); reading 2xEh acknowledges 8-bit
    push cx
    push dx
    mov dx, [cs:sb_base]
    add dx, 0Eh
    mov cx, 0FFFFh
.wait:
    in al, dx
    test al, 80h
    jnz .ready
    loop .wait
    mov al, 0FFh
    jmp .done
.ready:
    sub dx, 4
    in al, dx
.done:
    pop dx
    pop cx
    ret

dsp_reset:                      ; -> AL: the byte read after reset (AAh)
    push cx
    push dx
    mov dx, [cs:sb_base]
    add dx, 6
    mov al, 1
    out dx, al
    mov cx, 8
.delay:
    in al, dx                   ; ~1 us per ISA cycle; 3 us are required
    loop .delay
    xor al, al
    out dx, al
    pop dx
    pop cx
    call dsp_read
    ret

mixer_write:                    ; AH = index, AL = value
    push dx
    mov dx, [cs:sb_base]
    add dx, 4
    xchg al, ah
    out dx, al
    inc dx
    xchg al, ah
    out dx, al
    pop dx
    ret

mixer_read:                     ; AH = index -> AX = value
    push dx
    mov dx, [cs:sb_base]
    add dx, 4
    mov al, ah
    out dx, al
    inc dx
    in al, dx
    xor ah, ah
    pop dx
    ret

dsp_rate:                       ; AX = rate (41h)
    push ax
    mov al, 41h
    call dsp_write
    pop ax
    push ax
    mov al, ah
    call dsp_write
    pop ax
    call dsp_write
    ret

dsp_word:                       ; AL = command, CX = length-1 (low byte first)
    call dsp_write
    mov al, cl
    call dsp_write
    mov al, ch
    call dsp_write
    ret

dma_start8:                     ; SI = count-1, BL = 48h single / 58h auto-init
    push ax
    push bx
    push dx
    mov bh, [cs:dma8]
    and bh, 3
    mov al, bh
    or al, 4
    out 0Ah, al                 ; mask the channel
    out 0Ch, al                 ; clear the flip-flop
    xor dx, dx
    mov dl, bh
    shl dl, 1                   ; address port
    mov ax, [cs:buf_lin]
    out dx, al
    mov al, ah
    out dx, al
    inc dx                      ; count port
    mov ax, si
    out dx, al
    mov al, ah
    out dx, al
    push bx
    mov dl, bh
    xor bh, bh
    mov bl, dl
    mov dl, [cs:page8 + bx]
    pop bx
    mov al, [cs:buf_lin + 2]
    out dx, al                  ; page
    mov al, bl
    or al, bh
    out 0Bh, al                 ; mode: single transfer, read, channel
    mov al, bh
    out 0Ah, al                 ; unmask
    pop dx
    pop bx
    pop ax
    ret

dma_start16:                    ; SI = words-1, BL = 48h/58h; master controller
    push ax
    push bx
    push cx
    push dx
    mov bh, [cs:dma16]
    and bh, 3
    mov al, bh
    or al, 4
    out 0D4h, al
    out 0D8h, al
    xor dx, dx
    mov dl, bh
    shl dl, 1
    shl dl, 1
    add dl, 0C0h                ; address port C0h + 4c
    mov ax, [cs:buf_lin]
    mov cx, [cs:buf_lin + 2]
    shr cx, 1
    rcr ax, 1                   ; word address
    out dx, al
    mov al, ah
    out dx, al
    add dl, 2                   ; count port
    mov ax, si
    out dx, al
    mov al, ah
    out dx, al
    push bx
    mov dl, bh
    xor bh, bh
    mov bl, dl
    mov dl, [cs:page16 + bx]
    pop bx
    mov al, [cs:buf_lin + 2]
    and al, 0FEh
    out dx, al
    mov al, bl
    or al, bh
    out 0D6h, al
    mov al, bh
    out 0D4h, al
    pop dx
    pop cx
    pop bx
    pop ax
    ret

read_count8:                    ; -> AX: current count of the 8-bit channel
    push bx
    push dx
    out 0Ch, al
    xor dx, dx
    mov dl, [cs:dma8]
    and dl, 3
    shl dl, 1
    inc dl
    in al, dx
    mov bl, al
    in al, dx
    mov ah, al
    mov al, bl
    pop dx
    pop bx
    ret

; ---------------------------------------------------------------- detection (stage 1)
detect:
    call dsp_reset
    xor ah, ah
    mov [M_RESET], ax
    mov al, 0E1h
    call dsp_write
    call dsp_read
    mov ah, al
    call dsp_read
    mov [M_VERSION], ax         ; major:minor
    mov al, 0E0h
    call dsp_write
    mov al, 55h
    call dsp_write
    call dsp_read
    xor ah, ah
    mov [M_IDENT], ax
    mov al, 0E4h
    call dsp_write
    mov al, 5Ah
    call dsp_write
    mov al, 0E8h
    call dsp_write
    call dsp_read
    xor ah, ah
    mov [M_TEST], ax
    mov al, 0D1h
    call dsp_write
    mov al, 0D8h
    call dsp_write
    call dsp_read
    mov bh, al
    mov al, 0D3h
    call dsp_write
    mov al, 0D8h
    call dsp_write
    call dsp_read
    mov bl, al
    mov [M_SPEAKER], bx         ; FF00h: on, then off
    mov ah, 80h
    call mixer_read
    mov [M_MIX80], ax
    mov ah, 81h
    call mixer_read
    mov [M_MIX81], ax
    mov ah, 82h
    call mixer_read
    mov [M_STATUS], ax
    mov ax, 22A5h               ; SB Pro master 10/5 -> CT1745 levels 21/11
    call mixer_write
    mov ah, 30h
    call mixer_read
    mov [M_MASTER], ax
    mov ah, 22h
    call mixer_read
    mov [M_COMPAT], ax
    xor ax, ax                  ; mixer reset
    call mixer_write
    mov ah, 30h
    call mixer_read
    mov [M_DEFAULT], ax
    ret

install_isr:
    cli
    mov al, [cs:irq]
    cmp al, 8
    jb .master
    add al, 70h - 8
    jmp .vector
.master:
    add al, 8
.vector:
    mov [cs:vector], al
    xor bh, bh
    mov bl, al
    shl bx, 1
    shl bx, 1
    mov ax, [bx]
    mov [cs:old_vec], ax
    mov ax, [bx + 2]
    mov [cs:old_vec + 2], ax
    mov word [bx], isr
    mov [bx + 2], cs
    in al, 21h
    mov [cs:old_imr0], al
    in al, 0A1h
    mov [cs:old_imr1], al
    mov cl, [cs:irq]
    cmp cl, 8
    jae .slave
    mov ah, 1
    shl ah, cl
    not ah
    in al, 21h
    and al, ah                  ; 8259A: a mask bit of 0 enables the line
    out 21h, al
    jmp .done
.slave:
    sub cl, 8
    mov ah, 1
    shl ah, cl
    not ah
    in al, 0A1h
    and al, ah
    out 0A1h, al
    in al, 21h
    and al, 0FBh                ; cascade
    out 21h, al
.done:
    sti
    ret

; ---------------------------------------------------------------- interrupt
isr:
    push ax
    push dx
    push ds
    cld
    xor ax, ax
    mov ds, ax
    mov dx, [cs:sb_base]
    add dx, 4
    mov al, 82h
    out dx, al
    inc dx
    in al, dx                   ; interrupt status: which DSP cause
    xor ah, ah
    mov [M_LAST82], ax
    test al, [cs:expect]
    jnz .mine
    inc word [M_ISRBAD]
.mine:
    inc word [M_IRQS]
    cmp byte [cs:fill_mode], 1
    je .fill
    cmp byte [cs:fill_mode], 2
    jne .exit_check
.fill:
    call fill_half              ; refill the half that has just been played
.exit_check:
    mov ax, [M_IRQS]
    cmp ax, [cs:stop_at]
    jne .ack
    mov al, [cs:exit_cmd]
    call dsp_write
.ack:
    mov dx, [cs:sb_base]
    add dx, 0Eh
    cmp byte [cs:expect], 2
    jne .ack8
    inc dx                      ; 2xFh: 16-bit acknowledge
.ack8:
    in al, dx
    mov al, 20h
    cmp byte [cs:irq], 8
    jb .eoi
    out 0A0h, al
.eoi:
    out 20h, al
    pop ds
    pop dx
    pop ax
    iret

fill_half:                      ; half [cs:half] <- sequence [cs:seq]; advances both
    push ax
    push cx
    push di
    push es
    cld
    mov es, [cs:buf_seg]
    mov di, [cs:buf_off]
    cmp byte [cs:fill_mode], 2
    je .wide
    cmp byte [cs:half], 0
    je .h8
    add di, 64
.h8:
    mov al, [cs:seq]
    add al, 20h
    mov cx, 64
    rep stosb
    jmp .advance
.wide:                          ; 64 stereo frames: L = seq:11h, R = NOT L
    cmp byte [cs:half], 0
    je .h16
    add di, 256
.h16:
    mov cx, 64
    mov ah, [cs:seq]
    mov al, 11h
.frame:
    stosw
    not ax
    stosw
    not ax
    loop .frame
.advance:
    inc byte [cs:seq]
    xor byte [cs:half], 1
    pop es
    pop di
    pop cx
    pop ax
    ret

transfer_setup:                 ; AL = fill mode, AH = expected 82h bit
    mov word [M_IRQS], 0
    mov [cs:fill_mode], al
    mov [cs:expect], ah
    mov word [cs:stop_at], 0FFFFh
    ret

; ---------------------------------------------------------------- stages
stage2:                         ; 8-bit single cycle: 256-byte ramp at 8000 Hz
    mov ax, 0100h
    call transfer_setup
    push es
    mov es, [cs:buf_seg]
    mov di, [cs:buf_off]
    xor al, al
    mov cx, 256
.ramp:
    stosb
    inc al
    loop .ramp
    pop es
    mov si, 255
    mov bl, 48h
    call dma_start8
    mov al, 0D1h
    call dsp_write
    mov ax, 8000
    call dsp_rate
    mov al, 14h
    mov cx, 255
    call dsp_word
    mov ax, 1
    call wait_irqs
    in al, 08h                  ; DMA status: terminal count bits
    xor ah, ah
    mov [M_DMATC], ax
    ret

stage3:                         ; 64-unit auto-init double buffer, 32 blocks, 8000 Hz
    cli
    mov ax, 0101h
    call transfer_setup
    mov byte [cs:seq], 0
    mov byte [cs:half], 0
    call fill_half
    call fill_half
    mov word [cs:stop_at], 31   ; exit auto-init after the 31st IRQ: 32 blocks
    mov byte [cs:exit_cmd], 0DAh
    sti
    mov si, 127
    mov bl, 58h
    call dma_start8
    mov ax, 8000
    call dsp_rate
    mov al, 48h
    mov cx, 63
    call dsp_word
    mov al, 1Ch
    call dsp_write
    mov ax, 32
    call wait_irqs
    ret

stage4:                         ; pause: 80-unit auto-init blocks (10 ms), D0h after 2 IRQs
    mov ax, 0103h
    call transfer_setup
    mov si, 159
    mov bl, 58h
    call dma_start8
    mov ax, 8000
    call dsp_rate
    mov al, 48h
    mov cx, 79
    call dsp_word
    mov al, 1Ch
    call dsp_write
    mov ax, 2
    call wait_irqs
    mov al, 0D0h
    call dsp_write
    call read_count8
    mov [M_COUNTA], ax
    ret

stage5:                         ; continue after the controller's pause window, then exit
    call read_count8
    mov [M_COUNTB], ax
    mov al, 0D4h
    call dsp_write
    mov ax, 4
    call wait_irqs
    mov al, 0DAh
    call dsp_write
    mov ax, 5
    call wait_irqs
    ret

stage6:                         ; 16-bit signed stereo auto-init, 64-frame blocks at 12000 Hz
    cli
    mov ax, 0202h
    call transfer_setup
    mov byte [cs:seq], 0
    mov byte [cs:half], 0
    call fill_half
    call fill_half
    mov word [cs:stop_at], 7
    mov byte [cs:exit_cmd], 0D9h
    sti
    mov si, 255
    mov bl, 58h
    call dma_start16
    mov ax, 12000
    call dsp_rate
    mov al, 0B6h
    call dsp_write
    mov al, 30h
    call dsp_write
    mov al, 7Fh
    call dsp_write
    xor al, al
    call dsp_write
    mov ax, 8
    call wait_irqs
    ret

stage7:                         ; IF clear: 10 ms blocks; CLI after the first IRQ
    mov ax, 0103h
    call transfer_setup
    mov si, 159
    mov bl, 58h
    call dma_start8
    mov ax, 8000
    call dsp_rate
    mov al, 48h
    mov cx, 79
    call dsp_word
    mov al, 1Ch
    call dsp_write
    mov ax, 1
    call wait_irqs
    cli                         ; stays clear until command 8 (the controller holds 100 ms)
    ret

stage8:                         ; STI: exactly the pending request is delivered
    mov ax, [M_IRQS]
    mov [M_IRQSB], ax
    sti
    mov ax, 2
    call wait_irqs
    mov ax, [M_LAST82]
    mov [M_STI82], ax
    mov al, 0DAh
    call dsp_write
    mov ax, 3
    call wait_irqs
    ret

stage9:                         ; OPL3 script
    mov si, opl_script
    mov di, M_S1
.next:
    mov al, [cs:si]
    cmp al, 0FFh
    je .done
    mov bl, [cs:si + 1]
    mov bh, [cs:si + 2]
    add si, 3
    cmp al, 10h
    jae .op
    call opl_port
    mov al, bl
    out dx, al
    inc dx
    mov al, bh
    out dx, al
    jmp .next
.op:
    cmp al, 10h
    jne .delay
    call opl_status
    jmp .store
.delay:
    cmp al, 11h
    jne .poll
    mov cx, 100                 ; detection delay: 100 status reads (>= 80 us of ISA time)
.read:
    call opl_status
    loop .read
    jmp .store
.poll:
    mov cx, 200
.again:
    call opl_status
    test al, bl
    jnz .store
    push ax
    push cx
    push dx
    mov ah, 86h                 ; BIOS wait 1 ms
    xor cx, cx
    mov dx, 1000
    int 15h
    pop dx
    pop cx
    pop ax
    loop .again
    mov word [M_ERROR], 9
.store:
    xor ah, ah
    mov [di], ax
    add di, 2
    jmp .next
.done:
    ret

opl_status:
    push dx
    mov dx, 388h
    in al, dx
    pop dx
    ret

opl_port:                       ; AL = kind -> DX = address port
    mov dx, 388h
    cmp al, 0
    je .ok
    mov dx, 38Ah
    cmp al, 1
    je .ok
    mov dx, [cs:sb_base]
    cmp al, 2
    je .ok
    add dx, 2
    cmp al, 3
    je .ok
    add dx, 6                   ; base + 8
.ok:
    ret

stage10:                        ; DMA page outside the VM: local failure, DSP still resets
    mov ax, 0103h
    call transfer_setup
    push word [cs:buf_lin + 2]
    mov word [cs:buf_lin + 2], 0F0h
    mov si, 255
    mov bl, 48h
    call dma_start8
    pop word [cs:buf_lin + 2]
    mov ax, 8000
    call dsp_rate
    mov al, 14h
    mov cx, 255
    call dsp_word
    mov ah, 86h                 ; 50 ms: the block would have ended after 32 ms
    xor cx, cx
    mov dx, 50000
    int 15h
    mov ax, [M_IRQS]
    mov [M_FIRQS], ax
    call dsp_reset
    xor ah, ah
    mov [M_FRESET], ax
    ret

stage11:                        ; restore and return to DOS
    call dsp_reset
    cli
    mov al, [cs:old_imr0]
    out 21h, al
    mov al, [cs:old_imr1]
    out 0A1h, al
    xor bh, bh
    mov bl, [cs:vector]
    shl bx, 1
    shl bx, 1
    mov ax, [cs:old_vec]
    mov [bx], ax
    mov ax, [cs:old_vec + 2]
    mov [bx + 2], ax
    sti
    mov word [M_STAGE], 11
    mov si, done_text
.print:
    mov al, [cs:si]
    test al, al
    jz .exit
    out 0E9h, al
    inc si
    jmp .print
.exit:
    mov ax, 4C00h
    int 21h

; ---------------------------------------------------------------- data
stage_table dw stage2, stage3, stage4, stage5, stage6, stage7, stage8, stage9, stage10
; kind: 0 388h/389h, 1 38Ah/38Bh, 2 base+0/1, 3 base+2/3, 4 base+8/9;
; 10h status -> next slot, 11h status after 100 reads, 12h poll status&reg (1 ms waits).
; The controller holds the same register/value list (expected events).
opl_script:
    db 0, 04h, 60h
    db 0, 04h, 80h
    db 10h, 0, 0
    db 0, 02h, 0FFh
    db 0, 04h, 21h
    db 11h, 0, 0
    db 0, 04h, 60h
    db 0, 04h, 80h
    db 1, 0C0h, 31h             ; NEW clear: an array-1 address selects 0C0h
    db 1, 05h, 01h              ; 105h NEW: OPL3 mode
    db 1, 04h, 3Fh              ; 104h four-operator pairs
    db 1, 0C0h, 0F1h            ; 1C0h
    db 0, 0BDh, 3Fh             ; rhythm mode, all drums
    db 3, 0B0h, 31h             ; SB16 2x2h/2x3h -> 1B0h
    db 2, 0A0h, 98h             ; SB16 2x0h/2x1h -> 0A0h
    db 4, 40h, 10h              ; SB16 2x8h/2x9h -> 040h
    db 0, 20h, 01h
    db 0, 23h, 01h
    db 0, 60h, 0F0h
    db 0, 63h, 0F0h
    db 0, 80h, 77h
    db 0, 83h, 77h
    db 0, 0A1h, 98h
    db 0, 0B1h, 31h             ; key on
    db 0, 02h, 00h
    db 0, 03h, 00h
    db 0, 04h, 03h              ; start T1 (20.48 ms) and T2 (81.92 ms)
    db 12h, 40h, 0
    db 12h, 20h, 0
    db 0, 04h, 43h              ; mask T1: its flag clears
    db 10h, 0, 0
    db 0, 04h, 80h              ; IRQ reset
    db 10h, 0, 0
    db 0, 04h, 00h
    db 0, 0B1h, 11h             ; key off
    db 1, 05h, 00h              ; back to OPL2 compatibility
    db 0FFh
blaster_name db 'BLASTER='
done_text db 'VMAUDIO complete', 10, 0
page8 db 87h, 83h, 81h, 82h
page16 db 0, 8Bh, 89h, 8Ah
sb_base dw 220h
irq db 7
dma8 db 1
dma16 db 5
vector db 0
old_vec dw 0, 0
old_imr0 db 0
old_imr1 db 0
buf_lin dw 0, 0
buf_seg dw 0
buf_off dw 0
fill_mode db 0
expect db 1
seq db 0
half db 0
stop_at dw 0FFFFh
exit_cmd db 0DAh
align 16, db 0
dma_area:                       ; 2 x BUF_SIZE of free COM memory after the image
