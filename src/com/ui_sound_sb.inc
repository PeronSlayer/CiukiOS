; Optional ISA Sound Blaster event backend. Unsigned mono 8 kHz, <=2688 B.
; Foreground only, no DSP reset, interrupt-vector ownership or PC speaker.
; Resources come from SB16 mixer80/81 or matching BLASTER for SB Pro.
; Only master-PIC IRQ5/7 and DMA0/1/3 are supported by this polling backend.
SFX_SB_MAX equ 2688

sfx_sb_init:
    call sfx_sb_environment
    mov si,sfx_sb_bases
.next:
    lodsw
    test ax,ax
    jz .unavailable
    mov [sfx_sb_base],ax
    push si
    ; E1 is a nondestructive version query. Never reset an unknown device.
    mov al,0xE1
    call sfx_sb_write
    jc .skip
    call sfx_sb_read
    jc .skip
    mov [sfx_sb_version],al
    call sfx_sb_read
    jc .skip
    mov [sfx_sb_minor],al
    cmp byte [sfx_sb_version],2
    jb .skip
    cmp byte [sfx_sb_version],4
    ja .skip
    call sfx_sb_resources
    jc .skip
    call sfx_sb_idle
    jc .skip
    ; Twice the maximum sound size permits a buffer on either side of the
    ; next physical 64 KiB boundary without allocating a full extra64KiB.
    mov bx,(SFX_SB_MAX*2+15)/16
    mov ah,0x48
    int 0x21
    jc .memory
    mov [sfx_buffer],ax
    shl ax,4
    xor dx,dx
    mov bx,ax
    add bx,SFX_SB_MAX-1
    jnc .buffer
    neg ax
    mov dx,ax
.buffer:
    mov [sfx_sb_offset],dx
    mov byte [sfx_available],2
    mov word [sfx_result],0
    pop si
    ret
.memory:
    mov word [sfx_result],6
    pop si
    ret
.skip:
    pop si
    jmp .next
.unavailable:
    mov word [sfx_result],3
    ret

; BLASTER belongs to the running DOS environment. Bounded walk; no strings
; from arbitrary physical memory and no guessed resources on pre-SB16 cards.
sfx_sb_environment:
    mov word [sfx_sb_env_base],0
    mov word [sfx_sb_env_irq],0xFFFF
    mov word [sfx_sb_env_dma],0xFFFF
    mov ah,0x62
    int 0x21
    mov es,bx
    mov ax,[es:0x2C]
    test ax,ax
    jz .done
    mov es,ax
    xor di,di
.entry:
    cmp di,0x7F00
    jae .done
    cmp byte [es:di],0
    je .done
    mov si,sfx_sb_env_name
    mov bx,di
    mov cx,8
.name:
    mov al,[es:bx]
    inc bx
    and al,0xDF
    cmp al,[si]
    jne .skip
    inc si
    loop .name
    mov di,bx
.token:
    cmp di,0x7F00
    jae .done
    mov al,[es:di]
    inc di
    test al,al
    jz .done
    cmp al,' '
    je .token
    cmp al,9
    je .token
    and al,0xDF
    mov dl,al
    mov bp,10
    cmp al,'A'
    jne .number
    mov bp,16
.number:
    xor bx,bx
    xor cx,cx
.digit:
    mov al,[es:di]
    cmp al,'0'
    jb .number_end
    cmp al,'9'
    jbe .decimal
    and al,0xDF
    cmp al,'A'
    jb .number_end
    cmp al,'F'
    ja .number_end
    sub al,'A'-10
    jmp .value
.decimal:
    sub al,'0'
.value:
    xor ah,ah
    cmp ax,bp
    jae .number_end
    cmp cx,4
    jae .done
    imul bx,bp
    add bx,ax
    inc cx
    inc di
    jmp .digit
.number_end:
    jcxz .remainder
    cmp dl,'A'
    jne .irq
    mov [sfx_sb_env_base],bx
.irq:
    cmp dl,'I'
    jne .dma
    mov [sfx_sb_env_irq],bx
.dma:
    cmp dl,'D'
    jne .remainder
    mov [sfx_sb_env_dma],bx
.remainder:
    mov al,[es:di]
    test al,al
    jz .done
    cmp al,' '
    je .token
    inc di
    cmp di,0x7F00
    jb .remainder
    ret
.skip:
    cmp byte [es:di],0
    je .skip_end
    inc di
    cmp di,0x7F00
    jb .skip
    ret
.skip_end:
    inc di
    jmp .entry
.done:
    ret

sfx_sb_resources:
    cmp byte [sfx_sb_version],4
    jb .environment
    mov al,0x80
    call sfx_sb_mixer_read
    and al,15
    mov byte [sfx_sb_irq],5
    cmp al,2
    je .dma
    mov byte [sfx_sb_irq],7
    cmp al,4
    jne .bad
.dma:
    mov al,0x81
    call sfx_sb_mixer_read
    and al,15
    mov byte [sfx_sb_dma],0
    cmp al,1
    je .ports
    mov byte [sfx_sb_dma],1
    cmp al,2
    je .ports
    mov byte [sfx_sb_dma],3
    cmp al,8
    jne .bad
    jmp .ports
.environment:
    mov ax,[sfx_sb_env_base]
    cmp ax,[sfx_sb_base]
    jne .bad
    mov ax,[sfx_sb_env_irq]
    cmp ax,5
    je .env_irq
    cmp ax,7
    jne .bad
.env_irq:
    mov [sfx_sb_irq],al
    mov ax,[sfx_sb_env_dma]
    cmp ax,1
    jbe .env_dma
    cmp ax,3
    jne .bad
.env_dma:
    mov [sfx_sb_dma],al
.ports:
    mov cl,[sfx_sb_irq]
    mov al,1
    shl al,cl
    mov [sfx_sb_irq_bit],al
    movzx bx,byte [sfx_sb_dma]
    shl bx,1
    mov [sfx_sb_addr_port],bx
    inc bx
    mov [sfx_sb_count_port],bx
    movzx bx,byte [sfx_sb_dma]
    mov al,[sfx_sb_pages+bx]
    movzx ax,al
    mov [sfx_sb_page_port],ax
    clc
    ret
.bad:
    stc
    ret

; Conservative ownership test. A speaker-on DSP, live DMA count, or pending
; IRQ means busy. No portable ISA register can identify a third-party driver
; with a paused/speaker-off stream; hosts must release audio before EXEC.
sfx_sb_idle:
    pushf
    cli
    mov al,0x0B
    out 0x20,al
    in al,0x20
    mov ah,al
    mov al,0x0A
    out 0x20,al
    in al,0x20
    or al,ah
    and al,[sfx_sb_irq_bit]
    popf
    test al,al
    jnz .busy
    mov al,0xD8
    call sfx_sb_write
    jc .busy
    call sfx_sb_read
    jnc .speaker_status
    ; QEMU SB16 does not implement D8. On recognized KVM/TCG guests only,
    ; use QEMU's readable DMA-mask extension and require our channel masked.
    ; Never use this nonportable port on unidentified/physical hardware.
    call sfx_sb_qemu_idle
    jc .busy
    jmp .dma_idle
.speaker_status:
    test al,al
    jnz .busy
.dma_idle:
    call sfx_sb_dma_count
    mov bx,ax
    mov cx,1024
.delay:
    in al,0x80
    loop .delay
    call sfx_sb_dma_count
    cmp ax,bx
    jne .busy
    clc
    ret
.busy:
    stc
    ret

cpu 586                         ; supported floor is Pentium III
sfx_sb_qemu_idle:
    cmp byte [sfx_sb_version],4
    jne .unsupported
    cmp byte [sfx_sb_minor],5
    jne .unsupported
    pushad
    mov eax,1
    cpuid
    bt ecx,31
    jnc .bad
    mov eax,0x40000000
    cpuid
    cmp ebx,'KVMK'
    jne .tcg
    cmp ecx,'VMKV'
    jne .bad
    cmp edx,'M'
    je .mask
.tcg:
    cmp ebx,'TCGT'
    jne .bad
    cmp ecx,'CGTC'
    jne .bad
    cmp edx,'GTCG'
    jne .bad
.mask:
    in al,0x09                 ; QEMU i8257_read_cont(mask), not ISA8237 API
    mov cl,[sfx_sb_dma]
    mov ah,1
    shl ah,cl
    test al,ah
    jz .bad
    popad
    clc
    ret
.bad:
    popad
.unsupported:
    stc
    ret
cpu 386

sfx_sb_start:
    mov word [sfx_result],5
    call sfx_sb_idle
    jc .done
    ; All preparation is complete before claiming PIC/DMA ownership.
    pushf
    cli
    in al,0x21
    mov [sfx_sb_original_mask],al
    or al,[sfx_sb_irq_bit]
    out 0x21,al
    mov byte [sfx_sb_owned],1
    popf
    mov al,0x22
    call sfx_sb_mixer_read
    mov [sfx_sb_master],al
    mov al,0x04
    call sfx_sb_mixer_read
    mov [sfx_sb_pcm],al
    mov al,0x0E
    call sfx_sb_mixer_read
    mov [sfx_sb_filter],al
    and al,0xFD                 ; 8-bit mono for SB Pro
    mov ah,al
    mov al,0x0E
    call sfx_sb_mixer_write
    mov ax,0xFF22
    call sfx_sb_mixer_write
    mov ax,0xFF04
    call sfx_sb_mixer_write
    movzx eax,word [sfx_buffer]
    shl eax,4
    movzx edx,word [sfx_sb_offset]
    add eax,edx
    mov [sfx_sb_physical],eax
    mov cx,[sfx_bytes]
    dec cx
    add cx,ax
    jc .fail
    pushf
    cli
    call sfx_sb_mask_dma
    xor al,al
    out 0x0C,al
    mov al,[sfx_sb_dma]
    or al,0x48
    out 0x0B,al
    mov ax,[sfx_sb_physical]
    mov dx,[sfx_sb_addr_port]
    out dx,al
    mov al,ah
    out dx,al
    mov ax,[sfx_bytes]
    dec ax
    mov dx,[sfx_sb_count_port]
    out dx,al
    mov al,ah
    out dx,al
    mov al,[sfx_sb_physical+2]
    mov dx,[sfx_sb_page_port]
    out dx,al
    mov al,[sfx_sb_dma]
    out 0x0A,al
    popf
    mov al,0xD1
    call sfx_sb_write
    jc .fail
    mov al,0x40
    call sfx_sb_write
    jc .fail
    mov al,131                  ; 1 MHz / (256-131) = 8000 Hz
    call sfx_sb_write
    jc .fail
    mov al,0x14
    call sfx_sb_write
    jc .fail
    mov ax,[sfx_bytes]
    dec ax
    call sfx_sb_write
    jc .fail
    mov al,ah
    call sfx_sb_write
    jc .fail
    xor ax,ax
    mov es,ax
    mov ax,[es:0x46C]
    mov [sfx_tick],ax
    mov byte [sfx_playing],1
    mov word [sfx_result],1
.done:
    ret
.fail:
    mov word [sfx_result],7
    call sfx_sb_stop
    ret

sfx_sb_poll:
    cmp byte [sfx_sb_owned],1
    jne .done
    mov al,0x0A
    out 0x20,al
    in al,0x20
    test al,[sfx_sb_irq_bit]
    jnz sfx_sb_stop
    xor ax,ax
    mov es,ax
    mov ax,[es:0x46C]
    sub ax,[sfx_tick]
    cmp ax,18
    jae .fail
    mov word [sfx_result],1
.done:
    ret
.fail:
    mov word [sfx_result],7
    jmp sfx_sb_stop

sfx_sb_stop:
    cmp byte [sfx_sb_owned],1
    jne .done
    mov al,0xD0
    call sfx_sb_write
    call sfx_sb_mask_dma
    mov al,0xD3
    call sfx_sb_write
    pushf
    cli
    ; Acknowledge only our masked IRQ. PIC poll substitutes INTA; expose just
    ; this line for that read, then issue its specific EOI. Timer, keyboard,
    ; cascade and other pending requests remain latched and are not polled.
    in al,0x21
    mov bl,al
    mov al,[sfx_sb_irq_bit]
    not al
    out 0x21,al
    mov al,0x0C
    out 0x20,al
    in al,0x20
    test al,0x80
    jz .ack_device
    and al,7
    cmp al,[sfx_sb_irq]
    jne .ack_device             ; unreachable when the PIC mask is respected
    or al,0x60
    out 0x20,al
.ack_device:
    mov dx,[sfx_sb_base]
    add dx,0x0E
    in al,dx
    mov al,0x0A
    out 0x20,al
    mov ah,[sfx_sb_irq_bit]
    not ah
    mov al,bl
    and al,ah
    mov ah,[sfx_sb_original_mask]
    and ah,[sfx_sb_irq_bit]
    or al,ah
    out 0x21,al
    mov [sfx_pic1],al
    mov byte [sfx_sb_owned],0
    mov byte [sfx_playing],0
    popf
    mov ah,[sfx_sb_master]
    mov al,0x22
    call sfx_sb_mixer_write
    mov ah,[sfx_sb_pcm]
    mov al,0x04
    call sfx_sb_mixer_write
    mov ah,[sfx_sb_filter]
    mov al,0x0E
    call sfx_sb_mixer_write
.done:
    ret

sfx_sb_return_masks:
    cmp byte [sfx_sb_owned],0
    je .done
    mov al,[sfx_sb_irq_bit]
    or [sfx_pic1],al
.done:
    ret

sfx_sb_mask_dma:
    mov al,[sfx_sb_dma]
    or al,4
    out 0x0A,al
    ret

sfx_sb_dma_count:
    pushf
    cli
    xor al,al
    out 0x0C,al
    mov dx,[sfx_sb_count_port]
    in al,dx
    mov ah,al
    in al,dx
    xchg al,ah
    popf
    ret

; AL command, preserves AX so low/high block-length writes are safe.
sfx_sb_write:
    push ax
    push cx
    push dx
    mov ah,al
    mov dx,[sfx_sb_base]
    add dx,0x0C
    mov cx,4096
.wait:
    in al,dx
    test al,0x80
    jz .ready
    loop .wait
    stc
    jmp .done
.ready:
    mov al,ah
    out dx,al
    clc
.done:
    pop dx
    pop cx
    pop ax
    ret
sfx_sb_read:
    push cx
    push dx
    mov dx,[sfx_sb_base]
    add dx,0x0E
    mov cx,4096
.wait:
    in al,dx
    test al,0x80
    jnz .ready
    loop .wait
    stc
    jmp .done
.ready:
    sub dx,4
    in al,dx
    clc
.done:
    pop dx
    pop cx
    ret
sfx_sb_mixer_read:
    mov dx,[sfx_sb_base]
    add dx,4
    out dx,al
    inc dx
    in al,dx
    ret
sfx_sb_mixer_write:
    mov dx,[sfx_sb_base]
    add dx,4
    out dx,al
    inc dx
    mov al,ah
    out dx,al
    ret

sfx_sb_paths dw sfx_sb_error,sfx_sb_info,sfx_sb_warning,sfx_sb_confirm
sfx_sb_error db '\SYSTEM\SOUNDS\ERROR.SB',0
sfx_sb_info db '\SYSTEM\SOUNDS\INFO.SB',0
sfx_sb_warning db '\SYSTEM\SOUNDS\WARN.SB',0
sfx_sb_confirm db '\SYSTEM\SOUNDS\CONFIRM.SB',0
sfx_sb_bases dw 0x220,0x240,0x260,0x280,0
sfx_sb_pages db 0x87,0x83,0x81,0x82
sfx_sb_env_name db 'BLASTER',('='&0xDF)
sfx_sb_env_base dw 0
sfx_sb_env_irq dw 0xFFFF
sfx_sb_env_dma dw 0xFFFF
sfx_sb_base dw 0
sfx_sb_version db 0
sfx_sb_minor db 0
sfx_sb_irq db 0
sfx_sb_irq_bit db 0
sfx_sb_dma db 0
sfx_sb_owned db 0
sfx_sb_original_mask db 0
sfx_sb_master db 0
sfx_sb_pcm db 0
sfx_sb_filter db 0
sfx_sb_addr_port dw 0
sfx_sb_count_port dw 0
sfx_sb_page_port dw 0
sfx_sb_offset dw 0
sfx_sb_physical dd 0
