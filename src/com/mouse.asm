bits 16
org 0x0100

start:
    push cs
    pop ds

    call parse_tail_command

    mov ax, 0x0000
    int 0x33
    or ax, ax
    jz mouse_missing
    mov [buttons], bx

    mov si, msg_ready
    call print_dollar_string

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
    jne print_status
    cmp byte [cmd_word + 1], 'I'
    jne print_status
    cmp byte [cmd_word + 2], 'D'
    jne print_status
    cmp byte [cmd_word + 3], 'E'
    jne print_status
    cmp byte [cmd_word + 4], 0
    jne print_status
    mov ax, 0x0002
    int 0x33
    mov si, msg_hide
    call print_dollar_string

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
    mov ax, 0x4C00
    int 0x21

mouse_missing:
    mov si, msg_missing
    call print_dollar_string
    mov ax, 0x4C01
    int 0x21

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
    ret

.empty:
    mov byte [cmd_word], 0
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
msg_show db 'mouse: show cursor', 13, 10, '$'
msg_hide db 'mouse: hide cursor', 13, 10, '$'
msg_buttons db 'buttons=0x', '$'
msg_x db ' x=0x', '$'
msg_y db ' y=0x', '$'
msg_crlf db 13, 10, '$'

buttons dw 0
cmd_word times 8 db 0