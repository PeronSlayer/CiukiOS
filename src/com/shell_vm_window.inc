; A DOS window belongs to DOSVM.APP.  SHELL.COM only opens that module and
; falls back to its full-screen DOS path when the VM manager is unavailable.
dw_vm_available:
    push ax
    push bx
    push di
    push es
    xor ax,ax
    mov es,ax
    mov ax,1684h
    mov bx,4349h
    int 2Fh
    mov ax,es
    or ax,di
    jz .missing
    pop es
    pop di
    pop bx
    pop ax
    clc
    ret
.missing:
    pop es
    pop di
    pop bx
    pop ax
    stc
    ret

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
    call dw_vm_available
    jc ui_to_dos
    mov bl,11
    mov si,dw_empty_command
    call app_open
    jmp ui_redraw

ui_run_window:
    cmp byte [ui_run_len],0
    je ui_redraw
    cmp byte [ui_active_window],1
    jne .not_run_dialog
    call ui_windows_close
.not_run_dialog:
    call dw_vm_available
    jc .full_screen
    mov bl,11
    mov si,ui_run_text
    call app_open
    jmp ui_redraw
.full_screen:
    mov si,ui_run_text
    jmp ui_launch_command

ut_dos_window db 'DOS window',0
dw_empty_command db 0
