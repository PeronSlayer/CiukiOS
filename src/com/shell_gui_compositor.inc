; Compose bounded horizontal bands in ordinary DOS memory, then copy only
; completed pixels to VRAM. No framebuffer clear is ever presented midway
; through a normal redraw. Storage is 60 KiB regardless of video resolution.
ui_comp_begin:
    pushad
    push es
    cmp byte [ui_vbe],1
    jne .done
    cmp word [ui_comp_seg],0
    jne .done
    ; Compute the band before allocating: even one scanline must fit the
    ; private segment. Never accept a wrapped stride or a zero-height band.
    movzx eax,word [ui_width]
    movzx ebx,byte [vc_bytes]
    imul eax,ebx
    test eax,eax
    jz .done
    cmp eax,61440
    ja .done
    mov [ui_comp_stride],ax
    mov bx,3840
    mov ah,0x48
    int 0x21
    jc .done
    mov [ui_comp_seg],ax
    mov bx,[ui_comp_stride]
    mov ax,61440
    xor dx,dx
    div bx
    cmp ax,64
    jbe .height
    mov ax,64
.height:
    mov [ui_comp_rows],ax
    call ui_pages_begin
.done:
    pop es
    popad
    ret

ui_comp_end:
    pushad
    push es
    mov ax,[ui_comp_seg]
    test ax,ax
    jz .done
    mov es,ax
    mov ah,0x49
    int 0x21
    mov word [ui_comp_seg],0
.done:
    mov byte [ui_page_enabled],0
    mov byte [ui_page_pending],0
    mov dword [ui_front_base],0
    mov eax,[vc_frame_bytes]
    mov [vc_access_bytes],eax
    mov byte [ui_composing],0
    mov byte [ui_comp_dirty],0
    pop es
    popad
    ret

ui_comp_draw:
    cmp word [ui_comp_seg],0
    je .direct
    cmp byte [ui_comp_dirty],1
    je .bounds
    mov word [ui_comp_left],0
    mov word [ui_comp_limit_top],0
    mov ax,[ui_width]
    mov [ui_comp_right],ax
    mov ax,[ui_height]
    mov [ui_comp_limit_bottom],ax
.bounds:
    ; Damage is an exclusive rectangle. Empty/off-screen requests must not
    ; enter the band loop: a REP/LOOP count of zero can otherwise wrap.
    mov ax,[ui_comp_right]
    cmp ax,[ui_width]
    jbe .right
    mov ax,[ui_width]
    mov [ui_comp_right],ax
.right:
    cmp ax,[ui_comp_left]
    jbe .pointer
    mov ax,[ui_comp_limit_bottom]
    cmp ax,[ui_height]
    jbe .bottom_limit
    mov ax,[ui_height]
    mov [ui_comp_limit_bottom],ax
.bottom_limit:
    cmp ax,[ui_comp_limit_top]
    jbe .pointer
    call ui_pages_prepare
    call ui_comp_bands
    jmp .pointer
.direct:
    call ui_pointer_hide
    call ui_draw_scene
.pointer:
    mov byte [ui_comp_dirty],0
    call ui_pages_present
    jc .done
    call ui_pointer_show
.done:
    ret

; Compose the damage rectangle band by band, presenting each finished band
; to ui_present_base. Single-page presentation hides the pointer only for
; the copy of a band that overlaps it.
ui_comp_bands:
    call ui_comp_band_setup
    mov byte [ui_composing],1
    mov ax,[ui_comp_limit_top]
    mov [ui_comp_top],ax
.band:
    mov ax,[ui_comp_top]
    add ax,[ui_comp_rows]
    cmp ax,[ui_comp_limit_bottom]
    jbe .bottom
    mov ax,[ui_comp_limit_bottom]
.bottom:
    mov [ui_comp_bottom],ax
    call ui_draw_scene
    ; Leave the software cursor visible while composing. Hide it only for
    ; the brief VRAM copy of a band which actually intersects its footprint.
    mov byte [ui_comp_cursor_band],0
    cmp byte [ui_page_enabled],1
    je .present
    mov ax,[ui_mouse_y]
    cmp ax,[ui_comp_bottom]
    jae .present
    add ax,16
    cmp ax,[ui_comp_top]
    jbe .present
    mov ax,[ui_mouse_x]
    cmp ax,[ui_comp_right]
    jae .present
    add ax,24
    cmp ax,[ui_comp_left]
    jbe .present
    mov byte [ui_comp_cursor_band],1
    call ui_pointer_hide
.present:
    call ui_comp_present
    cmp byte [ui_comp_cursor_band],1
    jne .advance
    mov byte [ui_composing],0
    call ui_pointer_show
    mov byte [ui_composing],1
.advance:
    mov ax,[ui_comp_bottom]
    mov [ui_comp_top],ax
    cmp ax,[ui_comp_limit_bottom]
    jb .band
    mov byte [ui_composing],0
    ret

; Idle catch-up for two-page presentation. The hidden page lags the visible
; one by the last presented damage; compose that area into it now, so the
; next frame composes only its own damage instead of both.
ui_pages_sync:
    cmp byte [ui_page_enabled],1
    jne .done
    cmp byte [ui_comp_dirty],0
    jne .done
    pushad
    mov ax,[ui_page_previous_left]
    mov [ui_comp_left],ax
    mov bx,[ui_page_previous_right]
    mov [ui_comp_right],bx
    cmp bx,ax
    jbe .clean
    mov ax,[ui_page_previous_top]
    mov [ui_comp_limit_top],ax
    mov bx,[ui_page_previous_bottom]
    mov [ui_comp_limit_bottom],bx
    cmp bx,ax
    jbe .clean
    mov eax,[vc_frame_bytes]
    sub eax,[ui_front_base]
    mov [ui_present_base],eax
    call ui_comp_bands
.clean:
    mov ax,[ui_width]
    mov [ui_page_previous_left],ax
    mov ax,[ui_height]
    mov [ui_page_previous_top],ax
    xor ax,ax
    mov [ui_page_previous_right],ax
    mov [ui_page_previous_bottom],ax
    popad
.done:
    ret

; Hardware pages are optional. Some firmware omits static image-page counts;
; require two frames in the active scanline/VRAM allocation, then prove that
; set/get-display-start can select page one and restore page zero.
; No retrace polling or blocking "wait for retrace" firmware request is used.
ui_pages_begin:
    pushad
    mov byte [ui_page_enabled],0
    mov byte [ui_page_pending],0
    mov dword [ui_front_base],0
    cmp byte [ui_page_failed],0
    jne .done
    movzx eax,word [ui_height]
    shl eax,1
    movzx edx,word [vc_scan_lines]
    cmp eax,edx
    ja .done
    mov eax,[vc_frame_bytes]
    shl eax,1
    movzx edx,word [vc_controller+18]
    shl edx,16
    cmp eax,edx
    ja .done
    cmp byte [vc_lfb],1
    je .probe
    dec eax
    shr eax,16
    movzx edx,word [vc_bank_step]
    imul eax,edx
    cmp eax,65535
    ja .done
.probe:
    mov dx,[ui_height]
    call ui_pages_switch
    jc .unsupported
    xor dx,dx
    call ui_pages_switch
    jc .recover
    mov eax,[vc_frame_bytes]
    shl eax,1
    mov [vc_access_bytes],eax
    mov byte [ui_page_enabled],1
    mov word [ui_page_previous_left],0
    mov word [ui_page_previous_top],0
    mov ax,[ui_width]
    mov [ui_page_previous_right],ax
    mov ax,[ui_height]
    mov [ui_page_previous_bottom],ax
    jmp .done
.unsupported:
    mov byte [ui_page_failed],1
    xor dx,dx
    call ui_pages_switch
    jnc .done
.recover:
    call ui_pages_failure
.done:
    popad
    ret

; DX = first visible scanline. Check the readback rather than trusting AX
; alone; a firmware may accept a request but retain the previous viewport.
ui_pages_switch:
    pushad
    mov [ui_page_scanline],dx
    xor bx,bx
    xor cx,cx
    mov ax,0x4F07
    call vc_bios
    cmp ax,0x004F
    jne .fail
    mov bx,1
    mov ax,0x4F07
    call vc_bios
    cmp ax,0x004F
    jne .fail
    cmp word [vc_bios_cx],0
    jne .fail
    mov ax,[ui_page_scanline]
    cmp ax,[vc_bios_dx]
    jne .fail
    popad
    clc
    ret
.fail:
    popad
    stc
    ret

ui_pages_prepare:
    pushad
    mov byte [ui_page_pending],0
    mov dword [ui_present_base],0
    cmp byte [ui_page_enabled],1
    jne .done
    ; The back page contains the scene from one presentation ago. Redraw
    ; previous damage plus current damage, then retain ONLY current damage:
    ; after swapping, the old front differs from the new scene by that area.
    mov ax,[ui_comp_left]
    mov [ui_page_next_left],ax
    cmp ax,[ui_page_previous_left]
    jbe .left
    mov ax,[ui_page_previous_left]
    mov [ui_comp_left],ax
.left:
    mov ax,[ui_comp_right]
    mov [ui_page_next_right],ax
    cmp ax,[ui_page_previous_right]
    jae .right
    mov ax,[ui_page_previous_right]
    mov [ui_comp_right],ax
.right:
    mov ax,[ui_comp_limit_top]
    mov [ui_page_next_top],ax
    cmp ax,[ui_page_previous_top]
    jbe .top
    mov ax,[ui_page_previous_top]
    mov [ui_comp_limit_top],ax
.top:
    mov ax,[ui_comp_limit_bottom]
    mov [ui_page_next_bottom],ax
    cmp ax,[ui_page_previous_bottom]
    jae .bottom
    mov ax,[ui_page_previous_bottom]
    mov [ui_comp_limit_bottom],ax
.bottom:
    mov eax,[vc_frame_bytes]
    sub eax,[ui_front_base]
    mov [ui_present_base],eax
    mov byte [ui_page_pending],1
.done:
    popad
    ret

ui_pages_present:
    cmp byte [ui_vbe],1
    jne .ready
    cmp byte [vc_active],1
    jne .inactive
    cmp byte [ui_page_pending],1
    jne .ready
    pushad
    mov byte [ui_page_pending],0
    ; Cursor save/restore belongs solely to the visible page. Restore it
    ; before swapping so the next back page never contains a stale cursor.
    call ui_pointer_hide
    xor dx,dx
    cmp dword [ui_present_base],0
    je .flip
    mov dx,[ui_height]
.flip:
    call ui_pages_switch
    jc .fail
    mov eax,[ui_present_base]
    mov [ui_front_base],eax
    mov ax,[ui_page_next_left]
    mov [ui_page_previous_left],ax
    mov ax,[ui_page_next_right]
    mov [ui_page_previous_right],ax
    mov ax,[ui_page_next_top]
    mov [ui_page_previous_top],ax
    mov ax,[ui_page_next_bottom]
    mov [ui_page_previous_bottom],ax
    popad
.ready:
    clc
    ret
.fail:
    call ui_pages_failure
    popad
    stc
    ret
.inactive:
    pushad
    call ui_pages_failure
    popad
    stc
    ret

ui_pages_failure:
    mov byte [ui_page_failed],1
    mov byte [ui_page_enabled],0
    mov byte [ui_page_pending],0
    mov byte [ui_comp_recover],1
    mov dword [ui_front_base],0
    mov eax,[vc_frame_bytes]
    mov [vc_access_bytes],eax
    call vc_end
    ret

; Pack only the damaged horizontal span into the scratch segment. A narrow
; window on a 2K desktop should not replay the scene once per six scanlines
; merely because the physical screen is wide. Coordinates remain absolute
; at the public draw API; only scratch addresses subtract ui_comp_left.
ui_comp_band_setup:
    pushad
    mov ax,[ui_comp_right]
    sub ax,[ui_comp_left]
    movzx bx,byte [vc_bytes]
    mul bx
    mov [ui_comp_stride],ax
    mov bx,ax
    mov ax,61440
    xor dx,dx
    div bx
    cmp ax,64
    jbe .rows
    mov ax,64
.rows:
    mov [ui_comp_rows],ax
    popad
    ret

; ui_rect has already checked the physical screen bounds.
ui_comp_rect:
    mov [ui_fill_color],al
    mov ax,bx
    add ax,cx
    cmp bx,[ui_comp_right]
    jae .done
    cmp ax,[ui_comp_left]
    jbe .done
    cmp bx,[ui_comp_left]
    jae .left
    mov bx,[ui_comp_left]
.left:
    cmp ax,[ui_comp_right]
    jbe .right
    mov ax,[ui_comp_right]
.right:
    sub ax,bx
    mov cx,ax
    mov ax,dx
    add ax,si
    cmp dx,[ui_comp_bottom]
    jae .done
    cmp ax,[ui_comp_top]
    jbe .done
    cmp dx,[ui_comp_top]
    jae .top
    mov dx,[ui_comp_top]
.top:
    cmp ax,[ui_comp_bottom]
    jbe .bottom
    mov ax,[ui_comp_bottom]
.bottom:
    sub ax,dx
    mov bp,ax
    mov [ui_fill_width],cx
    mov ax,dx
    sub ax,[ui_comp_top]
    mul word [ui_comp_stride]
    mov di,ax
    mov ax,bx
    sub ax,[ui_comp_left]
    movzx cx,byte [vc_bytes]
    mul cx
    add di,ax
    mov ax,[ui_comp_seg]
    mov es,ax
    movzx bx,byte [ui_fill_color]
    shl bx,2
    mov eax,[vc_colors+bx]
.row:
    push di
    mov cx,[ui_fill_width]
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
    add di,[ui_comp_stride]
    dec bp
    jnz .row
.done:
    pop es
    popad
    ret

ui_comp_glyph:
    pushad
    push es
    ; Most scene glyphs do not intersect a short scanline band. Reject the
    ; complete footprint before visiting sixteen clipped rows per character.
    cmp dx,[ui_comp_bottom]
    jae .done
    mov ax,dx
    add ax,16
    cmp ax,[ui_comp_top]
    jbe .done
    cmp bx,[ui_comp_right]
    jae .done
    mov ax,bx
    add ax,16
    cmp ax,[ui_comp_left]
    jbe .done
    mov bp,16
    mov ax,[ui_comp_seg]
    mov es,ax
.row:
    cmp dx,[ui_comp_top]
    jb .skip
    cmp dx,[ui_comp_bottom]
    jae .done
    push dx
    mov ax,dx
    sub ax,[ui_comp_top]
    mul word [ui_comp_stride]
    mov di,ax
    mov ax,bx
    sub ax,[ui_comp_left]
    movzx cx,byte [vc_bytes]
    mul cx
    add di,ax
    pop dx
    mov [ui_comp_glyph_x],bx
    push bx
    mov bx,[fs:si]
    mov cx,16
.pixel:
    shl bx,1
    jnc .next
    mov ax,[ui_comp_glyph_x]
    cmp ax,[ui_comp_left]
    jb .next
    cmp ax,[ui_comp_right]
    jae .next
    push bx
    movzx bx,byte [ug_text_color]
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
    inc word [ui_comp_glyph_x]
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

ui_comp_present:
    ; Write each completed band row once, sequentially, without reading VRAM.
    pushad
    push fs
    mov ax,[ui_comp_right]
    cmp ax,[ui_comp_left]
    jbe .done
    mov ax,[ui_comp_bottom]
    cmp ax,[ui_comp_top]
    jbe .done
    mov fs,[ui_comp_seg]
    movzx ebx,byte [vc_bytes]
    movzx eax,word [ui_comp_left]
    imul eax,ebx
    movzx edi,word [ui_comp_top]
    movzx edx,word [vc_pitch]
    imul edi,edx
    add edi,eax
    add edi,[ui_present_base]
    mov cx,[ui_comp_right]
    sub cx,[ui_comp_left]
    imul cx,bx
    mov dx,[ui_comp_bottom]
    sub dx,[ui_comp_top]
    xor si,si
    mov bx,[ui_comp_stride]
    call vc_fb_put_rows
.done:
    pop fs
    popad
    ret

; Extend the invalidated region with a physical rectangle. The previous and
; next bounds of a moving window are both included, including its shadow.
ui_comp_damage:
    pushad
    test cx,cx
    jz .done
    test si,si
    jz .done
    cmp bx,[ui_width]
    jae .done
    cmp dx,[ui_height]
    jae .done
    ; Clamp widths before addition so even a large extent cannot wrap.
    mov ax,[ui_width]
    sub ax,bx
    cmp cx,ax
    jbe .width
    mov cx,ax
.width:
    mov ax,[ui_height]
    sub ax,dx
    cmp si,ax
    jbe .height
    mov si,ax
.height:
    add cx,bx
    add si,dx
    cmp cx,[ui_width]
    jbe .right
    mov cx,[ui_width]
.right:
    cmp si,[ui_height]
    jbe .bottom
    mov si,[ui_height]
.bottom:
    cmp byte [ui_comp_dirty],1
    je .extend
    mov [ui_comp_left],bx
    mov [ui_comp_right],cx
    mov [ui_comp_limit_top],dx
    mov [ui_comp_limit_bottom],si
    mov byte [ui_comp_dirty],1
    jmp .done
.extend:
    cmp bx,[ui_comp_left]
    jae .top
    mov [ui_comp_left],bx
.top:
    cmp dx,[ui_comp_limit_top]
    jae .wide
    mov [ui_comp_limit_top],dx
.wide:
    cmp cx,[ui_comp_right]
    jbe .tall
    mov [ui_comp_right],cx
.tall:
    cmp si,[ui_comp_limit_bottom]
    jbe .done
    mov [ui_comp_limit_bottom],si
.done:
    popad
    ret

ui_comp_damage_drag:
    pushad
    cmp byte [ui_dragging],3
    je .native
    mov bx,[ui_wx]
    mov dx,[ui_wy]
    mov cx,[ui_ww]
    add cx,5
    mov si,[ui_wh]
    add si,6
    jmp .damage
.native:
    movzx bx,byte [ui_window_drag]
    call ui_window_bounds
    add cx,5
    add si,6
.damage:
    call ui_comp_damage
    popad
    ret

ui_comp_seg dw 0
ui_comp_stride dw 0
ui_comp_rows dw 0
ui_comp_top dw 0
ui_comp_bottom dw 0
ui_composing db 0
ui_comp_dirty db 0
ui_comp_left dw 0
ui_comp_right dw 0
ui_comp_limit_top dw 0
ui_comp_limit_bottom dw 0
ui_comp_x_bytes dw 0
ui_comp_row_bytes dw 0
ui_comp_glyph_x dw 0
ui_comp_cursor_band db 0
ui_page_enabled db 0
ui_page_pending db 0
ui_page_failed db 0
ui_comp_recover db 0
ui_front_base dd 0
ui_present_base dd 0
ui_page_scanline dw 0
ui_page_previous_left dw 0
ui_page_previous_right dw 0
ui_page_previous_top dw 0
ui_page_previous_bottom dw 0
ui_page_next_left dw 0
ui_page_next_right dw 0
ui_page_next_top dw 0
ui_page_next_bottom dw 0
