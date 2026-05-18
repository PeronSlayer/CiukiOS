bits 16
org 0x0100

start:
    push cs
    pop ds

    mov si, msg_banner
    call print_dual_dollar_string

main_loop:
    call print_prompt

    call read_line
    call skip_spaces
    jcxz main_loop

    mov di, cmd_buf
.copy_cmd:
    jcxz .cmd_done
    mov al, [si]
    cmp al, ' '
    je .cmd_done
    call upcase_al
    cmp di, cmd_buf + 7
    jae .skip_store
    stosb
.skip_store:
    inc si
    dec cx
    jmp .copy_cmd

.cmd_done:
    mov byte [di], 0
    call skip_spaces
    mov [echo_ptr], si
    mov [echo_len], cx

    cmp byte [cmd_buf + 0], 'H'
    jne .check_ver
    cmp byte [cmd_buf + 1], 'E'
    jne .unknown
    cmp byte [cmd_buf + 2], 'L'
    jne .unknown
    cmp byte [cmd_buf + 3], 'P'
    jne .unknown
    cmp byte [cmd_buf + 4], 0
    jne .unknown
    mov si, msg_help
    call print_dual_dollar_string
    jmp main_loop

.check_ver:
    cmp byte [cmd_buf + 0], 'V'
    jne .check_echo
    cmp byte [cmd_buf + 1], 'E'
    jne .unknown
    cmp byte [cmd_buf + 2], 'R'
    jne .unknown
    cmp byte [cmd_buf + 3], 0
    jne .unknown
    mov si, msg_ver
    call print_dual_dollar_string
    jmp main_loop

.check_echo:
    cmp byte [cmd_buf + 0], 'E'
    jne .check_cls
    cmp byte [cmd_buf + 1], 'C'
    jne .check_exit
    cmp byte [cmd_buf + 2], 'H'
    jne .check_exit
    cmp byte [cmd_buf + 3], 'O'
    jne .check_exit
    cmp byte [cmd_buf + 4], 0
    jne .check_exit
    mov cx, [echo_len]
    jcxz .echo_done
    mov si, [echo_ptr]
    call print_dual_cx_string
.echo_done:
    mov si, msg_crlf
    call print_dual_dollar_string
    jmp main_loop

.check_cls:
    cmp byte [cmd_buf + 0], 'C'
    jne .check_exit
    cmp byte [cmd_buf + 1], 'L'
    jne .check_cd
    cmp byte [cmd_buf + 2], 'S'
    jne .check_cd
    cmp byte [cmd_buf + 3], 0
    jne .check_cd
    mov ax, 0x0600
    mov bh, 0x07
    xor cx, cx
    mov dx, 0x184F
    int 0x10
    mov ax, 0x0200
    xor bx, bx
    xor dx, dx
    int 0x10
    jmp main_loop

.check_exit:
    cmp byte [cmd_buf + 0], 'E'
    jne .check_cd
    cmp byte [cmd_buf + 1], 'X'
    jne .check_cd
    cmp byte [cmd_buf + 2], 'I'
    jne .unknown
    cmp byte [cmd_buf + 3], 'T'
    jne .unknown
    cmp byte [cmd_buf + 4], 0
    jne .unknown
    mov ax, 0x4C00
    int 0x21

; CD / CHDIR: change directory via INT 21h AH=3Bh, then echo the new path.
.check_cd:
    cmp byte [cmd_buf + 0], 'C'
    jne .check_dir
    cmp byte [cmd_buf + 1], 'D'
    jne .check_chdir
    cmp byte [cmd_buf + 2], 0
    jne .check_dir
    jmp .do_cd
.check_chdir:
    cmp byte [cmd_buf + 1], 'H'
    jne .check_dir
    cmp byte [cmd_buf + 2], 'D'
    jne .check_dir
    cmp byte [cmd_buf + 3], 'I'
    jne .check_dir
    cmp byte [cmd_buf + 4], 'R'
    jne .check_dir
    cmp byte [cmd_buf + 5], 0
    jne .check_dir
.do_cd:
    mov cx, [echo_len]
    jcxz .cd_show
    mov dx, [echo_ptr]
    mov ah, 0x3B
    int 0x21
    jc .cd_error
.cd_show:
    mov si, msg_cwd_pre
    call print_dual_dollar_string
    call print_cwd_path
    mov si, msg_crlf
    call print_dual_dollar_string
    jmp main_loop
.cd_error:
    mov si, msg_cd_err
    call print_dual_dollar_string
    jmp main_loop

; DIR: list a directory via INT 21h FindFirst/FindNext (AH=4Eh/4Fh).
.check_dir:
    cmp byte [cmd_buf + 0], 'D'
    jne .check_type
    cmp byte [cmd_buf + 1], 'I'
    jne .check_del
    cmp byte [cmd_buf + 2], 'R'
    jne .check_del
    cmp byte [cmd_buf + 3], 0
    jne .check_del
    mov dx, dta_buf
    mov ah, 0x1A
    int 0x21
    call build_dir_pattern
    mov si, msg_dir_hdr
    call print_dual_dollar_string
    mov dx, dir_pattern
    mov cx, 0x0010
    mov ah, 0x4E
    int 0x21
    jc .dir_none
.dir_loop:
    call dir_print_entry
    mov ah, 0x4F
    int 0x21
    jnc .dir_loop
    jmp main_loop
.dir_none:
    mov si, msg_dir_none
    call print_dual_dollar_string
    jmp main_loop

; DEL / ERASE: delete a file via INT 21h AH=41h.
.check_del:
    cmp byte [cmd_buf + 0], 'D'
    jne .check_type
    cmp byte [cmd_buf + 1], 'E'
    jne .check_type
    cmp byte [cmd_buf + 2], 'L'
    jne .check_type
    cmp byte [cmd_buf + 3], 0
    jne .check_type
    jmp .do_del

; TYPE: print a file via INT 21h open/read/close (AH=3Dh/3Fh/3Eh).
.check_type:
    cmp byte [cmd_buf + 0], 'T'
    jne .check_erase
    cmp byte [cmd_buf + 1], 'Y'
    jne .check_erase
    cmp byte [cmd_buf + 2], 'P'
    jne .check_erase
    cmp byte [cmd_buf + 3], 'E'
    jne .check_erase
    cmp byte [cmd_buf + 4], 0
    jne .check_erase
    jmp .do_type

.check_erase:
    cmp byte [cmd_buf + 0], 'E'
    jne .check_copy
    cmp byte [cmd_buf + 1], 'R'
    jne .unknown
    cmp byte [cmd_buf + 2], 'A'
    jne .unknown
    cmp byte [cmd_buf + 3], 'S'
    jne .unknown
    cmp byte [cmd_buf + 4], 'E'
    jne .unknown
    cmp byte [cmd_buf + 5], 0
    jne .unknown
.do_del:
    mov cx, [echo_len]
    jcxz .del_usage
    mov dx, [echo_ptr]
    mov ah, 0x41
    int 0x21
    jc .del_error
    mov si, msg_del_ok
    call print_dual_dollar_string
    jmp main_loop
.del_error:
    mov si, msg_del_err
    call print_dual_dollar_string
    jmp main_loop
.del_usage:
    mov si, msg_del_use
    call print_dual_dollar_string
    jmp main_loop

.do_type:
    mov cx, [echo_len]
    jcxz .type_usage
    mov dx, [echo_ptr]
    mov ax, 0x3D00
    int 0x21
    jc .type_error
    mov [file_handle], ax
.type_read:
    mov bx, [file_handle]
    mov cx, 512
    mov dx, file_buf
    mov ah, 0x3F
    int 0x21
    jc .type_read_err
    test ax, ax
    jz .type_done
    mov cx, ax
    mov si, file_buf
    call print_dual_cx_string
    jmp .type_read
.type_done:
    mov bx, [file_handle]
    mov ah, 0x3E
    int 0x21
    mov si, msg_crlf
    call print_dual_dollar_string
    jmp main_loop
.type_read_err:
    mov bx, [file_handle]
    mov ah, 0x3E
    int 0x21
.type_error:
    mov si, msg_type_err
    call print_dual_dollar_string
    jmp main_loop
.type_usage:
    mov si, msg_type_use
    call print_dual_dollar_string
    jmp main_loop

; COPY: copy a file via INT 21h open/create/read/write/close.
; Error/usage handlers sit up front so the forward conditional jumps in the
; copy flow stay within short-jump range; a trampoline cluster bridges the
; rest.
.check_copy:
    cmp byte [cmd_buf + 0], 'C'
    jne .unknown
    cmp byte [cmd_buf + 1], 'O'
    jne .unknown
    cmp byte [cmd_buf + 2], 'P'
    jne .unknown
    cmp byte [cmd_buf + 3], 'Y'
    jne .unknown
    cmp byte [cmd_buf + 4], 0
    jne .unknown
.do_copy:
    mov cx, [echo_len]
    test cx, cx
    jnz .copy_args
.copy_usage:
    mov si, msg_copy_use
    call print_dual_dollar_string
    jmp main_loop
.copy_src_err:
    mov si, msg_copy_src_err
    call print_dual_dollar_string
    jmp main_loop
.copy_io_err:
    mov bx, [copy_src_handle]
    mov ah, 0x3E
    int 0x21
    mov bx, [copy_dst_handle]
    mov ah, 0x3E
    int 0x21
    jmp .copy_failmsg
.copy_dst_err:
    mov bx, [copy_src_handle]
    mov ah, 0x3E
    int 0x21
.copy_failmsg:
    mov si, msg_copy_err
    call print_dual_dollar_string
    jmp main_loop
.copy_args:
    mov si, [echo_ptr]
    mov di, src_path
.copy_src_parse:
    mov al, [si]
    cmp al, 0
    je .copy_src_end
    cmp al, ' '
    je .copy_src_end
    cmp di, src_path + 63
    jae .copy_src_adv
    mov [di], al
    inc di
.copy_src_adv:
    inc si
    jmp .copy_src_parse
.copy_src_end:
    mov byte [di], 0
    cmp di, src_path
    je .copy_t_usage
.copy_skip_sp:
    cmp byte [si], ' '
    jne .copy_dst_start
    inc si
    jmp .copy_skip_sp
.copy_dst_start:
    mov di, dst_path
.copy_dst_parse:
    mov al, [si]
    cmp al, 0
    je .copy_dst_end
    cmp al, ' '
    je .copy_dst_end
    cmp di, dst_path + 63
    jae .copy_dst_adv
    mov [di], al
    inc di
.copy_dst_adv:
    inc si
    jmp .copy_dst_parse
.copy_dst_end:
    mov byte [di], 0
    cmp di, dst_path
    je .copy_t_usage
    jmp .copy_open
.copy_t_usage:
    jmp .copy_usage
.copy_t_srcerr:
    jmp .copy_src_err
.copy_t_dsterr:
    jmp .copy_dst_err
.copy_t_ioerr:
    jmp .copy_io_err
.copy_open:
    mov dx, src_path
    mov ax, 0x3D00
    int 0x21
    jc .copy_t_srcerr
    mov [copy_src_handle], ax
    mov dx, dst_path
    xor cx, cx
    mov ah, 0x3C
    int 0x21
    jc .copy_t_dsterr
    mov [copy_dst_handle], ax
.copy_loop:
    mov bx, [copy_src_handle]
    mov cx, 512
    mov dx, file_buf
    mov ah, 0x3F
    int 0x21
    jc .copy_t_ioerr
    test ax, ax
    jz .copy_finish
    mov cx, ax
    mov bx, [copy_dst_handle]
    mov dx, file_buf
    mov ah, 0x40
    int 0x21
    jc .copy_t_ioerr
    cmp ax, cx
    jne .copy_t_ioerr
    jmp .copy_loop
.copy_finish:
    mov bx, [copy_src_handle]
    mov ah, 0x3E
    int 0x21
    mov bx, [copy_dst_handle]
    mov ah, 0x3E
    int 0x21
    mov si, msg_copy_ok
    call print_dual_dollar_string
    jmp main_loop

.unknown:
    mov si, msg_unknown
    call print_dual_dollar_string
    jmp main_loop

skip_spaces:
    jcxz .done
.loop:
    cmp byte [si], ' '
    jne .done
    inc si
    dec cx
    jnz .loop
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

read_line:
    xor bx, bx

.read:
    mov ah, 0x01
    int 0x21
    cmp al, 0x0D
    je .done
    cmp al, 0x08
    je .backspace
    cmp al, 0x20
    jb .read
    cmp bx, 126
    jae .read
    mov [input_buf + bx], al
    inc bx
    jmp .read

.backspace:
    cmp bx, 0
    je .read
    dec bx
    jmp .read

.done:
    mov byte [input_buf + bx], 0
    mov cx, bx
    mov si, input_buf
    mov si, msg_crlf
    call print_dual_dollar_string
    mov si, input_buf
    ret

; Build the SHELL.COM prompt: "SHELL <drive>:\<cwd>> ".
print_prompt:
    mov si, msg_prompt_pre
    call print_dual_dollar_string
    call print_cwd_path
    mov al, '>'
    call dual_putc
    mov al, ' '
    call dual_putc
    ret

; Print "<drive>:\<cwd>" using INT 21h AH=19h (drive) and AH=47h (cwd).
print_cwd_path:
    push ax
    push bx
    push cx
    push dx
    push si
    mov ah, 0x19
    int 0x21
    add al, 'A'
    call dual_putc
    mov al, ':'
    call dual_putc
    mov al, '\'
    call dual_putc
    xor dl, dl
    mov si, path_buf
    mov ah, 0x47
    int 0x21
    mov si, path_buf
    call print_dual_z_string
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Build dir_pattern (ASCIIZ) from the command argument.
; No argument -> "*.*"; otherwise "<arg>\*.*" with any trailing slash trimmed.
build_dir_pattern:
    push ax
    push cx
    push si
    push di
    mov di, dir_pattern
    mov cx, [echo_len]
    jcxz .use_star
    mov si, [echo_ptr]
.copy:
    mov al, [si]
    cmp al, 0
    je .copy_done
    cmp al, ' '
    je .copy_done
    cmp di, dir_pattern + 24
    jae .copy_done
    mov [di], al
    inc di
    inc si
    dec cx
    jnz .copy
.copy_done:
    cmp di, dir_pattern
    je .use_star
    mov al, [di - 1]
    cmp al, '\'
    je .strip
    cmp al, '/'
    je .strip
    jmp .add_sep
.strip:
    dec di
.add_sep:
    mov byte [di], '\'
    inc di
    mov byte [di], '*'
    inc di
    mov byte [di], '.'
    inc di
    mov byte [di], '*'
    inc di
    mov byte [di], 0
    jmp .done
.use_star:
    mov byte [dir_pattern + 0], '*'
    mov byte [dir_pattern + 1], '.'
    mov byte [dir_pattern + 2], '*'
    mov byte [dir_pattern + 3], 0
.done:
    pop di
    pop si
    pop cx
    pop ax
    ret

; Print one FindFirst/FindNext result from the DTA, tagging directories.
dir_print_entry:
    push ax
    push si
    mov si, dta_buf + 0x1E
    call print_dual_z_string
    test byte [dta_buf + 0x15], 0x10
    jz .crlf
    mov si, msg_dir_tag
    call print_dual_dollar_string
.crlf:
    mov si, msg_crlf
    call print_dual_dollar_string
    pop si
    pop ax
    ret

print_dual_dollar_string:
.next:
    lodsb
    cmp al, '$'
    je .done
    call dual_putc
    jmp .next
.done:
    ret

print_dual_z_string:
.next:
    lodsb
    cmp al, 0
    je .done
    call dual_putc
    jmp .next
.done:
    ret

print_dual_cx_string:
    jcxz .done
.next:
    lodsb
    call dual_putc
    loop .next
.done:
    ret

dual_putc:
    push ax
    push dx

    mov dl, al
    mov ah, 0x02
    int 0x21

    pop dx
    pop ax

    push ax
    push dx
    xor dx, dx
    mov ah, 0x01
    int 0x14
    pop dx
    pop ax
    ret

msg_banner  db 'CiukiOS SHELL prototype', 0x0D, 0x0A
            db 'Type HELP for commands.', 0x0D, 0x0A, '$'
msg_prompt_pre db 'SHELL ', '$'
msg_help    db 'Commands: HELP VER ECHO CLS EXIT CD DIR TYPE DEL COPY', 0x0D, 0x0A, '$'
msg_ver     db 'CiukiOS SHELL.COM prototype 0.1', 0x0D, 0x0A, '$'
msg_unknown db 'Unknown command. Type HELP.', 0x0D, 0x0A, '$'
msg_cwd_pre db 'Current directory: ', '$'
msg_cd_err  db 'cd: invalid path', 0x0D, 0x0A, '$'
msg_dir_hdr db 'Directory listing', 0x0D, 0x0A, '$'
msg_dir_tag db ' <DIR>', '$'
msg_dir_none db 'dir: path not found', 0x0D, 0x0A, '$'
msg_type_use db 'type: usage: TYPE <file>', 0x0D, 0x0A, '$'
msg_type_err db 'type: cannot open file', 0x0D, 0x0A, '$'
msg_del_use db 'del: usage: DEL <file>', 0x0D, 0x0A, '$'
msg_del_err db 'del: file not found', 0x0D, 0x0A, '$'
msg_del_ok  db 'File deleted', 0x0D, 0x0A, '$'
msg_copy_use db 'copy: usage: COPY <src> <dst>', 0x0D, 0x0A, '$'
msg_copy_src_err db 'copy: source not found', 0x0D, 0x0A, '$'
msg_copy_err db 'copy: failed', 0x0D, 0x0A, '$'
msg_copy_ok db 'File copied', 0x0D, 0x0A, '$'
msg_crlf    db 0x0D, 0x0A, '$'

echo_ptr dw 0
echo_len dw 0
cmd_buf  times 8 db 0
input_buf times 127 db 0
path_buf times 68 db 0
dir_pattern times 32 db 0
dta_buf  times 48 db 0
file_handle dw 0
copy_src_handle dw 0
copy_dst_handle dw 0
src_path times 64 db 0
dst_path times 64 db 0
file_buf times 512 db 0
