bits 16
org 0
%include "src/com/dos_window_abi.inc"

    jmp near runtime_entry
    db 0
    db 'CWRT0001'
    dw DW_ABI_VERSION, DW_HEADER_BYTES, image_end
    dw cells, dirty_rows, 0
host_callback dd 0
host_ds dw 0
indos_pointer dd 0
installed db 0
host_busy db 0
guest_focus db 1
guest_live db 0
cursor dw 0
cursor_shape dw 0x0607
video_mode db 3
video_page db 0
errors dw 0
graphics_request dw 0
timer_ticks dd 0
callback_count dd 0
    dw expanded_font
screen_width dw 800
screen_height dw 600
mouse_x dw 400
mouse_y dw 300
mouse_buttons dw 0
host_unsafe db 0
close_pending db 0
key_wait db 0
columns db 80
rows db 25
video_depth db 0
last_host_result dw 0
close_request db 0
    times DW_PAINT_COUNT-($-$$) db 0
paint_count dd 0
callback_tsc dq 0
callback_max_tsc dq 0
paint_tsc dq 0
paint_max_tsc dq 0
callback_start_tsc dq 0
paint_start_tsc dq 0
wait_service_count dd 0
mouse_change_count dd 0
    times DW_CELL_ENTRY-($-$$) db 0
    dw dw_cell_render,0
    times DW_GFX_ACTIVE-($-$$) db 0
cg_active db 0
cg_bpp db 0
cg_buffer_seg dw 0
cg_masks times 6 db 0
    dw cg_draw_band,0
    dw cg_descriptor
    times DW_HEADER_BYTES-($-$$) db 0

; Foreground FAR entry. AX operation; return AX=0/CF=0 for lifecycle success.
; Key operations return the BIOS scan/ASCII word, CF=1 if no key is available.
runtime_entry:
    pushfd
    pushad
    push ds
    push es
    push fs
    push gs
    push cs
    pop ds
    mov word [entry_result],0
    mov byte [entry_carry],0
    cld
    cmp ax,DW_OP_INSTALL
    je .install
    cmp ax,DW_OP_UNINSTALL
    je .uninstall
    cmp ax,DW_OP_CLEAR
    je .clear
    cmp ax,DW_OP_POP_KEY
    je .pop_key
    cmp ax,DW_OP_PEEK_KEY
    je .peek_key
    jmp .error
.clear:
    call clear_cells
    jmp .done
.pop_key:
    pushf
    cli
    call keyboard_pop
    jmp .key_result
.peek_key:
    pushf
    cli
    call keyboard_peek
.key_result:
    mov [entry_result],ax
    setc byte [entry_carry]
    popf
    jmp .done
.install:
    cmp byte [installed],0
    jne .error                     ; retained ownership is not a new install
    smsw ax
    test al,1
    jnz .v86_install
    cmp word [host_callback+2],0
    je .error
    cmp word [host_ds],0
    je .error
    cmp word [screen_width],80
    jb .error
    cmp word [screen_height],32
    jb .error
    call clear_cells
    mov word [errors],0
    mov byte [guest_focus],1
    mov byte [guest_live],0
    mov byte [host_busy],0
    mov byte [host_unsafe],0
    mov byte [close_pending],0
    mov byte [close_request],0
    mov byte [video_depth],0
    mov byte [key_wait],0
    xor ax,ax
    mov es,ax
    mov eax,[es:0x10*4]
    mov [old_int10],eax
    mov eax,[es:0x16*4]
    mov [old_int16],eax
    mov eax,[es:0x08*4]
    mov [old_int08],eax
    mov eax,[es:0x33*4]
    mov [old_int33],eax
    mov eax,[es:0x2F*4]
    mov [cg_old_int2f],eax
    mov eax,[es:0x15*4]
    mov [cg_old_int15],eax
    call cg_initialize
    ; Query the real ROM font before interposing video. The host's expanded
    ; font is opaque storage and is not a BIOS 8x16 glyph table.
    mov ax,0x1130
    mov bh,6
    pushf
    call far [old_int10]
    push cs
    pop ds
    mov [rom_font],bp
    mov [rom_font+2],es
    call disable_physical_mouse_callback
    cli
    xor ax,ax
    mov es,ax
    mov word [es:0x10*4],video_handler
    mov word [es:0x16*4],keyboard_handler
    mov word [es:0x08*4],timer_handler
    mov word [es:0x33*4],mouse_handler
    mov word [es:0x2F*4],cg_multiplex
    mov word [es:0x15*4],cg_keyboard_intercept
    mov ax,cs
    mov [es:0x10*4+2],ax
    mov [es:0x16*4+2],ax
    mov [es:0x08*4+2],ax
    mov [es:0x33*4+2],ax
    mov [es:0x2F*4+2],ax
    mov [es:0x15*4+2],ax
    mov byte [installed],1
    jmp .done
.uninstall:
    cmp byte [installed],0
    je .done
    cmp byte [guest_live],0
    jne .error
    cmp byte [host_busy],0
    jne .error
    cmp byte [vga_session],0
    je .text_uninstall
    call vga_session_uninstall
    jc .error
    mov ax,[vga_saved_draw]
    mov [DW_GFX_DRAW],ax
    mov byte [DW_RESIZABLE],0
    jmp .done
.v86_install:
    ; Under a V86 monitor only the CVSESSION VGA mode can host the child.
    mov word [errors],0
    mov byte [guest_focus],1
    mov byte [guest_live],0
    mov byte [host_busy],0
    mov byte [host_unsafe],0
    mov byte [close_pending],0
    mov byte [close_request],0
    mov byte [video_depth],0
    mov byte [key_wait],0
    mov ax,[DW_GFX_DRAW]
    mov [vga_saved_draw],ax
    call vga_session_install
    jc .protected
    mov word [DW_GFX_DRAW],vga_draw_band
    mov byte [DW_RESIZABLE],1
    jmp .done
.text_uninstall:
    cli
    mov byte [guest_live],0
    xor ax,ax
    mov es,ax
    mov eax,[old_int10]
    mov [es:0x10*4],eax
    mov eax,[old_int16]
    mov [es:0x16*4],eax
    mov eax,[old_int08]
    mov [es:0x08*4],eax
    mov eax,[old_int33]
    mov [es:0x33*4],eax
    mov eax,[cg_old_int2f]
    mov [es:0x2F*4],eax
    mov eax,[cg_old_int15]
    mov [es:0x15*4],eax
    mov byte [installed],0
    call restore_physical_mouse_callback
    jmp .done
.protected:
    or word [errors],DW_ERR_PROTECTED
.error:
    mov word [entry_result],1
    mov byte [entry_carry],1
.done:
    pop gs
    pop fs
    pop es
    pop ds
    popad
    popfd
    mov ax,[cs:entry_result]
    cmp byte [cs:entry_carry],0
    jne .carry
    clc
    retf
.carry:
    stc
    retf

; Original IRQ0 runs first, including BIOS tick accounting, INT1C and EOI.
; The second save protects our frame from firmware that clobbers upper halves.
timer_handler:
    push bp
    mov bp,sp
    pushad
    push ds
    push es
    push fs
    push gs
    pushfd
    pushad
    push ds
    push es
    push fs
    push gs
    pushfd
    pushf
    call far [cs:old_int08]
    cli
    popfd
    pop gs
    pop fs
    pop es
    pop ds
    popad
    cli
    push cs
    pop ds
    inc dword [timer_ticks]
    ; A blocked BIOS reader services the host after every wake, including
    ; mouse IRQ12. It need not wait for the next 18.2 Hz timer interrupt.
    cmp byte [key_wait],0
    jne .return
    ; Never enter a presenter from inside interrupted firmware.
    cmp word [ss:bp+4],0xA000
    jae .return
    call service_host
.return:
    popfd
    pop gs
    pop fs
    pop es
    pop ds
    popad
    pop bp
    iret

; Common foreground/IRQ service. No PIT/RTC changes and no DOS reentry.
; Every caller gets its exact stack, 32-bit registers and flags back.
service_host:
    pushfd
    pushad
    push ds
    push es
    push fs
    push gs
    cli
    push cs
    pop ds
    cmp byte [installed],1
    jne .return
    cmp byte [guest_live],1
    jne .return
    cmp byte [host_busy],0
    jne .return
    cmp byte [host_unsafe],0
    jne .return
    cmp byte [video_depth],0
    jne .return
    smsw ax
    test al,1
    jz .real_mode_host
    ; V86: only the CVSESSION VGA mode, only while the guest owns the window.
    cmp byte [vga_session],1
    jne .protected
    cmp byte [vga_exec_depth],1
    jne .return
    cmp byte [vga_host_entered],0
    jne .return
.real_mode_host:
    mov byte [host_busy],1
    mov [irq_saved_ss],ss
    mov [irq_saved_sp],sp
    mov ax,cs
    mov ss,ax
    mov sp,irq_stack_top
    cld
    call poll_physical_mouse
    cmp byte [vga_session],1
    je .text_focus                  ; BIOS key focus, as for text children
    cmp byte [cg_active],1
    jne .text_focus
    ; Source ports consume Set1 make/break events. Keeping duplicate BIOS
    ; keystrokes would fill its sixteen-key ring and make firmware beep.
    call keyboard_discard
    jmp .callback
.text_focus:
    cmp byte [guest_focus],1
    je .close_key
    call keyboard_discard
    jmp .callback
.close_key:
    cmp byte [close_pending],0
    je .callback
    call vga_device_close_key
    jc .close_sent                   ; raw Esc through the device model
    mov ax,0x011B
    call keyboard_insert
    jc .callback
.close_sent:
    mov byte [close_pending],0
.callback:
    call vga_before_callback
    rdtsc
    mov [callback_start_tsc],eax
    mov [callback_start_tsc+4],edx
    mov ax,0
    cmp dword [dirty_rows],0
    je .packet
    or ax,DW_HOST_TEXT_DIRTY
.packet:
    mov bx,[mouse_buttons]
    mov cx,[mouse_x]
    mov dx,[mouse_y]
    push cs
    pop es
    mov ds,[host_ds]
    ; busy is already set and the original IRQ0 acknowledged. A nested tick
    ; still reaches the BIOS but cannot enter this same stack/presenter again.
    sti
    call far [cs:host_callback]
    cli
    push cs
    pop ds
    mov [last_host_result],ax
    call vga_after_callback
    push ax
    rdtsc
    sub eax,[callback_start_tsc]
    sbb edx,[callback_start_tsc+4]
    add [callback_tsc],eax
    adc [callback_tsc+4],edx
    cmp edx,[callback_max_tsc+4]
    jb .metric_done
    ja .metric_max
    cmp eax,[callback_max_tsc]
    jbe .metric_done
.metric_max:
    mov [callback_max_tsc],eax
    mov [callback_max_tsc+4],edx
.metric_done:
    pop ax
    inc dword [callback_count]
    mov bx,ax
    and al,DW_HOST_FOCUS
    mov [guest_focus],al
    call vga_device_focus
    test al,al
    jnz .close_result
    call keyboard_discard
.close_result:
    test bx,DW_HOST_CLOSE
    jz .restore_stack
    mov byte [close_pending],1
    mov byte [close_request],1
.restore_stack:
    call vga_mouse_events
    mov ax,[irq_saved_ss]
    mov ss,ax
    mov sp,[irq_saved_sp]
    mov byte [host_busy],0
    jmp .return
.protected:
    cmp byte [cg_active],1
    je .return
    or word [errors],DW_ERR_PROTECTED
.return:
    pop gs
    pop fs
    pop es
    pop ds
    popad
    popfd
    ret

; AX physical BIOS key. Validate firmware's ring bounds before touching BDA.
keyboard_insert:
    push bx
    push cx
    push dx
    push es
    mov dx,ax
    call keyboard_ring
    jc .done
    mov bx,[es:0x1C]
    mov ax,bx
    add ax,2
    cmp ax,cx
    jb .next
    mov ax,si
.next:
    cmp ax,[es:0x1A]
    je .full
    mov [es:bx],dx
    mov [es:0x1C],ax
    clc
    jmp .done
.full:
    stc
.done:
    pop es
    pop dx
    pop cx
    pop bx
    ret

keyboard_ring:
    mov ax,0x40
    mov es,ax
    mov si,[es:0x80]
    mov cx,[es:0x82]
    cmp si,0x1E
    jb .legacy
    cmp cx,0x100
    ja .legacy
    cmp si,cx
    jb .bounds
.legacy:
    mov si,0x1E
    mov cx,0x3E
.bounds:
    mov ax,si
    or ax,cx
    test al,1
    jnz .invalid
    mov bx,[es:0x1A]
    cmp bx,si
    jb .invalid
    cmp bx,cx
    jae .invalid
    test bl,1
    jnz .invalid
    mov ax,[es:0x1C]
    cmp ax,si
    jb .invalid
    cmp ax,cx
    jae .invalid
    test al,1
    jnz .invalid
    clc
    ret
.invalid:
    or word [cs:errors],DW_ERR_KEYBOARD
    stc
    ret

keyboard_peek:
    call keyboard_ring
    jc .none
    cmp bx,[es:0x1C]
    je .none
    mov ax,[es:bx]
    clc
    ret
.none:
    xor ax,ax
    stc
    ret
keyboard_pop:
    call keyboard_peek
    jc .done
    add bx,2
    cmp bx,cx
    jb .head
    mov bx,si
.head:
    mov [es:0x1A],bx
    clc
.done:
    ret

; Physical keys typed while another retained window has focus must not be
; replayed later into the DOS child. Called only with interrupts disabled.
keyboard_discard:
    push ax
    push bx
    push cx
    push si
    push es
    call keyboard_ring
    jc .done
    mov ax,[es:0x1C]
    mov [es:0x1A],ax
.done:
    pop es
    pop si
    pop cx
    pop bx
    pop ax
    ret

; Frame offsets after PUSH BP / MOV BP,SP / PUSHAD / segment pushes.
F_AX equ -4
F_CX equ -8
F_DX equ -12
F_BX equ -16
F_BP equ -24
F_ES equ -36
F_FLAGS equ 6

keyboard_handler:
    cmp byte [cs:guest_live],0
    je .chain
    cmp ah,0
    je .handled
    cmp ah,0x10
    je .handled
    cmp ah,1
    je .handled
    cmp ah,0x11
    je .handled
.chain:
    jmp far [cs:old_int16]
.handled:
    push bp
    mov bp,sp
    pushad
    push ds
    push es
    push fs
    push gs
    pushfd
    push cs
    pop ds
    test ah,1
    jnz .status
    mov byte [key_wait],1
.wait:
    inc dword [wait_service_count]
    call service_host
    cmp byte [guest_focus],1
    jne .sleep
    call keyboard_pop
    jnc .key
.sleep:
    sti
    hlt
    cli
    jmp .wait
.key:
    mov byte [key_wait],0
    mov [ss:bp+F_AX],ax
    jmp .done
.status:
    cmp byte [guest_focus],1
    jne .empty
    call keyboard_peek
    jc .empty
    mov [ss:bp+F_AX],ax
    and word [ss:bp+F_FLAGS],~0x40
    jmp .done
.empty:
    or word [ss:bp+F_FLAGS],0x40
.done:
    popfd
    pop gs
    pop fs
    pop es
    pop ds
    popad
    pop bp
    iret

; Virtual BIOS text display. The physical VBE mode and BDA video metadata
; are never changed. Direct B800 writes, ports, DPMI and TSRs are outside this
; foreground text contract; no unsupported video request reaches hardware.
video_handler:
    push bp
    mov bp,sp
    pushad
    push ds
    push es
    push fs
    push gs
    pushfd
    push cs
    pop ds
    push cs
    pop es
    cld
    inc byte [video_depth]
    cmp ax,DW_QUERY_AX
    jne .video_api
    cmp bx,DW_QUERY_BX
    jne .video_api
    mov word [ss:bp+F_AX],DW_QUERY_REPLY
    movzx ax,byte [close_request]
    and ax,1
    mov [ss:bp+F_DX],ax
    jmp .done
.video_api:
    cmp ah,0
    je .mode
    cmp ah,1
    je .shape
    cmp ah,2
    je .position
    cmp ah,3
    je .get_cursor
    cmp ah,5
    je .page
    cmp ah,6
    je .scroll
    cmp ah,7
    je .scroll
    cmp ah,8
    je .read_character
    cmp ah,9
    je .repeat
    cmp ah,0x0A
    je .repeat
    cmp ah,0x0E
    je .teletype
    cmp ah,0x0F
    je .get_mode
    cmp ah,0x13
    je .string
    cmp ax,0x1130
    je .font
    cmp ax,0x1114
    je .done                    ; already logical 8x16, 25 rows
    cmp ax,0x1003
    je .done                    ; no physical blink/palette change
    cmp ah,0x4F
    jne .unsupported
    mov ax,[ss:bp+F_BX]
    mov [graphics_request],ax
    or word [errors],DW_ERR_GRAPHICS
    mov word [ss:bp+F_AX],0x014F
    jmp .done
.unsupported:
    or word [errors],DW_ERR_VIDEO_API
    jmp .done
.mode:
    mov bl,al
    and al,0x7F
    cmp al,3
    ja .graphics
    mov [video_mode],al
    mov byte [columns],80
    cmp al,2
    jae .mode_columns
    mov byte [columns],40
.mode_columns:
    mov word [cursor],0
    test bl,0x80
    jnz .dirty
    call clear_cells
    jmp .done
.graphics:
    mov [graphics_request],ax
    or word [errors],DW_ERR_GRAPHICS
    jmp .done
.shape:
    mov [cursor_shape],cx
    mov dx,[cursor]
    call mark_row
    jmp .done
.position:
    call page_zero
    jc .done
    call clamp_cursor
    push dx
    mov dx,[cursor]
    call mark_row
    pop dx
    mov [cursor],dx
    call mark_row
    jmp .done
.get_cursor:
    call page_zero
    jc .done
    mov ax,[cursor]
    mov [ss:bp+F_DX],ax
    mov ax,[cursor_shape]
    mov [ss:bp+F_CX],ax
    jmp .done
.page:
    test al,al
    jz .done
    jmp .bad_page
.get_mode:
    mov al,[video_mode]
    mov ah,[columns]
    mov [ss:bp+F_AX],ax
    and word [ss:bp+F_BX],0x00FF
    jmp .done
.read_character:
    call page_zero
    jc .done
    mov dx,[cursor]
    call cell_address
    mov ax,[cells+di]
    mov [ss:bp+F_AX],ax
    jmp .done
.repeat:
    call page_zero
    jc .done
    mov [work_function],ah
    mov [work_char],al
    mov [work_attr],bl
    mov dx,[cursor]
.repeat_loop:
    test cx,cx
    jz .done
    cmp dh,25
    jae .done
    call cell_address
    mov al,[work_char]
    mov [cells+di],al
    cmp byte [work_function],9
    jne .repeat_next
    mov al,[work_attr]
    mov [cells+di+1],al
.repeat_next:
    call mark_row
    inc dl
    cmp dl,[columns]
    jb .repeat_in_row
    xor dl,dl
    inc dh
.repeat_in_row:
    loop .repeat_loop
    jmp .done
.teletype:
    call page_zero
    jc .done
    mov byte [write_attr],0
    call put_character
    jmp .done
.string:
    call page_zero
    jc .done
    mov [string_flags],al
    mov [work_attr],bl
    mov ax,[cursor]
    mov [string_cursor],ax
    call clamp_cursor
    mov [cursor],dx
    mov ax,[ss:bp+F_ES]
    mov fs,ax
    ; PUSH BP precedes the register frame: caller BP is [SS:BP].
    mov si,[ss:bp]
.string_loop:
    jcxz .string_done
    mov al,[fs:si]
    inc si
    test byte [string_flags],2
    jz .string_attribute
    mov ah,[fs:si]
    inc si
    mov [work_attr],ah
.string_attribute:
    mov byte [write_attr],1
    push cx
    push si
    call put_character
    pop si
    pop cx
    loop .string_loop
.string_done:
    test byte [string_flags],1
    jnz .done
    mov dx,[cursor]
    call mark_row
    mov ax,[string_cursor]
    mov [cursor],ax
    mov dx,ax
    call mark_row
    jmp .done
.font:
    mov ax,[rom_font]
    mov [ss:bp],ax
    mov ax,[rom_font+2]
    mov [ss:bp+F_ES],ax
    mov word [ss:bp+F_CX],16
    mov byte [ss:bp+F_DX],24
    jmp .done
.scroll:
    mov [scroll_direction],ah
    mov [scroll_count],al
    mov [scroll_attribute],bh
    cmp ch,25
    jae .done
    cmp cl,[columns]
    jae .done
    cmp dh,25
    jb .scroll_bottom
    mov dh,24
.scroll_bottom:
    cmp dl,[columns]
    jb .scroll_right
    mov dl,[columns]
    dec dl
.scroll_right:
    cmp ch,dh
    ja .done
    cmp cl,dl
    ja .done
    mov [scroll_top_left],cx
    mov [scroll_bottom_right],dx
    call scroll_cells
    jmp .done
.bad_page:
    or word [errors],DW_ERR_PAGE
    jmp .done
.dirty:
    mov dword [dirty_rows],0x01FFFFFF
.done:
    dec byte [video_depth]
    popfd
    pop gs
    pop fs
    pop es
    pop ds
    popad
    pop bp
    iret

page_zero:
    test bh,bh
    jz .ok
    or word [errors],DW_ERR_PAGE
    stc
    ret
.ok:
    clc
    ret
clamp_cursor:
    cmp dh,25
    jb .column
    mov dh,24
.column:
    cmp dl,[columns]
    jb .done
    mov dl,[columns]
    dec dl
.done:
    ret
cell_address:
    push ax
    movzx di,dh
    imul di,160
    movzx ax,dl
    shl ax,1
    add di,ax
    pop ax
    ret
mark_row:
    push eax
    push ecx
    movzx ecx,dh
    mov eax,1
    shl eax,cl
    or [dirty_rows],eax
    pop ecx
    pop eax
    ret
clear_cells:
    push ax
    push cx
    push di
    push es
    push cs
    pop es
    mov di,cells
    mov ax,0x0720
    mov cx,2000
    cld
    rep stosw
    mov word [cursor],0
    mov dword [dirty_rows],0x01FFFFFF
    pop es
    pop di
    pop cx
    pop ax
    ret

put_character:
    mov dx,[cursor]
    ; Cursor movement must also erase the old caret row.
    call mark_row
    cmp al,7
    je .done
    cmp al,8
    je .backspace
    cmp al,13
    je .carriage
    cmp al,10
    je .linefeed
    call cell_address
    mov [cells+di],al
    cmp byte [write_attr],0
    je .written
    mov al,[work_attr]
    mov [cells+di+1],al
.written:
    call mark_row
    inc dl
    cmp dl,[columns]
    jb .save
    xor dl,dl
.linefeed:
    inc dh
    cmp dh,25
    jb .save
    mov byte [scroll_direction],6
    mov byte [scroll_count],1
    mov byte [scroll_attribute],7
    mov word [scroll_top_left],0
    mov al,[columns]
    dec al
    mov ah,24
    mov [scroll_bottom_right],ax
    push dx
    call scroll_cells
    pop dx
    mov dh,24
    mov dword [dirty_rows],0x01FFFFFF
    jmp .save
.backspace:
    test dl,dl
    jz .done
    dec dl
    jmp .save
.carriage:
    xor dl,dl
.save:
    mov [cursor],dx
    call mark_row
.done:
    ret

; A bounded 25x80 rectangle copy, rows traversed in memmove order.
scroll_cells:
    mov ax,[scroll_top_left]
    mov [scroll_row],ah
    mov ax,[scroll_bottom_right]
    cmp byte [scroll_direction],7
    jne .row
    mov [scroll_row],ah
.row:
    mov dh,[scroll_row]
    call mark_row
    mov dl,[scroll_top_left]
    call cell_address
    mov si,di
    mov al,[scroll_count]
    test al,al
    jz .blank
    cmp byte [scroll_direction],7
    je .down
    add al,dh
    jc .blank
    cmp al,[scroll_bottom_right+1]
    ja .blank
    jmp .source
.down:
    cmp dh,al
    jb .blank
    mov ah,dh
    sub ah,al
    mov al,ah
    cmp al,[scroll_top_left+1]
    jb .blank
.source:
    mov dh,al
    call cell_address
    xchg si,di                 ; SI source, DI destination
    mov byte [scroll_blank],0
    jmp .columns
.blank:
    mov di,si
    mov byte [scroll_blank],1
.columns:
    movzx cx,byte [scroll_bottom_right]
    movzx ax,byte [scroll_top_left]
    sub cx,ax
    inc cx
.cell:
    mov ax,0x0020
    mov ah,[scroll_attribute]
    cmp byte [scroll_blank],1
    je .store
    mov ax,[cells+si]
    add si,2
.store:
    mov [cells+di],ax
    add di,2
    loop .cell
    mov al,[scroll_row]
    cmp byte [scroll_direction],7
    je .previous
    cmp al,[scroll_bottom_right+1]
    jae .done
    inc byte [scroll_row]
    jmp .row
.previous:
    cmp al,[scroll_top_left+1]
    jbe .done
    dec byte [scroll_row]
    jmp .row
.done:
    ret

disable_physical_mouse_callback:
    mov byte [physical_mouse],0
    mov ax,[old_int33+2]
    test ax,ax
    jz .done
    mov es,ax
    mov bx,[old_int33]
    cmp byte [es:bx],0xCF
    je .done
    mov byte [physical_mouse],1
    xor ax,ax
    mov es,ax
    xor cx,cx
    xor dx,dx
    mov ax,0x14
    pushf
    call far [cs:old_int33]
    push cs
    pop ds
    mov [saved_mouse_mask],cx
    mov [saved_mouse_callback],dx
    mov [saved_mouse_callback+2],es
.done:
    ret
restore_physical_mouse_callback:
    cmp byte [physical_mouse],1
    jne .done
    mov cx,[saved_mouse_mask]
    mov dx,[saved_mouse_callback]
    mov es,[saved_mouse_callback+2]
    mov ax,0x14
    pushf
    call far [cs:old_int33]
    push cs
    pop ds
.done:
    ret
poll_physical_mouse:
    cmp byte [physical_mouse],1
    jne .done
    push dword [mouse_x]
    push word [mouse_buttons]
    xor bx,bx
    mov ax,3
    pushf
    call far [cs:old_int33]
    push cs
    pop ds
    mov [mouse_buttons],bx
    xor cx,cx
    xor dx,dx
    mov ax,0x0B
    pushf
    call far [cs:old_int33]
    push cs
    pop ds
    movsx eax,cx
    shl eax,1
    movzx ebx,word [mouse_x]
    add eax,ebx
    movzx ebx,word [screen_width]
    sub ebx,24
    call clamp_mouse_axis
    mov [mouse_x],ax
    movsx eax,dx
    shl eax,1
    movzx ebx,word [mouse_y]
    add eax,ebx
    movzx ebx,word [screen_height]
    sub ebx,16
    call clamp_mouse_axis
    mov [mouse_y],ax
    pop bx
    pop ecx
    cmp bx,[mouse_buttons]
    jne .changed
    cmp ecx,[mouse_x]
    je .done
.changed:
    inc dword [mouse_change_count]
.done:
    ret
clamp_mouse_axis:
    test eax,eax
    jns .maximum
    xor eax,eax
.maximum:
    cmp eax,ebx
    jbe .done
    mov eax,ebx
.done:
    ret

; The desktop owns the real mouse. Guest callbacks are never registered on
; it. No virtual guest mouse is advertised until client coordinate mapping
; is supplied; polling returns a nonblocking absent interface, not host pixels.
mouse_handler:
    cmp byte [cs:guest_live],0
    jne .guest
    jmp far [cs:old_int33]
.guest:
    cmp byte [cs:vga_session],1
    jne .absent_mouse
    jmp vga_guest_mouse
.absent_mouse:
    cmp ax,0
    je .absent
    cmp ax,0x21
    je .absent
    cmp ax,3
    je .status
    cmp ax,0x0B
    je .motion
    cmp ax,0x0C
    je .callback
    cmp ax,0x14
    je .callback
    iret
.absent:
    xor ax,ax
    xor bx,bx
    iret
.status:
    xor bx,bx
.motion:
    xor cx,cx
    xor dx,dx
    iret
.callback:
    or word [cs:errors],DW_ERR_MOUSE_API
    cmp ax,0x14
    jne .callback_done
    xor cx,cx
    xor dx,dx
    push ax
    xor ax,ax
    mov es,ax
    pop ax
.callback_done:
    iret

%include "src/com/dos_window_cell.inc"
%include "src/com/dos_window_graphics.inc"
%include "src/com/dos_window_graphics_draw.inc"
%include "src/com/dos_window_vga.inc"
vga_saved_draw dw 0

align 4
old_int10 dd 0
old_int16 dd 0
old_int08 dd 0
old_int33 dd 0
rom_font dd 0
saved_mouse_callback dd 0
saved_mouse_mask dw 0
physical_mouse db 0
entry_carry db 0
entry_result dw 0
irq_saved_ss dw 0
irq_saved_sp dw 0
work_function db 0
work_char db 0
work_attr db 7
write_attr db 0
string_flags db 0
string_cursor dw 0
scroll_direction db 6
scroll_count db 0
scroll_attribute db 7
scroll_top_left dw 0
scroll_bottom_right dw 0x184F
scroll_row db 0
scroll_blank db 0
align 4
dirty_rows dd 0x01FFFFFF
cells times 80*25 dw 0x0720
align 16
expanded_font times 8192 db 0
align 16
irq_stack times 8192 db 0
irq_stack_top:
image_end:
%if image_end-$$ > 0xFFF0
%error "DOS window runtime exceeds one real-mode segment"
%endif
