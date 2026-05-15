bits 16
org 0x0100

start:
    cld
    push cs
    pop ds
    push cs
    pop es
    call parse_quiet_switch

    cmp byte [quiet_mode], 0
    je .tail_ready
    mov word [sb16init_param_block + 2], sb16init_quiet_tail

.tail_ready:

    mov dx, msg_begin
    call print_line

    mov dx, src_cfg_path
    mov bx, dst_cfg_path
    call copy_file
    jc .copy_fail

    mov dx, src_cfg_path
    mov bx, doomdata_cfg_path
    call copy_file
    jc .copy_fail

    mov dx, msg_cfg_ready
    call print_line

    mov dx, sb16init_path
    mov bx, sb16init_param_block
    call exec_child
    jc .child_fail

    mov dx, msg_drvload_done
    call print_line

    mov dx, doom_path
    mov bx, doom_param_block
    call exec_child
    jc .child_fail

    mov dx, msg_done
    call print_line
    mov ax, 0x4C00
    int 0x21

.copy_fail:
    mov dx, msg_copy_fail
    call print_line
    mov ax, 0x4C01
    int 0x21

.child_fail:
    mov dx, msg_exec_fail
    call print_line
    mov ax, 0x4C02
    int 0x21

parse_quiet_switch:
    push ax
    push cx
    push si

    mov si, 0x0081
    mov cl, [0x0080]
    xor ch, ch

.next_char:
    jcxz .done
    lodsb
    dec cx
    cmp al, ' '
    je .next_char
    cmp al, 9
    je .next_char
    cmp al, '/'
    jne .skip_token

    jcxz .done
    lodsb
    dec cx
    and al, 0xDF
    cmp al, 'Q'
    jne .skip_token

    mov byte [quiet_mode], 1
    jmp .done

.skip_token:
    jcxz .done
    lodsb
    dec cx
    cmp al, ' '
    je .next_char
    cmp al, 9
    jne .skip_token
    jmp .next_char

.done:
    pop si
    pop cx
    pop ax
    ret

print_line:
    cmp byte [quiet_mode], 0
    jne .done
    mov ah, 0x09
    int 0x21
.done:
    ret

copy_file:
    ; IN: DS:DX source ASCIIZ, DS:BX dest ASCIIZ
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    mov si, dx
    mov di, bx

    mov dx, si
    mov ax, 0x3D00
    int 0x21
    jc .fail
    mov [src_handle], ax

    mov dx, di
    xor cx, cx
    mov ax, 0x3C00
    int 0x21
    jc .close_src_fail
    mov [dst_handle], ax

.read_loop:
    mov bx, [src_handle]
    mov dx, copy_buf
    mov cx, copy_buf_len
    mov ah, 0x3F
    int 0x21
    jc .close_both_fail
    or ax, ax
    jz .close_ok

    mov cx, ax
    mov bx, [dst_handle]
    mov dx, copy_buf
    mov ah, 0x40
    int 0x21
    jc .close_both_fail
    jmp .read_loop

.close_ok:
    mov bx, [dst_handle]
    mov ah, 0x3E
    int 0x21
    mov bx, [src_handle]
    mov ah, 0x3E
    int 0x21
    clc
    jmp .done

.close_both_fail:
    mov bx, [dst_handle]
    mov ah, 0x3E
    int 0x21
.close_src_fail:
    mov bx, [src_handle]
    mov ah, 0x3E
    int 0x21
    stc
    jmp .done

.fail:
    stc

.done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

exec_child:
    ; IN: DS:DX path ASCIIZ, DS:BX param block
    push ax
    push bx
    push dx
    push ds
    push es
    xor ax, ax
    mov [bx + 0], ax
    mov ax, ds
    mov [bx + 4], ax
    mov [bx + 8], ax
    mov [bx + 12], ax
    push ds
    pop es
    mov ax, 0x4B00
    int 0x21
    jc .done
    mov ax, 0x4D00
    int 0x21
    cmp ah, 0
    jne .child_fail
    cmp al, 0
    jne .child_fail
    clc
    jmp .done

.child_fail:
    stc

.done:
    pop es
    pop ds
    pop dx
    pop bx
    pop ax
    ret

msg_begin db '[DOOMSB] BEGIN', 13, 10, '$'
msg_cfg_ready db '[DOOMSB] CFG READY', 13, 10, '$'
msg_drvload_done db '[DOOMSB] AUDIO READY', 13, 10, '$'
msg_done db '[DOOMSB] DONE', 13, 10, '$'
msg_copy_fail db '[DOOMSB] COPY FAIL', 13, 10, '$'
msg_exec_fail db '[DOOMSB] EXEC FAIL', 13, 10, '$'

src_cfg_path db '\APPS\DOOM\DOOMSB.CFG', 0
dst_cfg_path db '\APPS\DOOM\DEFAULT.CFG', 0
doomdata_cfg_path db '\DOOMDATA\DEFAULT.CFG', 0
sb16init_path db '\SYSTEM\DRIVERS\SB16INIT.COM', 0
doom_path db '\APPS\DOOM\DOOM.EXE', 0

sb16init_tail db 0, 13
sb16init_quiet_tail db 3, ' /Q', 13
sb16init_fcb1 dw 0, 0
sb16init_fcb2 dw 0, 0
sb16init_param_block:
    dw 0
    dw sb16init_tail
    dw 0
    dw sb16init_fcb1
    dw 0
    dw sb16init_fcb2
    dw 0

doom_tail db 0, 13
doom_fcb1 dw 0, 0
doom_fcb2 dw 0, 0
doom_param_block:
    dw 0
    dw doom_tail
    dw 0
    dw doom_fcb1
    dw 0
    dw doom_fcb2
    dw 0

src_handle dw 0
dst_handle dw 0
quiet_mode db 0
copy_buf_len equ 512
copy_buf times copy_buf_len db 0
