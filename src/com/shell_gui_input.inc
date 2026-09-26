; Nonblocking keyboard/mouse events. The renderer alone owns cursor pixels.
ui_event:
    mov ah,0x11
    int 0x16
    jz .mouse
    mov ah,0x10
    int 0x16
    cmp ah,0xA5             ; enhanced BIOS Alt+Tab, retained after Alt release
    je ui_windows_cycle
    cmp byte [ui_dialog],8
    jne .task_key
    call fm_key
    jc .out
.task_key:
    call ui_task_manager_key
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
    cmp byte [ui_dialog],1
    je ui_run_key
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
    jz .escape_window
    mov ax,23                 ; Ctrl+Shift+Esc opens native Tasks
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
    mov ah,5
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
    mov [ui_mouse_buttons],bx
    ; Own signed motion coordinates. The shared driver's absolute position
    ; can wrap at zero; consuming raw mickeys keeps this GUI independent.
    mov ax,0x000B
    int 0x33
    movsx ecx,cx
    movsx edx,dx
    shl ecx,1
    shl edx,1
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
    sub eax,24
    cmp ecx,eax
    jbe .raw_x_max
    mov ecx,eax
.raw_x_max:
    mov ax,[ui_height]
    movzx eax,ax
    sub eax,16
    cmp edx,eax
    jbe .raw_y_max
    mov edx,eax
.raw_y_max:
    cmp cx,[ui_mouse_x]
    jne .move
    cmp dx,[ui_mouse_y]
    je .click
.move:
    call ui_pointer_hide
    test cx,cx
    jns .positive_x
    xor cx,cx
.positive_x:
    test dx,dx
    jns .positive_y
    xor dx,dx
.positive_y:
    mov ax,[ui_width]
    sub ax,24
    cmp cx,ax
    jbe .x
    mov cx,ax
.x:
    mov ax,[ui_height]
    sub ax,16
    cmp dx,ax
    jbe .y
    mov dx,ax
.y:
    ; Keep the driver position synchronized: no hidden travel beyond a border.
    push bx
    mov ax,4
    int 0x33
    pop bx
    mov [ui_mouse_x],cx
    mov [ui_mouse_y],dx
    call ui_pointer_show
    cmp byte [ui_dragging],0
    jne .drag
.click:
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
    je .focus_ready
    call ui_windows_raise
    mov byte [ui_window_repaint],1
.focus_ready:
    pop ax
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
    mov ax,[ui_mouse_x]
    mov [ui_drag_dx],ax
    mov ax,[ui_mouse_y]
    mov [ui_drag_dy],ax
    mov ax,[ui_ww]
    mov [ui_resize_ww],ax
    mov ax,[ui_wh]
    mov [ui_resize_wh],ax
    jmp .idle
.release:
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
    call ui_pointer_show
    jmp .idle
.drag:
    test word [ui_mouse_buttons],1
    jz .drag_end
    call ui_comp_damage_drag
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
    mov ax,[ui_mouse_x]
    sub ax,[ui_drag_dx]
    add ax,[ui_resize_ww]
    cmp ax,510
    jge .resize_w_min
    mov ax,510
.resize_w_min:
    mov bx,[ui_width]
    sub bx,[ui_wx]
    sub bx,12
    cmp ax,bx
    jle .resize_w_max
    mov ax,bx
.resize_w_max:
    mov [ui_ww],ax
    mov ax,[ui_mouse_y]
    sub ax,[ui_drag_dy]
    add ax,[ui_resize_wh]
    cmp ax,370
    jge .resize_h_min
    mov ax,370
.resize_h_min:
    mov bx,[ui_height]
    sub bx,[ui_wy]
    sub bx,36
    cmp ax,bx
    jle .resize_h_max
    mov ax,bx
.resize_h_max:
    mov [ui_wh],ax
    call ui_library_focus_clamp
    call ui_comp_damage_drag
    mov ax,250
    ret
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

ui_run_key:
    cmp al,13
    je .enter
    cmp al,27
    je .cancel
    cmp al,9
    je ui_focus_next
    cmp al,8
    je .backspace
    test al,al
    jz .editing
    cmp al,0xE0
    jne .printable
.editing:
    cmp ah,0x4B
    je .left
    cmp ah,0x4D
    je .right
    cmp ah,0x47
    je .home
    cmp ah,0x4F
    je .end
    cmp ah,0x53
    je .delete
.printable:
    cmp al,32
    jb .idle
    cmp al,126
    ja .idle
    cmp byte [ui_run_len],95
    jae .idle
    mov dl,al
    movzx bx,byte [ui_run_len]
    movzx si,byte [ui_run_cursor]
.shift:
    mov al,[ui_run_text+bx]
    mov [ui_run_text+bx+1],al
    cmp bx,si
    je .insert
    dec bx
    jmp .shift
.insert:
    mov [ui_run_text+si],dl
    inc byte [ui_run_len]
    inc byte [ui_run_cursor]
    jmp .draw
.enter:
    mov ax,[ui_focus]
    cmp ax,17
    je .out
    cmp ax,18
    je .out
    cmp ax,41
    je .out
    mov ax,40
.out:
    ret
.cancel:
    mov ax,41
    ret
.backspace:
    cmp byte [ui_run_cursor],0
    je .idle
    dec byte [ui_run_cursor]
.delete:
    mov al,[ui_run_cursor]
    cmp al,[ui_run_len]
    jae .idle
    movzx bx,al
.remove:
    mov al,[ui_run_text+bx+1]
    mov [ui_run_text+bx],al
    inc bx
    test al,al
    jnz .remove
    dec byte [ui_run_len]
    jmp .draw
.left:
    cmp byte [ui_run_cursor],0
    je .idle
    dec byte [ui_run_cursor]
    jmp .draw
.right:
    mov al,[ui_run_cursor]
    cmp al,[ui_run_len]
    jae .idle
    inc byte [ui_run_cursor]
    jmp .draw
.home:
    mov byte [ui_run_cursor],0
    jmp .draw
.end:
    mov al,[ui_run_len]
    mov [ui_run_cursor],al
.draw:
    mov ax,251
    ret
.idle:
    xor ax,ax
    ret

ui_pointer_hide:
    cmp byte [ui_pointer_visible],1
    jne .done
    cmp byte [ui_vbe],0
    je ui_vga_pointer_hide
    pushad
    mov edi,[ui_pointer_linear]
    mov si,ui_pointer_under
    call vc_pointer_restore
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
    movzx edi,word [ui_mouse_y]
    movzx eax,word [vc_pitch]
    imul edi,eax
    movzx eax,word [ui_mouse_x]
    movzx ecx,byte [vc_bytes]
    imul eax,ecx
    add edi,eax
    add edi,[ui_front_base]
    mov [ui_pointer_linear],edi
    mov si,ui_pointer_under
    call vc_pointer_save
    mov bx,ui_pointer_black
    call vc_pointer_draw
    mov byte [ui_pointer_visible],1
    popad
.done:
    ret

ui_vga_pointer_hide:
    cmp byte [ui_pointer_visible], 1
    jne .done
    pushad
    push es
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
    mov ah, 1
.plane:
    mov dx, 0x3C4
    mov al, 2
    out dx, ax
    push ax
    mov di, [ui_pointer_offset]
    mov bp, 16
.row:
    mov cx, 3
    rep movsb
    add di, 77
    dec bp
    jnz .row
    pop ax
    shl ah, 1
    cmp ah, 16
    jb .plane
    mov ax, 0x0F02
    out dx, ax
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
    mov ax, 0xA000
    mov es, ax
    mov ax, [ui_mouse_y]
    imul ax, 80
    mov di, [ui_mouse_x]
    shr di, 3
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
    mov bp, 16
.row:
    mov al, [es:di]
    mov [si], al
    mov al, [es:di+1]
    mov [si+1], al
    mov al, [es:di+2]
    mov [si+2], al
    add si, 3
    add di, 80
    dec bp
    jnz .row
    pop ax
    inc ah
    cmp ah, 4
    jb .plane
    mov bx, [ui_mouse_x]
    mov dx, [ui_mouse_y]
    mov si, ui_pointer_black
    xor al, al
    call ui_pointer_shape
    mov si, ui_pointer_white
    mov al, 15
    call ui_pointer_shape
    mov byte [ui_pointer_visible], 1
    pop es
    popad
.done:
    ret

ui_pointer_shape:
    pushad
    mov [ui_pointer_color], al
    mov bp, 16
.row:
    lodsw
    push bx
    mov di, 16
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
ui_mouse_buttons dw 0
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
ui_pointer_linear dd 0
ui_pointer_color db 0
ui_pointer_under times 24*16*4 db 0
ui_pointer_black dw 0x8000,0xC000,0xE000,0xF000,0xF800,0xFC00,0xFE00,0xFF00,0xFF80,0xFFC0,0xFE00,0xEF00,0xCF00,0x8780,0x0780,0x0300
ui_pointer_white dw 0,0,0x4000,0x6000,0x7000,0x7800,0x7C00,0x7E00,0x7F00,0x7C00,0x6C00,0x4600,0x0600,0x0300,0x0300,0
