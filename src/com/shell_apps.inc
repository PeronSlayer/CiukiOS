; Desktop applications as modules: Files (window 8), Tasks (9), CiukNote (12),
; the desktop surface, Control Panel (13), Devices (14) and Display (18) are separate
; programs in \SYSTEM\APPS (built from src/apps). The shell loads one into its
; own memory block and draws the frames of its windows: its main window and
; the windows it opens itself (ids 19-31: dialogs, properties). It forwards
; paint, keys, the mouse (both buttons, hover) and actions 100-149 of the hits
; it registered, naming the window. The desktop module paints the desktop
; surface (window FEh) and an overlay above every window (FDh: its menus).
; Contract: src/apps/app.h.
APP_COUNT equ 12
APP_DESKTOP equ 3                       ; slot of the desktop module
APP_WIN_DISPLAY equ 18
APP_WIN_DESKTOP equ 0xFE
APP_WIN_OVERLAY equ 0xFD
APP_WIN_FIRST_DYNAMIC equ 19
APP_EV_OPEN equ 1
APP_EV_PAINT equ 2
APP_EV_PAINT_TOPBAR equ 9
APP_EV_TOPBAR equ 10
APP_EV_KEY equ 3
APP_EV_ACTION equ 4
APP_EV_MOUSE equ 5
APP_EV_POLL equ 6
APP_EV_CLOSE equ 7
APP_EV_SUSPEND equ 8
APP_HDR_PARAS equ 0x106
APP_HDR_SERVICE equ 0x108
APP_HDR_ENTRY equ 0x10C
APP_HDR_WIDTH equ 0x10E
APP_HDR_HEIGHT equ 0x110
APP_HDR_TITLE equ 0x112
APP_HDR_FLAGS equ 0x114
APP_HOST equ 0x116
APP_HOST_WINDOW equ APP_HOST+24
APP_ARG equ 0x140
APP_ARG_BYTES equ 128
APP_STATE_BYTES equ 80
APP_TITLE_H equ 30                     ; client area below the title rail

; BL = window -> SI = slot*2, CF=1 when it is no application window.
app_slot:
    push ax
    cmp bl,APP_WIN_OVERLAY
    jae .desktop
    cmp bl,UI_WINDOW_COUNT
    jae .none
    movzx si,bl
    movzx si,byte [app_win_slot+si]
    cmp si,0xFF
    je .none
    shl si,1
    pop ax
    clc
    ret
.desktop:
    mov si,APP_DESKTOP*2
    pop ax
    clc
    ret
.none:
    pop ax
    stc
    ret

; SI = slot*2: the module in memory. CF=1 when it cannot be loaded.
app_load:
    pushad
    push es
    cmp word [app_segs+si],0
    jne .ok
    mov [app_load_slot],si
    mov dx,[app_paths+si]
    mov ax,0x3D00
    int 0x21
    jc .fail
    mov bx,ax
    mov dx,app_header
    mov cx,0x16
    mov ah,0x3F
    int 0x21
    jc .close
    cmp ax,0x16
    jne .close
    cmp dword [app_header],'CAPP'
    jne .close
    push bx
    mov bx,[app_header+6]
    call hi_alloc                      ; an upper block when there is room
    pop bx
    jc .close
    mov es,ax
    mov [app_new_seg],ax
    ; DOS memory is not cleared: the module's static data must start at zero.
    push di
    mov cx,[app_header+6]
    shl cx,3
    xor di,di
    xor ax,ax
    cld
    rep stosw
    pop di
    xor cx,cx
    xor dx,dx
    mov ax,0x4200
    int 0x21
    push ds
    mov ds,[app_new_seg]
    mov dx,0x100
    mov cx,0xFE00
    mov ah,0x3F
    int 0x21
    pop ds
    jc .release
    mov ah,0x3E
    int 0x21
    mov word [es:APP_HDR_SERVICE],app_service
    mov [es:APP_HDR_SERVICE+2],cs
    mov si,[app_load_slot]
    mov [app_segs+si],es
.ok:
    pop es
    popad
    clc
    ret
.release:
    call hi_free
.close:
    mov ah,0x3E
    int 0x21
.fail:
    pop es
    popad
    stc
    ret

; SI = slot*2: release the module's memory.
app_unload:
    push ax
    push es
    mov ax,[app_segs+si]
    test ax,ax
    jz .done
    mov es,ax
    call hi_free
    mov word [app_segs+si],0
.done:
    pop es
    pop ax
    ret

; SI = slot*2, AX = event, DX/BX/CX arguments: the module's answer in AX.
; [app_ev_window] names the window (FFh: the module's main one); the host
; block describes it, the pointer and the keyboard first.
app_call:
    push es
    push di
    push bp
    push word [app_cur_slot]
    push word [app_ev_window]
    mov [app_cur_slot],si
    mov es,[app_segs+si]
    push ax
    push bx
    push cx
    mov di,APP_HOST
    movzx bx,byte [app_ev_window]
    cmp bl,0xFF
    jne .window
    shr si,1
    mov bl,[app_main+si]
    shl si,1
.window:
    push bx
    cmp bl,APP_WIN_OVERLAY
    jb .framed
    xor ax,ax                            ; the desktop: the whole screen, its
    stosw                                ; client origin at the top (y = -30)
    mov ax,-APP_TITLE_H
    stosw
    mov ax,[ui_width]
    stosw
    mov ax,[ui_height]
    add ax,APP_TITLE_H
    stosw
    jmp .pointer
.framed:
    shl bx,1
    mov ax,[ui_window_x+bx]
    stosw
    mov ax,[ui_window_y+bx]
    stosw
    mov ax,[ui_window_width+bx]
    stosw
    mov ax,[ui_window_height+bx]
    stosw
.pointer:
    mov ax,[ui_mouse_x]
    stosw
    mov ax,[ui_mouse_y]
    stosw
    mov ax,[ui_mouse_buttons]
    stosw
    mov ah,2
    int 0x16
    xor ah,ah
    stosw
    pop bx
    xor ax,ax
    cmp bl,APP_WIN_OVERLAY
    jb .framed_focus
    mov al,[app_desktop_focus]
    jmp .inactive
.framed_focus:
    cmp bl,[ui_active_window]
    jne .inactive
    inc ax
.inactive:
    stosw
    push ds
    push 0x40
    pop ds
    mov ax,[0x6C]
    pop ds
    stosw
    mov ax,[ui_width]
    stosw
    mov ax,[ui_height]
    stosw
    mov ax,bx                            ; +24 window
    stosw
    mov ax,[app_ev_context]              ; +26 context of a desktop menu
    stosw
    movzx ax,byte [app_set_mouse+2]      ; +28 double-click ticks
    stosw
    pop cx
    pop bx
    pop ax
    mov bp,[es:APP_HDR_ENTRY]
    mov [app_far],bp
    mov [app_far+2],es
    mov [app_shell_sp],sp
    call far [app_far]
    pop word [app_ev_window]
    pop word [app_cur_slot]
    pop bp
    pop di
    pop es
    ret

; BL = window, SI = argument (0: its saved state). Load, open and show it.
app_open:
    pushad
    push es
    xor bh,bh
    mov [app_open_id],bx
    mov di,si
    call app_slot
    jc .done
    mov byte [app_ev_window],0xFF
    call app_load
    jc .failed
    mov [app_open_slot],si
    test di,di
    jnz .argument
    mov di,si
    shr di,1
    imul di,APP_STATE_BYTES
    add di,app_state
.argument:
    mov si,di
    mov bx,[app_open_slot]
    mov es,[app_segs+bx]
    mov di,APP_ARG
    mov cx,APP_ARG_BYTES-1
.copy:
    lodsb
    stosb
    test al,al
    loopnz .copy
    mov byte [es:di],0
    mov si,[app_open_slot]
    mov ax,APP_EV_OPEN
    xor dx,dx
    call app_call
    cmp byte [app_open_id],APP_WIN_OVERLAY
    jae .done                              ; the desktop has no frame
    ; A new window takes the size the module chose for this screen.
    mov bx,[app_open_id]
    cmp byte [ui_window_flags+bx],0
    jne .sized
    mov di,bx
    shl di,1
    mov ax,[es:APP_HDR_WIDTH]
    test ax,ax
    jz .sized
    mov [ui_window_width+di],ax
    mov ax,[es:APP_HDR_HEIGHT]
    mov [ui_window_height+di],ax
.sized:
    mov bx,[app_open_id]
    mov [ui_dialog],bl
    call ui_windows_open
    call app_after
    jmp .done
.failed:
    mov byte [ui_sfx_pending],5
.done:
    pop es
    popad
    ret

; Window BL is closing: CF=1 when its module keeps it open (unsaved work).
; A module's own window just goes; its main window takes the module's other
; windows and the module with it.
app_close:
    push si
    push ax
    call app_slot
    jc .allow
    cmp word [app_segs+si],0
    je .allow
    mov [app_ev_window],bl
    mov ax,APP_EV_CLOSE
    call app_call
    mov byte [app_ev_window],0xFF
    cmp ax,2
    je .detach
    cmp ax,3
    je .unload_detached
    test ax,ax
    jnz .keep
    push bx
    shr si,1
    cmp bl,[app_main+si]
    pushf
    shl si,1
    popf
    jne .own_window
    call app_close_owned
    call app_unload
.own_window:
    pop bx
    call app_win_free
    jmp .allow
.unload_detached:
    call app_unload
.detach:
    call app_win_free
.allow:
    pop ax
    pop si
    clc
    ret
.keep:
    call app_after
    pop ax
    pop si
    stc
    ret

; SI = slot*2: the windows it opened are closed.
app_close_owned:
    pushad
    mov ax,si
    shr ax,1
    mov bx,APP_WIN_FIRST_DYNAMIC
.next:
    cmp [app_win_slot+bx],al
    jne .advance
    call app_damage
    mov byte [ui_window_flags+bx],0
    call app_win_free
.advance:
    inc bx
    cmp bx,UI_WINDOW_COUNT
    jb .next
    mov byte [ui_bar_dirty],1
    popad
    ret

; BL = a module's own window: it no longer belongs to the module.
app_win_free:
    push bx
    xor bh,bh
    cmp bl,APP_WIN_FIRST_DYNAMIC
    jb .done
    cmp bl,UI_WINDOW_COUNT
    jae .done
    mov byte [app_win_slot+bx],0xFF
    mov byte [app_win_style+bx],0
.done:
    pop bx
    ret

; Before DOS/EXEC: each module saves what it needs to reopen in its state
; buffer and is unloaded, unless it must stay (unsaved work).
app_suspend:
    pushad
    push es
    mov byte [app_ev_window],0xFF
    mov byte [app_overlay],0
    xor si,si
.next:
    cmp word [app_segs+si],0
    je .advance
    mov ax,APP_EV_SUSPEND
    call app_call
    test ax,ax
    jnz .advance
    push si
    push ds
    mov di,si
    shr di,1
    imul di,APP_STATE_BYTES
    add di,app_state
    push ds
    pop es
    mov ds,[app_segs+si]
    mov si,APP_ARG
    mov cx,APP_STATE_BYTES
    rep movsb
    pop ds
    pop si
    call app_close_owned
    call app_unload
.advance:
    add si,2
    cmp si,APP_COUNT*2
    jb .next
    pop es
    popad
    ret

; After DOS/EXEC (and at desktop start): reopen the modules of the windows
; that stayed open, and the desktop module.
app_resume:
    pushad
    inc word [app_changes]               ; a DOS program may have changed files
    mov byte [app_ev_window],0xFF
    xor si,si
.next:
    movzx bx,byte [app_main+si]
    cmp bl,APP_WIN_DESKTOP
    je .open
    cmp byte [ui_window_flags+bx],0
    je .advance
.open:
    shl si,1
    cmp word [app_segs+si],0
    jne .resident
    call app_load
    jc .gone
    push si
    mov es,[app_segs+si]
    shr si,1
    imul si,APP_STATE_BYTES
    add si,app_state
    mov di,APP_ARG
    mov cx,APP_STATE_BYTES
    rep movsb
    pop si
    mov ax,APP_EV_OPEN
    mov dx,1                             ; resumed
    call app_call
    jmp .loaded
.resident:
    mov ax,APP_EV_OPEN
    mov dx,2                             ; stayed: its other windows closed
    call app_call
    jmp .loaded
.gone:
    cmp bl,APP_WIN_DESKTOP
    je .loaded
    mov byte [ui_window_flags+bx],0
.loaded:
    shr si,1
.advance:
    inc si
    cmp si,APP_COUNT
    jb .next
    push cs
    pop es
    popad
    ret

; Main loop: idle work of the open modules. AX=250 when one repaints.
app_poll:
    push si
    push dx
    xor dx,dx
    mov byte [app_busy],0
    mov byte [app_ev_window],0xFF
    xor si,si
.next:
    cmp word [app_segs+si],0
    je .advance
    mov ax,APP_EV_POLL
    call app_call
    cmp ax,3
    jne .busy_only
    ; Module queued a precise compositor rectangle and needs a paint pass.
    mov byte [app_busy],1
    mov dx,250
    jmp .advance
.busy_only:
    cmp ax,2
    jne .quiet
    mov byte [app_busy],1
    jmp .advance
.quiet:
    test ax,ax
    jz .advance
    mov dx,250
    push bx
    shr si,1
    mov bl,[app_main+si]
    shl si,1
    call app_damage
    pop bx
.advance:
    add si,2
    cmp si,APP_COUNT*2
    jb .next
    mov ax,dx
    pop dx
    pop si
    ; An applet opened from a deferred argument may request a DOS helper
    ; during EV_POLL. Deliver that request through the normal UI command path.
    cmp byte [app_request],2
    jne .check_close
    mov byte [app_request],0
    mov ax,249
    ret
.check_close:
    cmp byte [app_request],1
    jne .return
    mov byte [app_request],0
    pushad
    call ui_windows_close
    popad
    mov ax,250
.return:
    ret

; BL = window: its whole rectangle, shadow included, is repainted.
app_damage:
    pushad
    xor bh,bh
    cmp bl,APP_WIN_OVERLAY
    jb .window
    mov byte [ui_full_redraw],1
    jmp .done
.window:
    cmp bl,UI_WINDOW_COUNT
    jae .done
    cmp byte [ui_window_flags+bx],1
    jne .done
    shl bx,1
    mov cx,[ui_window_width+bx]
    add cx,5
    mov si,[ui_window_height+bx]
    add si,6
    mov dx,[ui_window_y+bx]
    mov bx,[ui_window_x+bx]
    call ui_comp_damage
.done:
    popad
    ret

; The active window's module takes a key first: the desktop's menu when it
; is open, else the active window, else (no window has the focus) the
; desktop. AX = BIOS key. CF=1 with AX = the UI action when it used the key.
app_key:
    push ax                      ; an unhandled key must reach global shortcuts
    push si
    push bx
    mov bl,APP_WIN_OVERLAY
    cmp byte [app_overlay],0
    jne .target
    cmp byte [app_desktop_focus],0
    jne .desktop
    mov bl,[ui_active_window]
    test bl,bl
    jnz .target
    cmp byte [ui_window_visible],1
    je .target
.desktop:
    mov bl,APP_WIN_DESKTOP
.target:
    call app_slot
    jc .unused
    cmp word [app_segs+si],0
    je .unused
    mov [app_ev_window],bl
    mov dx,ax
    mov ax,APP_EV_KEY
    call app_call
    mov byte [app_ev_window],0xFF
    test ax,ax
    jz .unused
    call app_after_action
    pop bx
    pop si
    add sp,2                     ; handled: keep the module's action in AX
    stc
    ret
.unused:
    pop bx
    pop si
    pop ax
    clc
    ret

; AX = 100..149: the active module's control. AX = UI action.
app_action:
    push si
    push bx
    mov bl,[ui_active_window]
    call app_slot
    jc .none
    cmp word [app_segs+si],0
    je .none
    lea dx,[eax-100]
    mov [app_ev_window],bl
    mov ax,APP_EV_ACTION
    call app_call
    mov byte [app_ev_window],0xFF
    call app_after_action
    pop bx
    pop si
    ret
.none:
    xor ax,ax
    pop bx
    pop si
    ret

; DX = kind (1 left down, 2 move, 3 left up, 4 right down, 6 hover),
; window BL (FEh the desktop, FDh its menu overlay). The module gets the
; pointer relative to the window's client area (BX, CX); on the desktop,
; screen coordinates. AX = UI action (250 when the module repaints).
app_mouse:
    push si
    push bx
    push cx
    push di
    call app_slot
    jc .none
    cmp word [app_segs+si],0
    je .none
    mov [app_ev_window],bl
    mov cx,[ui_mouse_y]
    mov di,[ui_mouse_x]
    cmp bl,APP_WIN_OVERLAY
    jae .screen
    movzx bx,bl
    shl bx,1
    sub di,[ui_window_x+bx]
    sub cx,[ui_window_y+bx]
    sub cx,APP_TITLE_H
.screen:
    mov bx,di
    mov ax,[app_pointer_event]
    call app_call
    mov byte [app_ev_window],0xFF
    call app_after_action
    jmp .done
.none:
    xor ax,ax
.done:
    pop di
    pop cx
    pop bx
    pop si
    ret

; Signed wheel steps in DX. Deliver to the client under the pointer.
app_wheel:
    push bx
    push si
    push dx
    call ui_hit_find
    movzx bx,byte [ui_hit_target]
    pop dx
    test bx,bx
    jz .none
    dec bx
    mov [ui_wheel_target],bx
    mov [ui_wheel_sent],dx
    mov byte [app_pointer_event],11
    call app_mouse
    mov [ui_wheel_result],ax
    mov byte [app_pointer_event],APP_EV_MOUSE
    test ax,ax
    jz .done
    call app_damage
    jmp .done
.none:
    xor ax,ax
.done:
    pop si
    pop bx
    ret

; Hover (no button): the overlay, else the module window or desktop under
; the pointer. AX = 250 when it repaints.
app_hover:
    push bx
    push si
    mov bl,APP_WIN_OVERLAY
    cmp byte [app_overlay],0
    jne .send
    push ax
    call ui_hit_find
    movzx bx,byte [ui_hit_target]
    pop ax
    test bx,bx
    jz .desktop
    dec bx
    call app_slot
    jnc .send
    jmp .none
.desktop:
    mov bl,APP_WIN_DESKTOP
.send:
    call app_hover_leave
    mov dx,6
    call app_mouse
    test ax,ax
    jz .done
    cmp bl,APP_WIN_OVERLAY             ; menus damage their own area
    jae .done
    call app_damage
    jmp .done
.none:
    mov bl,0xFF
    call app_hover_leave
    xor ax,ax
.done:
    pop si
    pop bx
    ret

; BL = the new hover target (FFh none): the previous one hears the pointer
; leave (a hover outside it) and may drop its highlight.
app_hover_leave:
    push ax
    push bx
    push dx
    xchg bl,[app_hover_last]
    cmp bl,[app_hover_last]
    je .done
    cmp bl,0xFF
    je .done
    xor ax,ax
    call app_s_cursor
    mov dx,6
    call app_mouse
    test ax,ax
    jz .done
    cmp bl,APP_WIN_OVERLAY
    jae .done
    call app_damage
.done:
    pop dx
    pop bx
    pop ax
    ret

; A module answer (AX: 0 nothing, 1 repaint) plus what it asked for through
; app_service meanwhile -> UI action in AX (249 with SI = command).
app_after_action:
    test ax,ax
    jz .requests
    mov ax,250
.requests:
    call app_after
    cmp byte [app_request],3
    jne .command
    mov byte [app_request],0
    movzx ax,byte [app_action_code]
    ret
.command:
    cmp byte [app_request],2
    jne .done
    mov byte [app_request],0
    mov si,app_command
    mov ax,249
.done:
    ret

; Deferred requests: close a window, open another module.
app_after:
    cmp byte [app_close_window],0
    je .own
    pushad
    movzx bx,byte [app_close_window]
    mov byte [app_close_window],0
    call ui_windows_raise
    call ui_windows_close
    popad
    mov ax,250
.own:
    cmp byte [app_request],1
    jne .open
    mov byte [app_request],0
    pushad
    call ui_windows_close
    popad
    mov ax,250
.open:
    cmp byte [app_open_window],0
    je .done
    push bx
    push si
    mov bl,[app_open_window]
    mov byte [app_open_window],0
    mov si,app_open_arg
    call app_open
    pop si
    pop bx
    mov ax,250
.done:
    ret

; Window BL (ui_dialog), its frame and then the module's client area. A
; dialog frame (style bit 0) has only the close button and no resize grip.
app_draw:
    pushad
    movzx di,bl
    mov al,[app_win_style+di]
    mov [app_draw_style],al
    shl di,1
    mov bx,[ui_window_x+di]
    mov dx,[ui_window_y+di]
    mov cx,[ui_window_width+di]
    mov si,[ui_window_height+di]
    mov ax,70
    call ui_hit_add
    add bx,4
    add dx,5
    mov al,0
    call ui_rect
    sub bx,4
    sub dx,5
    mov al,7
    call ui_bevel
    ; Title rail and its drag area.
    add bx,3
    add dx,3
    sub cx,6
    mov si,25
    call ui_windows_title_color
    call ui_title_rail
    sub cx,78
    mov ax,62
    call ui_hit_add
    ; Title from the module (it may carry a document name).
    push bx
    push dx
    movzx bx,byte [ui_dialog]
    shl bx,1
    mov ax,[app_win_title+bx]
    shr bx,1
    call app_slot
    push ds
    pop es
    mov di,app_title
    push ds
    mov ds,[app_segs+si]
    mov si,ax
    test si,si
    jnz .title
    mov si,[APP_HDR_TITLE]
.title:
    mov cx,63
.char:
    lodsb
    stosb
    test al,al
    loopnz .char
    mov byte [es:di],0
    pop ds
    pop dx
    pop bx
    add bx,7
    add dx,4
    mov si,app_title
    push bx
    movzx bx,byte [ui_dialog]
    shl bx,1
    mov cx,[ui_window_width+bx]
    sub cx,92
    test byte [app_draw_style],1
    jz .title_width
    add cx,48
.title_width:
    pop bx
    call ui_windows_title_color
    call ui_title_text
    ; Minimize, maximize, close.
    movzx di,byte [ui_dialog]
    shl di,1
    mov bx,[ui_window_x+di]
    add bx,[ui_window_width+di]
    sub bx,29
    mov dx,[ui_window_y+di]
    add dx,6
    mov byte [app_max_label],2          ; maximize, or restore when maximized
    cmp word [app_saved_w+di],0         ; (U_BUTTON reloads DI below)
    je .max_glyph
    mov byte [app_max_label],3
.max_glyph:
    test byte [app_draw_style],1
    jnz .close_only
    sub bx,48
    U_BUTTON bx,dx,22,19,ut_min,17
    add bx,24
    U_BUTTON bx,dx,22,19,app_max_label,19
    add bx,24
.close_only:
    U_BUTTON bx,dx,22,19,ut_close,18
    test byte [app_draw_style],1
    jnz .client
    ; Resize grip, bottom right.
    mov bx,[ui_window_x+di]
    add bx,[ui_window_width+di]
    sub bx,14
    mov dx,[ui_window_y+di]
    add dx,[ui_window_height+di]
    sub dx,14
    mov cx,12
    mov si,12
    mov ax,65
    call ui_hit_add
.client:
    mov bl,[ui_dialog]
    mov [app_ev_window],bl
    call app_slot
    mov ax,APP_EV_PAINT
    call app_call
    mov byte [app_ev_window],0xFF
    popad
    ret

; The desktop module's surface (under the windows) and its overlay (above
; everything). CF=1 when there is no desktop module (the shell's own icons).
app_desktop_paint:
    cmp word [app_segs+APP_DESKTOP*2],0
    je .none
    push bx
    mov bl,APP_WIN_DESKTOP
    call app_paint_desktop
    pop bx
    clc
    ret
.none:
    stc
    ret

; The VGA fallback updates its top indicators without repainting the whole
; visible desktop. The normal scene still uses APP_EV_PAINT.
app_desktop_topbar_paint:
    cmp word [app_segs+APP_DESKTOP*2],0
    je .done
    pushad
    mov byte [app_ev_window],APP_WIN_DESKTOP
    mov si,APP_DESKTOP*2
    mov ax,APP_EV_PAINT_TOPBAR
    call app_call
    mov byte [app_ev_window],0xFF
    popad
.done:
    ret

; AX = 1 the CiukiOS menu, 2 the volume control: popups of the desktop
; module on the overlay. CF=1 when the desktop module is not loaded.
app_desktop_topbar_click:
    cmp word [app_segs+APP_DESKTOP*2],0
    je .none
    pushad
    ; The menu or popup takes the keyboard (Esc, arrows), not the active
    ; window: a game in a DOS window would otherwise receive those keys.
    mov byte [app_desktop_focus],1
    call ui_damage_active
    mov dx,ax
    mov byte [app_ev_window],APP_WIN_DESKTOP
    mov si,APP_DESKTOP*2
    mov ax,APP_EV_TOPBAR
    call app_call
    mov byte [app_ev_window],0xFF
    popad
    clc
    ret
.none:
    stc
    ret

app_overlay_paint:
    cmp byte [app_overlay],0
    je .done
    cmp word [app_segs+APP_DESKTOP*2],0
    je .done
    push bx
    mov bl,APP_WIN_OVERLAY
    call app_paint_desktop
    pop bx
.done:
    ret

app_paint_desktop:
    pushad
    mov byte [ui_hit_owner],0
    mov [app_ev_window],bl
    mov si,APP_DESKTOP*2
    mov ax,APP_EV_PAINT
    call app_call
    mov byte [app_ev_window],0xFF
    popad
    ret

; Maximize/restore the active module window (action 19).
app_maximize:
    pushad
    movzx bx,byte [ui_active_window]
    call app_damage
    mov di,bx
    shl di,1
    cmp word [app_saved_w+di],0
    je .maximize
    mov ax,[app_saved_x+di]
    mov [ui_window_x+di],ax
    mov ax,[app_saved_y+di]
    mov [ui_window_y+di],ax
    mov ax,[app_saved_w+di]
    mov [ui_window_width+di],ax
    mov ax,[app_saved_h+di]
    mov [ui_window_height+di],ax
    mov word [app_saved_w+di],0
    call ui_windows_clamp         ; saved size may predate a resolution change
    jmp .done
.maximize:
    mov ax,[ui_window_x+di]
    mov [app_saved_x+di],ax
    mov ax,[ui_window_y+di]
    mov [app_saved_y+di],ax
    mov ax,[ui_window_width+di]
    mov [app_saved_w+di],ax
    mov ax,[ui_window_height+di]
    mov [app_saved_h+di],ax
    mov word [ui_window_x+di],4
    mov word [ui_window_y+di],34
    mov ax,[ui_width]
    sub ax,13
    mov [ui_window_width+di],ax
    mov ax,[ui_height]
    sub ax,72
    mov [ui_window_height+di],ax
.done:
    call app_damage
    popad
    ret

; Far service entry for the modules: AX = service, DS:SI = 16-byte
; arguments in the module (x, y, w, h, text, value, value2, value3).
app_service:
    ; Services run on the shell's own stack (its drawing code assumes
    ; SS = DS), below the frame of the app_call that entered the module.
    cli
    mov [cs:app_module_ss],ss
    mov [cs:app_module_sp],sp
    mov [cs:app_service_ax],ax
    mov ax,cs
    mov ss,ax
    mov sp,[cs:app_shell_sp]
    sub sp,32
    sti
    mov ax,[cs:app_service_ax]
    push ds
    push es
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    cld
    mov bp,ax                          ; the service number
    push cs
    pop es
    mov di,app_args
    mov cx,8
    rep movsw
    mov si,[es:app_args+8]
    mov di,app_text
    mov cx,159
.text:
    lodsb
    stosb
    test al,al
    loopnz .text
    mov byte [es:di],0
    mov [es:app_caller],ds
    push cs
    pop ds
    cmp bp,APP_SVC_COUNT
    jae .zero
    shl bp,1
    mov bx,[app_args]
    mov dx,[app_args+2]
    mov cx,[app_args+4]
    mov si,[app_args+6]
    mov ax,[app_args+10]
    call [app_services+bp]
    jmp .done
.zero:
    xor ax,ax
.done:
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    pop ds
    cli
    mov ss,[cs:app_module_ss]
    mov sp,[cs:app_module_sp]
    sti
    retf

app_services dw app_s_rect,app_s_text,app_s_bevel,app_s_inset,app_s_button
             dw app_s_hit,app_s_icon,app_s_measure,app_s_repaint,app_s_command
             dw app_s_open,app_s_close,app_s_sound,app_s_windows,app_s_window_cmd
             dw app_s_idle,app_s_mono,app_s_win_open,app_s_win_close,app_s_damage
             dw app_s_action,app_s_overlay,app_s_font,app_s_settings,app_s_palette
             dw app_s_changes,app_s_bitmap,app_s_band_info,app_s_desktop_focus,app_s_display_mode
             dw app_s_cursor
APP_SVC_COUNT equ ($-app_services)/2

app_s_cursor:
    cmp ax,2
    jbe .ok
    xor ax,ax
.ok:
    cmp al,[ui_cursor_type]
    je .done
    mov [ui_cursor_type],al
    call ui_cursor_apply
.done:
    ret

; Display module only. AX=0 queries; AX=100h..FFFh queues a graphics change.
app_s_display_mode:
    cmp word [app_cur_slot],20
    jne .bad
    test ax,ax
    jz .query
    cmp ax,0x12
    je .queue
    cmp ax,0x100
    jb .bad
    cmp ax,0xFFF
    ja .bad
.queue:
    ; Only Display owns this service. A rollback before the next UI turn
    ; supersedes its unconsumed preview instead of losing the restore.
    mov [ui_mode_request],ax
    ret
.query:
    mov ax,-1
    cmp byte [ui_vbe],1
    jne .done
    mov ax,[vc_mode]
    cmp word [ui_mode_request],0
    je .done
    xor ax,ax
.done:
    ret
.bad:
    mov ax,-1
    ret

app_s_desktop_focus:
    movzx ax,byte [app_desktop_focus]
    ret

; The 19-byte video-band descriptor for a VM-window presenter. The caller
; uses VIDEO_BAND while this band's EV_PAINT is running; the module never
; holds a pointer to the compositor's private memory between calls.
app_s_band_info:
    cmp byte [ui_vbe],1
    jne .none
    mov ax,[ui_comp_seg]
    test ax,ax
    jz .none
    mov es,[app_caller]
    mov di,[app_args+8]
    stosw
    mov ax,[ui_comp_top]
    stosw
    mov ax,[ui_comp_bottom]
    stosw
    mov ax,[ui_comp_stride]
    stosw
    mov ax,[ui_comp_right]
    sub ax,[ui_comp_left]
    stosw
    mov al,[vc_bytes]
    stosb
    mov si,vc_info+31
    mov cx,6
    rep movsb
    mov ax,[ui_comp_left]
    stosw
    mov ax,1
    ret
.none:
    xor ax,ax
    ret

app_s_rect:
    jmp ui_rect
app_s_text:
    mov si,app_text
    test ah,2                          ; the alternate font
    jz ui_text
    and ah,1
    call app_font_swap
    call ui_text
    jmp app_font_swap
app_s_bevel:
    jmp ui_bevel
app_s_inset:
    jmp ui_inset
app_s_button:
    add ax,100
    mov di,app_text
    jmp ui_button
app_s_hit:
    add ax,100
    jmp ui_hit_add
app_s_icon:
    jmp ui_icon
app_s_measure:
    mov si,app_text
    test ah,2
    jz .measure
    and ah,1
    call app_font_swap
    call ui_measure_styled
    call app_font_swap
    mov ax,cx
    ret
.measure:
    call ui_measure_styled
    mov ax,cx
    ret

; Exchange the system font segment and the alternate font's (when loaded).
app_font_swap:
    push ax
    mov ax,[app_alt_font]
    test ax,ax
    jz .done
    xchg ax,[ui_assets_seg]
    mov [app_alt_font],ax
.done:
    pop ax
    ret
app_s_repaint:
    mov bl,al                          ; value: a window of the module
    test al,al
    jnz .window
    mov bl,[app_ev_window]
    cmp bl,0xFF
    jne .window
    mov bl,[ui_active_window]
.window:
    jmp app_damage
app_s_command:
    mov si,app_text
    mov di,app_command
    mov cx,127
.copy:
    lodsb
    test al,al
    jz .ready
    stosb
    loop .copy
    cmp byte [si],0
    jne .too_long
.ready:
    mov byte [di],0
    mov byte [app_request],2
    ret
.too_long:
    mov byte [di],0
    mov byte [ui_sfx_pending],5
    ret
app_s_open:
    mov [app_open_window],al
    mov si,app_text
    mov di,app_open_arg
    jmp copy_z_to_di
app_s_close:
    mov byte [app_request],1
    ret
app_s_sound:
    mov [ui_sfx_pending],al
    ret
; Windows list into the caller's buffer (text offset): for each window
; 0..UI_WINDOW_COUNT-1 its state byte (0 closed, 1 shown, 2 minimized,
; +80h active) and its title (24 bytes, zero padded).
app_s_windows:
    mov es,[app_caller]
    mov di,[app_args+8]
    xor bx,bx
.next:
    mov al,[ui_window_flags+bx]
    test bx,bx
    jnz .state
    mov al,[ui_window_visible]
.state:
    cmp bl,[ui_active_window]
    jne .stored
    or al,0x80
.stored:
    stosb
    push bx
    shl bx,1
    mov si,[ui_window_titles+bx]
    pop bx
    mov cx,24
.title:
    lodsb
    stosb
    test al,al
    loopnz .title
    jcxz .advance
    xor al,al
    rep stosb
.advance:
    inc bx
    cmp bx,UI_WINDOW_COUNT
    jb .next
    push cs
    pop es
    mov ax,UI_WINDOW_COUNT
    ret
; AL = window, value2 = 1 raise, 2 close, 3 minimize, 4 maximize/restore,
; 5 hide (module windows). A hidden window (flag 3) keeps its module loaded
; but is neither drawn nor listed on the taskbar: its own dialogs stand alone.
app_s_window_cmd:
    movzx bx,al
    cmp bx,UI_WINDOW_COUNT
    jae .done
    mov ax,[app_args+12]
    cmp al,1
    jne .close
    mov byte [ui_window_flags+bx],1
    test bx,bx
    jnz .raise
    mov byte [ui_window_visible],1
.raise:
    mov [ui_dialog],bl
    jmp ui_windows_open
.close:
    cmp al,2
    jne .minimize
    mov [app_close_window],bl
    ret
.minimize:
    cmp al,4
    jne .minimize_only
    push bx
    mov [ui_dialog],bl
    call ui_windows_open
    pop bx
    call app_slot
    jc .done
    jmp app_maximize
.minimize_only:
    cmp al,3
    jne .hide
    test bx,bx
    jz .done
    mov byte [ui_window_flags+bx],2
    mov byte [ui_bar_dirty],1
.done:
    ret
.hide:
    cmp al,5
    jne .done
    test bx,bx
    jz .done
    mov byte [ui_window_flags+bx],3
    mov byte [ui_bar_dirty],1
    mov byte [ui_full_redraw],1
    ret
; Idle share of the last second in percent (desktop HLT time).
app_s_idle:
    movzx ax,byte [app_idle_percent]
    ret
; Fixed-pitch text: each glyph centred in a cell of width w (8 when 0).
app_s_mono:
    test cx,cx
    jnz .cell
    mov cx,8
.cell:
    mov [app_cell],cx
    mov si,app_text
    test ah,2                          ; the alternate font
    jz .font
    and ah,1
    call app_font_swap
    call .font
    jmp app_font_swap
.font:
    push ax
.next:
    lodsb
    test al,al
    jz .done
    mov [app_glyph],al
    push si
    push bx
    mov si,app_glyph
    push cx
    call ui_measure
    mov ax,[app_cell]
    sub ax,cx
    sar ax,1
    add bx,ax
    pop cx
    mov bp,sp
    mov ax,[bp+4]
    call ui_text
    pop bx
    pop si
    add bx,[app_cell]
    jmp .next
.done:
    pop ax
    ret

; A window of the calling module: x, y (-1: centred), w, h, text = its title
; (in the module, kept there), value = style (1 dialog frame). AX = id, 0
; when every window is in use.
app_s_win_open:
    mov bx,APP_WIN_FIRST_DYNAMIC
.find:
    cmp byte [ui_window_flags+bx],0
    jne .next
    cmp byte [app_win_slot+bx],0xFF
    je .found
.next:
    inc bx
    cmp bx,UI_WINDOW_COUNT
    jb .find
    xor ax,ax
    ret
.found:
    mov ax,[app_cur_slot]
    shr ax,1
    mov [app_win_slot+bx],al
    mov al,[app_args+10]
    mov [app_win_style+bx],al
    mov di,bx
    shl di,1
    mov ax,[app_args+8]
    mov [app_win_title+di],ax
    mov ax,[app_args+4]
    mov [ui_window_width+di],ax
    mov ax,[app_args+6]
    mov [ui_window_height+di],ax
    mov [ui_dialog],bl
    push bx
    call ui_windows_open
    pop bx
    mov di,bx
    shl di,1
    mov ax,[app_args]
    cmp ax,-1
    je .placed
    mov [ui_window_x+di],ax
    mov ax,[app_args+2]
    mov [ui_window_y+di],ax
    call ui_windows_clamp
.placed:
    call app_damage
    mov ax,bx
    ret

; value = one of the caller's windows: it closes (no CLOSE event).
app_s_win_close:
    movzx bx,al
    cmp bx,UI_WINDOW_COUNT
    jae .done
    mov ax,[app_cur_slot]
    shr ax,1
    cmp [app_win_slot+bx],al
    jne .done
    call app_damage
    mov al,bl
    mov si,ui_window_closed
    call ui_window_log
    mov byte [ui_window_flags+bx],0
    call app_win_free
    cmp bl,[ui_active_window]
    jne .done
    call ui_windows_unfocus
.done:
    ret

; x, y, w, h: this screen area is painted again (menus beyond a window).
app_s_damage:
    jmp ui_comp_damage

; value = a UI action for the shell (Programs 1, Run 4, About 6, Shut down
; 7, Display 8, Sound 9, DOS window 3, full-screen DOS 25, Tasks 23,
; Wallpaper 24, media 14-16, Restart 42), after this event.
app_s_action:
    mov [app_action_code],al
    mov byte [app_request],3
    ret

; value = 1: the desktop module's menu overlay is shown (above everything,
; and it receives the mouse and the keys), 0: hidden.
app_s_overlay:
    mov [app_overlay],al
    mov al,[ui_mouse_buttons]          ; buttons already down stay quiet
    and al,3
    mov [app_overlay_buttons],al
    ret

; text = a font file (\SYSTEM\FONTS\*.CFN), value = 0 the system font, 1
; the alternate font (text with style 200h). AX = 0, or 1 on failure.
APP_FONT_BYTES equ 6270
app_s_font:
    cmp byte [app_args+10],0
    je .open
    cmp word [app_alt_font],0
    je .open
    mov si,app_text                    ; this file is in the slot already
    mov di,app_alt_name
    mov cx,64
    repe cmpsb
    jne .open
    xor ax,ax
    ret
.open:
    mov dx,app_text
    mov ax,0x3D00
    int 0x21
    jc .fail
    mov bx,ax
    push bx
    xor cx,cx
    mov dx,64                          ; after the CFN header
    mov ax,0x4200
    int 0x21
    pop bx
    mov ax,[ui_assets_seg]
    cmp byte [app_args+10],0
    je .target
    mov ax,[app_alt_font]
    test ax,ax
    jnz .target
    push bx
    mov bx,(UI_ASSET_FONT+APP_FONT_BYTES+15)/16
    mov ah,0x48
    int 0x21
    pop bx
    jc .close_fail
    mov [app_alt_font],ax
.target:
    push ds
    mov ds,ax
    mov dx,UI_ASSET_FONT
    mov cx,APP_FONT_BYTES
    mov ah,0x3F
    int 0x21
    pop ds
    pushf
    mov ah,0x3E
    int 0x21
    popf
    jc .fail
    cmp byte [app_args+10],0
    je .system
    mov si,app_text
    mov di,app_alt_name
    mov cx,64
    rep movsb
    jmp .loaded
.system:
    mov byte [ui_full_redraw],1
.loaded:
    xor ax,ax
    ret
.close_fail:
    mov ah,0x3E
    int 0x21
.fail:
    mov ax,1
    ret

; \SYSTEM\UI\DESKTOP.CFG, written by the Control Panel: colours, mouse,
; keyboard, system font. Read and applied at desktop start and on request
; (service 23), then everything is painted again.
app_s_settings:
app_settings_load:
    pushad
    mov dx,app_settings_path
    mov ax,0x3D00
    int 0x21
    jc .fallback
    mov bx,ax
    mov byte [app_settings+72],0
    mov dx,app_settings
    mov cx,APP_SETTINGS_BYTES
    mov ah,0x3F
    int 0x21
    pushf
    push ax
    mov ah,0x3E
    int 0x21
    pop ax
    popf
    jc .fallback
    cmp ax,72
    jb .fallback
    cmp dword [app_settings],'CUI1'
    jne .fallback
    mov si,app_settings+4               ; 16 colours
    mov di,ui_palette
    mov cx,48
    cld
    rep movsb
    mov si,app_settings+52              ; mouse speed, swap, double-click
    mov di,app_set_mouse
    movsw
    movsb
    mov bx,[app_settings+55]            ; keyboard delay (BH), rate (BL)
    mov ax,0x0305
    int 0x16
    cmp byte [ui_active],1
    jne .font
    call app_palette_apply
.font:
    cmp byte [app_settings+57],0         ; the system font file name
    je .scheme
    mov si,app_settings_font_dir
    mov di,app_text
    call copy_z_to_di                  ; DI stays on the NUL
    mov si,app_settings+57
    call copy_z_to_di
    mov byte [app_args+10],0
    call app_s_font
.scheme:
    mov al,[app_settings+72]
    call ui_set_cursor_scheme
    jmp .redraw
.fallback:
    xor al,al
    call ui_set_cursor_scheme
.redraw:
    mov byte [ui_full_redraw],1
.done:
    popad
    ret

; A picture of 8-bit palette indices in module memory: value = its segment,
; value2 = its offset, value3 = the row stride (bits 0-11) and the zoom (bits
; 12-15, 0 = 1). Drawn at x, y: w x h source pixels, each zoom x zoom. Rows
; outside the compositor band are skipped; equal pixels merge into runs.
app_s_bitmap:
    push fs
    mov fs,ax
    mov ax,[app_args+14]
    mov bp,ax
    shr ax,12
    jnz .zoom
    inc ax
.zoom:
    mov [app_bm_zoom],ax
    and bp,0x0FFF
    mov [app_bm_stride],bp
    mov di,[app_args+12]
    mov [app_bm_x],bx
    mov [app_bm_w],cx
.row:
    test si,si
    jz .done
    cmp byte [ui_composing],1
    jne .visible
    mov ax,dx
    add ax,[app_bm_zoom]
    cmp ax,[ui_comp_top]
    jle .next_row
    cmp dx,[ui_comp_bottom]
    jge .next_row
.visible:
    push si
    push di
    mov si,di
    mov bx,[app_bm_x]
    mov cx,[app_bm_w]
.pixel:
    jcxz .row_done
    mov al,[fs:si]
    inc si
    mov bp,1
.merge:
    cmp bp,cx
    jae .span
    cmp al,[fs:si]
    jne .span
    inc si
    inc bp
    jmp .merge
.span:
    sub cx,bp
    push cx
    push si
    cmp byte [ui_vbe],1
    je .colour
    cmp al,16
    jb .colour
    cmp al,16+CIUKI_LOGO_COLOR_COUNT+UI_ICONS_COLOR_COUNT
    jb .fallback
    xor al,al
    jmp .colour
.fallback:
    movzx si,al
    sub si,16
    mov al,[ui_icons_fallback+si]
.colour:
    mov cx,bp
    imul cx,[app_bm_zoom]
    mov si,[app_bm_zoom]
    call ui_rect
    add bx,cx
    pop si
    pop cx
    jmp .pixel
.row_done:
    pop di
    pop si
.next_row:
    add di,[app_bm_stride]
    add dx,[app_bm_zoom]
    dec si
    jmp .row
.done:
    pop fs
    ret

; value = 1: a module changed files. AX = the count of such changes (and of
; DOS programs ended): views refresh when it moves, without polling disks
; while DOS programs run in their VMs.
app_s_changes:
    add [app_changes],ax
    mov ax,[app_changes]
    ret

; text = 48 bytes in the module (not a string): the 16 desktop colours,
; shown at once (the Control Panel's preview). DESKTOP.CFG is unchanged.
app_s_palette:
    push ds
    mov ds,[app_caller]
    mov si,[cs:app_args+8]
    mov di,ui_palette
    mov cx,48
    cld
    rep movsb
    pop ds
    mov byte [ui_full_redraw],1
app_palette_apply:
    pushad
    push es
    push cs
    pop es
    mov dx,ui_palette
    xor bx,bx
    mov cx,16
    cmp byte [ui_vbe],1
    jne .vga
    mov cx,VC_PALETTE_COLORS
    call vc_set_palette
    jmp .done
.vga:
    mov ax,0x1012
    call vc_bios
.done:
    pop es
    popad
    ret

; Run dialog: "notepad [file]", "explorer [folder]" (or "files") and
; "taskmgr" open the desktop applications, as in Windows. CF=1 when handled.
app_run_builtin:
    pushad
    mov si,app_builtin_names
.next:
    lodsb                              ; window, then the name
    test al,al
    jz .none
    mov bl,al
    mov di,ui_run_text
.compare:
    lodsb
    test al,al
    jz .end_of_name
    mov ah,[di]
    cmp ah,'a'
    jb .upper
    cmp ah,'z'
    ja .upper
    sub ah,32
.upper:
    inc di
    cmp al,ah
    je .compare
.skip:
    lodsb
    test al,al
    jnz .skip
    jmp .next
.end_of_name:
    mov al,[di]
    test al,al
    jz .match
    cmp al,' '
    jne .skip_after
    inc di
.match:
    push bx
    push di
    call ui_windows_close              ; the Run window
    pop si
    pop bx
    call app_open
    popad
    stc
    ret
.skip_after:
    jmp .next
.none:
    popad
    clc
    ret

; Hotkey AL from the intercept -> UI action (ui_event's contract).
app_hotkey_action:
    cmp al,0x5B
    je .start
    cmp al,0x13                        ; Win+R
    je .run
    cmp al,0x12                        ; Win+E
    je .files
    cmp al,0x20                        ; Win+D, Win+M
    je .desktop
    cmp al,0x32
    je .desktop
    cmp al,0x53                        ; Ctrl+Alt+Del
    je .tasks
    cmp al,0x3B                        ; Win+F1
    je .about
    cmp al,0x0F                        ; Win+Tab
    je ui_windows_cycle
    cmp al,0x5D                        ; Menu key, as Shift+F10
    jne .none
    mov ax,0x5D00
    call app_key
    jc .done
.none:
    xor ax,ax
.done:
    ret
.start:
    mov ax,1
    ret
.run:
    mov ax,4
    ret
.files:
    mov ax,2
    ret
.tasks:
    mov ax,23
    ret
.about:
    mov ax,6
    ret
.desktop:
    ; Show the desktop: every window minimized, Programs hidden.
    push bx
    mov bx,1
.minimize:
    cmp byte [ui_window_flags+bx],1
    jne .next
    mov byte [ui_window_flags+bx],2
.next:
    inc bx
    cmp bx,UI_WINDOW_COUNT
    jb .minimize
    pop bx
    mov byte [ui_window_visible],0
    mov byte [ui_bar_dirty],1
    mov byte [ui_full_redraw],1
    call ui_windows_unfocus
    mov si,app_desktop_marker
    call ui_serial
    mov ax,250
    ret

; Windows key, Menu key and Ctrl+Alt+Del through the BIOS keyboard intercept
; (INT 15h AH=4Fh): the BIOS never reports them. Win+key and the others end
; in app_hotkey (Win alone 5Bh, Menu 5Dh, Ctrl+Alt+Del 53h, Win+key its make
; code); ui_event takes it.
app_keys_begin:
    cmp word [app_old15+2],0
    jne .done
    push es
    mov ax,0x3515
    int 0x21
    mov [app_old15],bx
    mov [app_old15+2],es
    pop es
    mov dx,app_int15
    mov ax,0x2515
    int 0x21
.done:
    ret

app_keys_end:
    cmp word [app_old15+2],0
    je .done
    push ds
    lds dx,[app_old15]
    mov ax,0x2515
    int 0x21
    pop ds
    mov word [app_old15+2],0
.done:
    ret

app_int15:
    cmp ah,0x4F
    jne .chain
    cmp al,0xE0
    jne .code
    mov byte [cs:app_e0],1
    jmp .chain
.code:
    push bx
    mov bl,al
    and bl,0x7F
    mov bh,[cs:app_e0]
    mov byte [cs:app_e0],0
    test bh,bh
    jz .plain
    cmp bl,0x5B
    je .win
    cmp bl,0x5C
    je .win
    cmp bl,0x5D
    jne .plain
    test al,0x80
    jnz .consume
    mov byte [cs:app_hotkey],0x5D
    jmp .consume
.win:
    test al,0x80
    jnz .win_up
    cmp byte [cs:app_win],0
    jne .consume
    mov byte [cs:app_win],1
    jmp .consume
.win_up:
    cmp byte [cs:app_win],1
    jne .win_clear
    mov byte [cs:app_hotkey],0x5B
.win_clear:
    mov byte [cs:app_win],0
    jmp .consume
.plain:
    cmp byte [cs:app_win],0
    je .ctrl_alt_del
    test al,0x80
    jnz .consume
    mov [cs:app_hotkey],al
    mov byte [cs:app_win],2
    jmp .consume
.ctrl_alt_del:
    cmp al,0x53
    jne .pass
    push ds
    push 0x40
    pop ds
    mov bl,[0x17]
    pop ds
    and bl,0x0C
    cmp bl,0x0C
    jne .pass
    mov byte [cs:app_hotkey],0x53
.consume:
    pop bx
    clc
    retf 2
.pass:
    pop bx
.chain:
    jmp far [cs:app_old15]

; Idle accounting for the Tasks performance view: of each 18 BIOS ticks,
; those that ended a desktop HLT wait were idle. app_idle_tick runs before
; the HLT, app_idle_woke after it.
app_idle_tick:
    push es
    push ax
    push 0x40
    pop es
    mov ax,[es:0x6C]
    cmp ax,[app_idle_last]
    je .done
    mov [app_idle_last],ax
    inc byte [app_idle_ticks]
    cmp byte [app_idle_ticks],18
    jb .done
    push bx
    mov al,[app_idle_hlt]
    mov bl,100
    mul bl
    mov bl,18
    div bl
    pop bx
    cmp al,100
    jbe .store
    mov al,100
.store:
    mov [app_idle_percent],al
    mov byte [app_idle_ticks],0
    mov byte [app_idle_hlt],0
.done:
    pop ax
    pop es
    ret

app_idle_woke:
    push es
    push ax
    push 0x40
    pop es
    mov ax,[es:0x6C]
    cmp ax,[app_idle_last]
    je .done
    inc byte [app_idle_hlt]
.done:
    pop ax
    pop es
    ret

app_main db 8,9,12,APP_WIN_DESKTOP,13,14,15,16,11,17,APP_WIN_DISPLAY,2
app_paths dw app_path_files,app_path_tasks,app_path_notepad,app_path_desktop,app_path_control,app_path_devices,app_path_recycle,app_path_paint,app_path_dosvm,app_path_browser,app_path_display,app_path_about
app_path_files db '\SYSTEM\APPS\FILES.APP',0
app_path_tasks db '\SYSTEM\APPS\TASKS.APP',0
app_path_notepad db '\SYSTEM\APPS\CIUKNOTE.APP',0
app_path_desktop db '\SYSTEM\APPS\DESKTOP.APP',0
app_path_control db '\SYSTEM\APPS\CONTROL.APP',0
app_path_devices db '\SYSTEM\APPS\DEVICES.APP',0
app_path_recycle db '\SYSTEM\APPS\RECYCLE.APP',0
app_path_paint db '\SYSTEM\APPS\PAINT.APP',0
app_path_dosvm db '\SYSTEM\APPS\DOSVM.APP',0
app_path_browser db '\SYSTEM\APPS\BROWSER.APP',0
app_path_about db '\SYSTEM\APPS\ABOUT.APP',0
app_path_display db '\SYSTEM\APPS\DISPLAY.APP',0
app_settings_path db '\SYSTEM\UI\DESKTOP.CFG',0
app_settings_font_dir db '\SYSTEM\FONTS\',0
; Each window's module slot (FFh: the shell's), title offset in that module
; (0: its header title), style (bit 0: dialog frame).
app_win_slot db 0xFF,0xFF,11,0xFF,0xFF,0xFF,0xFF,0xFF,0,1,0xFF,8,2,4,5,6,7,9,10
             times UI_WINDOW_COUNT-19 db 0xFF
app_win_title times UI_WINDOW_COUNT dw 0
app_win_style times UI_WINDOW_COUNT db 0
app_ev_window db 0xFF
app_ev_context dw 0
app_cur_slot dw 0
app_draw_style db 0
app_overlay db 0
app_overlay_buttons db 0
app_changes dw 0
app_bm_zoom dw 1
app_bm_stride dw 0
app_bm_x dw 0
app_bm_w dw 0
app_alt_name times 64 db 0              ; the file in the alternate font slot
app_pointer_event dw APP_EV_MOUSE
app_hover_last db 0xFF
app_overlay_dirty dw 0
app_desktop_focus db 0             ; the desktop has the keyboard
app_action_code db 0
app_alt_font dw 0
APP_SETTINGS_BYTES equ 74                 ; CUI1, colours, mouse, keyboard, font, cursor scheme
app_settings times APP_SETTINGS_BYTES db 0
app_set_mouse db 2,0,9                  ; speed (2 = normal), swap, double-click
app_max_label db 2,0
app_desktop_marker db '[DESKTOP] SHOW DESKTOP',13,10,0
app_builtin_names db 12,'CIUKNOTE',0,12,'NOTEPAD',0,16,'CIUKPAINT',0,16,'MSPAINT',0,16,'PAINT',0,8,'EXPLORER',0,8,'FILES',0,9,'TASKMGR',0,13,'CONTROL',0,14,'DEVMGMT',0,18,'DISPLAY',0,17,'CIUKWEB',0,17,'BROWSER',0,2,'ABOUT',0,0
app_media_floppy db 'media:1',0
app_media_usb db 'media:2',0
app_media_cd db 'media:3',0
app_segs times APP_COUNT dw 0
app_far dw 0,0
app_shell_sp dw 0
app_module_ss dw 0
app_module_sp dw 0
app_service_ax dw 0
app_load_slot dw 0
app_open_id dw 0
app_open_slot dw 0
app_new_seg dw 0
app_caller dw 0
app_request db 0                       ; 1 close the window, 2 run app_command
app_open_window db 0
app_close_window db 0
app_busy db 0
app_capture db 0                        ; window+1 whose module has the left button
app_previous_right db 0
app_e0 db 0
app_win db 0
app_hotkey db 0
app_idle_last dw 0
app_idle_ticks db 0
app_idle_hlt db 0
app_idle_percent db 0
app_glyph db 0,0
app_cell dw 8
app_old15 dw 0,0
app_saved_x times UI_WINDOW_COUNT dw 0
app_saved_y times UI_WINDOW_COUNT dw 0
app_saved_w times UI_WINDOW_COUNT dw 0
app_saved_h times UI_WINDOW_COUNT dw 0
app_state times APP_COUNT*APP_STATE_BYTES db 0
app_header times 0x16 db 0
app_args times 8 dw 0
app_text times 160 db 0
app_title times 64 db 0
app_command times 128 db 0
app_open_arg times 128 db 0
