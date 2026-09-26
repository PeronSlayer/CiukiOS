; Private 800x600 setup surface, composed in 60 KiB bands. Fonts stay at
; their original pixel size. Only completed bands are written to VRAM.
g_render_begin:
    mov word [vc_mode],0x103
    call vc_begin
    jc .fail
    cmp byte [vc_bytes],1
    jne .close
    cmp word [vc_info+18],800
    jne .close
    cmp word [vc_info+20],600
    jne .close
    mov bx,3840
    mov ah,0x48
    int 0x21
    jc .close
    mov [g_comp_seg],ax
    mov byte [g_vbe],1
    mov word [g_screen_width],800
    mov word [g_screen_height],600
    mov word [g_origin_x],80
    mov word [g_origin_y],60
    push cs
    pop es
    mov dx,g_palette
    xor bx,bx
    mov cx,VC_PALETTE_COLORS
    call vc_set_palette
    clc
    ret
.close:
    call vc_end
.fail:
    stc
    ret

g_render_end:
    mov ax,[g_comp_seg]
    test ax,ax
    jz .done
    push es
    mov es,ax
    mov ah,0x49
    int 0x21
    pop es
    mov word [g_comp_seg],0
    call vc_end
.done:
    ret

g_render_draw:
    pushad
    push es
    mov word [g_comp_left],0
    mov word [g_comp_right],800
    mov word [g_comp_top],0
    mov word [g_comp_limit_bottom],600
    jmp g_render_bands

g_render_progress:
    pushad
    push es
    mov word [g_comp_left],291
    mov word [g_comp_right],652
    mov word [g_comp_top],228
    mov word [g_comp_limit_bottom],324
g_render_bands:
    mov word [g_comp_stride],800
    mov byte [g_pointer_band],0
    mov ax,[g_mouse_y]
    cmp ax,[g_comp_limit_bottom]
    jae .compose
    add ax,16
    cmp ax,[g_comp_top]
    jbe .compose
    mov ax,[g_mouse_x]
    cmp ax,[g_comp_right]
    jae .compose
    add ax,24
    cmp ax,[g_comp_left]
    jbe .compose
    mov byte [g_pointer_band],1
    call g_pointer_hide
.compose:
    mov byte [g_composing],1
.band:
    mov ax,[g_comp_top]
    add ax,64
    cmp ax,[g_comp_limit_bottom]
    jbe .bottom
    mov ax,[g_comp_limit_bottom]
.bottom:
    mov [g_comp_bottom],ax
    call gui_draw_scene
    call g_comp_present
    mov ax,[g_comp_bottom]
    mov [g_comp_top],ax
    cmp ax,[g_comp_limit_bottom]
    jb .band
    mov byte [g_composing],0
    call g_pointer_show       ; no-op while the pointer is still visible
    pop es
    popad
    ret

; Inputs are logical wizard coordinates. Pointer calls use the same origin.
g_render_rect:
    add bx,[g_origin_x]
    add dx,[g_origin_y]
    cmp byte [g_composing],1
    je g_comp_rect
    ; Only the 1x1 software pointer is painted outside a composed scene.
    movzx edi,dx
    movzx ebp,word [vc_pitch]
    imul edi,ebp
    movzx ebp,bx
    add edi,ebp
    call vc_put_index
    pop es
    popad
    ret

g_render_glyph:
    push bx
    push dx
    add bx,[g_origin_x]
    add dx,[g_origin_y]
    call g_comp_glyph
    pop dx
    pop bx
    ret

g_comp_rect:
    mov [g_fill_color],al
    mov ax,bx
    add ax,cx
    cmp bx,[g_comp_right]
    jae .done
    cmp ax,[g_comp_left]
    jbe .done
    cmp bx,[g_comp_left]
    jae .left
    mov bx,[g_comp_left]
.left:
    cmp ax,[g_comp_right]
    jbe .right
    mov ax,[g_comp_right]
.right:
    sub ax,bx
    mov cx,ax
    mov ax,dx
    add ax,si
    cmp dx,[g_comp_bottom]
    jae .done
    cmp ax,[g_comp_top]
    jbe .done
    cmp dx,[g_comp_top]
    jae .top
    mov dx,[g_comp_top]
.top:
    cmp ax,[g_comp_bottom]
    jbe .bottom
    mov ax,[g_comp_bottom]
.bottom:
    sub ax,dx
    mov bp,ax
    mov [g_fill_width],cx
    mov ax,dx
    sub ax,[g_comp_top]
    mul word [g_comp_stride]
    mov di,ax
    mov ax,bx
    movzx cx,byte [vc_bytes]
    mul cx
    add di,ax
    mov ax,[g_comp_seg]
    mov es,ax
    movzx bx,byte [g_fill_color]
    shl bx,2
    mov eax,[vc_colors+bx]
.row:
    push di
    mov cx,[g_fill_width]
    cmp byte [vc_bytes],4
    je .dwords
    cmp byte [vc_bytes],2
    je .words
    cmp byte [vc_bytes],3
    je .triples
    rep stosb
    jmp .next
.words:
    rep stosw
    jmp .next
.dwords:
    rep stosd
    jmp .next
.triples:
    mov ebx,eax
.triple:
    stosw
    shr eax,16
    stosb
    mov eax,ebx
    loop .triple
.next:
    pop di
    add di,[g_comp_stride]
    dec bp
    jnz .row
.done:
    pop es
    popad
    ret

g_comp_glyph:
    pushad
    push es
    mov bp,16
    mov ax,[g_comp_seg]
    mov es,ax
.row:
    cmp dx,[g_comp_top]
    jb .skip
    cmp dx,[g_comp_bottom]
    jae .done
    push dx
    mov ax,dx
    sub ax,[g_comp_top]
    mul word [g_comp_stride]
    mov di,ax
    mov ax,bx
    movzx cx,byte [vc_bytes]
    mul cx
    add di,ax
    pop dx
    mov [g_comp_glyph_x],bx
    push bx
    mov bx,[si]
    mov cx,16
.pixel:
    shl bx,1
    jnc .next
    mov ax,[g_comp_glyph_x]
    cmp ax,[g_comp_left]
    jb .next
    cmp ax,[g_comp_right]
    jae .next
    push bx
    movzx bx,byte [g_text_color]
    shl bx,2
    mov eax,[vc_colors+bx]
    pop bx
    cmp byte [vc_bytes],4
    je .dword
    cmp byte [vc_bytes],1
    je .byte
    mov [es:di],ax
    cmp byte [vc_bytes],2
    je .next
    shr eax,16
    mov [es:di+2],al
    jmp .next
.dword:
    mov [es:di],eax
    jmp .next
.byte:
    mov [es:di],al
.next:
    inc word [g_comp_glyph_x]
    movzx ax,byte [vc_bytes]
    add di,ax
    loop .pixel
    pop bx
.skip:
    add si,2
    inc dx
    dec bp
    jnz .row
.done:
    pop es
    popad
    ret

g_comp_present:
    ; Completed band rows are written once and never read back from VRAM.
    pushad
    push fs
    mov fs,[g_comp_seg]
    movzx ebx,byte [vc_bytes]
    movzx eax,word [g_comp_left]
    imul eax,ebx
    mov si,ax
    movzx edi,word [g_comp_top]
    movzx edx,word [vc_pitch]
    imul edi,edx
    add edi,eax
    mov cx,[g_comp_right]
    sub cx,[g_comp_left]
    imul cx,bx
    mov dx,[g_comp_bottom]
    sub dx,[g_comp_top]
    mov bx,[g_comp_stride]
    call vc_fb_put_rows
    pop fs
    popad
    ret

g_vbe_pointer_hide:
    cmp byte [g_pointer_visible],1
    jne .done
    pushad
    mov edi,[g_pointer_linear]
    mov si,g_pointer_under
    call vc_pointer_restore
    popad
    mov byte [g_pointer_visible],0
.done:
    ret

g_vbe_pointer_show:
    cmp byte [g_mouse_ready],1
    jne .done
    cmp byte [g_pointer_visible],1
    je .done
    pushad
    movzx edi,word [g_mouse_y]
    movzx eax,word [vc_pitch]
    imul edi,eax
    movzx eax,word [g_mouse_x]
    movzx ecx,byte [vc_bytes]
    imul eax,ecx
    add edi,eax
    mov [g_pointer_linear],edi
    mov si,g_pointer_under
    call vc_pointer_save
    mov bx,g_pointer_black
    call vc_pointer_draw
    mov byte [g_pointer_visible],1
    popad
.done:
    ret

g_vbe db 0
g_composing db 0
g_pointer_band db 0
g_screen_width dw 640
g_screen_height dw 480
g_origin_x dw 0
g_origin_y dw 0
g_comp_seg dw 0
g_comp_stride dw 800
g_comp_top dw 0
g_comp_bottom dw 0
g_comp_limit_bottom dw 600
g_comp_left dw 0
g_comp_right dw 800
g_fill_color db 0
g_fill_width dw 0
g_comp_glyph_x dw 0
g_comp_x_bytes dw 0
g_comp_row_bytes dw 0
g_pointer_linear dd 0
%define VC_GRAPHICS_ONLY
%include "src/com/vbe_console.inc"
%undef VC_GRAPHICS_ONLY
