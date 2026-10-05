; Native desktop primitives. VBE (linear or banked), with planar VGA fallback.
; Inputs: BX=x, DX=y, CX=width, SI=height, AL=color. Registers preserved.
%define UI_FONT_BYTES (95+95*32)
%macro U_RECT 5
    mov bx,%1
    mov dx,%2
    mov cx,%3
    mov si,%4
    mov al,%5
    call ui_rect
%endmacro
%macro U_TEXT 4-5 0
    mov bx,%1
    mov dx,%2
    mov si,%3
    mov ax,(%5<<8)|%4
    call ui_text
%endmacro

ui_rect:
    pushad
    push es
    cld
    test bx,bx
    js .done
    test dx,dx
    js .done
    cmp bx,[ui_width]
    jae .done
    cmp dx,[ui_height]
    jae .done
    test cx,cx
    jz .done
    test si,si
    jz .done
    mov bp,[ui_width]
    sub bp,bx
    cmp cx,bp
    jbe .height
    mov cx,bp
.height:
    mov bp,[ui_height]
    sub bp,dx
    cmp si,bp
    jbe .clipped
    mov si,bp
.clipped:
    cmp byte [ui_composing],1
    je ui_comp_rect
    cmp byte [ui_vbe],1
    je .packed
    call ug_rect
    jmp .done
.packed:
    mov [ui_fill_color],al
    mov [ui_fill_width],cx
    mov [ui_fill_height],si
    movzx edi,dx
    movzx eax,word [vc_pitch]
    imul edi,eax
    movzx eax,bx
    movzx ecx,byte [vc_bytes]
    imul eax,ecx
    add edi,eax
    add edi,[ui_front_base]
.row:
    movzx bx,byte [ui_fill_color]
    shl bx,2
    mov eax,[vc_colors+bx]
    movzx ecx,word [ui_fill_width]
    call vc_span
    movzx eax,word [vc_pitch]
    add edi,eax
    dec word [ui_fill_height]
    jnz .row
.done:
    pop es
    popad
    ret

; Transparent 13px proportional glyphs, regular or bold (AH=1).
ui_text:
    pushad
    push fs
    mov fs,[ui_assets_seg]
    ; The scene is visited once per compositor band. Reject a whole text
    ; row before walking its characters when its 16-pixel glyph extent
    ; cannot touch this band. Zero-extension prevents 16-bit y+16 wrapping.
    cmp byte [ui_composing],1
    jne .visible
    cmp dx,[ui_comp_bottom]
    jae .done
    movzx edi,dx
    add edi,16
    movzx ebp,word [ui_comp_top]
    cmp edi,ebp
    jbe .done
.visible:
    mov [ug_text_color],al
    mov di,UI_ASSET_FONT
    test ah,ah
    jz .font
    add di,UI_FONT_BYTES
.font:
    mov [ui_font_ptr],di
.next:
    lodsb
    test al,al
    jz .done
    cmp al,32
    jb .next
    cmp al,126
    ja .next
    sub al,32
    movzx di,al
    push si
    mov si,[ui_font_ptr]
    mov bp,si
    add bp,di
    mov al,[fs:bp]
    mov [ui_advance],al
    shl di,5
    add si,95
    add si,di
    ; A whole glyph must fit. Never let a planar glyph wrap onto a new row.
    mov ax,bx
    add ax,24
    cmp ax,[ui_width]
    ja .skip
    mov ax,dx
    add ax,16
    cmp ax,[ui_height]
    ja .skip
    cmp byte [ui_composing],1
    je .packed
    cmp byte [ui_vbe],1
    je .packed
    call ug_glyph
    jmp .skip
.packed:
    call ui_glyph
.skip:
    pop si
    movzx ax,byte [ui_advance]
    add bx,ax
    jmp .next
.done:
    pop fs
    popad
    ret

ui_glyph:
    cmp byte [ui_composing],1
    je ui_comp_glyph
    pushad
    push es
    movzx edi,dx
    movzx eax,word [vc_pitch]
    imul edi,eax
    movzx eax,bx
    movzx ecx,byte [vc_bytes]
    imul eax,ecx
    add edi,eax
    mov bp,16
.row:
    push edi
    mov bx,[fs:si]
    add si,2
    mov cx,16
.pixel:
    shl bx,1
    jnc .next
    mov al,[ug_text_color]
    call vc_put_index
.next:
    movzx eax,byte [vc_bytes]
    add edi,eax
    loop .pixel
    pop edi
    movzx eax,word [vc_pitch]
    add edi,eax
    dec bp
    jnz .row
    pop es
    popad
    ret

; Raised two-step frame; AL=fill, same rectangle inputs. The outer top and
; left remain bright, with an ink outer edge and gray inner shadow below.
ui_bevel:
    pushad
    call ui_rect
    cmp cx,4
    jb .done
    cmp si,4
    jb .done
    push bx
    push dx
    push cx
    push si
    push si
    mov si,1
    mov al,15
    call ui_rect
    pop si
    push cx
    mov cx,1
    call ui_rect
    pop cx
    add dx,si
    dec dx
    push si
    mov si,1
    mov al,0
    call ui_rect
    pop si
    sub dx,si
    inc dx
    add bx,cx
    dec bx
    mov cx,1
    call ui_rect
    pop si
    pop cx
    pop dx
    pop bx
    inc bx
    inc dx
    sub cx,2
    sub si,2
    push si
    mov si,1
    mov al,7
    call ui_rect
    pop si
    push cx
    mov cx,1
    call ui_rect
    pop cx
    add dx,si
    dec dx
    push si
    mov si,1
    mov al,8
    call ui_rect
    pop si
    sub dx,si
    inc dx
    add bx,cx
    dec bx
    mov cx,1
    call ui_rect
.done:
    popad
    ret

; Recessed paper field. Same rectangle inputs; two one-pixel shadow lines
; give form to workspaces without gradients, alpha blending or extra buffers.
ui_inset:
    pushad
    mov al,15
    call ui_rect
    cmp cx,4
    jb .done
    cmp si,4
    jb .done
    push si
    mov si,1
    mov al,8
    call ui_rect
    pop si
    push cx
    mov cx,1
    call ui_rect
    pop cx
    inc bx
    inc dx
    sub cx,2
    sub si,2
    push si
    mov si,1
    mov al,0
    call ui_rect
    pop si
    mov cx,1
    call ui_rect
.done:
    popad
    ret

; Platinum title rails with dynamic indigo/slate title tab. Inputs match
; ui_rect; AL is the active/inactive title color.
ui_title_rail:
    pushad
    mov ah,al
    mov al,7
    call ui_rect
    cmp cx,140
    jb .plain
    push cx
    push bx
    push dx
    add bx,6
    sub cx,80                       ; leave clear space for window controls
    jbe .no_rail
    add dx,7
    mov bp,3
.rail:
    mov si,1
    mov al,8
    call ui_rect
    inc dx
    mov al,15
    call ui_rect
    add dx,3
    dec bp
    jnz .rail
.no_rail:
    pop dx
    pop bx
    pop cx
    jmp .done
.plain:
    mov al,ah
    call ui_rect
.done:
    popad
    ret

; BX=x, DX=y, CX=maximum title advance, SI=title, AL=the rail color.
; Keep captions out of the window controls and visually distinguish the
; focused window. The title buffer is local to this single-threaded renderer.
ui_title_text:
    pushad
    push ds
    push es
    push fs
    push cs
    pop ds
    push cs
    pop es
    mov [uit_x],bx
    mov [uit_y],dx
    mov [uit_max],cx
    mov [uit_source],si
    mov [uit_color],al
    cmp al,1
    jne .inactive
    mov word [uit_attr],0x010F      ; bold paper text on the active rail
    jmp .font
.inactive:
    mov word [uit_attr],0x000F      ; regular paper text on the inactive rail
.font:
    mov ax,[ui_assets_seg]
    mov fs,ax
    mov ax,UI_ASSET_FONT
    cmp byte [uit_attr+1],0
    je .save_ptr
    add ax,UI_FONT_BYTES
.save_ptr:
    mov [uit_font_ptr],ax
    test word [uit_max],0x8000
    jnz .empty
    cmp word [uit_max],16
    jb .empty

    ; Measure exactly with the same proportional font advances as ui_text.
    mov si,[uit_source]
    xor cx,cx
.measure:
    lodsb
    test al,al
    jz .copy_full
    cmp al,32
    jb .measure
    cmp al,126
    ja .measure
    movzx bp,al
    sub bp,32
    ; ui_text draws every glyph into a fixed 16-pixel cell, even when the
    ; proportional advance is narrower. Bound the glyph extent, not just its
    ; advance, so the final character cannot overlap window controls.
    mov ax,cx
    add ax,16
    cmp ax,[uit_max]
    ja .truncate
    add bp,[uit_font_ptr]
    movzx ax,byte [fs:bp]
    add cx,ax
    jmp .measure
.copy_full:
    mov [uit_text_width],cx
    mov si,[uit_source]
    mov di,uit_buffer
    mov cx,63
.copy:
    lodsb
    stosb
    test al,al
    jz .draw
    loop .copy
    mov byte [di],0
    jmp .draw

.truncate:
    mov bp,[uit_font_ptr]
    add bp,14
    movzx ax,byte [fs:bp] ; proportional width of '.'
    imul ax,3
    mov [uit_dots],ax
    mov ax,[uit_max]
    cmp ax,16
    jb .empty
    sub ax,16                        ; reserve the last dot's 16px glyph cell
    cmp ax,[uit_dots]
    jb .empty
    sub ax,[uit_dots]
    mov [uit_prefix_max],ax
    mov word [uit_prefix_width],0
    mov word [uit_length],0
    mov si,[uit_source]
    mov di,uit_buffer
.prefix:
    lodsb
    test al,al
    jz .ellipsis
    cmp al,32
    jb .prefix
    cmp al,126
    ja .prefix
    push ax
    movzx bp,al
    sub bp,32
    add bp,[uit_font_ptr]
    movzx ax,byte [fs:bp]
    add [uit_prefix_width],ax
    mov ax,[uit_prefix_width]
    cmp ax,[uit_prefix_max]
    ja .stop_prefix
    cmp word [uit_length],60
    jae .stop_prefix
    pop ax
    stosb
    inc word [uit_length]
    jmp .prefix
.stop_prefix:
    pop ax
.ellipsis:
    mov al,'.'
    stosb
    stosb
    stosb
    mov byte [di],0
    mov ax,[uit_prefix_width]
    add ax,[uit_dots]
    mov [uit_text_width],ax
    jmp .draw
.empty:
    mov byte [uit_buffer],0
    mov word [uit_text_width],0
.draw:
    ; Draw dynamic title tab following the title text extent.
    mov cx,[uit_text_width]
    add cx,19                       ; 7px left padding + 12px right padding
    cmp cx,96
    jae .tab_min_ok
    mov cx,96
.tab_min_ok:
    mov ax,[uit_max]
    add ax,12
    cmp cx,ax
    jbe .tab_max_ok
    mov cx,ax
.tab_max_ok:
    test cx,cx
    jle .draw_text
    mov bx,[uit_x]
    sub bx,7
    mov dx,[uit_y]
    sub dx,4
    mov si,25
    mov al,[uit_color]
    call ui_rect

    ; Clear 6px spacer between tab and rails if there is still room before controls.
    mov bx,[uit_x]
    sub bx,7
    add bx,cx                       ; bx = right edge of tab
    mov ax,[uit_max]
    add ax,12
    sub ax,cx                       ; ax = remaining margin before controls
    cmp ax,6
    jb .draw_text
    push cx
    mov cx,6
    mov si,25
    mov al,7
    call ui_rect
    pop cx

.draw_text:
    mov bx,[uit_x]
    mov dx,[uit_y]
    mov si,uit_buffer
    mov ax,[uit_attr]
    call ui_text
    pop fs
    pop es
    pop ds
    popad
    ret

ug_rect:
    pushad
    push es
    mov [ug_color], al
    mov [ug_x], bx
    mov [ug_y], dx
    mov [ug_width], cx
    mov [ug_height], si
    test cx, cx
    jz .out
    test si, si
    jz .out
    mov ax, 0xA000
    mov es, ax
    mov dx, 0x3C4
    mov ax, 0x0F02
    out dx, ax
    mov dx, 0x3CE
    mov ax, 0x0005
    out dx, ax
    mov ax, 0x0003
    out dx, ax
    mov ax, 0x0F01
    out dx, ax
    xor al, al
    mov ah, [ug_color]
    out dx, ax
    mov ax, [ug_y]
    imul ax, 80
    mov di, [ug_x]
    shr di, 3
    add di, ax
    mov bx, [ug_x]
    and bx, 7
    mov cl, bl
    mov al, 255
    shr al, cl
    mov [ug_left_mask], al
    mov bx, [ug_x]
    add bx, [ug_width]
    dec bx
    mov ax, bx
    shr ax, 3
    mov cx, [ug_x]
    shr cx, 3
    sub ax, cx
    mov [ug_span], ax
    and bx, 7
    mov cx, 7
    sub cx, bx
    mov al, 255
    shl al, cl
    mov [ug_right_mask], al
.row:
    push di
    mov ah, [ug_left_mask]
    cmp word [ug_span], 0
    jne .many
    and ah, [ug_right_mask]
    mov al, 8
    out dx, ax
    mov al, [es:di]
    mov [es:di], al
    jmp .advance
.many:
    mov al, 8
    out dx, ax
    mov al, [es:di]
    mov [es:di], al
    inc di
    mov ax, 0xFF08
    out dx, ax
    mov cx, [ug_span]
    dec cx
    mov al, 255
    rep stosb
    mov ah, [ug_right_mask]
    mov al, 8
    out dx, ax
    mov al, [es:di]
    mov [es:di], al
.advance:
    pop di
    add di, 80
    dec word [ug_height]
    jnz .row
    mov ax, 0xFF08
    out dx, ax
.out:
    pop es
    popad
    ret

ug_glyph:
    pushad
    push es
    mov ax, 0xA000
    mov es, ax
    mov ax, dx
    imul ax, 80
    mov di, bx
    shr di, 3
    add di, ax
    and bx, 7
    mov [ug_glyph_shift], bl
    mov dx, 0x3C4
    mov ax, 0x0F02
    out dx, ax
    mov dx, 0x3CE
    mov ax, 0x0005
    out dx, ax
    mov ax, 0x0003
    out dx, ax
    mov ax, 0x0F01
    out dx, ax
    xor al, al
    mov ah, [ug_text_color]
    out dx, ax
    mov bx, 16
.row:
    movzx eax, word [fs:si]
    add si, 2
    shl eax, 8
    mov cl, [ug_glyph_shift]
    shr eax, cl
    mov ebp, eax
    shr eax, 16
    call .byte
    inc di
    mov eax, ebp
    shr eax, 8
    call .byte
    inc di
    mov eax, ebp
    call .byte
    add di, 78
    dec bx
    jnz .row
    mov ax, 0xFF08
    out dx, ax
    pop es
    popad
    ret
.byte:
    test al, al
    jz .skip
    mov ah, al
    mov al, 8
    out dx, ax
    mov al, [es:di]
    mov [es:di], al
.skip:
    ret


ui_fill_color db 0
ui_fill_width dw 0
ui_fill_height dw 0
ui_font_ptr dw 0
ui_advance db 0
ug_color db 0
ug_x dw 0
ug_y dw 0
ug_width dw 0
ug_height dw 0
ug_left_mask db 0
ug_right_mask db 0
ug_span dw 0
ug_text_color db 0
ug_glyph_shift db 0
uit_x dw 0
uit_y dw 0
uit_max dw 0
uit_source dw 0
uit_attr dw 0
uit_font_ptr dw 0
uit_dots dw 0
uit_prefix_max dw 0
uit_prefix_width dw 0
uit_length dw 0
uit_color db 0
uit_text_width dw 0
uit_buffer times 64 db 0
