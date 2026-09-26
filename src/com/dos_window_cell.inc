; External opaque text-cell renderer. DS=host, FS=runtime font segment,
; BX/DX=cell origin, SI=expanded 8x16 glyph, AL=text attribute.
; Header fields describe the current preallocated compositor band. Host
; palette is DS:DW_CELL_PALETTE, 16 native colors stored as dwords.
; Preserves all GPR/segment registers and IF; uses caller's private stack.
; A foreground draw or guarded host callback owns this routine exclusively.
%include "src/com/dos_window_abi.inc"

dw_cell_render:
    pushad
    push es
    cld
    movzx ebp,al
    mov ax,bx
    add ax,8
    cmp bx,[cs:DW_CELL_RIGHT]
    jae .done
    cmp ax,[cs:DW_CELL_LEFT]
    jbe .done
    mov di,bx
    cmp bx,[cs:DW_CELL_LEFT]
    jae .left
    mov bx,[cs:DW_CELL_LEFT]
.left:
    mov cx,bx
    sub cx,di
    mov [cs:dw_clip_skip],cl
    cmp ax,[cs:DW_CELL_RIGHT]
    jbe .right
    mov ax,[cs:DW_CELL_RIGHT]
.right:
    sub ax,bx
    mov [cs:dw_clip_width],ax
    mov ax,dx
    add ax,16
    cmp dx,[cs:DW_CELL_BOTTOM]
    jae .done
    cmp ax,[cs:DW_CELL_TOP]
    jbe .done
    mov di,dx
    cmp dx,[cs:DW_CELL_TOP]
    jae .top
    mov dx,[cs:DW_CELL_TOP]
.top:
    mov cx,dx
    sub cx,di
    shl cx,1
    add si,cx
    cmp ax,[cs:DW_CELL_BOTTOM]
    jbe .bottom
    mov ax,[cs:DW_CELL_BOTTOM]
.bottom:
    sub ax,dx
    mov [cs:dw_clip_height],ax
    mov ax,dx
    sub ax,[cs:DW_CELL_TOP]
    mul word [cs:DW_CELL_STRIDE]
    mov di,ax
    mov ax,bx
    sub ax,[cs:DW_CELL_LEFT]
    movzx cx,byte [cs:DW_CELL_BYTES]
    mul cx
    add di,ax
    mov ax,[cs:DW_CELL_BAND_SEG]
    mov es,ax
    mov bx,bp
    and bx,15
    shl bx,2
    mov cx,[cs:DW_CELL_PALETTE]
    add bx,cx
    mov ebx,[ds:bx]
    shr bp,4
    and bp,7
    shl bp,2
    add bp,cx
    mov ebp,[ds:bp]
    movzx cx,byte [cs:DW_CELL_BYTES]
    mov ax,[cs:DW_CELL_STRIDE]
    imul cx,[cs:dw_clip_width]
    sub ax,cx
    mov [cs:dw_clip_advance],ax
    cmp byte [cs:DW_CELL_BYTES],4
    je .dwords
    cmp byte [cs:DW_CELL_BYTES],2
    je .words
    cmp byte [cs:DW_CELL_BYTES],3
    je .triples
%macro DW_CELL_ROWS 2
.%1:
    mov dl,[fs:si+1]
    mov cl,[cs:dw_clip_skip]
    shl dl,cl
    mov cx,[cs:dw_clip_width]
.pixel_%1:
    shl dl,1
    mov eax,ebp
    jnc .store_%1
    mov eax,ebx
.store_%1:
    %2
%ifidni %1,triples
    shr eax,16
    stosb
%endif
    loop .pixel_%1
    add di,[cs:dw_clip_advance]
    add si,2
    dec word [cs:dw_clip_height]
    jnz .%1
    jmp .done
%endmacro
    DW_CELL_ROWS bytes,stosb
    DW_CELL_ROWS words,stosw
    DW_CELL_ROWS triples,stosw
    DW_CELL_ROWS dwords,stosd
%unmacro DW_CELL_ROWS 2
.done:
    pop es
    popad
    retf

dw_clip_width dw 0
dw_clip_height dw 0
dw_clip_advance dw 0
dw_clip_skip db 0
