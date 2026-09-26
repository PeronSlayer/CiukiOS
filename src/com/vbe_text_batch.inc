; Consoles paint in scanline order (banked video needs it). A tall 32-bit glyph alone
; can cross several banks; printing a directory character-by-character then
; scrolling every intermediate state otherwise spends most time in the BIOS.
vc_batch_reset:
    pushad
    push es
    push cs
    pop es
    mov di,vc_dirty_first
    mov ax,0xFFFF
    mov cx,VC_ROW_LIMIT
    rep stosw
    mov di,vc_dirty_last
    xor ax,ax
    mov cx,VC_ROW_LIMIT
    rep stosw
    mov byte [vc_batch],1
    mov ax,0x40
    mov es,ax
    mov ax,[es:0x6C]
    mov [vc_batch_tick],ax
    pop es
    popad
    ret

vc_queue_cell:
    pushad
    mov ax,bx
    xor dx,dx
    div word [cs:vc_cols]
    cmp ax,[cs:vc_rows]
    jae .done
    mov si,ax
    shl si,1
    cmp dx,[cs:vc_dirty_first+si]
    jae .first
    mov [cs:vc_dirty_first+si],dx
.first:
    cmp dx,[cs:vc_dirty_last+si]
    jbe .done
    mov [cs:vc_dirty_last+si],dx
.done:
    popad
    ret

vc_flush:
    ; Keep scanline order: each pass composes up to 512 bytes of consecutive
    ; cells in memory and writes them with one framebuffer transfer.
    pushad
    push ds
    push es
    push fs
    push gs
    push cs
    pop ds
    push cs
    pop es
    cld
    cmp byte [vc_active],1
    jne .done
    mov gs,[vc_cells_seg]
    mov fs,[vc_font_seg]
    movzx ax,byte [vc_bytes]
    shl ax,3
    mov [vc_batch_cell_bytes],ax
    xor bp,bp
.line:
    mov si,bp
    shl si,1
    cmp word [vc_dirty_first+si],0xFFFF
    je .next
    mov ax,[vc_dirty_first+si]
    mov [vc_batch_first],ax
    mov ax,[vc_dirty_last+si]
    inc ax
    mov [vc_batch_last],ax
    mov word [vc_dirty_first+si],0xFFFF
    mov word [vc_dirty_last+si],0
    mov word [vc_batch_scan],0
.scan:
    movzx edi,bp
    shl edi,4
    movzx eax,word [vc_batch_scan]
    add edi,eax
    movzx eax,word [vc_pitch]
    imul edi,eax
    movzx eax,word [vc_batch_first]
    movzx ecx,word [vc_batch_cell_bytes]
    imul eax,ecx
    add edi,eax
    mov ax,bp
    mul word [vc_cols]
    add ax,[vc_batch_first]
    mov bx,ax
    mov ax,[vc_batch_last]
    sub ax,[vc_batch_first]
    mov [vc_batch_left],ax
    call vc_fb_begin
.chunk:
    mov es,[vc_cells_seg]      ; a burst leaves ES undefined
    push edi
    mov di,[vc_cell_off]
    mov dx,[vc_batch_scan]
    xor cx,cx
.cell:
    call vc_cell_scan
    inc bx
    add cx,[vc_batch_cell_bytes]
    dec word [vc_batch_left]
    jz .write
    mov ax,cx
    add ax,[vc_batch_cell_bytes]
    cmp ax,16*8*4
    jbe .cell
.write:
    pop edi
    push fs
    mov fs,[vc_cells_seg]
    mov si,[vc_cell_off]
    call vc_fb_put
    pop fs
    movzx eax,cx
    add edi,eax
    cmp word [vc_batch_left],0
    jne .chunk
    call vc_fb_end
    inc word [vc_batch_scan]
    cmp word [vc_batch_scan],16
    jb .scan
.next:
    inc bp
    cmp bp,[vc_rows]
    jb .line
.done:
    mov byte [vc_batch],0
    call vc_cursor
    pop gs
    pop fs
    pop es
    pop ds
    popad
    ret

vc_maybe_flush:
    pushad
    push es
    cmp byte [cs:vc_batch],1
    jne .done
    mov ax,0x40
    mov es,ax
    mov ax,[es:0x6C]
    mov dx,ax
    sub ax,[cs:vc_batch_tick]
    cmp ax,18
    jb .done
    mov [cs:vc_batch_tick],dx
    call vc_flush
    mov byte [cs:vc_batch],1
.done:
    pop es
    popad
    ret

vc_batch db 1
vc_batch_scan dw 0
vc_batch_first dw 0
vc_batch_last dw 0
vc_batch_left dw 0
vc_batch_cell_bytes dw 0
vc_batch_tick dw 0
vc_dirty_first times VC_ROW_LIMIT dw 0xFFFF
vc_dirty_last times VC_ROW_LIMIT dw 0
