; Editable, wrapping command line. The anchor follows console scrolling;
; both the BIOS text console and the private VBE console use the same editor.
read_line:
%ifndef COMMAND_COMPAT
    call vc_flush
%endif
    xor bx, bx
    mov [input_pos], bx
    mov byte [history_nav], 0xFF
    mov byte [input_draw_len], 0
    mov byte [input_buf], 0
    mov byte [completion_active], 0
    call input_get_anchor
.read:
%ifdef COMMAND_COMPAT
    call command_window_poll
%endif
    xor ah, ah
    int 0x16
    cmp al, 9
    je .complete
    mov byte [completion_active], 0
    cmp al, 13
    je .done
    cmp al, 3
    je .cancel
    cmp al, 27
    je .clear
    cmp al, 8
    je .backspace
    test al, al
    jz .extended
    cmp al, 0xE0
    je .extended
    cmp al, 32
    jb .read
    cmp bx, INPUT_BUF_MAX
    jae .read
    mov [input_key], al
    mov di, bx
    mov cx, bx
    sub cx, [input_pos]
    inc cx
.shift:
    mov al, [input_buf + di]
    mov [input_buf + di + 1], al
    dec di
    loop .shift
    mov di, [input_pos]
    mov al, [input_key]
    mov [input_buf + di], al
    inc bx
    inc word [input_pos]
    mov byte [history_nav], 0xFF
    cmp [input_pos], bx
    jne .repaint
    call dual_putc
    mov [input_draw_len], bl
    call input_adjust_anchor
    jmp .read
.extended:
    cmp ah, 0x48
    je .history_up
    cmp ah, 0x50
    je .history_down
    cmp ah, 0x4B
    je .left
    cmp ah, 0x4D
    je .right
    cmp ah, 0x47
    je .home
    cmp ah, 0x4F
    je .end
    cmp ah, 0x53
    je .delete
    cmp ah, 0x3D
    je .last
%ifndef COMMAND_COMPAT
    cmp ah, 0x3B
    je .help
    cmp ah, 0x3C
    je .apps
%endif
    jmp .read
.left:
    cmp word [input_pos], 0
    je .read
    dec word [input_pos]
    jmp .position
.right:
    cmp [input_pos], bx
    jae .read
    inc word [input_pos]
    jmp .position
.home:
    mov word [input_pos], 0
    jmp .position
.end:
    mov [input_pos], bx
.position:
    call input_position
    jmp .read
.backspace:
    cmp word [input_pos], 0
    je .read
    dec word [input_pos]
.delete:
    mov di, [input_pos]
    cmp di, bx
    jae .read
.delete_copy:
    mov al, [input_buf + di + 1]
    mov [input_buf + di], al
    inc di
    test al, al
    jnz .delete_copy
    dec bx
    mov byte [history_nav], 0xFF
    jmp .repaint
.last:
    mov byte [history_nav], 0xFF
.history_up:
    call history_recall_up
    jnc .read
    jmp .history_loaded
.history_down:
    call history_recall_down
    jnc .read
.history_loaded:
    mov [input_pos], bx
    jmp .repaint
.complete:
    call input_complete
    mov [input_pos], bx
.repaint:
    call redraw_input_line
    jmp .read
.clear:
    xor bx, bx
    mov [input_pos], bx
    mov byte [input_buf], 0
    mov byte [history_nav], 0xFF
    jmp .repaint
.cancel:
    mov [input_pos], bx
    call input_position
    mov si, msg_ctrl_c
    call print_dual_dollar_string
    xor cx, cx
    mov si, input_buf
    ret
%ifndef COMMAND_COMPAT
.help:
    mov si, dc_help
    jmp .shortcut
.apps:
    mov si, dc_apps
.shortcut:
    mov di, input_buf
    call copy_z_to_di
    mov bx, 4
    mov [input_pos], bx
    call redraw_input_line
%endif
.done:
    mov [input_pos], bx
    call input_position
    mov byte [input_buf + bx], 0
    call history_try_store
    mov cx, bx
    mov si, msg_crlf
    call print_dual_dollar_string
    mov si, input_buf
%ifndef COMMAND_COMPAT
    mov byte [vc_batch],1
%endif
    ret

input_get_anchor:
    pushad
    push es
%ifndef COMMAND_COMPAT
    cmp byte [vc_active], 1
    jne .text
    mov ax, [vc_cols]
    mov [input_columns], ax
    mov ax, [vc_rows]
    mov [input_rows], ax
    mov ax, [vc_x]
    mov [input_anchor_x], ax
    mov ax, [vc_y]
    mov [input_anchor_y], ax
    jmp .done
.text:
%endif
    mov ax, 0x0040
    mov es, ax
    mov ax, [es:0x4A]
    mov [input_columns], ax
    movzx ax, byte [es:0x84]
    inc ax
    mov [input_rows], ax
    mov ah, 3
    xor bh, bh
    int 0x10
    movzx ax, dl
    mov [input_anchor_x], ax
    movzx ax, dh
    mov [input_anchor_y], ax
.done:
    pop es
    popad
    ret

input_adjust_anchor:
    pushad
    movzx ax, byte [input_draw_len]
    add ax, [input_anchor_x]
    xor dx, dx
    div word [input_columns]
    add ax, [input_anchor_y]
    sub ax, [input_rows]
    jb .done
    inc ax
    sub [input_anchor_y], ax
.done:
    popad
    ret

input_position:
    pushad
    mov ax, [input_pos]
    add ax, [input_anchor_x]
    xor dx, dx
    div word [input_columns]
    add ax, [input_anchor_y]
    ; AX=row, DX=column.
%ifndef COMMAND_COMPAT
    cmp byte [vc_active], 1
    jne .text
    push ax
    push dx
    call vc_cell_index
    call vc_draw_cell
    pop dx
    pop ax
    mov [vc_x], dx
    mov [vc_y], ax
    call vc_cursor
    jmp .done
.text:
%endif
    mov dh, al
    mov ah, 2
    xor bh, bh
    int 0x10
.done:
    popad
    ret

redraw_input_line:
    pushad
    mov bp, [input_pos]
    mov word [input_pos], 0
    call input_position
    mov si, input_buf
    mov cx, bx
    jcxz .padding
    call print_dual_cx_string
.padding:
    movzx dx, byte [input_draw_len]
    cmp dx, bx
    jbe .length
    mov cx, dx
    sub cx, bx
    mov al, ' '
.erase:
    call dual_putc
    loop .erase
    jmp .adjust
.length:
    mov [input_draw_len], bl
.adjust:
    call input_adjust_anchor
    mov [input_draw_len], bl
    mov [input_pos], bp
    call input_position
    popad
    ret

input_complete:
    mov [completion_length], bx
    mov byte [completion_wrapped], 0
    push ax
    push cx
    push dx
    push si
    push di
    push bp
    push es
    mov ah, 0x2F
    int 0x21
    mov [completion_old_dta], bx
    mov [completion_old_dta + 2], es
    push cs
    pop es
    mov dx, completion_dta
    mov ah, 0x1A
    int 0x21
    cmp byte [completion_active], 1
    je .next
    mov bx, [completion_length]
    mov si, bx
.token:
    test si, si
    jz .start
    cmp byte [input_buf + si - 1], ' '
    je .start
    dec si
    jmp .token
.start:
    mov [completion_prefix], si
    mov di, completion_pattern
    mov byte [completion_has_dot], 0
.pattern:
    cmp si, bx
    jae .suffix
    mov al, [input_buf + si]
    stosb
    inc si
    cmp al, '\'
    je .separator
    cmp al, '/'
    je .separator
    cmp al, ':'
    je .separator
    cmp al, '.'
    jne .pattern
    mov byte [completion_has_dot], 1
    jmp .pattern
.separator:
    mov [completion_prefix], si
    mov byte [completion_has_dot], 0
    jmp .pattern
.suffix:
    mov al, '*'
    stosb
    cmp byte [completion_has_dot], 1
    je .terminate
    mov ax, '.*'
    stosw
.terminate:
    mov byte [di], 0
    mov byte [completion_active], 1
.first:
    mov dx, completion_pattern
    mov cx, 0x10
    mov ah, 0x4E
    int 0x21
    jc .done
    jmp .found
.next:
    mov ah, 0x4F
    int 0x21
    jnc .found
    cmp byte [completion_wrapped], 1
    je .done
    mov byte [completion_wrapped], 1
    jmp .first
.found:
    cmp byte [completion_dta + 30], '.'
    je .next
    mov bx, [completion_prefix]
    mov si, completion_dta + 30
.name:
    lodsb
    test al, al
    jz .finish
    cmp bx, INPUT_BUF_MAX
    jae .finish
    mov [input_buf + bx], al
    inc bx
    jmp .name
.finish:
    test byte [completion_dta + 21], 0x10
    jz .end_name
    cmp bx, INPUT_BUF_MAX
    jae .end_name
    mov byte [input_buf + bx], '\'
    inc bx
.end_name:
    mov byte [input_buf + bx], 0
    mov [completion_length], bx
.done:
    push ds
    lds dx, [completion_old_dta]
    mov ah, 0x1A
    int 0x21
    pop ds
    mov bx, [completion_length]
    pop es
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop ax
    ret

input_pos dw 0
input_anchor_x dw 0
input_anchor_y dw 0
input_columns dw 80
input_rows dw 25
input_key db 0
completion_active db 0
completion_wrapped db 0
completion_has_dot db 0
completion_length dw 0
completion_prefix dw 0
completion_old_dta dd 0
completion_pattern times 132 db 0
completion_dta times 48 db 0
