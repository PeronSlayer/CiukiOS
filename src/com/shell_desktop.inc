; Small discoverable commands for the native shell. COMMAND.COM keeps its
; compact DOS compatibility interface and does not include the app dashboard.
desktop_dispatch:
    mov bx, desktop_commands
.next:
    mov di, [bx]
    test di, di
    jz .miss
    mov si, cmd_buf
    call strings_equal
    jz .run
    add bx, 4
    jmp .next
.run:
    call word [bx + 2]
    clc
    ret
.miss:
    stc
    ret

desktop_home:
    call desktop_clear
    mov byte [shell_output_color], 0x0B
    mov si, desktop_welcome
    call print_dual_dollar_string
    mov byte [shell_output_color], 7
    mov si, desktop_welcome_body
    call print_dual_dollar_string
    ret

desktop_clear:
    cmp byte [vc_active], 1
    jne .text
    call vc_clear
    call vc_title
    ret
.text:
    mov ax, 0x0040
    mov es, ax
    mov dh, [es:0x84]
    mov dl, 79
    mov ax, 0x0600
    mov bx, 0x0700
    xor cx, cx
    int 0x10
    mov dx, 0x0100
    mov ah, 2
    xor bx, bx
    int 0x10
    push cs
    pop es
    call redraw_title_bar
    ret

desktop_help:
    mov si, desktop_help_main
    mov bx, [echo_ptr]
    mov al, [bx]
    call upcase_al
    cmp al, 'F'
    je .files
    cmp al, 'N'
    je .network
    cmp al, 'S'
    je .system
    jmp .print
.files:
    mov si, desktop_help_files
    jmp .print
.network:
    mov si, desktop_help_network
    jmp .print
.system:
    mov si, desktop_help_system
.print:
    call print_dual_dollar_string
    ret

desktop_apps:
    mov byte [desktop_selection], 0
.draw:
    call desktop_clear
    mov byte [shell_output_color], 0x0B
    mov si, desktop_apps_title
    call print_dual_dollar_string
    mov bp, desktop_app_labels
    xor bx, bx
.row:
    mov byte [shell_output_color], 7
    cmp bl, [desktop_selection]
    jne .plain
    mov byte [shell_output_color], 0x0B
.plain:
    mov al, ' '
    call dual_putc
    call dual_putc
    mov al, '1'
    add al, bl
    call dual_putc
    mov al, ' '
    call dual_putc
    mov al, ' '
    cmp bl, [desktop_selection]
    jne .marker
    mov al, '>'
.marker:
    call dual_putc
    mov al, ' '
    call dual_putc
    mov si, [ds:bp]
    call print_dual_z_string
    mov si, msg_crlf
    call print_dual_dollar_string
    add bp, 2
    inc bx
    cmp bx, 9
    jb .row
    mov byte [shell_output_color], 7
    mov si, desktop_apps_footer
    call print_dual_dollar_string
.key:
    xor ah, ah
    int 0x16
    cmp al, 27
    je desktop_home
    cmp ah, 0x48
    je .up
    cmp ah, 0x50
    je .down
    cmp al, 13
    je .launch
    cmp al, '1'
    jb .key
    cmp al, '9'
    ja .key
    sub al, '1'
    mov [desktop_selection], al
    jmp .draw
.up:
    dec byte [desktop_selection]
    jns .draw
    mov byte [desktop_selection], 8
    jmp .draw
.down:
    inc byte [desktop_selection]
    cmp byte [desktop_selection], 9
    jb .draw
    mov byte [desktop_selection], 0
    jmp .draw
.launch:
    xor ax, ax
    mov al, [desktop_selection]
    jmp desktop_launch_app

desktop_doom: xor ax, ax
    inc ax
    jmp desktop_launch_app
desktop_doomvan: mov ax, 2
    jmp desktop_launch_app
desktop_wolf: mov ax, 3
    jmp desktop_launch_app
desktop_windows: xor ax, ax
    jmp desktop_launch_app
desktop_dosnav: mov ax, 5
    jmp desktop_launch_app

desktop_launch_app:
    shl ax, 1
    mov bp, ax
    mov si, [desktop_app_exes + bp]
    mov di, src_path
    call copy_z_to_di
    mov si, [echo_ptr]
    cmp bp, 16
    jne .tail_ready
    mov si, desktop_sound_test
    mov word [echo_len], 4
.tail_ready:
    mov [exec_tail_src], si
    call build_exec_tail
    call setup_exec_block
    mov si, [desktop_app_dirs + bp]
    call exec_try_in_dir
    pushf
    push ax
    call restore_shell_video_state
    pop ax
    popf
    jnc .done
    push ax
    mov al,[ui_command_running]
    mov [ui_exec_error],al
    pop ax
    call print_exec_error
.done:
    ret

desktop_history:
    mov si, desktop_history_title
    call print_dual_dollar_string
    xor bp, bp
    mov al, [history_next]
    sub al, [history_count]
    and al, HISTORY_MAX - 1
    mov dl, al
.next:
    xor ax, ax
    mov al, [history_count]
    cmp bp, ax
    jae .done
    mov ax, bp
    inc ax
    call desktop_number
    mov al, ' '
    call dual_putc
    mov al, dl
    call history_slot_to_di
    mov si, di
    call print_dual_z_string
    mov si, msg_crlf
    call print_dual_dollar_string
    inc dl
    and dl, HISTORY_MAX - 1
    inc bp
    jmp .next
.done:
    ret

desktop_info:
    mov si, desktop_info_title
    call print_dual_dollar_string
    call desktop_date
    call desktop_time
    call desktop_memory
    mov si, desktop_info_display
    call print_dual_dollar_string
    mov ax, [vc_cols]
    cmp byte [vc_active], 1
    je .cols
    mov ax, 80
.cols:
    call desktop_number
    mov si, desktop_info_end
    call print_dual_dollar_string
    ret

desktop_date:
    mov ah, 0x2A
    int 0x21
    push cx
    push dx
    mov si, desktop_date_label
    call print_dual_dollar_string
    pop dx
    xor ax, ax
    mov al, dl
    call desktop_two_digits
    mov al, '/'
    call dual_putc
    mov al, dh
    call desktop_two_digits
    mov al, '/'
    call dual_putc
    pop ax
    call desktop_number
    mov si, msg_crlf
    call print_dual_dollar_string
    ret

desktop_time:
    mov ah, 0x2C
    int 0x21
    push dx
    push cx
    mov si, desktop_time_label
    call print_dual_dollar_string
    pop cx
    mov al, ch
    call desktop_two_digits
    mov al, ':'
    call dual_putc
    mov al, cl
    call desktop_two_digits
    mov al, ':'
    call dual_putc
    pop dx
    mov al, dh
    call desktop_two_digits
    mov si, msg_crlf
    call print_dual_dollar_string
    ret

desktop_memory:
    mov si, desktop_memory_label
    call print_dual_dollar_string
    mov bx, 0xFFFF
    mov ah, 0x48
    int 0x21
    mov ax, bx
    shr ax, 6
    call desktop_number
    mov si, desktop_memory_end
    call print_dual_dollar_string
    ret

desktop_number:
    push ax
    push bx
    push cx
    push dx
    xor cx, cx
    mov bx, 10
.divide:
    xor dx, dx
    div bx
    push dx
    inc cx
    test ax, ax
    jnz .divide
.digit:
    pop ax
    add al, '0'
    call dual_putc
    loop .digit
    pop dx
    pop cx
    pop bx
    pop ax
    ret

desktop_two_digits:
    push ax
    aam
    push ax
    mov al, ah
    add al, '0'
    call dual_putc
    pop ax
    add al, '0'
    call dual_putc
    pop ax
    ret

desktop_commands:
    dw dc_desktop, ui_desktop_command
    dw dc_home, desktop_home, dc_help, desktop_help, dc_apps, desktop_apps
    dw dc_history, desktop_history, dc_info, desktop_info, dc_mem, desktop_memory
    dw dc_date, desktop_date, dc_time, desktop_time, dc_win, desktop_windows
    dw dc_doom, desktop_doom, dc_doomvan, desktop_doomvan, dc_wolf, desktop_wolf
    dw dc_dosnav, desktop_dosnav, 0, 0
dc_desktop db 'DESKTOP',0
dc_home db 'HOME', 0
dc_help db 'HELP', 0
dc_apps db 'APPS', 0
dc_history db 'HISTORY', 0
dc_info db 'SYSINFO', 0
dc_mem db 'MEM', 0
dc_date db 'DATE', 0
dc_time db 'TIME', 0
dc_win db 'WIN', 0
dc_doom db 'DOOM', 0
dc_doomvan db 'DOOMVAN', 0
dc_wolf db 'WOLF3D', 0
dc_dosnav db 'DOSNAV', 0
desktop_sound_test db 'TEST', 0
desktop_selection db 0
desktop_app_labels dw da_win, da_doom, da_van, da_wolf, da_costa, da_dn, da_edit, da_video, da_sound
desktop_app_exes dw de_win, de_doom, de_van, de_wolf, de_costa, de_dn, de_edit, de_video, de_sound
desktop_app_dirs dw dd_win, dd_doom, dd_van, dd_wolf, dd_costa, dd_dn, dd_apps, dd_drivers, dd_drivers
da_win db 'Windows 3.1        Windows desktop and applications', 0
da_doom db 'Doom               Original game', 0
da_van db 'Doom Vanille       Alternate DOS engine', 0
da_wolf db 'Wolfenstein 3D     DOS game', 0
da_costa db 'Costa              DOS desktop', 0
da_dn db 'DOS Navigator      Two-panel file manager', 0
da_edit db 'Editor             Text editor', 0
da_video db 'Display            Shell and Windows resolution', 0
da_sound db 'Sound              Play the startup sound', 0
de_win db 'WIN.COM', 0
de_doom db 'DOOM.EXE', 0
de_van db 'DOOMVAN.EXE', 0
de_wolf db 'WOLF3D.COM', 0
de_costa db 'COSTA.EXE', 0
de_dn db 'DN.COM', 0
de_edit db 'CIUKEDIT.COM', 0
de_video db 'VGASETUP.COM', 0
de_sound db 'SOUND.COM', 0
dd_win db '\WINDOWS', 0
dd_doom db '\APPS\DOOM', 0
dd_van db '\APPS\DOOMVAN', 0
dd_wolf db '\APPS\WOLF3D', 0
dd_costa db '\APPS\COSTA', 0
dd_dn db '\APPS\DOSNAV', 0
dd_apps db '\APPS', 0
dd_drivers db '\SYSTEM\DRIVERS', 0
desktop_welcome db 13,10,'  A modern Retro OS',13,10,13,10,'$'
desktop_welcome_body db '$'
desktop_apps_title db 13,10,'  APPLICATIONS',13,10,13,10,'$'
desktop_apps_footer db 13,10,'  Arrows or 1-9: select   Enter: open   Esc: shell',13,10,'$'
desktop_help_main db 13,10,'  QUICK HELP',13,10
    db '  APPS / F2       Games, desktops and tools',13,10
    db '  HELP FILES      Files, folders and text editor',13,10
    db '  HELP NETWORK    Network, ping and file transfers',13,10
    db '  HELP SYSTEM     Display, sound, information and power',13,10
    db '  HISTORY         Recent commands; Up/Down recalls them',13,10
    db '  Tab completes names. F3 recalls the last command. Esc clears.',13,10
    db '  Left/Right, Home/End and Delete edit the command line.',13,10
    db '  Examples: DIR  |  CD DOOM  |  TYPE README.TXT  |  WIN',13,10,13,10,'$'
desktop_help_files db 13,10,'  FILES AND FOLDERS',13,10
    db '  DIR [path]              List files and folders',13,10
    db '  CD path / CD..          Change folder / go up one level',13,10
    db '  PWD / WHERE name        Current folder / find a program',13,10
    db '  TYPE file / EDIT file   Read / edit text',13,10
    db '  COPY source dest        Copy a file',13,10
    db '  REN source dest         Rename a file',13,10
    db '  DEL file                Delete a file',13,10
    db '  MD name / RD name       Create / remove an empty folder',13,10
    db '  DOSNAV                  Two-panel file manager',13,10,13,10,'$'
desktop_help_network db 13,10,'  NETWORK',13,10
    db '  NETSTART                Start networking and ICMP service',13,10
    db '  IPCONFIG                Show addresses and status',13,10
    db '  NETCFG DHCP             Request an automatic address',13,10
    db '  NETCFG STATIC ip mask gateway dns',13,10
    db '  PING host               Test a connection',13,10
    db '  FTP host / FTPSRV       FTP client / server in C:\SHARE',13,10,13,10,'$'
desktop_help_system db 13,10,'  SYSTEM',13,10
    db '  SETUP                   Install CiukiOS or format a disk',13,10
    db '  VGASETUP                Shared shell and Windows resolution',13,10
    db '  SOUND ON / OFF / TEST   Startup sound and preview',13,10
    db '  SOUND /D                Show audio initialization steps',13,10
    db '  EXIT / DESKTOP         Return to the graphical desktop',13,10
    db '  SYSINFO / MEM           System information / free DOS memory',13,10
    db '  DATE / TIME             Current date and time',13,10
    db '  CLS / HOME              Clear screen / home',13,10
    db '  REBOOT 5 / SHUTDOWN 5    Restart / shut down after 5 seconds',13,10
    db '  REBOOT CANCEL           Cancel a scheduled restart',13,10,13,10,'$'
desktop_history_title db '  HISTORY',13,10,'$'
desktop_info_title db '  CIUKIOS / SYSTEM',13,10,'$'
desktop_date_label db '  Date: ', '$'
desktop_time_label db '  Time: ', '$'
desktop_memory_label db '  Largest free DOS block: ', '$'
desktop_memory_end db ' KiB',13,10,'$'
desktop_info_display db '  Console: ', '$'
desktop_info_end db ' columns. Use VGASETUP to change resolution.',13,10,'$'
