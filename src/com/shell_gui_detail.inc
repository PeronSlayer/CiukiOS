; Desktop details and native control panels. All controls perform real actions.
ui_desktop_detail:
    pushad
    call ui_display_text
    mov si,ui_mode_text
    call ui_measure
    mov bx,[ui_width]
    sub bx,cx
    sub bx,32
    mov dx,[ui_height]
    sub dx,54
    mov ax,7
    call ui_text
    popad
    ret

ui_display_text:
    pushad
    push es
    push cs
    pop es
    mov di,ui_mode_text
    mov ax,[ui_width]
    call ui_decimal
    mov al,'x'
    stosb
    mov ax,[ui_height]
    call ui_decimal
    mov ax,'  '
    stosw
    mov al,'/'
    stosb
    mov al,' '
    stosb
    mov ax,4
    cmp byte [ui_vbe],0
    je .depth
    movzx ax,byte [vc_info+25]
.depth:
    call ui_decimal
    mov si,ut_bit_color
.copy:
    lodsb
    stosb
    test al,al
    jnz .copy
    pop es
    popad
    ret

ui_decimal:
    push ax
    push bx
    push cx
    push dx
    xor cx,cx
    mov bx,10
.divide:
    xor dx,dx
    div bx
    push dx
    inc cx
    test ax,ax
    jnz .divide
.digit:
    pop ax
    add al,'0'
    stosb
    loop .digit
    pop dx
    pop cx
    pop bx
    pop ax
    ret

ui_library_detail:
    pushad
    mov word [ui_sidebar_width],0
    cmp word [ui_ww],680
    jb .toolbar
    mov word [ui_sidebar_width],146
    mov bx,[ui_wx]
    add bx,11
    mov dx,[ui_wy]
    add dx,73
    mov si,[ui_wh]
    sub si,114
    U_RECT bx,dx,137,si,7
    add bx,136
    U_RECT bx,dx,1,si,8
    sub bx,136
    add bx,10
    add dx,10
    U_TEXT bx,dx,ut_places,1,1
    add dx,27
    U_BUTTON bx,dx,114,27,ut_files,2
    add dx,32
    U_BUTTON bx,dx,114,27,ut_dos,3
    add dx,32
    U_BUTTON bx,dx,114,27,ut_display,8
    add dx,32
    U_BUTTON bx,dx,114,27,ut_sound,9
    add dx,43
    U_TEXT bx,dx,ut_shortcuts,8,1
    add dx,22
    U_TEXT bx,dx,ut_keys_run,0
    add dx,18
    U_TEXT bx,dx,ut_keys_dos,0
.toolbar:
    mov bx,[ui_wx]
    add bx,22
    add bx,[ui_sidebar_width]
    mov dx,[ui_wy]
    add dx,80
    U_TEXT bx,dx,ut_open_hint,0
    popad
    ret

ui_item_detail:
    pushad
    mov si,ut_dos_app
    cmp byte [ui_page],1
    je .label
    mov si,ut_system_tool
    cmp byte [ui_page],2
    je .label
    movzx bx,byte [ui_draw_item]
    shl bx,1
    mov si,[ui_item_types+bx]
.label:
    call ui_measure
    mov bx,137
    sub bx,cx
    shr bx,1
    add bx,[ui_tile_x]
    mov dx,[ui_tile_y]
    add dx,79
    mov ax,8
    call ui_text
    popad
    ret

ui_icon_windows:
    U_RECT bx,dx,44,36,0
    inc bx
    inc dx
    U_RECT bx,dx,42,34,7
    inc bx
    inc dx
    U_RECT bx,dx,40,6,1
    add bx,4
    add dx,10
    U_RECT bx,dx,16,15,1
    add bx,2
    add dx,2
    U_RECT bx,dx,12,3,9
    add dx,5
    U_RECT bx,dx,12,6,15
    sub bx,2
    sub dx,7
    add bx,20
    U_RECT bx,dx,14,3,8
    add dx,6
    U_RECT bx,dx,14,3,8
    add dx,6
    U_RECT bx,dx,9,3,8
    jmp ui_icon.done

ui_icon_costa:
    U_RECT bx,dx,44,36,0
    inc bx
    inc dx
    U_RECT bx,dx,42,34,3
    add bx,4
    add dx,4
    U_RECT bx,dx,30,20,7
    U_RECT bx,dx,30,5,10
    add bx,10
    add dx,12
    U_RECT bx,dx,26,16,15
    U_RECT bx,dx,26,4,9
    jmp ui_icon.done

ui_icon_display:
    U_RECT bx,dx,44,33,0
    inc bx
    inc dx
    U_RECT bx,dx,42,30,7
    add bx,3
    add dx,3
    U_RECT bx,dx,36,23,3
    add bx,4
    add dx,3
    U_RECT bx,dx,28,16,15
    U_RECT bx,dx,28,4,1
    add bx,3
    add dx,7
    U_RECT bx,dx,6,6,14
    add bx,10
    U_RECT bx,dx,8,2,8
    add dx,4
    U_RECT bx,dx,13,2,8
    sub bx,4
    sub dx,14
    add dx,28
    U_RECT bx,dx,10,5,8
    sub bx,8
    add dx,5
    U_RECT bx,dx,26,3,7
    jmp ui_icon.done

ui_icon_run:
    U_RECT bx,dx,44,33,0
    inc bx
    inc dx
    U_RECT bx,dx,42,31,7
    inc bx
    inc dx
    U_RECT bx,dx,40,5,1
    add bx,4
    add dx,12
    U_RECT bx,dx,32,10,15
    add bx,3
    U_TEXT bx,dx,ut_prompt,1
    jmp ui_icon.done

ui_draw_settings:
    mov bx,[ui_dialog_x]
    add bx,22
    mov dx,[ui_dialog_y]
    add dx,43
    cmp byte [ui_dialog],6
    je .sound
    cmp byte [ui_dialog],7
    je .menu
    U_TEXT bx,dx,ui_mode_text,1,1
    add dx,23
    U_TEXT bx,dx,ut_auto_help,0
    add dx,27
    U_BUTTON bx,dx,420,26,ut_automatic,50
    add dx,33
    U_BUTTON bx,dx,132,25,ut_mode800,52
    add bx,144
    U_BUTTON bx,dx,132,25,ut_mode1024,53
    add bx,144
    U_BUTTON bx,dx,132,25,ut_mode1280,90
    mov bx,[ui_dialog_x]
    add bx,22
    add dx,31
    U_BUTTON bx,dx,132,25,ut_mode1600,91
    add bx,144
    U_BUTTON bx,dx,132,25,ut_mode1920,92
    add bx,144
    U_BUTTON bx,dx,132,25,ut_mode2560,93
    mov bx,[ui_dialog_x]
    add bx,22
    mov dx,[ui_dialog_y]
    add dx,188
    U_BUTTON bx,dx,160,26,ut_display_setup,94
    jmp ui_draw_dialog.ok
.sound:
    mov si,ut_startup_on
    cmp byte [ui_sound_preference_value],'0'
    jne .sound_state
    mov si,ut_startup_off
.sound_state:
    mov ax,0x0101
    call ui_text
    add dx,24
    U_TEXT bx,dx,ut_sound_melody,0
    add dx,35
    U_BUTTON bx,dx,132,29,ut_sound_test,55
    add bx,144
    U_BUTTON bx,dx,132,29,ut_sound_on,56
    add bx,144
    U_BUTTON bx,dx,132,29,ut_sound_off,57
    mov bx,[ui_dialog_x]
    add bx,22
    add dx,40
    mov si,[ui_sfx_status]
    cmp si,7
    jbe .status_valid
    mov si,7
.status_valid:
    shl si,1
    mov si,[ui_sfx_messages+si]
    mov ax,1
    call ui_text
    jmp ui_draw_dialog.ok
.menu:
    U_BUTTON bx,dx,202,29,ut_programs,1
    add bx,218
    U_BUTTON bx,dx,202,29,ut_files,2
    sub bx,218
    add dx,37
    U_BUTTON bx,dx,202,29,ut_run,4
    add bx,218
    U_BUTTON bx,dx,202,29,ut_dos,3
    sub bx,218
    add dx,37
    U_BUTTON bx,dx,202,29,ut_display,8
    add bx,218
    U_BUTTON bx,dx,202,29,ut_sound,9
    sub bx,218
    add dx,37
    U_BUTTON bx,dx,202,29,ut_about,6
    add bx,218
    U_BUTTON bx,dx,202,29,ut_power,7
    ret

ui_sound_preference:
    pushad
    mov byte [ui_sound_preference_value],'1'
    mov dx,ui_sound_preference_path
    mov ax,0x3D00
    int 0x21
    jc .done
    mov bx,ax
    mov dx,ui_sound_preference_value
    mov cx,1
    mov ah,0x3F
    int 0x21
    mov ah,0x3E
    int 0x21
.done:
    popad
    ret

ui_sound_preference_value db '1'
ui_sound_preference_path db '\SYSTEM\BOOT.SND',0
ut_startup_on db 'System sounds: on',0
ut_startup_off db 'System sounds: off',0
ui_sidebar_width dw 0
ui_mode_text times 48 db 0
ut_bit_color db '-bit color',0
ut_places db 'Places',0
ut_shortcuts db 'Keyboard',0
ut_keys_run db 'F3   Run',0
ut_keys_dos db 'F4   DOS',0
ut_open_hint db 'Double-click an item to open it.',0
ut_dos_app db 'DOS application',0
ui_item_types dw ut_desktop_app,ut_file_manager,ut_text_editor,ut_desktop_app,ut_commands,ut_commands
ut_file_manager db 'File manager',0
ut_text_editor db 'Text editor',0
ut_commands db 'Commands',0
ut_system_tool db 'System tool',0
ut_desktop_app db 'Desktop environment',0
ut_auto_help db 'Desktop resolution.',0
ut_automatic db "Use the monitor's preferred resolution",0
ut_mode640 db '640 x 480',0
ut_mode800 db '800 x 600',0
ut_mode1024 db '1024 x 768',0
ut_mode1280 db '1280 x 1024',0
ut_mode1600 db '1600 x 1200',0
ut_mode1920 db '1920 x 1080',0
ut_mode2560 db '2560 x 1440',0
ut_display_setup db 'Display setup...',0
ut_safe_video db 'Safe VGA',0
ut_dos_shared db 'Shared by desktop and DOS.',0
ut_sound_help db 'Startup sound',0
ut_sound_melody db 'Preview a sound or change the sound preference.',0
ut_sound_test db 'Play sound',0
ut_sound_on db 'Enable sounds',0
ut_sound_off db 'Disable sounds',0
ui_settings_commands dw uc_auto,uc_640,uc_800,uc_1024,uc_safe
ui_display_commands dw uc_1280,uc_1600,uc_1920,uc_2560,uc_display
uc_auto db 'VGASETUP AUTO',0
uc_640 db 'VGASETUP SET 640',0
uc_800 db 'VGASETUP DESKTOP 800',0
uc_1024 db 'VGASETUP DESKTOP 1024',0
uc_1280 db 'VGASETUP DESKTOP 1280',0
uc_1600 db 'VGASETUP DESKTOP 1600',0
uc_1920 db 'VGASETUP DESKTOP 1920',0
uc_2560 db 'VGASETUP DESKTOP 2560',0
uc_safe db 'VGASETUP SET SAFE',0
