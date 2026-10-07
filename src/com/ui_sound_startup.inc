; Native startup playback shares the existing 64 KiB event/music allocation.
; A descriptor owns one 2 KiB slot; at most 31 of the 32 slots are queued.
; Only foreground polling performs file I/O. No startup-sized DOS allocation.
%define SFX_BOOT_SLOT_BYTES 2048
%define SFX_BOOT_GUARD_SLOTS 31
%define SFX_BOOT_POLL_SLOTS 8

sfx_boot_pcm:
    mov byte [sfx_audio_stage],10
    mov byte [sfx_boot_event],'F'
    mov dword [sfx_boot_loaded],0
    mov dword [sfx_boot_played],0
    mov dword [sfx_boot_total],0
    mov dword [sfx_boot_left],0
    mov word [sfx_audio_sr],0
    mov byte [sfx_audio_civ],0
    mov byte [sfx_boot_restarts],0
    mov byte [sfx_boot_eof_wait],0
    mov byte [sfx_boot_active],1
    mov word [sfx_boot_file],0xFFFF
    mov dx,sfx_boot_path
    mov ax,0x3D00
    int 0x21
    jc .failure
    mov [sfx_boot_file],ax
    mov bx,ax
    mov byte [sfx_audio_stage],11
    xor cx,cx
    xor dx,dx
    mov ax,0x4202
    int 0x21
    jc .failure
    movzx edx,dx
    shl edx,16
    mov dx,ax
    test dl,3
    jnz .failure
    test edx,edx
    jz .failure
    cmp edx,32*16384
    ja .failure
    mov [sfx_boot_left],edx
    mov [sfx_boot_total],edx
    xor cx,cx
    xor dx,dx
    mov ax,0x4200
    int 0x21
    jc .failure
    mov word [sfx_result],5
    mov byte [sfx_audio_stage],13
    call ac97_enable_io
    jc .failure
    mov byte [sfx_audio_stage],14
    call ac97_enable_pci
    jc .failure
    xor ah,ah
    int 0x1A
    mov [sfx_tick],dx
    mov word [sfx_timeout],90         ; complete track deadline, including gaps
    call sfx_boot_prime
    jc .failure
    mov byte [sfx_boot_event],'S'
    call sfx_audio_log_boot
    mov byte [sfx_boot_event],'X'      ; a subsequent explicit stop/cancel
    mov word [sfx_result],1
    ret
.failure:
    call ac97_cleanup_output         ; verifies halt/BME-off before ring reuse
    jnc .freed
    mov word [sfx_result],7
.freed:
    jmp sfx_boot_free

; Called only before the initial run. Resetting the PCM-out engine gives an
; unambiguous CIV=0, with no change to the codec or global AC-link.
sfx_boot_prime:
    mov byte [sfx_audio_stage],15
    mov word [sfx_result],7
    mov dx,[nabm_base]
    add dx,ICH_PO_CR
    xor al,al
    out dx,al
    mov al,ICH_CR_RESET
    out dx,al
    mov cx,0xFFFF
.reset:
    in al,dx
    test al,ICH_CR_RESET
    jz .ready
    loop .reset
    stc
    ret
.ready:
    mov word [sfx_boot_count],0
    mov byte [sfx_boot_next],0
    mov byte [sfx_boot_civ],0
    mov byte [sfx_playing],0
    mov dx,[nabm_base]
    add dx,ICH_PO_SR
    mov ax,0x1C
    out dx,ax
    mov ax,cs
    movzx eax,ax
    shl eax,4
    add eax,ac97_bdl
    mov dx,[nabm_base]
    add dx,ICH_PO_BDBAR
    out dx,eax
    mov byte [sfx_audio_stage],16
    mov byte [sfx_boot_budget],SFX_BOOT_GUARD_SLOTS
    call sfx_boot_fill
    jc .out
    cmp word [sfx_boot_count],0
    je .empty
    mov dx,[nabm_base]
    add dx,ICH_PO_CR
    mov al,ICH_CR_RUN
    out dx,al
    mov dx,[nabm_base]
    add dx,ICH_PO_SR
    in ax,dx
    mov [sfx_audio_sr],ax
    sub dx,2
    in al,dx
    and al,31
    mov [sfx_audio_civ],al
    mov byte [sfx_playing],1
    mov byte [sfx_audio_stage],17
    mov word [sfx_result],1
    clc
.out:
    ret
.empty:
    mov word [sfx_result],4
    stc
    ret

; Software count includes CIV and every descriptor through LVI. One spare
; slot and retirement-before-write keep both current and prefetched buffers
; immutable. Publish LVI only after DOS read and the entire BDL entry finish.
sfx_boot_fill:
.next:
    cmp dword [sfx_boot_left],0
    je .done
    cmp word [sfx_boot_count],SFX_BOOT_GUARD_SLOTS
    jae .done
    cmp byte [sfx_boot_budget],0
    je .done
    mov ecx,SFX_BOOT_SLOT_BYTES
    cmp [sfx_boot_left],ecx
    jae .chunk
    mov ecx,[sfx_boot_left]
.chunk:
    mov [sfx_boot_chunk],cx
    movzx dx,byte [sfx_boot_next]
    shl dx,11
    mov [sfx_boot_offset],dx
    mov bx,[sfx_boot_file]
    push ds
    mov ds,[cs:sfx_buffer]
    mov ah,0x3F
    int 0x21
    pop ds
    jc .read_error
    cmp ax,[sfx_boot_chunk]
    jne .read_error
    movzx bx,byte [sfx_boot_next]
    shl bx,3
    movzx eax,word [sfx_buffer]
    shl eax,4
    movzx edx,word [sfx_boot_offset]
    add eax,edx
    mov [ac97_bdl+bx],eax
    mov ax,[sfx_boot_chunk]
    shr ax,1                         ; 16-bit samples, stereo already aligned
    mov [ac97_bdl+bx+4],ax
    mov word [ac97_bdl+bx+6],0
    movzx eax,word [sfx_boot_chunk]
    sub [sfx_boot_left],eax
    add [sfx_boot_loaded],eax
    inc word [sfx_boot_count]
    mov dx,[nabm_base]
    add dx,ICH_PO_SR
    mov ax,0x0C                      ; discard old completion, retain FIFO error
    out dx,ax
    mov dx,[nabm_base]
    add dx,ICH_PO_LVI
    mov al,[sfx_boot_next]
    out dx,al
    inc byte [sfx_boot_next]
    and byte [sfx_boot_next],31
    dec byte [sfx_boot_budget]
    jmp .next
.read_error:
    mov word [sfx_result],4
    stc
    ret
.done:
    clc
    ret

sfx_boot_poll:
    mov byte [sfx_audio_stage],18
    mov dx,[nabm_base]
    add dx,ICH_PO_SR
    in ax,dx
    mov [sfx_audio_sr],ax
    test ax,ICH_SR_ERROR
    jnz .dma_error
    push ax
    sub dx,2
    in al,dx
    and al,31
    mov [sfx_audio_civ],al
    pop ax
    test ax,ICH_SR_DCH
    jz .running
    test ax,4                        ; DCH alone is not proof of completion
    jz .deadline
    mov eax,[sfx_boot_loaded]
    mov [sfx_boot_played],eax
    mov word [sfx_boot_count],0
    cmp dword [sfx_boot_left],0
    je .completed
    ; The engine still has RUN=1. Publish the next prepared slot after the
    ; drained LVI; hardware leaves CELV/DCH and continues from that slot.
    ; Keeping the engine state also preserves samples still in its FIFO.
    inc byte [sfx_boot_restarts]
    mov al,[sfx_boot_next]
    mov [sfx_boot_civ],al
    mov byte [sfx_boot_budget],SFX_BOOT_POLL_SLOTS
    call sfx_boot_fill
    jc .failed
    jmp .deadline
.running:
    mov al,[sfx_audio_civ]           ; PCM-out CIV captured above, read-only
    mov bl,al
    sub al,[sfx_boot_civ]
    and ax,31
    cmp ax,[sfx_boot_count]
    ja .dma_error                    ; impossible advancement: never reuse it
    mov cx,ax
    jcxz .refill
    sub [sfx_boot_count],ax
    push bx
    movzx bx,byte [sfx_boot_civ]
.retire:
    mov si,bx
    shl si,3
    movzx eax,word [ac97_bdl+si+4]
    shl eax,1
    add [sfx_boot_played],eax
    inc bx
    and bx,31
    loop .retire
    pop bx
    mov [sfx_boot_civ],bl
.refill:
    mov byte [sfx_boot_budget],SFX_BOOT_POLL_SLOTS
    call sfx_boot_fill
    jc .failed
.deadline:
    xor ax,ax
    mov es,ax
    mov ax,[es:0x046C]
    sub ax,[sfx_tick]
    cmp ax,[sfx_timeout]
    jae .dma_error
    mov word [sfx_result],1
    ret
.completed:
    ; LVBCI proves the final memory fetch, not transmission across AC-link.
    ; Let the output FIFO drain for at least one BIOS tick before reset/free.
    xor ax,ax
    mov es,ax
    mov ax,[es:0x046C]
    cmp byte [sfx_boot_eof_wait],0
    jne .eof_deadline
    mov byte [sfx_boot_eof_wait],1
    mov [sfx_boot_eof_tick],ax
.eof_deadline:
    sub ax,[sfx_boot_eof_tick]
    jz .eof_wait
    mov byte [sfx_boot_event],'E'
    mov word [sfx_result],0
    jmp sfx_stop
.eof_wait:
    mov word [sfx_result],1
    ret
.dma_error:
    mov word [sfx_result],7
.failed:
    mov byte [sfx_boot_event],'F'
    jmp sfx_stop

sfx_boot_free:
    cmp byte [sfx_boot_active],0
    je .done
    ; This routine is reached only after verified DMA cleanup (or before
    ; enabling DMA). No free/close while hardware still owns the ring/BDL.
    mov byte [sfx_playing],0
    mov bx,[sfx_boot_file]
    cmp bx,0xFFFF
    je .log
    mov ah,0x3E
    int 0x21
    mov word [sfx_boot_file],0xFFFF
.log:
    call sfx_audio_log_boot
    mov byte [sfx_boot_active],0
    mov word [sfx_boot_count],0
.done:
    ret

; The readback is diagnostic only; an individual failed read remains FFFFh
; and does not change the prepared codec/result. Never read after PCI restore.
sfx_audio_snapshot_mixer:
    pushf
    push ax
    push dx
    mov dx,[nam_base]
    add dx,AC97_MASTER_VOL
    call ac97_codec_read
    jc .headphone
    mov [sfx_audio_master],ax
.headphone:
    mov dx,[nam_base]
    add dx,AC97_HEADPHONE_VOL
    call ac97_codec_read
    jc .pcm
    mov [sfx_audio_headphone],ax
.pcm:
    mov dx,[nam_base]
    add dx,AC97_PCM_OUT_VOL
    call ac97_codec_read
    jc .done
    mov [sfx_audio_pcm],ax
.done:
    pop dx
    pop ax
    popf
    ret

; One init and at most one start/terminal record per loaded CSFX instance.
; DOS logging is confined to transitions; ordinary polls/paint do no log I/O.
sfx_audio_log_init:
    cmp byte [sfx_audio_init_logged],0
    jne .done
    mov byte [sfx_audio_init_logged],1
    push ax
    mov al,'I'
    call sfx_audio_log
    pop ax
.done:
    ret

sfx_audio_log_boot:
    push ax
    mov al,[sfx_boot_event]
    cmp al,'S'
    jne .terminal
    cmp byte [sfx_audio_start_logged],0
    jne .done
    mov byte [sfx_audio_start_logged],1
    jmp .write
.terminal:
    cmp byte [sfx_audio_end_logged],0
    jne .done
    mov byte [sfx_audio_end_logged],1
.write:
    call sfx_audio_log
.done:
    pop ax
    ret

; AL event I=init S=start E=EOF F=failure X=explicit stop. All numbers hex;
; largest is DOS paragraphs. A failed logger never changes the sound result.
sfx_audio_log:
    pushfd
    pushad
    push ds
    push es
    push cs
    pop ds
    cld
    mov [sfx_audio_line_event],al
    movzx ax,byte [sfx_audio_stage]
    mov di,sfx_audio_line_stage
    call sfx_audio_hex
    mov ax,[sfx_result]
    mov di,sfx_audio_line_result
    call sfx_audio_hex
    mov ax,[pci_device_id]
    mov di,sfx_audio_line_pci
    call sfx_audio_hex
    mov ax,[pci_device_id]
    test ax,ax
    jz .pci_vendor
    mov ax,0x8086
.pci_vendor:
    mov di,sfx_audio_line_vendor
    call sfx_audio_hex
    mov ax,[pci_subsystem_vendor]
    mov di,sfx_audio_line_sub
    call sfx_audio_hex
    mov ax,[codec_vendor_1]
    mov di,sfx_audio_line_codec1
    call sfx_audio_hex
    mov ax,[codec_vendor_2]
    mov di,sfx_audio_line_codec2
    call sfx_audio_hex
    mov ax,[codec_power_status]
    mov di,sfx_audio_line_power
    call sfx_audio_hex
    mov ax,[sfx_audio_master]
    mov di,sfx_audio_line_master
    call sfx_audio_hex
    mov ax,[sfx_audio_headphone]
    mov di,sfx_audio_line_headphone
    call sfx_audio_hex
    mov ax,[sfx_audio_pcm]
    mov di,sfx_audio_line_pcm
    call sfx_audio_hex
    mov ax,[sfx_buffer]
    mov di,sfx_audio_line_ring
    call sfx_audio_hex
    mov ax,[sfx_audio_largest]
    mov di,sfx_audio_line_largest
    call sfx_audio_hex
    mov ax,[sfx_boot_total+2]
    mov di,sfx_audio_line_total
    call sfx_audio_hex
    mov ax,[sfx_boot_total]
    call sfx_audio_hex
    mov ax,[sfx_boot_loaded+2]
    mov di,sfx_audio_line_loaded
    call sfx_audio_hex
    mov ax,[sfx_boot_loaded]
    call sfx_audio_hex
    mov ax,[sfx_boot_played+2]
    mov di,sfx_audio_line_played
    call sfx_audio_hex
    mov ax,[sfx_boot_played]
    call sfx_audio_hex
    movzx ax,byte [sfx_boot_restarts]
    mov di,sfx_audio_line_restarts
    call sfx_audio_hex
    mov ax,[sfx_audio_sr]
    mov di,sfx_audio_line_sr
    call sfx_audio_hex
    movzx ax,byte [sfx_audio_civ]
    mov di,sfx_audio_line_civ
    call sfx_audio_hex
    mov dx,sfx_audio_log_path
    mov ax,0x3D01
    int 0x21
    jnc .open
    xor cx,cx
    mov ah,0x3C
    int 0x21
    jc .done
.open:
    mov bx,ax
    xor cx,cx
    xor dx,dx
    mov ax,0x4202
    int 0x21
    jc .close
    mov dx,sfx_audio_line
    mov cx,sfx_audio_line_end-sfx_audio_line
    mov ah,0x40
    int 0x21
.close:
    mov ah,0x3E
    int 0x21
.done:
    pop es
    pop ds
    popad
    popfd
    ret

sfx_audio_hex:
    mov cx,4
.digit:
    rol ax,4
    mov dl,al
    and dl,15
    add dl,'0'
    cmp dl,'9'
    jbe .store
    add dl,7
.store:
    mov [di],dl
    inc di
    loop .digit
    ret

sfx_boot_active db 0
sfx_boot_event db 'X'
sfx_boot_file dw 0xFFFF
sfx_boot_count dw 0
sfx_boot_next db 0
sfx_boot_civ db 0
sfx_boot_budget db 0
sfx_boot_restarts db 0
sfx_boot_eof_wait db 0
sfx_boot_eof_tick dw 0
sfx_boot_chunk dw 0
sfx_boot_offset dw 0
align 4
sfx_boot_left dd 0
sfx_boot_total dd 0
sfx_boot_loaded dd 0
sfx_boot_played dd 0
sfx_audio_stage db 0
sfx_audio_init_logged db 0
sfx_audio_start_logged db 0
sfx_audio_end_logged db 0
sfx_audio_largest dw 0
sfx_audio_master dw 0xFFFF
sfx_audio_headphone dw 0xFFFF
sfx_audio_pcm dw 0xFFFF
sfx_audio_sr dw 0
sfx_audio_civ db 0
sfx_audio_log_path db '\SYSTEM\AUDIO.LOG',0
sfx_audio_line db '[AUDIO] native event='
sfx_audio_line_event db 'I'
    db ' stage='
sfx_audio_line_stage db '0000'
    db ' result='
sfx_audio_line_result db '0000'
    db ' pci='
sfx_audio_line_vendor db '0000:'
sfx_audio_line_pci db '0000'
    db ' subvendor='
sfx_audio_line_sub db '0000'
    db ' codec='
sfx_audio_line_codec1 db '0000:'
sfx_audio_line_codec2 db '0000'
    db ' power='
sfx_audio_line_power db '0000'
    db ' master='
sfx_audio_line_master db 'FFFF'
    db ' hp='
sfx_audio_line_headphone db 'FFFF'
    db ' pcm='
sfx_audio_line_pcm db 'FFFF'
    db ' ring='
sfx_audio_line_ring db '0000'
    db ' largest='
sfx_audio_line_largest db '0000'
    db ' total='
sfx_audio_line_total db '00000000'
    db ' loaded='
sfx_audio_line_loaded db '00000000'
    db ' played='
sfx_audio_line_played db '00000000'
    db ' restarts='
sfx_audio_line_restarts db '0000'
    db ' sr='
sfx_audio_line_sr db '0000'
    db ' civ='
sfx_audio_line_civ db '0000',13,10
sfx_audio_line_end:
