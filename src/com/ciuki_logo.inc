; Runtime icons are compiled from the exact approved Ciuki portrait.
; Source/manifest: assets/brand/ciuki-logo.png and assets/brand/native/manifest.json.
; No hand-drawn replacement, PNG decoding, resizing, or allocation at runtime.
; Entry: BX=x, DX=y. Preserve caller registers, segments and flags.
; CIUKI_LOGO_RECT is ui_rect/g_rect; CIUKI_LOGO_VBE is ui_vbe/g_vbe.
%ifndef CIUKI_LOGO_RECT
%error "CIUKI_LOGO_RECT must name the existing rectangle primitive"
%endif
%ifndef CIUKI_LOGO_VBE
%error "CIUKI_LOGO_VBE must name the caller's packed-mode flag"
%endif

%macro CIUKI_LOGO_ENTRY 1
ciuki_logo%1:
    pushfd
    pushad
%ifdef CIUKI_LOGO_EXTERNAL_SEG
    push fs
    mov fs,[CIUKI_LOGO_EXTERNAL_SEG]
%endif
    mov di,ciuki_logo%1_pixels
    mov cx,%1
    call ciuki_logo_draw
%ifdef CIUKI_LOGO_EXTERNAL_SEG
    pop fs
%endif
    popad
    popfd
    ret
%endmacro
CIUKI_LOGO_ENTRY 16
CIUKI_LOGO_ENTRY 24
CIUKI_LOGO_ENTRY 64
%unmacro CIUKI_LOGO_ENTRY 1

ciuki_logo_draw:
    ; Stack-local origin, width, row and column. The existing rect callback
    ; preserves registers; SS-relative locals also support Setup's negative
    ; logical origin without changing its viewport or clipping contract.
    push bp
    mov bp,sp
    sub sp,10
    mov [bp-2],bx
    mov [bp-4],dx
    mov [bp-6],cx
    mov word [bp-8],0
.row:
%ifdef CIUKI_LOGO_COMPOSING
    ; Bands outside a sprite row need no source-pixel decoding or draw calls.
    ; This also preserves Setup's physical +60 viewport at negative origins.
    cmp byte [CIUKI_LOGO_COMPOSING],1
    jne .row_visible
    mov ax,[bp-4]
    add ax,[bp-8]
    add ax,CIUKI_LOGO_ORIGIN_Y
    cmp ax,[CIUKI_LOGO_BAND_TOP]
    jl .skip_row
    cmp ax,[CIUKI_LOGO_BAND_BOTTOM]
    jge .skip_row
.row_visible:
%endif
    mov word [bp-10],0
.pixel:
    mov bx,[bp-10]
    cmp bx,[bp-6]
    jae .next_row
%ifdef CIUKI_LOGO_EXTERNAL_SEG
    mov al,[fs:di]
%else
    mov al,[di]
%endif
    inc di
    mov cx,1
.merge:
    mov dx,bx
    add dx,cx
    cmp dx,[bp-6]
    jae .span
%ifdef CIUKI_LOGO_EXTERNAL_SEG
    cmp al,[fs:di]
%else
    cmp al,[di]
%endif
    jne .span
    inc di
    inc cx
    jmp .merge
.span:
    add [bp-10],cx
    cmp al,255
    je .pixel
    cmp byte [CIUKI_LOGO_VBE],1
    je .colour_ready
    movzx bx,al
    sub bx,16
    mov al,[ciuki_logo_fallback+bx]
.colour_ready:
    mov bx,[bp-10]
    sub bx,cx
    add bx,[bp-2]
    mov dx,[bp-8]
    add dx,[bp-4]
    mov si,1
    call CIUKI_LOGO_RECT
    jmp .pixel
.skip_row:
    add di,[bp-6]
.next_row:
    inc word [bp-8]
    mov ax,[bp-8]
    cmp ax,[bp-6]
    jb .row
    mov sp,bp
    pop bp
    ret

%ifdef CIUKI_LOGO_EXTERNAL_SEG
ciuki_logo16_pixels equ UI_ASSET_LOGO16
ciuki_logo24_pixels equ UI_ASSET_LOGO24
ciuki_logo64_pixels equ UI_ASSET_LOGO64
%else
ciuki_logo16_pixels:
    incbin "assets/brand/native/ciuki-16.bin"
%if ($-ciuki_logo16_pixels) != 16*16
%error "Invalid compiled 16px logo size"
%endif
ciuki_logo24_pixels:
    incbin "assets/brand/native/ciuki-24.bin"
%if ($-ciuki_logo24_pixels) != 24*24
%error "Invalid compiled 24px logo size"
%endif
ciuki_logo64_pixels:
    incbin "assets/brand/native/ciuki-64.bin"
%if ($-ciuki_logo64_pixels) != 64*64
%error "Invalid compiled 64px logo size"
%endif
%endif
ciuki_logo_fallback:
    incbin "assets/brand/native/fallback.bin"
%if ($-ciuki_logo_fallback) != 64
%error "Invalid planar fallback map size"
%endif
%undef CIUKI_LOGO_RECT
%undef CIUKI_LOGO_VBE
%undef CIUKI_LOGO_COMPOSING
%undef CIUKI_LOGO_BAND_TOP
%undef CIUKI_LOGO_BAND_BOTTOM
%undef CIUKI_LOGO_ORIGIN_Y
%undef CIUKI_LOGO_EXTERNAL_SEG
