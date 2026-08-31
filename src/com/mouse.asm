bits 16
org 0x0100

start:
    push cs
    pop ds

    call parse_tail_command

    call probe_mouse_driver
    jc mouse_missing

    mov si, msg_ready
    call print_dollar_string

    cmp byte [cmd_word], 0
    je .status

    cmp byte [cmd_word], 'I'
    jne .check_status
    cmp byte [cmd_word + 1], 'N'
    jne .check_info
    cmp byte [cmd_word + 2], 'S'
    jne .check_info
    cmp byte [cmd_word + 3], 'T'
    jne .check_info
    cmp byte [cmd_word + 4], 'A'
    jne .check_info
    cmp byte [cmd_word + 5], 'L'
    jne .check_info
    cmp byte [cmd_word + 6], 'L'
    jne .check_info
    cmp byte [cmd_word + 7], 0
    jne .check_info
    mov si, msg_install
    call print_dollar_string
    jmp print_status

.check_info:
    cmp byte [cmd_word + 1], 'N'
    jne .check_status
    cmp byte [cmd_word + 2], 'F'
    jne .check_status
    cmp byte [cmd_word + 3], 'O'
    jne .check_status
    cmp byte [cmd_word + 4], 0
    jne .check_status
    jmp print_info

.check_status:
    cmp byte [cmd_word], 'S'
    jne .check_show
    cmp byte [cmd_word + 1], 'T'
    jne .check_show
    cmp byte [cmd_word + 2], 'A'
    jne .check_show
    cmp byte [cmd_word + 3], 'T'
    jne .check_show
    cmp byte [cmd_word + 4], 'U'
    jne .check_show
    cmp byte [cmd_word + 5], 'S'
    jne .check_show
    cmp byte [cmd_word + 6], 0
    jne .check_show

.status:
    mov si, msg_runtime
    call print_dollar_string
    jmp print_status

.check_show:
    cmp byte [cmd_word], 'S'
    jne .check_hide
    cmp byte [cmd_word + 1], 'H'
    jne .check_hide
    cmp byte [cmd_word + 2], 'O'
    jne .check_hide
    cmp byte [cmd_word + 3], 'W'
    jne .check_hide
    cmp byte [cmd_word + 4], 0
    jne .check_hide
    mov ax, 0x0001
    int 0x33
    mov si, msg_show
    call print_dollar_string
    jmp print_status

.check_hide:
    cmp byte [cmd_word], 'H'
    jne .check_pos
    cmp byte [cmd_word + 1], 'I'
    jne .check_help
    cmp byte [cmd_word + 2], 'D'
    jne .check_help
    cmp byte [cmd_word + 3], 'E'
    jne .check_help
    cmp byte [cmd_word + 4], 0
    jne .check_help
    mov ax, 0x0002
    int 0x33
    mov si, msg_hide
    call print_dollar_string
    jmp print_status

.check_help:
    cmp byte [cmd_word + 1], 'E'
    jne .check_pos
    cmp byte [cmd_word + 2], 'L'
    jne .check_pos
    cmp byte [cmd_word + 3], 'P'
    jne .check_pos
    cmp byte [cmd_word + 4], 0
    jne .check_pos
    jmp mouse_help

.check_pos:
    cmp byte [cmd_word], 'P'
    jne .check_range
    cmp byte [cmd_word + 1], 'O'
    jne .check_press
    cmp byte [cmd_word + 2], 'S'
    jne .check_press
    cmp byte [cmd_word + 3], 0
    jne .check_press
    mov si, [cmd_args_ptr]
    call parse_u16_arg
    jc mouse_usage
    mov cx, ax
    call parse_u16_arg
    jc mouse_usage
    mov dx, ax
    mov ax, 0x0004
    int 0x33
    mov si, msg_pos
    call print_dollar_string
    jmp print_status

.check_press:
    cmp byte [cmd_word + 1], 'R'
    jne .check_range
    cmp byte [cmd_word + 2], 'E'
    jne .check_range
    cmp byte [cmd_word + 3], 'S'
    jne .check_range
    cmp byte [cmd_word + 4], 'S'
    jne .check_range
    cmp byte [cmd_word + 5], 0
    jne .check_range
    mov si, [cmd_args_ptr]
    call parse_u16_arg
    jc mouse_usage
    mov bx, ax
    mov ax, 0x0005
    int 0x33
    mov si, msg_press
    call print_dollar_string
    jmp print_button_event

.check_range:
    cmp byte [cmd_word], 'R'
    jne .check_release
    cmp byte [cmd_word + 1], 'A'
    jne .check_rate
    cmp byte [cmd_word + 2], 'N'
    jne .check_rate
    cmp byte [cmd_word + 3], 'G'
    jne .check_rate
    cmp byte [cmd_word + 4], 'E'
    jne .check_rate
    cmp byte [cmd_word + 5], 0
    jne .check_rate
    mov si, [cmd_args_ptr]
    call parse_u16_arg
    jc mouse_usage
    mov cx, ax
    call parse_u16_arg
    jc mouse_usage
    mov dx, ax
    call parse_u16_arg
    jc mouse_usage
    mov [range_y_min], ax
    call parse_u16_arg
    jc mouse_usage
    mov [range_y_max], ax
    mov ax, 0x0007
    int 0x33
    mov cx, [range_y_min]
    mov dx, [range_y_max]
    mov ax, 0x0008
    int 0x33
    mov si, msg_range
    call print_dollar_string
    jmp print_status

.check_rate:
    cmp byte [cmd_word + 1], 'A'
    jne .check_reset
    cmp byte [cmd_word + 2], 'T'
    jne .check_reset
    cmp byte [cmd_word + 3], 'E'
    jne .check_reset
    cmp byte [cmd_word + 4], 0
    jne .check_reset
    mov si, [cmd_args_ptr]
    call parse_u16_arg
    jc mouse_usage
    mov bx, ax
    mov ax, 0x001C
    int 0x33
    mov si, msg_rate
    call print_dollar_string
    jmp mouse_ok_exit

.check_reset:
    cmp byte [cmd_word + 1], 'E'
    jne .check_release
    cmp byte [cmd_word + 2], 'S'
    jne .check_release
    cmp byte [cmd_word + 3], 'E'
    jne .check_release
    cmp byte [cmd_word + 4], 'T'
    jne .check_release
    cmp byte [cmd_word + 5], 0
    jne .check_release
    mov ax, 0x0021
    int 0x33
    mov si, msg_reset
    call print_dollar_string
    jmp print_status

.check_release:
    cmp byte [cmd_word], 'R'
    jne .check_sens
    cmp byte [cmd_word + 1], 'E'
    jne .check_sens
    cmp byte [cmd_word + 2], 'L'
    jne .check_sens
    cmp byte [cmd_word + 3], 'E'
    jne .check_sens
    cmp byte [cmd_word + 4], 'A'
    jne .check_sens
    cmp byte [cmd_word + 5], 'S'
    jne .check_sens
    cmp byte [cmd_word + 6], 'E'
    jne .check_sens
    cmp byte [cmd_word + 7], 0
    jne .check_sens
    mov si, [cmd_args_ptr]
    call parse_u16_arg
    jc mouse_usage
    mov bx, ax
    mov ax, 0x0006
    int 0x33
    mov si, msg_release
    call print_dollar_string
    jmp print_button_event

.check_sens:
    cmp byte [cmd_word], 'S'
    jne .check_getsens
    cmp byte [cmd_word + 1], 'E'
    jne .check_getsens
    cmp byte [cmd_word + 2], 'N'
    jne .check_getsens
    cmp byte [cmd_word + 3], 'S'
    jne .check_getsens
    cmp byte [cmd_word + 4], 0
    jne .check_getsens
    mov si, [cmd_args_ptr]
    call parse_u16_arg
    jc mouse_usage
    mov bx, ax
    call parse_u16_arg
    jc mouse_usage
    mov cx, ax
    call parse_u16_arg
    jc mouse_usage
    mov dx, ax
    mov ax, 0x001A
    int 0x33
    mov si, msg_sens
    call print_dollar_string
    jmp print_sensitivity

.check_getsens:
    cmp byte [cmd_word], 'G'
    jne .check_motion
    cmp byte [cmd_word + 1], 'E'
    jne .check_motion
    cmp byte [cmd_word + 2], 'T'
    jne .check_motion
    cmp byte [cmd_word + 3], 'S'
    jne .check_getpage
    cmp byte [cmd_word + 4], 'E'
    jne .check_getpage
    cmp byte [cmd_word + 5], 'N'
    jne .check_getpage
    cmp byte [cmd_word + 6], 'S'
    jne .check_getpage
    cmp byte [cmd_word + 7], 0
    jne .check_getpage
    mov ax, 0x001B
    int 0x33
    mov si, msg_sens
    call print_dollar_string
    jmp print_sensitivity

.check_getpage:
    cmp byte [cmd_word + 3], 'P'
    jne .check_motion
    cmp byte [cmd_word + 4], 'A'
    jne .check_motion
    cmp byte [cmd_word + 5], 'G'
    jne .check_motion
    cmp byte [cmd_word + 6], 'E'
    jne .check_motion
    cmp byte [cmd_word + 7], 0
    jne .check_motion
    mov ax, 0x001E
    int 0x33
    mov si, msg_page
    call print_dollar_string
    mov ax, bx
    call print_hex16
    mov si, msg_crlf
    call print_dollar_string
    jmp mouse_ok_exit

.check_motion:
    cmp byte [cmd_word], 'M'
    jne .check_page
    cmp byte [cmd_word + 1], 'O'
    jne .check_page
    cmp byte [cmd_word + 2], 'T'
    jne .check_page
    cmp byte [cmd_word + 3], 'I'
    jne .check_page
    cmp byte [cmd_word + 4], 'O'
    jne .check_page
    cmp byte [cmd_word + 5], 'N'
    jne .check_page
    cmp byte [cmd_word + 6], 0
    jne .check_page
    mov ax, 0x000B
    int 0x33
    mov si, msg_motion
    call print_dollar_string
    mov ax, cx
    call print_hex16
    mov si, msg_y
    call print_dollar_string
    mov ax, dx
    call print_hex16
    mov si, msg_crlf
    call print_dollar_string
    jmp mouse_ok_exit

.check_page:
    cmp byte [cmd_word], 'P'
    jne .check_enable
    cmp byte [cmd_word + 1], 'A'
    jne .check_enable
    cmp byte [cmd_word + 2], 'G'
    jne .check_enable
    cmp byte [cmd_word + 3], 'E'
    jne .check_enable
    cmp byte [cmd_word + 4], 0
    jne .check_enable
    mov si, [cmd_args_ptr]
    call parse_u16_arg
    jc mouse_usage
    mov bx, ax
    mov ax, 0x001D
    int 0x33
    mov si, msg_page
    call print_dollar_string
    mov ax, bx
    call print_hex16
    mov si, msg_crlf
    call print_dollar_string
    jmp mouse_ok_exit

.check_enable:
    cmp byte [cmd_word], 'E'
    jne .check_disable
    cmp byte [cmd_word + 1], 'N'
    jne .check_disable
    cmp byte [cmd_word + 2], 'A'
    jne .check_disable
    cmp byte [cmd_word + 3], 'B'
    jne .check_disable
    cmp byte [cmd_word + 4], 'L'
    jne .check_disable
    cmp byte [cmd_word + 5], 'E'
    jne .check_disable
    cmp byte [cmd_word + 6], 0
    jne .check_disable
    mov ax, 0x0020
    int 0x33
    mov si, msg_enable
    call print_dollar_string
    jmp print_status

.check_disable:
    cmp byte [cmd_word], 'D'
    jne mouse_usage
    cmp byte [cmd_word + 1], 'I'
    jne mouse_usage
    cmp byte [cmd_word + 2], 'S'
    jne mouse_usage
    cmp byte [cmd_word + 3], 'A'
    jne mouse_usage
    cmp byte [cmd_word + 4], 'B'
    jne mouse_usage
    cmp byte [cmd_word + 5], 'L'
    jne mouse_usage
    cmp byte [cmd_word + 6], 'E'
    jne mouse_usage
    cmp byte [cmd_word + 7], 0
    jne mouse_usage
    mov ax, 0x001F
    int 0x33
    mov si, msg_disable
    call print_dollar_string
    jmp mouse_ok_exit

mouse_usage:
    mov si, msg_usage
    call print_dollar_string
    mov ax, 0x4C01
    int 0x21

mouse_help:
    mov si, msg_help
    call print_dollar_string
    jmp mouse_ok_exit

print_status:
    mov ax, 0x0003
    int 0x33
    mov si, msg_buttons
    call print_dollar_string
    mov ax, bx
    call print_hex16
    mov si, msg_x
    call print_dollar_string
    mov ax, cx
    call print_hex16
    mov si, msg_y
    call print_dollar_string
    mov ax, dx
    call print_hex16
    mov si, msg_crlf
    call print_dollar_string
mouse_ok_exit:
    mov ax, 0x4C00
    int 0x21

print_button_event:
    push ax
    mov si, msg_count
    call print_dollar_string
    mov ax, bx
    call print_hex16
    mov si, msg_x
    call print_dollar_string
    mov ax, cx
    call print_hex16
    mov si, msg_y
    call print_dollar_string
    mov ax, dx
    call print_hex16
    mov si, msg_buttons_tail
    call print_dollar_string
    pop ax
    call print_hex16
    mov si, msg_crlf
    call print_dollar_string
    jmp mouse_ok_exit

print_sensitivity:
    mov si, msg_hx
    call print_dollar_string
    mov ax, bx
    call print_hex16
    mov si, msg_vy
    call print_dollar_string
    mov ax, cx
    call print_hex16
    mov si, msg_th
    call print_dollar_string
    mov ax, dx
    call print_hex16
    mov si, msg_crlf
    call print_dollar_string
    jmp mouse_ok_exit

print_info:
    mov ax, 0x0024
    int 0x33
    mov [info_ax], ax
    mov [info_bx], bx
    mov [info_cx], cx
    mov [info_dx], dx
    mov si, msg_info_ver
    call print_dollar_string
    mov ax, [info_bx]
    call print_hex16
    mov si, msg_info_type
    call print_dollar_string
    mov al, [info_cx + 1]
    xor ah, ah
    call print_hex16
    mov si, msg_info_irq
    call print_dollar_string
    mov al, [info_cx]
    xor ah, ah
    call print_hex16
    mov si, msg_info_status
    call print_dollar_string
    mov ax, [info_ax]
    call print_hex16
    mov si, msg_crlf
    call print_dollar_string
    jmp mouse_ok_exit

mouse_missing:
    mov si, msg_missing
    call print_dollar_string
    mov ax, 0x4C01
    int 0x21

probe_mouse_driver:
    mov ax, 0x0000
    int 0x33
    or ax, ax
    jz .missing
    mov [buttons], bx
    clc
    ret

.missing:
    stc
    ret

parse_tail_command:
    mov si, 0x0081
    mov cl, [0x0080]
    xor ch, ch

.skip_spaces:
    jcxz .empty
    cmp byte [si], ' '
    jne .copy
    inc si
    dec cx
    jmp .skip_spaces

.copy:
    mov di, cmd_word
.copy_loop:
    jcxz .done
    mov al, [si]
    cmp al, ' '
    je .done
    cmp di, cmd_word + 7
    jae .advance
    call upcase_al
    mov [di], al
    inc di
.advance:
    inc si
    dec cx
    jmp .copy_loop

.done:
    mov byte [di], 0
    mov [cmd_args_ptr], si
    ret

.empty:
    mov byte [cmd_word], 0
    mov word [cmd_args_ptr], 0x0081
    ret

parse_u16_arg:
    call skip_spaces
    xor bx, bx
    xor di, di

.digit_loop:
    mov al, [si]
    cmp al, 13
    je .done
    cmp al, 0
    je .done
    cmp al, '0'
    jb .done
    cmp al, '9'
    ja .done
    sub al, '0'
    inc di
    mov dx, bx
    shl bx, 1
    shl dx, 3
    add bx, dx
    xor ah, ah
    add bx, ax
    inc si
    jmp .digit_loop

.done:
    cmp di, 0
    je .fail
    mov ax, bx
    clc
    ret

.fail:
    stc
    ret

skip_spaces:
.loop:
    mov al, [si]
    cmp al, 13
    je .done
    cmp al, 0
    je .done
    cmp al, ' '
    jne .done
    inc si
    jmp .loop
.done:
    ret

upcase_al:
    cmp al, 'a'
    jb .done
    cmp al, 'z'
    ja .done
    sub al, 32
.done:
    ret

print_dollar_string:
.next:
    lodsb
    cmp al, '$'
    je .done
    mov dl, al
    mov ah, 0x02
    int 0x21
    jmp .next
.done:
    ret

print_hex16:
    push ax
    mov al, ah
    call print_hex8
    pop ax
    call print_hex8
    ret

print_hex8:
    push ax
    shr al, 4
    call print_hex_nibble
    pop ax
    and al, 0x0F
    call print_hex_nibble
    ret

print_hex_nibble:
    and al, 0x0F
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp .emit
.digit:
    add al, '0'
.emit:
    mov dl, al
    mov ah, 0x02
    int 0x21
    ret

msg_ready db 'mouse: int33h ready', 13, 10, '$'
msg_missing db 'mouse: int33h not installed', 13, 10, '$'
msg_runtime db 'mouse: runtime backed service active', 13, 10, '$'
msg_install db 'mouse: runtime backed service already installed', 13, 10, '$'
msg_show db 'mouse: show cursor', 13, 10, '$'
msg_hide db 'mouse: hide cursor', 13, 10, '$'
msg_pos db 'mouse: set position', 13, 10, '$'
msg_range db 'mouse: set range', 13, 10, '$'
msg_sens db 'mouse: sensitivity', 13, 10, '$'
msg_motion db 'motion dx=0x', '$'
msg_press db 'press', '$'
msg_release db 'release', '$'
msg_reset db 'mouse: reset', 13, 10, '$'
msg_rate db 'mouse: rate set', 13, 10, '$'
msg_page db 'page=0x', '$'
msg_enable db 'mouse: driver enabled', 13, 10, '$'
msg_disable db 'mouse: driver disabled', 13, 10, '$'
msg_count db ' count=0x', '$'
msg_buttons_tail db ' buttons=0x', '$'
msg_hx db ' hx=0x', '$'
msg_vy db ' vy=0x', '$'
msg_th db ' th=0x', '$'
msg_info_ver db 'info ver=0x', '$'
msg_info_type db ' type=0x', '$'
msg_info_irq db ' irq=0x', '$'
msg_info_status db ' status=0x', '$'
msg_usage db 'usage: mouse [status|show|hide|pos|range|sens|getsens|motion|press|release|reset|info|page|getpage|rate|enable|disable|install|help]', 13, 10, '$'
msg_help db 'mouse cmds: status show hide pos range sens getsens motion press release reset info page getpage rate enable disable install', 13, 10, '$'
msg_buttons db 'buttons=0x', '$'
msg_x db ' x=0x', '$'
msg_y db ' y=0x', '$'
msg_crlf db 13, 10, '$'

buttons dw 0
info_ax dw 0
info_bx dw 0
info_cx dw 0
info_dx dw 0
range_y_min dw 0
range_y_max dw 0
cmd_args_ptr dw 0
cmd_word times 8 db 0
