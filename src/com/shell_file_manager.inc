; Native Files window. DOS 8.3 paths, real INT21 operations, bounded storage.
; Root GUI integration: window 8, 620x410, actions 100..149. See integration.md.
; No file I/O occurs during painting. DOS children retain the desktop state.
FM_WINDOW equ 8
FM_WIDTH equ 620
FM_HEIGHT equ 410              ; plus 5px shadow fits 640x480 desktop work area
FM_ROWS equ 6
FM_RECORD equ 18                 ; name[13], attribute, size[4]
FM_PATH equ 64

fm_begin:
    push cs
    pop es
    cld
    cmp byte [fm_initialized],1
    je .done
    mov byte [fm_initialized],1
    mov ah,0x19
    int 0x21
    add al,'A'
    mov [fm_path],al
    mov [fm_mounted_drive],al
.done:
    ret

; Called before DOS/EXEC: no native copy handles may leak to a child.
fm_end:
    cmp byte [fm_busy],0
    je .done
    call fm_cancel_copy
.done:
    ret

fm_open:
    cmp byte [md_active],0
    je .local
    mov byte [md_active],0
    mov byte [md_preview],0
    mov al,[fm_mounted_drive]
    mov [fm_path],al
    mov word [fm_path+1],':\'
    mov byte [fm_path+3],0
    mov word [fm_top],0
.local:
    call fm_begin
    call fm_refresh
    mov byte [ui_dialog],FM_WINDOW
    call ui_windows_open
    ret

fm_resume:
    cmp byte [ui_window_flags+FM_WINDOW],0
    je .done
    call fm_refresh
.done:
    ret

; Full directory enumeration; only the visible page is retained. Never cap
; the directory to the first six entries or read a DTA after a failed search.
fm_refresh:
    cmp byte [md_active],0
    jne md_refresh
    cmp byte [fm_busy],0
    jne .busy
    pushad
    push es
    push cs
    pop es
    cld
    mov byte [fm_count],0
    mov word [fm_total],0
    mov word [fm_status],fm_ready
    mov byte [fm_last_item],0xFF
    mov ax,0x2F00
    int 0x21
    mov [fm_old_dta],bx
    mov [fm_old_dta+2],es
    push cs
    pop es
    mov dx,fm_dta
    mov ah,0x1A
    int 0x21
    mov si,fm_path
    mov bx,fm_wildcard
    mov di,fm_work
    call fm_join
    jc .long_path
    mov dx,fm_work
    mov cx,0x37                 ; normal, hidden, system and directories
    mov ah,0x4E
    int 0x21
    jc .end_search
.entry:
    test byte [fm_dta+21],8     ; no volume labels
    jnz .next
    cmp byte [fm_dta+30],'.'   ; Up owns parent navigation
    je .next
    mov ax,[fm_total]
    cmp ax,[fm_top]
    jb .count
    cmp byte [fm_count],FM_ROWS
    jae .count
    movzx di,byte [fm_count]
    imul di,FM_RECORD
    add di,fm_records
    mov si,fm_dta+30
    mov cx,13
    rep movsb
    mov al,[fm_dta+21]
    stosb
    mov eax,[fm_dta+26]
    stosd
    inc byte [fm_count]
.count:
    cmp word [fm_total],0xFFFF
    je .next
    inc word [fm_total]
.next:
    mov ah,0x4F
    int 0x21
    jnc .entry
.end_search:
    cmp ax,18
    je .restore
    cmp ax,2
    je .restore                ; empty DOS directories may return file absent
    call fm_error
    jmp .restore
.long_path:
    mov word [fm_status],fm_path_long
.restore:
    push ds
    mov dx,[fm_old_dta]
    mov ds,[fm_old_dta+2]
    mov ah,0x1A
    int 0x21
    pop ds
    cmp byte [fm_count],0
    jne .selection
    cmp word [fm_top],0
    je .selection
    mov word [fm_top],0
    pop es
    popad
    jmp fm_refresh             ; removed final page: return to first page
.selection:
    mov al,[fm_count]
    test al,al
    jz .empty
    cmp [fm_selection],al
    jb .done
    dec al
.empty:
    mov [fm_selection],al
.done:
    pop es
    popad
.busy:
    ret

; SI base + BX leaf -> DI destination (64 bytes). CF rejects truncation.
fm_join:
    push ax
    push cx
    push dx
    mov dx,di
    mov cx,FM_PATH-1
.base:
    lodsb
    test al,al
    jz .separator
    jcxz .fail
    stosb
    dec cx
    jmp .base
.separator:
    cmp di,dx
    je .leaf
    cmp byte [di-1],'\'
    je .leaf
    jcxz .fail
    mov al,'\'
    stosb
    dec cx
.leaf:
    mov si,bx
.copy:
    lodsb
    test al,al
    jz .ok
    jcxz .fail
    stosb
    dec cx
    jmp .copy
.ok:
    mov byte [di],0
    clc
    jmp .out
.fail:
    mov byte [di],0
    stc
.out:
    pop dx
    pop cx
    pop ax
    ret

fm_selected:
    cmp byte [fm_count],0
    je .none
    movzx bx,byte [fm_selection]
    imul bx,FM_RECORD
    add bx,fm_records
    mov al,[bx+13]
    mov [fm_attr],al
    mov eax,[bx+14]
    mov [fm_size],eax
    mov si,fm_path
    mov di,fm_source
    call fm_join
    jc .long
    clc
    ret
.long:
    mov word [fm_status],fm_path_long
    stc
    ret
.none:
    mov word [fm_status],fm_choose
    stc
    ret

; AX action -> 250 redraw, 0 unchanged, or 249 with SI=command to launch.
; Return before dispatching a DOS child; never strand this CALL's stack word.
fm_action:
    cmp byte [md_active],0
    je .local
    cmp ax,120
    jb md_action
.local:
    push cs
    pop es
    cld
    cmp byte [fm_busy],0
    je .normal
    cmp ax,114
    jne .idle
    call fm_cancel_copy
    jmp .redraw
.normal:
    cmp byte [fm_prompt],0
    je .dispatch
    cmp ax,113
    je fm_accept
    cmp ax,114
    jne .idle
    mov byte [fm_prompt],0
    mov word [fm_status],fm_ready
    jmp .redraw
.dispatch:
    cmp ax,120
    jb .toolbar
    cmp ax,120+FM_ROWS
    jae .idle
    sub ax,120
    cmp al,[fm_count]
    jae .idle
    mov [fm_selection],al
    mov word [ui_focus],100
    mov dl,al
    push es
    mov ax,0x40
    mov es,ax
    mov ax,[es:0x6C]
    pop es
    mov cx,ax
    sub ax,[fm_last_click]
    cmp dl,[fm_last_item]
    jne .first_click
    cmp ax,9
    ja .first_click
    mov byte [fm_last_item],0xFF
    jmp fm_open_selected
.first_click:
    mov [fm_last_item],dl
    mov [fm_last_click],cx
    jmp .redraw
.toolbar:
    cmp ax,100
    je fm_open_selected
    cmp ax,101
    je fm_up
    cmp ax,102
    je .refresh
    cmp ax,109
    je .previous
    cmp ax,110
    je .next
    cmp ax,111
    je .previous_drive
    cmp ax,112
    je .next_drive
    cmp ax,103
    jb .idle
    cmp ax,108
    ja .idle
    mov [fm_prompt],al
    mov byte [fm_edit],0
    cmp ax,103
    je .prompt_ready
    cmp ax,108
    je .location
    call fm_selected
    jc .cancel_prompt
    cmp byte [fm_prompt],104
    jne .prompt_ready
    movzx si,byte [fm_selection]
    imul si,FM_RECORD
    add si,fm_records
    mov di,fm_edit
    call copy_z_to_di
    jmp .prompt_ready
.location:
    mov si,fm_path
    mov di,fm_edit
    call copy_z_to_di
.prompt_ready:
    mov si,fm_edit
    xor cx,cx
.length:
    lodsb
    test al,al
    jz .prompt_show
    inc cx
    jmp .length
.prompt_show:
    mov [fm_edit_len],cl
    mov [fm_edit_cursor],cl
    mov word [ui_focus],113
    cmp byte [fm_prompt],107
    jne .redraw
    mov byte [ui_sfx_pending],5
    mov word [ui_focus],114    ; deletion confirmation defaults to Cancel
    jmp .redraw
.cancel_prompt:
    mov byte [fm_prompt],0
    jmp .redraw
.previous:
    cmp word [fm_top],0
    je .idle
    sub word [fm_top],FM_ROWS
    jmp .refresh_selection
.next:
    mov ax,[fm_top]
    add ax,FM_ROWS
    cmp ax,[fm_total]
    jae .idle
    mov [fm_top],ax
.refresh_selection:
    mov byte [fm_selection],0
.refresh:
    call fm_refresh
    jmp .redraw
.previous_drive:
.next_drive:
    mov word [fm_status],fm_other_volume
    jmp .redraw
.idle:
    xor ax,ax
    ret
.redraw:
    mov ax,250
    ret

fm_up:
    mov si,fm_path
    mov di,fm_edit
    call copy_z_to_di
    mov di,fm_edit
.end:
    cmp byte [di],0
    je .back
    inc di
    jmp .end
.back:
    cmp di,fm_edit+3
    jbe fm_navigate
    dec di
    cmp byte [di],'\'
    jne .back
    cmp di,fm_edit+2
    jne .trim
    inc di
.trim:
    mov byte [di],0
    jmp fm_navigate

; Navigation validates the real absolute directory without changing CWD.
; Do not round-trip through AH=47: this kernel's legacy CWD text is shorter
; than its absolute-path resolver, and would silently truncate nested paths.
fm_navigate:
    cmp byte [fm_edit],'A'
    jb .invalid
    cmp byte [fm_edit],'Z'
    ja .invalid
    cmp word [fm_edit+1],':\'
    jne .invalid
    mov al,[fm_edit]
    cmp al,[fm_mounted_drive]
    jne .other_volume
    mov dx,fm_edit
    mov ax,0x4300
    int 0x21
    jc .error
    test cx,16
    jz .invalid
    mov si,fm_edit
    mov di,fm_path
    call copy_z_to_di
    cmp di,fm_path+3
    jbe .trimmed
    cmp byte [di-1],'\'
    jne .trimmed
    mov byte [di-1],0
.trimmed:
    mov word [fm_top],0
    mov byte [fm_selection],0
    mov byte [fm_prompt],0
    call fm_refresh
    mov ax,250
    ret
.invalid:
    mov word [fm_status],fm_absolute
    mov ax,250
    ret
.other_volume:
    mov word [fm_status],fm_other_volume
    mov ax,250
    ret
.error:
    call fm_error
    mov ax,250
    ret

fm_open_selected:
    cmp byte [md_active],0
    jne md_selected
    call fm_selected
    jc .return
    test byte [fm_attr],16
    jz .file
    mov si,fm_source
    mov di,fm_edit
    call copy_z_to_di
    jmp fm_navigate
.file:
    mov si,fm_source
    xor bx,bx
.extension:
    lodsb
    test al,al
    jz .classify
    cmp al,'.'
    jne .extension
    mov bx,si
    jmp .extension
.classify:
    test bx,bx
    jz .unknown
    mov eax,[bx]
    cmp eax,'COM'
    je .program
    cmp eax,'EXE'
    je .program
    cmp eax,'TXT'
    je .text
    cmp eax,'INI'
    je .text
    cmp eax,'CFG'
    je .text
    cmp eax,'LOG'
    je .text
    cmp eax,'BAT'             ; explicit editor; no unsupported batch execution
    je .text
    cmp eax,'ASM'
    je .text
    cmp eax,'MD'
    je .text
.unknown:
    mov word [fm_status],fm_no_association
    jmp .return
.text:
    mov dword [fm_command],'EDIT'
    mov byte [fm_command+4],' '
    mov di,fm_command+5
    jmp .command
.program:
    ; Legacy games often load data relative to CWD, not executable pathname.
    mov dl,[fm_path]
    sub dl,'A'
    mov ah,0x0E
    int 0x21
    mov dx,fm_path
    mov ah,0x3B
    int 0x21
    jc .error
    mov di,fm_command
.command:
    mov si,fm_source
    call copy_z_to_di
    mov si,fm_command
    mov ax,249
    ret
.error:
    call fm_error
.return:
    mov ax,250
    ret

; Confirmed dialog operation. Copy/move destinations include the full name;
; rejecting an existing target avoids implicit truncation or replacement.
fm_accept:
    mov al,[fm_prompt]
    cmp al,108
    je fm_navigate
    cmp al,107
    je .delete
    cmp byte [fm_edit],0
    je .invalid_name
    cmp al,103
    je .leaf
    cmp al,104
    je .leaf
    test byte [fm_attr],16
    jnz .folder_copy
    cmp byte [fm_edit],'A'
    jb .absolute
    cmp byte [fm_edit],'Z'
    ja .absolute
    cmp word [fm_edit+1],':\'
    jne .absolute
    mov si,fm_edit
    mov di,fm_work
    call copy_z_to_di
    jmp .target
.leaf:
    ; DOS accepts 8.3 names. No path separators, wildcard, or traversal here.
    mov si,fm_edit
    xor bx,bx                 ; BL base length, BH extension length
    xor dx,dx                ; DL has seen dot
.name:
    lodsb
    test al,al
    jz .name_end
    cmp al,'.'
    je .dot
    cmp al,' '
    jbe .invalid_name
    mov di,fm_bad_chars
.bad_character:
    cmp byte [di],0
    je .length
    cmp al,[di]
    je .invalid_name
    inc di
    jmp .bad_character
.length:
    test dl,dl
    jnz .extension
    inc bl
    cmp bl,8
    ja .invalid_name
    jmp .name
.extension:
    inc bh
    cmp bh,3
    ja .invalid_name
    jmp .name
.dot:
    test bl,bl
    jz .invalid_name
    test dl,dl
    jnz .invalid_name
    inc dl
    jmp .name
.name_end:
    test bl,bl
    jz .invalid_name
    test dl,dl
    jz .join
    test bh,bh
    jz .invalid_name
.join:
    mov si,fm_path
    mov bx,fm_edit
    mov di,fm_work
    call fm_join
    jc .long
.target:
    ; DOS device names must never turn a requested disk copy into device I/O.
    mov si,fm_work+3
    mov bx,si
.leaf_scan:
    lodsb
    test al,al
    jz .device_name
    cmp al,'/'
    je .invalid_name
    cmp al,'\'
    jne .leaf_scan
    mov bx,si
    jmp .leaf_scan
.device_name:
    mov eax,[bx]
    and eax,0x00FFFFFF
    cmp eax,'CON'
    je .reserved3
    cmp eax,'PRN'
    je .reserved3
    cmp eax,'AUX'
    je .reserved3
    cmp eax,'NUL'
    je .reserved3
    cmp eax,'COM'
    je .reserved4
    cmp eax,'LPT'
    jne .attribute
.reserved4:
    cmp byte [bx+3],'1'
    jb .attribute
    cmp byte [bx+3],'9'
    ja .attribute
    inc bx
.reserved3:
    cmp byte [bx+3],0
    je .invalid_name
    cmp byte [bx+3],'.'
    je .invalid_name
.attribute:
    mov al,[fm_work]
    cmp al,[fm_mounted_drive]
    jne .cross_drive
    mov dx,fm_work
    mov ax,0x4300
    int 0x21
    jnc .exists
    cmp ax,2
    jne .error
    cmp byte [fm_prompt],103
    je .mkdir
    ; Handles have no physical-drive binding in this kernel. Neither copying
    ; nor renaming between different logical drive letters is a safe contract.
    mov al,[fm_source]
    cmp al,[fm_work]
    jne .cross_drive
    cmp byte [fm_prompt],105
    je fm_copy_start
    mov dx,fm_source
    mov di,fm_work
    mov ah,0x56
    int 0x21
    jc .error
    jmp .success
.mkdir:
    mov dx,fm_work
    mov ah,0x39
    int 0x21
    jc .error
    jmp .success
.delete:
    mov dx,fm_source
    mov ah,0x41
    test byte [fm_attr],16
    jz .remove
    mov ah,0x3A               ; DOS rejects nonempty folders
.remove:
    int 0x21
    jc .error
.success:
    mov byte [fm_prompt],0
    call fm_refresh
    mov word [fm_status],fm_done
    mov ax,250
    ret
.exists:
    mov word [fm_status],fm_exists
    jmp .return
.cross_drive:
    mov word [fm_status],fm_same_drive
    jmp .return
.folder_copy:
    mov word [fm_status],fm_files_only
    jmp .return
.absolute:
    mov word [fm_status],fm_absolute
    jmp .return
.long:
    mov word [fm_status],fm_path_long
    jmp .return
.invalid_name:
    mov word [fm_status],fm_name_error
    jmp .return
.error:
    call fm_error
.return:
    mov ax,250
    ret

fm_copy_start:
    mov dx,fm_source
    mov ax,0x3D00
    int 0x21
    jc .error
    mov [fm_src_handle],ax
    mov dx,fm_work
    xor cx,cx
    mov ah,0x3C               ; target was checked absent; native UI is single-tasked
    int 0x21
    jc .create_error
    mov [fm_dst_handle],ax
    mov byte [fm_busy],1
    mov byte [fm_prompt],0
    mov dword [fm_copied],0
    mov word [fm_status],fm_copying
    mov ax,250
    ret
.create_error:
    push ax
    call fm_close_handles
    pop ax
.error:
    call fm_error
    mov ax,250
    ret

; Called once per desktop event iteration: <=4096 bytes per invocation.
; The shell's 512-byte file buffer is idle while no DOS command is running.
fm_poll:
    cmp byte [fm_busy],1
    jne .idle
    push cs
    pop es
    cld
    mov word [fm_chunk],8
.chunk:
    mov bx,[fm_src_handle]
    mov dx,file_buf
    mov cx,512
    mov ah,0x3F
    int 0x21
    jc .error
    test ax,ax
    jz .finished
    mov [fm_transfer],ax
    mov cx,ax
    mov bx,[fm_dst_handle]
    mov dx,file_buf
    mov ah,0x40
    int 0x21
    jc .error
    cmp ax,[fm_transfer]
    jne .disk_full
    movzx eax,ax
    add [fm_copied],eax
    dec word [fm_chunk]
    jnz .chunk
    ; Update progress only once per timer tick; do not force full redraws.
    push es
    mov ax,0x40
    mov es,ax
    mov ax,[es:0x6C]
    pop es
    cmp ax,[fm_copy_tick]
    je .idle
    mov [fm_copy_tick],ax
    call fm_damage
    mov ax,250
    ret
.finished:
    call fm_close_handles
    jc .error
    mov byte [fm_busy],0
    call fm_refresh
    mov word [fm_status],fm_copied_ok
    call fm_damage
    mov ax,250
    ret
.disk_full:
    mov ax,39
.error:
    push ax
    call fm_cancel_copy
    pop ax
    cmp word [fm_status],fm_partial
    je .return_error
    call fm_error
.return_error:
    call fm_damage
    mov ax,250
    ret
.idle:
    xor ax,ax
    ret

fm_damage:
    pushad
    mov bx,FM_WINDOW
    call ui_window_bounds
    add cx,4
    add si,5
    call ui_comp_damage
    popad
    ret

fm_close_handles:
    xor si,si
    mov bx,[fm_dst_handle]
    cmp bx,0xFFFF
    je .source
    mov ah,0x3E
    int 0x21
    jnc .source
    mov si,ax
.source:
    mov word [fm_dst_handle],0xFFFF
    mov bx,[fm_src_handle]
    cmp bx,0xFFFF
    je .done
    mov ah,0x3E
    int 0x21
    jnc .done
    test si,si
    jnz .done
    mov si,ax
.done:
    mov word [fm_src_handle],0xFFFF
    mov ax,si
    test ax,ax
    jz .ok
    stc
    ret
.ok:
    clc
    ret

fm_cancel_copy:
    call fm_close_handles
    mov byte [fm_busy],0
    mov dx,fm_work            ; only this new destination, never the source
    mov ah,0x41
    int 0x21
    jc .cleanup_failed
    call fm_refresh
    mov word [fm_status],fm_cancelled
    ret
.cleanup_failed:
    call fm_refresh
    mov word [fm_status],fm_partial
    ret

; AX DOS error -> specific message, retaining the exact numeric code.
fm_error:
    mov byte [ui_sfx_pending],3
    push ax
    push bx
    push di
    mov [fm_error_code],ax
    mov si,fm_io_error
    cmp ax,2
    je .missing
    cmp ax,3
    je .missing
    cmp ax,5
    je .denied
    cmp ax,15
    je .drive
    cmp ax,19
    je .protected
    cmp ax,21
    je .drive
    cmp ax,39
    je .full
    jmp .message
.missing: mov si,fm_not_found
    jmp .message
.denied: mov si,fm_access
    jmp .message
.drive: mov si,fm_no_drive
    jmp .message
.protected: mov si,fm_read_only
    jmp .message
.full: mov si,fm_full
.message:
    mov di,fm_error_text
    call copy_z_to_di
    mov ax,' ('
    stosw
    mov ax,[fm_error_code]
    call ui_decimal
    mov ax,')'
    stosw
    mov word [fm_status],fm_error_text
    pop di
    pop bx
    pop ax
    ret

; BIOS AX key -> CF set when consumed, action AX (0 means no repaint).
fm_key:
    cmp byte [fm_busy],0
    jne .busy
    cmp byte [fm_prompt],0
    jne .edit
    cmp al,13
    je .open
    cmp al,8
    je .up
    cmp al,27
    je .close
    cmp ah,0x3C
    je .rename
    cmp ah,0x3F
    je .refresh
    cmp ah,0x40
    je .move
    cmp ah,0x41
    je .mkdir
    cmp ah,0x42
    je .delete
    cmp ah,0x53
    je .delete
    cmp ah,0x49
    je .previous
    cmp ah,0x51
    je .next
    cmp ah,0x48
    je .selection_up
    cmp ah,0x50
    je .selection_down
    clc
    ret
.busy:
    cmp al,27
    je .cancel
    cmp ah,0x3E              ; DOS handoff cancels/closes copy via fm_end
    jne .busy_idle
    clc
    ret
.busy_idle:
    xor ax,ax
    stc
    ret
.open:
    mov ax,[ui_focus]
    cmp ax,100
    jb .open_file
    cmp ax,114
    jbe .handled
.open_file:
    mov ax,100
    jmp .handled
.up: mov ax,101
    jmp .handled
.close: mov ax,18
    jmp .handled
.rename: mov ax,104
    jmp .handled
.refresh: mov ax,102
    jmp .handled
.move: mov ax,106
    jmp .handled
.mkdir: mov ax,103
    jmp .handled
.delete: mov ax,107
    jmp .handled
.previous: mov ax,109
    jmp .handled
.next: mov ax,110
    jmp .handled
.selection_up:
    mov word [ui_focus],100
    cmp byte [fm_selection],0
    je .previous
    dec byte [fm_selection]
    jmp .draw
.selection_down:
    mov word [ui_focus],100
    mov al,[fm_selection]
    inc al
    cmp al,[fm_count]
    jae .next
    mov [fm_selection],al
    jmp .draw
.edit:
    cmp al,27
    je .cancel
    cmp al,13
    je .accept
    cmp al,9
    je .dialog_tab
    cmp byte [fm_prompt],107
    je .idle
    cmp al,8
    je .backspace
    cmp ah,0x4B
    je .left
    cmp ah,0x4D
    je .right
    cmp ah,0x47
    je .home
    cmp ah,0x4F
    je .end
    cmp ah,0x53
    je .erase
    cmp al,32
    jb .idle
    cmp al,126
    ja .idle
    cmp byte [fm_edit_len],FM_PATH-1
    jae .idle
    cmp al,'a'
    jb .insert
    cmp al,'z'
    ja .insert
    sub al,32
.insert:
    mov dl,al
    movzx bx,byte [fm_edit_len]
    movzx si,byte [fm_edit_cursor]
.shift:
    mov al,[fm_edit+bx]
    mov [fm_edit+bx+1],al
    cmp bx,si
    je .put
    dec bx
    jmp .shift
.put:
    mov [fm_edit+si],dl
    inc byte [fm_edit_len]
    inc byte [fm_edit_cursor]
    jmp .draw
.backspace:
    cmp byte [fm_edit_cursor],0
    je .idle
    dec byte [fm_edit_cursor]
.erase:
    movzx bx,byte [fm_edit_cursor]
    cmp bl,[fm_edit_len]
    jae .idle
.remove:
    mov al,[fm_edit+bx+1]
    mov [fm_edit+bx],al
    inc bx
    test al,al
    jnz .remove
    dec byte [fm_edit_len]
    jmp .draw
.left:
    cmp byte [fm_edit_cursor],0
    je .idle
    dec byte [fm_edit_cursor]
    jmp .draw
.right:
    mov al,[fm_edit_cursor]
    cmp al,[fm_edit_len]
    jae .idle
    inc byte [fm_edit_cursor]
    jmp .draw
.home:
    mov byte [fm_edit_cursor],0
    jmp .draw
.end:
    mov al,[fm_edit_len]
    mov [fm_edit_cursor],al
.draw: mov ax,250
    jmp .handled
.dialog_tab:
    mov ax,113
    cmp word [ui_focus],113
    jne .dialog_focus
    inc ax
.dialog_focus:
    mov [ui_focus],ax
    jmp .draw
.cancel: mov ax,114
    jmp .handled
.accept:
    mov ax,113
    cmp word [ui_focus],114
    jne .handled
    mov ax,114
    jmp .handled
.idle: xor ax,ax
.handled:
    stc
    ret

; Drawing helpers accept coordinates relative to the Files window.
fm_text:
    add bx,[fm_x]
    add dx,[fm_y]
    call ui_text
    ret
fm_rect:
    add bx,[fm_x]
    add dx,[fm_y]
    call ui_rect
    ret
fm_button:
    add bx,[fm_x]
    add dx,[fm_y]
    call ui_button
    ret

fm_draw:
    pushad
    push es
    push cs
    pop es
    cld
    mov bx,[ui_window_x+FM_WINDOW*2]
    mov [fm_x],bx
    mov dx,[ui_window_y+FM_WINDOW*2]
    mov [fm_y],dx
    mov cx,FM_WIDTH
    mov si,FM_HEIGHT
    mov ax,70
    call ui_hit_add
    add bx,4
    add dx,5
    mov al,0
    call ui_rect
    mov bx,[fm_x]
    mov dx,[fm_y]
    mov al,7
    call ui_bevel
    add bx,3
    add dx,3
    mov cx,FM_WIDTH-6
    mov si,25
    call ui_windows_title_color
    call ui_title_rail
    mov ax,62
    mov cx,FM_WIDTH-60
    call ui_hit_add
    mov bx,10
    mov dx,7
    mov si,fm_title
    mov ax,0x010F
    call fm_text
    mov bx,FM_WIDTH-53
    mov dx,6
    mov cx,22
    mov si,19
    mov di,ut_min
    mov ax,17
    call fm_button
    mov bx,FM_WIDTH-29
    mov dx,6
    mov di,ut_close
    mov ax,18
    call fm_button
    ; Compact, familiar toolbar; labels describe actual operations.
    xor bp,bp
.toolbar:
    mov si,bp
    shl si,1
    mov di,[fm_button_labels+si]
    mov ax,[fm_button_actions+si]
    imul bx,bp,75
    add bx,10
    mov dx,36
    mov cx,72
    mov si,25
    call fm_button
    inc bp
    cmp bp,8
    jb .toolbar
    mov si,fm_path
    call fm_visible_text
    mov bx,14
    mov dx,76
    mov si,fm_visible
    xor ax,ax
    call fm_text
    mov bx,500
    mov dx,68
    mov cx,109
    mov si,25
    mov di,fm_location
    mov ax,108
    call fm_button
    mov bx,12
    mov dx,105
    mov cx,596
    mov si,232
    mov al,15
    call fm_rect
    cmp byte [md_preview],0
    je .columns
    call md_draw_preview
    jmp .footer
.columns:
    mov bx,55
    mov dx,106
    mov si,fm_name_column
    mov ax,0x0100
    call fm_text
    mov bx,245
    mov dx,106
    mov si,fm_kind_column
    mov ax,0x0100
    call fm_text
    mov bx,440
    mov dx,106
    mov si,fm_bytes_column
    mov ax,0x0100
    call fm_text
    xor bp,bp
.row:
    movzx ax,byte [fm_count]
    cmp bp,ax
    jae .footer
    imul dx,bp,34
    add dx,128
    mov bx,14
    mov cx,590
    mov si,34
    mov ax,bp
    add ax,120
    add bx,[fm_x]
    add dx,[fm_y]
    call ui_hit_add
    mov al,15
    mov word [fm_row_color],0
    movzx ax,byte [fm_selection]
    cmp bp,ax
    mov al,15
    jne .background
    mov al,1
    mov word [fm_row_color],15
.background:
    call ui_rect
    imul di,bp,FM_RECORD
    add di,fm_records
    mov al,2
    test byte [di+13],16
    jz .icon
    mov al,1
.icon:
    sub dx,3
    call ui_icons_draw
    imul dx,bp,34
    add dx,139
    mov bx,57
    mov si,di
    mov ax,[fm_row_color]
    call fm_text
    mov si,fm_file
    test byte [di+13],16
    jz .type
    mov si,fm_folder
.type:
    mov bx,245
    imul dx,bp,34
    add dx,139
    mov ax,[fm_row_color]
    call fm_text
    test byte [di+13],16
    jnz .row_next
    mov eax,[di+14]
    mov di,fm_number
    call fm_decimal32
    mov si,fm_number
    mov bx,440
    imul dx,bp,34
    add dx,139
    mov ax,[fm_row_color]
    call fm_text
.row_next:
    inc bp
    jmp .row
.footer:
    mov bx,12
    mov dx,347
    mov cx,110
    mov si,24
    mov al,[fm_path]
    mov [fm_drive_label],al
    mov di,fm_drive_label
    cmp byte [md_active],0
    je .drive_label
    mov di,md_readonly
.drive_label:
    mov ax,108
    call fm_button
    mov di,fm_number
    mov ax,[fm_total]
    call ui_decimal
    mov si,fm_items_suffix
    call copy_z_to_di
    mov bx,140
    mov dx,352
    mov si,fm_number
    xor ax,ax
    call fm_text
    mov bx,428
    mov dx,347
    mov cx,86
    mov si,24
    mov di,fm_previous
    mov ax,109
    call fm_button
    mov bx,522
    mov dx,347
    mov di,fm_next
    mov ax,110
    call fm_button
    mov bx,12
    mov dx,389
    mov si,[fm_status]
    xor ax,ax
    call fm_text
    cmp byte [fm_prompt],0
    jne .prompt
    cmp byte [fm_busy],0
    je .done
.prompt:
    call fm_draw_prompt
.done:
    pop es
    popad
    ret

fm_draw_prompt:
    mov bx,[fm_x]
    add bx,40
    mov dx,[fm_y]
    add dx,137
    mov cx,540
    mov si,184
    mov al,7
    call ui_bevel
    mov si,fm_copying
    cmp byte [fm_busy],0
    jne .title
    movzx bx,byte [fm_prompt]
    sub bx,103
    shl bx,1
    mov si,[fm_prompt_titles+bx]
.title:
    mov bx,54
    mov dx,151
    mov ax,0x0101
    call fm_text
    mov si,fm_name_hint
    cmp byte [fm_prompt],105
    je .destination
    cmp byte [fm_prompt],106
    je .destination
    cmp byte [fm_prompt],108
    je .destination
    cmp byte [fm_prompt],107
    je .delete
    cmp byte [fm_busy],0
    je .hint
    mov eax,[fm_copied]
    mov di,fm_number
    call fm_decimal32
    mov si,fm_bytes_suffix
    call copy_z_to_di
    mov si,fm_number
    jmp .hint
.destination:
    mov si,fm_dest_hint
    jmp .hint
.delete:
    mov si,fm_delete_hint
.hint:
    mov bx,54
    mov dx,180
    xor ax,ax
    call fm_text
    mov si,fm_edit
    cmp byte [fm_prompt],107
    jne .field
    mov si,fm_source
.field:
    cmp byte [fm_busy],0
    je .visible
    mov si,fm_work
.visible:
    call fm_visible_text
    cmp byte [fm_busy],0
    jne .field_ready
    cmp byte [fm_prompt],107
    je .field_ready
    call fm_edit_visible
.field_ready:
    mov bx,[fm_x]
    add bx,52
    mov dx,[fm_y]
    add dx,207
    mov cx,516
    mov si,28
    call ui_inset
    mov bx,58
    mov dx,214
    mov si,fm_visible
    xor ax,ax
    call fm_text
    cmp byte [fm_busy],0
    jne .buttons
    cmp byte [fm_prompt],107
    je .buttons
    movzx di,byte [fm_edit_cursor]
    sub di,[fm_edit_view]
    mov al,[fm_visible+di]
    push ax
    mov byte [fm_visible+di],0
    mov si,fm_visible
    call ui_measure
    pop ax
    mov [fm_visible+di],al
    mov bx,cx
    add bx,58
    mov dx,212
    mov cx,1
    mov si,17
    xor ax,ax
    call fm_rect
.buttons:
    mov bx,380
    mov dx,272
    mov cx,84
    mov si,27
    mov di,fm_apply
    mov ax,113
    cmp byte [fm_busy],0
    jne .cancel
    cmp byte [fm_prompt],107
    jne .apply
    mov di,fm_delete
.apply:
    call fm_button
.cancel:
    mov bx,474
    mov dx,272
    mov cx,94
    mov si,27
    mov di,ut_cancel
    mov ax,114
    call fm_button
    ret

fm_edit_visible:
    movzx bx,byte [fm_edit_cursor]
    sub bx,43
    jnc .view
    xor bx,bx
.view:
    mov [fm_edit_view],bx
    mov si,fm_edit
    add si,bx
    mov di,fm_visible
    mov cx,44
.copy:
    lodsb
    stosb
    test al,al
    jz .done
    loop .copy
    mov byte [di],0
.done:
    ret

; Visible text stays within the window even with a maximum-length DOS path.
fm_visible_text:
    mov bx,si
    xor cx,cx
.length:
    lodsb
    test al,al
    jz .copy
    inc cx
    jmp .length
.copy:
    mov si,bx
    mov di,fm_visible
    cmp cx,44
    jbe .all
    mov ax,'..'
    stosw
    mov al,'.'
    stosb
    sub cx,41
    add si,cx
.all:
    jmp copy_z_to_di

fm_decimal32:
    pushad
    xor cx,cx
    mov ebx,10
.divide:
    xor edx,edx
    div ebx
    push dx
    inc cx
    test eax,eax
    jnz .divide
.digit:
    pop ax
    add al,'0'
    stosb
    loop .digit
    mov byte [di],0
    ; Preserve all but output DI, like ui_decimal.
    mov bx,sp
    mov [ss:bx],edi
    popad
    ret

fm_title db 'Files',0
fm_button_labels dw fm_up_text,fm_open_text,fm_refresh_text,fm_new,fm_rename,fm_copy,fm_move,fm_delete
fm_button_actions dw 101,100,102,103,104,105,106,107
fm_prompt_titles dw fm_new,fm_rename,fm_copy,fm_move,fm_delete,fm_location
fm_up_text db 'Up',0
fm_open_text db 'Open',0
fm_refresh_text db 'Refresh',0
fm_new db 'New folder',0
fm_rename db 'Rename',0
fm_copy db 'Copy',0
fm_move db 'Move',0
fm_delete db 'Delete',0
fm_location db 'Location...',0
fm_apply db 'Apply',0
fm_previous db 'Previous',0
fm_next db 'Next',0
fm_drive_label db 'C: mounted',0
fm_name_column db 'Name',0
fm_kind_column db 'Kind',0
fm_bytes_column db 'Bytes',0
fm_file db 'File',0
fm_folder db 'Folder',0
fm_items_suffix db ' items',0
fm_bytes_suffix db ' bytes copied',0
fm_wildcard db '*.*',0
fm_bad_chars db '\/:*?"<>|+,;=[]',0
fm_ready db 'Enter: open   F2: rename   F5: refresh   F7: new folder',0
fm_done db 'Done.',0
fm_choose db 'Select a file or folder first.',0
fm_path_long db 'Path is too long. DOS paths must fit in 63 characters.',0
fm_no_association db 'No application is registered for this file type.',0
fm_absolute db 'Use an absolute DOS path, for example C:\APPS\FILE.TXT.',0
fm_name_error db 'Use a DOS 8.3 name, without spaces or path separators.',0
fm_exists db 'Destination already exists. Choose a different name.',0
fm_same_drive db 'This kernel supports file operations within one DOS drive.',0
fm_files_only db 'Copy and Move work on files. Open folders to manage contents.',0
fm_name_hint db 'Name (DOS 8.3). Existing items are never overwritten.',0
fm_dest_hint db 'Full path including name, for example C:\APPS\NOTES.TXT',0
fm_delete_hint db 'Permanently delete this item? Folders must be empty.',0
fm_copying db 'Copying... Press Esc to cancel.',0
fm_copied_ok db 'File copied.',0
fm_cancelled db 'Copy cancelled. Partial destination removed.',0
fm_partial db 'Copy stopped. Could not remove the partial destination.',0
fm_io_error db 'Disk operation failed',0
fm_not_found db 'File or folder not found',0
fm_access db 'Access denied; check read-only state or folder contents',0
fm_no_drive db 'Drive is unavailable or has no readable medium',0
fm_other_volume db 'Other volumes are not mounted. Use Media for CD/USB/floppy.',0
fm_read_only db 'The medium is write-protected',0
fm_full db 'The destination disk is full',0
fm_status dw fm_ready
fm_initialized db 0
fm_mounted_drive db 'C'
fm_busy db 0
fm_prompt db 0
fm_attr db 0
fm_count db 0
fm_selection db 0
fm_last_item db 0xFF
fm_last_click dw 0
fm_edit_len db 0
fm_edit_cursor db 0
fm_edit_view dw 0
fm_top dw 0
fm_total dw 0
fm_x dw 0
fm_y dw 0
fm_row_color dw 0
fm_old_dta dd 0
fm_size dd 0
fm_copied dd 0
fm_copy_tick dw 0
fm_chunk dw 0
fm_transfer dw 0
fm_src_handle dw 0xFFFF
fm_dst_handle dw 0xFFFF
fm_error_code dw 0
fm_path db 'C:\',0
    times FM_PATH-4 db 0
fm_source times FM_PATH db 0
fm_work times FM_PATH db 0
fm_edit times FM_PATH db 0
fm_command times FM_PATH+6 db 0
fm_visible times 48 db 0
fm_number times 32 db 0
fm_error_text times 80 db 0
fm_dta times 48 db 0
fm_records times FM_ROWS*FM_RECORD db 0
