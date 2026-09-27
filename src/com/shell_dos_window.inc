; Foreground BIOS-text DOS execution in a live native window.
; The separately loaded runtime owns interrupt context and text storage.
%include "src/com/dos_window_abi.inc"

ui_window_game:
    mov dword [ui_run_text],'DWIN'
    cmp ax,59
    je .launch
    mov byte [ui_run_text],'W'
.launch:
    mov byte [ui_run_text+4],0
    mov byte [ui_run_len],4
    jmp ui_run_window

ui_dos_window:
    mov byte [dw_tail],2
    mov word [dw_tail+1],'/W'
    mov byte [dw_tail+3],13
    jmp dw_launch
ui_run_window:
    cmp byte [ui_run_len],0
    je ui_redraw
    push cs
    pop es
    mov di,dw_tail+1
    mov ax,'/W'
    stosw
    mov al,' '
    stosb
    mov ax,'/C'
    stosw
    mov al,' '
    stosb
    mov si,ui_run_text
    movzx cx,byte [ui_run_len]
    mov ax,cx
    add al,6
    mov [dw_tail],al
    rep movsb
    mov al,13
    stosb
dw_launch:
    call ui_sfx_end
    call dw_release
    jc dw_retained_show
    mov word [dw_status],dw_unavailable
    call dos_host_prepare
    jc .show
    call dw_load
    jc .finish
    mov es,[dw_segment]
    mov word [es:DW_CALLBACK_OFF],dos_host_tick
    mov [es:DW_CALLBACK_SEG],cs
    mov [es:DW_HOST_DS],cs
    mov ax,[ui_width]
    mov [es:DW_WIDTH],ax
    mov ax,[ui_height]
    mov [es:DW_HEIGHT],ax
    mov ax,[ui_mouse_x]
    mov [es:DW_MOUSE_X],ax
    mov ax,[ui_mouse_y]
    mov [es:DW_MOUSE_Y],ax
    mov [dw_parameters+4],cs
    mov word [dw_status],dw_live
    mov byte [ui_dialog],11
    call ui_windows_open
    ; The foreground paint completes before enabling asynchronous presentation.
    ; Include the new task button before the event loop is suspended.
    mov byte [ui_comp_dirty],0
    mov byte [ui_bar_dirty],0
    call ui_draw
    xor ax,ax
    call far [dw_entry]
    jc .failed
    mov es,[dw_segment]
    mov byte [es:DW_GUEST_LIVE],1
    push cs
    pop es
    mov dx,dw_command
    mov bx,dw_parameters
    mov ax,0x4B00
    int 0x21
    pushf
    push ax
    mov es,[cs:dw_segment]
    mov byte [es:DW_GUEST_LIVE],0
    mov ax,DW_OP_UNINSTALL
    call far [cs:dw_entry]
    jc dw_cleanup_failed
    push cs
    pop ds
    push cs
    pop es
    pop ax
    popf
    jc .failed
    mov ax,0x4D00
    int 0x21
    mov word [dw_status],dw_finished
    mov es,[dw_segment]
    cmp byte [es:DW_CLOSE_REQUEST],1
    je .closed
    cmp word [es:DW_ERRORS],0
    je .finish
    mov word [dw_status],dw_unsupported
    jmp .finish
.closed:
    call dos_host_finish
    call dw_release
    jc dw_retained_show
    call ui_sfx_begin
    push cs
    pop es
    call ui_windows_unfocus
    mov byte [ui_full_redraw],1
    jmp ui_redraw
.failed:
    mov word [dw_status],dw_exec_failed
    mov es,[dw_segment]
    cmp byte [es:DW_INSTALLED],0
    jne dw_retained_show
.finish:
    call dos_host_finish
.show:
    push cs
    pop ds
    push cs
    pop es
    call ui_sfx_begin
dw_show:
    mov byte [ui_dialog],11
    call ui_windows_open
    mov byte [ui_full_redraw],1
    jmp ui_redraw
dw_cleanup_failed:
    add sp,4                        ; discard EXEC result; cleanup owns failure
dw_retained_show:
    push cs
    pop ds
    push cs
    pop es
    mov word [dw_status],dw_retained
    mov byte [ui_command_running],0
    call dos_host_finish
    jmp dw_show                      ; no audio handoff while ownership remains

dw_load:
    cmp word [dw_segment],0
    jne .fail                       ; never overwrite a retained allocation
    mov dx,dw_runtime
    mov ax,0x3D00
    int 0x21
    jc .fail
    mov [dw_handle],ax
    mov bx,0x1000
    mov ah,0x48
    int 0x21
    jc .close_fail
    mov [dw_segment],ax
    mov [dw_entry+2],ax
    mov ds,ax
    mov bx,[cs:dw_handle]
    xor dx,dx
    mov cx,0xFFF0
    mov ah,0x3F
    int 0x21
    push cs
    pop ds
    jc .free_fail
    mov es,[dw_segment]
    cmp ax,DW_HEADER_BYTES
    jb .free_fail
    cmp ax,[es:DW_IMAGE_BYTES]
    jne .free_fail
    cmp dword [es:DW_MAGIC],'CWRT'
    jne .free_fail
    cmp dword [es:DW_MAGIC+4],'0001'
    jne .free_fail
    cmp word [es:DW_VERSION],DW_ABI_VERSION
    jne .free_fail
    cmp word [es:DW_HEADER_SIZE],DW_HEADER_BYTES
    jne .free_fail
    mov dx,[es:DW_CELL_ENTRY]
    cmp dx,DW_HEADER_BYTES
    jb .free_fail
    cmp dx,ax
    jae .free_fail
    mov [es:DW_CELL_ENTRY+2],es
    mov dx,[es:DW_FONT_OFFSET]
    add dx,8192
    jc .free_fail
    cmp dx,ax
    ja .free_fail
    mov dx,[es:DW_CELLS_OFFSET]
    add dx,4000
    jc .free_fail
    cmp dx,ax
    ja .free_fail
    mov dx,[es:DW_DIRTY_OFFSET]
    add dx,4
    jc .free_fail
    cmp dx,ax
    ja .free_fail
    add ax,15
    jc .free_fail
    shr ax,4
    mov bx,ax
    mov ah,0x4A
    int 0x21
    jc .free_fail
    mov bx,4000
    mov ah,0x48
    int 0x21
    jc .free_fail
    mov es,[dw_segment]
    mov [es:DW_GFX_BUFFER_SEG],ax
    mov [es:DW_GFX_DRAW+2],es
    mov al,[vc_info+25]
    mov [es:DW_GFX_BPP],al
    mov si,vc_info+31
    mov di,DW_GFX_MASKS
    mov cx,3
    rep movsw
    mov bx,[dw_handle]
    mov ah,0x3E
    int 0x21
    ; Copy the actual firmware 8x16 font before installing virtual INT10.
    push fs
    mov ax,0x1130
    mov bh,6
    call vc_bios
    mov si,[vc_bios_bp]
    mov ax,si
    shr ax,4
    add ax,[vc_bios_es]
    mov fs,ax
    and si,15
    mov es,[dw_segment]
    mov di,[es:DW_FONT_OFFSET]
    mov cx,4096
.font:
    mov ah,[fs:si]
    inc si
    xor al,al
    stosw
    loop .font
    pop fs
    push cs
    pop es
    clc
    ret
.free_fail:
    mov bx,[dw_handle]
    mov ah,0x3E
    int 0x21
    mov es,[dw_segment]
    mov ah,0x49
    int 0x21
    mov word [dw_segment],0
    jmp .fail
.close_fail:
    mov bx,[dw_handle]
    mov ah,0x3E
    int 0x21
.fail:
    stc
    ret

dw_release:
    pushad
    push es
    mov ax,[dw_segment]
    test ax,ax
    jz .success
    mov es,ax
    cmp byte [es:DW_INSTALLED],0
    je .free
    mov ax,DW_OP_UNINSTALL
    call far [dw_entry]
    jc .retained                 ; preserve owner and both DOS allocations
.free:
    push es
    mov ax,[es:DW_GFX_BUFFER_SEG]
    test ax,ax
    jz .module
    mov es,ax
    mov ah,0x49
    int 0x21
    pop es
    jc .retained
    mov word [es:DW_GFX_BUFFER_SEG],0
    push es
.module:
    pop es
    mov ah,0x49
    int 0x21
    jc .retained
    mov word [dw_segment],0
    mov byte [ui_window_flags+11],0
.success:
    clc
.done:
    pop es
    popad
    ret
.retained:
    mov word [dw_status],dw_retained
    stc
    jmp .done

; The frame follows ui_window_width/height (660x460 unless a VGA-session
; guest was resized); the client is the frame minus 20x60.
dw_draw:
    mov bx,[ui_window_x+22]
    mov [ui_dialog_x],bx
    mov dx,[ui_window_y+22]
    mov [ui_dialog_y],dx
    mov cx,[ui_window_width+22]
    mov si,[ui_window_height+22]
    mov ax,70
    call ui_hit_add
    add bx,4
    add dx,5
    mov al,0
    call ui_rect
    sub bx,4
    sub dx,5
    mov al,7
    call ui_bevel
    add bx,3
    add dx,3
    sub cx,6
    mov si,25
    call ui_windows_title_color
    call ui_title_rail
    mov ax,62
    sub cx,54
    call ui_hit_add
    add bx,7
    add dx,4
    U_TEXT bx,dx,ut_dos_window,15,1
    mov bx,[ui_dialog_x]
    add bx,cx
    add bx,7
    mov dx,[ui_dialog_y]
    add dx,6
    U_BUTTON bx,dx,22,19,ut_min,17
    add bx,24
    U_BUTTON bx,dx,22,19,ut_close,18
    mov bx,[ui_dialog_x]
    add bx,10
    mov dx,[ui_dialog_y]
    add dx,34
    mov cx,[ui_window_width+22]
    sub cx,20
    mov si,[ui_window_height+22]
    sub si,60
    mov al,0
    call ui_rect
    cmp word [dw_segment],0
    je .status
    push fs
    mov fs,[dw_segment]
    mov [fs:DW_CLIENT_WIDTH],cx
    mov [fs:DW_CLIENT_HEIGHT],si
    cmp byte [fs:DW_RESIZABLE],1
    jne .fixed
    pusha
    add bx,cx
    add bx,10
    add dx,si
    add dx,26
    mov ax,63
    call ui_grip
    popa
.fixed:
    ; Refresh the external renderer's band descriptor once per scene band.
    push es
    push fs
    pop es
    mov si,ui_comp_stride
    mov di,DW_CELL_STRIDE
    mov cx,4                 ; stride, row capacity, current top/bottom
    rep movsw
    mov si,ui_comp_left
    mov di,DW_CELL_LEFT
    mov cl,2                 ; exclusive left/right bounds
    rep movsw
    mov ax,[ui_comp_seg]
    mov [es:DW_CELL_BAND_SEG],ax
    mov word [es:DW_CELL_PALETTE],vc_colors
    mov al,[vc_bytes]
    mov [es:DW_CELL_BYTES],al
    pop es
    cmp byte [fs:DW_GFX_ACTIVE],1
    jne .text
    mov bx,[ui_dialog_x]
    add bx,10
    mov dx,[ui_dialog_y]
    add dx,34
    call far [fs:DW_GFX_DRAW]
    jmp .font_done
.text:
    mov word [dw_row],0
.row:
    mov dx,[dw_row]
    shl dx,4
    add dx,[ui_dialog_y]
    add dx,34
    cmp dx,[ui_comp_bottom]
    jae .font_done
    mov ax,dx
    add ax,16
    cmp ax,[ui_comp_top]
    jbe .next_row
    mov ax,[dw_row]
    imul ax,160
    add ax,[fs:DW_CELLS_OFFSET]
    mov [dw_cell],ax
    mov word [dw_col],0
.cell:
    mov bx,[dw_col]
    shl bx,3
    add bx,[ui_dialog_x]
    add bx,10
    mov si,[dw_cell]
    mov ax,[fs:si]
    movzx si,al
    shl si,5
    add si,[fs:DW_FONT_OFFSET]
    mov al,ah
    call dw_opaque_cell
    cmp byte [fs:DW_GUEST_LIVE],1
    jne .next_cell
    cmp byte [fs:DW_FOCUSED],1
    jne .next_cell
    test byte [fs:DW_CURSOR_SHAPE+1],0x20
    jnz .next_cell
    mov ax,[fs:DW_CURSOR]
    cmp al,[dw_col]
    jne .next_cell
    cmp ah,[dw_row]
    jne .next_cell
    push dx
    add dx,14
    mov cx,8
    mov si,2
    mov al,15
    call ui_rect
    pop dx
.next_cell:
    add word [dw_cell],2
    inc word [dw_col]
    cmp word [dw_col],80
    jb .cell
.next_row:
    inc word [dw_row]
    cmp word [dw_row],25
    jb .row
.font_done:
    pop fs
.status:
    mov bx,[ui_dialog_x]
    add bx,10
    mov dx,[ui_dialog_y]
    add dx,[ui_window_height+22]
    sub dx,22
    mov si,[dw_status]
    xor ax,ax
    call ui_text
    ret

; Opaque BIOS 8x16 cell into the current compositor band. Clip once and
; resolve its two native colors once; each visible pixel is written once.
; Font rows are stored as words with the original eight bits in their high
; byte. BX/DX=cell origin, FS:SI=font, AL=attribute. No firmware/VRAM access.
dw_opaque_cell:
    call far [fs:DW_CELL_ENTRY]
    ret

ut_dos_window db 'DOS window',0
dw_runtime db '\SYSTEM\DOSWIN.DRV',0
dw_command db '\COMMAND.COM',0
dw_live db 'Title bar: move, minimize, close.',0
dw_finished db 'Finished. Close frees memory.',0
dw_unavailable db 'Needs 800x600 video and DOSWIN.DRV.',0
dw_exec_failed db 'COMMAND.COM failed. Try full-screen DOS.',0
dw_unsupported db 'Video unsupported. Use full screen.',0
dw_retained db 'Cleanup pending. Close to retry.',0
dw_status dw dw_finished
dw_segment dw 0
dw_entry dw 0,0
dw_handle dw 0
dw_row dw 0
dw_col dw 0
dw_cell dw 0
dw_parameters dw 0,dw_tail,0,0,0,0,0
dw_tail equ exec_tail              ; EXEC copies the tail into the child PSP
