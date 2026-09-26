; Native removable media Files integration. All device I/O is foreground via
; MEDIA.DRV; drawing only reads cached memory. Exact names stay in the driver.
MD_SIZE equ 1192
md_open:
    mov [md_active],al
    mov byte [fm_prompt],0
    mov byte [md_preview],0
    mov word [fm_top],0
    mov byte [fm_selection],0
    mov word [fm_path],'/'
    call fm_begin
    call md_load
    jc .failed
    mov ax,1
    call md_run
    jmp .show
.failed:
    mov byte [fm_count],0
    mov word [fm_total],0
    mov word [fm_status],md_missing
.show:
    mov byte [ui_dialog],FM_WINDOW
    call ui_windows_open
    ret

md_load:
    cmp word [md_segment],0
    jne .ready
    mov dx,md_path
    mov ax,0x3d00
    int 0x21
    jc .fail
    push ax
    mov bx,0x1000
    mov ah,0x48
    int 0x21
    jc .close
    mov [md_segment],ax
    pop bx
    push bx
    push ds
    mov ds,ax
    xor dx,dx
    mov cx,0xef00
    mov ah,0x3f
    int 0x21
    pop ds
    jc .release
    mov es,[md_segment]
    cmp ax,32
    jb .release
    cmp dword [es:4],'CMED'
    jne .release
    cmp dword [es:8],'IA01'
    jne .release
    cmp word [es:12],1
    jne .release
    cmp ax,[es:14]
    jne .release
    cmp word [es:18],MD_SIZE
    jne .release
    mov dx,[es:20]
    add dx,MD_SIZE
    jc .release
    cmp dx,ax
    ja .release
    mov bx,[es:16]
    cmp bx,0x1000
    ja .release
    mov dx,ax
    add dx,15
    shr dx,4
    cmp bx,dx
    jb .release
    mov ah,0x4a
    int 0x21
    jc .release
    mov ax,[es:20]
    mov [md_packet],ax
    pop bx
    mov ah,0x3e
    int 0x21
.ready:
    push cs
    pop es
    clc
    ret
.release:
    mov es,[md_segment]
    mov ah,0x49
    int 0x21
    mov word [md_segment],0
.close:
    pop bx
    mov ah,0x3e
    int 0x21
.fail:
    push cs
    pop es
    stc
    ret

md_refresh:
    mov ax,8
md_run:
    cmp word [md_segment],0
    je .missing
    pushad
    push es
    push fs
    mov fs,[md_segment]
    mov bx,[md_packet]
    mov [fs:bx+2],ax
    movzx ax,byte [md_active]
    mov [fs:bx+6],ax
    mov ax,[fm_top]
    mov [fs:bx+8],ax
    movzx ax,byte [fm_selection]
    mov [fs:bx+16],ax
    mov ax,[md_segment]
    mov es,ax
    mov di,bx
    call far [md_entry]
    mov byte [md_preview],0
    mov byte [fm_selection],0
    mov byte [fm_last_item],0xff
    cmp word [fs:bx+4],0
    jne .error
.success:
    cmp word [fs:bx+2],4
    jne .preview
    mov byte [ui_sfx_pending],6
.preview:
    cmp word [fs:bx+2],3
    jne .list
    mov byte [md_preview],1
.list:
    mov ax,[fs:bx+8]
    mov [fm_top],ax
    mov ax,[fs:bx+10]
    mov [fm_count],al
    mov ax,[fs:bx+12]
    mov [fm_total],ax
    lea si,[bx+680]
    mov di,fm_records
    mov cx,FM_ROWS*FM_RECORD
    call md_copy
    lea si,[bx+20]
    mov di,fm_path
    mov cx,FM_PATH-1
    call md_copy
    mov byte [fm_path+FM_PATH-1],0
    jmp .status
.error:
    mov byte [ui_sfx_pending],3
    mov byte [fm_count],0
    mov word [fm_total],0
.status:
    lea si,[bx+276]
    mov di,fm_error_text
    mov cx,73
    call md_copy
    mov byte [fm_error_text+73],0
    mov word [fm_status],fm_error_text
    mov byte [fm_prompt],0
    pop fs
    pop es
    popad
    ret
.missing:
    mov word [fm_status],md_missing
    ret
md_copy:
    push es
    push cs
    pop es
.next:
    mov al,[fs:si]
    inc si
    stosb
    loop .next
    pop es
    ret

md_selected:
    mov ax,5
    call md_run
    jmp md_action.redraw
md_action:
    cmp ax,100
    je md_selected
    cmp ax,101
    jne .refresh
    mov ax,7
    jmp .run
.refresh:
    cmp ax,102
    je .reload
    cmp ax,109
    je .previous
    cmp ax,110
    je .next
    cmp ax,114
    je .reload
    cmp ax,113
    je .import
    cmp ax,105
    jne .readonly
    cmp byte [md_preview],0
    jne .readonly
    cmp byte [fm_count],0
    je .readonly
    mov byte [fm_prompt],105
    mov al,[fm_mounted_drive]
    mov [fm_edit],al
    mov word [fm_edit+1],':\'
    mov byte [fm_edit+3],0
    movzx si,byte [fm_selection]
    imul si,FM_RECORD
    add si,fm_records
    mov di,fm_source
    call copy_z_to_di
    jmp fm_action.prompt_ready
.import:
    cmp byte [fm_prompt],105
    jne .readonly
    mov es,[md_segment]
    mov di,[md_packet]
    add di,212
    mov si,fm_edit
    mov cx,FM_PATH
    rep movsb
    push cs
    pop es
    mov ax,6
.run:
    call md_run
.redraw:
    mov ax,250
    ret
.previous:
    cmp word [fm_top],0
    je .redraw
    sub word [fm_top],FM_ROWS
    jmp .reload
.next:
    mov ax,[fm_top]
    add ax,FM_ROWS
    cmp ax,[fm_total]
    jae .redraw
    mov [fm_top],ax
.reload:
    mov byte [fm_selection],0
    call md_refresh
    jmp .redraw
.readonly:
    mov word [fm_status],md_notice
    jmp .redraw

; Native read-only preview, 11 lines of 62 columns. No child application.
md_draw_preview:
    pushad
    push fs
    mov fs,[md_segment]
    mov si,[md_packet]
    add si,680
    mov bp,134
.line:
    mov di,fm_source
    mov cx,62
.char:
    mov al,[fs:si]
    inc si
    test al,al
    jz .draw_end
    cmp al,13
    je .char
    cmp al,10
    je .draw
    cmp al,9
    jne .store
    mov al,' '
.store:
    mov [di],al
    inc di
    loop .char
.draw:
    mov byte [di],0
    push si
    mov si,fm_source
    mov bx,20
    mov dx,bp
    xor ax,ax
    call fm_text
    pop si
    add bp,18
    cmp bp,332
    jb .line
    jmp .done
.draw_end:
    mov byte [di],0
    mov si,fm_source
    mov bx,20
    mov dx,bp
    xor ax,ax
    call fm_text
.done:
    pop fs
    popad
    ret

md_entry dw 0
md_segment dw 0
md_packet dw 0
md_active db 0
md_preview db 0
md_path db '\SYSTEM\MEDIA.DRV',0
md_missing db 'Media service unavailable. Check SYSTEM\MEDIA.DRV and free memory.',0
md_readonly db 'Read-only',0
md_notice db 'Read-only device. Open folders or text; Copy imports files.',0
