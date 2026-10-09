; Nonblocking keyboard/mouse events. The renderer alone owns cursor pixels.
ui_event:
    xor ax,ax
    xchg al,[app_hotkey]    ; Win/Menu/Ctrl+Alt+Del from the INT 15h intercept
    test al,al
    jnz app_hotkey_action
    mov ah,0x11
    int 0x16
    jnz .enhanced_key
    ; Some legacy notebook BIOSes only expose the original INT 16h queue.
    ; Graphics/safe-mode choice does not determine which BIOS queue exists.
    ; Try the original nonblocking pair whenever the enhanced queue is empty.
    mov ah,0x01
    int 0x16
    jz .mouse
    mov ah,0x00
    int 0x16
%ifdef BOOT_DISK_LOG
    cmp byte [boot_log_legacy_seen],1
    je .key_received
    mov byte [boot_log_legacy_seen],1
    push ax
    mov al,0x1B
    call boot_log_stage
    pop ax
%endif
    jmp .key_received
.enhanced_key:
    mov ah,0x10
    int 0x16
.key_received:
    cmp al,27
    jne .key_resize_ready
    cmp byte [ui_dragging],2
    je ui_resize_cancel
    cmp byte [ui_dragging],4
    je ui_resize_cancel
.key_resize_ready:
%ifdef BOOT_DISK_LOG
    cmp byte [boot_log_key_seen],1
    je .key_logged
    mov byte [boot_log_key_seen],1
    push ax
    mov al,0x11                ; first keyboard event read
    call boot_log_stage
    pop ax
.key_logged:
%endif
    cmp ah,0xA5             ; enhanced BIOS Alt+Tab, retained after Alt release
    je ui_windows_cycle
    cmp ah,0x6B
    je .alt_close
    cmp al,27
    jne .app_key
    push ax
    mov ah,2
    int 0x16
    test al,4
    pop ax
    jnz .escape             ; Ctrl+Esc and Ctrl+Shift+Esc stay global
.app_key:
    call app_key            ; the active application first
    jc .out
    cmp ah,0x94             ; enhanced BIOS Ctrl+Tab
    je .cycle_category
    cmp al,27
    je .escape
    cmp ah,0x3B
    je .about
    cmp ah,0x3C
    je .library
    cmp ah,0x3D
    je .run
    cmp ah,0x3E
    je .dos
    cmp ah,0x6B
    je .alt_close
    cmp ah,0x0F
    je .tab
    cmp al,9
    je .tab
    cmp al,9
    je .tab
    cmp al,13
    je .enter
    cmp byte [ui_dialog],0
    jne .idle
    cmp ah,0x3B
    je .about
    cmp ah,0x3C
    je .library
    cmp ah,0x3D
    je .run
    cmp ah,0x3E
    je .dos
    cmp ah,0x44
    je .power
    cmp ah,0x4B
    je .left
    cmp ah,0x4D
    je .right
    cmp ah,0x48
    je .up
    cmp ah,0x50
    je .down
    jmp .idle
.escape:
    mov ah,2
    int 0x16
    test al,4
    jz .escape_window
    test al,3
    jz .start_menu
    mov ax,23                 ; Ctrl+Shift+Esc opens native Tasks
    ret
.start_menu:
    mov ax,1                  ; Ctrl+Esc: Programs, as the Start menu
    ret
.escape_window:
    cmp byte [ui_dialog],0
    jne .close
    mov ax,10
    ret
.close:
    mov ax,41
    ret
.alt_close:
    cmp byte [ui_dialog],0
    jne .close
    cmp byte [ui_window_visible],1
    jne .power                ; Alt+F4 on the bare desktop: shut down
    mov ax,12
    ret
.about:
    mov ax,6
    ret
.library:
    mov ax,1
    ret
.run:
    mov ax,4
    ret
.dos:
    mov ah,2
    int 0x16
    test al,8
    jz .dos_open
    cmp byte [ui_dialog],0
    jne .close
    mov ax,12
    ret
.dos_open:
    mov ax,25
    ret
.power:
    mov ax,7
    ret
.left:
    mov al,[ui_selection]
    test al,al
    jz .idle
    dec al
    jmp .select
.right:
    mov al,[ui_selection]
    inc al
    jmp .select
.up:
    mov al,[ui_selection]
    cmp al,3
    jb .idle
    sub al,3
    jmp .select
.down:
    mov al,[ui_selection]
    add al,3
.select:
    mov ah,6
    cmp byte [ui_page],1
    jne .bound
    mov ah,1
.bound:
    cmp al,ah
    jae .idle
    mov [ui_selection],al
    mov word [ui_focus],44
    mov word [ui_last_item],0
    mov ax,250
    ret
.tab:
    ; BIOS encodes Shift+Tab in AX even if Shift is released while a paint
    ; delays this event. Preserve that direction before reading live flags.
    cmp ax,0x0F00
    je ui_focus_previous
    ; Ctrl+Tab cycles the three library categories.
    mov ah,2
    int 0x16
    test al,8
    jnz ui_windows_cycle
    test al,4
    jnz .cycle_category
    test al,3
    jnz ui_focus_previous
    jmp ui_focus_next
.cycle_category:
    cmp byte [ui_dialog],0
    jne ui_focus_next
    mov al,[ui_page]
    inc al
    cmp al,3
    jb .category
    xor al,al
.category:
    add al,20
    xor ah,ah
    ret
.enter:
    mov ax,[ui_focus]
    cmp ax,30
    jb .out
    cmp ax,36
    jae .out
    sub al,30
    mov [ui_selection],al
    mov ax,44
.out:
    ret
.mouse:
    cmp byte [ui_mouse_ready],1
    jne .idle
    mov ax,3
    int 0x33
    movsx ax,bh
    add [ui_wheel_pending],ax
    test ax,ax
    jz .wheel_sampled
    inc word [ui_wheel_samples]
    mov [ui_wheel_last],ax
.wheel_sampled:
    and bx,7
    push bx
    mov ax,0x000B
    int 0x33
    pop bx
    ; INT 33h keeps one-shot press/release counts. Preserve both edges when
    ; a short click happens during a compositor repaint between UI polls.
    push bx
    push cx
    push dx
    mov ax,5
    xor bx,bx
    int 0x33
    test bx,bx
    jz .no_press
    mov byte [ui_mouse_press_pending],1
.no_press:
    mov ax,6
    xor bx,bx
    int 0x33
    test bx,bx
    jz .no_release
    mov byte [ui_mouse_release_pending],1
.no_release:
    pop dx
    pop cx
    pop bx
    cmp byte [ui_mouse_press_pending],0
    je .pending_release
    mov byte [ui_mouse_press_pending],0
    or bx,1
    jmp .swap_buttons
.pending_release:
    cmp byte [ui_mouse_release_pending],0
    je .swap_buttons
    mov byte [ui_mouse_release_pending],0
    and bx,0FFFEh
.swap_buttons:
    cmp byte [app_set_mouse+1],0      ; buttons swapped (Control Panel)
    je .buttons
    mov ax,bx
    and bx,0xFFFC
    shr al,1
    rcl bl,1
    and al,1
    shl al,1
    or bl,al
.buttons:
    mov [ui_mouse_buttons],bx
    ; Own signed motion coordinates. The shared driver's absolute position
    ; can wrap at zero; consuming raw mickeys keeps this GUI independent.
    ; Speed 2 (the default) doubles the mickeys.
    movsx ecx,cx
    movsx edx,dx
    movzx eax,byte [app_set_mouse]
    imul ecx,eax
    imul edx,eax
    movzx eax,word [ui_mouse_x]
    add ecx,eax
    movzx eax,word [ui_mouse_y]
    add edx,eax
    test ecx,ecx
    jns .raw_x_min
    xor ecx,ecx
.raw_x_min:
    test edx,edx
    jns .raw_y_min
    xor edx,edx
.raw_y_min:
    mov ax,[ui_width]
    movzx eax,ax
    dec eax
    cmp ecx,eax
    jbe .raw_x_max
    mov ecx,eax
.raw_x_max:
    mov ax,[ui_height]
    movzx eax,ax
    dec eax
    cmp edx,eax
    jbe .raw_y_max
    mov edx,eax
.raw_y_max:
    cmp cx,[ui_mouse_x]
    jne .move
    cmp dx,[ui_mouse_y]
    je .click
.move:
%ifdef BOOT_DISK_LOG
    cmp byte [boot_log_mouse_seen],1
    je .mouse_logged
    mov byte [boot_log_mouse_seen],1
    push ax
    mov al,0x12                ; first mouse motion read
    call boot_log_stage
    pop ax
.mouse_logged:
%endif
    test cx,cx
    jns .positive_x
    xor cx,cx
.positive_x:
    test dx,dx
    jns .positive_y
    xor dx,dx
.positive_y:
    mov ax,[ui_width]
    dec ax
    cmp cx,ax
    jbe .x
    mov cx,ax
.x:
    mov ax,[ui_height]
    dec ax
    cmp dx,ax
    jbe .y
    mov dx,ax
.y:
    ; Keep the driver position synchronized: no hidden travel beyond a border.
    push bx
    mov ax,4
    int 0x33
    pop bx
    call ui_pointer_hide
    mov [ui_mouse_x],cx
    mov [ui_mouse_y],dx
    call ui_pointer_show
    cmp byte [app_overlay],0
    jne .overlay_move
    cmp byte [ui_dragging],0
    jne .drag
    cmp byte [app_capture],0
    je .hover
    test byte [ui_mouse_buttons],1
    jz .click
    mov bl,[app_capture]
    dec bl
    mov dx,2
    call app_mouse
    test ax,ax
    jnz .out
.hover:
    ; No button: the window, desktop or menu under the pointer may highlight.
    test byte [ui_mouse_buttons],3
    jnz .click
    call app_hover
    call ui_resize_hover
    test ax,ax
    jnz .out
.click:
    cmp word [ui_wheel_pending],0
    je .no_wheel
    xor dx,dx
    xchg dx,[ui_wheel_pending]
    call app_wheel
    test ax,ax
    jnz .out
.no_wheel:
    cmp byte [app_overlay],0
    jne .overlay_buttons
    ; Right button, according to the context: an application window's
    ; contents -> that application; a title bar or a shell window -> the
    ; window menu; the desktop, the taskbar, the top bar -> the desktop's
    ; menus (the desktop module).
    mov ax,[ui_mouse_buttons]
    and al,2
    cmp al,[app_previous_right]
    je .left_button
    mov [app_previous_right],al
    test al,al
    jz .left_button
    mov ax,[ui_width]
    sub ax,183
    cmp [ui_mouse_x],ax
    jb .right_find
    mov ax,[ui_height]
    sub ax,32
    cmp [ui_mouse_y],ax
    jae .right_datetime
.right_find:
    call ui_hit_find
    movzx bx,byte [ui_hit_target]
    test bx,bx
    jz .right_desktop
    dec bx
    cmp bl,[ui_active_window]
    jne .right_activate
    cmp byte [app_desktop_focus],0
    je .right_raised
.right_activate:
    push ax
    call ui_windows_raise
    pop ax
.right_raised:
    cmp ax,62
    je .right_window_menu
    cmp ax,60
    je .right_window_menu
    push si
    call app_slot
    pop si
    jc .right_window_menu
    mov dx,4
    call app_mouse
    test ax,ax
    jnz .out
    mov ax,250
    ret
.right_datetime:
    mov ax,85
    ret
.right_window_menu:
    mov ax,bx
    or ah,1                             ; context 1xxh: window xx
    jmp .right_context
.right_desktop:
    mov byte [app_desktop_focus],1      ; the desktop takes the keyboard
    call ui_damage_active
.right_context:
    mov [app_ev_context],ax
    mov bl,APP_WIN_DESKTOP
    mov dx,4
    call app_mouse
    mov word [app_ev_context],0
    test ax,ax
    jnz .out
    mov ax,250
    ret
.overlay_move:
    mov bl,APP_WIN_OVERLAY
    mov dx,6
    call app_mouse
    mov [app_overlay_dirty],ax
.overlay_buttons:
    ; The desktop's open menu takes every button (a click outside closes it).
    mov al,[ui_mouse_buttons]
    and al,3
    mov ah,[app_overlay_buttons]
    mov [app_overlay_buttons],al
    mov cl,al
    and cl,1
    mov [ui_previous_button],cl
    mov cl,al
    and cl,2
    mov [app_previous_right],cl
    xor ah,al
    jz .overlay_quiet
    mov dx,1
    test ah,1
    jz .overlay_right
    test al,1
    jnz .overlay_send
    mov dx,3
    jmp .overlay_send
.overlay_right:
    test al,2
    jz .overlay_quiet
    mov dx,4
.overlay_send:
    mov bl,APP_WIN_OVERLAY
    call app_mouse
    test ax,ax
    jnz .out
    mov ax,250
    ret
.overlay_quiet:
    xor ax,ax
    xchg ax,[app_overlay_dirty]
    test ax,ax
    jnz .out
    jmp .idle
.left_button:
    mov ax,[ui_mouse_buttons]
    and al,1
    cmp al,[ui_previous_button]
    je .idle
    mov [ui_previous_button],al
    test al,al
    jz .release
    call ui_hit_find
    mov [ui_pressed],ax
    mov byte [ui_button_down],0
    test ax,ax
    jz .pressed_ready
    cmp ax,60
    jb .press_control
    cmp ax,80
    jb .pressed_ready
.press_control:
    mov byte [ui_button_down],1
    mov bl,[ui_hit_target]
    mov [ui_press_owner],bl
    push si
    push fs
    mov fs,[ui_assets_seg]
    mov si,[ui_last_hit_ptr]
    mov bx,[fs:si]
    mov [ui_press_x],bx
    mov bx,[fs:si+2]
    mov [ui_press_y],bx
    mov bx,[fs:si+4]
    sub bx,[fs:si]
    mov [ui_press_width],bx
    mov bx,[fs:si+6]
    sub bx,[fs:si+2]
    mov [ui_press_height],bx
    pop fs
    pop si
    call ui_damage_pressed
.pressed_ready:
    push ax
    movzx bx,byte [ui_hit_target]
    test bx,bx
    jz .focus_ready
    dec bx
    cmp bl,[ui_active_window]
    jne .activate_window
    cmp byte [app_desktop_focus],0
    je .focus_ready
.activate_window:
    call ui_windows_raise
    mov byte [ui_window_repaint],1
.focus_ready:
    pop ax
    test ax,ax
    jnz .not_desktop
    cmp byte [ui_hit_target],0
    je .desktop_press
.not_desktop:
    cmp ax,70
    je .app_press
    cmp ax,65
    je .app_resize_start
    cmp ax,62
    je .window_drag_start
    cmp ax,61
    je .resize_start
    cmp ax,60
    jne .idle
    cmp byte [ui_maximized],1
    je .idle
    mov byte [ui_dragging],1
    mov ax,[ui_mouse_x]
    sub ax,[ui_wx]
    mov [ui_drag_dx],ax
    mov ax,[ui_mouse_y]
    sub ax,[ui_wy]
    mov [ui_drag_dy],ax
    jmp .idle
 .window_drag_start:
    mov byte [ui_dragging],3
    mov al,[ui_active_window]
    mov [ui_window_drag],al
    movzx si,al
    shl si,1
    mov ax,[ui_mouse_x]
    sub ax,[ui_window_x+si]
    mov [ui_drag_dx],ax
    mov ax,[ui_mouse_y]
    sub ax,[ui_window_y+si]
    mov [ui_drag_dy],ax
    jmp .idle
 .resize_start:
    cmp byte [ui_maximized],1
    je .idle
    mov byte [ui_dragging],2
    call ui_resize_begin
    jmp .idle
.release:
    cmp byte [app_capture],0
    je .release_controls
    mov bl,[app_capture]
    dec bl
    mov byte [app_capture],0
    mov dx,3
    call app_mouse
    test ax,ax
    jnz .out
    jmp .idle
.release_controls:
    mov bl,[ui_button_down]
    call ui_damage_pressed
    mov byte [ui_button_down],0
    cmp byte [ui_dragging],0
    jne .drag_end
    cmp bl,1
    jne .idle
    call ui_hit_find
    cmp ax,[ui_pressed]
    jne .idle
    mov bl,[ui_press_owner]
    cmp bl,[ui_hit_target]
    jne .idle
    mov bx,[ui_mouse_x]
    sub bx,[ui_press_x]
    cmp bx,[ui_press_width]
    jae .idle
    mov bx,[ui_mouse_y]
    sub bx,[ui_press_y]
    cmp bx,[ui_press_height]
    jb .out
    jmp .idle
.drag_end:
    mov byte [ui_dragging],0
    mov byte [ui_button_down],0
    mov byte [ui_previous_button],0
    call ui_hit_find
    call ui_resize_hover
    call ui_pointer_show
    jmp .idle
.drag:
    test word [ui_mouse_buttons],1
    jz .drag_end
    call ui_comp_damage_drag
    cmp byte [ui_dragging],4
    je .app_resize
    cmp byte [ui_dragging],2
    je .resize
    cmp byte [ui_dragging],3
    je .window_drag
    mov ax,[ui_mouse_x]
    sub ax,[ui_drag_dx]
    cmp ax,4
    jge .drag_x_min
    mov ax,4
.drag_x_min:
    mov bx,[ui_width]
    sub bx,[ui_ww]
    sub bx,12
    cmp ax,bx
    jle .drag_x_max
    mov ax,bx
.drag_x_max:
    mov [ui_wx],ax
    mov ax,[ui_mouse_y]
    sub ax,[ui_drag_dy]
    cmp ax,32
    jge .drag_y_min
    mov ax,32
.drag_y_min:
    mov bx,[ui_height]
    sub bx,[ui_wh]
    sub bx,36
    cmp ax,bx
    jle .drag_y_max
    mov ax,bx
.drag_y_max:
    mov [ui_wy],ax
    call ui_comp_damage_drag
    mov ax,250
    ret
.window_drag:
    movzx si,byte [ui_window_drag]
    shl si,1
    mov ax,[ui_mouse_x]
    sub ax,[ui_drag_dx]
    cmp ax,4
    jge .window_x_min
    mov ax,4
.window_x_min:
    mov bx,[ui_width]
    sub bx,[ui_window_width+si]
    sub bx,8
    cmp ax,bx
    jle .window_x_max
    mov ax,bx
.window_x_max:
    mov [ui_window_x+si],ax
    mov ax,[ui_mouse_y]
    sub ax,[ui_drag_dy]
    cmp ax,32
    jge .window_y_min
    mov ax,32
.window_y_min:
    mov bx,[ui_height]
    sub bx,[ui_window_height+si]
    sub bx,37
    cmp ax,bx
    jle .window_y_max
    mov ax,bx
.window_y_max:
    mov [ui_window_y+si],ax
    call ui_comp_damage_drag
    mov ax,250
    ret
.resize:
    call ui_resize_apply
    call ui_comp_damage_drag
    mov ax,250
    ret
.desktop_press:
    ; The desktop surface itself: its module takes the button (icons).
    cmp word [app_segs+APP_DESKTOP*2],0
    je .idle
    mov byte [app_capture],0xFF         ; FFh - 1 = the desktop
    mov byte [app_desktop_focus],1
    call ui_damage_active               ; its title shows the focus left
    mov bl,APP_WIN_DESKTOP
    mov dx,1
    call app_mouse
    test ax,ax
    jnz .out
    jmp .idle
.app_press:
    mov bl,[ui_active_window]
    push si
    call app_slot
    pop si
    jc .idle
    inc bl
    mov [app_capture],bl
    dec bl
    mov dx,1
    call app_mouse
    test ax,ax
    jnz .out
    jmp .idle
.app_resize_start:
    mov byte [ui_dragging],4
    mov al,[ui_active_window]
    mov [ui_window_drag],al
    call ui_resize_begin
    jmp .idle
.app_resize:
    ; Invalidate the old footprint before changing the module window size.
    ; The post-update invalidation below covers its new footprint.
    jmp .resize
.idle:
    cmp byte [ui_window_repaint],1
    jne .quiet
    mov byte [ui_window_repaint],0
    mov ax,250
    ret
.quiet:
    xor ax,ax
    ret

ui_focus_previous:
    mov word [ui_focus_step],-1
    jmp ui_focus_walk
ui_focus_next:
    mov word [ui_focus_step],1
ui_focus_walk:
    push fs
    mov fs,[ui_assets_seg]
    ; Rebuild the index from the focused ID: opening a different dialog
    ; replaces the hit list, so an index from the previous window is stale.
    mov cx,[ui_hit_count]
    test cx,cx
    jz .none
    mov si,ui_hits
    xor bx,bx
    mov ax,[ui_focus]
.find:
    mov dl,[ui_active_window]
    inc dl
    cmp dl,[fs:si+9]
    jne .find_next
    cmp al,[fs:si+8]
    je .found
.find_next:
    add si,10
    inc bx
    loop .find
    mov bx,0xFFFF
.found:
    mov [ui_focus_index],bx
    mov cx,[ui_hit_count]
.next:
    mov bx,[ui_focus_index]
    add bx,[ui_focus_step]
    cmp bx,[ui_hit_count]
    jb .valid
    xor bx,bx
    cmp word [ui_focus_step],1
    je .valid
    mov bx,[ui_hit_count]
    dec bx
.valid:
    mov [ui_focus_index],bx
    imul bx,10
    mov ax,[fs:ui_hits+bx+8]
    mov dl,[ui_active_window]
    inc dl
    cmp ah,dl
    jne .skip
    xor ah,ah
    cmp ax,30
    jb .control
    cmp ax,36
    jb .skip ; tiles show no focus mark: arrows select them, Enter opens
    cmp ax,60
    jb .control
    cmp ax,90
    jb .skip
    cmp ax,199
    ja .skip
.control:
    test ax,ax
    jz .skip
    mov [ui_focus],ax
    mov ax,250
    pop fs
    ret
.skip:
    loop .next
.none:
    xor ax,ax
    pop fs
    ret

ui_damage_pressed:
    cmp byte [ui_button_down],1
    jne .done
    pushad
    mov bx,[ui_press_x]
    mov dx,[ui_press_y]
    mov cx,[ui_press_width]
    mov si,[ui_press_height]
    call ui_comp_damage
    mov byte [ui_window_repaint],1
    popad
.done:
    ret

ui_pointer_hide:
    cmp byte [ui_pointer_visible],1
    jne .done
    cmp byte [ui_vbe],0
    je ui_vga_pointer_hide
    pushad
    push fs
    mov fs,[ui_assets_seg]
    mov edi,[ui_pointer_linear]
    call ui_pointer_buffer
    call vc_pointer_restore
    pop fs
    popad
    mov byte [ui_pointer_visible],0
.done:
    ret

ui_pointer_show:
    cmp byte [ui_mouse_ready],1
    jne .done
    cmp byte [ui_pointer_visible],1
    je .done
    cmp byte [ui_vbe],0
    je ui_vga_pointer_show
    pushad
    push fs
    mov fs,[ui_assets_seg]
    mov word [vc_pointer_skip_x],0
    mov ax,[ui_mouse_y]
    sub ax,[ui_cursor_hotspot_y]
    jnc .hy_ok
    neg ax
    mov [vc_pointer_skip_y],al
    xor ax,ax
.hy_ok:
    movzx edi,ax
    movzx eax,word [vc_pitch]
    imul edi,eax
    mov ax,[ui_mouse_x]
    sub ax,[ui_cursor_hotspot_x]
    jnc .hx_ok
    neg ax
    mov [vc_pointer_skip_x],al
    xor ax,ax
.hx_ok:
    movzx eax,ax
    movzx ecx,byte [vc_bytes]
    imul eax,ecx
    add edi,eax
    add edi,[ui_front_base]
    mov [ui_pointer_linear],edi
    call ui_pointer_buffer
    call vc_pointer_save
    mov bx,ui_pointer_black
    call vc_pointer_draw
    mov byte [ui_pointer_visible],1
    pop fs
    popad
.done:
    ret

; Each physical display page owns its cursor background. EDI is the pixel
; offset on that page; FS already addresses the desktop resource block.
ui_pointer_buffer:
    mov si,ui_pointer_under
    cmp edi,[vc_frame_bytes]
    jb .done
    add si,24*16*4
.done:
    ret

ui_vga_pointer_hide:
    cmp byte [ui_pointer_visible], 1
    jne .done
    pushad
    push es
    push ds
    mov ax, 0xA000
    mov es, ax
    mov dx, 0x3CE
    mov ax, 0x0001
    out dx, ax
    mov ax, 0x0003
    out dx, ax
    mov ax, 0x0005
    out dx, ax
    mov ax, 0xFF08
    out dx, ax
    mov si, ui_pointer_under
    mov ax,[ui_assets_seg]
    mov ds,ax
    mov ah, 1
.plane:
    mov dx, 0x3C4
    mov al, 2
    out dx, ax
    push ax
    mov di, [cs:ui_pointer_offset]
    mov bp,[cs:ui_vga_pointer_rows]
.row:
    mov cx,[cs:ui_vga_pointer_bytes]
    rep movsb
    sub di,[cs:ui_vga_pointer_bytes]
    add di,80
    dec bp
    jnz .row
    pop ax
    shl ah, 1
    cmp ah, 16
    jb .plane
    mov ax, 0x0F02
    out dx, ax
    pop ds
    pop es
    popad
    mov byte [ui_pointer_visible], 0
.done:
    ret

ui_vga_pointer_show:
    cmp byte [ui_mouse_ready], 1
    jne .done
    pushad
    push es
    push fs
    mov fs,[ui_assets_seg]
    mov ax, 0xA000
    mov es, ax
    mov word [vc_pointer_skip_x],0
    mov ax, [ui_mouse_y]
    sub ax, [ui_cursor_hotspot_y]
    jnc .vhy_ok
    neg ax
    mov [vc_pointer_skip_y],al
    xor ax, ax
.vhy_ok:
    mov dx,480
    sub dx,ax
    cmp dx,16
    jbe .vrows
    mov dx,16
.vrows:
    mov [ui_vga_pointer_rows],dx
    push ax
    imul ax, 80
    mov bx, [ui_mouse_x]
    sub bx, [ui_cursor_hotspot_x]
    jnc .vhx_ok
    neg bx
    mov [vc_pointer_skip_x],bl
    xor bx, bx
.vhx_ok:
    push bx
    mov di, bx
    shr di, 3
    mov cx,80
    sub cx,di
    cmp cx,3
    jbe .vbytes
    mov cx,3
.vbytes:
    mov [ui_vga_pointer_bytes],cx
    add di, ax
    mov [ui_pointer_offset], di
    mov si, ui_pointer_under
    mov dx, 0x3CE
    mov ax, 0x0005
    out dx, ax
    xor ah, ah
.plane:
    mov al, 4
    out dx, ax
    push ax
    mov di, [ui_pointer_offset]
    mov bp,[ui_vga_pointer_rows]
.row:
    mov cx,[ui_vga_pointer_bytes]
    push di
.byte:
    mov al,[es:di]
    mov [fs:si],al
    inc di
    inc si
    loop .byte
    pop di
    add di, 80
    dec bp
    jnz .row
    pop ax
    inc ah
    cmp ah, 4
    jb .plane
    pop bx
    pop dx
    mov si, ui_pointer_black
    xor al, al
    call ui_pointer_shape
    mov si, ui_pointer_white
    mov al, 15
    call ui_pointer_shape
    mov byte [ui_pointer_visible], 1
    pop fs
    pop es
    popad
.done:
    ret

ui_pointer_shape:
    pushad
    mov [ui_pointer_color], al
    mov bp, 16
    movzx ax,byte [vc_pointer_skip_y]
    sub bp,ax
    shl ax,1
    add si,ax
.row:
    lodsw
    mov cl,[vc_pointer_skip_x]
    shl ax,cl
    push bx
    mov di, 16
    movzx cx,cl
    sub di,cx
.bit:
    shl ax, 1
    jnc .clear
    push ax
    push si
    mov al, [ui_pointer_color]
    mov cx, 1
    mov si, 1
    call ui_rect
    pop si
    pop ax
.clear:
    inc bx
    dec di
    jnz .bit
    pop bx
    inc dx
    dec bp
    jnz .row
    popad
    ret


; Mouse save area is in the desktop resource block.
ui_mouse_saved db 0
ui_mouse_ready db 0
ui_pointer_visible db 0
ui_mouse_x dw 320
ui_mouse_y dw 240
ui_mouse_press_pending db 0
ui_mouse_release_pending db 0
ui_mouse_buttons dw 0
ui_wheel_pending dw 0
; Fixed-size input diagnostics, readable without logging every mouse IRQ.
ui_wheel_trace db 'CWHEEL01'
ui_wheel_samples dw 0
ui_wheel_last dw 0
ui_wheel_target dw 0
ui_wheel_sent dw 0
ui_wheel_result dw 0
ui_previous_button db 0
ui_pressed dw 0
ui_dragging db 0
ui_drag_dx dw 0
ui_drag_dy dw 0
ui_resize_ww dw 0
ui_resize_wh dw 0
ui_focus_index dw 0
ui_focus_step dw 1
ui_pointer_offset dw 0
ui_vga_pointer_rows dw 16
ui_vga_pointer_bytes dw 3
ui_pointer_linear dd 0
ui_pointer_color db 0
ui_pointer_black dw 0x8000,0xC000,0xE000,0xF000,0xF800,0xFC00,0xFE00,0xFF00,0xFF80,0xFFC0,0xFE00,0xEF00,0xCF00,0x8780,0x0780,0x0300
ui_pointer_white dw 0,0,0x4000,0x6000,0x7000,0x7800,0x7C00,0x7E00,0x7F00,0x7C00,0x6C00,0x4600,0x0600,0x0300,0x0300,0
ui_cursor_type db 0
ui_cursor_scheme db 0
ui_cursor_loaded db 0
ui_cursor_hotspot_x dw 0
ui_cursor_hotspot_y dw 0
ui_cursors_path db '\SYSTEM\UI\CURSORS.DAT',0

ui_set_cursor_scheme:
    pushad
    push ds
    push es
    push cs
    pop ds
    cmp al,2
    jbe .scheme_ok
    xor al,al
.scheme_ok:
    mov [ui_cursor_scheme],al
    mov ax,[ui_assets_seg]
    test ax,ax
    jz .done
    mov dx,ui_cursors_path
    mov ax,0x3D00
    int 0x21
    jc .done
    mov bx,ax
    ; Stage the header and records in the renderer's transient row scratch.
    ; Commit only a complete, versioned theme; a missing or truncated file
    ; leaves the last valid theme (or the built-in arrow) intact.
    mov ds,[cs:ui_assets_seg]
    mov dx,vc_pointer_row
    mov cx,12
    mov ah,0x3F
    int 0x21
    jc .close
    cmp ax,12
    jne .close
    cmp dword [vc_pointer_row],'CIUK'
    jne .close
    cmp dword [vc_pointer_row+4],'CUR2'
    jne .close
    cmp dword [vc_pointer_row+8],(478 << 16) | 3
    jne .close
    movzx ax,byte [cs:ui_cursor_scheme]
    imul ax,478
    add ax,28
    xor cx,cx
    mov dx,ax
    mov ax,0x4200
    int 0x21
    jc .close
    mov dx,vc_pointer_row
    mov cx,462
    mov ah,0x3F
    int 0x21
    jc .close
    cmp ax,462
    jne .close
    mov si,vc_pointer_row+64
    mov cx,7
.hotspots:
    cmp byte [si],15
    ja .close
    cmp byte [si+1],15
    ja .close
    add si,66
    loop .hotspots
    push ds
    pop es
    mov si,vc_pointer_row
    mov di,ui_cursor_table
    mov cx,231
    cld
    rep movsw
    mov byte [cs:ui_cursor_loaded],1
.close:
    push cs
    pop ds
    mov ah,0x3E
    int 0x21
    call ui_cursor_apply
.done:
    pop es
    pop ds
    popad
    ret

ui_cursor_apply:
    pushad
    push ds
    push es
    push cs
    pop ds
    mov ax,[ui_assets_seg]
    test ax,ax
    jz .done
    cmp byte [ui_cursor_loaded],0
    je .done
    mov al,[ui_pointer_visible]
    push ax
    test al,al
    jz .not_vis
    call ui_pointer_hide
.not_vis:
    mov ds,[ui_assets_seg]
    push cs
    pop es
    movzx bx,byte [cs:ui_cursor_type]
    imul bx,66
    add bx,ui_cursor_table
    mov si,bx
    mov di,ui_pointer_black
    mov cx,32
    cld
    rep movsw
    lodsb
    movzx ax,al
    mov [cs:ui_cursor_hotspot_x],ax
    lodsb
    movzx ax,al
    mov [cs:ui_cursor_hotspot_y],ax
    push cs
    pop ds
    pop ax
    test al,al
    jz .done
    call ui_pointer_show
.done:
    pop es
    pop ds
    popad
    ret
