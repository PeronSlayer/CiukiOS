; Restricted host presentation while one BIOS/DOS text child owns execution.
; The external runtime saves the complete interrupted CPU state, chains IRQ08
; through its BIOS EOI, guards reentry and calls on a private stack. This file
; never invokes DOS, BIOS, INT33, mode changes, allocations or normal ui_action.
; Near foreground lifecycle; FAR tick packet AX=dirty bit0, BX=buttons,
; CX/DX=absolute mouse. Result AX: focus bit0, close request bit1, dirty bit2.
%include "src/com/dos_window_abi.inc"
%define DOS_HOST_WINDOW 11

dos_host_prepare:
    pushad
    cmp byte [ui_vbe],1
    jne .fail
    cmp byte [vc_active],1
    jne .fail
    smsw ax
    test al,1
    jnz .v86
    cmp byte [vc_lfb],1         ; real mode: LFB bursts, as before
    jne .fail
    jmp .window_ok
.v86:
    ; A V86 monitor forbids the LFB burst's CR0 switch: only the banked
    ; window, which the runtime then serves through CVSESSION host mode.
    cmp byte [vc_lfb],0
    jne .fail
    cmp byte [vc_bank_ok],1
    jne .fail
.window_ok:
    cmp word [ui_comp_seg],0
    je .fail
    cmp word [ui_width],800
    jb .fail
    cmp word [ui_height],600
    jb .fail
    cmp byte [vc_fb_depth],0
    jne .fail
    cmp byte [dos_host_active],0
    jne .fail
    mov al,[ui_page_enabled]
    mov [dos_host_pages],al
    mov byte [ui_page_enabled],0
    mov byte [ui_page_pending],0
    mov byte [ui_motion_phase],0
    mov byte [dos_host_drag],255
    mov byte [dos_host_previous],0
    mov byte [dos_host_close],0
    mov word [dos_host_pressed],0
    mov byte [dos_host_active],1
    popad
    clc
    ret
.fail:
    popad
    stc
    ret

dos_host_finish:
    pushad
    cmp byte [dos_host_active],1
    jne .done
    mov byte [dos_host_active],0
    mov byte [dos_host_drag],255
    mov word [ui_window_width+DOS_HOST_WINDOW*2],660
    mov word [ui_window_height+DOS_HOST_WINDOW*2],460
    mov byte [ui_dragging],0
    mov byte [ui_previous_button],0
    mov byte [ui_button_down],0
    mov al,[dos_host_pages]
    mov [ui_page_enabled],al
    mov byte [ui_page_pending],0
    ; The hidden page was not maintained while the child ran. Its next use
    ; must compose the whole scene; the visible front page remains selected.
    mov word [ui_page_previous_left],0
    mov word [ui_page_previous_top],0
    mov ax,[ui_width]
    mov [ui_page_previous_right],ax
    mov ax,[ui_height]
    mov [ui_page_previous_bottom],ax
    mov byte [ui_full_redraw],1
.done:
    popad
    ret

dos_host_tick:
    pushf
    cmp byte [dos_host_active],1
    jne .inactive
    cmp byte [vc_active],1
    jne .inactive
    push ax
    smsw ax
    test al,1
    pop ax
    jz .real_mode
    ; V86: paint only while the runtime holds CVSESSION host mode, which
    ; serves this banked window from the protected LFB (no CR0, no firmware).
    cmp byte [es:DW_V86_HOST],1
    jne .inactive
    cmp byte [vc_lfb],0
    jne .inactive
    cmp byte [vc_bank_ok],1
    jne .inactive
    jmp .window_ok
.real_mode:
    cmp byte [vc_lfb],1
    jne .inactive
.window_ok:
    cmp byte [vc_fb_depth],0
    jne .inactive
    push cs
    pop ds
    mov [dos_host_module],es
    push cs
    pop es
    cld
    mov [dos_host_buttons],bx
    ; Consume this frame's dirty mask before enabling IRQs. The main child
    ; is suspended, but a chained INT1C handler can emit more virtual text
    ; during paint. Its new row bits must survive for the following frame.
    ; XCHG with memory is atomic; CX/DX mouse coordinates remain untouched.
    mov es,[dos_host_module]
    mov byte [es:DW_GFX_PAINTED],0
    mov bx,[es:DW_DIRTY_OFFSET]
    xor eax,eax
    xchg eax,[es:bx]
    mov [dos_host_dirty],eax
    push cs
    pop es
    ; IRQ08's previous handler has returned/EOIed and the module's busy
    ; flag blocks recursive painting. Leave keyboard/mouse IRQs responsive.
    sti
    call dos_host_mouse
    cmp byte [ui_window_flags+DOS_HOST_WINDOW],1
    jne .paint
    mov eax,[dos_host_dirty]
    and eax,0x01FFFFFF
    jz .paint
    bsf edx,eax
    bsr esi,eax
    sub si,dx
    inc si
    ; 25 dirty bands of ceil(client height/25) rows (16 at 640x400).
    mov ax,[ui_window_height+DOS_HOST_WINDOW*2]
    sub ax,60-24
    mov cl,25
    div cl
    movzx cx,al
    imul dx,cx
    imul si,cx
    add dx,[ui_window_y+DOS_HOST_WINDOW*2]
    add dx,34
    mov bx,[ui_window_x+DOS_HOST_WINDOW*2]
    add bx,10
    mov cx,[ui_window_width+DOS_HOST_WINDOW*2]
    sub cx,20
    call ui_comp_damage
.paint:
    cmp byte [ui_comp_dirty],1
    jne .pointer
    ; Deliberately bypass ui_draw/ui_comp_draw: those can flip pages with
    ; INT10 and recover a failed video mode using DOS allocation/free.
    mov eax,[ui_front_base]
    mov [ui_present_base],eax
    mov es,[dos_host_module]
    rdtsc
    mov [es:DW_PAINT_START_TSC],eax
    mov [es:DW_PAINT_START_TSC+4],edx
    push cs
    pop es
    call ui_comp_bands
    mov es,[dos_host_module]
    rdtsc
    sub eax,[es:DW_PAINT_START_TSC]
    sbb edx,[es:DW_PAINT_START_TSC+4]
    add [es:DW_PAINT_TSC],eax
    adc [es:DW_PAINT_TSC+4],edx
    inc dword [es:DW_PAINT_COUNT]
    mov byte [es:DW_GFX_PAINTED],1
    cmp edx,[es:DW_PAINT_MAX_TSC+4]
    jb .metric_done
    ja .metric_max
    cmp eax,[es:DW_PAINT_MAX_TSC]
    jbe .metric_done
.metric_max:
    mov [es:DW_PAINT_MAX_TSC],eax
    mov [es:DW_PAINT_MAX_TSC+4],edx
.metric_done:
    push cs
    pop es
    mov byte [ui_comp_dirty],0
.pointer:
    call ui_pointer_show
    mov es,[dos_host_module]
    mov bx,[es:DW_DIRTY_OFFSET]
    xor ax,ax
    cmp byte [es:DW_GFX_PAINTED],1
    jne .focus
    cmp byte [ui_window_flags+DOS_HOST_WINDOW],1
    jne .focus
    or al,8
.focus:
    cmp byte [ui_active_window],DOS_HOST_WINDOW
    jne .close
    cmp byte [ui_window_flags+DOS_HOST_WINDOW],1
    jne .close
    or al,1
.close:
    cmp byte [dos_host_close],0
    je .pending
    or al,2
    mov byte [dos_host_close],0
.pending:
    cmp dword [es:bx],0
    jne .more_damage
    cmp byte [ui_comp_dirty],0
    je .return
.more_damage:
    or al,4
.return:
    popf
    retf
.inactive:
    xor ax,ax
    popf
    retf

dos_host_damage_window:
    pushad
    call ui_window_bounds
    add cx,4
    add si,5
    call ui_comp_damage
    popad
    ret

dos_host_mouse:
    ; Treat the packet as unsigned coordinates; clamp before touching VRAM.
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
    cmp cx,[ui_mouse_x]
    jne .moved
    cmp dx,[ui_mouse_y]
    je .buttons
.moved:
    call ui_pointer_hide
    mov [ui_mouse_x],cx
    mov [ui_mouse_y],dx
.buttons:
    mov ax,[dos_host_buttons]
    and al,1
    cmp al,[dos_host_previous]
    je .held
    mov [dos_host_previous],al
    test al,al
    jz .release
    call ui_hit_find
    mov [dos_host_pressed],ax
    mov al,[ui_hit_target]
    mov [dos_host_owner],al
    test al,al
    jz .done
    dec al
    cmp al,UI_WINDOW_COUNT
    jae .done
    movzx bx,al
    call ui_windows_raise
    mov byte [ui_motion_phase],0
    mov ax,[dos_host_pressed]
    cmp ax,63                       ; DOS window grip (VGA session only)
    sete [dos_host_resize]
    je .drag_ready
    cmp ax,60
    je .start_drag
    cmp ax,62
    jne .done
.start_drag:
    test bx,bx
    jnz .drag_ready
    cmp byte [ui_maximized],1
    je .done
.drag_ready:
    mov [dos_host_drag],bl
    call ui_window_bounds
    cmp byte [dos_host_resize],0
    je .drag_origin
    mov bx,cx                       ; resize: pointer offset from the size
    mov dx,si
.drag_origin:
    mov ax,[ui_mouse_x]
    sub ax,bx
    mov [dos_host_dx],ax
    mov ax,[ui_mouse_y]
    sub ax,dx
    mov [dos_host_dy],ax
    ret
.held:
    test al,al
    jz .done
    cmp byte [dos_host_drag],255
    je .done
    movzx bx,byte [dos_host_drag]
    call dos_host_damage_window
    call ui_window_bounds
    cmp byte [dos_host_resize],0
    jne .resize
    mov ax,[ui_mouse_x]
    sub ax,[dos_host_dx]
    cmp ax,4
    jge .drag_x
    mov ax,4
.drag_x:
    mov bx,[ui_width]
    sub bx,cx
    sub bx,8
    cmp ax,bx
    jle .drag_x_end
    mov ax,bx
.drag_x_end:
    mov dx,[ui_mouse_y]
    sub dx,[dos_host_dy]
    cmp dx,32
    jge .drag_y
    mov dx,32
.drag_y:
    mov bx,[ui_height]
    sub bx,si
    sub bx,37
    cmp dx,bx
    jle .drag_y_end
    mov dx,bx
.drag_y_end:
    movzx bx,byte [dos_host_drag]
    test bx,bx
    jnz .native_position
    mov [ui_wx],ax
    mov [ui_wy],dx
    jmp .damage_new
.native_position:
    mov si,bx
    shl si,1
    mov [ui_window_x+si],ax
    mov [ui_window_y+si],dx
.damage_new:
    call dos_host_damage_window
    ret
.resize:
    ; Frame >= 340x260 (a 320x200 client) and inside the desktop work area.
    mov ax,[ui_mouse_x]
    sub ax,[dos_host_dx]
    cmp ax,340
    jge .resize_w
    mov ax,340
.resize_w:
    mov cx,[ui_width]
    sub cx,bx
    sub cx,8
    cmp ax,cx
    jle .resize_w_end
    mov ax,cx
.resize_w_end:
    mov [ui_window_width+DOS_HOST_WINDOW*2],ax
    mov ax,[ui_mouse_y]
    sub ax,[dos_host_dy]
    cmp ax,260
    jge .resize_h
    mov ax,260
.resize_h:
    mov cx,[ui_height]
    sub cx,dx
    sub cx,37
    cmp ax,cx
    jle .resize_h_end
    mov ax,cx
.resize_h_end:
    mov [ui_window_height+DOS_HOST_WINDOW*2],ax
    mov bx,DOS_HOST_WINDOW
    jmp .damage_new
.release:
    cmp byte [dos_host_drag],255
    je .activate
    mov byte [dos_host_drag],255
    ret
.activate:
    call ui_hit_find
    cmp ax,[dos_host_pressed]
    jne .done
    mov dl,[ui_hit_target]
    cmp dl,[dos_host_owner]
    jne .done
    cmp ax,200
    jb .window_control
    cmp ax,200+UI_WINDOW_COUNT
    jae .done
    sub ax,200
    mov bx,ax
    test bx,bx
    jz .done
    cmp byte [ui_window_flags+bx],0
    je .done
    mov byte [ui_window_flags+bx],1
    call ui_windows_raise
    mov byte [ui_motion_phase],0
    ret
.window_control:
    cmp ax,17
    je .minimize
    cmp ax,10
    je .minimize_programs
    cmp ax,18
    je .close
    cmp ax,12
    je .minimize_programs
    ret                     ; all application/file/settings actions disabled
.minimize:
    movzx bx,byte [ui_active_window]
    test bx,bx
    jz .done
    call dos_host_damage_window
    mov byte [ui_window_flags+bx],2
    call ui_windows_unfocus
    mov byte [ui_motion_phase],0
    ret
.minimize_programs:
    cmp byte [ui_active_window],0
    jne .done
    xor bx,bx
    call dos_host_damage_window
    mov byte [ui_window_visible],0
    call ui_windows_unfocus
    mov byte [ui_motion_phase],0
    ret
.close:
    cmp byte [ui_active_window],DOS_HOST_WINDOW
    jne .close_host
    mov byte [dos_host_close],1
    ret                     ; module requests normal guest exit, never kills
.close_host:
    ; Keep lifecycle-bearing host windows allocated. About has no resources
    ; and can close immediately; the other panels can only be minimized.
    cmp byte [ui_active_window],2
    jne .minimize
    mov bx,2
    call dos_host_damage_window
    mov byte [ui_window_flags+2],0
    call ui_windows_unfocus
    mov byte [ui_motion_phase],0
    xor bx,bx
    mov dx,[ui_height]
    sub dx,32
    mov cx,[ui_width]
    mov si,32
    call ui_comp_damage
.done:
    ret

dos_host_active db 0
dos_host_pages db 0
dos_host_drag db 255
dos_host_resize db 0
dos_host_previous db 0
dos_host_owner db 0
dos_host_close db 0
dos_host_pressed dw 0
dos_host_dx dw 0
dos_host_dy dw 0
dos_host_dirty dd 0
dos_host_buttons dw 0
dos_host_module dw 0
