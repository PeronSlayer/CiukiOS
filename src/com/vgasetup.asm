bits 16
cpu 386
org 0x0100
; Exit code for a preview or menu that was cancelled or timed out: nothing
; was committed. The shell keeps SAFE video and shows no error for it.
VGASETUP_UNCHANGED equ 5

; CiukiOS interactive shared display manager. The private VBE console also
; powers SHELL.COM; one transaction saves its mode and Windows configuration.
; IBM ThinkPad LCD brightness
; uses the documented EC HBRV byte and is only touched after an IBM ROM check.

%define EC_DATA             0x62
%define EC_STATUS_COMMAND   0x66
%define EC_STATUS_OBF       0x01
%define EC_STATUS_IBF       0x02
%define EC_CMD_READ         0x80
%define EC_CMD_WRITE        0x81
%define TP_EC_BACKLIGHT     0x31

start:
    cli
    mov ax, cs
    mov ss, ax
    mov sp, stack_top
    sti
    mov es, ax
    mov bx, ((image_end - $$ + 0x0100) + 15) >> 4
    mov ah, 0x4A
    int 0x21

    push cs
    pop ds
    push ds
    pop es
    cld
    call copy_command_tail

    mov si, arg_buffer
    call skip_spaces
    cmp byte [si], 0
    je interactive_setup
    mov di, word_set
    call token_equal
    je windows_profile_command
    mov di, word_desktop
    call token_equal
    je desktop_profile_command
    mov di, word_boot
    call token_equal
    je boot_probe
    mov di, word_config
    call token_equal
    je run_config
    mov di, word_auto
    call token_equal
    je run_automatic
    mov di, word_win
    call token_equal
    je windows_profile_command
    mov di, word_text
    call token_equal
    je text_profile_command
    mov di, word_modes
    call token_equal
    je run_modes
    mov di, word_detect
    call token_equal
    je run_detect
    mov di, word_status
    call token_equal
    je show_status
    mov di, word_test
    call token_equal
    je run_test
    mov di, word_brightness
    call token_equal
    je brightness_command
    mov di, word_refresh
    call token_equal
    je refresh_command
    mov di, word_help
    call token_equal
    je show_help
    jmp show_help_error

boot_probe:
    ; Apply the persistent CiukiOS text-console profile.  Windows video is a
    ; separate profile because its protected-mode driver must never be
    ; selected merely because a VBE BIOS answered the capability probe.
    call load_text_profile
    mov ax, 0x4C00
    int 0x21

show_status:
    mov dx, msg_header
    call print
    ; Status must be safe on every BIOS.  The previous implementation entered
    ; INT 10h/VBE and the ThinkPad EC before displaying a useful prompt; a
    ; firmware call which never returned therefore made VGASETUP itself look
    ; dead.  Hardware probing is now an explicit DETECT/MODES operation.
    mov dx, msg_safe_status
    call print
    call show_text_rows
    mov dx, msg_help
    call print
    mov ax, 0x4C00
    int 0x21

run_detect:
    mov dx, msg_header
    call print
    call probe_vbe
    call thinkpad_detect
    jc .generic
    mov dx, msg_thinkpad
    call print
    call ec_read_brightness
    jc .brightness_unavailable
    and al, 0x1F
    xor ah, ah
    mov dx, msg_brightness
    call print
    call print_u16
    mov dx, msg_level_suffix
    call print
    jmp .done
.brightness_unavailable:
    mov dx, msg_ec_unavailable
    call print
    jmp .done
.generic:
    mov dx, msg_generic
    call print
.done:
    call show_text_rows
    mov ax, 0x4C00
    int 0x21

show_text_rows:
    push ax
    push bx
    push dx
    push es
    mov ax, 0x0040
    mov es, ax
    xor ax, ax
    mov al, [es:0x0084]
    inc ax
    cmp ax, 25
    jae .valid
    mov ax, 25
.valid:
    mov bx, ax
    mov dx, msg_text_mode
    call print
    mov ax, bx
    call print_u16
    mov dx, msg_rows_suffix
    call print
    pop es
    pop dx
    pop bx
    pop ax
    ret

probe_vbe:
    mov ax, 0x4F00
    mov di, vbe_info
    mov dword [di], 'VBE2'
    int 0x10
    cmp ax, 0x004F
    jne .none
    cmp dword [vbe_info], 'VESA'
    jne .none
    mov dx, msg_vbe
    call print
    mov ax, [vbe_info + 4]
    call print_hex16
    mov dx, msg_memory
    call print
    mov ax, [vbe_info + 18]
    mov bx, 64
    mul bx
    call print_u16
    mov dx, msg_kib
    call print
    ret
.none:
    mov dx, msg_no_vbe
    call print
    ret

run_config:
    jmp interactive_setup

windows_profile_command:
    mov si, arg_buffer
    call skip_spaces
    call skip_token
    call skip_spaces
    cmp byte [si], 0
    je .usage
    mov di, word_safe
    call token_equal
    je run_windows_safe
    mov di, word_640
    call token_equal
    je .mode_640
    mov di, word_800
    call token_equal
    je .mode_800
    mov di, word_1024
    call token_equal
    je .mode_1024
.usage:
    mov dx, msg_win_usage
    call print
    mov ax, 0x4C02
    int 0x21
.mode_800:
    mov word [vc_mode],0x103
    call vc_probe
    jc .unavailable
    mov dword [global_profile], '0800'
    mov dx, windows_profile_800
    mov si, msg_win_800
    jmp install_windows_profile
.mode_1024:
    mov word [vc_mode],0x105
    call vc_probe
    jc .unavailable
    mov dword [global_profile], '1024'
    mov dx, windows_profile_1024
    mov si, msg_win_1024
    jmp install_windows_profile
.mode_640:
    mov dword [global_profile], '0640'
    mov dx, windows_profile_safe
    mov si, msg_win_safe
    jmp install_windows_profile

.unavailable:
    mov dx,msg_profile_unavailable
    call print
    mov ax,0x4C03
    int 0x21

; Native desktop/DOS modes are independent from Windows' installed driver.
; Enumerate only after an explicit request, preview with a real timeout, and
; commit the four-byte preference only after Enter confirms a usable picture.
desktop_profile_command:
    call skip_token
    call skip_spaces
    xor bp,bp
.find:
    mov bx,bp
    shl bx,1
    mov di,[desktop_words+bx]
    call token_equal
    je .selected
    inc bp
    cmp bp,7
    jb .find
    mov dx,msg_desktop_usage
    call print
    mov ax,0x4C02
    int 0x21
.selected:
    mov bx,bp
    shl bx,2
    mov eax,[desktop_profiles+bx]
    mov [global_profile],eax
    ; The shell maps 0800/1024 to VBE modes 0x103/0x105 (vc_load_profile);
    ; preview exactly those, not merely the deepest mode of that size.
    mov word [vc_mode],0x103
    cmp eax,'0800'
    je .preview
    mov word [vc_mode],0x105
    cmp eax,'1024'
    je .preview
    mov ax,[desktop_dimensions+bx]
    mov dx,[desktop_dimensions+bx+2]
    call vc_resolve_mode
.preview:
    cmp word [vc_mode],0
    je windows_profile_command.unavailable
    call vc_begin
    jc windows_profile_command.unavailable
    call vc_title
    mov word [vc_x],4
    mov word [vc_y],4
    mov dx,msg_desktop_preview
    call print
    mov ax,[vc_info+18]
    call print_u16
    mov dx,msg_by
    call print
    mov ax,[vc_info+20]
    call print_u16
    mov dx,msg_desktop_preview_body
    call print
    call vc_flush
    cmp byte [vc_active],1
    je .drain
    call vc_end
    jmp windows_profile_command.unavailable
.drain:
    mov ah,1
    int 0x16
    jz .timer
    xor ah,ah
    int 0x16
    jmp .drain
.timer:
    call preview_ticks
    mov [preview_tick],edx
.wait:
    mov ah,1
    int 0x16
    jz .time
    xor ah,ah
    int 0x16
    cmp al,27
    je .cancel
    cmp al,13
    je .confirm
.time:
    call preview_elapsed
    cmp edx,218
    jae .cancel
    sti
    hlt
    jmp .wait
.cancel:
    call vc_end
    mov dx,msg_preview_reverted
    call print
    mov ax,0x4C00+VGASETUP_UNCHANGED
    int 0x21
.confirm:
    call vc_end
    call prepare_global_profile
    jc run_automatic.failed
    call commit_global_profile
    jc run_automatic.failed
    mov dx,msg_desktop_saved
    call print
    mov ax,0x4C00
    int 0x21

run_automatic:
    mov dword [global_profile], 'AUTO'
    call prepare_global_profile
    jc .failed
    call commit_global_profile
    jc .failed
    mov dx,msg_auto
    call print
    mov ax,0x4C00
    int 0x21
.failed:
    mov dx,msg_profile_failed
    call print
    mov ax,0x4C03
    int 0x21

run_windows_safe:
    mov dword [global_profile], 'TEXT'
    mov dx, windows_profile_safe
    mov si, msg_win_safe

install_windows_profile:
    ; IN DS:DX source profile, DS:SI success description.  Build SYSTEM.NEW
    ; completely before swapping names; a read-only Live CD therefore fails
    ; before touching SYSTEM.INI and an interrupted copy leaves it intact.
    mov [profile_source_ptr], dx
    mov [profile_success_ptr], si
    mov word [source_handle], 0xFFFF
    mov word [target_handle], 0xFFFF
    mov ax, 0x3D00
    int 0x21
    jc .failed
    mov [source_handle], ax
    mov dx, windows_profile_new
    call profile_file_only
    jc .failed
    xor cx, cx
    mov ah, 0x3C
    int 0x21
    jc .failed
    mov [target_handle], ax
.copy:
    mov bx, [source_handle]
    mov dx, io_buffer
    mov cx, 512
    mov ah, 0x3F
    int 0x21
    jc .failed
    or ax, ax
    jz .copied
    mov cx, ax
    mov bx, [target_handle]
    mov ah, 0x40
    int 0x21
    jc .failed
    cmp ax, cx
    jne .failed
    jmp .copy
.copied:
    call close_profile_handles
    call prepare_global_profile
    jc .swap_failed
    mov dx, windows_profile_active
    call profile_file_only
    jc .swap_failed
    mov dx, windows_profile_backup
    call delete_profile_backup
    jc .swap_failed
    push ds
    pop es
    mov dx, windows_profile_active
    mov di, windows_profile_backup
    mov ah, 0x56
    int 0x21
    jc .swap_failed
    mov dx, windows_profile_new
    mov di, windows_profile_active
    mov ah, 0x56
    int 0x21
    jc .rollback
    call commit_global_profile
    jc .rollback_active
    mov dx, [profile_success_ptr]
    call print
    mov dx, msg_global_saved
    call print
    mov ax, 0x4C00
    int 0x21
.rollback_active:
    mov dx, windows_profile_active
    mov ah, 0x41
    int 0x21
.rollback:
    mov dx, windows_profile_backup
    mov di, windows_profile_active
    mov ah, 0x56
    int 0x21
.swap_failed:
    mov dx, windows_profile_new
    mov ah, 0x41
    int 0x21
.failed:
    call close_profile_handles
    mov dx, msg_profile_failed
    call print
    mov ax, 0x4C03
    int 0x21

; Stage the console setting before touching SYSTEM.INI. Only confirmed menu
; choices (or explicit CLI choices) reach this transaction.
prepare_global_profile:
    mov dx, global_profile_new
    call profile_file_only
    jc .done
    xor cx, cx
    mov ah, 0x3C
    int 0x21
    jc .done
    mov bx, ax
    mov dx, global_profile
    mov cx, 4
    mov ah, 0x40
    int 0x21
    jc .close_fail
    cmp ax, 4
    jne .close_fail
    mov ah, 0x3E
    int 0x21
    ret
.close_fail:
    mov ah, 0x3E
    int 0x21
    stc
.done:
    ret

commit_global_profile:
    mov byte [global_had_backup], 0
    mov dx, vc_profile_path
    call profile_file_only
    jc .fail
    mov dx, global_profile_backup
    call delete_profile_backup
    jc .fail
    push ds
    pop es
    mov dx, vc_profile_path
    mov di, global_profile_backup
    mov ah, 0x56
    int 0x21
    jnc .backed_up
    cmp ax, 2
    jne .fail
    jmp .install
.backed_up:
    mov byte [global_had_backup], 1
.install:
    mov dx, global_profile_new
    mov di, vc_profile_path
    mov ah, 0x56
    int 0x21
    jnc .done
    cmp byte [global_had_backup], 1
    jne .fail
    mov dx, global_profile_backup
    mov di, vc_profile_path
    mov ah, 0x56
    int 0x21
.fail:
    stc
.done:
    ret

; Reserved transaction paths must never replace directories or read-only
; files, even on DOS implementations with permissive delete semantics.
profile_file_only:
    mov ax, 0x4300
    int 0x21
    jc .missing
    test cx, 0x11
    jnz .fail
    clc
    ret
.missing:
    cmp ax, 2
    jne .fail
    clc
    ret
.fail:
    stc
    ret

delete_profile_backup:
    call profile_file_only
    jc .done
    mov ah, 0x41
    int 0x21
    jnc .done
    cmp ax, 2
    jne .fail
    clc
    ret
.fail:
    stc
.done:
    ret

interactive_setup:
    ; Opening the menu must not enter DDC or enumerate the video BIOS. Some
    ; laptop firmware blocks on those calls. Probe only the mode the user
    ; explicitly asks to preview; AUTO starts with the 800x600 option.
    mov byte [menu_selection], 1
    mov dx, vc_profile_path
    mov ax, 0x3D00
    int 0x21
    jc .mode
    mov bx, ax
    mov dx, vc_profile
    mov cx, 4
    mov ah, 0x3F
    int 0x21
    pushf
    push ax
    mov ah, 0x3E
    int 0x21
    pop ax
    popf
    jc .mode
    cmp ax, 4
    jne .mode
    cmp dword [vc_profile], '0640'
    jne .other
    mov byte [menu_selection], 0
    jmp .mode
.other:
    cmp dword [vc_profile], '1024'
    jne .text_profile
    mov byte [menu_selection], 2
    jmp .mode
.text_profile:
    cmp dword [vc_profile], 'TEXT'
    jne .mode
    mov byte [menu_selection], 3
.mode:
    mov ax, 3
    call vc_bios
.draw:
    call draw_setup_menu
.key:
    xor ah, ah
    int 0x16
    cmp al, 27
    je .cancel
    cmp ah, 0x48
    je .up
    cmp ah, 0x50
    je .down
    cmp al, '1'
    jb .enter
    cmp al, '4'
    ja .enter
    sub al, '1'
    mov [menu_selection], al
    jmp .draw
.up:
    dec byte [menu_selection]
    and byte [menu_selection], 3
    jmp .draw
.down:
    inc byte [menu_selection]
    and byte [menu_selection], 3
    jmp .draw
.enter:
    cmp al, 13
    jne .key
    cmp byte [menu_selection], 3
    je run_windows_safe
    movzx ax, byte [menu_selection]
    shl ax, 1
    add ax, 0x101
    mov [vc_mode], ax
    call vc_begin
    jc .unavailable
    call vc_title
    mov word [vc_x], 4
    mov word [vc_y], 4
    mov dx, msg_preview
    call print
    movzx bx, byte [menu_selection]
    shl bx, 1
    mov dx, [menu_options + bx]
    call print
    mov dx, msg_preview_body
    call print
    call vc_flush
    cmp byte [vc_active], 1
    jne .unavailable
.drain:
    mov ah, 1
    int 0x16
    jz .timer
    xor ah, ah
    int 0x16
    jmp .drain
.timer:
    call preview_ticks
    mov [preview_tick], edx
.wait:
    mov ah, 1
    int 0x16
    jz .time
    xor ah, ah
    int 0x16
    cmp al, 27
    je .revert
    cmp al, 13
    je .confirm
.time:
    call preview_elapsed
    cmp edx, 218             ; approximately 12 seconds
    jae .revert
    sti
    hlt
    jmp .wait
.revert:
    call vc_end
    call draw_setup_menu
    mov dx, 0x1708
    call menu_cursor
    mov dx, msg_preview_reverted
    call print
    jmp .key
.unavailable:
    call vc_end
    mov dx, msg_mode_unavailable
    call print
    xor ah, ah
    int 0x16
    jmp .draw
.confirm:
    call vc_end
    cmp byte [menu_selection], 0
    je windows_profile_command.mode_640
    cmp byte [menu_selection], 1
    je windows_profile_command.mode_800
    jmp windows_profile_command.mode_1024
.cancel:
    mov ax, 0x4C00+VGASETUP_UNCHANGED
    int 0x21

; EDX = BIOS tick count read from the BDA. INT 1Ah/00h would consume the
; midnight rollover flag that DOS needs to advance the date.
preview_ticks:
    push es
    push ax
    mov ax, 0x40
    mov es, ax
    mov edx, [es:0x6C]
    pop ax
    pop es
    ret

; EDX = ticks since preview_tick. The daily count restarts at 0 after
; 1800AFh, so a smaller current value means midnight has passed.
preview_elapsed:
    call preview_ticks
    cmp edx, [preview_tick]
    jae .same_day
    add edx, 0x1800B0
.same_day:
    sub edx, [preview_tick]
    ret

draw_setup_menu:
    mov ax, 0x0600
    mov bh, 0x17
    xor cx, cx
    mov dx, 0x184F
    call vc_bios
    mov dx, 0x0208
    call menu_cursor
    mov dx, msg_menu_header
    call print
    mov dx, 0x0508
    call menu_cursor
    mov dx, msg_menu_description
    call print
    xor bp, bp
.option:
    mov dx, 0x0808
    mov ax, bp
    shl ax, 1
    add dh, al
    push dx
    call menu_cursor
    mov bh, 0x17
    movzx ax, byte [menu_selection]
    cmp bp, ax
    jne .highlight
    mov bh, 0x70
.highlight:
    mov cx, dx
    add dl, 61
    mov ax, 0x0600
    call vc_bios
    pop dx
    call menu_cursor
    mov bx, bp
    shl bx, 1
    mov dx, [menu_options + bx]
    call print
    inc bp
    cmp bp, 4
    jb .option
    mov dx, 0x1208
    call menu_cursor
    mov dx, msg_menu_keys
    call print
    mov dx, 0x1508
    call menu_cursor
    mov dx, msg_menu_footer
    call print
    mov dx, msg_menu_ready
    ; Explicit serial marker without changing the visible menu.
    push si
    mov si, dx
.serial:
    lodsb
    cmp al, '$'
    je .done
    push ax
    call setup_serial_byte
    pop ax
    jmp .serial
.done:
    pop si
    ret

menu_cursor:
    mov ah, 2
    xor bx, bx
    call vc_bios
    ret

close_profile_handles:
    push ax
    push bx
    mov bx, [source_handle]
    cmp bx, 0xFFFF
    je .target
    mov ah, 0x3E
    int 0x21
    mov word [source_handle], 0xFFFF
.target:
    mov bx, [target_handle]
    cmp bx, 0xFFFF
    je .done
    mov ah, 0x3E
    int 0x21
    mov word [target_handle], 0xFFFF
.done:
    pop bx
    pop ax
    ret

text_profile_command:
    mov si, arg_buffer
    call skip_spaces
    call skip_token
    call skip_spaces
    mov di, word_25
    call token_equal
    je .mode_25
    mov di, word_50
    call token_equal
    je .mode_50
    mov dx, msg_text_usage
    call print
    mov ax, 0x4C02
    int 0x21
.mode_25:
    mov word [text_profile_buf], '25'
    call save_text_profile
    mov byte [text_save_failed], 0
    jnc .apply_25
    inc byte [text_save_failed]
.apply_25:
    call apply_text_25
    mov dx, msg_text_25
    jmp .report
.mode_50:
    mov word [text_profile_buf], '50'
    call save_text_profile
    mov byte [text_save_failed], 0
    jnc .apply_50
    inc byte [text_save_failed]
.apply_50:
    call apply_text_50
    mov dx, msg_text_50
.report:
    call print
    cmp byte [text_save_failed], 0
    je .ok
    mov dx, msg_text_not_saved
    call print
.ok:
    mov ax, 0x4C00
    int 0x21

save_text_profile:
    mov dx, text_profile_path
    xor cx, cx
    mov ah, 0x3C
    int 0x21
    jc .failed
    mov bx, ax
    mov dx, text_profile_buf
    mov cx, 2
    mov ah, 0x40
    int 0x21
    pushf
    push ax
    mov ah, 0x3E
    int 0x21
    pop ax
    popf
    jc .failed
    cmp ax, 2
    jne .failed
    clc
    ret
.failed:
    stc
    ret

load_text_profile:
    mov word [text_profile_buf], '25'
    mov dx, text_profile_path
    mov ax, 0x3D00
    int 0x21
    jc apply_text_25
    mov bx, ax
    mov dx, text_profile_buf
    mov cx, 2
    mov ah, 0x3F
    int 0x21
    pushf
    push ax
    mov ah, 0x3E
    int 0x21
    pop ax
    popf
    jc apply_text_25
    cmp ax, 2
    jne apply_text_25
    cmp word [text_profile_buf], '50'
    je apply_text_50

apply_text_25:
    mov ax, 0x0003
    int 0x10
    mov ax, 0x1114
    xor bx, bx
    int 0x10
    ret

apply_text_50:
    mov ax, 0x0003
    int 0x10
    mov ax, 0x1112
    xor bx, bx
    int 0x10
    ret

run_modes:
    mov dx, vidmodes_path
    xor cx, cx
    call exec_program_empty
    jmp exit_from_exec

run_test:
    ; Pass everything following the TEST token to MODETEST.COM.
    mov si, arg_buffer
    call skip_spaces
    call skip_token
    call skip_spaces
    cmp byte [si], 0
    je show_help_error
    mov dx, modetest_path
    call exec_program_z_tail
    jmp exit_from_exec

refresh_command:
    ; VBE 2.x (including the T23 SuperSavage BIOS) exposes modes but no safe
    ; portable physical-refresh setter.  LCD native timing is 60 Hz; diagnose
    ; the exact BIOS modes instead of programming unsafe raw CRTC registers.
    mov si, arg_buffer
    call skip_spaces
    call skip_token
    call skip_spaces
    cmp byte [si], 0
    je .info
    mov di, word_auto
    call token_equal
    je .accepted
    mov di, word_60
    call token_equal
    jne .unsupported
.accepted:
    mov dx, msg_refresh_60
    call print
    mov ax, 0x4C00
    int 0x21
.unsupported:
    mov dx, msg_refresh_unsupported
    call print
    mov ax, 0x4C02
    int 0x21
.info:
    mov dx, msg_refresh_help
    call print
    mov ax, 0x4C00
    int 0x21

brightness_command:
    call thinkpad_detect
    jc .not_thinkpad
    mov si, arg_buffer
    call skip_spaces
    call skip_token
    call skip_spaces
    cmp byte [si], 0
    je .read
    call parse_0_100
    jc .usage
    ; Percent -> one of the T23 firmware's eight levels, rounded.
    mov bx, 7
    mul bx
    add ax, 50
    mov bl, 100
    div bl
    mov bl, al
    call ec_write_brightness
    jc .failed
    call nvram_store_brightness
    mov dx, msg_brightness_saved
    call print
    mov ax, 0x4C00
    int 0x21
.read:
    call ec_read_brightness
    jc .failed
    and al, 0x1F
    xor ah, ah
    mov dx, msg_brightness
    call print
    call print_u16
    mov dx, msg_level_suffix
    call print
    mov ax, 0x4C00
    int 0x21
.failed:
    mov dx, msg_ec_unavailable
    call print
    mov ax, 0x4C03
    int 0x21
.not_thinkpad:
    mov dx, msg_brightness_generic
    call print
    mov ax, 0x4C04
    int 0x21
.usage:
    mov dx, msg_brightness_usage
    call print
    mov ax, 0x4C02
    int 0x21

thinkpad_detect:
    ; Limit direct EC/NVRAM access to IBM firmware.  Generic machines still
    ; receive the universal VBE driver but never get ThinkPad register writes.
    push ax
    push cx
    push di
    push es
    mov ax, 0xF000
    mov es, ax
    xor di, di
    mov cx, 0xFFFD
.scan:
    cmp byte [es:di], 'I'
    jne .next
    cmp byte [es:di + 1], 'B'
    jne .next
    cmp byte [es:di + 2], 'M'
    je .found
.next:
    inc di
    loop .scan
    stc
    jmp .out
.found:
    clc
.out:
    pop es
    pop di
    pop cx
    pop ax
    ret

ec_wait_input_clear:
    push cx
    mov cx, 0xFFFF
.loop:
    in al, EC_STATUS_COMMAND
    test al, EC_STATUS_IBF
    jz .ok
    loop .loop
    stc
    jmp .out
.ok:
    clc
.out:
    pop cx
    ret

ec_wait_output_full:
    push cx
    mov cx, 0xFFFF
.loop:
    in al, EC_STATUS_COMMAND
    test al, EC_STATUS_OBF
    jnz .ok
    loop .loop
    stc
    jmp .out
.ok:
    clc
.out:
    pop cx
    ret

ec_read_brightness:
    call ec_wait_input_clear
    jc .fail
    mov al, EC_CMD_READ
    out EC_STATUS_COMMAND, al
    call ec_wait_input_clear
    jc .fail
    mov al, TP_EC_BACKLIGHT
    out EC_DATA, al
    call ec_wait_output_full
    jc .fail
    in al, EC_DATA
    clc
    ret
.fail:
    stc
    ret

ec_write_brightness:
    ; IN BL=0..7.  Preserve command bits in the EC brightness register.
    push ax
    push bx
    call ec_read_brightness
    jc .fail
    and al, 0xE0
    or al, bl
    mov bh, al
    call ec_wait_input_clear
    jc .fail
    mov al, EC_CMD_WRITE
    out EC_STATUS_COMMAND, al
    call ec_wait_input_clear
    jc .fail
    mov al, TP_EC_BACKLIGHT
    out EC_DATA, al
    call ec_wait_input_clear
    jc .fail
    mov al, bh
    out EC_DATA, al
    clc
    jmp .out
.fail:
    stc
.out:
    pop bx
    pop ax
    ret

nvram_store_brightness:
    ; Linux thinkpad-acpi uses NVRAM byte 5Eh for pre-ACPI IBM models.  Keep
    ; the unrelated upper nibble and checkpoint the selected EC level.
    push ax
    push bx
    cli
    in al, 0x70
    and al, 0x80
    mov ah, al
    or al, 0x5E
    out 0x70, al
    in al, 0x71
    and al, 0xF0
    or al, bl
    mov bh, al
    mov al, ah
    or al, 0x5E
    out 0x70, al
    mov al, bh
    out 0x71, al
    sti
    pop bx
    pop ax
    ret

exec_program_empty:
    mov si, empty_tail
    xor cx, cx
exec_program:
    ; IN DS:DX path, DS:SI argument bytes, CX byte count (without CR).
    push dx
    mov di, child_tail + 1
    mov ax, cx
    cmp cx, 126
    jbe .length_ok
    mov cx, 126
    mov ax, cx
.length_ok:
    mov [child_tail], al
    rep movsb
    mov byte [di], 0x0D
    pop dx
    mov word [child_params + 0], 0
    mov word [child_params + 2], child_tail
    mov word [child_params + 4], cs
    mov word [child_params + 6], 0x005C
    mov word [child_params + 8], cs
    mov word [child_params + 10], 0x006C
    mov word [child_params + 12], cs
    push cs
    pop es
    mov bx, child_params
    mov ax, 0x4B00
    int 0x21
    jc .fail
    mov ax, 0x4D00
    int 0x21
    xor ah, ah
    ret
.fail:
    mov al, 1
    ret

exec_program_z_tail:
    push si
    mov bx, si
    xor cx, cx
.count:
    cmp byte [bx], 0
    je .ready
    inc bx
    inc cx
    cmp cx, 126
    jb .count
.ready:
    pop si
    jmp exec_program

exit_from_exec:
    or al, al
    jz .ok
    mov dx, msg_exec_failed
    call print
    mov ax, 0x4C01
    int 0x21
.ok:
    mov ax, 0x4C00
    int 0x21

copy_command_tail:
    xor cx, cx
    mov cl, [0x0080]
    mov si, 0x0081
    mov di, arg_buffer
    cmp cx, 126
    jbe .copy
    mov cx, 126
.copy:
    rep movsb
    mov byte [di], 0
    ret

skip_spaces:
    cmp byte [si], ' '
    je .one
    cmp byte [si], 9
    jne .done
.one:
    inc si
    jmp skip_spaces
.done:
    ret

skip_token:
    cmp byte [si], 0
    je .done
    cmp byte [si], ' '
    je .done
    cmp byte [si], 9
    je .done
    inc si
    jmp skip_token
.done:
    ret

token_equal:
    ; Case-insensitive token comparison. ZF=1 only at a token boundary.
    push ax
    push si
    push di
.loop:
    cmp byte [di], 0
    je .boundary
    mov al, [si]
    cmp al, 'a'
    jb .upper_done
    cmp al, 'z'
    ja .upper_done
    sub al, 32
.upper_done:
    cmp al, [di]
    jne .different
    inc si
    inc di
    jmp .loop
.boundary:
    cmp byte [si], 0
    je .equal
    cmp byte [si], ' '
    je .equal
    cmp byte [si], 9
    je .equal
.different:
    pop di
    pop si
    pop ax
    or al, 1
    ret
.equal:
    pop di
    pop si
    pop ax
    cmp al, al
    ret

parse_0_100:
    xor ax, ax
    xor cx, cx
.digit:
    mov dl, [si]
    cmp dl, '0'
    jb .done
    cmp dl, '9'
    ja .done
    sub dl, '0'
    mov bx, 10
    mul bx
    xor dh, dh
    add ax, dx
    inc si
    inc cx
    cmp ax, 100
    ja .fail
    jmp .digit
.done:
    or cx, cx
    jz .fail
    call skip_spaces
    cmp byte [si], 0
    jne .fail
    clc
    ret
.fail:
    stc
    ret

print:
    cmp byte [cs:vc_active], 1
    jne .text
    push ax
    push dx
    push si
    mov si, dx
.graphics:
    lodsb
    cmp al, '$'
    je .done
    call vc_putc
    call setup_serial_byte
    jmp .graphics
.done:
    pop si
    pop dx
    pop ax
    ret
.text:
    mov ah, 0x09
    int 0x21
    ret

; Optional diagnostics must never wait for a disconnected serial cable or
; enter a BIOS transmit timeout once per character on a physical notebook.
setup_serial_byte:
    push ax
    push bx
    push cx
    push dx
    push es
    mov bl, al
    xor ax, ax
    mov es, ax
    mov dx, [es:0x400]
    test dx, dx
    jz .done
    add dx, 5
    mov cx, 256
.wait:
    in al, dx
    cmp al, 0xFF
    je .done
    test al, 0x20
    jnz .ready
    loop .wait
    jmp .done
.ready:
    sub dx, 5
    mov al, bl
    out dx, al
.done:
    pop es
    pop dx
    pop cx
    pop bx
    pop ax
    ret

print_hex16:
    push ax
    push bx
    push cx
    mov bx, ax
    mov cx, 4
.nibble:
    rol bx, 4
    mov dl, bl
    and dl, 0x0F
    cmp dl, 10
    jb .decimal
    add dl, 'A' - 10
    jmp .emit
.decimal:
    add dl, '0'
.emit:
    mov ah, 0x02
    int 0x21
    loop .nibble
    pop cx
    pop bx
    pop ax
    ret

print_u16:
    push ax
    push bx
    push cx
    push dx
    xor cx, cx
    mov bx, 10
.divide:
    xor dx, dx
    div bx
    push dx
    inc cx
    or ax, ax
    jnz .divide
.digits:
    pop dx
    add dl, '0'
    cmp byte [vc_active],1
    jne .text_digit
    mov al,dl
    call vc_putc
    call setup_serial_byte
    jmp .next_digit
.text_digit:
    mov ah, 0x02
    int 0x21
.next_digit:
    loop .digits
    pop dx
    pop cx
    pop bx
    pop ax
    ret

show_help_error:
    mov dx, msg_bad_command
    call print
show_help:
    mov dx, msg_help
    call print
    mov ax, 0x4C02
    int 0x21

word_boot       db '/BOOT', 0
word_config     db 'CONFIG', 0
word_auto       db 'AUTO', 0
word_win        db 'WIN', 0
word_set        db 'SET', 0
word_desktop    db 'DESKTOP', 0
word_text       db 'TEXT', 0
word_modes      db 'MODES', 0
word_detect     db 'DETECT', 0
word_status     db 'STATUS', 0
word_test       db 'TEST', 0
word_brightness db 'BRIGHTNESS', 0
word_refresh    db 'REFRESH', 0
word_help       db 'HELP', 0
word_60         db '60', 0
word_safe       db 'SAFE', 0
word_640        db '640', 0
word_800        db '800', 0
word_1024       db '1024', 0
word_1280       db '1280', 0
word_1600       db '1600', 0
word_1920       db '1920', 0
word_2048       db '2048', 0
word_2560       db '2560', 0
desktop_words dw word_800,word_1024,word_1280,word_1600,word_1920,word_2048,word_2560
desktop_profiles dd '0800','1024','1280','1600','1920','2048','2560'
desktop_dimensions dw 800,600,1024,768,1280,1024,1600,1200,1920,1080,2048,1152,2560,1440
word_25         db '25', 0
word_50         db '50', 0

setup_path      db '\SYSTEM\VIDEO\SETUP.EXE', 0
vidmodes_path   db '\SYSTEM\VIDEO\VIDMODES.COM', 0
modetest_path   db '\SYSTEM\VIDEO\MODETEST.COM', 0
text_profile_path db '\SYSTEM\VIDEO\VGASET.CFG', 0
windows_profile_safe db '\WINDOWS\SYSTEM.VGA', 0
windows_profile_800  db '\WINDOWS\SYSTEM.800', 0
windows_profile_1024 db '\WINDOWS\SYSTEM.102', 0
windows_profile_active db '\WINDOWS\SYSTEM.INI', 0
windows_profile_backup db '\WINDOWS\SYSTEM.BAK', 0
windows_profile_new db '\WINDOWS\SYSTEM.NEW', 0
setup_tail_text db ' \WINDOWS'
setup_tail_end:
empty_tail      db 0

msg_header db 'CiukiOS VGA setup 0.8.0', 13, 10, '$'
msg_safe_status db 'Safe status: no BIOS VBE or embedded-controller probe executed', 13, 10, '$'
msg_vbe db 'VBE BIOS version 0x', '$'
msg_memory db ', video memory ', '$'
msg_kib db ' KiB', 13, 10, '$'
msg_no_vbe db 'No VBE BIOS: safe VGA 640x480 fallback only', 13, 10, '$'
msg_thinkpad db 'IBM firmware detected: ThinkPad EC controls enabled', 13, 10, '$'
msg_generic db 'Generic hardware: safe VGA default; banked VBE profiles available', 13, 10, '$'
msg_text_mode db 'CiukiOS text console: 80x', '$'
msg_rows_suffix db ' rows', 13, 10, '$'
msg_brightness db 'LCD brightness level ', '$'
msg_level_suffix db '/7', 13, 10, '$'
msg_brightness_saved db 'LCD brightness applied and saved in ThinkPad NVRAM', 13, 10, '$'
msg_ec_unavailable db 'ThinkPad embedded controller did not respond', 13, 10, '$'
msg_brightness_generic db 'Brightness has no generic VBE control; use monitor/firmware keys', 13, 10, '$'
msg_brightness_usage db 'usage: vgasetup brightness <0..100>', 13, 10, '$'
msg_refresh_60 db 'Refresh AUTO/60 selected: LCD native timing remains BIOS-managed', 13, 10, '$'
msg_refresh_help db 'usage: vgasetup refresh <auto|60>; MODES lists BIOS capabilities', 13, 10, '$'
msg_refresh_unsupported db 'Requested refresh is not exposed safely by this VBE BIOS', 13, 10, '$'
msg_exec_failed db 'vgasetup: helper execution failed', 13, 10, '$'
msg_config_warning db 'Advanced VBE editor: changes Windows only; use WIN SAFE to recover', 13, 10, '$'
msg_win_usage db 'usage: vgasetup set <safe|640|800|1024>', 13, 10, '$'
msg_desktop_usage db 'usage: vgasetup desktop <800|1024|1280|1600|1920|2048|2560>',13,10,'$'
msg_desktop_preview db '[VGASETUP] DESKTOP PREVIEW ', '$'
msg_by db ' x ', '$'
msg_desktop_preview_body db 13,10,13,10
    db 'ENTER keeps this desktop and DOS resolution.',13,10
    db 'ESC cancels. The previous mode returns after 12 seconds.',13,10
    db 'The Windows display driver stays unchanged.',13,10,'$'
msg_desktop_saved db '[VGASETUP] Desktop resolution saved; Windows profile preserved.',13,10,'$'
msg_win_safe db 'Windows safe VGA 640x480 profile installed; restart Windows', 13, 10, '$'
msg_win_800 db 'Windows buffered VBE 800x600x8 profile installed; restart Windows', 13, 10, '$'
msg_win_1024 db 'Windows buffered VBE 1024x768x8 profile installed; restart Windows', 13, 10, '$'
msg_profile_failed db 'vgasetup: profile not changed (cannot save display configuration)', 13, 10, '$'
msg_text_usage db 'usage: vgasetup text <25|50>', 13, 10, '$'
msg_text_25 db 'CiukiOS 80x25 text console applied', 13, 10, '$'
msg_text_50 db 'CiukiOS 80x50 text console applied', 13, 10, '$'
msg_text_not_saved db 'Warning: mode is temporary because the current drive is read-only', 13, 10, '$'
msg_bad_command db 'vgasetup: invalid command', 13, 10, '$'
msg_help db 'VGASETUP commands:', 13, 10
         db '  vgasetup              interactive shared display setup', 13, 10
         db '  vgasetup set 640|800|1024|safe  shared shell + Windows profile', 13, 10
         db '  vgasetup desktop 800|1024|1280|1600|1920|2048|2560',13,10
         db '                       preview native desktop/DOS resolution',13,10
         db '  vgasetup text 25|50   apply and persist CiukiOS console rows', 13, 10
         db '  vgasetup win safe     recoverable Windows VGA 640x480 profile', 13, 10
         db '  vgasetup win 800      Windows buffered VBE 800x600x256', 13, 10
         db '  vgasetup win 1024     Windows buffered VBE 1024x768x256', 13, 10
         db '  vgasetup config       interactive shared display setup', 13, 10
         db '  vgasetup status       safe status (never probes BIOS/EC)', 13, 10
         db '  vgasetup detect       explicit VBE + ThinkPad LCD diagnosis', 13, 10
         db '  vgasetup modes        VBE + EDID capability diagnosis', 13, 10
         db '  vgasetup test <mode>  reversible physical video-mode test', 13, 10
         db '  vgasetup brightness <0..100>', 13, 10
         db '  vgasetup refresh <auto|60>', 13, 10, '$'

align 16
vbe_info times 512 db 0
arg_buffer times 128 db 0
child_tail times 128 db 0
child_params times 14 db 0
source_handle dw 0xFFFF
target_handle dw 0xFFFF
profile_source_ptr dw 0
profile_success_ptr dw 0
text_profile_buf dw '25'
text_save_failed db 0
io_buffer times 512 db 0

msg_profile_unavailable db 'Display mode is not supported; settings have not changed.',13,10,'$'
msg_auto db 'Automatic desktop and DOS display selection enabled',13,10,'$'
global_profile dd 'TEXT'
global_profile_new db '\SYSTEM\VIDEO\DISPLAY.NEW', 0
global_profile_backup db '\SYSTEM\VIDEO\DISPLAY.BAK', 0
global_had_backup db 0
menu_selection db 3
preview_tick dd 0
menu_options dw menu_640, menu_800, menu_1024, menu_text
menu_640 db '1   640 x 480     Shell 80 x 30 characters   | Windows 640 x 480', '$'
menu_800 db '2   800 x 600     Shell 100 x 37 characters  | Windows 800 x 600', '$'
menu_1024 db '3   1024 x 768    Shell 128 x 48 characters  | Windows 1024 x 768', '$'
menu_text db '4   VGA text      Shell 80 columns         | Windows safe VGA', '$'
msg_menu_header db 'CIUKIOS  /  DISPLAY SETTINGS', '$'
msg_menu_description db 'Choose a shared resolution for the shell and Windows:', '$'
msg_menu_keys db 'Arrows or 1-4: select    Enter: preview/apply    Esc: return to shell', '$'
msg_menu_footer db 'Graphics modes require confirmation in the preview.', '$'
msg_menu_ready db '[VGASETUP] MENU READY', 13, 10, '$'
msg_preview db '[VGASETUP] PREVIEW', 13, 10, 13, 10, '$'
msg_preview_body db 13, 10, 13, 10
    db 'ENTER keeps this resolution for the shell and Windows.', 13, 10
    db 'ESC cancels. The previous mode returns after 12 seconds.', 13, 10, 13, 10
    db 'CiukiOS SHELL C:\APPS> All text should be readable.', 13, 10
    db 'ABCDEFGHIJKLMNOPQRSTUVWXYZ  abcdefghijklmnopqrstuvwxyz  0123456789', 13, 10, '$'
msg_preview_reverted db '[VGASETUP] Preview cancelled; settings preserved.', 13, 10, '$'
msg_mode_unavailable db 'VBE mode unavailable. Press a key to return to the menu.', 13, 10, '$'
msg_global_saved db '[VGASETUP] Shared resolution saved for the shell and Windows.', 13, 10, '$'

%include "src/com/vbe_console.inc"

align 16
; Include room for firmware calls and nested hardware interrupt handlers.
stack_space times 4096 db 0
stack_top:
image_end:
