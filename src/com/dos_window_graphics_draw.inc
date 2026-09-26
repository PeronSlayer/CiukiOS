; Indexed 320x200 cooperative client, drawn at exact 2x into a native band.
; FAR ABI: BX/DX = nonnegative client screen origin; DS = host, FS = runtime.
; CS:cg_descriptor+12/+14 is the DOS-owned framebuffer offset/segment.
; CS:cg_native_palette holds 256 already-packed native DWORD colors.
; DW_CELL_* describes a valid <=64KiB compositor band. Nothing touches VRAM,
; firmware or DOS. The existing host-busy guard owns this private scratch.
; Preserves all GPR/segment registers and IF; returns FAR.
%include "src/com/dos_window_abi.inc"

cg_draw_band:
    pushad
    push es
    push fs
    cld
    mov [cs:cg_draw_origin_x],bx
    mov [cs:cg_draw_origin_y],dx
    mov ax,bx
    add ax,640
    jc .done
    cmp bx,[cs:DW_CELL_LEFT]
    jae .left
    mov bx,[cs:DW_CELL_LEFT]
.left:
    cmp ax,[cs:DW_CELL_RIGHT]
    jbe .right
    mov ax,[cs:DW_CELL_RIGHT]
.right:
    cmp ax,bx
    jbe .done
    sub ax,bx
    mov [cs:cg_draw_span],ax
    mov ax,dx
    add ax,400
    jc .done
    cmp dx,[cs:DW_CELL_TOP]
    jae .top
    mov dx,[cs:DW_CELL_TOP]
.top:
    cmp ax,[cs:DW_CELL_BOTTOM]
    jbe .bottom
    mov ax,[cs:DW_CELL_BOTTOM]
.bottom:
    cmp ax,dx
    jbe .done
    sub ax,dx
    mov [cs:cg_draw_rows],ax
    mov ax,dx
    sub ax,[cs:cg_draw_origin_y]
    mov cl,al
    and cl,1
    mov [cs:cg_draw_y_odd],cl
    shr ax,1
    imul si,ax,320
    mov ax,bx
    sub ax,[cs:cg_draw_origin_x]
    mov cl,al
    and cl,1
    mov [cs:cg_draw_x_odd],cl
    shr ax,1
    add si,ax
    cmp word [cs:cg_descriptor+12],1536
    ja .done                    ; 64000 source bytes must fit its segment
    add si,[cs:cg_descriptor+12]
    mov [cs:cg_draw_source],si
    mov fs,[cs:cg_descriptor+14]
    mov ax,dx
    sub ax,[cs:DW_CELL_TOP]
    mul word [cs:DW_CELL_STRIDE]
    mov di,ax
    mov ax,bx
    sub ax,[cs:DW_CELL_LEFT]
    movzx cx,byte [cs:DW_CELL_BYTES]
    cmp cx,1
    jb .done
    cmp cx,4
    ja .done
    mul cx
    add di,ax
    imul cx,[cs:cg_draw_span]
    mov ax,[cs:DW_CELL_STRIDE]
    sub ax,cx
    mov [cs:cg_draw_advance],ax
    mov es,[cs:DW_CELL_BAND_SEG]
    cmp byte [cs:DW_CELL_BYTES],4
    je .dwords
    cmp byte [cs:DW_CELL_BYTES],2
    je .words
    cmp byte [cs:DW_CELL_BYTES],3
    je .triples

%macro CG_LOAD_NATIVE_PIXEL 0
    movzx bx,byte [fs:si]
    inc si
    shl bx,2
    mov eax,[cs:cg_native_palette+bx]
%endmacro
%macro CG_STORE_ONE 1
%ifidni %1,bytes
    stosb
%elifidni %1,words
    stosw
%elifidni %1,dwords
    stosd
%else
    stosw
    shr eax,16
    stosb
%endif
%endmacro
%macro CG_STORE_PAIR 1
%ifidni %1,bytes
    mov ah,al
    stosw
%elifidni %1,words
    mov dx,ax
    shl eax,16
    mov ax,dx
    stosd
%elifidni %1,dwords
    stosd
    stosd
%else
    ; Two RGB triplets: one packed DWORD followed by the remaining WORD.
    and eax,0x00FFFFFF
    mov edx,eax
    shl edx,24
    or edx,eax
    shr eax,8
    xchg eax,edx
    stosd
    mov ax,dx
    stosw
%endif
%endmacro
%macro CG_DRAW_ROWS 1
.%1:
    mov si,[cs:cg_draw_source]
    mov cx,[cs:cg_draw_span]
    cmp byte [cs:cg_draw_x_odd],0
    je .pairs_%1
    CG_LOAD_NATIVE_PIXEL
    CG_STORE_ONE %1
    dec cx
.pairs_%1:
    mov bp,cx
    shr cx,1
    jcxz .tail_%1
.pair_%1:
    CG_LOAD_NATIVE_PIXEL
    CG_STORE_PAIR %1
    loop .pair_%1
.tail_%1:
    test bp,1
    jz .next_%1
    CG_LOAD_NATIVE_PIXEL
    CG_STORE_ONE %1
.next_%1:
    add di,[cs:cg_draw_advance]
    xor byte [cs:cg_draw_y_odd],1
    jnz .same_source_%1
    add word [cs:cg_draw_source],320
.same_source_%1:
    dec word [cs:cg_draw_rows]
    jnz .%1
    jmp .done
%endmacro
    CG_DRAW_ROWS bytes
    CG_DRAW_ROWS words
    CG_DRAW_ROWS triples
    CG_DRAW_ROWS dwords
%unmacro CG_DRAW_ROWS 1
%unmacro CG_STORE_PAIR 1
%unmacro CG_STORE_ONE 1
%unmacro CG_LOAD_NATIVE_PIXEL 0
.done:
    pop fs
    pop es
    popad
    retf

cg_draw_origin_x dw 0
cg_draw_origin_y dw 0
cg_draw_span dw 0
cg_draw_rows dw 0
cg_draw_source dw 0
cg_draw_advance dw 0
cg_draw_x_odd db 0
cg_draw_y_odd db 0
