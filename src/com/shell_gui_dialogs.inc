ui_draw_dialog:
    movzx si,byte [ui_dialog]
    shl si,1
    mov bx,[ui_window_x+si]
    mov [ui_dialog_x],bx
    mov dx,[ui_window_y+si]
    mov [ui_dialog_y],dx
    mov si,[ui_window_height+si]
    mov [ui_dialog_h],si
    mov cx,466
    mov ax,70
    call ui_hit_add
    add bx,4
    add dx,5
    U_RECT bx,dx,466,word [ui_dialog_h],0
    mov bx,[ui_dialog_x]
    mov dx,[ui_dialog_y]
    mov cx,466
    mov si,[ui_dialog_h]
    mov al,7
    call ui_bevel
    add bx,3
    add dx,3
    mov cx,460
    mov si,25
    call ui_windows_title_color
    call ui_title_rail
    mov ax,62
    mov cx,406
    mov si,25
    call ui_hit_add
    movzx si,byte [ui_dialog]
    shl si,1
    mov si,[ui_window_titles+si]
.title:
    add bx,7
    add dx,4
    mov ax,0x010F
    call ui_text
    mov bx,[ui_dialog_x]
    add bx,413
    mov dx,[ui_dialog_y]
    add dx,6
    ; Only persistent panels have a task button from which to restore them.
    ; Power, command errors and the launch menu must remain dismissible.
    cmp byte [ui_dialog],3
    je .close_only
    cmp byte [ui_dialog],4
    je .close_only
    cmp byte [ui_dialog],7
    je .close_only
    U_BUTTON bx,dx,22,19,ut_min,17
.close_only:
    add bx,24
    U_BUTTON bx,dx,22,19,ut_close,18
    cmp byte [ui_dialog],10
    je wp_draw
    cmp byte [ui_dialog],5
    jae ui_draw_settings
    cmp byte [ui_dialog],1
    je .run
    cmp byte [ui_dialog],3
    je .power
    cmp byte [ui_dialog],4
    je .error
    ; The portrait is the visual anchor; quiet section rules give the credits
    ; and Ciuk's dedication their own space.
    mov bx,[ui_dialog_x]
    add bx,22
    mov dx,[ui_dialog_y]
    add dx,40
    call ciuki_logo64
    mov bx,[ui_dialog_x]
    add bx,106
    mov dx,[ui_dialog_y]
    add dx,44
    U_TEXT bx,dx,ut_brand,1,1
    add dx,23
    U_TEXT bx,dx,ut_about_version,0
    add dx,20
    U_TEXT bx,dx,ut_tagline,8
    mov bx,[ui_dialog_x]
    add bx,22
    mov dx,[ui_dialog_y]
    add dx,114
    U_RECT bx,dx,422,1,8
    add dx,9
    U_TEXT bx,dx,ut_about_system,1,1
    add dx,20
    U_RECT bx,dx,422,98,15
    add bx,10
    add dx,3
    U_TEXT bx,dx,ut_about_core,1
    add bx,126
    U_TEXT bx,dx,ut_about_core_detail,0
    sub bx,126
    add dx,16
    U_TEXT bx,dx,ut_about_vm,1
    add bx,126
    U_TEXT bx,dx,ut_about_vm_detail,0
    sub bx,126
    add dx,16
    U_TEXT bx,dx,ut_about_input,1
    add bx,126
    U_TEXT bx,dx,ut_about_input_detail,0
    sub bx,126
    add dx,16
    U_TEXT bx,dx,ut_about_audio,1
    add bx,126
    U_TEXT bx,dx,ut_about_audio_detail,0
    sub bx,126
    add dx,16
    U_TEXT bx,dx,ut_about_art,1
    add bx,126
    U_TEXT bx,dx,ut_about_art_detail,0
    sub bx,126
    add dx,16
    U_TEXT bx,dx,ut_about_graphics,1
    add bx,126
    U_TEXT bx,dx,ut_about_graphics_detail,0
    mov bx,[ui_dialog_x]
    add bx,22
    mov dx,[ui_dialog_y]
    add dx,244
    U_RECT bx,dx,422,1,8
    add dx,8
    U_TEXT bx,dx,ut_about_project,1,1
    add dx,19
    U_TEXT bx,dx,ut_about_owner,0
    add dx,18
    U_TEXT bx,dx,ut_about_license,0
    mov bx,[ui_dialog_x]
    add bx,22
    mov dx,[ui_dialog_y]
    add dx,309
    U_RECT bx,dx,422,1,8
    add dx,8
    U_TEXT bx,dx,ut_about_memory,1,1
    add dx,19
    U_TEXT bx,dx,ut_about_ciuk1,1
    add dx,18
    U_TEXT bx,dx,ut_about_ciuk2,1
    add dx,18
    U_TEXT bx,dx,ut_about_ciuk3,1
    jmp .ok
.run:
    mov bx,[ui_dialog_x]
    add bx,22
    mov dx,[ui_dialog_y]
    add dx,45
    U_TEXT bx,dx,ut_run_help,0
    call ui_draw_run_field
    mov bx,[ui_dialog_x]
    add bx,22
    mov dx,[ui_dialog_y]
    add dx,118
    U_TEXT bx,dx,ut_run_example,8
    add dx,22
    U_TEXT bx,dx,ut_run_output,0
    mov bx,[ui_dialog_x]
    add bx,159
    mov dx,[ui_dialog_y]
    add dx,188
    U_BUTTON bx,dx,96,26,ut_window,58
    add bx,104
    U_BUTTON bx,dx,84,26,ut_run_ok,40
    add bx,92
    U_BUTTON bx,dx,88,26,ut_cancel,41
    ret
.power:
    mov bx,[ui_dialog_x]
    add bx,22
    mov dx,[ui_dialog_y]
    add dx,55
    U_TEXT bx,dx,ut_power_help,0
    add dx,27
    cmp byte [ui_boot_drive],3
    jne .power_buttons
    U_TEXT bx,dx,ut_power_live,0
.power_buttons:
    mov bx,[ui_dialog_x]
    add bx,24
    mov dx,[ui_dialog_y]
    add dx,176
    U_BUTTON bx,dx,130,28,ut_restart,42
    add bx,141
    U_BUTTON bx,dx,130,28,ut_shutdown,43
    add bx,141
    U_BUTTON bx,dx,130,28,ut_cancel,41
    ret
.error:
    mov bx,[ui_dialog_x]
    add bx,22
    mov dx,[ui_dialog_y]
    add dx,57
    U_TEXT bx,dx,ut_error_help,0
    add dx,24
    U_TEXT bx,dx,ut_error_dos,0
.ok:
    mov bx,[ui_dialog_x]
    add bx,355
    mov dx,[ui_dialog_y]
    cmp byte [ui_dialog],2
    jne .ok_regular
    add dx,386
    jmp .ok_draw
.ok_regular:
    add dx,188
.ok_draw:
    U_BUTTON bx,dx,88,26,ut_ok,41
    ret

ui_draw_run_field:
    pushad
    push es
    push cs
    pop es
    cld
    mov bx,[ui_dialog_x]
    add bx,22
    mov dx,[ui_dialog_y]
    add dx,76
    mov cx,420
    mov si,28
    call ui_inset
    add bx,6
    add dx,5
    mov [ui_field_x],bx
    mov [ui_field_y],dx
    ; Scroll a 32-character viewport to keep the insertion point visible.
    movzx si,byte [ui_run_cursor]
    cmp si,32
    jbe .view_start
    sub si,32
    jmp .view
.view_start:
    xor si,si
.view:
    mov [ui_run_view],si
    add si,ui_run_text
    mov di,ui_run_visible
    mov cx,32
.copy:
    lodsb
    stosb
    test al,al
    jz .visible
    loop .copy
    mov byte [di],0
.visible:
    U_TEXT bx,dx,ui_run_visible,0
    movzx di,byte [ui_run_cursor]
    sub di,[ui_run_view]
    mov al,[ui_run_visible+di]
    push ax
    mov byte [ui_run_visible+di],0
    mov si,ui_run_visible
    call ui_measure
    pop ax
    mov [ui_run_visible+di],al
    mov bx,[ui_field_x]
    add bx,cx
    mov dx,[ui_field_y]
    U_RECT bx,dx,1,16,0
    pop es
    popad
    ret

ui_clock_poll:
    pushad
    push es
    mov ax,0x40
    mov es,ax
    mov ax,[es:0x6C]
    mov dx,ax
    sub ax,[ui_clock_tick]
    cmp ax,18
    jb .done
    mov [ui_clock_tick],dx
    mov eax,[ui_clock_text]
    mov [ui_old_clock],eax
    mov al,[ui_clock_text+4]
    mov [ui_old_clock+4],al
    mov eax,[ui_date_text]
    mov [ui_old_date],eax
    mov eax,[ui_date_text+4]
    mov [ui_old_date+4],eax
    mov eax,[ui_date_text+8]
    mov [ui_old_date+8],eax
    call ui_clock_read
    mov eax,[ui_clock_text]
    cmp eax,[ui_old_clock]
    jne .draw
    mov al,[ui_clock_text+4]
    cmp al,[ui_old_clock+4]
    jne .draw
    mov eax,[ui_date_text]
    cmp eax,[ui_old_date]
    jne .draw
    mov eax,[ui_date_text+4]
    cmp eax,[ui_old_date+4]
    jne .draw
    mov eax,[ui_date_text+8]
    cmp eax,[ui_old_date+8]
    je .done
.draw:
    ; The BIOS tick lookup uses the BDA (0040), while scene helpers copy
    ; strings inside SHELL. Never render with the BDA as the destination.
    push cs
    pop es
    mov bx,[ui_width]
    sub bx,184
    mov dx,[ui_height]
    sub dx,30
    mov cx,180
    mov si,27
    call ui_comp_damage
    call ui_draw
.done:
    pop es
    popad
    ret

ui_clock_read:
    pushad
    mov ah,2
    int 0x1A
    jc .date
    mov al,ch
    mov di,ui_clock_text
    call .bcd
    inc di
    mov al,cl
    call .bcd
.date:
    mov ah,0x2A
    int 0x21
    mov al,dl
    mov di,ui_date_text
    call .decimal
    movzx bx,dh
    cmp bx,1
    jb .done
    cmp bx,12
    ja .done
    dec bx
    shl bx,2
    mov eax,[ui_months+bx]
    mov [ui_date_text+3],eax
    mov ax,cx
    mov bl,100
    div bl
    mov bh,ah
    mov di,ui_date_text+7
    call .decimal
    mov al,bh
    call .decimal
.done:
    popad
    ret
.bcd:
    mov ah,al
    shr al,4
    and ah,15
    add ax,0x3030
    mov [di],al
    mov [di+1],ah
    add di,2
    ret
.decimal:
    aam
    add ax,0x3030
    mov [di],ah
    mov [di+1],al
    add di,2
    ret

ut_about_version db 'Version 0.8.0',0
ut_about_system db 'SYSTEM',0
ut_about_core db 'Core',0
ut_about_core_detail db 'CiukiDOS kernel and desktop',0
ut_about_vm db 'DOS windows',0
ut_about_vm_detail db 'Jemm386, CVSESSION and HDPMI',0
ut_about_input db 'Input',0
ut_about_input_detail db 'CuteMouse',0
ut_about_audio db 'Audio',0
ut_about_audio_detail db 'SBEMU and VSBHDA',0
ut_about_art db 'Icons / build',0
ut_about_art_detail db 'Tango (Public Domain) / Open Watcom',0
ut_about_graphics db 'Graphics',0
ut_about_graphics_detail db 'TinyGL software OpenGL subset',0
ut_about_project db 'PROJECT',0
ut_about_memory db 'IN MEMORY OF CIUK',0
ut_about_owner db 'Copyright (c) 2026 Alberto Lopez / Alcybercloud.it',0
ut_about_license db 'CiukiOS code: GNU GPL v2. See LICENSE and README.',0
ut_about_ciuk1 db 'In loving memory of Ciuk (Ciuki), the beloved dog',0
ut_about_ciuk2 db 'behind CiukiOS. Though you are no longer here,',0
ut_about_ciuk3 db 'your memory lives on every time this system starts.',0
ui_dialog_h dw 0
ut_run_help db 'Enter a program name or a DOS command.',0
ut_run_example db 'Examples: dir   |   edit notes.txt',0
ut_run_output db 'Run: in a DOS window.   Full screen: the whole screen.',0
ut_run_ok db 'Run',0
ut_window db 'Full screen',0
ut_power_help db 'Choose how to leave CiukiOS.',0
ut_power_live db 'Changes made on the live CD are kept in RAM only.',0
ut_restart db 'Restart',0
ut_shutdown db 'Shut down',0
ut_cancel db 'Cancel',0
ut_ok db 'OK',0
ut_launch_error db 'Command error',0
ut_error_help db 'The command could not be completed.',0
ut_error_dos db 'Check the file name, location and available memory.',0
ui_dialog db 0
ui_dialog_x dw 0
ui_dialog_y dw 0
ui_field_x dw 0
ui_field_y dw 0
ui_run_len db 0
ui_run_cursor db 0
ui_run_view dw 0
ui_run_text times 97 db 0
ui_run_visible times 33 db 0
ui_clock_tick dw 0
ui_clock_text db '--:--',0
ui_old_clock times 5 db 0
ui_date_text db '-- --- ----',0
ui_old_date times 12 db 0
ui_months db 'Jan ','Feb ','Mar ','Apr ','May ','Jun '
          db 'Jul ','Aug ','Sep ','Oct ','Nov ','Dec '
