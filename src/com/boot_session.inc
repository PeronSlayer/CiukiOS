; GRUB writes four bytes in the mapped RAM disk, never the optical/host disk.
; Consume the intent before SETUP clones its source so installed disks boot Live.
startup_select_session:
    mov ah,0x19
    int 0x21
    cmp al,3                    ; live D: one-shot Setup/DOS/Safe/Mute choice
    je .live_cd
    jmp .installed

.live_cd:
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
    cmp dword [boot_session_mode],'NOVM'
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
    cmp dword [boot_session_mode],'NOVM'
    je .live_novm
    mov byte [vc_force_safe],1
    mov byte [driver_skip_vm],1
    mov si,boot_session_safe
    call ui_serial
    jmp .live
.live_novm:
    mov byte [driver_skip_vm],1
    mov si,boot_session_novm
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

.installed:
    ; Read STARTUP.CFG if present
    mov dx,boot_session_path
    mov ax,0x3D00
    int 0x21
    jc .check_keys
    mov bx,ax
    mov dx,boot_session_mode
    mov cx,4
    mov ah,0x3F
    int 0x21
    pushf
    push ax
    mov ah,0x3E
    int 0x21
    pop ax
    popf
    jc .check_keys
    cmp ax,4
    jne .check_keys
    cmp dword [boot_session_mode],'HARD'
    je .inst_safe
    cmp dword [boot_session_mode],'SAFE'
    je .inst_safe
    cmp dword [boot_session_mode],'NOVM'
    je .inst_novm
    cmp dword [boot_session_mode],'MUTE'
    je .inst_mute
    cmp dword [boot_session_mode],'DOS!'
    je .direct_dos
    cmp dword [boot_session_mode],'MENU'
    je .show_menu

.check_keys:
    ; Check BIOS Data Area shift flags (0040:0017)
    push ds
    xor ax,ax
    mov ds,ax
    test byte [0x0417],0x03     ; Left or Right Shift held?
    pop ds
    jnz .inst_safe              ; Holding Shift forces Safe Mode

    ; Check non-blocking BIOS keyboard buffer
    mov ah,0x01
    int 0x16
    jz .live                    ; No key waiting -> proceed to normal live boot
    mov ah,0x00
    int 0x16                    ; Read key: AH=scancode, AL=ASCII
    cmp ah,0x3F                 ; F5 key
    je .inst_safe
    cmp al,'s'
    je .inst_safe
    cmp al,'S'
    je .inst_safe
    cmp al,'n'
    je .inst_novm
    cmp al,'N'
    je .inst_novm
    cmp al,'m'
    je .inst_mute
    cmp al,'M'
    je .inst_mute
    cmp ah,0x3E                 ; F4 key
    je .direct_dos
    cmp al,'d'
    je .direct_dos
    cmp al,'D'
    je .direct_dos
    cmp ah,0x42                 ; F8 key
    je .show_menu
    cmp al,' '
    je .show_menu
    jmp .live

.inst_safe:
    mov byte [vc_force_safe],1
    mov byte [ui_sound_played],1
    mov byte [driver_skip_vm],1
    mov si,boot_session_safe
    call ui_serial
    jmp .live
.inst_novm:
    mov byte [driver_skip_vm],1
    mov si,boot_session_novm
    call ui_serial
    jmp .live
.inst_mute:
    mov byte [ui_sound_played],1
    mov si,boot_session_muted
    call ui_serial
    jmp .live

.show_menu:
    mov ax,0x0003
    int 0x10                    ; standard 80x25 text mode
    mov si,boot_menu_msg
    call boot_print_str
    ; Wait up to 5 seconds (approx 91 timer ticks at 0040:006C)
    push ds
    xor ax,ax
    mov ds,ax
    mov eax,[0x046C]
    add eax,91
    mov [cs:boot_menu_deadline],eax
    pop ds
.menu_poll:
    mov ah,0x01
    int 0x16
    jnz .menu_key
    push ds
    xor ax,ax
    mov ds,ax
    mov eax,[0x046C]
    pop ds
    cmp eax,[cs:boot_menu_deadline]
    jae .live
    hlt
    jmp .menu_poll
.menu_key:
    mov ah,0x00
    int 0x16
    cmp al,'1'
    je .live
    cmp al,13                   ; Enter -> Normal boot
    je .live
    cmp al,'2'
    je .inst_novm
    cmp al,'3'
    je .inst_safe
    cmp al,'4'
    je .inst_mute
    cmp al,'5'
    je .direct_dos
    cmp ah,0x3F                 ; F5 -> Safe mode
    je .inst_safe
    cmp ah,0x3E                 ; F4 -> DOS
    je .direct_dos
    jmp .menu_poll

boot_print_str:
    cld
.char:
    lodsb
    test al,al
    jz .ret
    mov ah,0x0E
    mov bx,0x0007
    int 0x10
    jmp .char
.ret:
    ret

boot_session_path db '\SYSTEM\STARTUP.CFG',0
boot_session_setup_path db '\APPS\SETUP.COM',0
boot_session_mode dd 0
boot_session_live db 'LIVE'
boot_session_marker db '[BOOT-SESSION] SETUP',13,10,0
boot_session_safe db '[BOOT-SESSION] SAFE',13,10,0
boot_session_hard db '[BOOT-SESSION] HARD',13,10,0
boot_session_novm db '[BOOT-SESSION] NOVM',13,10,0
boot_session_muted db '[BOOT-SESSION] MUTE',13,10,0
boot_session_dos_marker db '[BOOT-SESSION] DOS',13,10,0
boot_session_dos db 'DOS session. Type HELP for commands or EXIT for the desktop.',13,10,'$'
boot_session_closed db 'Setup closed. Type SETUP to reopen or EXIT for the desktop.',13,10,'$'
boot_session_error db 'Cannot reset the CD startup choice. Setup was not started.',13,10,'$'
boot_session_launch_error db 'Cannot start SETUP. Type SETUP to retry.',13,10,'$'
boot_menu_deadline dd 0
boot_menu_msg:
    db 13,10,' CiukiOS Startup Menu',13,10
    db ' 1. Normal Desktop',13,10
    db ' 2. Real Mode (No VM)',13,10
    db ' 3. Safe Mode (VGA)',13,10
    db ' 4. Muted Desktop',13,10
    db ' 5. CiukiDOS Prompt',13,10,13,10
    db ' Option [1-5]: ',0
