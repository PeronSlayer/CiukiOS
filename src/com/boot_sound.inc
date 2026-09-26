; Non-resident startup sound, using the same checked ICH path as AC97INIT.
; Owns no IRQs. The complete 240 KiB PCM stream is loaded before starting DMA.
boot_sound_start:
    cli
    mov ax, cs
    mov ss, ax
    mov sp, boot_stack_top
    sti
    mov ds, ax
    mov es, ax
    cld
    in al,0x21
    mov [boot_pic_master],al
    in al,0xA1
    mov [boot_pic_slave],al
    call parse_quiet_switch
    AUDIO_TRACE boot_diag_entry
    cmp byte [audio_diagnostic],1
    jne .resize
    ; Explicit diagnostics must remain observable on a physical machine.
    ; This gate precedes every DOS/PCI call and all allocations: Esc can
    ; return normally without leaving hardware or memory ownership behind.
    AUDIO_TRACE boot_diag_gate
.entry_key:
    xor ah,ah
    int 0x16
    cmp al,27
    je .exit
    cmp al,13
    jne .entry_key
.resize:
    mov bx, ((boot_image_end - $$ + 0x100) + 15) >> 4
    mov ah, 0x4A
    int 0x21
    cmp byte [audio_diagnostic], 1
    je .play                     ; SOUND /D tests even a muted preference
    mov si, 0x81
    xor cx, cx
    mov cl, [0x80]
.spaces:
    jcxz .preference
    lodsb
    dec cx
    cmp al, ' '
    je .spaces
    cmp al, '/'
    je .spaces
    and al, 0xDF
    cmp al, 'T'
    je .play
    cmp al, 'O'
    jne .preference
    jcxz .preference
    lodsb
    and al, 0xDF
    mov byte [boot_setting], '1'
    cmp al, 'N'
    je .save
    cmp al, 'F'
    jne .preference
    mov byte [boot_setting], '0'
.save:
    mov dx, boot_config
    xor cx, cx
    mov ah, 0x3C
    int 0x21
    jc .save_failed
    mov bx, ax
    mov dx, boot_setting
    mov cx, 1
    mov ah, 0x40
    int 0x21
    jc .write_failed
    cmp ax,1
    jne .write_failed
    mov ah, 0x3E
    int 0x21
    jc .save_failed
    mov dx, boot_saved
    call print_line
    jmp .exit
.write_failed:
    mov ah,0x3E
    int 0x21
.save_failed:
    mov dx,boot_save_error
    call print_line
    mov ax,0x4C03
    int 0x21
.preference:
    mov dx, boot_config
    mov ax, 0x3D00
    int 0x21
    jc .play
    mov bx, ax
    mov dx, boot_setting
    mov cx, 1
    mov ah, 0x3F
    int 0x21
    mov ah, 0x3E
    int 0x21
    cmp byte [boot_setting], '0'
    je .exit
.play:
    mov byte [boot_status],1
    mov dx,boot_test_begin
    call print_line
    AUDIO_TRACE boot_diag_native
    call audio_native_probe
    jnc .native_sb
    call pci_find_intel_ac97
    jc .speaker
    mov word [boot_error],boot_error_pci
    AUDIO_TRACE boot_diag_io
    call ac97_enable_io
    jc .restore_pci
    mov word [boot_error],boot_error_busy
    ; PCI BME controls all three DMA engines. In particular, enabling it must
    ; not revive a firmware capture stream that can write through a stale BDL.
    ; Preserve every active stream by declining playback, never resetting it.
    AUDIO_TRACE boot_diag_busy
    call ac97_all_streams_idle
    jc .restore_pci
    mov word [boot_error],boot_error_file
    AUDIO_TRACE boot_diag_pcm
    call boot_load_pcm
    jc .restore_pci
    mov byte [boot_failed],1
    mov word [boot_error],boot_error_pci
    call ac97_enable_pci
    jc .cleanup
    mov word [boot_error],boot_error_codec
    AUDIO_TRACE boot_diag_link
    call ac97_wait_codec
    jc .cleanup
    mov word [boot_error],boot_error_format
    AUDIO_TRACE boot_diag_codec
    call ac97_prepare_codec
    jc .cleanup
    ; Automatic boot never issues raw EC commands. Firmware owns the
    ; notebook amplifier and keyboard hotkeys; only explicit /E can probe.
    cmp byte [quiet_mode],0
    jne .amplifier_start
    call thinkpad_audio_unmute
.amplifier_start:
    mov word [boot_error],boot_error_timer
    AUDIO_TRACE boot_diag_timer
    ; Notebook amplifier gates settle after codec power-up and EC unmute.
    xor ah,ah
    int 0x1A
    mov bx,dx
    mov dword [boot_poll_budget],0x100000
.amplifier:
    in al,0x80                 ; bound a stalled timer without a CPU-speed timeout
    xor ah,ah
    int 0x1A
    sub dx,bx
    cmp dx,4
    jae .amplifier_ready
    dec dword [boot_poll_budget]
    jnz .amplifier
    jmp .cleanup
.amplifier_ready:
    mov word [boot_error],boot_error_dma
    call ac97_play_sample
    jc .cleanup
    mov byte [boot_failed],0
.cleanup:
    call ac97_cleanup_output
    AUDIO_TRACE boot_diag_cleanup
    jnc .free
    mov byte [boot_failed],1
    mov word [boot_error],boot_error_pci
    cmp byte [ac97_dma_unquiesced],0
    je .free
    mov word [boot_error],boot_error_shutdown
.free:
    mov es, [boot_buffer]
    mov ah, 0x49
    int 0x21
    cmp byte [boot_failed],0
    jne .speaker
    mov byte [boot_status],0
    mov dx,boot_pcm_done
    call print_line
    jmp .exit
.restore_pci:
    ; Includes busy output, failed allocation and a rejected I/O-enable write.
    ; No stream reset/BDL write is allowed on these early exits.
    call boot_restore_pci
    jmp .speaker
.native_sb:
    mov word [boot_error],boot_error_sb
    push cs
    pop ds
    push ds
    pop es
    mov ax, ds
    mov [boot_sb_param+4], ax
    mov [boot_sb_param+8], ax
    mov [boot_sb_param+12], ax
    mov dx, boot_sb_path
    mov bx, boot_sb_param
    mov ax, 0x4B00
    int 0x21
    jc .speaker
    mov ah, 0x4D
    int 0x21
    or ax, ax
    jnz .speaker
    mov byte [boot_status],0
    mov dx, boot_sb_done
    call print_line
    jmp .exit
.speaker:
    ; Failed PCM playback is not reported as success by playing PIT beeps.
    ; Quiet automatic startup returns immediately; a manual test retains
    ; its diagnostic until a key is pressed, before the shell restores video.
.exit:
    ; PCI firmware calls must not leave keyboard/timer/mouse IRQs masked.
    ; This polling player owns no interrupt route, so restore the caller's masks.
    cli
    mov al,[boot_pic_slave]
    out 0xA1,al
    mov al,[boot_pic_master]
    out 0x21,al
    sti
    cmp byte [boot_status],0
    je .return
    cmp byte [quiet_mode],0
    jne .return
    cmp word [nam_base],0
    je .error
    call print_hardware_report
.error:
    mov dx,[boot_error]
    call print_line
    mov dx,boot_error_key
    call print_line
    xor ah,ah
    int 0x16
.return:
    mov ah,0x4C
    mov al,[boot_status]
    int 0x21

boot_load_pcm:
    mov bx, 0x3C00
    mov ah, 0x48
    int 0x21
    jc .fail
    mov [boot_buffer], ax
    mov dx, boot_pcm_path
    mov ax, 0x3D00
    int 0x21
    jc .free
    mov bx, ax
    mov bp, 15
    mov si, [boot_buffer]
.read:
    push ds
    mov ds, si
    xor dx, dx
    mov cx, 16384
    mov ah, 0x3F
    int 0x21
    pop ds
    jc .close_fail
    cmp ax, 16384
    jne .close_fail
    add si, 1024
    dec bp
    jnz .read
    mov ah, 0x3E
    int 0x21
    clc
    ret
.close_fail:
    mov ah, 0x3E
    int 0x21
.free:
    mov es, [boot_buffer]
    mov ah, 0x49
    int 0x21
.fail:
    stc
    ret

boot_fill_bdl:
    push cs
    pop es
    mov di, ac97_bdl
    movzx eax, word [boot_buffer]
    shl eax, 4
    mov cx, 15
.descriptor:
    stosd
    mov word [es:di], 8192
    mov word [es:di + 2], 0
    add di, 4
    add eax, 16384
    loop .descriptor
    ; DS is our code segment; use it to calculate the physical BDL address.
    xor ebx, ebx
    mov bx, ds
    shl ebx, 4
    add ebx, ac97_bdl
    ret

boot_status db 0
boot_diag_entry db '[SOUND:D] Entered player; DOS resize',13,10,'$'
boot_diag_gate db '[SOUND:D] Enter: continue test   Esc: return without testing',13,10,'$'
boot_diag_native db '[SOUND:D] Probe native Sound Blaster',13,10,'$'
boot_diag_io db '[SOUND:D] Enable PCI I/O only',13,10,'$'
boot_diag_busy db '[SOUND:D] Check output busy',13,10,'$'
boot_diag_pcm db '[SOUND:D] Allocate and read PCM',13,10,'$'
boot_diag_link db '[SOUND:D] Wait for AC-link codec',13,10,'$'
boot_diag_codec db '[SOUND:D] Configure codec (CAS)',13,10,'$'
boot_diag_timer db '[SOUND:D] Wait for amplifier timer',13,10,'$'
boot_diag_cleanup db '[SOUND:D] DMA stopped; cleanup',13,10,'$'
boot_error dw boot_error_device
boot_test_begin db '[SOUND] Testing PCM playback...',13,10,'$'
boot_error_device db '[SOUND] No supported PCM device was detected.',13,10,'$'
boot_error_busy db '[SOUND] An AC97 DMA channel is already in use.',13,10,'$'
boot_error_file db '[SOUND] Could not read SYSTEM\BOOT.PCM completely.',13,10,'$'
boot_error_pci db '[SOUND] Could not enable the PCI audio controller.',13,10,'$'
boot_error_codec db '[SOUND] The AC97 codec did not become ready.',13,10,'$'
boot_error_format db '[SOUND] The codec rejected PCM initialization.',13,10,'$'
boot_error_timer db '[SOUND] The playback timer did not advance.',13,10,'$'
boot_error_dma db '[SOUND] AC97 DMA did not complete playback.',13,10,'$'
boot_error_shutdown db '[SOUND] AC97 DMA did not stop normally; bus mastering is disabled.',13,10,'$'
boot_error_sb db '[SOUND] Native Sound Blaster playback failed.',13,10,'$'
boot_error_key db 'Press any key to return to the shell.',13,10,'$'

boot_pcm_path db '\SYSTEM\BOOT.PCM', 0
boot_config db '\SYSTEM\BOOT.SND', 0
boot_setting db '1'
boot_pcm_done db 'CiukiOS startup melody played through AC97.',13,10,'$'
boot_sb_done db 'CiukiOS startup melody played through Sound Blaster.',13,10,'$'
boot_sb_path db '\SYSTEM\DRIVERS\SBSTART.COM',0
boot_sb_tail db 3,' /Q',13
boot_sb_param dw 0,boot_sb_tail,0,0x5C,0,0x6C,0

%include "src/com/audio_native.inc"
boot_fallback db 'CiukiOS startup melody: PC speaker fallback.',13,10,'$'
boot_save_error db 'Could not save the startup sound preference.',13,10,'$'
boot_saved db 'Startup sound preference saved. SOUND TEST plays the melody.', 13, 10, '$'
boot_codec_tick dw 0
boot_poll_budget dd 0
boot_pic_master db 0
boot_pic_slave db 0
boot_failed db 0
boot_buffer dw 0
boot_speaker_old db 0
boot_notes dw 2032, 1356, 1208, 806
align 16
; PCI BIOS 2.1 section 3.2 requires 1024 bytes available to the firmware.
; This player also calls DOS file services, whose dispatcher and BIOS disk
; calls run on the caller's stack. A 1024-byte total stack let those frames
; overwrite boot_buffer, the saved PIC masks and the player's data before
; the desktop started. Reserve headroom for both DOS and firmware frames.
boot_stack times 4096 db 0
boot_stack_top:
boot_image_end:
