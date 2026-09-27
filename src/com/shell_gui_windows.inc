%define UI_WINDOW_COUNT 12
; Independent native windows. DOS children still run exclusively, with this
; shell's complete window state retained while their process owns the display.
ui_windows_resume:
    ; ui_dialog is cleared for DOS dispatch. Restore the actual focused window
    ; without changing z-order or activating an invisible Programs window.
    push bx
    movzx bx,byte [ui_active_window]
    test bx,bx
    jnz .dialog
    cmp byte [ui_window_visible],1
    je .visible
    jmp .hidden
.dialog:
    cmp byte [ui_window_flags+bx],1
    je .visible
.hidden:
    pop bx
    jmp ui_windows_unfocus
.visible:
    mov [ui_dialog],bl
    pop bx
    ret

ui_library_focus_clamp:
    ; The narrow Programs layout omits its four sidebar buttons. Keep both
    ; active and saved focus on a visible control when that layout is entered.
    cmp word [ui_ww],680
    jae .done
    push ax
    mov ax,[ui_window_focus]
    call .visible
    mov [ui_window_focus],ax
    cmp byte [ui_active_window],0
    jne .restored
    mov ax,[ui_focus]
    call .visible
    mov [ui_focus],ax
.restored:
    pop ax
.done:
    ret
.visible:
    cmp ax,2
    je .reset
    cmp ax,3
    je .reset
    cmp ax,8
    je .reset
    cmp ax,9
    jne .valid
.reset:
    mov ax,44
.valid:
    ret

ui_windows_open:
    pushad
    call ui_sfx_notice
    movzx bx,byte [ui_dialog]
    test bx,bx
    jz .raise
    cmp byte [ui_window_flags+bx],0
    je .new
    mov byte [ui_window_flags+bx],1
    jmp .raise
.new:
    mov byte [ui_window_flags+bx],1
    mov byte [ui_bar_dirty],1  ; a task button may appear
    mov di,bx
    shl di,1
    mov ax,[ui_default_focus+di]
    mov [ui_window_focus+di],ax
    mov ax,[ui_width]
    sub ax,[ui_window_width+di]
    shr ax,1
    mov dx,bx
    shl dx,3
    add ax,dx
    mov [ui_window_x+di],ax
    mov ax,[ui_height]
    sub ax,[ui_window_height+di]
    shr ax,1
    add ax,dx
    mov [ui_window_y+di],ax
.raise:
    call ui_windows_clamp
    call ui_windows_raise
    popad
    ret

; BX = window number, zero is Programs. Preserve geometry and open contents.
ui_windows_raise:
    pushad
    call ui_damage_active
    call ui_windows_save_focus
    mov [ui_active_window],bl
    mov [ui_dialog],bl
    mov si,bx
    shl si,1
    mov ax,[ui_window_focus+si]
    mov [ui_focus],ax
    xor si,si
.find:
    cmp [ui_window_order+si],bl
    je .found
    inc si
    cmp si,UI_WINDOW_COUNT
    jb .find
    jmp .done
.found:
    cmp si,UI_WINDOW_COUNT-1
    jae .top
    mov al,[ui_window_order+si+1]
    mov [ui_window_order+si],al
    inc si
    jmp .found
.top:
    mov [ui_window_order+UI_WINDOW_COUNT-1],bl
.done:
    call ui_damage_active
    call ui_motion_start
    popad
    ret

; The taskbar depends only on which windows exist (ui_windows_taskbar never
; marks focus), so raising, focusing and minimizing leave it untouched.
ui_windows_close:
    pushad
    movzx bx,byte [ui_active_window]
    cmp bx,11
    jne .files
    call dw_release
    jc .retained
.files:
    cmp bx,8
    jne .close
    call fm_end
.close:
    mov byte [ui_window_flags+bx],0
    mov byte [ui_bar_dirty],1
    popad
    jmp ui_windows_unfocus
.retained:
    popad
    stc
    ret
ui_windows_unfocus:
    pushad
    call ui_damage_active
    call ui_windows_save_focus
    mov si,UI_WINDOW_COUNT-1
.next:
    movzx bx,byte [ui_window_order+si]
    test bx,bx
    jnz .dialog_visible
    cmp byte [ui_window_visible],1
    je .found
    jmp .skip
.dialog_visible:
    cmp byte [ui_window_flags+bx],1
    je .found
.skip:
    dec si
    jns .next
    xor bx,bx
.found:
    mov [ui_active_window],bl
    mov [ui_dialog],bl
    mov di,bx
    shl di,1
    mov ax,[ui_window_focus+di]
    mov [ui_focus],ax
    call ui_damage_active
    call ui_motion_start
    popad
    ret

ui_windows_save_focus:
    push ax
    push di
    movzx di,byte [ui_active_window]
    shl di,1
    mov ax,[ui_focus]
    mov [ui_window_focus+di],ax
    pop di
    pop ax
    ret

ui_windows_draw:
    mov byte [ui_window_paint],0
.next:
    movzx si,byte [ui_window_paint]
    movzx bx,byte [ui_window_order+si]
    mov [ui_dialog],bl
    inc bl
    mov [ui_hit_owner],bl
    dec bl
    test bx,bx
    jnz .dialog
    cmp byte [ui_window_visible],1
    jne .advance
    call ui_draw_window
    jmp .advance
.dialog:
    cmp byte [ui_window_flags+bx],1
    jne .advance
    cmp bx,8
    jne .standard_dialog
    call fm_draw
    jmp .advance
.standard_dialog:
    cmp bx,11
    jne .regular
    call dw_draw
    jmp .advance
.regular:
    call ui_draw_dialog
.advance:
    inc byte [ui_window_paint]
    cmp byte [ui_window_paint],UI_WINDOW_COUNT
    jb .next
    mov al,[ui_active_window]
    mov [ui_dialog],al
    mov byte [ui_hit_owner],0
    ret

; Hit IDs include their owner. A body hit prevents clicks passing through a
; window to controls hidden underneath; title clicks raise then start a drag.
ui_windows_hit:
    mov [ui_hit_target],ah
    xor ah,ah
    ret

ui_windows_cycle:
    push bx
    push si
    mov si,UI_WINDOW_COUNT-1
.next:
    movzx bx,byte [ui_window_order+si]
    cmp bl,[ui_active_window]
    je .skip
    test bx,bx
    jz .programs
    cmp byte [ui_window_flags+bx],1
    je .found
    jmp .skip
.programs:
    cmp byte [ui_window_visible],1
    je .found
.skip:
    dec si
    jns .next
    xor ax,ax
    jmp .done
.found:
    call ui_windows_raise
    mov ax,250
.done:
    pop si
    pop bx
    ret

ui_windows_title_color:
    mov al,[ui_dialog]
    cmp al,[ui_active_window]
    mov al,1
    je .done
    mov al,8
.done:
    ret

ui_windows_taskbar:
    pushad
    xor cx,cx
    xor bp,bp
.count:
    movzx si,byte [ds:ui_task_windows+bp]
    cmp byte [ui_window_flags+si],0
    je .skip_count
    inc cx
.skip_count:
    inc bp
    cmp bp,8
    jb .count
    jcxz .done
    mov ax,[ui_width]
    sub ax,397
    xor dx,dx
    div cx
    cmp ax,76
    jbe .sized
    mov ax,76
.sized:
    mov [ui_task_button_width],ax
    mov dx,[ui_height]
    sub dx,28
    mov bx,247
    xor bp,bp
.next:
    movzx si,byte [ds:ui_task_windows+bp]
    cmp byte [ui_window_flags+si],0
    je .advance
    mov ax,si
    add ax,200
    mov di,bp
    shl di,1
    mov di,[ui_task_labels+di]
    mov cx,[ui_task_button_width]
    sub cx,4
    mov si,24
    call ui_button
    add bx,[ui_task_button_width]
.advance:
    inc bp
    cmp bp,8
    jb .next
.done:
    popad
    ret

ui_windows_clamp:
    pushad
    mov si,2
.next:
    mov ax,[ui_width]
    sub ax,[ui_window_width+si]
    sub ax,8
    cmp [ui_window_x+si],ax
    jbe .x_max
    mov [ui_window_x+si],ax
.x_max:
    cmp word [ui_window_x+si],4
    jae .x_min
    mov word [ui_window_x+si],4
.x_min:
    mov ax,[ui_height]
    sub ax,[ui_window_height+si]
    sub ax,37
    cmp [ui_window_y+si],ax
    jbe .y_max
    mov [ui_window_y+si],ax
.y_max:
    cmp word [ui_window_y+si],32
    jae .y_min
    mov word [ui_window_y+si],32
.y_min:
    add si,2
    cmp si,UI_WINDOW_COUNT*2
    jb .next
    popad
    ret

; Keep normal UI actions local to the affected windows. The taskbar is
; presented separately: unioning it with a title would span a 2K desktop.
ui_damage_active:
    pushad
    movzx bx,byte [ui_active_window]
    call ui_window_bounds
    add cx,4
    add si,5
    call ui_comp_damage
    popad
    ret

; BX window ID -> BX/DX position, CX/SI client-frame extent.
ui_window_bounds:
    test bx,bx
    jnz .dialog
    mov bx,[ui_wx]
    mov dx,[ui_wy]
    mov cx,[ui_ww]
    mov si,[ui_wh]
    ret
.dialog:
    shl bx,1
    mov dx,[ui_window_y+bx]
    mov cx,[ui_window_width+bx]
    mov si,[ui_window_height+bx]
    mov bx,[ui_window_x+bx]
    ret

; A small travelling title accent gives focus/open feedback in four BIOS
; ticks. It never delays input, allocates a surface, or animates a full frame.
; Completing a transition repaints the same 96x2 strip to restore its pixels.
ui_motion_start:
    push ax
    push es
    mov ax,0x40
    mov es,ax
    mov ax,[es:0x6C]
    mov [ui_motion_tick],ax
    mov byte [ui_motion_phase],4
    pop es
    pop ax
    ret

ui_motion_bounds:
    movzx bx,byte [ui_active_window]
    call ui_window_bounds
    add bx,10
    add dx,25
    mov cx,96
    mov si,2
    ret

ui_motion_draw:
    cmp byte [ui_motion_phase],0
    je .done
    cmp byte [ui_active_window],0
    jne .visible
    cmp byte [ui_window_visible],1
    jne .done
.visible:
    pushad
    call ui_motion_bounds
    movzx ax,byte [ui_motion_phase]
    neg ax
    add ax,4
    imul ax,24
    add bx,ax
    mov cx,24
    mov al,10
    call ui_rect
    popad
.done:
    ret

ui_motion_poll:
    cmp byte [ui_motion_phase],0
    je .done
    cmp byte [ui_active_window],0
    jne .visible
    cmp byte [ui_window_visible],1
    je .visible
    mov byte [ui_motion_phase],0
    ret
.visible:
    pushad
    push es
    mov ax,0x40
    mov es,ax
    mov ax,[es:0x6C]
    cmp ax,[ui_motion_tick]
    je .restore
    mov [ui_motion_tick],ax
    dec byte [ui_motion_phase]
    push cs
    pop es
    call ui_motion_bounds
    call ui_comp_damage
    call ui_draw
.restore:
    pop es
    popad
.done:
    ret

ui_task_windows db 1,2,5,6,8,9,10,11
ui_task_labels dw ui_task_run,ui_task_info,ui_task_video,ui_task_audio,ut_files,ui_task_tasks,ui_task_bg,ut_dos
ui_task_run db 'Run',0
ui_task_info db 'Info',0
ui_task_video db 'Video',0
ui_task_audio db 'Audio',0
ui_task_tasks db 'Tasks',0
ui_task_bg db 'BG',0
ui_task_button_width dw 72
ui_window_titles dw ut_programs,ut_run,ut_about,ut_power,ut_launch_error,ut_display,ut_sound,ut_brand,ut_files,ut_tasks,ut_wallpaper,ut_dos_window
ui_window_flags times UI_WINDOW_COUNT db 0
ui_default_focus dw 44,40,41,41,41,50,55,1,100,160,183,18
ui_window_focus dw 44,40,41,41,41,50,55,1,100,160,183,18
ui_window_width dw 466,466,466,466,466,466,466,466,FM_WIDTH,466,466,660
ui_window_height dw 226,226,226,226,226,226,226,226,FM_HEIGHT,226,226,460
ui_window_x times UI_WINDOW_COUNT dw 0
ui_window_y times UI_WINDOW_COUNT dw 0
ui_window_order db 0,1,2,3,4,5,6,7,8,9,10,11
ui_active_window db 0
ui_window_paint db 0
ui_hit_owner db 0
ui_hit_target db 0
ui_window_drag db 0
ui_window_repaint db 0
ui_full_redraw db 1
ui_bar_dirty db 0
ui_motion_phase db 0
ui_motion_tick dw 0
ui_button_down db 0
ui_press_owner db 0
ui_last_hit_ptr dw 0
ui_press_x dw 0
ui_press_y dw 0
ui_press_width dw 0
ui_press_height dw 0
