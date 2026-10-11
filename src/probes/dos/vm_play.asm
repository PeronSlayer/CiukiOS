; f4-05 DOS-side SB16 / OPL3 output pattern (VMPLAY.COM) for the f4:vm-audio
; probe (f4_probes_vm_audio.c). Mailbox at 0000:0600 as VMAUDIO.COM (separate
; VM): the controller writes the command word, this program runs the stage
; and publishes it; the kernel judges what the mixer rendered, not this code.
; Stages: 1 detection and CT1745 levels at 0 dB; 2 the f4-05 regression, a
; 64-unit 8-bit auto-init double buffer at 8000 Hz over 2048 units (32 blocks,
; ISR refill, DAh from the ISR); 3 16-bit signed stereo auto-init at 12000 Hz
; with voice at level 25 (-12 dB); 4 OPL2 440 Hz note; 5 OPL3 (NEW) 880 Hz
; note routed to the left output only; 6 restore IVT/IMR and return to DOS.
; DSP/DMA/ISR routines are those of vm_audio.asm (f3-05), unchanged.
; Wait loops test their condition with IF clear and sleep with STI;HLT.
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
%define M_RESET   0x60E
%define M_ISRBAD  0x610
%define M_MASTER  0x612
%define M_VOICE   0x614
%define M_FM      0x616
%define M_TICKS   0x618
%define M_OPLSTAT 0x61A
%define M_BUFFER  0x61C      ; dword: linear address of the DMA buffer
%define M_BASE    0x620      ; BLASTER A/I/D/H as parsed
%define M_IRQ     0x622
%define M_DMA     0x624
%define M_HDMA    0x626
%define M_WORDS   0x14       ; 0600h-0627h
%define BUF_SIZE  1024
%define NOTE_TICKS 11        ; BIOS ticks (55 ms) a note is held
%define TAIL_TICKS 3

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
    mov word [M_MAGIC], 0F405h
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
    cmp bx, 6
    jb .next
    call wait_command           ; command 6: clean up and return to DOS
    jmp stage6

wait_command:                   ; BX = command
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

wait_ticks:                     ; AX = BIOS ticks to wait; returns the ticks waited in AX
    push bx
    push cx
    mov cx, ax
    mov bx, [0x46C]
.loop:
    cli
    mov ax, [0x46C]
    sub ax, bx
    cmp ax, cx
    jae .done
    sti
    hlt
    jmp .loop
.done:
    sti
    pop cx
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
    mov si, unity_levels        ; CT1745 master, voice, MIDI/FM at level 31 (0 dB)
.level:
    mov ax, [cs:si]
    test ax, ax
    jz .read
    call mixer_write
    add si, 2
    jmp .level
.read:
    mov ah, 30h
    call mixer_read
    mov [M_MASTER], ax
    mov ah, 32h
    call mixer_read
    mov [M_VOICE], ax
    mov ah, 34h
    call mixer_read
    mov [M_FM], ax
    mov dx, 388h
    in al, dx
    xor ah, ah
    mov [M_OPLSTAT], ax
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
stage2:                         ; the f4-05 regression: 64-unit auto-init double buffer, 32 blocks, 8000 Hz
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

stage3:                         ; 16-bit signed stereo at 12000 Hz, voice at level 25 (-12 dB)
    mov ax, 32C8h               ; kept after the stage: the mixer renders ~56 ms behind
    call mixer_write            ; the VM and reads the CT1745 levels when it renders
    mov ax, 33C8h
    call mixer_write
    call stage3_stream
    ret

stage3_stream:                         ; 16-bit signed stereo auto-init, 64-frame blocks at 12000 Hz
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

stage4:                         ; OPL2: 440 Hz sine (AM pair, modulator at TL 63), held, released
    mov si, opl2_note
    call opl_script
    mov ax, NOTE_TICKS
    call wait_ticks
    mov [M_TICKS], ax
    mov si, opl2_off
    call opl_script
    mov ax, TAIL_TICKS
    call wait_ticks
    ret

stage5:                         ; OPL3 NEW: 880 Hz on output A (left) only, then OPL2 again
    mov si, opl3_note
    call opl_script
    mov ax, NOTE_TICKS
    call wait_ticks
    mov [M_TICKS], ax
    mov si, opl3_off
    call opl_script
    mov ax, TAIL_TICKS
    call wait_ticks
    mov si, opl3_exit
    call opl_script
    ret

opl_script:                     ; CS:SI = (bank, register, value) triples, bank FFh ends
.next:
    mov al, [cs:si]
    cmp al, 0FFh
    je .done
    mov dx, 388h
    test al, al
    jz .port
    mov dx, 38Ah
.port:
    mov al, [cs:si + 1]
    out dx, al
    mov cx, 6                   ; AdLib address delay: 6 status reads
.wa:
    in al, dx
    loop .wa
    inc dx
    mov al, [cs:si + 2]
    out dx, al
    dec dx
    mov cx, 35                  ; data delay: 35 status reads
.wd:
    in al, dx
    loop .wd
    add si, 3
    jmp .next
.done:
    ret

stage6:                         ; restore and return to DOS
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
    mov word [M_STAGE], 6
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
stage_table dw stage2, stage3, stage4, stage5
unity_levels dw 30F8h, 31F8h, 32F8h, 33F8h, 34F8h, 35F8h, 0
; OPL scripts: bank 0 = 388h/389h, 1 = 38Ah/38Bh (array 1).
opl2_note:
    db 0, 01h, 20h              ; waveform select enable
    db 0, 0C0h, 01h             ; channel 0: AM (additive), no feedback
    db 0, 20h, 21h              ; modulator: sustain, multiple 1
    db 0, 23h, 21h              ; carrier: sustain, multiple 1
    db 0, 40h, 3Fh              ; modulator TL 63 (-47 dB)
    db 0, 43h, 00h              ; carrier TL 0
    db 0, 60h, 0F0h             ; attack 15, decay 0
    db 0, 63h, 0F0h
    db 0, 80h, 0Fh              ; sustain level 0, release 15
    db 0, 83h, 0Fh
    db 0, 0E0h, 00h             ; sine
    db 0, 0E3h, 00h
    db 0, 0A0h, 44h             ; F-number 244h
    db 0, 0B0h, 32h             ; key on, block 4: 440 Hz
    db 0FFh
opl2_off:
    db 0, 0B0h, 12h             ; key off
    db 0FFh
opl3_note:
    db 1, 05h, 01h              ; 105h NEW: OPL3 mode
    db 0, 0C0h, 11h             ; channel 0: AM, output A (left) only
    db 0, 0A0h, 44h
    db 0, 0B0h, 36h             ; key on, block 5: 880 Hz
    db 0FFh
opl3_off:
    db 0, 0B0h, 16h             ; key off
    db 0FFh
opl3_exit:
    db 0, 0C0h, 01h
    db 1, 05h, 00h              ; back to OPL2 compatibility
    db 0FFh
blaster_name db 'BLASTER='
done_text db 'VMPLAY complete', 10, 0
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
