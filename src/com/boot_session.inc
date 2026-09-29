; GRUB writes four bytes in the mapped RAM disk, never the optical/host disk.
; Consume the intent before SETUP clones its source so installed disks boot Live.
startup_select_session:
    mov ah,0x19
    int 0x21
    cmp al,3                    ; live D: only; an installed C: ignores the flag
    jne .live
    mov dx,boot_session_path
    mov ax,0x3D02
    int 0x21
    jc .live
    mov bx,ax
    mov dx,boot_session_mode
    mov cx,4
    mov ah,0x3F
    int 0x21
    jc .close_live
    cmp ax,4
    jne .close_live
    cmp dword [boot_session_mode],'SETU'
    je .consume
    cmp dword [boot_session_mode],'DOS!'
    je .consume
    cmp dword [boot_session_mode],'SAFE'
    je .consume
    cmp dword [boot_session_mode],'MUTE'
    jne .close_live
.consume:
    xor cx,cx
    xor dx,dx
    mov ax,0x4200
    int 0x21
    jc .close_error
    mov dx,boot_session_live
    mov cx,4
    mov ah,0x40
    int 0x21
    jc .close_error
    cmp ax,4
    jne .close_error
    mov ah,0x3E
    int 0x21
    jc .error
    mov byte [ui_sound_played],1
    cmp dword [boot_session_mode],'DOS!'
    je .direct_dos
    cmp dword [boot_session_mode],'SETU'
    je .setup
    cmp dword [boot_session_mode],'MUTE'
    je .muted
    mov byte [vc_force_safe],1
    mov si,boot_session_safe
    call ui_serial
    jmp .live
.muted:
    ; Isolate startup audio without changing the selected video backend.
    ; The one-shot flag is consumed before an installer can clone this disk.
    mov si,boot_session_muted
    call ui_serial
    jmp .live
.close_live:
    mov ah,0x3E
    int 0x21
.live:
    xor al,al
    ret
.close_error:
    mov ah,0x3E
    int 0x21
.error:
    mov si,boot_session_error
    jmp .dos
.setup:
    mov si,boot_session_marker
    call ui_serial
    call setup_exec_block
    mov byte [exec_tail],0
    mov byte [exec_tail+1],13
    mov dx,boot_session_setup_path
    mov bx,exec_env_seg
    mov ax,0x4B00
    int 0x21
    jc .launch_error
    mov ax,0x4D00
    int 0x21
    mov si,boot_session_closed
    jmp .dos
.launch_error:
    mov si,boot_session_launch_error
    jmp .dos
.direct_dos:
    mov si,boot_session_dos_marker
    call ui_serial
    mov si,boot_session_dos
.dos:
    ; Cancellation returns to an immediately usable text console. EXIT may
    ; open the configured desktop, but does not replay sound/setup. Only the
    ; explicit SAFE boot choice above may force the low-resolution fallback.
    push si
    mov byte [ui_sound_played],1
    call vc_end
    call shell_apply_text_profile
    call redraw_title_bar
    pop si
    call print_dual_dollar_string
    sti
    cld
    mov al,1
    ret

boot_session_path db '\SYSTEM\STARTUP.CFG',0
boot_session_setup_path db '\APPS\SETUP.COM',0
boot_session_mode dd 0
boot_session_live db 'LIVE'
boot_session_marker db '[BOOT-SESSION] SETUP',13,10,0
boot_session_safe db '[BOOT-SESSION] SAFE',13,10,0
boot_session_muted db '[BOOT-SESSION] MUTE',13,10,0
boot_session_dos_marker db '[BOOT-SESSION] DOS',13,10,0
boot_session_dos db 'DOS session. Type HELP for commands or EXIT for the desktop.',13,10,'$'
boot_session_closed db 'Setup closed. Type SETUP to reopen or EXIT for the desktop.',13,10,'$'
boot_session_error db 'Cannot reset the CD startup choice. Setup was not started.',13,10,'$'
boot_session_launch_error db 'Cannot start SETUP. Type SETUP to retry.',13,10,'$'
