bits 16
org 0x0100
%include "src/com/dos_window_abi.inc"

%define INPUT_BUF_MAX 126
%define HISTORY_MAX 8
%define HISTORY_ENTRY_LEN 128
%define TITLE_BAR_ATTR 0x1F
%define TITLE_BAR_COL 29

start:
    ; COM programs enter with a stack near the end of their 64 KiB arena.
    ; Move it into the image before shrinking the shell PSP block, otherwise
    ; every child loses almost all of that arena even while the shell sleeps.
    cli
    mov ax, cs
    mov ss, ax
    mov sp, shell_stack_top
    sti

    mov es, ax
    mov bx, ((shell_image_end - $$ + 0x0100) + 15) >> 4
    mov ah, 0x4A
    int 0x21

    push cs
    pop ds
    push ds
    pop es

%ifdef COMMAND_COMPAT
    call command_compat_init
    cmp byte [command_once_pending], 1
    je main_loop
%endif

%ifndef COMMAND_COMPAT
    call history_allocate
    ; Protect BIOS video calls, then select Live/Setup before probing graphics.
    call startup_display_services
    call startup_driver_services
    call startup_select_session
    test al,al
    jnz main_loop
%endif

%ifndef COMMAND_COMPAT
    jmp ui_enter
%else
    call redraw_title_bar
    mov si, msg_banner_body
    call print_dual_dollar_string
%endif

main_loop:
%ifndef COMMAND_COMPAT
    cmp byte [ui_request],1
    je ui_enter
    cmp byte [ui_command_running],0
    jne ui_command_return
%endif
%ifdef COMMAND_COMPAT
    call command_window_poll
    cmp byte [command_once_done], 0
    jne command_compat_terminate
%endif
    push cs
    pop ds
    push ds
    pop es

    call poll_pending_power_action
%ifdef COMMAND_COMPAT
    cmp byte [command_once_pending], 1
    je .use_command_tail
%endif
    call print_prompt

    call read_line
    jmp .line_ready

%ifdef COMMAND_COMPAT
.use_command_tail:
    mov byte [command_once_pending], 0
    mov al, [command_keep_open]
    xor al, 1
    mov [command_once_done], al
    xor cx, cx
    mov cl, [command_once_length]
    mov si, input_buf
%endif
.line_ready:
    call skip_spaces
    jcxz main_loop
    mov [cmd_start], si

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

%ifndef COMMAND_COMPAT
    call desktop_dispatch
    jnc main_loop
%endif
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
%ifndef COMMAND_COMPAT
    cmp byte [vc_active], 1
    jne .text_cls
    call vc_clear
    call vc_title
    jmp main_loop
.text_cls:
%endif
    ; BDA 40:84 is the last displayed row on EGA/VGA.  Respect the 80x50
    ; profile instead of clearing only the historic first 25 rows.
    push es
    mov ax, 0x0040
    mov es, ax
    mov dh, [es:0x0084]
    cmp dh, 24
    jae .cls_rows_ready
    mov dh, 24
.cls_rows_ready:
    pop es
    mov dl, 79
    mov ax, 0x0600
    mov bh, 0x07
    xor cx, cx
    call shell_bios
    call redraw_title_bar
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
%ifdef COMMAND_COMPAT
    jmp command_compat_terminate
%else
    jmp ui_enter
%endif

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
    jne .check_run
    cmp byte [cmd_buf + 1], 'H'
    jne .check_run
    cmp byte [cmd_buf + 2], 'E'
    jne .check_run
    cmp byte [cmd_buf + 3], 'R'
    jne .check_run
    cmp byte [cmd_buf + 4], 'E'
    jne .check_run
    cmp byte [cmd_buf + 5], 0
    jne .check_run
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
    call upcase_al
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
    mov si, shell_path_net
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

.check_run:
    cmp byte [cmd_buf + 0], 'R'
    jne .check_cd
    cmp byte [cmd_buf + 1], 'U'
    jne .check_cd
    cmp byte [cmd_buf + 2], 'N'
    jne .check_cd
    cmp byte [cmd_buf + 3], 0
    jne .check_cd
    mov si, [echo_ptr]
.run_skip_spaces:
    cmp byte [si], ' '
    jne .run_parse
    inc si
    jmp .run_skip_spaces
.run_parse:
    cmp byte [si], 0
    je .run_usage
    mov di, src_path
    jmp .exec_name_parse
.run_usage:
    mov si, msg_run_use
    call print_dual_dollar_string
    jmp main_loop

; CD / CHDIR: change directory via INT 21h AH=3Bh, then echo the new path.
.check_cd:
    cmp byte [cmd_buf + 0], 'C'
    jne .check_mkdir
    cmp byte [cmd_buf + 1], 'D'
    jne .check_chdir
    cmp byte [cmd_buf + 2], 0
    je .do_cd
    cmp byte [cmd_buf + 2], '.'
    je .do_cd_compact
    cmp byte [cmd_buf + 2], '\'
    je .do_cd_compact
    cmp byte [cmd_buf + 2], '/'
    je .do_cd_compact
    jmp .check_dir
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
    mov si, [echo_ptr]
    call copy_path_token_to_src
    jmp .cd_apply
.do_cd_compact:
    mov si, cmd_buf + 2
    call copy_path_token_to_src
.cd_apply:
    call resolve_src_path_to_dst
    mov dx, dst_path
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
.rename_open:
    push cs
    pop ds
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
    mov si, [echo_ptr]
    call copy_path_token_to_src
    call canonicalize_src_path
    mov dx, src_path
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
    mov si, [echo_ptr]
    call copy_path_token_to_src
    call canonicalize_src_path
    mov dx, src_path
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
    call copy_path_token_to_src
    cmp byte [src_path], 0
    je .copy_t_usage
    call canonicalize_src_path
.copy_skip_sp:
    cmp byte [si], ' '
    jne .copy_dst_start
    inc si
    jmp .copy_skip_sp
.copy_dst_start:
    call copy_path_token_to_dst
    cmp byte [dst_path], 0
    je .copy_t_usage
    call canonicalize_dst_path
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
    mov si, [cmd_start]         ; leading spaces were already skipped
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
    je .exec_try_net
    cmp ax, 3
    je .exec_try_net
    jmp .exec_fail
.exec_try_net:
    mov si, exec_dir_net
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
%ifndef COMMAND_COMPAT
    ; Consume the actual child's status once. A successful explicit display
    ; choice can leave safe boot; failed/cancelled settings keep it intact.
    mov ah, 0x4D
    int 0x21
    mov [ui_child_exit], al
    test al, al
    jnz .display_choice_done
    mov si, src_path
    mov di, si
.display_basename:
    lodsb
    cmp al, '\'
    je .display_separator
    cmp al, ':'
    jne .display_name_next
.display_separator:
    mov di, si
.display_name_next:
    test al, al
    jnz .display_basename
    cmp dword [di], 'VGAS'
    jne .display_choice_done
    cmp dword [di+4], 'ETUP'
    jne .display_choice_done
    cmp byte [di+8], 0
    je .display_choice_applied
    cmp dword [di+8], '.COM'
    jne .display_choice_done
    cmp byte [di+12], 0
    jne .display_choice_done
.display_choice_applied:
    mov byte [vc_force_safe], 0
.display_choice_done:
%endif
%ifdef COMMAND_COMPAT
    mov ah, 0x4D
    int 0x21
    mov [command_exit_code], al
    ; `/C` is returning directly to its caller.  The caller, rather than this
    ; transient command interpreter, owns the display that the child left.
    cmp byte [command_once_done], 0
    jne command_compat_terminate
%endif
    ; Restore graphics children, but retain output from text commands such as
    ; VGASETUP. An unconditional mode set erased their results immediately.
    call restore_shell_video_state
    jmp main_loop
.exec_not_found:
%ifndef COMMAND_COMPAT
    mov al,[ui_command_running]
    mov [ui_exec_error],al
%endif
%ifdef COMMAND_COMPAT
    mov byte [command_exit_code], 1
%endif
%ifndef COMMAND_COMPAT
    call restore_shell_video_state
%endif
    mov si, msg_exec_not_found
    call print_dual_dollar_string
    jmp main_loop
.exec_fail:
%ifndef COMMAND_COMPAT
    push ax
    mov al,[ui_command_running]
    mov [ui_exec_error],al
    pop ax
%endif
%ifdef COMMAND_COMPAT
    mov byte [command_exit_code], 1
%endif
%ifndef COMMAND_COMPAT
    push ax
    call restore_shell_video_state
    pop ax
%endif
    cmp ax, 0x0008
    je .exec_no_memory
    cmp ax, 0x000B
    je .exec_bad_format
    call print_exec_error
    jmp main_loop
.exec_no_memory:
    mov si, msg_exec_no_mem
    call print_dual_dollar_string
    jmp main_loop
.exec_bad_format:
    mov si, msg_exec_bad_format
    call print_dual_dollar_string
    jmp main_loop

%ifdef COMMAND_COMPAT
command_compat_terminate:
    mov al, [command_exit_code]
    mov ah, 0x4C
    int 0x21
    hlt
    jmp command_compat_terminate

; Only the native window launcher opts into this private close handshake.
; A normal COMMAND.COM never sends unknown requests to firmware.
command_window_poll:
    cmp byte [command_window],1
    jne .done
    pushad
    mov ax,DW_QUERY_AX
    mov bx,DW_QUERY_BX
    int 0x10
    cmp ax,DW_QUERY_REPLY
    jne .restore
    test dx,1
    jnz command_compat_terminate
.restore:
    popad
.done:
    ret
%endif

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

%ifdef COMMAND_COMPAT
; COMMAND.COM compatibility entry.  Normal DOS applications commonly invoke
; COMSPEC with `/C command`; execute that command once through the same parser
; used by the interactive shell and then return through DOS AH=4Ch.  With no
; `/C`, this binary is an ordinary nested interactive command interpreter in
; which EXIT/QUIT terminate only the nested process.
command_compat_init:
    mov byte [command_once_pending], 0
    mov byte [command_once_done], 0
    mov byte [command_once_length], 0
    xor cx, cx
    mov cl, [0x0080]
    mov si, 0x0081
.skip_leading:
    jcxz .done
    cmp byte [si], ' '
    je .skip_one
    cmp byte [si], 0x09
    jne .check_switch
.skip_one:
    inc si
    dec cx
    jmp .skip_leading
.check_switch:
    cmp cx, 2
    jb .done
    mov al, [si]
    cmp al, '/'
    je .switch_prefix
    cmp al, '-'
    jne .done
.switch_prefix:
    mov al, [si + 1]
    and al, 0xDF
    cmp al,'W'
    jne .standard_switch
    mov byte [command_window],1
    add si,2
    sub cx,2
    jmp .skip_leading
.standard_switch:
    cmp al, 'C'
    je .switch_valid
    cmp al, 'K'
    jne .done
    mov byte [command_keep_open], 1
.switch_valid:
    add si, 2
    sub cx, 2
.skip_command_space:
    jcxz .arm
    cmp byte [si], ' '
    je .skip_command_one
    cmp byte [si], 0x09
    jne .copy
.skip_command_one:
    inc si
    dec cx
    jmp .skip_command_space
.copy:
    mov di, input_buf
    xor bx, bx
.copy_loop:
    jcxz .copy_done
    cmp bx, INPUT_BUF_MAX
    jae .copy_done
    mov al, [si]
    cmp al, 0x0D
    je .copy_done
    mov [di], al
    inc si
    inc di
    inc bx
    dec cx
    jmp .copy_loop
.copy_done:
    mov byte [di], 0
    mov [command_once_length], bl
.arm:
    mov byte [command_once_pending], 1
.done:
    ret
%endif

upcase_al:
    cmp al, 'a'
    jb .done
    cmp al, 'z'
    ja .done
    sub al, 32
.done:
    ret

%include "src/com/shell_input.inc"

%ifndef COMMAND_COMPAT
; Persistent, PSP-owned history is separate from the 64 KiB shell code arena.
; COMMAND.COM keeps its original inline buffer and binary layout.
history_allocate:
    pusha
    mov bx,(HISTORY_MAX*HISTORY_ENTRY_LEN)/16
    mov ah,0x48
    int 0x21
    jc .done
    mov [history_segment],ax
.done:
    popa
    ret
%endif

history_try_store:
    push ax
    push bx
    push cx
    push si
    push di
%ifndef COMMAND_COMPAT
    push es
    cmp word [history_segment],0
    je .done
    mov es,[history_segment]
%endif

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
%ifdef COMMAND_COMPAT
    call copy_z_to_di
%else
.copy_history:
    lodsb
    stosb
    test al,al
    jnz .copy_history
%endif

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
%ifndef COMMAND_COMPAT
    pop es
%endif
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
%ifndef COMMAND_COMPAT
    push fs
    mov fs,[history_segment]
%endif

    mov al, [history_next]
    dec al
    sub al, [history_nav]
    and al, HISTORY_MAX - 1
    call history_slot_to_di

    mov si, di
    mov di, input_buf
    xor bx, bx

.copy:
%ifdef COMMAND_COMPAT
    lodsb
%else
    fs lodsb
%endif
    stosb
    test al, al
    jz .done
    inc bx
    jmp .copy

.done:
%ifndef COMMAND_COMPAT
    pop fs
%endif
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
%ifdef COMMAND_COMPAT
    cmp al, [di]
%else
    cmp al, [es:di]
%endif
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
    mov cx, [echo_len]
    jcxz .use_cwd
    mov si, [echo_ptr]
    call copy_path_token_to_src
    jmp .have_target
.use_cwd:
    mov byte [src_path], 0
.have_target:
    call resolve_src_path_to_dst
    mov di, dir_pattern
    mov si, dst_path
    call copy_z_to_di
    cmp di, dir_pattern
    je .append_mask
    cmp byte [di - 1], '\'
    je .append_mask
    mov byte [di], '\'
    inc di
.append_mask:
    mov byte [di], '*'
    inc di
    mov byte [di], '.'
    inc di
    mov byte [di], '*'
    inc di
    mov byte [di], 0
.done:
    pop di
    pop si
    pop cx
    pop ax
    ret

copy_path_token_to_src:
    push ax
    push di
    mov di, src_path
.copy:
    mov al, [si]
    cmp al, 0
    je .done
    cmp al, ' '
    je .done
    cmp al, '/'
    jne .store
    mov al, '\'
.store:
    cmp di, src_path + 63
    jae .advance
    mov [di], al
    inc di
.advance:
    inc si
    jmp .copy
.done:
    mov byte [di], 0
    pop di
    pop ax
    ret

copy_path_token_to_dst:
    push ax
    push di
    mov di, dst_path
.copy:
    mov al, [si]
    cmp al, 0
    je .done
    cmp al, ' '
    je .done
    cmp al, '/'
    jne .store
    mov al, '\'
.store:
    cmp di, dst_path + 63
    jae .advance
    mov [di], al
    inc di
.advance:
    inc si
    jmp .copy
.done:
    mov byte [di], 0
    pop di
    pop ax
    ret

resolve_src_path_to_dst:
    push ax
    push bx
    push dx
    push si
    push di

    cmp byte [src_path], 0
    jne .have_arg
    call build_current_path_in_dst
    jmp .done

.have_arg:
    cmp byte [src_path + 1], ':'
    je .copy_passthrough

    mov di, dst_path
    mov al, [src_path]
    cmp al, '\'
    je .from_root
    cmp al, '/'
    je .from_root

    call build_current_path_in_dst
    mov si, src_path
    jmp .parse

.from_root:
    mov byte [di], '\'
    inc di
    mov byte [di], 0
    mov si, src_path
    inc si
    jmp .parse

.copy_passthrough:
    mov si, src_path
    mov di, dst_path
    call copy_z_to_di
    mov byte [di], 0
    jmp .done

.parse:
.skip_sep:
    mov al, [si]
    cmp al, '\'
    je .skip_one
    cmp al, '/'
    je .skip_one
    jmp .check_end
.skip_one:
    inc si
    jmp .skip_sep

.check_end:
    cmp byte [si], 0
    je .ensure_root

    mov bx, path_buf
.copy_component:
    mov al, [si]
    cmp al, 0
    je .component_done
    cmp al, '\'
    je .component_done
    cmp al, '/'
    je .component_done
    cmp bx, path_buf + 67
    jae .component_advance
    mov [bx], al
    inc bx
.component_advance:
    inc si
    jmp .copy_component

.component_done:
    mov byte [bx], 0
    cmp byte [path_buf], 0
    je .parse
    cmp byte [path_buf], '.'
    jne .append_component
    cmp byte [path_buf + 1], 0
    je .parse
    cmp byte [path_buf + 1], '.'
    jne .append_component
    cmp byte [path_buf + 2], 0
    jne .append_component
    call pop_dst_component
    jmp .parse

.append_component:
    cmp di, dst_path + 1
    jbe .append_text
    cmp byte [di - 1], '\'
    je .append_text
    mov byte [di], '\'
    inc di
.append_text:
    mov bx, path_buf
.append_loop:
    mov al, [bx]
    cmp al, 0
    je .append_done
    cmp di, dst_path + 63
    jae .append_advance
    mov [di], al
    inc di
.append_advance:
    inc bx
    jmp .append_loop

.append_done:
    mov byte [di], 0
    jmp .parse

.ensure_root:
    cmp di, dst_path
    jne .done
    mov byte [di], '\'
    inc di
    mov byte [di], 0

.done:
    pop di
    pop si
    pop dx
    pop bx
    pop ax
    ret

canonicalize_src_path:
    push si
    push di
    call resolve_src_path_to_dst
    mov si, dst_path
    mov di, src_path
    call copy_z_to_di
    pop di
    pop si
    ret

canonicalize_dst_path:
    push si
    push di
    mov si, src_path
    mov di, exec_restore_path
    call copy_z_to_di
    mov si, dst_path
    mov di, src_path
    call copy_z_to_di
    call resolve_src_path_to_dst
    mov si, exec_restore_path
    mov di, src_path
    call copy_z_to_di
    pop di
    pop si
    ret

canonicalize_rename_dst_path:
    push ax
    push si
    push di
    call dst_path_has_separator
    jnc .done
    call canonicalize_dst_path
.done:
    pop di
    pop si
    pop ax
    ret

prepare_src_parent_dir_and_name:
    push ax
    push bx
    push si
    push di
    mov si, src_path
    mov di, exec_restore_path
    mov dx, src_path + 1
.copy:
    lodsb
    stosb
    test al, al
    jz .done
    cmp al, '\'
    je .mark_sep
    cmp al, '/'
    jne .copy
.mark_sep:
    mov dx, si
    jmp .copy
.done:
    mov bx, dx
    sub bx, src_path
    cmp bx, 1
    jbe .root_dir
    dec bx
.root_dir:
    mov byte [exec_restore_path + bx], 0
    pop di
    pop si
    pop bx
    pop ax
    ret

dst_path_has_separator:
    push si
    mov si, dst_path
.scan:
    mov al, [si]
    cmp al, 0
    je .no
    cmp al, '\'
    je .yes
    cmp al, '/'
    je .yes
    cmp al, ':'
    je .yes
    inc si
    jmp .scan
.yes:
    stc
    pop si
    ret
.no:
    clc
    pop si
    ret

build_current_path_in_dst:
    push ax
    push dx
    push si

    mov di, dst_path
    mov byte [di], '\'
    inc di
    xor dl, dl
    mov si, path_buf
    mov ah, 0x47
    int 0x21
    mov si, path_buf
    cmp byte [si], 0
    je .done
    call copy_z_to_di
    mov byte [di], 0
.done:
    pop si
    pop dx
    pop ax
    ret

pop_dst_component:
    cmp di, dst_path + 1
    jbe .root
    dec di
.scan:
    cmp di, dst_path + 1
    jbe .root
    cmp byte [di - 1], '\'
    je .trim
    dec di
    jmp .scan
.trim:
    mov byte [di], 0
    ret
.root:
    mov di, dst_path + 1
    mov byte [di], 0
    ret

; Preserve the actual DOS failure value before text/graphics output changes AX.
print_exec_error:
    push ax
    push bx
    push cx
    push si
    mov bx, ax
    mov si, msg_exec_fail
    call print_dual_dollar_string
    mov cx, 4
.digit:
    rol bx, 4
    mov al, bl
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .emit
    add al, 'A' - '9' - 1
.emit:
    call dual_putc
    loop .digit
    mov si, msg_exec_error_end
    call print_dual_dollar_string
    pop si
    pop cx
    pop bx
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

; Keep BIOS scratch registers and flags out of the DOS command parser and
; EXEC caller. AX is the BIOS result; query-only BX/CX/DX outputs are captured
; separately, so setter calls cannot silently replace live shell registers.
; This path also exists in COMMAND.COM, which does not include the VBE UI.
shell_bios:
    pushf
    pushad
    push ds
    push es
    push fs
    push gs
    int 0x10
    mov [cs:shell_bios_ax],ax
    mov [cs:shell_bios_bx],bx
    mov [cs:shell_bios_cx],cx
    mov [cs:shell_bios_dx],dx
    pop gs
    pop fs
    pop es
    pop ds
    popad
    mov ax,[cs:shell_bios_ax]
    popf
    cld
    ret
shell_bios_ax dw 0
shell_bios_bx dw 0
shell_bios_cx dw 0
shell_bios_dx dw 0

redraw_title_bar:
%ifndef COMMAND_COMPAT
    cmp byte [cs:vc_active], 1
    je vc_title
%endif
    push ax
    push bx
    push cx
    push dx
    push si

    mov ax, 0x0600
    mov bh, TITLE_BAR_ATTR
    xor cx, cx
    mov dx, 0x004F
    call shell_bios

    mov ax, 0x0200
    xor bx, bx
    xor dh, dh
    mov dl, TITLE_BAR_COL
    call shell_bios

    mov si, msg_title_bar
    call print_dual_dollar_string

    mov ax, 0x0200
    xor bx, bx
    mov dh, 1
    xor dl, dl
    call shell_bios

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Preserve an ordinary 80-column, page-zero text child's output. Graphics,
; nonstandard text modes and alternate pages require a fresh mode 03h.
restore_shell_video_state:
    push ax
    push bx
    push cx
    push dx
    push ds
    push es

    mov ah, 0x0F
    call shell_bios
    mov bx,[cs:shell_bios_bx] ; BH is the BIOS active display page
    and al, 0x7F
    cmp al, 3
    jne .reset_mode
    cmp ah, 80
    jne .reset_mode
    or bh, bh
    jnz .reset_mode
    mov ah, 0x03
    xor bx, bx
    call shell_bios
    mov dx,[cs:shell_bios_dx] ; preserve the child's actual text cursor
    jmp .have_cursor
.reset_mode:
    mov ax, 0x0003
    call shell_bios
    mov dx, 0x0100
.have_cursor:
    push dx

    call shell_apply_text_profile

    mov ax, 0x0500
    call shell_bios

    mov ax, 0x0100
    mov cx, 0x0607
    call shell_bios

    mov ax, 0x1003
    xor bx, bx
    call shell_bios

    call redraw_title_bar
    pop dx
    ; Loading a 25-row profile after a 50-row child can shorten the screen.
    mov ax, 0x0040
    mov es, ax
    cmp dh, [es:0x84]
    jbe .cursor_valid
    mov dh, [es:0x84]
.cursor_valid:
    mov ah, 0x02
    xor bx, bx
    call shell_bios
    pop es
    pop ds
    pop dx
    pop cx
    pop bx
    pop ax
%ifndef COMMAND_COMPAT
    ; A program launched from the desktop returns straight to the desktop,
    ; which sets its own mode: a console mode set here would only add two
    ; visible monitor resyncs and a clear before the first desktop frame.
    cmp byte [ui_command_running], 2
    je .console_done
    pushad
    call vc_load_profile
    cmp word [vc_mode], 0
    je .profile_done
    mov byte [vc_import_text], 1
    call vc_begin
    jc .profile_done
    call vc_title
.profile_done:
    popad
.console_done:
%endif
    ret

dual_putc:
    push ax
    push dx

%ifndef COMMAND_COMPAT
    cmp byte [cs:vc_active], 1
    jne .text
    push ax
    mov al, [cs:shell_output_color]
    mov [cs:vc_attr], al
    pop ax
    call vc_putc
    jmp .serial
.text:
    cmp byte [cs:shell_output_color], 7
    je .plain
    cmp al, 32
    jb .plain
    push ax
    push bx
    push cx
    mov ah, 9
    xor bh, bh
    mov bl, [cs:shell_output_color]
    mov cx, 1
    call shell_bios
    pop cx
    pop bx
    pop ax
.plain:
%endif
    mov dl, al
    mov ah, 0x02
    int 0x21

.serial:
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
    mov si, src_path
    mov di, where_name_edit
    call strings_equal
    jz .edit
    mov si, src_path
    mov di, where_name_edit_com
    call strings_equal
    jz .edit
    mov si, src_path
    mov di, where_name_ipconfig
    call strings_equal
    jz .ipconfig
    mov si, src_path
    mov di, where_name_ipconfig_com
    call strings_equal
    jz .ipconfig
    mov si, src_path
    mov di, where_name_ne2000
    call strings_equal
    jz .ne2000
    mov si, src_path
    mov di, where_name_ne2000_com
    call strings_equal
    jz .ne2000
    mov si, src_path
    mov di, where_name_dhcp
    call strings_equal
    jz .dhcp
    mov si, src_path
    mov di, where_name_dhcp_exe
    call strings_equal
    jz .dhcp
    mov si, src_path
    mov di, where_name_ping
    call strings_equal
    jz .ping
    mov si, src_path
    mov di, where_name_ping_exe
    call strings_equal
    jz .ping
    mov si, src_path
    mov di, where_name_ftp
    call strings_equal
    jz .ftp
    mov si, src_path
    mov di, where_name_ftp_exe
    call strings_equal
    jz .ftp
    mov si, src_path
    mov di, where_name_ftpsrv
    call strings_equal
    jz .ftpsrv
    mov si, src_path
    mov di, where_name_ftpsrv_exe
    call strings_equal
    jz .ftpsrv
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
.edit:
    mov si, where_out_edit
    call print_dual_dollar_string
    clc
    ret
.ipconfig:
    mov si, where_out_ipconfig
    jmp .network
.ne2000:
    mov si, where_out_ne2000
    jmp .network
.dhcp:
    mov si, where_out_dhcp
    jmp .network
.ping:
    mov si, where_out_ping
    jmp .network
.ftp:
    mov si, where_out_ftp
    jmp .network
.ftpsrv:
    mov si, where_out_ftpsrv
.network:
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
    jnc .found
.done:
    ; The AX comparisons above change CF. Preserve a failed EXEC as failure
    ; when its error stops the extension search (for example disk error 5).
    stc
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
    jnc .found
.done:
    stc
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

startup_display_services:
    push ax
    push bx
    push dx
    push ds
    push es
    push cs
    pop ds
    push cs
    pop es
    call setup_exec_block

    mov byte [exec_tail], 0
    mov byte [exec_tail + 1], 0x0D
    mov dx, startup_auxstack_path
    mov bx, exec_env_seg
    mov ax, 0x4B00
    int 0x21
    jc .probe
    mov ax, 0x4D00
    int 0x21

.probe:
.sound_done:
    call shell_apply_text_profile
    ; ui_video_begin selects graphics once; do not set and clear VBE twice.
.done:
    pop es
    pop ds
    pop dx
    pop bx
    pop ax
    ret

startup_play_sound:
%ifndef COMMAND_COMPAT
    cmp byte [driver_startup_audio],0
    je .skip
%endif
    pushad
    push ds
    push es
    push cs
    pop ds
    push cs
    pop es
    call setup_exec_block
    mov byte [exec_tail], 3
    mov word [exec_tail + 1], ' /'
    mov word [exec_tail + 3], 0x0D51 ; Q, CR
    mov dx, startup_sound_path
    mov bx, exec_env_seg
    mov ax, 0x4B00
    int 0x21
    jc .sound_done
    mov ax, 0x4D00
    int 0x21
.sound_done:
    pop es
    pop ds
    popad
.skip:
    ret

shell_apply_text_profile:
    push ax
    push bx
    push cx
    push dx
    push ds
    push cs
    pop ds
    mov word [shell_text_profile], '25'
    mov dx, shell_text_profile_path
    mov ax, 0x3D00
    int 0x21
    jc .apply
    mov bx, ax
    mov dx, shell_text_profile
    mov cx, 2
    mov ah, 0x3F
    int 0x21
    pushf
    push ax
    mov ah, 0x3E
    int 0x21
    pop ax
    popf
    jc .default
    cmp ax, 2
    je .apply
.default:
    mov word [shell_text_profile], '25'
.apply:
    cmp word [shell_text_profile], '50'
    je .rows_50
    mov ax, 0x1114
    jmp .set_font
.rows_50:
    mov ax, 0x1112
.set_font:
    xor bx, bx
    call shell_bios
    pop ds
    pop dx
    pop cx
    pop bx
    pop ax
    ret

exec_run_candidate:
%ifndef COMMAND_COMPAT
    ; DOS children own their hardware mode. Restore the shared console once
    ; EXEC returns, including search failures; keep their text output.
    cmp byte [cs:vc_active], 1
    jne .native
    call vc_end
.native:
    ; The return path redraws the title over row 0. On a fresh text screen
    ; start the child on row 1 so its first output line survives.
    push ax
    push bx
    push cx
    push dx
    mov ah, 0x03
    xor bh, bh
    call shell_bios
    mov dx,[cs:shell_bios_dx]
    test dx, dx
    jnz .cursor_ready
    mov ah, 0x02
    mov dh, 1
    call shell_bios
.cursor_ready:
    pop dx
    pop cx
    pop bx
    pop ax
%endif
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
    mov si, src_path
    mov di, where_name_edit
    call strings_equal
    jz .edit
    mov si, src_path
    mov di, where_name_edit_com
    call strings_equal
    jz .edit
    mov si, src_path
    mov di, where_name_costa
    call strings_equal
    jz .costa
    mov si, src_path
    mov di, where_name_costa_exe
    call strings_equal
    jz .costa
    mov si, src_path
    mov di, where_name_ipconfig
    call strings_equal
    jz .ipconfig
    mov si, src_path
    mov di, where_name_ipconfig_com
    call strings_equal
    jz .ipconfig
    mov si, src_path
    mov di, where_name_ne2000
    call strings_equal
    jz .ne2000
    mov si, src_path
    mov di, where_name_ne2000_com
    call strings_equal
    jz .ne2000
    mov si, src_path
    mov di, where_name_dhcp
    call strings_equal
    jz .dhcp
    mov si, src_path
    mov di, where_name_dhcp_exe
    call strings_equal
    jz .dhcp
    mov si, src_path
    mov di, where_name_ping
    call strings_equal
    jz .ping
    mov si, src_path
    mov di, where_name_ping_exe
    call strings_equal
    jz .ping
    mov si, src_path
    mov di, where_name_ftp
    call strings_equal
    jz .ftp
    mov si, src_path
    mov di, where_name_ftp_exe
    call strings_equal
    jz .ftp
    mov si, src_path
    mov di, where_name_ftpsrv
    call strings_equal
    jz .ftpsrv
    mov si, src_path
    mov di, where_name_ftpsrv_exe
    call strings_equal
    jz .ftpsrv
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
    jmp .run
.edit:
    mov si, exec_path_apps_ciukedit
    jmp .run
.costa:
    mov si, exec_dir_costa
    call exec_try_in_dir
    ret
.ipconfig:
    mov si, exec_path_net_ipconfig
    jmp .run
.ne2000:
    mov si, exec_path_net_ne2000
    jmp .run
.dhcp:
    mov si, exec_path_net_dhcp
    jmp .run
.ping:
    mov si, exec_path_net_ping
    jmp .run
.ftp:
    mov si, exec_path_net_ftp
    jmp .run
.ftpsrv:
    mov si, exec_path_net_ftpsrv
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
    ; INT 19h only reloads the bootstrap loader.  It does not reset the PCI
    ; devices/chipset and on real ThinkPads leaves the machine half alive.
    ; Ask the chipset for a system reset first, then use the legacy 8042
    ; pulse and a triple fault as progressively more generic fallbacks.
    cli
    xor ax, ax
    mov ds, ax
    mov word [0x0472], ax              ; cold POST, never resume stage 1 state

    mov dx, 0x0CF9
    mov al, 0x02                       ; reset CPU request
    out dx, al
    or al, 0x04                        ; system reset (ICH3-M and compatibles)
    out dx, al

    mov cx, 0x1000
.wait_8042:
    in al, 0x64
    test al, 0x02                      ; controller input buffer busy?
    jz .pulse_8042
    loop .wait_8042
    jmp .triple_fault
.pulse_8042:
    mov al, 0xFE                       ; pulse RESET# low
    out 0x64, al
    mov cx, 0x1000
.wait_reset:
    nop
    loop .wait_reset

.triple_fault:
    lidt [cs:.null_idt]
    int 3
    jmp 0xFFFF:0x0000                  ; final BIOS-entry fallback
.null_idt:
    dw 0
    dd 0

shutdown_system:
    push cs
    pop ds
    call apm_shutdown_system
    jnc .halt_now
    mov si, msg_shutdown_unavailable
    call print_dual_dollar_string

.halt_now:
    xor ax, ax
    mov cx, ax
    mov dx, ax
    mov sp, 0xFFFC
    cli
    hlt
    jmp shutdown_system

apm_shutdown_system:
    push bx
    push cx
    push dx

    mov ax, 0x5300
    xor bx, bx
    int 0x15
    jc .fail
    cmp bx, 0x504D
    jne .fail

    mov ax, 0x5301
    xor bx, bx
    int 0x15
    jc .fail

    mov ax, 0x530E
    xor bx, bx
    mov cx, 0x0102
    int 0x15

    mov ax, 0x5308
    mov bx, 0x0001
    mov cx, 0x0001
    int 0x15
    jc .disconnect_fail

    mov ax, 0x5307
    mov bx, 0x0001
    mov cx, 0x0003
    int 0x15
    jc .disconnect_fail

    clc
    jmp .disconnect

.disconnect_fail:
    stc

.disconnect:
    pushf
    mov ax, 0x5304
    xor bx, bx
    int 0x15
    popf
    jmp .done

.fail:
    stc

.done:
    pop dx
    pop cx
    pop bx
    ret

msg_title_bar db 'CiukiOS pre-Alpha v0.7.1', 0x0D, 0x0A, '$'
startup_sound_path db '\SYSTEM\BOOTSND.COM', 0
msg_banner_body db 'HELP lists commands. WHERE shows launch targets.', 0x0D, 0x0A
                db 'Try REBOOT 5 or SHUTDOWN 5 for queued power actions.', 0x0D, 0x0A, '$'
msg_prompt_pre db 'CiukiOS SHELL ', '$'
msg_help    db '+------------------------ CiukiOS command guide -------------------------+', 0x0D, 0x0A
            db '| SYSTEM     HELP  VER  ECHO  CLS/CLEAR  REBOOT  SHUTDOWN                |', 0x0D, 0x0A
            db '| NAVIGATION CD/CHDIR  DIR  PWD  PATH  WHERE <name>                      |', 0x0D, 0x0A
            db '| FILES      TYPE  COPY  DEL/ERASE  REN/RENAME/MOVE                      |', 0x0D, 0x0A
            db '| EDITOR     EDIT [file]       full-screen editor; F1 shows shortcuts    |', 0x0D, 0x0A
            db '| DIRECTORIES MKDIR/MD  RMDIR/RD                                         |', 0x0D, 0x0A
            db '| PROGRAMS   <name> [args] or RUN <name/path> [args]                     |', 0x0D, 0x0A
            db '+------------------------------- Network --------------------------------+', 0x0D, 0x0A
            db '| 1. NETSTART                start NIC + permanent ARP/ICMP service      |', 0x0D, 0x0A
            db '| 2. IPCONFIG                show IPv4 values and resident ICMP status   |', 0x0D, 0x0A
            db '| 3. NETCFG STATIC <ip> <mask> <gateway> <dns>                           |', 0x0D, 0x0A
            db '|                            save; live-apply when NETSTART is active    |', 0x0D, 0x0A
            db '| 4. NETCFG DHCP             request lease and reload resident ICMP     |', 0x0D, 0x0A
            db '| 5. PING <host>             send ICMP echo (gateway or Internet)        |', 0x0D, 0x0A
            db '| 6. FTP <host> / FTPSRV     FTP client / start C:\SHARE server         |', 0x0D, 0x0A
            db '|    PKTCHK / PKTTOOL        Packet Driver diagnostics                   |', 0x0D, 0x0A
            db '|    ICMP remains active after FTPSRV stops; TAP enables host ping       |', 0x0D, 0x0A
            db '+------------------------------------------------------------------------+', 0x0D, 0x0A
            db 'Power queue: SHUTDOWN/REBOOT <seconds|STATUS|CANCEL>. EXIT is disabled.', 0x0D, 0x0A, '$'
msg_ver     db 'CiukiOS pre-Alpha v0.7.1', 0x0D, 0x0A, '$'
msg_unknown db 'command: not found', 0x0D, 0x0A, '$'
msg_exit_disabled db 'exit/quit is not available in loader-only mode', 0x0D, 0x0A
                  db 'use reboot or shutdown', 0x0D, 0x0A, '$'
msg_exec_not_found db 'command: not found', 0x0D, 0x0A, '$'
msg_exec_fail db 'exec: cannot execute (DOS error ', '$'
msg_exec_error_end db ')', 0x0D, 0x0A, '$'
msg_exec_bad_format db 'exec: unsupported executable format', 0x0D, 0x0A, '$'
msg_exec_no_mem db 'exec: insufficient memory', 0x0D, 0x0A, '$'
msg_path    db 'C:\APPS;C:\NET;C:\SYSTEM\DRIVERS;C:\SYSTEM', 0x0D, 0x0A, '$'
msg_run_use db 'usage: run <name/path>', 0x0D, 0x0A, '$'
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
msg_shutdown_now db 'shutting down...', 0x0D, 0x0A, '$'
msg_shutdown_unavailable db 'ACPI/APM shutdown is not available. System halted.', 0x0D, 0x0A, '$'
msg_power_cancel db 'shutdown: canceled', 0x0D, 0x0A, '$'
msg_power_status_none db 'shutdown: idle', 0x0D, 0x0A, '$'
msg_power_status_reboot db 'shutdown: pending reboot', 0x0D, 0x0A, '$'
msg_power_status_shutdown db 'shutdown: pending halt', 0x0D, 0x0A, '$'
msg_ctrl_c  db '^C', 0x0D, 0x0A, '$'
msg_crlf    db 0x0D, 0x0A, '$'
shell_path_apps db '\APPS\', 0
shell_path_net db '\NET\', 0
shell_path_drivers db '\SYSTEM\DRIVERS\', 0
shell_path_system db '\SYSTEM\', 0
exec_path_apps db 'C:\APPS\', 0
exec_path_drivers db 'C:\SYSTEM\DRIVERS\', 0
exec_path_system db 'C:\SYSTEM\', 0
exec_dir_apps db '\APPS', 0
exec_dir_net db '\NET', 0
exec_dir_drivers db '\SYSTEM\DRIVERS', 0
exec_dir_system db '\SYSTEM', 0
exec_dir_costa db '\APPS\COSTA', 0
ext_none db 0
ext_com db '.COM', 0
ext_exe db '.EXE', 0
where_name_shell db 'SHELL', 0
where_name_shell_com db 'SHELL.COM', 0
where_name_dos4gw db 'DOS4GW', 0
where_name_dos4gw_exe db 'DOS4GW.EXE', 0
where_name_mouse db 'MOUSE', 0
where_name_mouse_com db 'MOUSE.COM', 0
where_name_edit db 'EDIT', 0
where_name_edit_com db 'EDIT.COM', 0
where_name_costa db 'COSTA', 0
where_name_costa_exe db 'COSTA.EXE', 0
where_name_ipconfig db 'IPCONFIG', 0
where_name_ipconfig_com db 'IPCONFIG.COM', 0
where_name_ne2000 db 'NE2000', 0
where_name_ne2000_com db 'NE2000.COM', 0
where_name_dhcp db 'DHCP', 0
where_name_dhcp_exe db 'DHCP.EXE', 0
where_name_ping db 'PING', 0
where_name_ping_exe db 'PING.EXE', 0
where_name_ftp db 'FTP', 0
where_name_ftp_exe db 'FTP.EXE', 0
where_name_ftpsrv db 'FTPSRV', 0
where_name_ftpsrv_exe db 'FTPSRV.EXE', 0
power_token_cancel db 'CANCEL', 0
power_token_status db 'STATUS', 0
power_token_timer db '/T', 0
where_out_shell db 'C:\SYSTEM\SHELL.COM', 0x0D, 0x0A, '$'
where_out_dos4gw db 'C:\SYSTEM\DRIVERS\DOS4GW.EXE', 0x0D, 0x0A, '$'
where_out_mouse db 'C:\SYSTEM\MOUSE.COM', 0x0D, 0x0A, '$'
where_out_edit db 'C:\APPS\CIUKEDIT.COM', 0x0D, 0x0A, '$'
where_out_ipconfig db 'C:\NET\IPCONFIG.COM', 0x0D, 0x0A, '$'
where_out_ne2000 db 'C:\NET\NE2000.COM', 0x0D, 0x0A, '$'
where_out_dhcp db 'C:\NET\DHCP.EXE', 0x0D, 0x0A, '$'
where_out_ping db 'C:\NET\PING.EXE', 0x0D, 0x0A, '$'
where_out_ftp db 'C:\NET\FTP.EXE', 0x0D, 0x0A, '$'
where_out_ftpsrv db 'C:\NET\FTPSRV.EXE', 0x0D, 0x0A, '$'
exec_path_system_shell db 'C:\SYSTEM\SHELL.COM', 0
exec_path_drivers_dos4gw db 'C:\SYSTEM\DRIVERS\DOS4GW.EXE', 0
exec_path_system_mouse db 'C:\SYSTEM\MOUSE.COM', 0
exec_path_apps_ciukedit db 'C:\APPS\CIUKEDIT.COM', 0
exec_path_net_ipconfig db 'C:\NET\IPCONFIG.COM', 0
exec_path_net_ne2000 db 'C:\NET\NE2000.COM', 0
exec_path_net_dhcp db 'C:\NET\DHCP.EXE', 0
exec_path_net_ping db 'C:\NET\PING.EXE', 0
exec_path_net_ftp db 'C:\NET\FTP.EXE', 0
exec_path_net_ftpsrv db 'C:\NET\FTPSRV.EXE', 0
startup_auxstack_path db '\SYSTEM\VIDEO\AUXSTACK.COM', 0
shell_text_profile_path db '\SYSTEM\VIDEO\VGASET.CFG', 0
shell_text_profile dw '25'

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
cmd_start dw 0
input_buf times 127 db 0
input_draw_len db 0
%ifdef COMMAND_COMPAT
command_once_pending db 0
command_once_done db 0
command_keep_open db 0
command_window db 0
command_exit_code db 0
command_once_length db 0
%endif
history_count db 0
history_next db 0
history_nav db 0xFF
%ifdef COMMAND_COMPAT
history_buf times HISTORY_MAX * HISTORY_ENTRY_LEN db 0
%else
history_buf equ 0
history_segment dw 0
%endif
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

%ifndef COMMAND_COMPAT
shell_output_color db 7
%include "src/com/shell_desktop.inc"
%include "src/com/ui_theme.inc"
%include "src/com/vbe_console.inc"
%include "src/com/shell_gui.inc"
%include "src/com/shell_drivers.inc"
%include "src/com/boot_session.inc"
%endif

align 16
shell_stack times 2048 db 0
shell_stack_top:
shell_image_end:

%if ($-$$+0x100) > 0xEF00
%error "SHELL.COM overlaps its DOS arena ceiling"
%endif
