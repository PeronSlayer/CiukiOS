; Desktop applications as modules. Files (window 8), Tasks (9) and Notepad
; (12) are separate programs in \SYSTEM\APPS (built from src/apps). The shell
; loads one into its own memory block and draws its window frame. It forwards
; paint, keys, the mouse (both buttons) and actions 100-149 of the hits it
; registered. The module draws through app_service. Contract: src/apps/app.h.
APP_COUNT equ 3
APP_EV_OPEN equ 1
APP_EV_PAINT equ 2
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
APP_ARG equ 0x140
APP_ARG_BYTES equ 128
APP_STATE_BYTES equ 80
APP_TITLE_H equ 30                     ; client area below the title rail

; BL = window -> SI = slot*2, CF=1 when it is no application window.
app_slot:
    push ax
    xor si,si
.next:
    cmp bl,[app_window_ids+si]
    je .found
    inc si
    cmp si,APP_COUNT
    jb .next
    pop ax
    stc
    ret
.found:
    shl si,1
    pop ax
    clc
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
    mov ah,0x48
    int 0x21
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
    mov ah,0x49
    int 0x21
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
    mov ah,0x49
    int 0x21
    mov word [app_segs+si],0
.done:
    pop es
    pop ax
    ret

; SI = slot*2, AX = event, DX/BX/CX arguments: the module's answer in AX.
; Its host block describes the window, pointer and keyboard first.
app_call:
    push es
    push di
    push bp
    mov es,[app_segs+si]
    push ax
    push bx
    mov di,APP_HOST
    shr si,1
    movzx bx,byte [app_window_ids+si]
    shl si,1
    shl bx,1
    mov ax,[ui_window_x+bx]
    stosw
    mov ax,[ui_window_y+bx]
    stosw
    mov ax,[ui_window_width+bx]
    stosw
    mov ax,[ui_window_height+bx]
    stosw
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
    shr bx,1
    xor ax,ax
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
    pop bx
    pop ax
    mov bp,[es:APP_HDR_ENTRY]
    mov [app_far],bp
    mov [app_far+2],es
    mov [app_shell_sp],sp
    call far [app_far]
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
app_close:
    push si
    push ax
    call app_slot
    jc .allow
    cmp word [app_segs+si],0
    je .allow
    mov ax,APP_EV_CLOSE
    call app_call
    test ax,ax
    jnz .keep
    call app_unload
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

; Before DOS/EXEC: each module saves what it needs to reopen in its state
; buffer and is unloaded, unless it must stay (unsaved work).
app_suspend:
    pushad
    push es
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
    call app_unload
.advance:
    add si,2
    cmp si,APP_COUNT*2
    jb .next
    pop es
    popad
    ret

; After DOS/EXEC: reopen the modules of the windows that stayed open.
app_resume:
    pushad
    xor si,si
.next:
    mov bl,[app_window_ids+si]
    xor bh,bh
    cmp byte [ui_window_flags+bx],0
    je .advance
    shl si,1
    cmp word [app_segs+si],0
    jne .loaded
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
.gone:
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
    xor si,si
.next:
    cmp word [app_segs+si],0
    je .advance
    mov ax,APP_EV_POLL
    call app_call
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
    mov bl,[app_window_ids+si]
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
    ret

; BL = window: its whole rectangle, shadow included, is repainted.
app_damage:
    pushad
    xor bh,bh
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

; The active window's module takes a key first. AX = BIOS key. CF=1 with
; AX = the UI action when it used the key.
app_key:
    push si
    push bx
    mov bl,[ui_active_window]
    call app_slot
    jc .unused
    cmp word [app_segs+si],0
    je .unused
    mov dx,ax
    mov ax,APP_EV_KEY
    call app_call
    test ax,ax
    jz .unused
    call app_after_action
    pop bx
    pop si
    stc
    ret
.unused:
    pop bx
    pop si
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
    mov ax,APP_EV_ACTION
    call app_call
    call app_after_action
    pop bx
    pop si
    ret
.none:
    xor ax,ax
    pop bx
    pop si
    ret

; DX = kind (1 left down, 2 move, 3 left up, 4 right down), window BL.
; The module gets the pointer relative to its client area (BX, CX).
; AX = UI action (250 when the module repaints).
app_mouse:
    push si
    push bx
    push cx
    push di
    call app_slot
    jc .none
    cmp word [app_segs+si],0
    je .none
    movzx di,bl
    shl di,1
    mov bx,[ui_mouse_x]
    sub bx,[ui_window_x+di]
    mov cx,[ui_mouse_y]
    sub cx,[ui_window_y+di]
    sub cx,APP_TITLE_H
    mov ax,APP_EV_MOUSE
    call app_call
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

; A module answer (AX: 0 nothing, 1 repaint) plus what it asked for through
; app_service meanwhile -> UI action in AX (249 with SI = command).
app_after_action:
    test ax,ax
    jz .requests
    mov ax,250
.requests:
    call app_after
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

; Window BL (ui_dialog), its frame and then the module's client area.
app_draw:
    pushad
    movzx di,bl
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
    mov bl,[ui_dialog]
    call app_slot
    push ds
    pop es
    mov di,app_title
    push ds
    mov ds,[app_segs+si]
    mov si,[APP_HDR_TITLE]
    mov cx,63
.title:
    lodsb
    stosb
    test al,al
    loopnz .title
    mov byte [es:di],0
    pop ds
    pop dx
    pop bx
    add bx,7
    add dx,4
    mov si,app_title
    mov ax,0x010F
    call ui_text
    ; Minimize, maximize, close.
    movzx di,byte [ui_dialog]
    shl di,1
    mov bx,[ui_window_x+di]
    add bx,[ui_window_width+di]
    sub bx,77
    mov dx,[ui_window_y+di]
    add dx,6
    U_BUTTON bx,dx,22,19,ut_min,17
    add bx,24
    U_BUTTON bx,dx,22,19,app_max_label,19
    add bx,24
    U_BUTTON bx,dx,22,19,ut_close,18
    ; Resize grip, bottom right.
    movzx di,byte [ui_dialog]
    shl di,1
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
    mov bl,[ui_dialog]
    call app_slot
    mov ax,APP_EV_PAINT
    call app_call
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
             dw app_s_idle,app_s_mono
APP_SVC_COUNT equ ($-app_services)/2

app_s_rect:
    jmp ui_rect
app_s_text:
    mov si,app_text
    jmp ui_text
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
    call ui_measure
    mov ax,cx
    ret
app_s_repaint:
    mov bl,[ui_active_window]
    jmp app_damage
app_s_command:
    mov si,app_text
    mov di,app_command
    call copy_z_to_di
    mov byte [app_request],2
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
; AL = window, value2 = 1 raise, 2 close, 3 minimize.
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
    cmp al,3
    jne .done
    test bx,bx
    jz .done
    mov byte [ui_window_flags+bx],2
    mov byte [ui_bar_dirty],1
.done:
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

app_window_ids db 8,9,12
app_paths dw app_path_files,app_path_tasks,app_path_notepad
app_path_files db '\SYSTEM\APPS\FILES.APP',0
app_path_tasks db '\SYSTEM\APPS\TASKS.APP',0
app_path_notepad db '\SYSTEM\APPS\NOTEPAD.APP',0
app_max_label db '^',0
app_desktop_marker db '[DESKTOP] SHOW DESKTOP',13,10,0
app_builtin_names db 12,'NOTEPAD',0,8,'EXPLORER',0,8,'FILES',0,9,'TASKMGR',0,0
app_media_floppy db 'media:1',0
app_media_usb db 'media:2',0
app_media_cd db 'media:3',0
app_segs dw 0,0,0
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
