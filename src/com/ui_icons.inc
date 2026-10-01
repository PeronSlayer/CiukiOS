; Official Tango/Ciuki system sprites. Source attribution and deterministic
; conversion are recorded in assets/icons/. No disk access occurs in paint.
; Allocate for each desktop video session, then release before DOS/EXEC.
UI_ICONS_COUNT equ 36
UI_ICONS_SIZE equ 32
UI_ICONS_BYTES equ (16 + UI_ICONS_COUNT * UI_ICONS_SIZE * UI_ICONS_SIZE)

ui_icons_begin:
    pushfd
    pushad
    push ds
    push es
    push cs
    pop ds
    cmp word [ui_icons_seg],0
    jne .done
    mov dx,ui_icons_path
    mov ax,0x3D00
    int 0x21
    jc .done
    mov [ui_icons_handle],ax
    mov bx,(UI_ICONS_BYTES + 15) / 16
    mov ah,0x48
    int 0x21
    jc .fail
    mov [ui_icons_seg],ax
    mov bx,[ui_icons_handle]
    mov ds,ax
    xor dx,dx
    mov cx,UI_ICONS_BYTES
    mov ah,0x3F
    int 0x21
    push cs
    pop ds
    jc .fail
    cmp ax,UI_ICONS_BYTES
    jne .fail
    ; Reject trailing data as well as truncated files. This probe never
    ; writes beyond the allocated sprite block.
    mov bx,[ui_icons_handle]
    mov dx,ui_icons_extra
    mov cx,1
    mov ah,0x3F
    int 0x21
    jc .fail
    test ax,ax
    jnz .fail
    mov es,[ui_icons_seg]
    cmp dword [es:0],'CIUK'
    jne .fail
    cmp dword [es:4],'ICO1'
    jne .fail
    cmp word [es:8],UI_ICONS_COUNT
    jne .fail
    cmp word [es:10],UI_ICONS_SIZE
    jne .fail
    cmp word [es:12],UI_ICONS_SIZE
    jne .fail
    cmp word [es:14],0
    jne .fail
    mov di,16
    mov cx,UI_ICONS_BYTES - 16
.validate:
    mov al,[es:di]
    cmp al,255
    je .valid_pixel
    cmp al,VC_PALETTE_COLORS
    jae .fail
.valid_pixel:
    inc di
    loop .validate
    call ui_icons_close
    jmp .done
.fail:
    call ui_icons_close
    call ui_icons_end
.done:
    pop es
    pop ds
    popad
    popfd
    ret

ui_icons_close:
    mov bx,[ui_icons_handle]
    mov ah,0x3E
    int 0x21
    mov word [ui_icons_handle],0xFFFF
    ret

ui_icons_end:
    pushfd
    pushad
    push es
    mov ax,[ui_icons_seg]
    test ax,ax
    jz .done
    mov es,ax
    mov ah,0x49
    int 0x21
    mov word [ui_icons_seg],0
.done:
    pop es
    popad
    popfd
    ret

; AL=icon ID, BX=x, DX=y. Render native 32px sprites centred inside the
; existing 44x42 slots. FS keeps the external source separate from all UI
; state in DS, while ui_rect retains its existing clipping/colour contract.
ui_icons_draw:
    pushfd
    pushad
    push fs
    cmp al,UI_ICONS_COUNT
    jae .done
    movzx di,al
    shl di,10
    add di,16
    mov ax,[ui_icons_seg]
    test ax,ax
    jz .done
    mov fs,ax
    push bp
    mov bp,sp
    sub sp,8
    add bx,6
    add dx,4
    mov [bp-2],bx
    mov [bp-4],dx
    mov word [bp-6],0
.row:
    cmp byte [ui_composing],1
    jne .row_visible
    mov ax,[bp-4]
    add ax,[bp-6]
    cmp ax,[ui_comp_top]
    jl .skip_row
    cmp ax,[ui_comp_bottom]
    jge .skip_row
.row_visible:
    mov word [bp-8],0
.pixel:
    mov bx,[bp-8]
    cmp bx,UI_ICONS_SIZE
    jae .next_row
    mov al,[fs:di]
    inc di
    mov cx,1
.merge:
    mov dx,bx
    add dx,cx
    cmp dx,UI_ICONS_SIZE
    jae .span
    cmp al,[fs:di]
    jne .span
    inc di
    inc cx
    jmp .merge
.span:
    add [bp-8],cx
    cmp al,255
    je .pixel
    cmp byte [ui_vbe],1
    je .colour_ready
    cmp al,16
    jb .colour_ready
    movzx bx,al
    sub bx,16
    mov al,[ui_icons_fallback+bx]
.colour_ready:
    mov bx,[bp-8]
    sub bx,cx
    add bx,[bp-2]
    mov dx,[bp-6]
    add dx,[bp-4]
    mov si,1
    call ui_rect
    jmp .pixel
.skip_row:
    add di,UI_ICONS_SIZE
.next_row:
    inc word [bp-6]
    cmp word [bp-6],UI_ICONS_SIZE
    jb .row
    mov sp,bp
    pop bp
.done:
    pop fs
    popad
    popfd
    ret

ui_icons_seg dw 0
ui_icons_handle dw 0xFFFF
ui_icons_extra db 0
ui_icons_path db '\SYSTEM\UI\ICONS.DAT',0
ui_icons_fallback:
    incbin "assets/icons/native/fallback.bin"
%if ($-ui_icons_fallback) != (CIUKI_LOGO_COLOR_COUNT + UI_ICONS_COLOR_COUNT)
%error "Invalid system sprite planar fallback map"
%endif
