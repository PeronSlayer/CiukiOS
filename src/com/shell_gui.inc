%include "src/com/ui_theme.inc"
%define UI_WINDOW_COUNT 32
%include "src/com/shell_assets.inc"
; CiukiOS native desktop. Single-tasking applications retain the DOS ABI.
; Optional BIOS-text sessions retain the desktop through a scoped runtime.
%include "src/com/shell_gui_draw.inc"

%macro U_BUTTON 6
    mov bx,%1
    mov dx,%2
    mov cx,%3
    mov si,%4
    mov di,%5
    mov ax,%6
    call ui_button
%endmacro

ui_enter:
    call ui_assets_begin
    jnc .assets_ready
    mov byte [ui_request],0
    call restore_shell_video_state
    mov dx,ui_assets_error
    mov ah,9
    int 0x21
    jmp main_loop
.assets_ready:
%ifdef BOOT_DIAG_PALETTE
    push ax
    mov al,7                    ; pink: desktop assets returned
    call boot_diag_palette
    pop ax
%endif
    call app_settings_load    ; colours, mouse, keyboard, system font
%ifdef BOOT_DIAG_PALETTE
    push ax
    mov al,8                    ; lime: desktop settings returned
    call boot_diag_palette
    pop ax
%endif
    ; The startup melody is the event sound driver's first event: it starts
    ; with the first desktop frame and plays asynchronously.
    cmp byte [ui_sound_played],1
    je .sound_ready
    mov byte [ui_sound_played],1
    mov byte [ui_sfx_pending],7
.sound_ready:
    call ui_sfx_begin
%ifdef BOOT_DIAG_PALETTE
    push ax
    mov al,9                    ; white: sound driver returned
    call boot_diag_palette
    pop ax
%endif
    sti
    cld
    mov byte [ui_command_running],0
    mov byte [ui_request],0
    call ui_video_begin
%ifdef BOOT_DISK_LOG
    mov al,0x0E                 ; video initialization returned
    call boot_log_stage
%endif
    cmp byte [ui_exec_error],0
    jne .error
    call app_keys_begin
    call app_resume
    call ui_windows_resume
    call ui_welcome_start
%ifdef BOOT_DISK_LOG
    mov al,0x0F                 ; application/window resume returned
    call boot_log_stage
%endif
    jmp .ready
.error:
    mov byte [ui_dialog],4
    mov byte [ui_exec_error],0
    call ui_windows_open
.ready:
    mov byte [ui_ready_pending],1
ui_redraw:
    call ui_sfx_service
    mov byte [ui_window_repaint],0
    cmp byte [ui_full_redraw],1
    jne .partial
    mov byte [ui_full_redraw],0
    mov byte [ui_comp_dirty],0
    mov byte [ui_bar_dirty],0
    jmp .paint
.partial:
    cmp byte [ui_comp_dirty],1
    je .paint
    call ui_damage_active
.paint:
    call ui_draw
    cmp byte [ui_bar_dirty],1
    jne .painted
    mov byte [ui_bar_dirty],0
    xor bx,bx
    mov dx,[ui_height]
    sub dx,32
    mov cx,[ui_width]
    mov si,32
    call ui_comp_damage
    call ui_draw
.painted:
    mov si,ui_paint_marker
    call ui_serial
    cmp byte [ui_ready_pending],1
    jne ui_loop
%ifdef BOOT_DISK_LOG
    mov al,0x10                 ; first desktop frame painted
    call boot_log_stage
    call boot_log_input_state
%endif
    mov byte [ui_ready_pending],0
    mov si,ui_mode_text
    call ui_serial
    mov si,ui_newline
    call ui_serial
    mov si,ui_ready_marker
    call ui_serial
ui_loop:
    sti
    cmp word [ui_mode_request],0
    je .mode_ready
    call ui_mode_switch
    jmp ui_redraw
.mode_ready:
    call ui_sfx_service
    call poll_pending_power_action
    call ui_motion_poll
    call ui_gpu_poll
%ifdef BOOT_DISK_LOG
    cmp byte [boot_log_event_entered],1
    je .event_enter_logged
    mov byte [boot_log_event_entered],1
    mov al,0x13                 ; first UI input poll about to run
    call boot_log_stage
.event_enter_logged:
%endif
    call ui_event
%ifdef BOOT_DISK_LOG
    cmp byte [boot_log_event_returned],1
    je .event_return_logged
    mov byte [boot_log_event_returned],1
    push ax
    mov al,0x14                 ; first UI input poll returned
    call boot_log_stage
    pop ax
.event_return_logged:
%endif
    test ax,ax
    jnz .event_action
    ; Modules may queue a precise damage rectangle while returning zero.
    ; Present it now, before a VM poll or HLT can delay a live brush stroke.
    cmp byte [ui_comp_dirty],1
    je ui_redraw
    jmp .idle
.event_action:
    cmp ax,251
    je ui_redraw_input
    cmp ax,250
    je ui_redraw
    cmp ax,30
    jb ui_action
    cmp ax,36
    jb ui_item_click
    jmp ui_action
.idle:
    call app_poll
    cmp ax,249
    je ui_action
    test ax,ax
    jnz ui_redraw
    cmp byte [ui_comp_dirty],1
    je ui_redraw
    call ui_clock_poll
    ; Catch up the hidden page only when it is reused for composition.
    ; At that point current damage is known: pixels about to be replaced
    ; need neither a VRAM read nor a copy of the previous game frame.
    cmp byte [app_busy],1
    je ui_loop
    call app_idle_tick
    sti
%ifdef BOOT_DISK_LOG
    cmp byte [boot_log_halt_entered],1
    je .halt_enter_logged
    mov byte [boot_log_halt_entered],1
    mov al,0x15                 ; first idle HLT about to run
    call boot_log_stage
.halt_enter_logged:
%endif
    hlt
%ifdef BOOT_DISK_LOG
    cmp byte [boot_log_halt_returned],1
    je .halt_return_logged
    mov byte [boot_log_halt_returned],1
    mov al,0x18                 ; first idle HLT woke on an interrupt
    call boot_log_stage
.halt_return_logged:
%endif
    call app_idle_woke
    jmp ui_loop

ui_redraw_input:
    ; Coalesce queued editing keys before painting the field. Rapid typing
    ; must not spend one full field repaint per already buffered character.
.pending:
    mov ah,0x11
    int 0x16
    jz .paint
    cmp ah,0xA5
    je .paint
    cmp ah,0x94
    je .paint
    cmp ah,0x6B
    je .paint
    cmp ah,0x0F             ; Tab/Shift+Tab: ui_event reads the shift state
    je .paint
    cmp ah,0x3B
    jb .edit
    cmp ah,0x3E
    jbe .paint
.edit:
    mov ah,0x10
    int 0x16
    call ui_run_key
    cmp ax,251
    je .pending
    test ax,ax
    jz .pending
    cmp ax,250
    je ui_redraw
    jmp ui_action
.paint:
    mov bx,[ui_dialog_x]
    add bx,22
    mov dx,[ui_dialog_y]
    add dx,76
    mov cx,420
    mov si,28
    call ui_comp_damage
    jmp ui_redraw

ui_action:
    call ui_damage_active
    cmp ax,249
    jne .not_app_command
    mov si,app_command
    jmp ui_launch_command
.not_app_command:
    cmp byte [ui_active_window],7
    jne .menu_closed
    cmp ax,9
    ja .menu_closed
    mov byte [ui_window_flags+7],0
.menu_closed:
    cmp ax,100
    jb .non_files
    cmp ax,149
    ja .non_files
    call app_action
    cmp ax,249
    jne .not_module_command
    mov si,app_command
    jmp ui_launch_command
.not_module_command:
    test ax,ax
    jz ui_loop
    jmp ui_redraw
.non_files:
    mov bl,12                    ; CiukNote
    cmp ax,29
    je .module
    mov bl,16                    ; CiukPaint
    cmp ax,28
    jne .non_notepad
.module:
    xor si,si
    call app_open
    jmp ui_redraw
.non_notepad:
    cmp ax,19
    jne .non_maximize
    call app_maximize
    jmp ui_redraw
.non_maximize:
    cmp ax,180
    jb .non_wallpaper
    cmp ax,199
    ja .non_wallpaper
    call wp_action
    test ax,ax
    jz ui_loop
    jmp ui_redraw
.non_wallpaper:
    cmp ax,90
    jb .native_task
    cmp ax,94
    ja .native_task
    sub ax,90
    shl ax,1
    mov bx,ax
    mov si,[ui_display_commands+bx]
    jmp .apply_setting
.native_task:
    cmp ax,200
    jb .standard
    cmp ax,200+UI_WINDOW_COUNT-1
    ja .standard
    sub al,200
    mov [ui_dialog],al
    call ui_windows_open
    jmp ui_redraw
.standard:
    cmp ax,18
    je .close_dialog
    cmp ax,17
    je .minimize_native
    cmp ax,14
    je .media_floppy
    cmp ax,15
    je .media_usb
    cmp ax,16
    je .media_cd
    cmp ax,1
    je .library
    cmp ax,2
    je .files
    cmp ax,3
    je ui_dos_window
    cmp ax,25
    je ui_to_dos
    cmp ax,58
    je .run_full_screen
    cmp ax,59
    je ui_window_game
    cmp ax,60
    je ui_window_game
    cmp ax,4
    je .run
    cmp ax,5
    je .settings
    cmp ax,6
    je .about
    cmp ax,7
    je .power
    cmp ax,8
    je .display
    cmp ax,9
    je .sound
    cmp ax,23
    je .tasks
    cmp ax,24
    je .wallpaper
    cmp ax,61
    je .control_panel
    cmp ax,62
    je .installed_programs
    cmp ax,63
    je .browser
    cmp ax,13
    je .menu
    cmp ax,80
    je .top_sound
    cmp ax,81
    je .top_network
    cmp ax,85
    je .top_datetime
    cmp ax,82
    jb .top_done
    cmp ax,84
    jbe .top_performance
.top_done:
    cmp ax,10
    je .minimize
    cmp ax,11
    je .maximize
    cmp ax,12
    je .minimize
    cmp ax,20
    jb .modal
    cmp ax,22
    ja .modal
    sub ax,20
    mov [ui_page],al
    mov word [ui_focus],44
    mov byte [ui_selection],0
    mov word [ui_last_item],0
    jmp ui_redraw
.display:
    mov bl,APP_WIN_DISPLAY
    xor si,si
    call app_open
    jmp ui_redraw
.sound:
    mov byte [ui_dialog],6
    call ui_windows_open
    jmp ui_redraw
.menu:
    ; The brand opens a drop-down menu of the desktop module; the classic
    ; centred window remains for a session without that module.
    mov ax,1
    call app_desktop_topbar_click
    jnc ui_redraw
    mov byte [ui_dialog],7
    call ui_windows_open
    jmp ui_redraw
.library:
    mov byte [ui_window_visible],1
    mov byte [ui_dialog],0
    call ui_windows_open
    jmp ui_redraw
.files:
    mov bl,8
    xor si,si
    call app_open
    jmp ui_redraw
.tasks:
    mov bl,9
    xor si,si
    call app_open
    jmp ui_redraw
.control_panel:
    mov bl,13
    xor si,si
    call app_open
    jmp ui_redraw
.top_sound:
    ; The speaker: a small volume popup; its gear opens the Sound applet.
    mov ax,2
    call app_desktop_topbar_click
    jnc ui_redraw
    mov bl,13
    mov si,ui_arg_sound
    call app_open
    jmp ui_redraw
.top_network:
    mov bl,13
    mov si,ui_arg_network
    call app_open
    jmp ui_redraw
.top_datetime:
    mov bl,13
    mov si,ui_arg_datetime
    call app_open
    jmp ui_redraw
.top_performance:
    mov bl,9
    mov si,ui_arg_performance
    call app_open
    jmp ui_redraw
.installed_programs:
    mov bl,8
    mov si,ui_installed_dir
    call app_open
    jmp ui_redraw
.browser:
    mov bl,17
    xor si,si
    call app_open
    jmp ui_redraw
.wallpaper:
    call wp_open
    jmp ui_redraw
.media_floppy:
    mov si,app_media_floppy
    jmp .media_open
.media_usb:
    mov si,app_media_usb
    jmp .media_open
.media_cd:
    mov si,app_media_cd
.media_open:
    mov bl,8
    call app_open
    jmp ui_redraw
.settings:
    mov byte [ui_window_visible],1
    mov byte [ui_page],2
    mov byte [ui_selection],0
    xor bx,bx
    call ui_windows_raise
    jmp ui_redraw
.run:
    mov byte [ui_dialog],1
    cmp byte [ui_window_flags+1],0
    jne .run_ready
    mov byte [ui_run_len],0
    mov byte [ui_run_cursor],0
    mov byte [ui_run_text],0
.run_ready:
    call ui_windows_open
    jmp ui_redraw
.about:
    mov bl,2
    xor si,si
    call app_open
    jmp ui_redraw
.power:
    mov byte [ui_dialog],3
    call ui_windows_open
    jmp ui_redraw
.minimize:
    mov byte [ui_window_visible],0
    call ui_windows_unfocus
    jmp ui_redraw
.minimize_native:
    movzx bx,byte [ui_active_window]
    mov byte [ui_window_flags+bx],2
    call ui_windows_unfocus
    jmp ui_redraw
.maximize:
    xor byte [ui_maximized],1
    cmp byte [ui_maximized],0
    je .normal
    mov ax,[ui_wx]
    mov [ui_saved_wx],ax
    mov ax,[ui_wy]
    mov [ui_saved_wy],ax
    mov ax,[ui_ww]
    mov [ui_saved_ww],ax
    mov ax,[ui_wh]
    mov [ui_saved_wh],ax
    mov word [ui_wx],8
    mov word [ui_wy],34
    mov ax,[ui_width]
    sub ax,16
    mov [ui_ww],ax
    mov ax,[ui_height]
    sub ax,74
    mov [ui_wh],ax
    call ui_library_focus_clamp
    call ui_damage_active
    jmp ui_redraw
.normal:
    mov ax,[ui_saved_wx]
    mov [ui_wx],ax
    mov ax,[ui_saved_wy]
    mov [ui_wy],ax
    mov ax,[ui_saved_ww]
    mov [ui_ww],ax
    mov ax,[ui_saved_wh]
    mov [ui_wh],ax
    call ui_library_focus_clamp
    call ui_damage_active
    jmp ui_redraw
.modal:
    cmp ax,55
    je .play_event
    cmp ax,56
    je .sounds_on
    cmp ax,57
    je .sounds_off
    cmp ax,50
    jb .regular_modal
    cmp ax,57
    ja .regular_modal
    sub ax,50
    shl ax,1
    mov bx,ax
    mov si,[ui_settings_commands+bx]
.apply_setting:
    mov byte [ui_settings_pending],1
    mov byte [ui_dialog],0
    jmp ui_launch_command
.play_event:
    xor ax,ax
    call ui_sfx_call
    mov byte [ui_sfx_pending],4
    jmp ui_redraw
.sounds_on:
    mov al,'1'
    jmp ui_sound_set
.sounds_off:
    mov al,'0'
    jmp ui_sound_set
.regular_modal:
    cmp ax,40
    je .run_commit
    cmp ax,41
    je .close_dialog
    cmp ax,42
    je .restart
    cmp ax,43
    je .shutdown
    cmp ax,44
    je .open_selected
    jmp ui_loop
.close_dialog:
    call ui_windows_close
    jmp ui_redraw
.run_commit:
    call app_run_builtin      ; notepad, explorer, taskmgr: desktop applications
    jc ui_redraw
    ; The VM-backed window runs from the normal desktop event loop.
    jmp ui_run_window
.run_full_screen:
    cmp byte [ui_run_len],0
    je ui_redraw             ; queued Backspace + Enter must paint the empty field
    call ui_windows_close
    mov si,ui_run_text
    mov byte [ui_command_running],1
    jmp ui_queue_command
.restart:
    mov si,ui_cmd_restart
    jmp ui_launch_command
.shutdown:
    mov si,ui_cmd_shutdown
    jmp ui_launch_command
.open_selected:
    cmp byte [ui_window_visible],0
    je .library
    jmp ui_item_open

; Item selection follows desktop double-click conventions; Enter also opens.
ui_item_click:
    ; A mouse release already damages the clicked tile. The previous
    ; selection also belongs to this repaint or its highlight would remain.
    call ui_damage_active
    mov bx,ax
    sub al,30
    mov [ui_selection],al
    mov word [ui_focus],44
    push es
    mov ax,0x40
    mov es,ax
    mov ax,[es:0x6C]
    pop es
    mov dx,ax
    sub ax,[ui_last_click]
    cmp bx,[ui_last_item]
    jne .selected
    cmp ax,9
    ja .selected
    mov word [ui_last_item],0
    jmp ui_item_open
.selected:
    mov [ui_last_item],bx
    mov [ui_last_click],dx
    jmp ui_redraw

ui_item_open:
    call ui_selected_record
    mov ax,[si+6]
    test ax,ax
    jnz ui_action
    mov si,[si+4]
    ; Windows 95 style: DOS programs open in a DOS window whenever the desktop
    ; can host one. Games start their own binary there (the window's devices
    ; replace the full-screen audio launchers, which stay the fallback).
    push si
    call dw_vm_available
    pop si
    jc ui_launch_command
    mov bx,ui_window_commands
.window_command:
    mov ax,[bx]
    test ax,ax
    jz .copy_command
    add bx,4
    cmp ax,si
    jne .window_command
    mov si,[bx-2]
.copy_command:
    mov di,ui_run_text
    xor cx,cx
.copy:
    lodsb
    test al,al
    jz .copied
    stosb
    inc cx
    jmp .copy
.copied:
    mov [ui_run_len],cl
    mov byte [ui_dialog],0
    jmp ui_run_window
ui_launch_command:
    mov byte [ui_dialog],0
    mov byte [ui_command_running],2
ui_queue_command:
    mov byte [ui_exec_error],0
    push si
    call ui_video_end
    pop si
    push si
    call ui_assets_end
    pop si
    mov di,input_buf
    call copy_z_to_di
    mov si,input_buf
    xor cx,cx
.count:
    cmp byte [si],0
    je .ready
    inc si
    inc cx
    jmp .count
.ready:
    mov si,input_buf
    mov [ui_command_length],cx
    mov si,ui_launch_marker
    call ui_serial
    mov si,input_buf
    call ui_serial
    mov si,ui_newline
    call ui_serial
    mov si,input_buf
    mov cx,[ui_command_length]
    jmp main_loop.line_ready

ui_command_return:
    cmp byte [ui_settings_pending],1
    jne .normal_return
    mov byte [ui_settings_pending],0
    cmp byte [ui_child_exit],0
    jz .normal_return
    cmp byte [ui_child_exit],5 ; VGASETUP: preview cancelled, nothing saved
    je .normal_return
    mov byte [ui_exec_error],1
.normal_return:
    cmp byte [ui_command_running],1
    jne ui_enter
    mov byte [ui_command_running],0
    mov si,ui_return_hint
    call print_dual_dollar_string
    call vc_flush
    xor ah,ah
    int 0x16
    jmp ui_enter

ui_to_dos:
    mov byte [ui_dialog],0
    mov byte [ui_exec_error],0
    call ui_video_end
    call ui_assets_end
    call restore_shell_video_state
    call desktop_home
    mov si,ui_dos_hint
    call print_dual_dollar_string
    mov si,ui_dos_marker
    call ui_serial
    jmp main_loop

ui_desktop_command:
    mov byte [ui_request],1
    ret

ui_serial:
    pushad
    mov dx,0x3F8+2
    in al,dx
    mov bl,1
    and al,0xC0
    cmp al,0xC0 ; FIFO enabled: an empty transmitter accepts 16 bytes
    jne .next
    mov bl,16
.next:
    mov dx,0x3F8+5
    mov cx,65535
.wait:
    in al,dx
    test al,0x20
    jnz .ready
    loop .wait
    jmp .done
.ready:
    mov bh,bl
    mov dx,0x3F8
.send:
    lodsb
    test al,al
    jz .done
    out dx,al
    dec bh
    jnz .send
    jmp .next
.done:
    popad
    ret

ui_video_begin:
    ; Settings are refreshed at entry, never from a paint/animation callback.
    call ui_sound_preference
    call ui_icons_begin
%ifdef BOOT_DIAG_PALETTE
    push ax
    mov al,10                   ; grey: UI icon load returned
    call boot_diag_palette
    pop ax
%endif
    mov ax,[ui_width]
    mov [ui_previous_width],ax
    mov ax,[ui_height]
    mov [ui_previous_height],ax
    cld
    mov byte [ui_mouse_saved],0
    mov byte [ui_mouse_ready],0
    mov byte [ui_pointer_visible],0
    mov byte [ui_previous_button],0
    mov byte [ui_mouse_press_pending],0
    mov byte [ui_mouse_release_pending],0
    mov word [ui_mouse_buttons],0
    mov byte [app_capture],0
    mov byte [ui_button_down],0
    mov byte [ui_dragging],0
    mov byte [ui_full_redraw],1
    mov byte [ui_motion_phase],0
    ; Save client mouse state before resetting our private desktop session.
    xor bx,bx
    mov ax,0x15
    int 0x33
    test bx,bx
    jz .reset
    cmp bx,512
    ja .reset
    mov es,[ui_assets_seg]
    mov dx,ui_mouse_state
    mov ax,0x16
    int 0x33
    mov byte [ui_mouse_saved],1
.reset:
    xor ax,ax
    int 0x33
    cmp ax,0xFFFF
    jne .mode
    mov byte [ui_mouse_ready],1
.mode:
%ifdef BOOT_DISK_LOG
    mov al,0x17                 ; INT 33h mouse reset unavailable
    cmp byte [ui_mouse_ready],1
    jne .mouse_logged
    mov al,0x16                 ; INT 33h mouse reset succeeded
.mouse_logged:
    call boot_log_stage
%endif
%ifdef BOOT_DIAG_PALETTE
    push ax
    mov al,11                   ; teal: mouse initialization returned
    call boot_diag_palette
    pop ax
%endif
    cmp byte [driver_force_vga],1
    jne .detected_video
    mov byte [vc_force_safe],1
.detected_video:
    ; Only an active console is torn down: at boot the splash stays until
    ; the desktop's own mode set (vc_end would flash text mode 03h).
    cmp byte [vc_active],0
    je .console_clear
    call vc_end
.console_clear:
    cmp word [ui_mode_request],0
    je .load_profile
    mov ax,[ui_mode_request]
    mov [vc_mode],ax
    mov byte [vc_profile],'M'
    jmp .profile_ready
.load_profile:
    call vc_load_profile
    ; A saved DOS text/640 profile must not reduce the desktop below SVGA.
    ; Keep the explicit recovery switch available for firmware without VBE.
    cmp byte [vc_force_safe],1
    je .profile_ready
    cmp word [vc_mode],0
    je .desktop_auto
    call vc_probe
    jc .desktop_auto
    cmp byte [vc_profile],'M' ; an explicitly previewed mode may be 640x480
    je .profile_ready
    cmp word [vc_info+18],800
    jb .desktop_auto
    cmp word [vc_info+20],600
    jae .profile_ready
.desktop_auto:
    call vc_auto
.profile_ready:
%ifdef BOOT_DIAG_PALETTE
    push ax
    mov al,12                   ; sky blue: video profile resolved
    call boot_diag_palette
    pop ax
%endif
    mov byte [ui_vbe],0
    mov word [ui_width],640
    mov word [ui_height],480
    cmp word [vc_mode],0
    je .vga
    call vc_begin
    jc .vga
    mov byte [ui_vbe],1
    mov ax,[vc_info+18]
    mov [ui_width],ax
    mov ax,[vc_info+20]
    mov [ui_height],ax
    jmp .palette
.vga:
%ifdef BOOT_DIAG_PALETTE
    push ax
    mov al,13                   ; amber: calling VGA mode 12h
    call boot_diag_palette
    pop ax
%endif
    mov ax,0x12
    call vc_bios
    push cs
    pop es
    mov dx,ui_attributes
    mov ax,0x1002
    call vc_bios
.palette:
    push cs
    pop es
    mov dx,ui_palette
    xor bx,bx
    mov cx,16
    cmp byte [ui_vbe],1
    jne .vga_palette
    mov cx,VC_PALETTE_COLORS
    call vc_set_palette
    jmp .palette_done
.vga_palette:
    mov ax,0x1012
    call vc_bios
.palette_done:
    mov byte [ui_active],1
    call ui_comp_begin
%ifdef BOOT_DISK_LOG
    cmp byte [ui_vbe],1
    je .band_logged
    mov al,0x1A                 ; VGA scratch allocation failed: direct fallback
    cmp word [ui_comp_seg],0
    je .record_band
    mov al,0x19                 ; VGA indexed scratch band allocated
.record_band:
    call boot_log_stage
.band_logged:
%endif
    cmp byte [ui_comp_recover],1
    jne .surface_ready
    mov byte [ui_comp_recover],0
    call ui_comp_end
    jmp .mode
.surface_ready:
    call wp_begin
    cmp byte [ui_layout_ready],1
    jne .layout
    mov ax,[ui_width]
    cmp ax,[ui_previous_width]
    jne .layout
    mov ax,[ui_height]
    cmp ax,[ui_previous_height]
    je .mouse
.layout:
    mov byte [ui_layout_ready],1
    mov byte [ui_maximized],0
    mov ax,[ui_width]
    sub ax,150
    cmp ax,510
    jae .wide_min
    mov ax,510
.wide_min:
    cmp ax,730
    jbe .wide_max
    mov ax,730
.wide_max:
    mov [ui_ww],ax
    mov word [ui_wh],370
    mov ax,[ui_width]
    sub ax,[ui_ww]
    shr ax,1
    add ax,32
    mov [ui_wx],ax
    mov ax,[ui_height]
    sub ax,370
    shr ax,1
    sub ax,8
    mov [ui_wy],ax
.mouse:
    call ui_library_focus_clamp
    call ui_windows_clamp
    cmp byte [ui_mouse_ready],1
    jne .done
    mov ax,7
    xor cx,cx
    mov dx,[ui_width]
    sub dx,24
    int 0x33
    mov ax,8
    xor cx,cx
    mov dx,[ui_height]
    sub dx,16
    int 0x33
    mov ax,[ui_width]
    shr ax,1
    mov [ui_mouse_x],ax
    mov cx,ax
    mov ax,[ui_height]
    shr ax,1
    mov [ui_mouse_y],ax
    mov dx,ax
    mov ax,4
    int 0x33
.done:
    push cs
    pop ds
    push cs
    pop es
    call ui_clock_read
    mov ah,0x19
    int 0x21
    mov [ui_boot_drive],al
    ret

; Deferred to the main loop: no module callback or modal window is unloaded.
ui_mode_switch:
    mov byte [vc_graphics_transition],1
    call wp_end
    call ui_pointer_hide
    call ui_comp_end
    call ui_video_begin
    mov word [ui_mode_request],0
    mov byte [vc_graphics_transition],0
    ret

; Run once per shell lifetime; missing/invalid preference shows the welcome.
ui_welcome_start:
    cmp byte [ui_welcome_done],1
    je .done
    mov byte [ui_welcome_done],1
    mov dx,ui_welcome_path
    mov ax,0x3D00
    int 0x21
    jc .show
    mov bx,ax
    mov dx,ui_welcome_value
    mov cx,2
    mov ah,0x3F
    int 0x21
    push ax
    mov ah,0x3E
    int 0x21
    pop ax
    cmp ax,1
    jne .show
    cmp byte [ui_welcome_value],'0'
    je .done
.show:
    mov bl,2
    xor si,si
    call app_open
.done:
    ret

ui_video_end:
    call ui_sfx_end
    call app_suspend
    call app_keys_end
    call wp_end
    mov byte [ui_motion_phase],0
    call ui_pointer_hide
    call ui_comp_end
    call ui_icons_end
    mov byte [ui_active],0
    call vc_end
    cmp byte [ui_mouse_saved],1
    jne .done
    mov es,[ui_assets_seg]
    mov dx,ui_mouse_state
    mov ax,0x17
    int 0x33
.done:
    push cs
    pop ds
    push cs
    pop es
    clc
.retained:
    ret

ui_draw:
    call ui_comp_draw
    cmp byte [ui_comp_recover],1
    jne .done
    mov byte [ui_comp_recover],0
    call ui_mode_switch
    mov byte [ui_full_redraw],0
    mov byte [ui_bar_dirty],0
    mov byte [ui_comp_dirty],0
    call ui_comp_draw
.done:
    ret
ui_draw_topbar_visuals:
    U_RECT 0,0,[ui_width],29,7
    U_RECT 0,28,[ui_width],1,8
    mov bx,5
    mov dx,2
    call ciuki_logo24
    U_TEXT 33,6,ut_brand,0,1
    mov bx,[ui_width]
    sub bx,72
    U_BUTTON bx,3,66,23,ut_dos,3
    ret
ui_draw_scene:
    mov word [ui_hit_count],0
    mov byte [ui_hit_owner],0
    ; A band inside a visible window's client area is painted entirely by
    ; that window: skip the background, wallpaper, top bar and desktop icons
    ; (a DOS window presenting a game composes only its own frame).
    ; The hit table is rebuilt with every band, so the top bar's hit areas
    ; are registered even when its pixels are not drawn.
    mov byte [ui_scene_covered],0
    cmp byte [ui_composing],1
    jne .background
    push si
    mov si,-1
    call ui_window_band_covered
    pop si
    jnc .background
    mov byte [ui_scene_covered],1
    jmp .top_hits
.background:
    U_RECT 0,0,[ui_width],[ui_height],3
    call wp_background
    ; Ciuki's native portrait identifies the system dedicated to him.
    call ui_draw_topbar_visuals
.top_hits:
    mov bx,4
    mov dx,2
    mov cx,125
    mov si,25
    mov ax,13
    call ui_hit_add
    mov bx,[ui_width]
    sub bx,576
    mov di,100
    cmp word [ui_width],800
    jae .top_width_ready
    add bx,120
    mov di,76
.top_width_ready:
    mov ax,80
.top_indicators:
    push ax
    mov dx,3
    mov cx,di
    mov si,23
    call ui_hit_add
    pop ax
    add bx,di
    inc ax
    cmp ax,85
    jb .top_indicators
    ; The desktop module paints the desktop (icons, folders, Recycle Bin);
    ; without it, the shell's own shortcuts.
    cmp byte [ui_scene_covered],1
    je .shortcuts_done
    call app_desktop_paint
    jnc .shortcuts_done
    ; Desktop shortcuts remain usable with the library closed.
    mov bx,14
    mov dx,52
    mov al,0
    mov di,ut_programs
    mov bp,1
    call ui_shortcut
    mov dx,122
    mov al,1
    mov di,ut_files
    mov bp,2
    call ui_shortcut
    ; Removable drives are separate actions with explicit backend selection.
    mov bx,14
    mov dx,332
    cmp word [ui_height],600
    jae .media_places
    mov bx,92
    mov dx,52
.media_places:
    mov al,10
    mov di,ut_floppy
    mov bp,14
    call ui_shortcut
    add dx,70
    mov al,15
    mov di,ut_usb
    mov bp,15
    call ui_shortcut
    add dx,70
    mov al,16
    mov di,ut_cd
    mov bp,16
    call ui_shortcut
    mov bx,14
    mov dx,192
    mov al,4
    mov di,ut_dos
    mov bp,3
    call ui_shortcut
    mov dx,262
    mov al,8
    mov di,ut_display
    mov bp,8
    call ui_shortcut
.shortcuts_done:
    call ui_windows_draw
    call ui_motion_draw
.bar:
    mov dx,[ui_height]
    sub dx,32
    U_RECT 0,dx,[ui_width],32,7
    U_RECT 0,dx,[ui_width],1,15
    add dx,4
    U_BUTTON 5,dx,147,24,ut_library,1
    call ui_windows_taskbar
    mov bx,[ui_width]
    sub bx,183
    mov dx,[ui_height]
    sub dx,29
    mov ax,85
    mov cx,177
    mov si,25
    call ui_hit_add
    U_RECT bx,dx,177,25,8
    inc bx
    inc dx
    U_RECT bx,dx,175,23,15
    add bx,9
    add dx,4
    U_TEXT bx,dx,ui_date_text,0
    mov bx,[ui_width]
    sub bx,70
    mov dx,[ui_height]
    sub dx,25
    U_RECT bx,dx,1,16,7
    add bx,9
    U_TEXT bx,dx,ui_clock_text,1
    call app_overlay_paint    ; the desktop's menus, above everything
.pointer:
    ret

ui_draw_window:
    mov bx,[ui_wx]
    mov dx,[ui_wy]
    mov cx,[ui_ww]
    mov si,[ui_wh]
    mov ax,70
    call ui_hit_add
    mov bx,[ui_wx]
    add bx,4
    mov dx,[ui_wy]
    add dx,5
    U_RECT bx,dx,[ui_ww],[ui_wh],0
    mov bx,[ui_wx]
    mov dx,[ui_wy]
    mov cx,[ui_ww]
    mov si,[ui_wh]
    mov al,7
    call ui_bevel
    add bx,3
    add dx,3
    mov cx,[ui_ww]
    sub cx,6
    mov si,25
    call ui_windows_title_color
    call ui_title_rail
    ; Register title before its buttons, so hit testing gives controls priority.
    mov ax,60
    mov si,25
    call ui_hit_add
    add bx,7
    add dx,4
    mov si,ut_library
    mov cx,[ui_ww]
    sub cx,91
    call ui_windows_title_color
    call ui_title_text
    mov bx,[ui_wx]
    add bx,[ui_ww]
    sub bx,76
    mov dx,[ui_wy]
    add dx,6
    U_BUTTON bx,dx,22,19,ut_min,10
    add bx,24
    U_BUTTON bx,dx,22,19,ut_max,11
    add bx,24
    U_BUTTON bx,dx,22,19,ut_close,12
    mov bx,[ui_wx]
    add bx,12
    mov dx,[ui_wy]
    add dx,38
    U_BUTTON bx,dx,119,27,ut_applications,20
    add bx,120
    U_BUTTON bx,dx,90,27,ut_games,21
    add bx,91
    U_BUTTON bx,dx,90,27,ut_system,22
    ; Active category indicator is a small turquoise inset.
    mov bx,[ui_wx]
    add bx,15
    mov cx,113
    cmp byte [ui_page],0
    je .tab
    add bx,120
    mov cx,84
    cmp byte [ui_page],1
    je .tab
    add bx,91
.tab:
    add dx,23
    U_RECT bx,dx,cx,3,10
    mov bx,[ui_wx]
    add bx,10
    mov dx,[ui_wy]
    add dx,72
    mov cx,[ui_ww]
    sub cx,20
    mov si,[ui_wh]
    sub si,112
    call ui_inset
    call ui_library_detail
    mov byte [ui_draw_item],0
.items:
    mov al,[ui_draw_item]
    cmp al,6
    jae .footer
    cmp byte [ui_page],1
    jne .item
    cmp al,1
    jae .footer
.item:
    xor ah,ah
    mov bl,3
    div bl
    movzx dx,al
    imul dx,110
    add dx,[ui_wy]
    add dx,105
    movzx bx,ah
    imul bx,148
    mov ax,[ui_ww]
    sub ax,444
    add ax,[ui_sidebar_width]
    shr ax,1
    add bx,ax
    add bx,[ui_wx]
    mov [ui_tile_x],bx
    mov [ui_tile_y],dx
    mov al,[ui_draw_item]
    cmp al,[ui_selection]
    jne .plain
    U_RECT bx,dx,137,99,7
    U_RECT bx,dx,3,99,10
.plain:
    movzx ax,byte [ui_draw_item]
    add ax,30
    mov cx,137
    mov si,99
    call ui_hit_add
    mov al,[ui_draw_item]
    call ui_record
    mov di,si
    mov al,[di+8]
    mov bx,[ui_tile_x]
    add bx,47
    mov dx,[ui_tile_y]
    add dx,8
    call ui_icon
    mov si,[di]
    call ui_measure
    mov bx,137
    sub bx,cx
    shr bx,1
    add bx,[ui_tile_x]
    mov dx,[ui_tile_y]
    add dx,60
    mov ax,0
    call ui_text
    call ui_item_detail
    inc byte [ui_draw_item]
    jmp .items
.footer:
    mov bx,[ui_wx]
    add bx,12
    mov dx,[ui_wy]
    add dx,[ui_wh]
    sub dx,31
    call ui_selected_record
    mov si,[si+2]
    mov ax,0
    call ui_text
    mov bx,[ui_wx]
    add bx,[ui_ww]
    sub bx,94
    sub dx,1
    U_BUTTON bx,dx,81,23,ut_open,44
    mov bx,[ui_wx]
    add bx,[ui_ww]
    mov dx,[ui_wy]
    add dx,[ui_wh]
    mov ax,61
; BX/DX = window right/bottom edge, AX = hit action: resize grip + 12x12 hit.
ui_grip:
    sub bx,12
    sub dx,12
    push ax
    U_RECT bx,dx,2,9,8
    add bx,4
    add dx,4
    U_RECT bx,dx,2,5,8
    sub bx,4
    sub dx,4
    mov cx,12
    mov si,12
    pop ax
    jmp ui_hit_add

ui_shortcut:
    pushad
    push bx
    push dx
    add bx,17
    call ui_icon
    pop dx
    pop bx
    mov ax,bp
    mov cx,79
    mov si,76
    call ui_hit_add
    add dx,48
    mov si,di
    call ui_measure
    mov ax,79
    sub ax,cx
    shr ax,1
    add bx,ax
    mov ax,15
    call ui_text
    popad
    ret

; Official native sprite pack; retain built-in geometry for recovery media.
ui_icon:
    cmp al,UI_ICONS_COUNT
    jae .fallback
    cmp word [ui_icons_seg],0
    jne ui_icons_draw
.fallback:
    pushad
    cmp al,15
    je .usb
    cmp al,16
    je .cd
    cmp al,1
    je .folder
    cmp al,2
    je .document
    cmp al,6
    je .game
    cmp al,7
    je .game
    cmp al,9
    je .sound
    cmp al,10
    je .disk
    cmp al,17
    je .disk
    cmp al,11
    je .info
    cmp al,12
    je .power
    cmp al,5
    je ui_icon_run
    cmp al,8
    je ui_icon_display
    cmp al,13
    je ui_icon_windows
    cmp al,14
    je ui_icon_costa
    mov [ui_icon_kind],al
    U_RECT bx,dx,43,31,0
    inc bx
    inc dx
    U_RECT bx,dx,40,28,7
    add bx,4
    add dx,3
    U_RECT bx,dx,32,20,1
    cmp byte [ui_icon_kind],4
    je .dos
    add bx,8
    add dx,2
    call ciuki_logo16
    jmp .stand
.dos:
    add bx,3
    add dx,2
    U_TEXT bx,dx,ut_prompt,15
.stand:
    popad
    pushad
    add bx,17
    add dx,31
    U_RECT bx,dx,9,5,0
    sub bx,9
    add dx,5
    U_RECT bx,dx,27,3,7
    jmp .done
.folder:
    add dx,5
    U_RECT bx,dx,19,7,6
    add dx,5
    U_RECT bx,dx,44,29,6
    inc bx
    inc dx
    U_RECT bx,dx,41,26,14
    add dx,4
    U_RECT bx,dx,41,2,15
    jmp .done
.document:
    add bx,8
    U_RECT bx,dx,29,41,0
    inc bx
    inc dx
    U_RECT bx,dx,26,38,15
    add bx,5
    add dx,8
    U_RECT bx,dx,15,2,1
    add dx,6
    U_RECT bx,dx,15,2,8
    add dx,6
    U_RECT bx,dx,11,2,8
    add dx,6
    U_RECT bx,dx,15,2,8
    jmp .done
.game:
    add dx,5
    U_RECT bx,dx,44,30,1
    add bx,5
    add dx,10
    U_RECT bx,dx,15,5,7
    add bx,5
    sub dx,5
    U_RECT bx,dx,5,15,7
    add bx,17
    add dx,1
    U_RECT bx,dx,6,6,12
    add bx,6
    add dx,9
    U_RECT bx,dx,6,6,10
    jmp .done
.sound:
    add dx,14
    U_RECT bx,dx,11,15,1
    add bx,11
    sub dx,7
    U_RECT bx,dx,10,29,7
    add bx,13
    add dx,5
    U_RECT bx,dx,3,19,10
    add bx,7
    sub dx,5
    U_RECT bx,dx,3,29,10
    jmp .done
.disk:
    add bx,5
    U_RECT bx,dx,34,40,1
    add bx,5
    U_RECT bx,dx,24,14,7
    add dx,22
    U_RECT bx,dx,24,16,15
    add bx,4
    add dx,4
    U_RECT bx,dx,16,2,8
    jmp .done
.usb:
    add bx,13
    U_RECT bx,dx,17,12,7
    add bx,3
    add dx,2
    U_RECT bx,dx,3,5,0
    add bx,7
    U_RECT bx,dx,3,5,0
    sub bx,14
    add dx,10
    U_RECT bx,dx,25,27,1
    add bx,4
    add dx,5
    U_RECT bx,dx,17,13,10
    add bx,6
    add dx,4
    U_RECT bx,dx,5,5,15
    jmp .done
.cd:
    add bx,9
    U_RECT bx,dx,26,3,15
    sub bx,6
    add dx,3
    U_RECT bx,dx,38,6,7
    sub bx,3
    add dx,6
    U_RECT bx,dx,44,23,15
    add bx,3
    add dx,23
    U_RECT bx,dx,38,6,7
    add bx,6
    add dx,6
    U_RECT bx,dx,26,3,15
    add bx,8
    sub dx,24
    U_RECT bx,dx,10,10,8
    add bx,2
    add dx,2
    U_RECT bx,dx,6,6,3
    jmp .done
.info:
    add bx,6
    U_RECT bx,dx,32,39,1
    add bx,13
    add dx,6
    U_RECT bx,dx,6,6,15
    add dx,12
    U_RECT bx,dx,6,14,15
    jmp .done
.power:
    add bx,3
    add dx,3
    U_RECT bx,dx,37,35,1
    add bx,7
    add dx,7
    U_RECT bx,dx,23,21,10
    add bx,5
    sub dx,1
    U_RECT bx,dx,13,15,1
    add bx,5
    sub dx,4
    U_RECT bx,dx,3,13,15
.done:
    popad
    ret

ui_button:
    pushad
    mov [ui_button_id],ax
    call ui_hit_add
    cmp byte [ui_button_down],1
    jne .raised
    cmp ax,[ui_pressed]
    jne .raised
    mov al,[ui_hit_owner]
    cmp al,[ui_press_owner]
    jne .raised
    cmp bx,[ui_press_x]
    jne .raised
    cmp dx,[ui_press_y]
    jne .raised
    call ui_inset
    inc bx
    inc dx
    jmp .label
.raised:
    mov al,7
    call ui_bevel
    mov al,[ui_active_window]
    inc al
    cmp al,[ui_hit_owner]
    jne .label
    cmp word [ui_button_id],0
    je .label
    mov ax,[ui_button_id]
    cmp ax,[ui_focus]
    jne .label
    push dx
    push si
    add dx,si
    sub dx,3
    add bx,3
    sub cx,6
    mov si,2
    mov al,1
    call ui_rect
    sub bx,3
    add cx,6
    pop si
    pop dx
.label:
    cmp byte [di],5                    ; 1-4: a drawn caption glyph
    jb .glyph
    push cx
    push si
    mov si,di
    call ui_measure
    mov bp,cx
    pop si
    pop cx
    mov ax,cx
    sub ax,bp
    shr ax,1
    add bx,ax
    mov ax,si
    sub ax,16
    sar ax,1
    add dx,ax
    mov si,di
    xor ax,ax
    call ui_text
    popad
    ret
.glyph:
    ; Centre of the button face, then the glyph in ink, drawn in pixels
    ; like the theme's bevels (no font characters).
    shr cx,1
    add bx,cx
    shr si,1
    add dx,si
    mov al,0
    mov ah,[di]
    cmp ah,1
    jne .maximize
    sub bx,4                           ; minimize: a bar at the bottom
    add dx,2
    mov cx,8
    mov si,2
    call ui_rect
    jmp .glyph_done
.maximize:
    cmp ah,4
    je .close
    cmp ah,3
    jne .frame
    ; restore: a back frame up and right, then the front frame
    push bx
    push dx
    sub bx,2
    sub dx,5
    mov cx,7
    call ui_glyph_frame
    pop dx
    pop bx
    sub bx,5
    sub dx,2
    mov cx,7
    mov si,6
    mov al,7                           ; the front window hides the back one
    call ui_rect
    mov al,0
    call ui_glyph_frame
    jmp .glyph_done
.frame:
    sub bx,5                           ; maximize: a window frame
    sub dx,5
    mov cx,10
    call ui_glyph_frame
    jmp .glyph_done
.close:
    sub bx,4                           ; close: a two-pixel X
    sub dx,4
    mov bp,0
.cross:
    mov cx,2
    mov si,1
    push bx
    add bx,bp
    call ui_rect
    pop bx
    push bx
    add bx,7
    sub bx,bp
    call ui_rect
    pop bx
    inc dx
    inc bp
    cmp bp,8
    jb .cross
.glyph_done:
    popad
    ret

; BX, DX top left, CX size: a small window frame with a thick top (AL ink).
ui_glyph_frame:
    push si
    mov si,2
    call ui_rect                       ; title bar
    push cx
    push dx
    mov si,cx
    dec si
    mov cx,1
    call ui_rect                       ; left
    pop dx
    pop cx
    push bx
    add bx,cx
    dec bx
    push cx
    mov si,cx
    dec si
    mov cx,1
    call ui_rect                       ; right
    pop cx
    pop bx
    push dx
    add dx,cx
    sub dx,2
    mov si,1
    call ui_rect                       ; bottom
    pop dx
    pop si
    ret

ui_measure:
    push ax
    push si
    push bx
    push fs
    mov ax,[ui_assets_seg]
    mov fs,ax
    xor cx,cx
.next:
    lodsb
    test al,al
    jz .done
    cmp al,32
    jb .next
    cmp al,126
    ja .next
    sub al,32
    movzx bx,al
    movzx ax,byte [fs:UI_ASSET_FONT+bx]
    add cx,ax
    jmp .next
.done:
    pop fs
    pop bx
    pop si
    pop ax
    ret

ui_hit_add:
    pushad
    push fs
    mov fs,[ui_assets_seg]
    cmp word [ui_hit_count],256
    jae .done
    mov di,[ui_hit_count]
    imul di,10
    add di,ui_hits
    mov [fs:di],bx
    mov [fs:di+2],dx
    add cx,bx
    add si,dx
    mov [fs:di+4],cx
    mov [fs:di+6],si
    mov ah,[ui_hit_owner]
    mov [fs:di+8],ax
    inc word [ui_hit_count]
.done:
    pop fs
    popad
    ret

ui_hit_find:
    push fs
    mov fs,[ui_assets_seg]
    push bx
    push cx
    push si
    mov cx,[ui_hit_count]
    mov si,cx
    imul si,10
    add si,ui_hits
.next:
    jcxz .none
    sub si,10
    dec cx
    mov bx,[ui_mouse_x]
    cmp bx,[fs:si]
    jb .next
    cmp bx,[fs:si+4]
    jae .next
    mov bx,[ui_mouse_y]
    cmp bx,[fs:si+2]
    jb .next
    cmp bx,[fs:si+6]
    jae .next
    mov ax,[fs:si+8]
    jmp .done
.none:
    xor ax,ax
.done:
    mov [ui_last_hit_ptr],si
    call ui_windows_hit
    cmp byte [app_overlay],0
    jne .picked
    push ax
    call ui_window_pick_at_pointer
    test al,al
    jz .keep_hit
    cmp al,[ui_hit_target]
    je .same_hit
    mov [ui_hit_target],al
    pop ax
    mov ax,70                  ; exposed window body: raise on first click
    jmp .check_grip
.same_hit:
    pop ax
.check_grip:
    ; The last compositor band may omit a lower window's resize grip from
    ; the hit list. Recover its control from current window geometry.
    movzx si,byte [ui_hit_target]
    dec si
    cmp byte [app_win_slot+si],0xFF
    je .picked
    test byte [app_win_style+si],1
    jnz .picked
    shl si,1
    mov bx,[ui_window_x+si]
    add bx,[ui_window_width+si]
    sub bx,14
    cmp [ui_mouse_x],bx
    jb .picked
    add bx,12
    cmp [ui_mouse_x],bx
    jae .picked
    mov bx,[ui_window_y+si]
    add bx,[ui_window_height+si]
    sub bx,14
    cmp [ui_mouse_y],bx
    jb .picked
    add bx,12
    cmp [ui_mouse_y],bx
    jae .picked
    mov ax,65                 ; module resize grip
    jmp .picked
.keep_hit:
    pop ax
.picked:
    pop si
    pop cx
    pop bx
    pop fs
    ret

; Item records: name, description, command, special action, icon, pad (10B).
ui_selected_record:
    mov al,[ui_selection]
ui_record:
    push bx
    movzx bx,byte [ui_page]
    imul bx,60
    movzx si,al
    imul si,10
    add si,bx
    add si,ui_items
    pop bx
    ret

%include "src/com/shell_gui_input.inc"
%include "src/com/shell_gui_compositor.inc"
%include "src/com/shell_gui_windows.inc"
%include "src/com/shell_gui_dialogs.inc"
%include "src/com/shell_apps.inc"
%include "src/com/shell_wallpaper.inc"
%include "src/com/shell_vm_window.inc"
%include "src/com/shell_gui_detail.inc"
%include "src/com/shell_system_sounds.inc"
%define CIUKI_LOGO_RECT ui_rect
%define CIUKI_LOGO_VBE ui_vbe
%define CIUKI_LOGO_COMPOSING ui_composing
%define CIUKI_LOGO_BAND_TOP ui_comp_top
%define CIUKI_LOGO_BAND_BOTTOM ui_comp_bottom
%define CIUKI_LOGO_ORIGIN_Y 0
%define CIUKI_LOGO_EXTERNAL_SEG ui_assets_seg
%include "src/com/ciuki_logo.inc"
%include "src/com/ui_icons.inc"

ui_items:
    dw ut_dosbox,ud_prompt,0,3
    db 4,0
    dw ut_files,ud_files,0,2
    db 1,0
    dw ut_browser,ud_browser,0,63
    db 35,0
    dw ut_control,ud_control,0,61
    db 20,0
    dw ut_notepad,ud_notepad,0,29
    db 2,0
    dw ut_paint,ud_paint,0,28
    db 34,0
    dw ut_installed,ud_installed,0,62
    db 1,0
    times 50 db 0
    dw ut_display,ud_display,0,8
    db 8,0
    dw ut_sound,ud_sound,0,9
    db 9,0
    dw ut_install,ud_install,uc_install,0
    db 17,0
    dw ut_tasks,ud_tasks,0,23
    db 11,0
    dw ut_wallpaper,ud_wallpaper,0,24
    db 8,0
    dw ut_dos,ud_dos,0,3
    db 4,0

ut_brand db 'CiukiOS',0
ut_tagline db 'A modern Retro OS',0
ui_arg_sound db 'sound',0
ui_arg_network db 'network',0
ui_arg_datetime db 'datetime',0
ui_arg_performance db 'performance',0
ut_programs db 'Programs',0
ut_library db 'Application Library',0
ut_applications db 'Applications',0
ut_games db 'Installed',0
ut_system db 'System',0
ut_open db 'Open',0
ut_dos db 'DOS',0
ut_run db 'Run...',0
ut_files db 'Files',0
ut_tasks db 'Task Manager',0
ut_notepad db 'CiukNote',0
ut_paint db 'CiukPaint',0
ut_browser db 'CiukWeb',0
ut_control db 'Control Panel',0
ut_devices db 'Devices',0
ut_recycle db 'Recycle Bin',0
ut_modwin db 'Window',0
ut_wallpaper db 'Wallpaper',0
ud_tasks db 'Windows, programs and memory',0
ud_wallpaper db 'Choose a desktop background',0
ut_editor db 'DOS Editor',0
ut_costa db 'Costa',0
ut_display db 'Display',0
ut_sound db 'Sound',0
ut_install db 'Install CiukiOS',0
ut_about db 'About',0
ut_power db 'Power',0
ut_dosbox db 'DOS Prompt',0
ut_doom db 'Doom',0
ut_doom_window db 'Windowed Doom',0
ut_wolf_window db 'Windowed Wolf3D',0
ud_game_window db 'Source port; audio not available yet.',0
ut_doomvan db 'Doom Vanille',0
ut_wolf db 'Wolfenstein 3D',0
ut_min db 1,0                  ; drawn glyphs (ui_button)
ut_max db 2,0
ut_close db 4,0
ut_prompt db '>_',0
ud_prompt db 'DOS programs in a window',0
ud_files db 'Browse folders, copy, move and rename files',0
ud_editor db 'Edit text files full screen (EDIT)',0
ud_notepad db 'Write and edit text documents',0
ud_paint db 'Draw and edit pictures',0
ud_browser db 'Browse simple HTTP pages',0
ud_control db 'Change system settings and inspect devices',0
ut_installed db 'Installed Programs',0
ud_installed db 'Applications installed in C:\PROGRAMS',0
ui_installed_dir db 'C:\PROGRAMS',0
ud_costa db 'Open the Costa desktop',0
ud_dos db 'Full-screen DOS. Type EXIT to return here.',0
ud_run db 'Run a program or a shell command',0
ud_doom db 'Original Doom',0
ud_doomvan db 'Doom with the Vanille engine',0
ud_wolf db 'Wolfenstein 3D',0
ud_display db 'Set the desktop resolution',0
ud_sound db 'Play the startup sound',0
ud_install db 'Install CiukiOS or prepare a hard disk',0
ud_about db 'Version, controls and system information',0
ud_power db 'Restart or shut down the computer',0
ui_cmd_files db 'DOSNAV',0
ut_floppy db 'Floppy',0
ut_usb db 'USB drive',0
ut_cd db 'CD-ROM',0
uc_editor db 'EDIT',0
uc_costa db 'COSTA',0
ui_window_commands dw uc_doom,uw_doom,uc_doomvan,uw_doomvan,uc_wolf,uw_wolf,0
uw_doom db '\APPS\DOOM\DOOMCORE.EXE',0
uw_doomvan db '\APPS\DOOMVAN\PCDMCORE.EXE',0
uw_wolf db '\APPS\WOLF3D\WOLF3D.EXE',0
uc_doom db 'DOOM',0
uc_doomvan db 'DOOMVAN',0
uc_wolf db 'WOLF3D',0
uc_display db 'VGASETUP',0
uc_install db 'SETUP',0
ui_cmd_restart db 'REBOOT',0
ui_cmd_shutdown db 'SHUTDOWN',0
ui_paint_marker db '[DESKTOP] PAINT',13,10,0
ui_ready_marker db '[DESKTOP] READY',13,10,0
ui_dos_marker db '[DESKTOP] DOS',13,10,0
ui_launch_marker db '[DESKTOP] RUN ',0
ui_newline db 13,10,0
ui_return_hint db 13,10,'Press any key to return to the desktop.',13,10,'$'
ui_dos_hint db 'Type EXIT or DESKTOP to return to the desktop.',13,10,13,10,'$'
ui_active db 0
ui_ready_pending db 0
ui_sound_played db 0
ui_vbe db 0
ui_request db 0
ui_command_running db 0
ui_exec_error db 0
ui_settings_pending db 0
ui_child_exit db 0
ui_command_length dw 0
ui_width dw 640
ui_height dw 480
ui_wx dw 100
ui_wy dw 62
ui_ww dw 510
ui_wh dw 340
ui_saved_wx dw 100
ui_saved_wy dw 62
ui_saved_ww dw 510
ui_saved_wh dw 340
ui_previous_width dw 640
ui_previous_height dw 480
ui_layout_ready db 0
ui_mode_request dw 0
ui_welcome_done db 0
ui_welcome_value db '1',0
ui_welcome_path db '\SYSTEM\UI\WELCOME.CFG',0
ui_window_visible db 0
ui_maximized db 0
ui_page db 0
ui_selection db 0
ui_draw_item db 0
ui_tile_x dw 0
ui_tile_y dw 0
ui_icon_kind db 0
ui_button_id dw 0
ui_focus dw 44
ui_last_item dw 0
ui_last_click dw 0
ui_hit_count dw 0
; Hit records are in the desktop resource block.
ui_attributes db 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0
; DAC channels 0..63. Colors 0,1,3,7,10,15 define the desktop theme.
ui_palette:
    CIUKIOS_PALETTE
    CIUKI_LOGO_PALETTE
    UI_ICONS_PALETTE

ui_boot_drive db 2
