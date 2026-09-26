; Native Task Manager. Window controls manage shell windows, never MCBs.
; DOS programs execute synchronously: their PSP allocations are read-only here.
; Refresh/open take the DOS snapshot. Painting only reads cached data and UI
; state, so compositor bands never repeat INT 21h calls or filesystem work.
; Integration: window 9; actions 160..179; titles use ui_window_titles.
; ui_task_manager_action: CF=1 handled, AX=250 repaint; CF=0 unrelated.
%ifndef UI_WINDOW_COUNT
%define UI_WINDOW_COUNT 11
%endif
UI_TM_ROWS equ 5
UI_TM_PROCESSES equ 40

ui_task_manager_refresh:
    pushfd
    pushad
    push ds
    push es
    push cs
    pop ds
    cld
    mov byte [ui_tm_valid],0
    mov byte [ui_tm_process_count],0
    mov byte [ui_tm_truncated],0
    xor ax,ax
    mov [ui_tm_used],ax
    mov [ui_tm_free],ax
    mov [ui_tm_largest],ax
    mov es,ax
    mov ax,[es:0x413]
    cmp ax,640
    jbe .kb
    mov ax,640
.kb:
    shl ax,6
    mov bx,[es:0x40E]
    cmp bx,0x1000
    jb .top
    cmp bx,ax
    jae .top
    mov ax,bx
.top:
    mov [ui_tm_limit],ax
    mov ah,0x51
    int 0x21
    mov [ui_tm_psp],bx
    mov ah,0x52
    int 0x21
    cmp bx,2
    jb .done
    mov ax,[es:bx-2]
    mov di,4096
.mcb:
    cmp ax,0x0100
    jb .done
    cmp ax,[ui_tm_limit]
    jae .done
    mov es,ax
    mov dl,[es:0]
    cmp dl,'M'
    je .type_ok
    cmp dl,'Z'
    jne .done
.type_ok:
    mov cx,[es:3]
    mov bp,ax
    add bp,cx
    jc .done
    inc bp
    jz .done
    cmp bp,[ui_tm_limit]
    ja .done
    mov bx,[es:1]
    test bx,bx
    jnz .owned
    add [ui_tm_free],cx
    cmp cx,[ui_tm_largest]
    jbe .next
    mov [ui_tm_largest],cx
    jmp .next
.owned:
    add [ui_tm_used],cx
    call ui_tm_record_owner
.next:
    cmp dl,'Z'
    je .valid
    mov ax,bp
    dec di
    jnz .mcb
    jmp .done
.valid:
    mov byte [ui_tm_valid],1
.done:
    pop es
    pop ds
    popad
    popfd
    ret

; BX owner PSP, CX owned paragraphs. Group actual PSPs, not every MCB owner.
ui_tm_record_owner:
    pusha
    push es
    cmp bx,0x0100
    jb .done
    cmp bx,[ui_tm_limit]
    jae .done
    mov es,bx
    cmp word [es:0],0x20CD
    jne .done
    xor si,si
    xor dx,dx
    mov dl,[ui_tm_process_count]
.find:
    test dx,dx
    jz .new
    cmp [ui_tm_processes+si],bx
    je .add
    add si,4
    dec dx
    jmp .find
.new:
    cmp byte [ui_tm_process_count],UI_TM_PROCESSES
    jae .full
    inc byte [ui_tm_process_count]
    mov [ui_tm_processes+si],bx
    mov word [ui_tm_processes+si+2],0
.add:
    add [ui_tm_processes+si+2],cx
    jmp .done
.full:
    mov byte [ui_tm_truncated],1
.done:
    pop es
    popa
    ret

; Native window state is read live: windows may close while this panel stays
; open. The shell's Programs visibility is separate from its dialog flags.
ui_tm_scan_windows:
    pusha
    xor bx,bx
    xor di,di
.next:
    mov al,[ui_window_flags+bx]
    test bx,bx
    jnz .state
    mov al,[ui_window_visible]
.state:
    test al,al
    jz .skip
    mov [ui_tm_windows+di],bl
    inc di
.skip:
    inc bx
    cmp bx,UI_WINDOW_COUNT
    jb .next
    mov [ui_tm_window_count],di
    popa
    ret

ui_task_manager_action:
    cmp ax,160
    jb .unhandled
    cmp ax,179
    ja .unhandled
    cmp ax,160
    je .refresh
    cmp ax,165
    jb .controls
    cmp ax,167
    ja .row
    sub al,165
    mov [ui_tm_tab],al
    mov byte [ui_tm_first],0
    mov byte [ui_tm_selected],0
    jmp .handled
.controls:
    cmp ax,163
    je .previous
    cmp ax,164
    je .next
    cmp byte [ui_tm_tab],0
    jne .handled
    call ui_tm_scan_windows
    movzx bx,byte [ui_tm_selected]
    cmp bx,[ui_tm_window_count]
    jae .handled
    mov bl,[ui_tm_windows+bx]
    cmp ax,162
    je .close
    test bx,bx
    jnz .switch
    mov byte [ui_window_visible],1
.switch:
    mov [ui_dialog],bl
    call ui_windows_open
    jmp .handled
.close:
    call ui_windows_raise
    test bx,bx
    jnz .dialog_close
    mov byte [ui_window_visible],0
    mov byte [ui_bar_dirty],1
    call ui_windows_unfocus
    jmp .handled
.dialog_close:
    call ui_windows_close
    jmp .handled
.previous:
    cmp byte [ui_tm_first],0
    je .handled
    sub byte [ui_tm_first],UI_TM_ROWS
    mov al,[ui_tm_first]
    mov [ui_tm_selected],al
    jmp .handled
.next:
    call ui_tm_count
    movzx bx,byte [ui_tm_first]
    add bx,UI_TM_ROWS
    cmp bx,cx
    jae .handled
    mov [ui_tm_first],bl
    mov [ui_tm_selected],bl
    jmp .handled
.row:
    cmp ax,170
    jb .handled
    cmp ax,174
    ja .handled
    sub al,170
    add al,[ui_tm_first]
    mov [ui_tm_selected],al
    jmp .handled
.refresh:
    call ui_task_manager_refresh
.handled:
    mov ax,250
    stc
    ret
.unhandled:
    clc
    ret

ui_tm_count:
    call ui_tm_scan_windows
    mov cx,[ui_tm_window_count]
    cmp byte [ui_tm_tab],0
    je .done
    xor cx,cx
    cmp byte [ui_tm_valid],1
    jne .done
    mov cl,[ui_tm_process_count]
.done:
    ret

; Optional ui_event hook, before generic Enter handling. AX is the BIOS key;
; CF=1 returns a UI action in AX, CF=0 preserves an unrecognized key.
ui_task_manager_key:
    cmp byte [ui_dialog],9
    jne .unhandled
    cmp ah,0x3F
    je .refresh
    cmp ah,0x4B
    je .tab_left
    cmp ah,0x4D
    je .tab_right
    cmp al,13
    jne .navigation
    cmp byte [ui_tm_tab],0
    jne .unhandled
    cmp word [ui_focus],170
    jb .unhandled
    cmp word [ui_focus],174
    ja .unhandled
    mov ax,161
    jmp .handled
.navigation:
    cmp ah,0x49
    je .previous
    cmp ah,0x51
    je .next
    cmp ah,0x48
    je .move
    cmp ah,0x50
    je .move
    cmp ah,0x47
    je .move
    cmp ah,0x4F
    jne .unhandled
.move:
    cmp byte [ui_tm_tab],2
    je .idle
    mov dx,ax
    call ui_tm_count
    jcxz .idle
    movzx bx,byte [ui_tm_selected]
    cmp dh,0x47
    je .home
    cmp dh,0x4F
    je .end
    cmp dh,0x48
    jne .down
    test bx,bx
    jz .idle
    dec bx
    jmp .selected
.down:
    inc bx
    cmp bx,cx
    jb .selected
.end:
    mov bx,cx
    dec bx
    jmp .selected
.home:
    xor bx,bx
.selected:
    mov [ui_tm_selected],bl
    mov ax,bx
    xor dx,dx
    mov cx,UI_TM_ROWS
    div cx
    mul cx
    mov [ui_tm_first],al
    mov ax,bx
    sub al,[ui_tm_first]
    add ax,170
    mov [ui_focus],ax
    mov ax,250
    jmp .handled
.tab_left:
    mov al,[ui_tm_tab]
    test al,al
    jnz .left_step
    mov al,3
.left_step:
    dec al
    jmp .tab
.tab_right:
    mov al,[ui_tm_tab]
    inc al
    cmp al,3
    jb .tab
    xor al,al
.tab:
    xor ah,ah
    add ax,165
    jmp .handled
.refresh:
    mov ax,160
    jmp .handled
.previous:
    mov ax,163
    jmp .handled
.next:
    mov ax,164
    jmp .handled
.idle:
    xor ax,ax
.handled:
    stc
    ret
.unhandled:
    clc
    ret

ui_task_manager_draw:
    pushad
    push es
    push cs
    pop es
    cld
    mov bx,18
    mov dx,36
    mov cx,112
    mov si,23
    mov di,ui_tm_windows_text
    mov ax,165
    call ui_tm_button
    add bx,118
    mov di,ui_tm_processes_text
    inc ax
    call ui_tm_button
    add bx,118
    mov di,ui_tm_memory_text
    inc ax
    call ui_tm_button
    movzx bx,byte [ui_tm_tab]
    imul bx,118
    add bx,19
    mov dx,59
    mov cx,110
    mov si,2
    mov al,1
    call ui_tm_rect
    cmp byte [ui_tm_tab],2
    je .memory
    call ui_tm_count
    mov [ui_tm_count_now],cx
    movzx ax,byte [ui_tm_first]
    cmp ax,cx
    jb .page_ready
    mov byte [ui_tm_first],0
.page_ready:
    mov bx,20
    mov dx,64
    mov si,ui_tm_window_header
    cmp byte [ui_tm_tab],0
    je .header
    mov si,ui_tm_process_header
.header:
    mov ax,8
    call ui_tm_text
    mov byte [ui_tm_row],0
.row:
    movzx bp,byte [ui_tm_row]
    movzx di,byte [ui_tm_first]
    add di,bp
    cmp di,[ui_tm_count_now]
    jae .list_footer
    imul dx,bp,19
    add dx,83
    mov bx,18
    mov cx,386
    mov si,18
    xor ax,ax
    mov al,[ui_tm_row]
    add ax,170
    push bx
    push dx
    add bx,[ui_dialog_x]
    add dx,[ui_dialog_y]
    call ui_hit_add
    pop dx
    pop bx
    mov byte [ui_tm_color],0
    movzx ax,byte [ui_tm_selected]
    cmp ax,di
    jne .row_text
    mov al,1
    call ui_tm_rect
    mov byte [ui_tm_color],15
.row_text:
    add bx,4
    inc dx
    cmp byte [ui_tm_tab],0
    jne .process
    movzx bp,byte [ui_tm_windows+di]
    mov si,bp
    shl si,1
    mov si,[ui_window_titles+si]
    movzx ax,byte [ui_tm_color]
    call ui_tm_text
    mov si,ui_tm_open_text
    cmp byte [ds:ui_window_flags+bp],2
    jne .active
    mov si,ui_tm_minimized_text
.active:
    movzx ax,byte [ui_active_window]
    cmp ax,bp
    jne .status
    mov si,ui_tm_focused_text
.status:
    mov bx,270
    movzx ax,byte [ui_tm_color]
    call ui_tm_text
    jmp .row_done
.process:
    shl di,2
    mov bp,di
    mov ax,[ds:ui_tm_processes+bp]
    call ui_tm_hex
    mov si,ui_tm_number
    movzx ax,byte [ui_tm_color]
    call ui_tm_text
    mov ax,[ds:ui_tm_processes+bp+2]
    mov bx,129
    call ui_tm_kib
    mov si,ui_tm_allocated_text
    mov ax,[ds:ui_tm_processes+bp]
    cmp ax,[ui_tm_psp]
    jne .process_status
    mov si,ui_tm_shell_text
.process_status:
    mov bx,244
    movzx ax,byte [ui_tm_color]
    call ui_tm_text
.row_done:
    inc byte [ui_tm_row]
    cmp byte [ui_tm_row],UI_TM_ROWS
    jb .row
.list_footer:
    mov bx,411
    mov dx,83
    mov cx,36
    mov si,24
    mov di,ui_tm_up_text
    mov ax,163
    call ui_tm_button
    add dx,32
    mov di,ui_tm_down_text
    inc ax
    call ui_tm_button
    cmp byte [ui_tm_tab],0
    jne .process_note
    mov bx,170
    mov dx,188
    mov cx,126
    mov si,26
    mov di,ui_tm_switch_text
    mov ax,161
    call ui_tm_button
    add bx,134
    mov di,ui_tm_close_text
    inc ax
    call ui_tm_button
    jmp .footer
.process_note:
    mov bx,136
    mov dx,194
    mov si,ui_tm_readonly_text
    mov ax,8
    call ui_tm_text
    cmp byte [ui_tm_valid],1
    jne .unavailable
    cmp byte [ui_tm_truncated],0
    je .footer
    mov si,ui_tm_truncated_text
    jmp .notice
.memory:
    cmp byte [ui_tm_valid],1
    jne .unavailable
    mov byte [ui_tm_color],0
    xor bp,bp
.metric:
    mov bx,22
    imul dx,bp,21
    add dx,77
    mov si,bp
    shl si,1
    mov si,[ui_tm_metric_labels+si]
    xor ax,ax
    call ui_tm_text
    mov si,bp
    shl si,1
    mov si,[ui_tm_metric_values+si]
    mov ax,[si]
    mov bx,312
    call ui_tm_kib
    inc bp
    cmp bp,4
    jb .metric
    mov si,ui_tm_memory_note
.notice:
    mov bx,22
    mov dx,165
    mov ax,8
    call ui_tm_text
    jmp .footer
.unavailable:
    mov bx,22
    mov dx,84
    mov si,ui_tm_unavailable_text
    xor ax,ax
    call ui_tm_text
.footer:
    mov bx,18
    mov dx,188
    mov cx,108
    mov si,26
    mov di,ui_tm_refresh_text
    mov ax,160
    call ui_tm_button
    pop es
    popad
    ret

; All text buffers remain in the shell segment, as required by ui_text.
ui_tm_kib:
    mov di,ui_tm_number
    shr ax,6
    call ui_decimal
    mov ax,' K'
    stosw
    mov ax,'iB'
    stosw
    xor al,al
    stosb
    mov si,ui_tm_number
    movzx ax,byte [ui_tm_color]
    jmp ui_tm_text
ui_tm_hex:
    push ax
    push cx
    mov di,ui_tm_number
    mov cx,4
.digit:
    rol ax,4
    push ax
    and al,15
    add al,'0'
    cmp al,'9'
    jbe .store
    add al,7
.store:
    stosb
    pop ax
    loop .digit
    mov byte [es:di],0
    pop cx
    pop ax
    ret
ui_tm_text:
    push bx
    push dx
    add bx,[ui_dialog_x]
    add dx,[ui_dialog_y]
    call ui_text
    pop dx
    pop bx
    ret
ui_tm_button:
    push bx
    push dx
    add bx,[ui_dialog_x]
    add dx,[ui_dialog_y]
    call ui_button
    pop dx
    pop bx
    ret
ui_tm_rect:
    push bx
    push dx
    add bx,[ui_dialog_x]
    add dx,[ui_dialog_y]
    call ui_rect
    pop dx
    pop bx
    ret

ut_task_manager db 'Task Manager',0
ui_tm_windows_text db 'Windows',0
ui_tm_processes_text db 'Processes',0
ui_tm_memory_text db 'Memory',0
ui_tm_window_header db 'Shell window                          State',0
ui_tm_process_header db 'Owner PSP       Owned RAM       Process',0
ui_tm_open_text db 'Open',0
ui_tm_minimized_text db 'Minimized',0
ui_tm_focused_text db 'Focused',0
ui_tm_shell_text db 'CiukiOS Shell',0
ui_tm_allocated_text db 'DOS process',0
ui_tm_readonly_text db 'DOS process allocations are read-only.',0
ui_tm_truncated_text db 'More owners exist; memory totals include them.',0
ui_tm_unavailable_text db 'Memory map unavailable. Select Refresh to retry.',0
ui_tm_memory_note db 'Conventional DOS memory; values rounded down.',0
ui_tm_refresh_text db 'Refresh',0
ui_tm_switch_text db 'Switch to',0
ui_tm_close_text db 'Close window',0
ui_tm_up_text db '^',0
ui_tm_down_text db 'v',0
ui_tm_total_text db 'Conventional RAM',0
ui_tm_used_text db 'Allocated DOS blocks',0
ui_tm_free_text db 'Free DOS blocks',0
ui_tm_largest_text db 'Largest free block',0
ui_tm_metric_labels dw ui_tm_total_text,ui_tm_used_text,ui_tm_free_text,ui_tm_largest_text
ui_tm_metric_values dw ui_tm_limit,ui_tm_used,ui_tm_free,ui_tm_largest
ui_tm_valid db 0
ui_tm_truncated db 0
ui_tm_process_count db 0
ui_tm_tab db 0
ui_tm_first db 0
ui_tm_selected db 0
ui_tm_row db 0
ui_tm_color db 0
ui_tm_psp dw 0
ui_tm_limit dw 0
ui_tm_used dw 0
ui_tm_free dw 0
ui_tm_largest dw 0
ui_tm_count_now dw 0
ui_tm_window_count dw 0
ui_tm_windows times UI_WINDOW_COUNT db 0
ui_tm_processes times UI_TM_PROCESSES*4 db 0
ui_tm_number times 12 db 0
