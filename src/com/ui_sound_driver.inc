; CSFX0001: foreground-only, nonresident, asynchronous ICH PCM event player.
; AX: 0 prepare, 1 poll, 2 release, 3 error, 4 info, 5 warning, 6 confirm.
; AX result: 0 success/idle, 1 playing, 2 muted, 3 unavailable, 4 read error,
; 5 device busy, 6 allocation failure, 7 DMA error. No video or IRQ hooks.
; All caller state except AX is preserved. Never call from a DOS interrupt.
sfx_entry:
    pushfd
    pushad
    push ds
    push es
    push fs
    push gs
    cli
    mov [cs:sfx_host_ss],ss
    mov [cs:sfx_host_sp],sp
    mov bx,cs
    mov ss,bx
    mov sp,sfx_stack_top
    mov ds,bx
    mov es,bx
    sti
    cld
    mov [sfx_op],ax
    in al,0x21
    mov [sfx_pic1],al
    in al,0xA1
    mov [sfx_pic2],al
    mov word [sfx_result],0
    mov ax,[sfx_op]
    test ax,ax
    jz .init
    cmp ax,1
    je .poll
    cmp ax,2
    je .release
    cmp ax,3
    jb .done
    cmp ax,6
    ja .done
    call sfx_play
    jmp .done
.init:
    call sfx_init
    jmp .done
.poll:
    call sfx_poll
    jmp .done
.release:
    call sfx_release
.done:
    cli
    call sfx_sb_return_masks
    mov al,[sfx_pic2]
    out 0xA1,al
    mov al,[sfx_pic1]
    out 0x21,al
    mov ss,[sfx_host_ss]
    mov sp,[sfx_host_sp]
    pop gs
    pop fs
    pop es
    pop ds
    popad
    popfd
    mov ax,[cs:sfx_result]
    retf

sfx_init:
    cmp word [sfx_buffer],0
    jne .done
    mov byte [quiet_mode],1
    mov byte [audio_diagnostic],0
    mov byte [sfx_available],0
    mov word [sfx_result],3
    call pci_find_intel_ac97
    jc sfx_sb_init
    call ac97_enable_io
    jc .restore
    call ac97_all_streams_idle
    jc .restore
    call ac97_wait_codec
    jc .restore
    call ac97_prepare_codec
    jc .restore
    mov bx,0x1000
    mov ah,0x48
    int 0x21
    jc .memory
    mov [sfx_buffer],ax
    mov byte [sfx_available],1
    mov word [sfx_result],0
    jmp .restore
.memory:
    mov word [sfx_result],6
.restore:
    call boot_restore_pci
.done:
    ret

sfx_play:
    cmp byte [sfx_available],0
    je .unavailable
    ; Re-read the user's setting in foreground only. Muting also stops an
    ; in-flight sound before returning; drawing never accesses the filesystem.
    mov dx,sfx_preference
    mov ax,0x3D00
    int 0x21
    jc .enabled
    mov bx,ax
    mov byte [sfx_setting],'1'
    mov dx,sfx_setting
    mov cx,1
    mov ah,0x3F
    int 0x21
    mov ah,0x3E
    int 0x21
    cmp byte [sfx_setting],'0'
    jne .enabled
    call sfx_stop
    mov word [sfx_result],2
    ret
.enabled:
    call sfx_stop
    mov word [sfx_result],4
    mov bx,[sfx_op]
    sub bx,3
    shl bx,1
    mov dx,[sfx_paths+bx]
    cmp byte [sfx_available],2
    jne .open
    mov dx,[sfx_sb_paths+bx]
.open:
    mov ax,0x3D00
    int 0x21
    jc .done
    mov [sfx_file],ax
    mov bx,ax
    xor cx,cx
    xor dx,dx
    mov ax,0x4202
    int 0x21
    jc .close
    test dx,dx
    jnz .close
    test ax,ax
    jz .close
    cmp byte [sfx_available],2
    jne .pcm_size
    cmp ax,SFX_SB_MAX
    ja .close
    jmp .size_ready
.pcm_size:
    test al,3
    jnz .close
    cmp ax,65532
    ja .close
.size_ready:
    mov [sfx_bytes],ax
    xor cx,cx
    xor dx,dx
    mov ax,0x4200
    int 0x21
    jc .close
    mov cx,[sfx_bytes]
    xor dx,dx
    cmp byte [sfx_available],2
    jne .read
    mov dx,[sfx_sb_offset]
.read:
    mov ds,[sfx_buffer]
    mov ah,0x3F
    int 0x21
    push cs
    pop ds
    jc .close
    cmp ax,[sfx_bytes]
    jne .close
    mov ah,0x3E
    int 0x21
    jc .done
    cmp byte [sfx_available],2
    je sfx_sb_start
    mov word [sfx_result],5
    call ac97_enable_io
    jc .restore
    call ac97_enable_pci
    jc .restore
    mov dx,[nabm_base]
    add dx,ICH_PO_CR
    mov al,ICH_CR_RESET
    out dx,al
    mov cx,0xFFFF
.reset:
    in al,dx
    test al,ICH_CR_RESET
    jz .start
    loop .reset
    jmp .dma_error
.start:
    movzx eax,word [sfx_buffer]
    shl eax,4
    mov [ac97_bdl],eax
    mov ax,[sfx_bytes]
    shr ax,1
    mov [ac97_bdl+4],ax
    mov word [ac97_bdl+6],0
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
    mov dx,[nabm_base]
    add dx,ICH_PO_LVI
    xor al,al
    out dx,al
    mov dx,[nabm_base]
    add dx,ICH_PO_CR
    mov al,ICH_CR_RUN
    out dx,al
    mov byte [sfx_playing],1
    xor ah,ah
    int 0x1A
    mov [sfx_tick],dx
    mov word [sfx_result],1
    ret
.dma_error:
    mov word [sfx_result],7
.restore:
    call ac97_cleanup_output
    ret
.close:
    mov bx,[sfx_file]
    mov ah,0x3E
    int 0x21
.done:
    ret
.unavailable:
    mov word [sfx_result],3
    ret

sfx_poll:
    cmp byte [sfx_available],2
    je sfx_sb_poll
    cmp byte [sfx_playing],1
    jne .done
    mov dx,[nabm_base]
    add dx,ICH_PO_SR
    in ax,dx
    test ax,ICH_SR_ERROR
    jnz .fail
    test ax,4
    jnz sfx_stop
    xor ax,ax
    mov es,ax
    mov ax,[es:0x046C]
    sub ax,[sfx_tick]
    cmp ax,36
    jae .fail
    mov word [sfx_result],1
    ret
.fail:
    mov word [sfx_result],7
    call sfx_stop
.done:
    ret

sfx_stop:
    cmp byte [sfx_available],2
    je sfx_sb_stop
    cmp byte [sfx_playing],0
    je .done
    call ac97_cleanup_output
    mov byte [sfx_playing],0
    jnc .done
    mov word [sfx_result],7
.done:
    ret

sfx_release:
    call sfx_stop
    mov ax,[sfx_buffer]
    test ax,ax
    jz .done
    mov es,ax
    mov ah,0x49
    int 0x21
    mov word [sfx_buffer],0
    mov byte [sfx_available],0
.done:
    ret

%include "src/com/ui_sound_sb.inc"

sfx_paths dw sfx_error,sfx_info,sfx_warning,sfx_confirm
sfx_error db '\SYSTEM\SOUNDS\ERROR.PCM',0
sfx_info db '\SYSTEM\SOUNDS\INFO.PCM',0
sfx_warning db '\SYSTEM\SOUNDS\WARN.PCM',0
sfx_confirm db '\SYSTEM\SOUNDS\CONFIRM.PCM',0
sfx_preference db '\SYSTEM\BOOT.SND',0
sfx_setting db '1'
sfx_op dw 0
sfx_result dw 0
sfx_file dw 0
sfx_bytes dw 0
sfx_buffer dw 0
sfx_tick dw 0
sfx_host_ss dw 0
sfx_host_sp dw 0
sfx_pic1 db 0
sfx_pic2 db 0
sfx_playing db 0
sfx_available db 0
boot_codec_tick dw 0
boot_poll_budget dd 0
align 16
sfx_stack times 4096 db 0
sfx_stack_top:
sfx_image_end:
