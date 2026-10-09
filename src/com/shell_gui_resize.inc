; Eight sizing zones on the topmost visible normal frame. WS_DIALOG and
; maximized windows stay fixed. Bits: left=1, right=2, top=4, bottom=8.
ui_resize_hit:
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    cmp ax,61
    je .old_grip
    cmp ax,65
    jne .saved
.old_grip:
    mov ax,70                         ; stale grip of a fixed/maximized frame
.saved:
    push ax
    mov di,ax
    mov byte [ui_resize_edge],0
    movzx si,byte [ui_hit_target]
    test si,si
    jz .done
    dec si
    jz .programs
    cmp byte [app_win_slot+si],0xFF
    je .done
    test byte [app_win_style+si],1
    jnz .done
    shl si,1
    cmp word [app_saved_w+si],0
    jne .done
    mov bx,[ui_window_x+si]
    mov bp,[ui_window_y+si]
    mov cx,[ui_window_width+si]
    mov dx,[ui_window_height+si]
    mov di,65
    jmp .geometry
.programs:
    cmp byte [ui_maximized],0
    jne .done
    mov bx,[ui_wx]
    mov bp,[ui_wy]
    mov cx,[ui_ww]
    mov dx,[ui_wh]
    mov di,61
.geometry:
    mov ax,[ui_mouse_x]
    sub ax,bx
    mov si,[ui_mouse_y]
    sub si,bp
    xor bx,bx
    sub cx,5
    sub dx,5
    cmp ax,5
    jae .right
    or bl,1
.right:
    cmp ax,cx
    jb .top
    or bl,2
.top:
    cmp si,5
    jae .bottom
    or bl,4
.bottom:
    cmp si,dx
    jb .corners
    or bl,8
.corners:
    sub cx,9                         ; 14px corner zones
    sub dx,9
    test bl,3
    jz .horizontal_corner
    cmp si,14
    jae .bottom_corner
    or bl,4
.bottom_corner:
    cmp si,dx
    jb .horizontal_corner
    or bl,8
.horizontal_corner:
    test bl,12
    jz .grip
    cmp ax,14
    jae .right_corner
    or bl,1
.right_corner:
    cmp ax,cx
    jb .grip
    or bl,2
.grip:
    test bl,bl
    jnz .edge
    ; Preserve the established 12px bottom-right client grip as well.
    cmp ax,cx
    jb .none
    cmp si,dx
    jb .none
    mov bl,10
.edge:
    mov [ui_resize_edge],bl
    jmp .done
.none:
.done:
    pop ax
    cmp byte [ui_resize_edge],0
    je .restore
    mov ax,di
.restore:
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; The application's ordinary I-beam/arrow hover has already run. Override
; only a sizing zone; when leaving it do not overwrite a new client cursor.
ui_resize_hover:
    pushad
    xor bx,bx
    cmp byte [app_overlay],0
    jne .pick
    mov bl,[ui_resize_edge]
    cmp byte [ui_dragging],2
    je .captured
    cmp byte [ui_dragging],4
    jne .pick
.captured:
    mov bl,[ui_resize_drag_edge]
.pick:
    xor ax,ax
    test bl,bl
    jz .ordinary
    mov al,3                          ; horizontal
    test bl,3
    jz .vertical
    test bl,12
    jz .apply
    mov al,5                          ; NW/SE
    cmp bl,5
    je .apply
    cmp bl,10
    je .apply
    inc al                            ; NE/SW
    jmp .apply
.vertical:
    mov al,4
    jmp .apply
.ordinary:
    cmp byte [ui_cursor_type],3
    jb .done                          ; an application owns this cursor
.apply:
    call app_s_cursor
.done:
    popad
    ret

ui_resize_begin:
    mov al,[ui_resize_edge]
    mov [ui_resize_drag_edge],al
    mov ax,[ui_mouse_x]
    mov [ui_drag_dx],ax
    mov ax,[ui_mouse_y]
    mov [ui_drag_dy],ax
    movzx si,byte [ui_window_drag]
    shl si,1
    mov bx,[ui_window_x+si]
    mov dx,[ui_window_y+si]
    mov cx,[ui_window_width+si]
    mov ax,[ui_window_height+si]
    cmp byte [ui_dragging],2
    jne .save
    mov bx,[ui_wx]
    mov dx,[ui_wy]
    mov cx,[ui_ww]
    mov ax,[ui_wh]
.save:
    mov [ui_resize_x],bx
    mov [ui_resize_y],dx
    mov [ui_resize_ww],cx
    mov [ui_resize_wh],ax
    jmp ui_resize_hover

; Escape restores the geometry captured at button-down.
ui_resize_cancel:
    call ui_comp_damage_drag
    mov di,[ui_resize_x]
    mov ax,[ui_resize_ww]
    mov bx,[ui_resize_y]
    mov cx,[ui_resize_wh]
    call ui_resize_store              ; Escape restores even older small sizes
    call ui_comp_damage_drag
    mov byte [ui_dragging],0
    mov byte [ui_button_down],0
    call ui_hit_find
    call ui_resize_hover
    mov ax,250
    ret

ui_resize_apply:
    mov ax,[ui_mouse_x]
    sub ax,[ui_drag_dx]
    mov bx,[ui_resize_x]
    mov cx,[ui_resize_ww]
    mov dx,[ui_width]
    sub dx,8
    mov si,4
    mov di,320
    call ui_resize_minimum
    cmp byte [ui_dragging],2
    jne .width
    sub dx,4
    mov di,510
.width:
    movzx bp,byte [ui_resize_drag_edge]
    call ui_resize_axis
    push bx
    push cx
    mov ax,[ui_mouse_y]
    sub ax,[ui_drag_dy]
    mov bx,[ui_resize_y]
    mov cx,[ui_resize_wh]
    mov dx,[ui_height]
    sub dx,37
    mov si,32
    mov di,200
    call ui_resize_minimum
    cmp byte [ui_dragging],2
    jne .height
    mov di,370
.height:
    shr bp,2
    call ui_resize_axis
    pop ax
    pop di
; DI=x, AX=width, BX=y, CX=height; no clamping during exact cancellation.
ui_resize_store:
    movzx si,byte [ui_window_drag]
    shl si,1
    cmp byte [ui_dragging],2
    je .programs
    mov [ui_window_y+si],bx
    mov [ui_window_height+si],cx
    mov [ui_window_x+si],di
    mov [ui_window_width+si],ax
    ret
.programs:
    mov [ui_wy],bx
    mov [ui_wh],cx
    mov [ui_wx],di
    mov [ui_ww],ax
    jmp ui_library_focus_clamp

; DI=generic minimum (320 width / 200 height), all other registers preserved.
; Dynamic windows retain sizing borders, but cannot collapse fixed controls.
ui_resize_minimum:
    push ax
    push bx
    mov al,[ui_window_drag]
    mov bx,ui_resize_minima
.next:
    cmp al,[bx]
    je .found
    add bx,5
    cmp bx,ui_resize_minima_end
    jb .next
    jmp .done
.found:
    cmp di,320
    je .read
    add bx,2
.read:
    mov di,[bx+1]
.done:
    pop bx
    pop ax
    ret
ui_resize_minima:
    db 12                            ; Tasks: tabs and performance details
    dw 600,400
    db 14                            ; Control: four columns, four icon rows
    dw 442,390
    db 20                            ; Player: transport/volume and timeline
    dw 500,270
ui_resize_minima_end:

; AX=pointer delta, BX=original near edge, CX=size, SI=min near edge,
; DX=max far edge, DI=min size, BP bits 0/1=near/far edge. Output BX,CX.
; The opposite edge stays stationary; coordinates and deltas are signed.
ui_resize_axis:
    push ax
    add cx,bx                         ; original far edge
    mov ax,cx
    sub ax,si
    test bp,1
    jnz .available
    mov ax,dx
    sub ax,bx
.available:
    cmp di,ax
    jle .minimum
    mov di,ax                         ; low-resolution work area
.minimum:
    pop ax
    test bp,1
    jz .far
    add bx,ax
    cmp bx,si
    jge .near_min
    mov bx,si
.near_min:
    mov ax,cx
    sub ax,di
    cmp bx,ax
    jle .done
    mov bx,ax
    jmp .done
.far:
    test bp,2
    jz .done
    add cx,ax
    mov ax,bx
    add ax,di
    cmp cx,ax
    jge .far_min
    mov cx,ax
.far_min:
    cmp cx,dx
    jle .done
    mov cx,dx
.done:
    sub cx,bx
    ret

ui_resize_edge db 0
ui_resize_drag_edge db 0
ui_resize_x dw 0
ui_resize_y dw 0
