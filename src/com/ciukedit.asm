bits 16
org 0x0100

; CiukiEDIT - full-screen text editor for CiukiOS DOS.
; The document lives in a separately allocated 32 KiB DOS block, so the COM
; image stays small and returns its memory cleanly to the parent shell.

%define BUFFER_PARAS       0x0800
%define BUFFER_CAPACITY    32760
%define VIEW_TOP           2
%define VIEW_ROWS          21
%define TEXT_LEFT          6
%define TEXT_WIDTH         74

%define ATTR_TEXT          0x07
%define ATTR_TITLE         0x1F
%define ATTR_MENU          0x3F
%define ATTR_GUTTER        0x1B
%define ATTR_STATUS        0x70
%define ATTR_OK            0x2F
%define ATTR_WARNING       0x6F
%define ATTR_ERROR         0x4F

%define STATUS_NORMAL      0
%define STATUS_OK          1
%define STATUS_WARNING     2
%define STATUS_ERROR       3

start:
    cli
    mov ax, cs
    mov ss, ax
    mov sp, editor_stack_top
    sti

    push cs
    pop ds
    mov es, ax
    mov bx, ((editor_image_end - $$ + 0x0100) + 15) >> 4
    mov ah, 0x4A
    int 0x21
    jc fatal_memory

    mov bx, BUFFER_PARAS
    mov ah, 0x48
    int 0x21
    jc fatal_memory
    mov [buffer_segment], ax

    call parse_filename
    mov si, serial_boot
    call serial_print_z
    call load_document
    jc fatal_open

    mov ax, 0x0003
    int 0x10
    call render_all

editor_loop:
    xor ah, ah
    int 0x16
    cmp al, 0
    je handle_extended
    cmp al, 0xE0
    je handle_extended

    cmp al, 0x1B
    je request_exit
    cmp al, 0x13                    ; Ctrl+S
    je command_save
    cmp al, 0x11                    ; Ctrl+Q
    je request_exit
    cmp al, 0x0D
    je key_enter
    cmp al, 0x08
    je key_backspace
    cmp al, 0x09
    je key_tab
    cmp al, 0x20
    jb editor_loop
    cmp al, 0x7E
    ja editor_loop
    mov byte [exit_armed], 0
    call insert_character
    call render_all
    jmp editor_loop

handle_extended:
    cmp ah, 0x44                    ; F10 keeps the two-press discard state
    je request_exit
    mov byte [exit_armed], 0
    cmp ah, 0x3B                    ; F1
    je command_help
    cmp ah, 0x3C                    ; F2
    je command_save
    cmp ah, 0x3D                    ; F3
    je command_save_as
    cmp ah, 0x47                    ; Home
    je key_home
    cmp ah, 0x48                    ; Up
    je key_up
    cmp ah, 0x49                    ; Page Up
    je key_page_up
    cmp ah, 0x4B                    ; Left
    je key_left
    cmp ah, 0x4D                    ; Right
    je key_right
    cmp ah, 0x4F                    ; End
    je key_end
    cmp ah, 0x50                    ; Down
    je key_down
    cmp ah, 0x51                    ; Page Down
    je key_page_down
    cmp ah, 0x52                    ; Insert
    je key_insert_mode
    cmp ah, 0x53                    ; Delete
    je key_delete
    jmp editor_loop

command_help:
    call show_help
    call render_all
    jmp editor_loop

command_save:
    mov byte [exit_armed], 0
    call save_document
    call render_all
    jmp editor_loop

command_save_as:
    mov byte [exit_armed], 0
    call save_as_prompt
    call render_all
    jmp editor_loop

request_exit:
    cmp byte [dirty_flag], 0
    je exit_editor
    cmp byte [exit_armed], 0
    jne exit_editor
    mov byte [exit_armed], 1
    mov byte [status_kind], STATUS_WARNING
    mov word [status_message], status_unsaved
    call render_all
    jmp editor_loop

exit_editor:
    mov ax, 0x0003
    int 0x10
    mov si, serial_ok
    call print_dos_string
    mov es, [buffer_segment]
    mov ah, 0x49
    int 0x21
    mov ax, 0x4C00
    int 0x21

fatal_memory:
    push cs
    pop ds
    mov dx, error_memory
    jmp fatal_exit

fatal_open:
    mov es, [buffer_segment]
    mov ah, 0x49
    int 0x21
    push cs
    pop ds
    mov dx, error_open
fatal_exit:
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C01
    int 0x21

; ---------------------------------------------------------------------------
; Command line and file I/O

parse_filename:
    mov si, 0x0081
    xor cx, cx
    mov cl, [0x0080]
.skip:
    jcxz .default
    cmp byte [si], ' '
    jne .begin
    inc si
    dec cx
    jmp .skip
.begin:
    mov di, filename
    mov dl, 0
    cmp byte [si], '"'
    jne .copy
    mov dl, '"'
    inc si
    dec cx
.copy:
    jcxz .done
    lodsb
    dec cx
    cmp dl, '"'
    je .quoted
    cmp al, ' '
    je .done
    jmp .store
.quoted:
    cmp al, '"'
    je .done
.store:
    cmp di, filename + 63
    jae .copy
    stosb
    jmp .copy
.done:
    mov byte [di], 0
    cmp di, filename
    jne .return
.default:
    mov si, default_filename
    mov di, filename
.default_copy:
    lodsb
    stosb
    test al, al
    jnz .default_copy
.return:
    ret

load_document:
    mov word [text_length], 0
    mov word [cursor_offset], 0
    mov dx, filename
    mov ax, 0x3D00
    int 0x21
    jnc .opened
    cmp ax, 2
    je .new_file
    cmp ax, 3
    je .new_file
    stc
    ret
.new_file:
    mov byte [file_exists], 0
    mov byte [status_kind], STATUS_NORMAL
    mov word [status_message], status_new
    mov si, serial_new
    call serial_print_z
    clc
    ret
.opened:
    mov byte [file_exists], 1
    mov [file_handle], ax
.read_loop:
    mov ax, BUFFER_CAPACITY
    sub ax, [text_length]
    jz .check_oversize
    cmp ax, 512
    jbe .read_count_ok
    mov ax, 512
.read_count_ok:
    mov cx, ax
    mov dx, [text_length]
    push ds
    mov ax, [buffer_segment]
    mov ds, ax
    mov bx, [cs:file_handle]
    mov ah, 0x3F
    int 0x21
    pop ds
    jc .read_error
    test ax, ax
    jz .read_done
    add [text_length], ax
    jmp .read_loop
.check_oversize:
    mov bx, [file_handle]
    mov cx, 1
    mov dx, io_buffer
    mov ah, 0x3F
    int 0x21
    jc .read_error
    test ax, ax
    jnz .too_large
.read_done:
    mov bx, [file_handle]
    mov ah, 0x3E
    int 0x21
    call normalize_newlines
    mov byte [status_kind], STATUS_OK
    mov word [status_message], status_loaded
    mov si, serial_open
    call serial_print_z
    clc
    ret
.read_error:
    mov bx, [file_handle]
    mov ah, 0x3E
    int 0x21
    stc
    ret
.too_large:
    mov bx, [file_handle]
    mov ah, 0x3E
    int 0x21
    mov dx, error_large
    mov ah, 0x09
    int 0x21
    stc
    ret

normalize_newlines:
    push ds
    mov ax, [buffer_segment]
    mov ds, ax
    mov es, ax
    xor si, si
    xor di, di
    mov cx, [cs:text_length]
.next:
    jcxz .done
    lodsb
    dec cx
    cmp al, 0x0D
    jne .store
    cmp cx, 0
    je .cr_only
    cmp byte [si], 0x0A
    jne .cr_only
    jmp .next                       ; discard CR, next pass stores LF
.cr_only:
    mov al, 0x0A
.store:
    stosb
    jmp .next
.done:
    mov [cs:text_length], di
    pop ds
    push cs
    pop es
    ret

save_document:
    mov dx, filename
    xor cx, cx
    mov ah, 0x3C
    int 0x21
    jc .create_error
    mov [file_handle], ax
    mov word [io_count], 0
    xor si, si
.convert:
    cmp si, [text_length]
    jae .finish
    mov es, [buffer_segment]
    mov al, [es:si]
    inc si
    cmp al, 0x0A
    jne .put_one
    mov al, 0x0D
    call save_put_byte
    jc .write_error
    mov al, 0x0A
.put_one:
    call save_put_byte
    jc .write_error
    jmp .convert
.finish:
    call flush_io_buffer
    jc .write_error
    mov bx, [file_handle]
    mov ah, 0x3E
    int 0x21
    mov byte [dirty_flag], 0
    mov byte [file_exists], 1
    mov byte [status_kind], STATUS_OK
    mov word [status_message], status_saved
    mov si, serial_saved
    call serial_print_z
    clc
    ret
.write_error:
    mov bx, [file_handle]
    mov ah, 0x3E
    int 0x21
.create_error:
    mov byte [status_kind], STATUS_ERROR
    mov word [status_message], status_save_error
    stc
    ret

save_put_byte:
    push bx
    mov bx, [io_count]
    mov [io_buffer + bx], al
    inc bx
    mov [io_count], bx
    cmp bx, 512
    jb .ok
    call flush_io_buffer
    pop bx
    ret
.ok:
    pop bx
    clc
    ret

flush_io_buffer:
    push ax
    push bx
    push cx
    push dx
    mov cx, [io_count]
    jcxz .ok
    mov bx, [file_handle]
    mov dx, io_buffer
    mov ah, 0x40
    int 0x21
    jc .fail
    cmp ax, cx
    jne .fail
    mov word [io_count], 0
.ok:
    clc
    jmp .done
.fail:
    stc
.done:
    pop dx
    pop cx
    pop bx
    pop ax
    ret

save_as_prompt:
    mov si, filename
    mov di, prompt_filename
    xor cx, cx
.copy_name:
    lodsb
    stosb
    test al, al
    jz .copied
    inc cx
    cmp cx, 63
    jb .copy_name
    mov byte [di - 1], 0
.copied:
    mov [prompt_length], cx
.redraw:
    mov dh, 23
    mov bl, ATTR_MENU
    call fill_row
    mov dh, 23
    mov dl, 1
    mov bl, ATTR_MENU
    mov cx, 12
    mov si, label_save_as
    call putz_limit
    mov dh, 23
    mov dl, 12
    mov bl, ATTR_MENU
    mov cx, 63
    mov si, prompt_filename
    call putz_limit
    mov dx, [prompt_length]
    add dl, 12
    mov dh, 23
    mov bh, 0
    mov ah, 0x02
    int 0x10
.input:
    xor ah, ah
    int 0x16
    cmp al, 0x1B
    je .cancel
    cmp al, 0x0D
    je .accept
    cmp al, 0x08
    je .backspace
    cmp al, 0x20
    jb .input
    cmp al, 0x7E
    ja .input
    mov bx, [prompt_length]
    cmp bx, 63
    jae .input
    mov [prompt_filename + bx], al
    inc bx
    mov [prompt_length], bx
    mov byte [prompt_filename + bx], 0
    jmp .redraw
.backspace:
    mov bx, [prompt_length]
    test bx, bx
    jz .input
    dec bx
    mov [prompt_length], bx
    mov byte [prompt_filename + bx], 0
    jmp .redraw
.accept:
    cmp word [prompt_length], 0
    je .input
    mov si, prompt_filename
    mov di, filename
.install:
    lodsb
    stosb
    test al, al
    jnz .install
    call save_document
    ret
.cancel:
    mov byte [status_kind], STATUS_NORMAL
    mov word [status_message], status_save_cancel
    ret

; ---------------------------------------------------------------------------
; Editing primitives

mark_changed:
    mov byte [dirty_flag], 1
    mov byte [exit_armed], 0
    mov byte [status_kind], STATUS_NORMAL
    mov word [status_message], status_editing
    ret

insert_character:
    push ax
    cmp byte [insert_mode], 0
    jne .insert
    mov bx, [cursor_offset]
    cmp bx, [text_length]
    jae .insert
    mov es, [buffer_segment]
    cmp byte [es:bx], 0x0A
    je .insert
    pop ax
    mov [es:bx], al
    inc word [cursor_offset]
    call mark_changed
    ret
.insert:
    cmp word [text_length], BUFFER_CAPACITY
    jae .full
    mov es, [buffer_segment]
    mov di, [text_length]
    mov bx, [cursor_offset]
.shift_right:
    cmp di, bx
    jbe .place
    mov dl, [es:di - 1]
    mov [es:di], dl
    dec di
    jmp .shift_right
.place:
    pop ax
    mov [es:bx], al
    inc word [text_length]
    inc word [cursor_offset]
    call mark_changed
    ret
.full:
    pop ax
    mov byte [status_kind], STATUS_ERROR
    mov word [status_message], status_full
    ret

delete_at_cursor:
    mov bx, [cursor_offset]
    cmp bx, [text_length]
    jae .done
    mov es, [buffer_segment]
    mov di, bx
    inc bx
.shift_left:
    cmp bx, [text_length]
    jae .shrunk
    mov al, [es:bx]
    mov [es:di], al
    inc bx
    inc di
    jmp .shift_left
.shrunk:
    dec word [text_length]
    call mark_changed
.done:
    ret

key_enter:
    mov al, 0x0A
    call insert_character
    call render_all
    jmp editor_loop

key_tab:
    call calc_cursor_position
    mov ax, [cursor_column]
    and ax, 3
    mov cx, 4
    sub cx, ax
.spaces:
    mov al, ' '
    call insert_character
    loop .spaces
    call render_all
    jmp editor_loop

key_backspace:
    cmp word [cursor_offset], 0
    je .render
    dec word [cursor_offset]
    call delete_at_cursor
.render:
    call render_all
    jmp editor_loop

key_delete:
    call delete_at_cursor
    call render_all
    jmp editor_loop

key_left:
    cmp word [cursor_offset], 0
    je .render
    dec word [cursor_offset]
.render:
    call render_all
    jmp editor_loop

key_right:
    mov ax, [cursor_offset]
    cmp ax, [text_length]
    jae .render
    inc word [cursor_offset]
.render:
    call render_all
    jmp editor_loop

key_home:
    mov bx, [cursor_offset]
    mov es, [buffer_segment]
.scan:
    test bx, bx
    jz .set
    cmp byte [es:bx - 1], 0x0A
    je .set
    dec bx
    jmp .scan
.set:
    mov [cursor_offset], bx
    call render_all
    jmp editor_loop

key_end:
    mov bx, [cursor_offset]
    mov es, [buffer_segment]
.scan:
    cmp bx, [text_length]
    jae .set
    cmp byte [es:bx], 0x0A
    je .set
    inc bx
    jmp .scan
.set:
    mov [cursor_offset], bx
    call render_all
    jmp editor_loop

key_up:
    call move_vertical_up
    call render_all
    jmp editor_loop

key_down:
    call move_vertical_down
    call render_all
    jmp editor_loop

key_page_up:
    mov cx, VIEW_ROWS
.loop:
    call move_vertical_up
    loop .loop
    call render_all
    jmp editor_loop

key_page_down:
    mov cx, VIEW_ROWS
.loop:
    call move_vertical_down
    loop .loop
    call render_all
    jmp editor_loop

key_insert_mode:
    xor byte [insert_mode], 1
    mov byte [status_kind], STATUS_NORMAL
    mov word [status_message], status_mode
    call render_all
    jmp editor_loop

move_vertical_up:
    call current_line_start_and_column
    test bx, bx
    jz .done
    dec bx
    mov es, [buffer_segment]
.previous_start:
    test bx, bx
    jz .target
    cmp byte [es:bx - 1], 0x0A
    je .target
    dec bx
    jmp .previous_start
.target:
    mov ax, dx
    call clamp_column_from_bx
    mov [cursor_offset], bx
.done:
    ret

move_vertical_down:
    call current_line_start_and_column
    mov es, [buffer_segment]
.find_end:
    cmp bx, [text_length]
    jae .done
    cmp byte [es:bx], 0x0A
    je .next_start
    inc bx
    jmp .find_end
.next_start:
    inc bx
    mov ax, dx
    call clamp_column_from_bx
    mov [cursor_offset], bx
.done:
    ret

current_line_start_and_column:
    mov bx, [cursor_offset]
    mov dx, bx
    mov es, [buffer_segment]
.scan:
    test bx, bx
    jz .done
    cmp byte [es:bx - 1], 0x0A
    je .done
    dec bx
    jmp .scan
.done:
    sub dx, bx
    ret

; BX = target line start, AX = desired column. Returns BX clamped.
clamp_column_from_bx:
    mov es, [buffer_segment]
.advance:
    test ax, ax
    jz .done
    cmp bx, [text_length]
    jae .done
    cmp byte [es:bx], 0x0A
    je .done
    inc bx
    dec ax
    jmp .advance
.done:
    ret

; ---------------------------------------------------------------------------
; Screen rendering

render_all:
    call calc_cursor_position
    call calc_total_lines
    call ensure_cursor_visible
    call clear_screen
    call render_title
    call render_menu
    call render_document
    call render_status
    call render_footer
    call position_cursor
    ret

clear_screen:
    push ax
    push cx
    push di
    push es
    mov ax, 0xB800
    mov es, ax
    xor di, di
    mov ax, (ATTR_TEXT << 8) | ' '
    mov cx, 2000
    rep stosw
    pop es
    pop di
    pop cx
    pop ax
    ret

render_title:
    mov dh, 0
    mov bl, ATTR_TITLE
    call fill_row
    mov dh, 0
    mov dl, 2
    mov bl, ATTR_TITLE
    mov cx, 18
    mov si, title_brand
    call putz_limit
    mov dh, 0
    mov dl, 21
    mov bl, ATTR_TITLE
    mov cx, 54
    mov si, filename
    call putz_limit
    cmp byte [dirty_flag], 0
    je .clean
    mov dh, 0
    mov dl, 77
    mov bl, ATTR_WARNING
    mov al, '*'
    call put_char
    ret
.clean:
    mov dh, 0
    mov dl, 76
    mov bl, ATTR_TITLE
    mov al, 'O'
    call put_char
    mov dl, 77
    mov al, 'K'
    call put_char
    ret

render_menu:
    mov dh, 1
    mov bl, ATTR_MENU
    call fill_row
    mov dh, 1
    mov dl, 1
    mov bl, ATTR_MENU
    mov cx, 78
    mov si, menu_text
    call putz_limit
    ret

render_document:
    mov ax, [top_line]
    call find_line_start
    mov [render_offset], si
    mov ax, [top_line]
    inc ax
    mov [render_line_number], ax
    mov byte [render_row], VIEW_TOP
.row_loop:
    mov dh, [render_row]
    cmp dh, VIEW_TOP + VIEW_ROWS
    jae .done
    mov bl, ATTR_TEXT
    call fill_row
    mov dh, [render_row]
    mov dl, 0
    mov bl, ATTR_GUTTER
    mov cx, 6
    call fill_cells

    mov dh, [render_row]
    mov dl, 5
    mov bl, ATTR_GUTTER
    mov al, 179
    call put_char

    mov ax, [render_line_number]
    cmp ax, [total_line_count]
    jbe .valid_line
    mov dh, [render_row]
    mov dl, 3
    mov bl, ATTR_GUTTER
    mov al, '~'
    call put_char
    jmp .advance_row
.valid_line:
    call format_u16
    mov ax, 5
    sub ax, cx
    mov dl, al
    mov dh, [render_row]
    mov bl, ATTR_GUTTER
    call putz_limit

    mov si, [render_offset]
    xor bx, bx                       ; logical column
    xor cx, cx                       ; visible characters
    mov es, [buffer_segment]
.char_loop:
    cmp si, [text_length]
    jae .line_done
    mov al, [es:si]
    cmp al, 0x0A
    je .consume_lf
    inc si
    cmp bx, [horizontal_scroll]
    jb .skip_visible
    cmp cx, TEXT_WIDTH
    jae .skip_visible
    cmp al, 0x20
    jae .printable
    mov al, 0xFA
.printable:
    mov dh, [render_row]
    mov dl, TEXT_LEFT
    add dl, cl
    mov bl, ATTR_TEXT
    call put_char
    inc cx
.skip_visible:
    inc bx
    jmp .char_loop
.consume_lf:
    inc si
.line_done:
    mov [render_offset], si
.advance_row:
    inc word [render_line_number]
    inc byte [render_row]
    jmp .row_loop
.done:
    ret

render_status:
    mov bl, ATTR_STATUS
    cmp byte [status_kind], STATUS_OK
    jne .not_ok
    mov bl, ATTR_OK
    jmp .draw
.not_ok:
    cmp byte [status_kind], STATUS_WARNING
    jne .not_warning
    mov bl, ATTR_WARNING
    jmp .draw
.not_warning:
    cmp byte [status_kind], STATUS_ERROR
    jne .draw
    mov bl, ATTR_ERROR
.draw:
    mov dh, 23
    call fill_row
    mov dh, 23
    mov dl, 1
    mov cx, 78
    mov si, [status_message]
    call putz_limit
    ret

render_footer:
    mov dh, 24
    mov bl, ATTR_TITLE
    call fill_row
    mov dh, 24
    mov dl, 1
    mov bl, ATTR_TITLE
    mov cx, 45
    mov si, footer_text
    call putz_limit

    mov dh, 24
    mov dl, 47
    mov cx, 3
    mov si, footer_ln
    call putz_limit
    mov ax, [cursor_line]
    inc ax
    call format_u16
    mov dh, 24
    mov dl, 50
    mov bl, ATTR_TITLE
    call putz_limit

    mov dh, 24
    mov dl, 57
    mov cx, 4
    mov si, footer_col
    call putz_limit
    mov ax, [cursor_column]
    inc ax
    call format_u16
    mov dh, 24
    mov dl, 61
    mov bl, ATTR_TITLE
    call putz_limit

    mov dh, 24
    mov dl, 70
    mov cx, 3
    mov si, mode_ins
    cmp byte [insert_mode], 0
    jne .mode
    mov si, mode_ovr
.mode:
    call putz_limit
    ret

calc_cursor_position:
    mov es, [buffer_segment]
    xor si, si
    xor ax, ax                       ; line
    xor bx, bx                       ; column
.loop:
    cmp si, [cursor_offset]
    jae .done
    cmp byte [es:si], 0x0A
    jne .column
    inc ax
    xor bx, bx
    inc si
    jmp .loop
.column:
    inc bx
    inc si
    jmp .loop
.done:
    mov [cursor_line], ax
    mov [cursor_column], bx
    ret

calc_total_lines:
    mov es, [buffer_segment]
    xor si, si
    mov ax, 1
.scan:
    cmp si, [text_length]
    jae .done
    cmp byte [es:si], 0x0A
    jne .next
    inc ax
.next:
    inc si
    jmp .scan
.done:
    mov [total_line_count], ax
    ret

ensure_cursor_visible:
    mov ax, [cursor_line]
    cmp ax, [top_line]
    jae .check_bottom
    mov [top_line], ax
    jmp .horizontal
.check_bottom:
    mov bx, [top_line]
    add bx, VIEW_ROWS
    cmp ax, bx
    jb .horizontal
    sub ax, VIEW_ROWS - 1
    mov [top_line], ax
.horizontal:
    mov ax, [cursor_column]
    cmp ax, [horizontal_scroll]
    jae .check_right
    mov [horizontal_scroll], ax
    ret
.check_right:
    mov bx, [horizontal_scroll]
    add bx, TEXT_WIDTH
    cmp ax, bx
    jb .done
    sub ax, TEXT_WIDTH - 1
    mov [horizontal_scroll], ax
.done:
    ret

position_cursor:
    mov ax, [cursor_line]
    sub ax, [top_line]
    add al, VIEW_TOP
    mov dh, al
    mov ax, [cursor_column]
    sub ax, [horizontal_scroll]
    add al, TEXT_LEFT
    mov dl, al
    mov bh, 0
    mov ah, 0x02
    int 0x10
    mov ch, 6
    mov cl, 7
    cmp byte [insert_mode], 0
    jne .shape
    mov ch, 0
.shape:
    mov ah, 0x01
    int 0x10
    ret

; AX = zero-based line, returns SI = byte offset of its start.
find_line_start:
    mov es, [buffer_segment]
    xor si, si
    test ax, ax
    jz .done
.scan:
    cmp si, [text_length]
    jae .done
    cmp byte [es:si], 0x0A
    jne .next
    dec ax
    jz .after_lf
.next:
    inc si
    jmp .scan
.after_lf:
    inc si
.done:
    ret

show_help:
    mov dh, 4
    mov dl, 9
    mov ch, 19
    mov cl, 70
    mov bl, ATTR_TITLE
    call draw_box
    mov dh, 5
    mov dl, 25
    mov bl, ATTR_MENU
    mov cx, 30
    mov si, help_title
    call putz_limit
    mov si, help_lines
    mov dh, 7
.line:
    cmp byte [si], 0
    je .wait
    mov dl, 13
    mov bl, ATTR_TITLE
    mov cx, 54
    call putz_limit
    inc dh
    jmp .line
.wait:
    mov dh, 18
    mov dl, 26
    mov bl, ATTR_WARNING
    mov cx, 28
    mov si, help_close
    call putz_limit
    mov bh, 0
    mov dh, 18
    mov dl, 53
    mov ah, 0x02
    int 0x10
    xor ah, ah
    int 0x16
    ret

; ---------------------------------------------------------------------------
; VGA helpers

; DH=row, BL=attribute
fill_row:
    push cx
    push dx
    xor dl, dl
    mov cx, 80
    call fill_cells
    pop dx
    pop cx
    ret

; DH=row, DL=column, BL=attribute, CX=count
fill_cells:
    push ax
    push cx
    push di
    push es
    call screen_offset
    mov ax, 0xB800
    mov es, ax
    mov al, ' '
    mov ah, bl
    rep stosw
    pop es
    pop di
    pop cx
    pop ax
    ret

; DH=row, DL=column, BL=attribute, AL=character
put_char:
    push ax
    push di
    push es
    push ax
    call screen_offset
    mov ax, 0xB800
    mov es, ax
    pop ax
    mov ah, bl
    stosw
    pop es
    pop di
    pop ax
    ret

; DH=row, DL=column, BL=attribute, DS:SI=zstring, CX=max chars
putz_limit:
    push ax
    push cx
    push di
    push es
    call screen_offset
    mov ax, 0xB800
    mov es, ax
.next:
    jcxz .done
    lodsb
    test al, al
    jz .done
    mov ah, bl
    stosw
    dec cx
    jmp .next
.done:
    pop es
    pop di
    pop cx
    pop ax
    ret

; DH=row, DL=column -> DI byte offset
screen_offset:
    push ax
    push bx
    xor ax, ax
    mov al, dh
    mov di, ax
    shl di, 5                       ; row * 32
    shl ax, 7                       ; row * 128
    add di, ax                      ; row * 160
    xor ax, ax
    mov al, dl
    shl ax, 1
    add di, ax
    pop bx
    pop ax
    ret

; DH/DL top-left, CH/CL bottom-right, BL attribute
draw_box:
    push ax
    push bx
    push cx
    push dx
    mov [box_left], dl
    mov [box_right], cl
    mov [box_top], dh
    mov [box_bottom], ch
    mov [box_attr], bl
    mov al, 201
    call put_char
    mov dl, [box_right]
    mov al, 187
    call put_char
    mov dl, [box_left]
    inc dl
.top_line:
    cmp dl, [box_right]
    jae .middle
    mov al, 205
    call put_char
    inc dl
    jmp .top_line
.middle:
    mov dh, [box_top]
    inc dh
.middle_row:
    cmp dh, [box_bottom]
    jae .bottom
    mov dl, [box_left]
    mov al, 186
    call put_char
    inc dl
    xor cx, cx
    mov cl, [box_right]
    sub cl, dl
    call fill_cells
    mov dl, [box_right]
    mov al, 186
    call put_char
    inc dh
    jmp .middle_row
.bottom:
    mov dl, [box_left]
    mov al, 200
    call put_char
    mov dl, [box_right]
    mov al, 188
    call put_char
    mov dl, [box_left]
    inc dl
.bottom_line:
    cmp dl, [box_right]
    jae .done
    mov al, 205
    call put_char
    inc dl
    jmp .bottom_line
.done:
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; AX unsigned -> DS:SI zstring, CX length
format_u16:
    push ax
    push bx
    push dx
    push di
    mov di, number_buffer + 5
    mov byte [di], 0
    mov bx, 10
    xor cx, cx
.digit:
    xor dx, dx
    div bx
    add dl, '0'
    dec di
    mov [di], dl
    inc cx
    test ax, ax
    jnz .digit
    mov si, di
    pop di
    pop dx
    pop bx
    pop ax
    ret

; ---------------------------------------------------------------------------
; Serial/debug helpers

serial_print_z:
    push ax
    push dx
    push si
.next:
    lodsb
    test al, al
    jz .done
    mov ah, 0x01
    xor dx, dx
    int 0x14
    jmp .next
.done:
    pop si
    pop dx
    pop ax
    ret

print_dos_string:
    mov dx, si
    mov ah, 0x09
    int 0x21
    ret

; ---------------------------------------------------------------------------
; UI text and state

title_brand       db 'CiukiEDIT 0.8.3  |', 0
menu_text         db 'F1 Help   F2 Save   F3 Save as   Ins Insert/Overwrite   F10 Exit', 0
footer_text       db 'Ctrl+S Save  Arrows Move  Home/End  PgUp/Dn', 0
footer_ln         db 'Ln ', 0
footer_col        db 'Col ', 0
mode_ins          db 'INS', 0
mode_ovr          db 'OVR', 0
label_save_as     db 'Save as:   ', 0

status_new        db 'New document - type to edit, F2 to save', 0
status_loaded     db 'Document loaded - ready', 0
status_editing    db 'Editing - * marks unsaved changes', 0
status_saved      db 'Saved successfully', 0
status_save_error db 'Save failed - check path, disk, or permissions', 0
status_save_cancel db 'Save as cancelled', 0
status_unsaved    db 'UNSAVED CHANGES - F2 saves, press F10/Esc again to discard', 0
status_full       db 'Document limit reached (32 KiB)', 0
status_mode       db 'Insert/overwrite mode changed', 0

help_title        db 'CIUKEDIT QUICK REFERENCE', 0
help_lines:
    db 'Arrows         Move the caret', 0
    db 'Home / End     Start / end of line', 0
    db 'PgUp / PgDn    Move one screen', 0
    db 'Enter / Tab    New line / four-column tab', 0
    db 'Bksp / Del     Remove text', 0
    db 'Insert         Toggle insert / overwrite', 0
    db 'F2 / Ctrl+S    Save', 0
    db 'F3             Save with a different name', 0
    db 'F10 / Esc      Exit (asks before discarding)', 0
    db 0
help_close        db 'Press any key to return', 0

default_filename  db 'UNTITLED.TXT', 0
serial_boot       db '[CIUKEDIT:BOOT]', 0x0D, 0x0A, '[CIUKEDIT:READY]', 0x0D, 0x0A, 0
serial_new        db '[CIUKEDIT:NEW]', 0x0D, 0x0A, 0
serial_open       db '[CIUKEDIT:OPEN]', 0x0D, 0x0A, 0
serial_saved      db '[CIUKEDIT:SAVED]', 0x0D, 0x0A, 0
serial_ok         db '[CIUKEDIT:OK]', 0x0D, 0x0A, '$'
error_memory      db 'CIUKEDIT: not enough memory', 0x0D, 0x0A, '$'
error_open        db 'CIUKEDIT: cannot open file', 0x0D, 0x0A, '$'
error_large       db 'CIUKEDIT: file exceeds the 32 KiB editor limit', 0x0D, 0x0A, '$'

buffer_segment    dw 0
file_handle       dw 0
text_length       dw 0
cursor_offset     dw 0
cursor_line       dw 0
cursor_column     dw 0
top_line          dw 0
horizontal_scroll dw 0
render_offset     dw 0
render_line_number dw 0
total_line_count  dw 1
io_count          dw 0
prompt_length     dw 0
status_message    dw status_new
dirty_flag        db 0
file_exists       db 0
insert_mode       db 1
exit_armed        db 0
status_kind       db STATUS_NORMAL
render_row        db 0
box_left          db 0
box_right         db 0
box_top           db 0
box_bottom        db 0
box_attr          db 0

filename          times 64 db 0
prompt_filename   times 64 db 0
number_buffer     times 6 db 0
io_buffer         times 512 db 0

align 2
editor_stack       times 512 db 0
editor_stack_top:
editor_image_end:
