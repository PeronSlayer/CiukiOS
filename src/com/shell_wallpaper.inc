; Optional native-size indexed wallpaper tiles; no DOS calls during painting.
; Window 10 uses the shared 466x226 frame. Refresh imports an updated catalog.
WP_WINDOW equ 10
WP_MAX equ 99
WP_CATALOG_BYTES equ 4608

wp_begin:
    pushad
    push es
    push fs
    call wp_catalog_end
    push cs
    pop es
    cld
    mov word [wp_page],0
    mov word [wp_count],0
    mov word [wp_status],wp_catalog_error
    mov bx,WP_CATALOG_BYTES/16
    mov ah,0x48
    int 0x21
    jc .done
    mov [wp_catalog_seg],ax
    mov fs,ax
    mov dx,wp_catalog_path
    mov ax,0x3D00
    int 0x21
    jc .done
    mov bx,ax
    mov ds,[wp_catalog_seg]
    xor dx,dx
    mov cx,WP_CATALOG_BYTES
    mov ah,0x3F
    int 0x21
    push cs
    pop ds
    pushf
    push ax
    mov ah,0x3E
    int 0x21
    pop ax
    popf
    jc .done
    cmp ax,8
    jb .done
    mov dx,ax
    cmp dword [fs:0],'CWC1'
    jne .done
    cmp word [fs:6],0
    jne .done
    mov ax,[fs:4]
    cmp ax,WP_MAX
    ja .done
    imul cx,ax,44
    add cx,8
    cmp dx,cx
    jb .done
    mov [wp_count],ax
    mov word [wp_status],wp_hint
    ; Titles are bounded printable ASCII, never unterminated foreign data.
    xor bp,bp
.title:
    cmp bp,[wp_count]
    jae .config
    imul si,bp,44
    add si,8+13
    mov cx,30
.character:
    mov al,[fs:si]
    test al,al
    jz .title_end
    cmp al,32
    jb .bad
    cmp al,126
    ja .bad
    inc si
    loop .character
.title_end:
    mov byte [fs:si],0
    inc bp
    jmp .title
.bad:
    mov word [wp_status],wp_catalog_error
    mov word [wp_count],0
    jmp .done
.config:
    call wp_discover
    cmp word [wp_count],0
    jne .read_config
    mov word [wp_status],wp_empty
.read_config:
    mov byte [wp_selected],0
    mov byte [wp_style],0            ; Fill, including legacy one-byte profiles
    ; The owner's Ciuk1 photo is the default even without WALL.CFG.
    xor bp,bp
.default_photo:
    cmp bp,[wp_count]
    jae .config_open
    imul si,bp,44
    add si,8+13
    cmp dword [fs:si],'Ciuk'
    jne .default_next
    cmp word [fs:si+4],'1'
    jne .default_next
    mov ax,bp
    inc ax
    mov [wp_selected],al
    jmp .config_open
.default_next:
    inc bp
    jmp .default_photo
.config_open:
    mov dx,wp_config_path
    mov ax,0x3D00
    int 0x21
    jc .load
    mov bx,ax
    mov dx,wp_config_byte
    mov cx,3                       ; reject trailing bytes; support index/style
    mov ah,0x3F
    int 0x21
    pushf
    push ax
    mov ah,0x3E
    int 0x21
    pop ax
    popf
    jc .load
    cmp ax,1
    je .config_index
    cmp ax,2
    jne .load
    cmp byte [wp_config_byte+1],4
    ja .load
    mov dl,[wp_config_byte+1]
    mov [wp_style],dl
.config_index:
    movzx ax,byte [wp_config_byte]
    cmp ax,[wp_count]
    ja .load
    mov [wp_selected],al
.load:
    mov al,[wp_selected]
    mov [wp_choice],al
    call wp_load
.done:
    pop fs
    pop es
    popad
    ret

wp_end:
    call wp_tile_end
wp_catalog_end:
    pushad
    push es
    mov ax,[wp_catalog_seg]
    test ax,ax
    jz .done
    mov es,ax
    mov ah,0x49
    int 0x21
    mov word [wp_catalog_seg],0
.done:
    pop es
    popad
    ret

wp_tile_end:
    pushad
    push es
    mov ax,[wp_seg]
    test ax,ax
    jz .done
    mov es,ax
    mov ah,0x49
    int 0x21
    mov word [wp_seg],0
.done:
    pop es
    popad
    ret

; Allocate a replacement before releasing the current texture. Failures keep
; the old pixels/geometry valid. At most 66560 bytes per prepared tile.
wp_load:
    cmp byte [wp_choice],0
    jne .image
    call wp_tile_end
    mov byte [wp_kind],0
    inc word [wp_generation]
    clc
    ret
.image:
    mov al,[wp_choice]
    call wp_path
    mov word [wp_new_seg],0
    mov dx,wp_tile_path
    mov ax,0x3D00
    int 0x21
    jc .fail
    mov [wp_handle],ax
    mov bx,ax
    mov dx,wp_header
    mov cx,16
    mov ah,0x3F
    int 0x21
    jc .cleanup
    cmp ax,16
    jne .cleanup
    cmp dword [wp_header],'CWP2'
    je .photo
    cmp byte [ui_vbe],1
    jne .close_unsupported
    cmp word [ui_comp_seg],0
    je .close_unsupported
    cmp dword [wp_header],'CWP1'
    jne .cleanup
    cmp dword [wp_header+8],256
    jne .cleanup
    mov ax,[wp_header+4]
    dec ax
    cmp ax,255
    ja .cleanup
    mov bx,[wp_header+6]
    dec bx
    cmp bx,255
    ja .cleanup
    inc ax
    inc bx
    movzx eax,ax
    movzx ebx,bx
    imul eax,ebx
    cmp eax,[wp_header+12]
    jne .cleanup
    add eax,1024+15
    shr eax,4
    mov bx,ax
    mov ah,0x48
    int 0x21
    jc .cleanup
    mov [wp_new_seg],ax
    mov ds,ax
    xor dx,dx
    mov cx,768
    call wp_read
    jc .cleanup
    cmp ax,768
    jne .cleanup
    mov es,[wp_new_seg]
    mov bp,255
.palette:
    imul si,bp,3
    call wp_color
    mov di,bp
    shl di,2
    mov [es:di],eax
    dec bp
    jns .palette
    mov cx,[wp_header+12]
    cmp word [wp_header+14],0
    je .first_count
    mov cx,0xFFFF
.first_count:
    mov [wp_read_count],cx
    mov ax,[wp_new_seg]
    add ax,64
    mov ds,ax
    xor dx,dx
    call wp_read
    jc .cleanup
    cmp ax,[wp_read_count]
    jne .cleanup
    cmp word [wp_header+14],0
    je .eof
    mov ax,[wp_new_seg]
    add ax,64
    mov ds,ax
    mov dx,0xFFFF
    mov cx,1
    call wp_read
    jc .cleanup
    cmp ax,1
    jne .cleanup
.eof:
    mov dx,wp_config_byte+1
    mov cx,1
    call wp_read
    jc .cleanup
    test ax,ax
    jnz .cleanup
    call wp_close
    jc .cleanup_memory
    call wp_tile_end
    mov ax,[wp_new_seg]
    mov [wp_seg],ax
    mov ax,[wp_header+4]
    mov [wp_width],ax
    mov ax,[wp_header+6]
    mov [wp_height],ax
    mov byte [wp_kind],1
    inc word [wp_generation]
    push cs
    pop es
    clc
    ret
.photo:
    cmp dword [wp_header+4],(941 << 16) | 1672
    jne .cleanup
    cmp dword [wp_header+8],(1 << 16) | 5016
    jne .cleanup
    cmp dword [wp_header+12],4720056
    jne .cleanup
    mov bx,[wp_handle]
    xor cx,cx
    xor dx,dx
    mov ax,0x4202
    int 0x21
    jc .cleanup
    cmp dx,(4720072 >> 16)
    jne .cleanup
    cmp ax,(4720072 & 0xFFFF)
    jne .cleanup
    call wp_close
    jc .fail
    call wp_tile_end
    mov byte [wp_kind],2
    inc word [wp_generation]
    push cs
    pop es
    clc
    ret
.close_unsupported:
    call wp_close
    jmp .unsupported
.cleanup:
    call wp_close
.cleanup_memory:
    mov ax,[wp_new_seg]
    test ax,ax
    jz .fail
    mov es,ax
    mov ah,0x49
    int 0x21
.fail:
    push cs
    pop es
    mov word [wp_status],wp_load_error
    stc
    ret
.unsupported:
    mov word [wp_status],wp_solid_fallback
    stc
    ret

; Add consecutively numbered CWP files copied through Files/Media, without
; replacing WALLS.DAT. Existing catalog titles and indices remain stable.
wp_discover:
    cmp word [wp_count],WP_MAX
    jae .done
    mov ax,[wp_count]
    inc ax
    call wp_path
    mov dx,wp_tile_path
    mov ax,0x3D00
    int 0x21
    jc .done
    mov bx,ax
    mov ah,0x3E
    int 0x21
    imul di,[wp_count],44
    add di,8
    push es
    mov es,[wp_catalog_seg]
    mov si,wp_file_name
    mov cx,13
    rep movsb
    push di
    xor ax,ax
    mov cx,31
    rep stosb
    pop di
    mov si,wp_file_name
    mov cx,10
    rep movsb
    pop es
    inc word [wp_count]
    jmp wp_discover
.done:
    ret

wp_path:
    aam
    add ax,0x3030
    mov [wp_digits],ah
    mov [wp_digits+1],al
    ret

wp_read:
    mov bx,[cs:wp_handle]
    mov ah,0x3F
    int 0x21
    push cs
    pop ds
    ret
wp_close:
    mov bx,[wp_handle]
    mov ah,0x3E
    int 0x21
    ret

; ES:SI source RGB8 -> EAX current framebuffer pixel. Palette is converted
; backwards, so its expanding 3-to-4-byte representation never overwrites an
; unread source triple. BP (outer palette index) survives both branches.
wp_color:
    push bp
    cmp byte [vc_bytes],1
    je .indexed
    xor edx,edx
    mov bx,vc_info+31
    mov bp,3
.channel:
    movzx eax,byte [es:si]
    mov cl,8
    sub cl,[bx]
    shr eax,cl
    mov cl,[bx+1]
    shl eax,cl
    or edx,eax
    inc si
    add bx,2
    dec bp
    jnz .channel
    mov eax,edx
    pop bp
    ret
.indexed:
    mov word [wp_best_distance],0xFFFF
    xor bp,bp
    mov di,ui_palette
.candidate:
    xor dx,dx
    mov cx,3
.distance:
    movzx ax,byte [es:si]
    shr ax,2
    movzx bx,byte [di]
    sub ax,bx
    imul ax,ax
    add dx,ax
    inc si
    inc di
    loop .distance
    sub si,3
    cmp dx,[wp_best_distance]
    jae .next
    mov [wp_best_distance],dx
    mov [wp_best_index],bp
.next:
    inc bp
    cmp bp,VC_PALETTE_COLORS
    jb .candidate
    movzx eax,word [wp_best_index]
    pop bp
    ret

; Paint only the current compositor band/damage rectangle. Each source pixel
; has already been converted in the lookup table. FS/GS never escape this call.
wp_background:
    cmp byte [ui_composing],1
    jne .done
    cmp word [wp_seg],0
    je .done
    pushad
    push es
    push fs
    push gs
    mov ax,[ui_comp_right]
    cmp ax,[ui_comp_left]
    jbe .restore
    mov ax,[ui_comp_bottom]
    cmp ax,[ui_comp_top]
    jbe .restore
    mov ax,[wp_seg]
    mov gs,ax
    add ax,64
    mov fs,ax
    mov es,[ui_comp_seg]
    mov ax,[ui_comp_top]
    xor dx,dx
    div word [wp_height]
    mov [wp_row],dx
    mov ax,[ui_comp_left]
    xor dx,dx
    div word [wp_width]
    mov [wp_column],dx
    mov ax,[ui_comp_bottom]
    sub ax,[ui_comp_top]
    mov [wp_rows],ax
    xor di,di
    cld
.row:
    mov ax,[wp_row]
    mul word [wp_width]
    add ax,[wp_column]
    mov si,ax
    mov bp,[wp_width]
    sub bp,[wp_column]
    mov cx,[ui_comp_right]
    sub cx,[ui_comp_left]
.pixel:
    movzx bx,byte [fs:si]
    inc si
    shl bx,2
    mov eax,[gs:bx]
    cmp byte [vc_bytes],4
    je .dword
    cmp byte [vc_bytes],2
    je .word
    cmp byte [vc_bytes],3
    je .triple
    stosb
    jmp .advance
.dword:
    stosd
    jmp .advance
.word:
    stosw
    jmp .advance
.triple:
    stosw
    shr eax,16
    stosb
.advance:
    dec bp
    jnz .continue
    sub si,[wp_width]
    mov bp,[wp_width]
.continue:
    loop .pixel
    inc word [wp_row]
    mov ax,[wp_row]
    cmp ax,[wp_height]
    jb .next_row
    mov word [wp_row],0
.next_row:
    dec word [wp_rows]
    jnz .row
.restore:
    pop gs
    pop fs
    pop es
    popad
.done:
    ret

wp_open:
    mov al,[wp_selected]
    mov [wp_choice],al
    mov byte [ui_dialog],WP_WINDOW
    call ui_windows_open
    ret

; Actions 180 Solid,181 previous,182 next,183 apply,184..188 visible choices.
wp_action:
    cmp ax,180
    je .solid
    cmp ax,181
    je .previous
    cmp ax,182
    je .next
    cmp ax,183
    je .apply
    cmp ax,189
    je .refresh
    sub ax,184
    cmp ax,5
    jae .idle
    add ax,[wp_page]
    inc ax
    cmp ax,[wp_count]
    ja .idle
    mov [wp_choice],al
    jmp .redraw
.solid:
    mov byte [wp_choice],0
    jmp .redraw
.previous:
    cmp word [wp_page],0
    je .idle
    sub word [wp_page],5
    jmp .redraw
.next:
    mov ax,[wp_page]
    add ax,5
    cmp ax,[wp_count]
    jae .idle
    mov [wp_page],ax
    jmp .redraw
.refresh:
    call wp_begin
    mov byte [ui_full_redraw],1
    jmp .redraw
.apply:
    call wp_load
    jc .redraw
    mov al,[wp_choice]
    mov [wp_selected],al
    mov word [wp_status],wp_applied
    mov byte [ui_full_redraw],1
    call wp_save_config
    jnc .redraw
    mov word [wp_status],wp_save_error
    jmp .redraw
.redraw:
    mov ax,250
    ret
.idle:
    xor ax,ax
    ret

; Shared by the legacy panel and Display.APP. CF reports actual persistence.
wp_save_config:
    mov al,[wp_selected]
    mov [wp_config_byte],al
    mov al,[wp_style]
    mov [wp_config_byte+1],al
    mov dx,wp_config_path
    xor cx,cx
    mov ah,0x3C
    int 0x21
    jc .fail
    mov bx,ax
    mov dx,wp_config_byte
    mov cx,2
    mov ah,0x40
    int 0x21
    pushf
    push ax
    mov ah,0x3E
    int 0x21
    pop dx
    pop ax
    jc .fail
    test ax,1
    jnz .fail
    cmp dx,2
    jne .fail
    clc
    ret
.fail:
    stc
    ret

; Body only, called by generic frame painter with ui_dialog_x/y established.
wp_draw:
    pushad
    push fs
    mov fs,[wp_catalog_seg]
    mov bx,[ui_dialog_x]
    add bx,20
    mov dx,[ui_dialog_y]
    add dx,40
    mov cx,95
    mov si,25
    mov di,wp_solid
    mov ax,180
    call ui_button
    cmp byte [wp_choice],0
    jne .items
    add bx,6
    add dx,4
    mov si,wp_chosen
    mov ax,0x0101
    call ui_text
.items:
    xor bp,bp
.item:
    mov ax,[wp_page]
    add ax,bp
    cmp ax,[wp_count]
    jae .footer
    imul si,ax,44
    add si,8+13
    mov di,wp_label
    mov cx,31
.copy_label:
    mov al,[fs:si]
    inc si
    mov [di],al
    inc di
    loop .copy_label
    mov di,wp_label
    mov bx,[ui_dialog_x]
    add bx,126
    imul dx,bp,24
    add dx,[ui_dialog_y]
    add dx,36
    mov cx,320
    mov si,23
    mov ax,bp
    add ax,184
    call ui_button
    mov ax,[wp_page]
    add ax,bp
    inc ax
    cmp al,[wp_choice]
    jne .next_item
    add bx,6
    add dx,4
    mov si,wp_chosen
    mov ax,0x0101
    call ui_text
.next_item:
    inc bp
    cmp bp,5
    jb .item
.footer:
    mov bx,[ui_dialog_x]
    add bx,20
    mov dx,[ui_dialog_y]
    add dx,158
    mov si,[wp_status]
    xor ax,ax
    call ui_text
    mov bx,[ui_dialog_x]
    add bx,20
    mov dx,[ui_dialog_y]
    add dx,188
    mov cx,100
    mov si,26
    mov di,wp_previous
    mov ax,181
    call ui_button
    add bx,108
    mov di,wp_next
    mov ax,182
    call ui_button
    add bx,108
    mov di,wp_refresh
    mov ax,189
    call ui_button
    add bx,100
    mov di,wp_apply
    mov ax,183
    call ui_button
    pop fs
    popad
    ret

wp_title db 'Wallpaper',0
wp_solid db 'Solid',0
wp_previous db 'Previous',0
wp_next db 'Next',0
wp_apply db 'Apply',0
wp_chosen db '>',0
wp_hint db 'Copy tiles to \SYSTEM\UI; then Refresh.',0
wp_refresh db 'Refresh',0
wp_catalog_error db 'Wallpaper catalog missing or damaged.',0
wp_empty db 'No wallpaper tiles are installed.',0
wp_applied db 'Wallpaper applied.',0
wp_load_error db 'Wallpaper could not be loaded.',0
wp_save_error db 'Applied; preference could not be saved.',0
wp_solid_fallback db 'This video mode uses the solid background.',0
wp_catalog_path db '\SYSTEM\UI\WALLS.DAT',0
wp_config_path db '\SYSTEM\UI\WALL.CFG',0
wp_tile_path db '\SYSTEM\UI\'
wp_file_name db 'WALL'
wp_digits db '00.CWP',0,0,0
wp_status dw wp_hint
wp_seg dw 0
wp_new_seg dw 0
wp_handle dw 0
wp_count dw 0
wp_page dw 0
wp_selected db 0
wp_choice db 0
wp_kind db 0                          ; 0 solid, 1 indexed tile, 2 RGB photo
wp_style db 0                         ; Fill/Fit/Stretch/Center/Tile
wp_generation dw 0
wp_width dw 0
wp_height dw 0
wp_read_count dw 0
wp_row dw 0
wp_column dw 0
wp_rows dw 0
wp_best_distance dw 0
wp_best_index dw 0
wp_config_byte times 3 db 0
wp_header times 16 db 0
wp_catalog_seg dw 0
wp_label times 31 db 0
