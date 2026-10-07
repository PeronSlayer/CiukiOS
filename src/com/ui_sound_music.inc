; svc 8 music stream adapter. 32-entry ICH BDL, one 2 KiB (512-frame)
; descriptor per ring slot. The caller owns decode scheduling; this module owns
; only the 64 KiB DMA ring and polls CIV/PICB without installing an IRQ.

sfx_music_dispatch:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es
    mov ax,[sfx_music_packet_seg]
    mov es,ax
    mov di,[sfx_music_packet_off]
    mov word [sfx_result],0
    cmp word [es:di],30
    jb .bad
    mov ax,[sfx_music_op]
    cmp ax,0
    je .open
    cmp ax,1
    je .write
    cmp ax,2
    je .poll
    cmp ax,3
    je .pause
    cmp ax,4
    je .seek
    cmp ax,5
    je .volume
    cmp ax,6
    je .close
    jmp .bad
.open:
    call sfx_music_open
    jmp .status
.write:
    call sfx_music_write
    jmp .status
.poll:
    call sfx_music_publish_eos
    cmp word [sfx_result],7
    je .dma
    call sfx_music_update
    jc .dma
    jmp .report
.pause:
    cmp byte [sfx_music_owned],0
    je .bad
    mov ax,[es:di+28]
    test ax,1
    jz .resume
    mov dx,[nabm_base]
    add dx,ICH_PO_CR
    xor al,al
    out dx,al
    mov byte [sfx_music_paused],1
    jmp .report
.resume:
    mov byte [sfx_music_paused],0
    cmp byte [sfx_music_started],0
    jne .resume_active
    call sfx_music_publish
    jmp .report
.resume_active:
    mov dx,[nabm_base]
    add dx,ICH_PO_CR
    mov al,ICH_CR_RUN
    out dx,al
    jmp .report
.seek:
    cmp byte [sfx_music_owned],0
    je .bad
    cmp byte [sfx_music_started],0
    je .seek_stopped
    call ac97_quiesce_output
    jc .dma
.seek_stopped:
    mov ax,[sfx_music_packet_seg]
    mov es,ax
    mov di,[sfx_music_packet_off]
    mov eax,[es:di+12]
    mov [sfx_music_base],eax
    mov al,[es:di+28]
    and al,1
    mov [sfx_music_paused],al
    mov dword [sfx_music_queued],0
    mov dword [sfx_music_played],0
    mov dword [sfx_music_published],0
    mov dword [sfx_music_done],0
    mov word [sfx_music_write_pos],0
    mov byte [sfx_music_started],0
    mov byte [sfx_music_eos],0
    jmp .report
.volume:
    cmp byte [sfx_music_owned],0
    je .bad
    call sfx_music_set_volume
    jmp .status
.close:
    call sfx_music_close
    jmp .status
.report:
    call sfx_music_update
    jc .dma
    mov ax,[sfx_music_packet_seg]
    mov es,ax
    mov di,[sfx_music_packet_off]
    mov eax,[sfx_music_base]
    add eax,[sfx_music_played]
    mov [es:di+12],eax
    mov eax,[sfx_music_queued]
    sub eax,[sfx_music_played]
    mov [es:di+16],eax
    mov ax,48000
    mov [es:di+4],ax
    mov word [es:di+6],0
    mov word [es:di+2],1
    test byte [sfx_music_paused],1
    jz .report_flags
    or word [es:di+28],1
.report_flags:
    cmp byte [sfx_music_eos],0
    je .report_done
    or word [es:di+28],2
.report_done:
    mov word [sfx_result],0
    jmp .done
.status:
    mov ax,[sfx_music_packet_seg]
    mov es,ax
    mov di,[sfx_music_packet_off]
    mov ax,[sfx_result]
    mov [es:di+2],ax
    jmp .done
.bad:
    mov word [sfx_result],5
    jmp .status
.dma:
    mov word [sfx_result],7
    jmp .status
.done:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

sfx_music_open:
    cmp byte [sfx_music_owned],0
    jne .busy
    cmp byte [sfx_available],1
    je .ready
    call sfx_init
    cmp byte [sfx_available],1
    jne .unavailable
.ready:
    cmp byte [sfx_playing],1
    jne .cue_stopped
    call sfx_stop
    cmp word [sfx_result],7
    je .unavailable
.cue_stopped:
    call ac97_enable_io
    jc .unavailable
    call ac97_enable_pci
    jc .unavailable
    mov dx,[nam_base]
    add dx,AC97_MASTER_VOL
    in ax,dx
    mov [sfx_music_saved_volume],ax
    mov byte [sfx_music_owned],1
    mov byte [sfx_music_started],0
    mov byte [sfx_music_paused],0
    mov byte [sfx_music_eos],0
    mov dword [sfx_music_base],0
    mov dword [sfx_music_queued],0
    mov dword [sfx_music_played],0
    mov dword [sfx_music_published],0
    mov dword [sfx_music_done],0
    mov word [sfx_music_write_pos],0
    call sfx_music_set_volume
    jmp .ok
.busy:
    mov word [sfx_result],5
    ret
.unavailable:
    call boot_restore_pci
    mov word [sfx_result],3
    ret
.ok:
    cmp word [sfx_result],7
    jne .open_success
    call sfx_music_close
    mov word [sfx_result],7
    ret
.open_success:
    mov word [sfx_result],0
    ret

; WRITE consumes at most a descriptor-safe amount and never lets the source
; offset wrap the caller's 64 KiB segment.
sfx_music_write:
    cmp byte [sfx_music_owned],0
    je .busy
    call sfx_music_update
    jc .dma
    mov ax,[sfx_music_packet_seg]
    mov es,ax
    mov di,[sfx_music_packet_off]
    mov cx,[es:di+24]
    test cx,cx
    jz .invalid
    cmp cx,16384
    ja .invalid
    mov ax,[es:di+22]
    movzx edx,cx
    shl edx,2
    movzx eax,ax
    add eax,edx
    cmp eax,65536
    jae .invalid
    mov eax,[sfx_music_queued]
    sub eax,[sfx_music_played]
    cmp eax,15872
    jae .busy
    mov bx,15872
    sub bx,ax
    shr bx,1
    shr bx,1
    cmp cx,bx
    jbe .count_ready
    mov cx,bx
.count_ready:
    test cx,cx
    jz .busy
    mov [es:di+24],cx
    movzx eax,word [sfx_music_write_pos]
    shl eax,2
    mov [sfx_music_ring_off],ax
    mov ax,[es:di+22]
    mov [sfx_music_src_off],ax
    mov ax,[es:di+20]
    mov [sfx_music_src_seg],ax
    mov ax,cx
    shl ax,1
    shl ax,1
    mov [sfx_music_copy_bytes],ax
    call sfx_music_copy
    jc .invalid
    movzx eax,word [es:di+24]
    add [sfx_music_queued],eax
    mov ax,[es:di+24]
    add [sfx_music_write_pos],ax
    and word [sfx_music_write_pos],0x3FFF
    mov word [sfx_result],0
    call sfx_music_publish
    cmp word [sfx_result],7
    je .return
    mov word [sfx_result],0
    ret
.return:
    ret
.busy:
    mov word [sfx_result],5
    ret
.invalid:
    mov word [sfx_result],4
    ret
.dma:
    mov word [sfx_result],7
    ret

; Copy caller PCM into the DMA ring. REP MOVSB copies DS:SI to ES:DI;
; keep the driver data references CS-relative while DS names the caller.
sfx_music_copy:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es
    mov ax,[cs:sfx_music_src_seg]
    mov ds,ax
    mov si,[cs:sfx_music_src_off]
    mov ax,[cs:sfx_buffer]
    mov es,ax
    mov di,[cs:sfx_music_ring_off]
    mov cx,[cs:sfx_music_copy_bytes]
    mov bx,cx
    mov dx,di
    neg dx
    jnz .span
    mov dx,0xFFFF
.span:
    cmp bx,dx
    jbe .first
    mov cx,dx
.first:
    mov [cs:sfx_music_copy_first],cx
    rep movsb
    mov ax,[cs:sfx_music_copy_bytes]
    sub ax,[cs:sfx_music_copy_first]
    mov cx,ax
    jz .finish
    xor di,di
    rep movsb
.finish:
    clc
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

sfx_music_publish:
    ; Append each complete 512-frame descriptor. Ring capacity retains one
    ; descriptor as a guard so CIV can never catch an incompletely written slot.
.next:
    mov eax,[sfx_music_queued]
    shr eax,9
    cmp eax,[sfx_music_published]
    jbe .start
    mov eax,[sfx_music_published]
    and eax,31
    mov bx,ax
    shl bx,3
    movzx eax,word [sfx_buffer]
    shl eax,4
    movzx ecx,bx
    shl ecx,8
    add eax,ecx
    mov [sfx_music_bdl+bx],eax
    mov word [sfx_music_bdl+bx+4],1024
    mov word [sfx_music_bdl+bx+6],0
    mov dx,[nabm_base]
    add dx,ICH_PO_LVI
    mov al,[sfx_music_published]
    and al,31
    out dx,al
    inc dword [sfx_music_published]
    jmp .next
.start:
    cmp byte [sfx_music_started],0
    jne .done
    cmp byte [sfx_music_paused],0
    jne .done
    cmp dword [sfx_music_published],0
    je .done
    mov dx,[nabm_base]
    add dx,ICH_PO_CR
    xor al,al
    out dx,al
    mov al,ICH_CR_RESET
    out dx,al
    mov cx,0xFFFF
.reset_wait:
    in al,dx
    test al,ICH_CR_RESET
    jz .reset_done
    loop .reset_wait
    mov word [sfx_result],7
    ret
.reset_done:
    mov dx,[nabm_base]
    add dx,ICH_PO_SR
    mov ax,0x1C
    out dx,ax
    mov ax,cs
    movzx eax,ax
    shl eax,4
    add eax,sfx_music_bdl
    mov dx,[nabm_base]
    add dx,ICH_PO_BDBAR
    out dx,eax
    mov dx,[nabm_base]
    add dx,ICH_PO_LVI
    mov al,[sfx_music_published]
    dec al
    and al,31
    out dx,al
    mov dx,[nabm_base]
    add dx,ICH_PO_SR
    mov ax,0x1C
    out dx,ax
    mov dx,[nabm_base]
    add dx,ICH_PO_CR
    mov al,ICH_CR_RUN
    out dx,al
    mov word [sfx_music_last_civ],0
    mov byte [sfx_music_started],1
.done:
    ret

sfx_music_publish_eos:
    cmp byte [sfx_music_eos],0
    jne .done
    mov ax,[sfx_music_packet_seg]
    mov es,ax
    mov di,[sfx_music_packet_off]
    test word [es:di+28],2
    jz .done
    mov byte [sfx_music_eos],1
    mov eax,[sfx_music_queued]
    and eax,511
    jz .done
    mov eax,[sfx_music_published]
    and eax,31
    mov bx,ax
    shl bx,3
    movzx eax,word [sfx_buffer]
    shl eax,4
    movzx ecx,bx
    shl ecx,8
    add eax,ecx
    mov [sfx_music_bdl+bx],eax
    mov eax,[sfx_music_queued]
    and eax,511
    shl eax,1
    mov [sfx_music_bdl+bx+4],ax
    mov word [sfx_music_bdl+bx+6],0
    mov dx,[nabm_base]
    add dx,ICH_PO_LVI
    mov al,[sfx_music_published]
    and al,31
    out dx,al
    inc dword [sfx_music_published]
    call sfx_music_publish
.done:
    ret

sfx_music_update:
    cmp byte [sfx_music_owned],0
    je .out
    cmp byte [sfx_music_started],0
    je .out
    mov dx,[nabm_base]
    add dx,ICH_PO_SR
    in ax,dx
    test ax,ICH_SR_ERROR
    jnz .error
    test ax,ICH_SR_DCH
    jz .running
    cmp byte [sfx_music_paused],0
    jne .out
    jmp .halted
.running:
    mov dx,[nabm_base]
    add dx,0x14
    in al,dx
    xor ah,ah
    and ax,31
    mov bx,ax
    sub ax,[sfx_music_last_civ]
    and ax,31
    movzx eax,ax
    add [sfx_music_done],eax
    mov [sfx_music_last_civ],bx
    mov dx,[nabm_base]
    add dx,0x18
    in ax,dx
    mov [sfx_music_picb],ax
    mov bx,[sfx_music_last_civ]
    and bx,31
    shl bx,3
    mov ax,[sfx_music_bdl+bx+4]
    sub ax,[sfx_music_picb]
    shr ax,1
    movzx eax,ax
    mov ebx,[sfx_music_done]
    shl ebx,9
    add eax,ebx
    cmp eax,[sfx_music_queued]
    jbe .save
    mov eax,[sfx_music_queued]
.save:
    mov [sfx_music_played],eax
    clc
    ret
.halted:
    cmp byte [sfx_music_eos],0
    jne .eos_done
    ; Queue underrun is a legal producer/consumer boundary. All published
    ; frames drained, so rebase before the next descriptor run.
    mov eax,[sfx_music_queued]
    add [sfx_music_base],eax
    mov dword [sfx_music_queued],0
    mov dword [sfx_music_played],0
    mov dword [sfx_music_published],0
    mov dword [sfx_music_done],0
    mov word [sfx_music_write_pos],0
    mov byte [sfx_music_started],0
    clc
    ret
.eos_done:
    mov eax,[sfx_music_queued]
    mov [sfx_music_played],eax
    mov byte [sfx_music_started],0
.out:
    clc
    ret
.error:
    stc
    ret

sfx_music_quiesce:
    cmp byte [sfx_music_started],0
    je .out
    call ac97_quiesce_output
    mov byte [sfx_music_started],0
    jc .fail
.out:
    clc
    ret
.fail:
    stc
    ret

sfx_music_set_volume:
    mov ax,[sfx_music_packet_seg]
    mov es,ax
    mov di,[sfx_music_packet_off]
    mov ax,[es:di+26]
    cmp ax,100
    jbe .range
    mov ax,100
.range:
    mov bx,31
    mov cx,100
    sub cx,ax
    mov ax,cx
    mul bx
    mov bx,100
    div bx
    and ax,31
    mov cx,ax
    shl ax,8
    or ax,cx
    cmp word [es:di+26],0
    jne .write
    or ax,0x8000
.write:
    mov dx,[nam_base]
    add dx,AC97_MASTER_VOL
    call ac97_codec_write
    jc .bad
    mov word [sfx_result],0
    ret
.bad:
    mov word [sfx_result],7
    ret

; The shell's periodic service poll has no packet; it only maintains error and
; completion state. The app receives exact counters through svc 31/POLL.
sfx_music_poll:
    call sfx_music_update
    jc .failed
    mov word [sfx_result],0
    ret
.failed:
    mov word [sfx_result],7
    ret

sfx_music_close:
    cmp byte [sfx_music_owned],0
    je .done
    mov dx,[nam_base]
    add dx,AC97_MASTER_VOL
    mov ax,[sfx_music_saved_volume]
    call ac97_codec_write
    call ac97_cleanup_output
    jc .close_error
    mov byte [sfx_music_owned],0
    mov byte [sfx_music_started],0
    mov byte [sfx_music_paused],0
    mov byte [sfx_music_eos],0
    mov word [sfx_result],0
.done:
    ret
.close_error:
    mov byte [sfx_music_owned],0
    mov byte [sfx_music_started],0
    mov word [sfx_result],7
    ret

align 8
sfx_music_bdl times 32*8 db 0
sfx_music_owned db 0
sfx_music_started db 0
sfx_music_paused db 0
sfx_music_eos db 0
sfx_music_saved_volume dw 0
sfx_music_last_civ dw 0
sfx_music_write_pos dw 0
sfx_music_src_seg dw 0
sfx_music_src_off dw 0
sfx_music_ring_off dw 0
sfx_music_copy_bytes dw 0
sfx_music_copy_first dw 0
sfx_music_picb dw 0
align 4
sfx_music_base dd 0
sfx_music_queued dd 0
sfx_music_played dd 0
sfx_music_published dd 0
sfx_music_done dd 0
