; Immutable artwork lives in a separate PSP-owned block, freeing code-segment
; space for real native applications. Release it before DOS/EXEC and reload
; on desktop return so child programs retain their conventional memory.
UI_ASSET_BYTES equ 11214
ui_hits equ UI_ASSET_BYTES
ui_mouse_state equ ui_hits+2560
; Mutable renderer workspace belongs to the desktop resource block, not
; SHELL.COM's 16-bit code arena. Artwork file bytes and its format stay fixed.
ui_pointer_under equ ui_mouse_state+512
ui_catch_a equ ui_pointer_under+2*24*16*4
ui_catch_b equ ui_catch_a+UI_CATCH_MAX*8
vc_pointer_row equ ui_catch_b+UI_CATCH_MAX*8
UI_ASSET_ALLOC equ vc_pointer_row+24*16*4
ui_cursor_table equ UI_ASSET_ALLOC
UI_ASSET_TOTAL_ALLOC equ ui_cursor_table+512
UI_ASSET_FONT equ 16
UI_ASSET_LOGO16 equ (UI_ASSET_FONT+6270)
UI_ASSET_LOGO24 equ (UI_ASSET_LOGO16+256)
UI_ASSET_LOGO64 equ (UI_ASSET_LOGO24+576)

ui_assets_begin:
    pushad
    push ds
    push es
    push cs
    pop ds
    cmp word [ui_assets_seg],0
    jne .ready
    mov dx,ui_assets_path
    mov ax,0x3D00
    int 0x21
    jc .fail
    mov [ui_assets_handle],ax
    mov bx,(UI_ASSET_TOTAL_ALLOC+15)/16
    mov ah,0x48                        ; conventional: the UI gates read it
    int 0x21
    jc .close_fail
    mov [ui_assets_seg],ax
    mov ds,ax
    mov bx,[cs:ui_assets_handle]
    xor dx,dx
    mov cx,UI_ASSET_BYTES
    mov ah,0x3F
    int 0x21
    push cs
    pop ds
    jc .free_fail
    cmp ax,UI_ASSET_BYTES
    jne .free_fail
    mov es,[ui_assets_seg]
    cmp dword [es:0],'CIUK'
    jne .free_fail
    cmp dword [es:4],'UI01'
    jne .free_fail
    cmp word [es:8],6270
    jne .free_fail
    cmp word [es:10],4928
    jne .free_fail
    cmp word [es:12],UI_ASSET_BYTES
    jne .free_fail
    cmp word [es:14],0
    jne .free_fail
    mov bx,[ui_assets_handle]
    mov ah,0x3E
    int 0x21
    jc .release
.ready:
    pop es
    pop ds
    popad
    clc
    ret
.free_fail:
    mov bx,[ui_assets_handle]
    mov ah,0x3E
    int 0x21
.release:
    mov es,[ui_assets_seg]
    mov ah,0x49
    int 0x21
    mov word [ui_assets_seg],0
    jmp .fail
.close_fail:
    mov bx,[ui_assets_handle]
    mov ah,0x3E
    int 0x21
.fail:
    pop es
    pop ds
    popad
    stc
    ret

ui_assets_end:
    pushad
    push es
    mov ax,[ui_assets_seg]
    test ax,ax
    jz .done
    mov es,ax
    mov ah,0x49
    int 0x21
    mov word [ui_assets_seg],0
.done:
    pop es
    popad
    ret

ui_assets_seg dw 0
ui_assets_handle dw 0
ui_assets_path db '\SYSTEM\UI\DESKTOP.DAT',0
ui_assets_error db 'Cannot load \SYSTEM\UI\DESKTOP.DAT. Desktop unavailable.',13,10,'$'
