bits 16
org 0x0100

%define INPUT_BUF_MAX 126
%define HISTORY_MAX 8
%define HISTORY_ENTRY_LEN 128

start:
    push cs
    pop ds
    push ds
    pop es

    mov si, msg_banner
    call print_dual_dollar_string

main_loop:
    push cs
    pop ds
    push ds
    pop es

    call poll_pending_power_action
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
    cmp di, cmd_buf + 15
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
    je .check_cls_short
    cmp byte [cmd_buf + 2], 'E'
    jne .check_cd
    cmp byte [cmd_buf + 3], 'A'
    jne .check_cd
    cmp byte [cmd_buf + 4], 'R'
    jne .check_cd
    cmp byte [cmd_buf + 5], 0
    jne .check_cd
    jmp .do_cls
.check_cls_short:
    cmp byte [cmd_buf + 3], 0
    jne .check_cd
.do_cls:
    mov ax, 0x0600
    mov bh, 0x07
    xor cx, cx
    mov dx, 0x184F
    int 0x10
    mov ax, 0x0200
    xor bx, bx
    xor dx, dx
    int 0x10
    mov si, msg_banner_compact
    call print_dual_dollar_string
    jmp main_loop

.check_exit:
    cmp byte [cmd_buf + 0], 'E'
    jne .check_quit
    cmp byte [cmd_buf + 1], 'X'
    jne .check_quit
    cmp byte [cmd_buf + 2], 'I'
    jne .unknown
    cmp byte [cmd_buf + 3], 'T'
    jne .unknown
    cmp byte [cmd_buf + 4], 0
    jne .unknown
    jmp .do_exit

.check_quit:
    cmp byte [cmd_buf + 0], 'Q'
    jne .check_power
    cmp byte [cmd_buf + 1], 'U'
    jne .check_power
    cmp byte [cmd_buf + 2], 'I'
    jne .check_power
    cmp byte [cmd_buf + 3], 'T'
    jne .check_power
    cmp byte [cmd_buf + 4], 0
    jne .check_power
.do_exit:
    mov ax, 0x4C00
    int 0x21

.check_power:
    cmp byte [cmd_buf + 0], 'R'
    jne .check_shutdown
    cmp byte [cmd_buf + 1], 'E'
    jne .check_shutdown
    cmp byte [cmd_buf + 2], 'B'
    jne .check_shutdown
    cmp byte [cmd_buf + 3], 'O'
    jne .check_shutdown
    cmp byte [cmd_buf + 4], 'O'
    jne .check_shutdown
    cmp byte [cmd_buf + 5], 'T'
    jne .check_shutdown
    cmp byte [cmd_buf + 6], 0
    jne .check_shutdown
    mov al, 1
    call handle_power_command
    jmp main_loop

.check_shutdown:
    cmp byte [cmd_buf + 0], 'S'
    jne .check_path
    cmp byte [cmd_buf + 1], 'H'
    jne .check_path
    cmp byte [cmd_buf + 2], 'U'
    jne .check_path
    cmp byte [cmd_buf + 3], 'T'
    jne .check_path
    cmp byte [cmd_buf + 4], 'D'
    jne .check_path
    cmp byte [cmd_buf + 5], 'O'
    jne .check_path
    cmp byte [cmd_buf + 6], 'W'
    jne .check_path
    cmp byte [cmd_buf + 7], 'N'
    jne .check_path
    cmp byte [cmd_buf + 8], 0
    jne .check_path
    mov al, 2
    call handle_power_command
    jmp main_loop

.check_path:
    cmp byte [cmd_buf + 0], 'P'
    jne .check_where
    cmp byte [cmd_buf + 1], 'A'
    jne .check_pwd
    cmp byte [cmd_buf + 2], 'T'
    jne .check_where
    cmp byte [cmd_buf + 3], 'H'
    jne .check_where
    cmp byte [cmd_buf + 4], 0
    jne .check_where
    mov si, msg_path
    call print_dual_dollar_string
    jmp main_loop

.check_pwd:
    cmp byte [cmd_buf + 1], 'W'
    jne .check_where
    cmp byte [cmd_buf + 2], 'D'
    jne .check_where
    cmp byte [cmd_buf + 3], 0
    jne .check_where
    jmp .cd_show

.check_where:
    cmp byte [cmd_buf + 0], 'W'
    jne .check_cd
    cmp byte [cmd_buf + 1], 'H'
    jne .check_cd
    cmp byte [cmd_buf + 2], 'E'
    jne .check_cd
    cmp byte [cmd_buf + 3], 'R'
    jne .check_cd
    cmp byte [cmd_buf + 4], 'E'
    jne .check_cd
    cmp byte [cmd_buf + 5], 0
    jne .check_cd
    mov si, [echo_ptr]
    mov di, src_path
.where_arg_parse:
    mov al, [si]
    cmp al, 0
    je .where_arg_done
    cmp al, ' '
    je .where_arg_done
    cmp di, src_path + 63
    jae .where_arg_skip
    mov [di], al
    inc di
.where_arg_skip:
    inc si
    jmp .where_arg_parse
.where_arg_done:
    mov byte [di], 0
    cmp di, src_path
    je .where_usage
    call where_try_current
    jnc .where_done
    mov si, shell_path_apps
    call where_try_prefixed
    jnc .where_done
    mov si, shell_path_drivers
    call where_try_prefixed
    jnc .where_done
    mov si, shell_path_system
    call where_try_prefixed
    jnc .where_done
    call where_try_known_fallback
    jnc .where_done
    mov si, msg_where_miss
    call print_dual_dollar_string
    jmp main_loop
.where_usage:
    mov si, msg_where_use
    call print_dual_dollar_string
    jmp main_loop
.where_done:
    jmp main_loop

; CD / CHDIR: change directory via INT 21h AH=3Bh, then echo the new path.
.check_cd:
    cmp byte [cmd_buf + 0], 'C'
    jne .check_mkdir
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

; MKDIR / MD: create a directory via INT 21h AH=39h.
.check_mkdir:
    cmp byte [cmd_buf + 0], 'M'
    jne .check_rmdir
    cmp byte [cmd_buf + 1], 'D'
    jne .check_mkdir_long
    cmp byte [cmd_buf + 2], 0
    jne .check_rmdir
    jmp .do_mkdir
.check_mkdir_long:
    cmp byte [cmd_buf + 1], 'K'
    jne .check_rmdir
    cmp byte [cmd_buf + 2], 'D'
    jne .check_rmdir
    cmp byte [cmd_buf + 3], 'I'
    jne .check_rmdir
    cmp byte [cmd_buf + 4], 'R'
    jne .check_rmdir
    cmp byte [cmd_buf + 5], 0
    jne .check_rmdir
.do_mkdir:
    mov cx, [echo_len]
    jcxz .mkdir_usage
    mov dx, [echo_ptr]
    mov ah, 0x39
    int 0x21
    jc .mkdir_error
    mov si, msg_mkdir_ok
    call print_dual_dollar_string
    jmp main_loop
.mkdir_error:
    mov si, msg_mkdir_err
    call print_dual_dollar_string
    jmp main_loop
.mkdir_usage:
    mov si, msg_mkdir_use
    call print_dual_dollar_string
    jmp main_loop

; RMDIR / RD: remove a directory via INT 21h AH=3Ah.
.check_rmdir:
    cmp byte [cmd_buf + 0], 'R'
    jne .check_move
    cmp byte [cmd_buf + 1], 'D'
    jne .check_rmdir_long
    cmp byte [cmd_buf + 2], 0
    jne .check_rename
    jmp .do_rmdir
.check_rmdir_long:
    cmp byte [cmd_buf + 1], 'M'
    jne .check_rename
    cmp byte [cmd_buf + 2], 'D'
    jne .check_rename
    cmp byte [cmd_buf + 3], 'I'
    jne .check_rename
    cmp byte [cmd_buf + 4], 'R'
    jne .check_rename
    cmp byte [cmd_buf + 5], 0
    jne .check_rename
.do_rmdir:
    mov cx, [echo_len]
    jcxz .rmdir_usage
    mov dx, [echo_ptr]
    mov ah, 0x3A
    int 0x21
    jc .rmdir_error
    mov si, msg_rmdir_ok
    call print_dual_dollar_string
    jmp main_loop
.rmdir_error:
    mov si, msg_rmdir_err
    call print_dual_dollar_string
    jmp main_loop
.rmdir_usage:
    mov si, msg_rmdir_use
    call print_dual_dollar_string
    jmp main_loop

; REN / RENAME: rename or move a file via INT 21h AH=56h.
.check_rename:
    cmp byte [cmd_buf + 0], 'R'
    jne .check_move
    cmp byte [cmd_buf + 1], 'E'
    jne .check_move
    cmp byte [cmd_buf + 2], 'N'
    jne .check_move
    cmp byte [cmd_buf + 3], 0
    je .do_rename
    cmp byte [cmd_buf + 3], 'A'
    jne .check_move
    cmp byte [cmd_buf + 4], 'M'
    jne .check_move
    cmp byte [cmd_buf + 5], 'E'
    jne .check_move
    cmp byte [cmd_buf + 6], 0
    jne .check_move
    jmp .do_rename

; MOVE: same INT 21h AH=56h path rename/move primitive.
.check_move:
    cmp byte [cmd_buf + 0], 'M'
    jne .check_dir
    cmp byte [cmd_buf + 1], 'O'
    jne .check_dir
    cmp byte [cmd_buf + 2], 'V'
    jne .check_dir
    cmp byte [cmd_buf + 3], 'E'
    jne .check_dir
    cmp byte [cmd_buf + 4], 0
    jne .check_dir
.do_rename:
    mov cx, [echo_len]
    test cx, cx
    jnz .rename_args
.rename_usage:
    mov si, msg_rename_use
    call print_dual_dollar_string
    jmp main_loop
.rename_error:
    mov si, msg_rename_err
    call print_dual_dollar_string
    jmp main_loop
.rename_ok:
    mov si, msg_rename_ok
    call print_dual_dollar_string
    jmp main_loop
.rename_args:
    mov si, [echo_ptr]
    mov di, src_path
.rename_src_parse:
    mov al, [si]
    cmp al, 0
    je .rename_src_end
    cmp al, ' '
    je .rename_src_end
    cmp di, src_path + 63
    jae .rename_src_adv
    mov [di], al
    inc di
.rename_src_adv:
    inc si
    jmp .rename_src_parse
.rename_src_end:
    mov byte [di], 0
    cmp di, src_path
    je .rename_usage
.rename_skip_sp:
    cmp byte [si], ' '
    jne .rename_dst_start
    inc si
    jmp .rename_skip_sp
.rename_dst_start:
    mov di, dst_path
.rename_dst_parse:
    mov al, [si]
    cmp al, 0
    je .rename_dst_end
    cmp al, ' '
    je .rename_dst_end
    cmp di, dst_path + 63
    jae .rename_dst_adv
    mov [di], al
    inc di
.rename_dst_adv:
    inc si
    jmp .rename_dst_parse
.rename_dst_end:
    mov byte [di], 0
    cmp di, dst_path
    je .rename_usage
    push cs
    pop ds
    push cs
    mov dx, src_path
    mov ax, 0x3D00
    int 0x21
    jc .rename_error
    mov [file_handle], ax
    mov bx, ax
    mov ah, 0x3E
    int 0x21
    push cs
    pop ds
    mov dx, src_path
    push cs
    pop es
    mov di, dst_path
    mov ah, 0x56
    int 0x21
    jc .rename_error
    jmp .rename_ok

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
    mov si, input_buf
    mov di, src_path
.exec_name_parse:
    mov al, [si]
    cmp al, 0
    je .exec_name_done
    cmp al, ' '
    je .exec_name_done
    cmp di, src_path + 63
    jae .exec_name_skip
    call upcase_al
    mov [di], al
    inc di
.exec_name_skip:
    inc si
    jmp .exec_name_parse
.exec_name_done:
    mov byte [di], 0
    cmp di, src_path
    je .exec_not_found
.exec_skip_spaces:
    cmp byte [si], ' '
    jne .exec_search
    inc si
    jmp .exec_skip_spaces
.exec_search:
    mov [exec_tail_src], si
    call build_exec_tail
    call setup_exec_block
    call exec_try_known_fallback
    jnc .exec_found
    call exec_try_current
    jnc .exec_found
    cmp ax, 2
    je .exec_try_apps
    cmp ax, 3
    je .exec_try_apps
    jmp .exec_fail
.exec_try_apps:
    mov si, exec_dir_apps
    call exec_try_in_dir
    jnc .exec_found
    cmp ax, 2
    je .exec_try_drivers
    cmp ax, 3
    je .exec_try_drivers
    jmp .exec_fail
.exec_try_drivers:
    mov si, exec_dir_drivers
    call exec_try_in_dir
    jnc .exec_found
    cmp ax, 2
    je .exec_try_system
    cmp ax, 3
    je .exec_try_system
    jmp .exec_fail
.exec_try_system:
    mov si, exec_dir_system
    call exec_try_in_dir
    jnc .exec_found
    cmp ax, 2
    je .exec_not_found
    cmp ax, 3
    je .exec_not_found
    jmp .exec_fail
.exec_found:
    jmp main_loop
.exec_not_found:
    mov si, msg_exec_not_found
    call print_dual_dollar_string
    jmp main_loop
.exec_fail:
    mov si, msg_exec_fail
    call print_dual_dollar_string
    jmp main_loop

handle_power_command:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    mov [power_requested_action], al
    mov si, [echo_ptr]
    mov cx, [echo_len]
    call skip_spaces
    jcxz .immediate

    mov di, src_path
.token_copy:
    mov al, [si]
    cmp al, 0
    je .token_done
    cmp al, ' '
    je .token_done
    cmp di, src_path + 63
    jae .token_skip
    call upcase_al
    mov [di], al
    inc di
.token_skip:
    inc si
    jmp .token_copy

.token_done:
    mov byte [di], 0
    mov [power_arg_ptr], si
    mov si, src_path
    mov di, power_token_cancel
    call strings_equal
    jz .cancel
    mov si, src_path
    mov di, power_token_status
    call strings_equal
    jz .status
    mov si, src_path
    mov di, power_token_timer
    call strings_equal
    jz .timer_switch
    call parse_src_path_seconds
    jc .usage
    test ax, ax
    jz .immediate
    call schedule_power_action
    jmp .done

.timer_switch:
    mov si, [power_arg_ptr]
    call parse_next_power_token
    jc .usage
    call parse_src_path_seconds
    jc .usage
    test ax, ax
    jz .immediate
    call schedule_power_action
    jmp .done

.immediate:
    mov al, [power_requested_action]
    cmp al, 1
    je .do_reboot
    mov si, msg_shutdown_now
    call print_dual_dollar_string
    call shutdown_system
.do_reboot:
    mov si, msg_reboot_now
    call print_dual_dollar_string
    call reboot_system

.cancel:
    cmp byte [pending_power_action], 0
    je .none
    mov byte [pending_power_action], 0
    mov si, msg_power_cancel
    call print_dual_dollar_string
    jmp .done

.status:
    cmp byte [pending_power_action], 0
    je .none
    cmp byte [pending_power_action], 1
    je .status_reboot
    mov si, msg_power_status_shutdown
    call print_dual_dollar_string
    jmp .done
.status_reboot:
    mov si, msg_power_status_reboot
    call print_dual_dollar_string
    jmp .done

.none:
    mov si, msg_power_status_none
    call print_dual_dollar_string
    jmp .done

.usage:
    mov al, [power_requested_action]
    cmp al, 1
    je .usage_reboot
    mov si, msg_shutdown_use
    call print_dual_dollar_string
    jmp .done
.usage_reboot:
    mov si, msg_reboot_use
    call print_dual_dollar_string

.done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

schedule_power_action:
    push bx
    push dx

    mov bx, 18
    mul bx
    or dx, dx
    jnz .fallback_tick
    or ax, ax
    jnz .ticks_ready
.fallback_tick:
    mov ax, 1

.ticks_ready:
    mov bx, ax
    call get_bios_tick_low
    add ax, bx
    mov [pending_power_due_tick], ax
    mov al, [power_requested_action]
    mov [pending_power_action], al
    cmp al, 1
    je .reboot_msg
    mov si, msg_shutdown_scheduled
    call print_dual_dollar_string
    jmp .done
.reboot_msg:
    mov si, msg_reboot_scheduled
    call print_dual_dollar_string

.done:
    pop dx
    pop bx
    ret

poll_pending_power_action:
    cmp byte [pending_power_action], 0
    je .done
    call get_bios_tick_low
    sub ax, [pending_power_due_tick]
    cmp ax, 0x8000
    jae .done
    mov al, [pending_power_action]
    mov byte [pending_power_action], 0
    cmp al, 1
    je .reboot_now
    mov si, msg_shutdown_now
    call print_dual_dollar_string
    call shutdown_system
.reboot_now:
    mov si, msg_reboot_now
    call print_dual_dollar_string
    call reboot_system

.done:
    ret

parse_src_path_seconds:
    push bx
    push cx
    push dx
    push si

    xor ax, ax
    xor cx, cx
    mov si, src_path

.loop:
    mov bl, [si]
    test bl, bl
    jz .end
    cmp bl, '0'
    jb .fail
    cmp bl, '9'
    ja .fail
    mov bx, 10
    mul bx
    or dx, dx
    jne .fail
    mov bl, [si]
    sub bl, '0'
    xor bh, bh
    add ax, bx
    cmp ax, 600
    ja .fail
    inc si
    inc cx
    jmp .loop

.end:
    or cx, cx
    jz .fail
    clc
    jmp .done

.fail:
    stc

.done:
    pop si
    pop dx
    pop cx
    pop bx
    ret

get_bios_tick_low:
    push es
    xor ax, ax
    mov es, ax
    mov ax, [es:0x046C]
    pop es
    ret

parse_next_power_token:
    push di

.skip_spaces:
    cmp byte [si], ' '
    jne .copy_start
    inc si
    jmp .skip_spaces

.copy_start:
    cmp byte [si], 0
    je .fail
    mov di, src_path

.copy:
    mov al, [si]
    cmp al, 0
    je .done
    cmp al, ' '
    je .done
    cmp di, src_path + 63
    jae .advance
    call upcase_al
    mov [di], al
    inc di
.advance:
    inc si
    jmp .copy

.done:
    mov byte [di], 0
    clc
    pop di
    ret

.fail:
    stc
    pop di
    ret

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
    mov byte [history_nav], 0xFF
    mov byte [input_draw_len], 0
    mov byte [input_buf], 0

.read:
    xor ah, ah
    int 0x16
    cmp al, 0x0D
    je .done
    cmp al, 0x03
    je .cancel
    cmp al, 0x08
    je .backspace
    test al, al
    jz .extended
    cmp al, 0x20
    jb .read
    cmp bx, INPUT_BUF_MAX
    jae .read
    mov [input_buf + bx], al
    inc bx
    mov byte [input_buf + bx], 0
    mov byte [input_draw_len], bl
    mov byte [history_nav], 0xFF
    call dual_putc
    jmp .read

.extended:
    cmp ah, 0x48
    je .history_up
    cmp ah, 0x50
    je .history_down
    jmp .read

.backspace:
    cmp bx, 0
    je .read
    dec bx
    mov byte [input_buf + bx], 0
    mov byte [input_draw_len], bl
    mov byte [history_nav], 0xFF
    mov al, 0x08
    call dual_putc
    mov al, ' '
    call dual_putc
    mov al, 0x08
    call dual_putc
    jmp .read

.history_up:
    call history_recall_up
    jnc .read
    call redraw_input_line
    jmp .read

.history_down:
    call history_recall_down
    jnc .read
    call redraw_input_line
    jmp .read

.cancel:
    xor bx, bx
    mov byte [input_buf], 0
    mov byte [history_nav], 0xFF
    mov byte [input_draw_len], 0
    mov si, msg_ctrl_c
    call print_dual_dollar_string
    xor cx, cx
    mov si, input_buf
    ret

.done:
    mov byte [input_buf + bx], 0
    call history_try_store
    mov cx, bx
    mov si, msg_crlf
    call print_dual_dollar_string
    mov si, input_buf
    ret

redraw_input_line:
    push ax
    push cx
    push si

    mov al, 0x0D
    call dual_putc
    call print_prompt
    mov si, input_buf
    mov cx, bx
    call print_dual_cx_string

    xor ax, ax
    mov al, [input_draw_len]
    cmp ax, bx
    jbe .save_len
    sub ax, bx
    mov cx, ax
    mov al, ' '
.erase_tail:
    call dual_putc
    loop .erase_tail
    mov cx, ax
    mov al, 0x08
.back_tail:
    call dual_putc
    loop .back_tail

.save_len:
    mov byte [input_draw_len], bl
    pop si
    pop cx
    pop ax
    ret

history_try_store:
    push ax
    push bx
    push cx
    push si
    push di

    mov si, input_buf
    mov cx, bx
    call skip_spaces
    jcxz .done

    mov al, [history_count]
    or al, al
    jz .store

    mov al, [history_next]
    dec al
    and al, HISTORY_MAX - 1
    call history_slot_to_di
    call history_compare_input_di
    jc .done

.store:
    mov al, [history_next]
    call history_slot_to_di
    mov si, input_buf
    call copy_z_to_di

    mov al, [history_next]
    inc al
    and al, HISTORY_MAX - 1
    mov [history_next], al

    mov al, [history_count]
    cmp al, HISTORY_MAX
    jae .done
    inc al
    mov [history_count], al

.done:
    pop di
    pop si
    pop cx
    pop bx
    pop ax
    ret

history_recall_up:
    push ax
    push dx

    mov dl, [history_count]
    or dl, dl
    jz .fail

    mov al, [history_nav]
    cmp al, 0xFF
    jne .next_older
    xor al, al
    jmp .check

.next_older:
    inc al

.check:
    cmp al, dl
    jae .fail
    mov [history_nav], al
    call history_load_nav_entry
    stc
    jmp .done

.fail:
    clc

.done:
    pop dx
    pop ax
    ret

history_recall_down:
    push ax

    mov al, [history_nav]
    cmp al, 0xFF
    je .fail
    or al, al
    jnz .newer

    mov byte [history_nav], 0xFF
    xor bx, bx
    mov byte [input_buf], 0
    stc
    jmp .done

.newer:
    dec al
    mov [history_nav], al
    call history_load_nav_entry
    stc
    jmp .done

.fail:
    clc

.done:
    pop ax
    ret

history_load_nav_entry:
    push ax
    push si
    push di

    mov al, [history_next]
    dec al
    sub al, [history_nav]
    and al, HISTORY_MAX - 1
    call history_slot_to_di

    mov si, di
    mov di, input_buf
    xor bx, bx

.copy:
    lodsb
    stosb
    test al, al
    jz .done
    inc bx
    jmp .copy

.done:
    pop di
    pop si
    pop ax
    ret

history_compare_input_di:
    push ax
    push si
    push di

    mov si, input_buf

.loop:
    mov al, [si]
    cmp al, [di]
    jne .not_equal
    test al, al
    jz .equal
    inc si
    inc di
    jmp .loop

.equal:
    stc
    jmp .done

.not_equal:
    clc

.done:
    pop di
    pop si
    pop ax
    ret

history_slot_to_di:
    push ax
    xor ah, ah
    mov di, ax
    shl di, 1
    shl di, 1
    shl di, 1
    shl di, 1
    shl di, 1
    shl di, 1
    shl di, 1
    add di, history_buf
    pop ax
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

where_try_current:
    mov si, ext_none
    call where_build_relative_candidate
    call where_try_candidate
    jnc .found
    mov si, ext_com
    call where_build_relative_candidate
    call where_try_candidate
    jnc .found
    mov si, ext_exe
    call where_build_relative_candidate
    call where_try_candidate
    ret
.found:
    call where_print_current_candidate
    clc
    ret

where_try_prefixed:
    mov [where_prefix_ptr], si
    mov si, ext_none
    call where_build_prefixed_candidate
    call where_try_candidate
    jnc .found
    mov si, ext_com
    call where_build_prefixed_candidate
    call where_try_candidate
    jnc .found
    mov si, ext_exe
    call where_build_prefixed_candidate
    call where_try_candidate
    ret
.found:
    mov ah, 0x19
    int 0x21
    add al, 'A'
    call dual_putc
    mov al, ':'
    call dual_putc
    mov si, dst_path
    call print_dual_z_string
    mov si, msg_crlf
    call print_dual_dollar_string
    clc
    ret

where_build_relative_candidate:
    push si
    mov di, dst_path
    mov si, src_path
    call copy_z_to_di
    pop si
    call append_z_to_di
    ret

where_build_prefixed_candidate:
    push si
    mov di, dst_path
    mov si, [where_prefix_ptr]
    call copy_z_to_di
    mov si, src_path
    call append_z_to_di
    pop si
    call append_z_to_di
    ret

where_try_candidate:
    push ds
    push cs
    pop ds
    mov dx, dst_path
    mov ax, 0x4300
    int 0x21
    jc .fail
    test cl, 0x10
    jnz .fail
    pop ds
    clc
    ret
.fail:
    pop ds
    stc
    ret

where_print_current_candidate:
    push ax
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
    cmp byte [path_buf], 0
    je .name
    mov al, '\'
    call dual_putc
.name:
    mov si, dst_path
    call print_dual_z_string
    mov si, msg_crlf
    call print_dual_dollar_string
    pop si
    pop dx
    pop ax
    ret

copy_z_to_di:
.next:
    lodsb
    test al, al
    jz .done
    stosb
    jmp .next
.done:
    mov byte [di], 0
    ret

append_z_to_di:
.seek:
    cmp byte [di], 0
    je .copy
    inc di
    jmp .seek
.copy:
    lodsb
    mov [di], al
    inc di
    test al, al
    jnz .copy
    ret

where_try_known_fallback:
    mov si, src_path
    mov di, where_name_shell
    call strings_equal
    jz .shell
    mov si, src_path
    mov di, where_name_shell_com
    call strings_equal
    jz .shell
    mov si, src_path
    mov di, where_name_dos4gw
    call strings_equal
    jz .dos4gw
    mov si, src_path
    mov di, where_name_dos4gw_exe
    call strings_equal
    jz .dos4gw
    mov si, src_path
    mov di, where_name_mouse
    call strings_equal
    jz .mouse
    mov si, src_path
    mov di, where_name_mouse_com
    call strings_equal
    jz .mouse
    stc
    ret
.shell:
    mov si, where_out_shell
    call print_dual_dollar_string
    clc
    ret
.dos4gw:
    mov si, where_out_dos4gw
    call print_dual_dollar_string
    clc
    ret
.mouse:
    mov si, where_out_mouse
    call print_dual_dollar_string
    clc
    ret

strings_equal:
.next:
    mov al, [si]
    cmp al, [di]
    jne .noteq
    test al, al
    je .eq
    inc si
    inc di
    jmp .next
.noteq:
    mov al, 1
    or al, al
    ret
.eq:
    xor al, al
    or al, al
    ret

exec_try_current:
    mov si, ext_none
    call where_build_relative_candidate
    call exec_run_candidate
    jnc .found
    cmp ax, 2
    jne .done
    mov si, ext_com
    call where_build_relative_candidate
    call exec_run_candidate
    jnc .found
    cmp ax, 2
    jne .done
    mov si, ext_exe
    call where_build_relative_candidate
    call exec_run_candidate
.done:
    ret
.found:
    clc
    ret

exec_try_prefixed:
    mov [where_prefix_ptr], si
    mov si, ext_none
    call where_build_prefixed_candidate
    call exec_run_candidate
    jnc .found
    cmp ax, 2
    jne .done
    mov si, ext_com
    call where_build_prefixed_candidate
    call exec_run_candidate
    jnc .found
    cmp ax, 2
    jne .done
    mov si, ext_exe
    call where_build_prefixed_candidate
    call exec_run_candidate
.done:
    ret
.found:
    clc
    ret

exec_try_in_dir:
    push si
    push ds
    push cs
    pop ds
    xor dl, dl
    mov si, exec_saved_cwd
    mov ah, 0x47
    int 0x21
    pop ds
    pop si
    push ds
    push cs
    pop ds
    mov dx, si
    mov ah, 0x3B
    int 0x21
    pop ds
    jc .done
    call exec_try_current
    pushf
    push ax
    push ds
    push cs
    pop ds
    mov di, exec_restore_path
    mov byte [di], '\'
    inc di
    mov si, exec_saved_cwd
    call copy_z_to_di
    mov dx, exec_restore_path
    mov ah, 0x3B
    int 0x21
    pop ds
    pop ax
    popf
.done:
    ret

setup_exec_block:
    mov ah, 0x62
    int 0x21
    mov [exec_psp_seg], bx
    mov word [exec_env_seg], 0
    mov word [exec_tail_ptr], exec_tail
    mov word [exec_tail_seg], cs
    mov word [exec_fcb1_ptr], 0x005C
    mov word [exec_fcb1_seg], bx
    mov word [exec_fcb2_ptr], 0x006C
    mov word [exec_fcb2_seg], bx
    ret

exec_run_candidate:
    push ds
    push es
    push cs
    pop ds
    mov dx, dst_path
    push cs
    pop es
    mov bx, exec_env_seg
    mov ax, 0x4B00
    int 0x21
    pop es
    pop ds
    ret

exec_try_known_fallback:
    mov si, src_path
    mov di, where_name_shell
    call strings_equal
    jz .shell
    mov si, src_path
    mov di, where_name_shell_com
    call strings_equal
    jz .shell
    mov si, src_path
    mov di, where_name_dos4gw
    call strings_equal
    jz .dos4gw
    mov si, src_path
    mov di, where_name_dos4gw_exe
    call strings_equal
    jz .dos4gw
    mov si, src_path
    mov di, where_name_mouse
    call strings_equal
    jz .mouse
    mov si, src_path
    mov di, where_name_mouse_com
    call strings_equal
    jz .mouse
    stc
    ret
.shell:
    mov si, exec_path_system_shell
    jmp .run
.dos4gw:
    mov si, exec_path_drivers_dos4gw
    jmp .run
.mouse:
    mov si, exec_path_system_mouse
.run:
    mov di, dst_path
    call copy_z_to_di
    call exec_run_candidate
    ret

build_exec_tail:
    push ax
    push cx
    push si
    push di
    mov si, [exec_tail_src]
    mov di, exec_tail + 1
    xor cx, cx
    cmp byte [si], 0
    je .done
    mov byte [di], ' '
    inc di
    inc cx
.copy:
    mov al, [si]
    cmp al, 0
    je .done
    cmp cx, 126
    jae .done
    mov [di], al
    inc di
    inc si
    inc cx
    jmp .copy
.done:
    mov [exec_tail], cl
    mov byte [di], 0x0D
    inc di
    mov byte [di], 0
    pop di
    pop si
    pop cx
    pop ax
    ret

reboot_system:
    push cs
    pop ds
    push cs
    pop es
    xor ax, ax
    mov cx, ax
    mov dx, ax
    int 0x19
    hlt
    jmp reboot_system

shutdown_system:
    push cs
    pop ds
    xor ax, ax
    mov cx, ax
    mov dx, ax
    mov sp, 0xFFFC
    hlt
    jmp shutdown_system

msg_banner  db 'CiukiOS pre-Alpha v0.6.6 (CiukiDOS SHELL.COM)', 0x0D, 0x0A
            db 'HELP lists commands. WHERE shows launch targets.', 0x0D, 0x0A
            db 'Try REBOOT 5 or SHUTDOWN 5 for queued power actions.', 0x0D, 0x0A, '$'
msg_banner_compact db 'CiukiOS SHELL ready', 0x0D, 0x0A, '$'
msg_prompt_pre db 'CiukiOS SHELL ', '$'
msg_help    db 'SHELL.COM commands:', 0x0D, 0x0A
            db '  System: HELP VER ECHO CLS EXIT QUIT REBOOT SHUTDOWN', 0x0D, 0x0A
            db '  Navigation: CD CHDIR DIR PATH WHERE PWD', 0x0D, 0x0A
            db '  Files: TYPE COPY DEL ERASE REN RENAME MOVE MKDIR MD RMDIR RD', 0x0D, 0x0A
            db '  Execution: run name/path, MOUSE from C:\SYSTEM', 0x0D, 0x0A
            db '  Stage1 fallback: use EXIT or QUIT', 0x0D, 0x0A
            db '  Use WHERE <name>; SHUTDOWN STATUS or CANCEL manage queue', 0x0D, 0x0A
            db '  Aliases: CLEAR QUIT PWD', 0x0D, 0x0A, '$'
msg_ver     db 'CiukiOS pre-Alpha v0.6.6 (CiukiDOS SHELL.COM)', 0x0D, 0x0A, '$'
msg_unknown db 'command: not found', 0x0D, 0x0A, '$'
msg_exec_not_found db 'command: not found', 0x0D, 0x0A, '$'
msg_exec_fail db 'run: cannot execute', 0x0D, 0x0A, '$'
msg_path    db 'C:\APPS;C:\SYSTEM\DRIVERS;C:\SYSTEM', 0x0D, 0x0A, '$'
msg_where_use db 'usage: where <name>', 0x0D, 0x0A, '$'
msg_where_miss db 'where: not found', 0x0D, 0x0A, '$'
msg_cwd_pre db 'Current directory: ', '$'
msg_cd_err  db 'cd: invalid path', 0x0D, 0x0A, '$'
msg_mkdir_use db 'usage: mkdir <dir>', 0x0D, 0x0A, '$'
msg_mkdir_err db 'mkdir: invalid path', 0x0D, 0x0A, '$'
msg_mkdir_ok db 'Directory created', 0x0D, 0x0A, '$'
msg_rmdir_use db 'usage: rmdir <dir>', 0x0D, 0x0A, '$'
msg_rmdir_err db 'rmdir: invalid path', 0x0D, 0x0A, '$'
msg_rmdir_ok db 'Directory removed', 0x0D, 0x0A, '$'
msg_rename_use db 'usage: ren/move <src> <dst>', 0x0D, 0x0A, '$'
msg_rename_err db 'ren: cannot rename', 0x0D, 0x0A, '$'
msg_rename_ok db 'Rename complete', 0x0D, 0x0A, '$'
msg_dir_hdr db 'Directory listing', 0x0D, 0x0A, '$'
msg_dir_tag db ' <DIR>', '$'
msg_dir_none db 'dir: path not found', 0x0D, 0x0A, '$'
msg_type_use db 'usage: type <file>', 0x0D, 0x0A, '$'
msg_type_err db 'type: cannot open file', 0x0D, 0x0A, '$'
msg_del_use db 'usage: del <file>', 0x0D, 0x0A, '$'
msg_del_err db 'del: file not found', 0x0D, 0x0A, '$'
msg_del_ok  db 'File deleted', 0x0D, 0x0A, '$'
msg_copy_use db 'usage: copy <src> <dst>', 0x0D, 0x0A, '$'
msg_copy_src_err db 'copy: source not found', 0x0D, 0x0A, '$'
msg_copy_err db 'copy: failed', 0x0D, 0x0A, '$'
msg_copy_ok db 'File copied', 0x0D, 0x0A, '$'
msg_reboot_use db 'usage: reboot [/t seconds|seconds|status|cancel]', 0x0D, 0x0A, '$'
msg_shutdown_use db 'usage: shutdown [/t seconds|seconds|status|cancel]', 0x0D, 0x0A, '$'
msg_reboot_scheduled db 'reboot: queued', 0x0D, 0x0A, '$'
msg_shutdown_scheduled db 'shutdown: queued', 0x0D, 0x0A, '$'
msg_reboot_now db 'rebooting...', 0x0D, 0x0A, '$'
msg_shutdown_now db 'halting...', 0x0D, 0x0A, '$'
msg_power_cancel db 'shutdown: canceled', 0x0D, 0x0A, '$'
msg_power_status_none db 'shutdown: idle', 0x0D, 0x0A, '$'
msg_power_status_reboot db 'shutdown: pending reboot', 0x0D, 0x0A, '$'
msg_power_status_shutdown db 'shutdown: pending halt', 0x0D, 0x0A, '$'
msg_ctrl_c  db '^C', 0x0D, 0x0A, '$'
msg_crlf    db 0x0D, 0x0A, '$'
shell_path_apps db '\APPS\', 0
shell_path_drivers db '\SYSTEM\DRIVERS\', 0
shell_path_system db '\SYSTEM\', 0
exec_path_apps db 'C:\APPS\', 0
exec_path_drivers db 'C:\SYSTEM\DRIVERS\', 0
exec_path_system db 'C:\SYSTEM\', 0
exec_dir_apps db '\APPS', 0
exec_dir_drivers db '\SYSTEM\DRIVERS', 0
exec_dir_system db '\SYSTEM', 0
ext_none db 0
ext_com db '.COM', 0
ext_exe db '.EXE', 0
where_name_shell db 'SHELL', 0
where_name_shell_com db 'SHELL.COM', 0
where_name_dos4gw db 'DOS4GW', 0
where_name_dos4gw_exe db 'DOS4GW.EXE', 0
where_name_mouse db 'MOUSE', 0
where_name_mouse_com db 'MOUSE.COM', 0
power_token_cancel db 'CANCEL', 0
power_token_status db 'STATUS', 0
power_token_timer db '/T', 0
where_out_shell db 'C:\SYSTEM\SHELL.COM', 0x0D, 0x0A, '$'
where_out_dos4gw db 'C:\SYSTEM\DRIVERS\DOS4GW.EXE', 0x0D, 0x0A, '$'
where_out_mouse db 'C:\SYSTEM\MOUSE.COM', 0x0D, 0x0A, '$'
exec_path_system_shell db 'C:\SYSTEM\SHELL.COM', 0
exec_path_drivers_dos4gw db 'C:\SYSTEM\DRIVERS\DOS4GW.EXE', 0
exec_path_system_mouse db 'C:\SYSTEM\MOUSE.COM', 0

echo_ptr dw 0
echo_len dw 0
pending_power_due_tick dw 0
power_arg_ptr dw 0
where_prefix_ptr dw 0
power_requested_action db 0
pending_power_action db 0
exec_tail_src dw 0
exec_psp_seg dw 0
exec_env_seg dw 0
exec_tail_ptr dw 0
exec_tail_seg dw 0
exec_fcb1_ptr dw 0
exec_fcb1_seg dw 0
exec_fcb2_ptr dw 0
exec_fcb2_seg dw 0
cmd_buf  times 16 db 0
input_buf times 127 db 0
input_draw_len db 0
history_count db 0
history_next db 0
history_nav db 0xFF
history_buf times HISTORY_MAX * HISTORY_ENTRY_LEN db 0
path_buf times 68 db 0
dir_pattern times 32 db 0
dta_buf  times 48 db 0
file_handle dw 0
copy_src_handle dw 0
copy_dst_handle dw 0
src_path times 64 db 0
dst_path times 64 db 0
exec_saved_cwd times 68 db 0
exec_restore_path times 69 db 0
exec_tail times 129 db 0
file_buf times 512 db 0
