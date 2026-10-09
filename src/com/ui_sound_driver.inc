; CSFX0001: foreground-only, nonresident, asynchronous ICH PCM event player.
; AX: 0 prepare, 1 poll, 2 release, 3 error, 4 info, 5 warning, 6 confirm,
; 7 startup (\SYSTEM\BOOT.PCM; without an ICH it runs BOOTSND.COM /Q, which
; returns when its Sound Blaster or PC-speaker melody has finished).
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
    cmp ax,8
    jne .saved_packet
    mov [cs:sfx_music_op],bx
    mov [cs:sfx_music_packet_seg],es
    mov [cs:sfx_music_packet_off],di
.saved_packet:
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
    cmp ax,8
    je .music
    cmp ax,3
    jb .done
    cmp ax,7
    ja .done
    call sfx_play
    jmp .done
.music:
    call sfx_music_dispatch
    jmp .done
.init:
    call sfx_init
    jmp .done
.poll:
    cmp byte [sfx_music_owned],0
    je .cue_poll
    call sfx_music_poll
    jmp .done
.cue_poll:
    call sfx_poll
    jmp .done
.release:
    call sfx_music_close
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
    mov byte [sfx_inited],1
    mov byte [sfx_available],0
    mov word [sfx_result],3
    mov byte [sfx_audio_stage],1       ; PCI controller discovery
    call pci_find_intel_ac97
    jnc .ich
    call sfx_sb_init
    mov byte [sfx_audio_stage],8       ; legacy/SB fallback, no NAM reads
    call sfx_audio_log_init
    ret
.ich:
    mov byte [sfx_audio_stage],2
    call ac97_enable_io
    jc .restore
    mov byte [sfx_audio_stage],3
    call ac97_all_streams_idle
    jc .restore
    mov byte [sfx_audio_stage],4
    call ac97_wait_codec
    jc .restore
    mov byte [sfx_audio_stage],5
    call ac97_prepare_codec
    jc .restore
    call sfx_apply_master_preference
    call sfx_audio_snapshot_mixer     ; while PCI I/O is still enabled
    mov byte [sfx_audio_stage],6
    mov bx,0x1000
    mov ah,0x48
    int 0x21
    jc .memory
    mov [sfx_buffer],ax
    mov byte [sfx_available],1
    mov word [sfx_result],0
    mov byte [sfx_audio_stage],7
    jmp .restore
.memory:
    mov word [sfx_result],6
    mov bx,0xFFFF                    ; DOS reports the largest free block
    mov ah,0x48
    int 0x21
    mov [sfx_audio_largest],bx
.restore:
    call sfx_audio_log_init
    call boot_restore_pci
.done:
    ret

; The startup event is queued before the desktop app receives EV_OPEN. Apply
; its saved master preference during sound-driver initialization so the first
; melody uses the same level and mute state as later sounds.
sfx_apply_master_preference:
    push ax
    push bx
    push cx
    push dx
    push si
    mov ax,0x1A1A                    ; first-use level 5, unmuted
    mov [sfx_audio_pref_value],ax
    mov dx,sfx_audio_pref_path
    mov ax,0x3D00
    int 0x21
    jc .apply
    mov bx,ax
    mov dx,sfx_audio_pref_data
    mov cx,2
    mov ah,0x3F
    int 0x21
    jc .read_failed
    mov [sfx_audio_pref_bytes],ax
    jmp .close_preference
.read_failed:
    mov word [sfx_audio_pref_bytes],0xFFFF
.close_preference:
    mov ah,0x3E
    int 0x21
    jc .apply
    cmp word [sfx_audio_pref_bytes],1
    jne .apply
    test byte [sfx_audio_pref_data],0x60
    jnz .apply
    mov bl,[sfx_audio_pref_data]
    xor ax,ax
    mov al,bl
    and al,31
    mov si,ax
    mov ax,31
    sub ax,si
    mov ah,al
    test bl,0x80
    jz .save_value
    or ax,0x8000
.save_value:
    mov [sfx_audio_pref_value],ax
.apply:
    mov ax,[sfx_audio_pref_value]
    mov dx,[nam_base]
    add dx,AC97_MASTER_VOL
    call ac97_codec_write
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

sfx_play:
    cmp byte [sfx_music_owned],0
    je .owner_free
    mov word [sfx_result],5
    ret
.owner_free:
    cmp word [sfx_op],7
    jne .event
    cmp byte [sfx_available],1
    je .event                           ; ICH: asynchronous, below
    cmp byte [sfx_inited],0
    je .unavailable
    jmp sfx_boot_program
.event:
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
    cmp word [sfx_op],7
    je sfx_boot_pcm
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
    mov word [sfx_timeout],36
    xor bl,bl
..@sfx_dma_run:                          ; BL = last valid descriptor
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
    mov al,bl
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
    cmp byte [sfx_boot_active],1
    je sfx_boot_poll
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
    cmp ax,[sfx_timeout]
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
    je .free
    call ac97_cleanup_output
    mov byte [sfx_playing],0
    jnc .free
    mov word [sfx_result],7
    mov byte [sfx_boot_event],'F'
.free:
    call sfx_boot_free
.done:
    ret

%include "src/com/ui_sound_startup.inc"

; No ICH: the startup program itself (Sound Blaster or PC speaker), which
; returns when the melody has played. EXEC does not keep SS:SP.
sfx_boot_program:
    mov [sfx_exec+4],cs
    mov [sfx_exec+8],cs
    mov [sfx_exec+12],cs
    mov [sfx_exec_sp],sp
    mov dx,sfx_boot_program_path
    mov bx,sfx_exec
    mov ax,0x4B00
    int 0x21
    cli
    mov ax,cs
    mov ss,ax
    mov sp,[cs:sfx_exec_sp]
    mov ds,ax
    mov es,ax
    sti
    cld
    mov word [sfx_result],0
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
%include "src/com/ui_sound_music.inc"

sfx_paths dw sfx_error,sfx_info,sfx_warning,sfx_confirm
sfx_error db '\SYSTEM\SOUNDS\ERROR.PCM',0
sfx_info db '\SYSTEM\SOUNDS\INFO.PCM',0
sfx_warning db '\SYSTEM\SOUNDS\WARN.PCM',0
sfx_confirm db '\SYSTEM\SOUNDS\CONFIRM.PCM',0
sfx_preference db '\SYSTEM\BOOT.SND',0
sfx_audio_pref_path db '\SYSTEM\VOLUME.CFG',0
sfx_audio_pref_data times 2 db 0
sfx_audio_pref_bytes dw 0
sfx_audio_pref_value dw 0x1A1A
sfx_boot_path db '\SYSTEM\BOOT.PCM',0
sfx_boot_program_path db '\SYSTEM\BOOTSND.COM',0
sfx_exec_tail db 3,' /Q',13
sfx_exec dw 0,sfx_exec_tail,0,0x5C,0,0x6C,0
sfx_exec_sp dw 0
sfx_timeout dw 36
sfx_inited db 0
sfx_setting db '1'
sfx_op dw 0
sfx_music_op dw 0
sfx_music_packet_seg dw 0
sfx_music_packet_off dw 0
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
