%include "src/com/ui_theme.inc"
; Native 800x600 buffered installer, with VGA recovery on older adapters.
; Private drawing and mouse state: no resident hooks or shared-driver changes.
%define G_BLACK 0
%define G_BLUE 1
%define G_TEAL 3
%define G_GRAY 7
%define G_DARK 8
%define G_WHITE 15
%define G_FONT_BYTES (95 + 95*32)
%macro G_RECT 5
    mov bx, %1
    mov dx, %2
    mov cx, %3
    mov si, %4
    mov al, %5
    call g_rect
%endmacro
%macro G_TEXT 4-5 0
    mov bx, %1
    mov dx, %2
    mov si, %3
    mov ax, (%5 << 8) | %4
    call g_text
%endmacro
%macro G_BUTTON 6
    mov bx, %1
    mov dx, %2
    mov cx, %3
    mov si, %4
    mov al, %5
    mov ah, %6
    call g_button
%endmacro
%macro G_RADIO 5
    mov bx, %1
    mov dx, %2
    mov si, %3
    mov al, %4
    mov ah, %5
    call g_radio
%endmacro

; All primitives preserve their input registers and segments.
g_rect:
    pushad
    push es
    cmp byte [g_composing],1
    je g_render_rect
    cmp byte [g_vbe],1
    je g_render_rect
    mov [g_color], al
    mov [g_x], bx
    mov [g_y], dx
    mov [g_width], cx
    mov [g_height], si
    test cx, cx
    jz .out
    test si, si
    jz .out
    mov ax, 0xA000
    mov es, ax
    mov dx, 0x3C4
    mov ax, 0x0F02
    out dx, ax
    mov dx, 0x3CE
    mov ax, 0x0005
    out dx, ax
    mov ax, 0x0003
    out dx, ax
    mov ax, 0x0F01
    out dx, ax
    xor al, al
    mov ah, [g_color]
    out dx, ax
    mov ax, [g_y]
    imul ax, 80
    mov di, [g_x]
    shr di, 3
    add di, ax
    mov bx, [g_x]
    and bx, 7
    mov cl, bl
    mov al, 255
    shr al, cl
    mov [g_left_mask], al
    mov bx, [g_x]
    add bx, [g_width]
    dec bx
    mov ax, bx
    shr ax, 3
    mov cx, [g_x]
    shr cx, 3
    sub ax, cx
    mov [g_span], ax
    and bx, 7
    mov cx, 7
    sub cx, bx
    mov al, 255
    shl al, cl
    mov [g_right_mask], al
.row:
    push di
    mov ah, [g_left_mask]
    cmp word [g_span], 0
    jne .many
    and ah, [g_right_mask]
    mov al, 8
    out dx, ax
    mov al, [es:di]
    mov [es:di], al
    jmp .advance
.many:
    mov al, 8
    out dx, ax
    mov al, [es:di]
    mov [es:di], al
    inc di
    mov ax, 0xFF08
    out dx, ax
    mov cx, [g_span]
    dec cx
    mov al, 255
    rep stosb
    mov ah, [g_right_mask]
    mov al, 8
    out dx, ax
    mov al, [es:di]
    mov [es:di], al
.advance:
    pop di
    add di, 80
    dec word [g_height]
    jnz .row
    mov ax, 0xFF08
    out dx, ax
.out:
    pop es
    popad
    ret

; Transparent proportional text, two 13px font weights; no BIOS font dependence.
g_text:
    pushad
    mov [g_text_color], al
    mov di, g_font
    test ah, ah
    jz .font
    add di, G_FONT_BYTES
.font:
    mov [g_font_ptr], di
.next:
    lodsb
    test al, al
    jz .done
    cmp al, 32
    jb .next
    cmp al, 126
    ja .next
    sub al, 32
    movzx di, al
    push si
    mov si, [g_font_ptr]
    mov bp, si
    add bp, di
    mov al, [ds:bp]
    mov [g_advance], al
    shl di, 5
    add si, 95
    add si, di
    call g_glyph
    pop si
    movzx ax, byte [g_advance]
    add bx, ax
    jmp .next
.done:
    popad
    ret

; Write up to three planar bytes per glyph row, rather than one BIOS/port
; transaction per pixel. Text redraw does not dominate disk progress updates.
g_glyph:
    cmp byte [g_composing],1
    je g_render_glyph
    cmp byte [g_vbe],1
    je g_render_glyph
    pushad
    push es
    mov ax, 0xA000
    mov es, ax
    mov ax, dx
    imul ax, 80
    mov di, bx
    shr di, 3
    add di, ax
    and bx, 7
    mov [g_glyph_shift], bl
    mov dx, 0x3C4
    mov ax, 0x0F02
    out dx, ax
    mov dx, 0x3CE
    mov ax, 0x0005
    out dx, ax
    mov ax, 0x0003
    out dx, ax
    mov ax, 0x0F01
    out dx, ax
    xor al, al
    mov ah, [g_text_color]
    out dx, ax
    mov bx, 16
.row:
    movzx eax, word [si]
    add si, 2
    shl eax, 8
    mov cl, [g_glyph_shift]
    shr eax, cl
    mov ebp, eax
    shr eax, 16
    call .byte
    inc di
    mov eax, ebp
    shr eax, 8
    call .byte
    inc di
    mov eax, ebp
    call .byte
    add di, 78
    dec bx
    jnz .row
    mov ax, 0xFF08
    out dx, ax
    pop es
    popad
    ret
.byte:
    test al, al
    jz .skip
    mov ah, al
    mov al, 8
    out dx, ax
    mov al, [es:di]
    mov [es:di], al
.skip:
    ret

; Raised border around a rectangle (BX=x DX=y CX=w SI=h).
g_bevel:
    pushad
    mov [g_bevel_height], si
    mov [g_bevel_width], cx
    mov si, 1
    mov al, G_WHITE
    call g_rect
    mov si, [g_bevel_height]
    mov cx, 1
    call g_rect
    add dx, si
    dec dx
    mov cx, [g_bevel_width]
    mov si, 1
    mov al, G_BLACK
    call g_rect
    sub dx, [g_bevel_height]
    inc dx
    add bx, cx
    dec bx
    mov cx, 1
    mov si, [g_bevel_height]
    call g_rect
    popad
    ret

; Register a hit target: AL event, AH disabled; BX,DX,CX,DI rectangle.
g_hit:
    pushad
    cmp byte [g_widget_count], 16
    jae .done
    movzx bp, byte [g_widget_count]
    imul bp, 10
    add bp, g_widgets
    mov [ds:bp], bx
    mov [ds:bp+2], dx
    add cx, bx
    mov [ds:bp+4], cx
    add di, dx
    mov [ds:bp+6], di
    mov [ds:bp+8], ax
    inc byte [g_widget_count]
.done:
    popad
    ret

g_button:
    pushad
    mov [g_button_label], si
    mov [g_button_event], ax
    mov di, 26
    call g_hit
    mov si, 26
    mov al, G_GRAY
    call g_rect
    call g_bevel
    inc bx
    inc dx
    sub cx, 2
    mov si, 24
    ; Inner shadow along lower/right edge.
    push bx
    push dx
    add dx, 23
    mov si, 1
    mov al, G_DARK
    call g_rect
    pop dx
    pop bx
    mov al, [g_button_event]
    cmp al, [g_focus]
    jne .label
    add bx, 3
    add dx, 3
    sub cx, 6
    mov si, 1
    mov al, G_BLACK
    call g_rect
    add dx, 16
    call g_rect
    sub dx, 19
    sub bx, 3
    add cx, 6
.label:
    add bx, 12
    add dx, 4
    mov si, [g_button_label]
    mov al, G_BLACK
    cmp byte [g_button_event+1], 0
    je .text
    mov al, G_DARK
.text:
    xor ah, ah
    call g_text
    popad
    ret

; AL event; AH=selected. Radio row is entirely clickable.
g_radio:
    pushad
    mov [g_button_label], si
    mov [g_radio_selected], ah
    xor ah, ah
    mov cx, 326
    mov di, 27
    call g_hit
    mov cx, 12
    mov si, 12
    add dx, 4
    mov al, G_DARK
    call g_rect
    inc bx
    inc dx
    mov cx, 10
    mov si, 10
    mov al, G_WHITE
    call g_rect
    cmp byte [g_radio_selected], 0
    je .label
    add bx, 3
    add dx, 3
    mov cx, 4
    mov si, 4
    mov al, G_BLACK
    call g_rect
    sub bx, 3
    sub dx, 3
.label:
    add bx, 21
    sub dx, 5
    mov si, [g_button_label]
    xor ax, ax
    call g_text
    popad
    ret

; Mouse state is saved/restored using INT 33h; drawing stays private and hidden
; from the shared driver, so other DOS/Windows programs retain their settings.
g_init:
    ; COM children initially own the remaining DOS arena. Release everything
    ; above our real code/data/stack before allocating the graphics buffer.
    push cs
    pop es
    mov bx,(setup_stack_top - $$ + 0x100 + 15) >> 4
    mov ah,0x4A
    int 0x21
    xor bx, bx
    mov ax, 0x15
    int 0x33
    cmp bx, 512
    ja .reset
    test bx, bx
    jz .reset
    push cs
    pop es
    mov dx, g_mouse_state
    mov ax, 0x16
    int 0x33
    mov byte [g_mouse_saved], 1
.reset:
    xor ax, ax
    int 0x33
    cmp ax, 0xFFFF
    jne .video
    mov byte [g_mouse_ready], 1
.video:
%ifndef SETUP_FORCE_VGA
    call g_render_begin
    jnc .video_ready
%endif
    mov ax, 0x0012
    int 0x10
    push cs
    pop ds
    push cs
    pop es
    mov byte [gui_active], 1
    ; Set a literal classic UI palette; VGA logical color 7 maps to DAC 7 etc.
    mov ax, 0x1002
    mov dx, g_attributes
    int 0x10
    mov ax, 0x1012
    xor bx, bx
    mov cx, 16
    mov dx, g_palette
    int 0x10
    call g_render_begin_vga
.video_ready:
    mov byte [gui_active],1
    cmp byte [g_mouse_ready], 1
    jne .done
    mov ax, 7
    xor cx, cx
    mov dx,[g_screen_width]
    sub dx,24
    int 0x33
    mov ax, 8
    xor cx, cx
    mov dx,[g_screen_height]
    sub dx,16
    int 0x33
    mov ax, 4
    mov cx, 580
    mov dx, 435
    int 0x33
.done:
    ret

g_exit:
    call g_pointer_hide
    mov byte [gui_active], 0
    call g_render_end
    mov ax, 3
    int 0x10
    cmp byte [g_mouse_saved], 1
    jne .done
    push cs
    pop es
    mov dx, g_mouse_state
    mov ax, 0x17
    int 0x33
.done:
    push cs
    pop ds
    push cs
    pop es
    ret

; Save 24x16 pixels as planar bytes; restore exactly on movement/redraw.
g_pointer_hide:
    cmp byte [g_vbe],1
    je g_vbe_pointer_hide
    cmp byte [g_pointer_visible], 1
    jne .done
    pushad
    push es
    mov ax, 0xA000
    mov es, ax
    mov dx, 0x3CE
    mov ax, 0x0001
    out dx, ax
    mov ax, 0x0003
    out dx, ax
    mov ax, 0x0005
    out dx, ax
    mov ax, 0xFF08
    out dx, ax
    mov si, g_pointer_under
    mov ah, 1
.plane:
    mov dx, 0x3C4
    mov al, 2
    out dx, ax
    push ax
    mov di, [g_pointer_offset]
    mov bp, 16
.row:
    mov cx, 3
    rep movsb
    add di, 77
    dec bp
    jnz .row
    pop ax
    shl ah, 1
    cmp ah, 16
    jb .plane
    mov ax, 0x0F02
    out dx, ax
    pop es
    popad
    mov byte [g_pointer_visible], 0
.done:
    ret

g_pointer_show:
    cmp byte [g_composing],1
    je .done
    cmp byte [g_vbe],1
    je g_vbe_pointer_show
    cmp byte [g_mouse_ready], 1
    jne .done
    cmp byte [g_pointer_visible], 1
    je .done
    pushad
    push es
    mov ax, 0xA000
    mov es, ax
    mov ax, [g_mouse_y]
    imul ax, 80
    mov di, [g_mouse_x]
    shr di, 3
    add di, ax
    mov [g_pointer_offset], di
    mov si, g_pointer_under
    mov dx, 0x3CE
    mov ax, 0x0005
    out dx, ax
    xor ah, ah
.plane:
    mov al, 4
    out dx, ax
    push ax
    mov di, [g_pointer_offset]
    mov bp, 16
.row:
    mov al, [es:di]
    mov [si], al
    mov al, [es:di+1]
    mov [si+1], al
    mov al, [es:di+2]
    mov [si+2], al
    add si, 3
    add di, 80
    dec bp
    jnz .row
    pop ax
    inc ah
    cmp ah, 4
    jb .plane
    mov bx, [g_mouse_x]
    mov dx, [g_mouse_y]
    mov si, g_pointer_black
    xor al, al
    call g_pointer_shape
    mov si, g_pointer_white
    mov al, G_WHITE
    call g_pointer_shape
    mov byte [g_pointer_visible], 1
    pop es
    popad
.done:
    ret

g_pointer_shape:
    pushad
    mov [g_pointer_color], al
    mov bp, 16
.row:
    lodsw
    push bx
    mov di, 16
.bit:
    shl ax, 1
    jnc .clear
    push ax
    push si
    mov al, [g_pointer_color]
    mov cx, 1
    mov si, 1
    call g_rect
    pop si
    pop ax
.clear:
    inc bx
    dec di
    jnz .bit
    pop bx
    inc dx
    dec bp
    jnz .row
    popad
    ret

; Nonblocking event. AL=0 idle, 1 back, 2 next/start, 3 cancel, 4 finish,
; 10/11 radios, 20..26 disks, 30 destructive acknowledgement, 250 focus change.
g_poll:
    mov ah, 1
    int 0x16
    jz .mouse
    xor ah, ah
    int 0x16
    cmp al, 27
    je .cancel
    cmp al, 13
    je .activate
    cmp al, ' '
    je .space
    cmp al, 9
    je .tab
    cmp ah, 0x48
    je .up
    cmp ah, 0x50
    je .down
    cmp al, 'b'
    je .back
    cmp al, 'n'
    je .next
    cmp al, 'q'
    je .radio0
    cmp al, 'f'
    je .radio1
    cmp al, 'i'
    je .radio0
    cmp al, '1'
    jb .idle
    cmp al, '7'
    ja .idle
    sub al, '1'
    add al, 20
    ret
.space:
    cmp byte [gui_page], 3
    jne .activate
    mov al, 30
    ret
.tab:
    ; Cycle only through buttons the current page actually draws.
    mov ah, [g_focus]
    cmp byte [gui_page], 4
    je .focus_cancel_only
    cmp byte [gui_page], 5
    jb .focus_wizard
    ja .focus_error
    mov al, 4                   ; complete: Restart/Finish, Close after install
    cmp byte [gui_operation], 1
    je .focus_set
    cmp ah, 4
    jne .focus_set
.focus_cancel_only:
    mov al, 3
    jmp .focus_set
.focus_error:
    mov al, 1                   ; error: Try again and Close
    cmp ah, 1
    jne .focus_set
    mov al, 3
    jmp .focus_set
.focus_wizard:
    mov al, 3
    cmp ah, 2
    je .focus_set
    mov al, 2
    cmp ah, 3
    jne .focus_set
    cmp byte [gui_page], 0      ; Back is disabled on the first page
    je .focus_set
    mov al, 1
.focus_set:
    mov [g_focus], al
    mov al, 250
    ret
.activate:
    mov al, [g_focus]
    ret
.up:
    cmp byte [gui_page], 1
    jne .radio0
    mov al, [gui_disk_selection]
    test al, al
    jz .idle
    dec al
    add al, 20
    ret
.down:
    cmp byte [gui_page], 1
    jne .radio1
    mov al, [gui_disk_selection]
    inc al
    add al, 20
    ret
.radio0:
    mov al, 10
    ret
.radio1:
    mov al, 11
    ret
.back:
    mov al, 1
    ret
.next:
    mov al, 2
    ret
.cancel:
    mov al, 3
    ret
.mouse:
    cmp byte [g_mouse_ready], 1
    jne .idle
    mov ax, 3
    int 0x33
    mov [g_mouse_buttons], bx
    ; Own signed motion coordinates. The shared driver's absolute position
    ; can wrap at zero; consuming raw mickeys keeps this GUI independent.
    mov ax,0x000B
    int 0x33
    movsx ecx,cx
    movsx edx,dx
    shl ecx,1
    shl edx,1
    movzx eax,word [g_mouse_x]
    add ecx,eax
    movzx eax,word [g_mouse_y]
    add edx,eax
    test ecx,ecx
    jns .raw_x_min
    xor ecx,ecx
.raw_x_min:
    test edx,edx
    jns .raw_y_min
    xor edx,edx
.raw_y_min:
    mov ax,[g_screen_width]
    movzx eax,ax
    sub eax,24
    cmp ecx,eax
    jbe .raw_x_max
    mov ecx,eax
.raw_x_max:
    mov ax,[g_screen_height]
    movzx eax,ax
    sub eax,16
    cmp edx,eax
    jbe .raw_y_max
    mov edx,eax
.raw_y_max:
    cmp cx, [g_mouse_x]
    jne .move
    cmp dx, [g_mouse_y]
    je .click
.move:
    call g_pointer_hide
    mov [g_mouse_x], cx
    mov [g_mouse_y], dx
    mov ax,4
    int 0x33
    call g_pointer_show
.click:
    mov ax, [g_mouse_buttons]
    and al, 1
    cmp al, [g_mouse_previous]
    je .idle
    mov [g_mouse_previous], al
    test al, al
    jz .release
    call g_hit_test
    mov [g_mouse_pressed], al
    jmp .idle
.release:
    call g_hit_test
    cmp al, [g_mouse_pressed]
    je .out
.idle:
    xor ax, ax
.out:
    ret

g_hit_test:
    mov si, g_widgets
    mov cl, [g_widget_count]
    xor ch, ch
    jcxz .none
.next:
    cmp byte [si+9], 0
    jne .advance
    mov ax, [g_mouse_x]
    sub ax,[g_origin_x]
    cmp ax, [si]
    jb .advance
    cmp ax, [si+4]
    jae .advance
    mov ax, [g_mouse_y]
    sub ax,[g_origin_y]
    cmp ax, [si+2]
    jb .advance
    cmp ax, [si+6]
    jae .advance
    mov al, [si+8]
    ret
.advance:
    add si, 10
    loop .next
.none:
    xor ax, ax
    ret

g_event:
    call g_poll
    test al, al
    jnz .done
    sti
    hlt
    jmp g_event
.done:
    ret

; The page chrome is fixed; only actual wizard stages occupy the sidebar.
g_frame:
    cmp byte [g_composing],1
    je .scene
    call g_pointer_hide
.scene:
    mov byte [g_widget_count], 0
    cmp byte [g_vbe],1
    jne .vga_background
    G_RECT -80,-60,800,600,G_TEAL
    G_RECT -80,-60,800,29,G_GRAY
    G_RECT -80,-32,800,1,G_DARK
    mov bx,-75
    mov dx,-58
    call ciuki_logo24
    G_TEXT -47,-54,gt_brand,G_BLACK,1
    G_TEXT 621,-54,gt_title_short,G_BLACK
    G_RECT -80,508,800,32,G_GRAY
    G_RECT -80,508,800,1,G_WHITE
    G_TEXT -66,516,gt_tagline,G_BLACK
    jmp .wizard
.vga_background:
    G_RECT 0,0,640,480,G_TEAL
    G_RECT 0,0,640,29,G_GRAY
    G_RECT 0,28,640,1,G_DARK
    mov bx,5
    mov dx,2
    call ciuki_logo24
    G_TEXT 33,6,gt_brand,G_BLACK,1
    G_TEXT 541,6,gt_title_short,G_BLACK
    G_RECT 0,448,640,32,G_GRAY
    G_RECT 0,448,640,1,G_WHITE
    G_TEXT 14,456,gt_tagline,G_BLACK
.wizard:
    G_RECT 43,43,554,386,G_DARK
    G_RECT 40,40,554,386,G_GRAY
    mov bx,40
    mov dx,40
    mov cx,554
    mov si,386
    call g_bevel
    G_RECT 43,43,174,22,G_BLUE
    G_RECT 225,47,358,1,G_DARK
    G_RECT 225,48,358,1,G_WHITE
    G_RECT 225,51,358,1,G_DARK
    G_RECT 225,52,358,1,G_WHITE
    G_RECT 225,55,358,1,G_DARK
    G_RECT 225,56,358,1,G_WHITE
    G_RECT 225,59,358,1,G_DARK
    G_RECT 225,60,358,1,G_WHITE
    G_TEXT 51,45,gt_title,G_WHITE,1
    G_RECT 52,77,140,296,G_BLUE
    ; Pixel-drawn monitor and disk, deliberately small like the original wizard.
    G_RECT 79,100,84,61,G_GRAY
    G_RECT 83,104,76,49,G_BLACK
    G_RECT 86,107,70,43,G_TEAL
    mov bx,109
    mov dx,115
    call ciuki_logo24
    G_RECT 115,161,13,9,G_DARK
    G_RECT 99,170,45,4,G_GRAY
    G_RECT 78,183,86,20,G_GRAY
    G_RECT 84,188,57,4,G_DARK
    G_RECT 151,188,6,4,10
    G_TEXT 65,225,gt_step1,G_WHITE
    G_TEXT 65,254,gt_step2,G_WHITE
    G_TEXT 65,283,gt_step3,G_WHITE
    G_TEXT 65,312,gt_step4,G_WHITE
    movzx dx, byte [gui_page]
    cmp dx,3
    jbe .step
    mov dx,3
.step:
    imul dx,29
    add dx,228
    mov bx,56
    mov cx,3
    mov si,9
    mov al,10
    call g_rect
    G_RECT 52,384,530,1,G_DARK
    G_RECT 52,385,530,1,G_WHITE
    ret

g_nav:
    mov ah, 0
    cmp byte [gui_page], 0
    jne .back
    inc ah
.back:
    mov bx,296
    mov dx,396
    mov cx,88
    mov si,gt_back
    mov al,1
    call g_button
    mov ah,0
    cmp byte [gui_page],3
    jne .next
    cmp byte [gui_ack],1
    je .next
    inc ah
.next:
    cmp byte [gui_page],1
    jne .next_ready
    cmp byte [gui_disk_count],0
    jne .next_ready
    mov ah,1
.next_ready:
    mov bx,384
    mov dx,396
    mov cx,88
    mov si,gt_next
    cmp byte [gui_page],3
    jne .next_label
    mov si,gt_start
.next_label:
    mov al,2
    call g_button
    G_BUTTON 490,396,88,gt_cancel,3,0
    ret

gui_main:
    call g_init
    mov byte [gui_page],0
    mov byte [g_focus],2
    mov byte [gui_operation],0
    mov byte [gui_format],0
    call gui_scan_disks
.draw:
    call gui_draw
.event:
    call g_event
    cmp al,250
    je .draw
    cmp al,3
    je .cancel
    cmp al,1
    je .back
    cmp al,2
    je .next
    cmp al,4
    je .finish
    cmp al,30
    je .ack
    cmp al,10
    je .radio
    cmp al,11
    je .radio
    cmp al,20
    jb .event
    cmp al,27
    jae .event
    cmp byte [gui_page],1
    jne .event
    sub al,20
    cmp al,[gui_disk_count]
    jae .event
    mov [gui_disk_selection],al
    jmp .draw
.radio:
    sub al,10
    cmp byte [gui_page],0
    jne .format_radio
    mov [gui_operation],al
    jmp .draw
.format_radio:
    cmp byte [gui_page],2
    jne .event
    mov [gui_format],al
    jmp .draw
.ack:
    cmp byte [gui_page],3
    jne .event
    xor byte [gui_ack],1
    jmp .draw
.back:
    cmp byte [gui_page],0
    je .event
    cmp byte [gui_page],4
    je .event
    cmp byte [gui_page],5
    je .finish
    cmp byte [gui_page],6
    jne .previous
    mov byte [gui_page],1
    mov byte [setup_error],0
    mov byte [gui_ack],0
    mov byte [g_focus],2
    jmp .draw
.previous:
    dec byte [gui_page]
    mov byte [gui_ack],0
    mov byte [g_focus],2
    jmp .draw
.next:
    cmp byte [gui_page],5
    je .finish
    cmp byte [gui_page],3
    je .start
    ja .event
    cmp byte [gui_page],1
    jne .advance
    cmp byte [gui_disk_count],0
    je .event
    movzx bx,byte [gui_disk_selection]
    mov al,[gui_disks+bx]
    mov [raw_target_bios],al
    call setup_preflight
    jc .error
.advance:
    inc byte [gui_page]
    mov byte [g_focus],2
    jmp .draw
.start:
    cmp byte [gui_ack],1
    jne .event
    mov byte [gui_page],4
    mov byte [gui_cancelled],0
    mov byte [gui_phase],2
    mov byte [gui_percent],0
    mov byte [g_focus],3
    call gui_draw
    call setup_preflight
    jc .error
    cmp byte [gui_format],1
    jne .prepared
    call setup_full_surface
    jc .error
.prepared:
    cmp byte [gui_operation],1
    je .format_only
    mov byte [gui_phase],3
    xor al,al
    call gui_progress_update
    call raw_hdd_clone_install
    jc .error
    jmp .success
.format_only:
    call format_target_hdd
    jc .error
.success:
    mov byte [gui_page],5
    mov byte [g_focus],4
    jmp .draw
.error:
    mov byte [gui_page],6
    mov byte [g_focus],3
    jmp .draw
.cancel:
    cmp byte [gui_page],4
    je .event
    call g_exit
    ret
.finish:
    cmp byte [gui_page],5
    jne .event
    cmp byte [gui_operation],1
    je .cancel
    ; A reset must not depend on a video BIOS mode restore returning.
    call reboot_system
    ret

; Enumerate only real target BIOS disks, never the live source at 80h.
; EDD capacity is informational here; exact ATA identification gates all writes.
gui_scan_disks:
    pushad
    mov byte [gui_disk_count],0
    mov byte [gui_disk_selection],0
    cmp byte [source_drive],3
    jne .done
    mov dl,0x81
.next:
    push dx
    mov word [edd_drive_params],0x4A
    mov si,edd_drive_params
    mov ah,0x48
    call setup_bios_disk
    push cs
    pop ds
    pop dx
    jc .advance
    movzx bx,byte [gui_disk_count]
    mov [gui_disks+bx],dl
    shl bx,2
    mov eax,[edd_drive_params+16]
    shr eax,11
    mov [gui_disk_sizes+bx],eax
    inc byte [gui_disk_count]
.advance:
    inc dl
    cmp dl,0x88
    jb .next
.done:
    popad
    ret

gui_draw:
    mov al,[gui_page]
    mov dx,gt_marker_page
    call serial_write_z
    call serial_write_hex_byte
    call serial_write_crlf
    cmp word [g_comp_seg],0
    jne g_render_draw
    jmp gui_draw_scene

gui_draw_scene:
    call g_frame
    mov al,[gui_page]
    cmp al,0
    je .welcome
    cmp al,1
    je .disk
    cmp al,2
    je .format
    cmp al,3
    je .review
    cmp al,4
    je .progress
    cmp al,5
    je .done
    jmp .error
.welcome:
    G_TEXT 211,89,gt_welcome,G_BLACK,1
    G_TEXT 211,115,gt_tagline,G_BLACK
    G_TEXT 211,154,gt_action,G_BLACK
    mov ah,[gui_operation]
    xor ah,1
    G_RADIO 213,189,gt_install,10,ah
    G_TEXT 235,214,gt_install_detail,G_BLACK
    G_RADIO 213,251,gt_format_only,11,[gui_operation]
    G_TEXT 235,277,gt_format_detail,G_BLACK
    G_TEXT 211,338,gt_keyboard,G_DARK
    jmp .nav
.disk:
    G_TEXT 211,89,gt_destination,G_BLACK,1
    G_TEXT 211,115,gt_disk_choose,G_BLACK
    cmp byte [gui_disk_count],0
    je .no_disk
    mov byte [gui_row],0
.disk_row:
    movzx bx,byte [gui_row]
    mov al,[gui_disks+bx]
    sub al,0x80
    add al,'0'
    mov [gt_disk_number],al
    mov al,[gui_disks+bx]
    and al,15
    add al,'0'
    mov [gt_disk_bios_digit],al
    movzx dx,byte [gui_row]
    imul dx,27
    add dx,153
    mov bx,213
    mov si,gt_disk_name
    mov al,[gui_row]
    add al,20
    xor ah,ah
    mov cl,[gui_row]
    cmp cl,[gui_disk_selection]
    jne .disk_radio
    inc ah
.disk_radio:
    call g_radio
    movzx bx,byte [gui_row]
    shl bx,2
    mov eax,[gui_disk_sizes+bx]
    mov bx,454
    call g_number
    G_TEXT 515,dx,gt_mib_short,G_BLACK
    inc byte [gui_row]
    mov al,[gui_row]
    cmp al,[gui_disk_count]
    jb .disk_row
    G_TEXT 211,350,gt_disk_check,G_DARK
    jmp .nav
.no_disk:
    G_TEXT 211,166,gt_no_disk,G_BLACK,1
    G_TEXT 211,195,gt_no_disk_detail,G_BLACK
    G_TEXT 211,218,gt_live_required,G_BLACK
    jmp .nav
.format:
    G_TEXT 211,89,gt_format_heading,G_BLACK,1
    G_TEXT 211,115,gt_partition,G_BLACK
    mov eax,RAW_HDD_PARTITION_SECTORS / 2048
    mov bx,373
    mov dx,115
    call g_number
    G_TEXT 408,115,gt_mib,G_BLACK
    mov ah,[gui_format]
    xor ah,1
    G_RADIO 213,164,gt_quick,10,ah
    G_TEXT 235,192,gt_quick_detail,G_BLACK
    G_TEXT 235,211,gt_quick_detail2,G_BLACK
    G_RADIO 213,248,gt_full,11,[gui_format]
    G_TEXT 235,275,gt_full_detail,G_BLACK
    G_TEXT 235,294,gt_full_detail2,G_BLACK
    G_TEXT 211,350,gt_unallocated,G_DARK
    jmp .nav
.review:
    G_TEXT 211,89,gt_review,G_BLACK,1
    G_TEXT 211,119,setup_disk_model,G_BLACK
    mov eax,[setup_disk_mib]
    mov bx,211
    mov dx,141
    call g_number
    G_TEXT 274,141,gt_mib_disk,G_BLACK
    G_TEXT 211,172,gt_partition,G_BLACK
    mov eax,RAW_HDD_PARTITION_SECTORS / 2048
    mov bx,373
    mov dx,172
    call g_number
    G_TEXT 408,172,gt_mib,G_BLACK
    mov si,gt_quick
    cmp byte [gui_format],0
    je .method
    mov si,gt_full
.method:
    G_TEXT 211,198,si,G_BLACK
    mov si,gt_install
    cmp byte [gui_operation],0
    je .action_label
    mov si,gt_format_only
.action_label:
    G_TEXT 211,221,si,G_BLACK
    G_TEXT 211,254,gt_erase_warning,G_BLACK,1
    G_TEXT 211,275,gt_unallocated,G_BLACK
    G_RADIO 213,316,gt_ack,30,[gui_ack]
    G_TEXT 235,340,gt_ack2,G_BLACK
    jmp .nav
.progress:
    G_TEXT 211,89,gt_working,G_BLACK,1
    G_TEXT 211,119,setup_disk_model,G_BLACK
    G_TEXT 211,300,gt_wait,G_BLACK
    G_TEXT 211,323,gt_cancel_detail,G_DARK
    G_BUTTON 490,396,88,gt_cancel,3,0
    mov al,[gui_percent]
    call gui_progress_update
    jmp .shown
.done:
    G_TEXT 211,89,gt_complete,G_BLACK,1
    cmp byte [gui_operation],1
    je .formatted
    G_TEXT 211,152,gt_installed,G_BLACK
    G_TEXT 211,181,gt_verified,G_BLACK
    G_TEXT 211,240,gt_remove_cd,G_BLACK,1
    G_TEXT 211,267,gt_restart_detail,G_BLACK
    G_BUTTON 476,396,102,gt_restart,4,0
    ; Leaving the installer is still available without rebooting.
    G_BUTTON 364,396,102,gt_close,3,0
    jmp .shown
.formatted:
    G_TEXT 211,152,gt_formatted,G_BLACK
    G_TEXT 211,181,gt_formatted_detail,G_BLACK
    G_TEXT 211,240,gt_formatted_next,G_BLACK
    G_BUTTON 490,396,88,gt_finish,4,0
    jmp .shown
.error:
    G_TEXT 211,89,gt_stopped,G_BLACK,1
    cmp byte [gui_cancelled],1
    jne .error_reason
    G_TEXT 211,138,gt_cancelled,G_BLACK
    jmp .error_detail
.error_reason:
    mov si,gt_io_error
    cmp byte [setup_error],1
    jne .identify_error
    mov si,gt_source_error
    jmp .reason
.identify_error:
    cmp byte [setup_error],2
    jne .size_error
    mov si,gt_target_error
    jmp .reason
.size_error:
    cmp byte [setup_error],3
    jne .reason
    mov si,gt_size_error
.reason:
    G_TEXT 211,138,si,G_BLACK
.error_detail:
    G_TEXT 211,179,gt_lba,G_BLACK
    mov eax,[raw_clone_lba_lo]
    mov bx,279
    mov dx,179
    call g_number
    G_TEXT 211,224,gt_stopped_detail,G_BLACK
    G_TEXT 211,249,gt_retry_detail,G_BLACK
    G_BUTTON 354,396,112,gt_try_again,1,0
    G_BUTTON 490,396,88,gt_close,3,0
    jmp .shown
.nav:
    call g_nav
.shown:
    call g_pointer_show
    ret

; Decimal EAX, at BX/DX. Full 32-bit disk capacities never wrap at 64 MiB.
g_number:
    pushad
    mov di,g_number_buf+11
    mov byte [di],0
    mov ecx,10
    push dx
.next:
    xor edx,edx
    div ecx
    dec di
    add dl,'0'
    mov [di],dl
    test eax,eax
    jnz .next
    pop dx
    mov si,di
    xor ax,ax
    call g_text
    popad
    ret

; AL=percent, [gui_phase]=1 full format, 2 quick format, 3 install, 4 readback.
gui_progress_update:
    mov [gui_percent],al
    cmp word [g_comp_seg],0
    je .paint
    cmp byte [g_composing],1
    je .paint
    jmp g_render_progress
.paint:
    pushad
    cmp byte [gui_active],1
    jne .done
    cmp byte [g_composing],1
    je .hidden
    call g_pointer_hide
.hidden:
    mov [gui_percent],al
    G_RECT 211,168,361,96,G_GRAY
    mov si,gt_phase_metadata
    cmp byte [gui_phase],1
    jne .copy
    mov si,gt_phase_surface
    jmp .phase
.copy:
    cmp byte [gui_phase],4
    jne .copy_files
    mov si,gt_phase_verify
    jmp .phase
.copy_files:
    cmp byte [gui_phase],3
    jne .phase
    mov si,gt_phase_copy
.phase:
    G_TEXT 211,170,si,G_BLACK,1
    G_RECT 211,202,360,24,G_DARK
    G_RECT 212,203,358,22,G_WHITE
    G_RECT 213,204,356,20,G_GRAY
    movzx ax,byte [gui_percent]
    imul ax,350
    xor dx,dx
    mov cx,100
    div cx
    mov cx,ax
    mov bx,216
    mov dx,207
    mov si,14
    mov al,G_BLUE
    call g_rect
    movzx eax,byte [gui_percent]
    mov bx,211
    mov dx,241
    call g_number
    G_TEXT 239,241,gt_percent,G_BLACK
    call g_pointer_show
.done:
    popad
    ret

gui_cancel_poll:
    pushad
    cmp byte [gui_active],1
    jne .ok
    call g_poll
    cmp al,3
    jne .ok
    mov byte [gui_cancelled],1
    mov byte [raw_last_stage],'X'
    mov byte [raw_last_status],0xF4
    mov dx,gt_marker_cancelled
    call serial_write_z
    call serial_write_crlf
    popad
    stc
    ret
.ok:
    popad
    clc
    ret

; Legacy call sites use these names; the current live entry owns the wizard.
visual_main_loop equ gui_main
vis_install_screen_init equ gui_draw
vis_clone_phase_update equ gui_progress_update
vis_install_phase_clone:
    mov byte [gui_phase],3
    ret
vis_install_phase_done:
vis_install_eject_prompt:
vis_install_fail_prompt:
    ret

; Exact classic palette: black, navy, teal, silver, gray and white.
g_attributes db 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0
g_palette:
    CIUKIOS_PALETTE
    CIUKI_LOGO_PALETTE
    UI_ICONS_PALETTE
g_pointer_black dw 0x8000,0xC000,0xE000,0xF000,0xF800,0xFC00,0xFE00,0xFF00,0xFF80,0xFFC0,0xFE00,0xEF00,0xCF00,0x8780,0x0780,0x0300
g_pointer_white dw 0,0,0x4000,0x6000,0x7000,0x7800,0x7C00,0x7E00,0x7F00,0x7C00,0x6C00,0x4600,0x0600,0x0300,0x0300,0

gt_brand db 'CiukiOS',0
gt_title_short db 'Setup',0
gt_title db 'CiukiOS Setup',0
gt_welcome db 'CiukiOS Setup',0
gt_tagline db 'A modern Retro OS',0
gt_action db 'Choose an operation.',0
gt_install db 'Install CiukiOS',0
gt_install_detail db 'Install the system, applications and drivers.',0
gt_format_only db 'Format a disk',0
gt_format_detail db 'Create an empty FAT16 system partition.',0
gt_keyboard db 'Tab: buttons    Arrows: options    Enter: continue',0
gt_step1 db '1  Operation',0
gt_step2 db '2  Destination',0
gt_step3 db '3  Format',0
gt_step4 db '4  Review',0
gt_back db '< Back',0
gt_next db 'Next >',0
gt_start db 'Start',0
gt_cancel db 'Cancel',0
gt_close db 'Close',0
gt_finish db 'Finish',0
gt_restart db 'Restart',0
gt_try_again db '< Try again',0
gt_destination db 'Select the destination disk',0
gt_disk_choose db 'The selected disk will be repartitioned.',0
gt_disk_name db 'Hard disk '
gt_disk_number db '1'
    db '   (BIOS 8'
gt_disk_bios_digit db '1'
    db 'h)',0
gt_disk_check db 'Next checks the disk model and available capacity.',0
gt_no_disk db 'No destination disk is available.',0
gt_no_disk_detail db 'Connect an IDE hard disk and start from the live CD.',0
gt_live_required db 'The running system cannot be used as a destination.',0
gt_format_heading db 'Choose a format method',0
gt_partition db 'CiukiOS system partition:',0
gt_mib db 'MiB / FAT16',0
gt_mib_disk db 'MiB total disk capacity',0
gt_quick db 'Quick format',0
gt_quick_detail db 'Create a new filesystem. Skip the surface check.',0
gt_quick_detail2 db 'Installed files are still read back and checked.',0
gt_full db 'Full format (slower)',0
gt_full_detail db 'Clear and read back every sector in the system',0
gt_full_detail2 db 'partition. Stop if a sector cannot be read or written.',0
gt_unallocated db 'Remaining disk space will be unallocated.',0
gt_review db 'Ready to begin',0
gt_erase_warning db 'All existing volumes on this disk will be removed.',0
gt_ack db 'I want to erase the selected disk and continue.',0
gt_ack2 db 'Click the box or press Space to confirm.',0
gt_working db 'Preparing your disk',0
gt_phase_surface db 'Clearing and checking the system partition',0
gt_phase_metadata db 'Creating and checking the FAT16 filesystem',0
gt_phase_copy db 'Copying and checking system files',0
gt_phase_verify db 'Verifying the installed disk before startup',0
gt_percent db '% complete',0
gt_wait db 'Keep the computer powered on.',0
gt_cancel_detail db 'Cancel stops the operation; an unfinished disk cannot boot.',0
gt_complete db 'Setup is complete',0
gt_installed db 'CiukiOS has been installed on the selected disk.',0
gt_verified db 'System data was read back and checked.',0
gt_remove_cd db 'Remove the CD before restarting.',0
gt_restart_detail db 'Click Restart to start CiukiOS from the hard disk.',0
gt_formatted db 'The FAT16 system partition is ready.',0
gt_formatted_detail db 'Formatting does not install an operating system.',0
gt_formatted_next db 'Run Setup again and choose Install CiukiOS to install.',0
gt_stopped db 'Setup stopped',0
gt_cancelled db 'The operation was cancelled.',0
gt_io_error db 'The disk could not be read, written or verified.',0
gt_source_error db 'The live image is missing or has an invalid layout.',0
gt_target_error db 'This disk or controller is not supported.',0
gt_size_error db 'The selected disk is too small for this system.',0
gt_lba db 'Sector:',0
gt_stopped_detail db 'Setup did not finish the requested operation.',0
gt_retry_detail db 'Check the disk and try again, or return to the shell.',0
gt_marker_page db '[SETUP-GUI] PAGE ',0
gt_marker_cancelled db '[SETUP-GUI] CANCELLED',0

gui_active db 0
gui_page db 0
gui_operation db 0
gui_format db 0
gui_ack db 0
gui_cancelled db 0
gui_phase db 0
gui_percent db 0
gui_disk_count db 0
gui_disk_selection db 0
gui_disks times 7 db 0
gui_disk_sizes times 7 dd 0
gui_row db 0
g_widget_count db 0
g_widgets times 16*10 db 0
g_focus db 2
g_color db 0
g_x dw 0
g_y dw 0
g_width dw 0
g_height dw 0
g_span dw 0
g_left_mask db 0
g_right_mask db 0
g_text_color db 0
g_font_ptr dw 0
g_advance db 0
g_bevel_height dw 0
g_bevel_width dw 0
g_button_label dw 0
g_button_event dw 0
g_radio_selected db 0
g_number_buf times 12 db 0
g_mouse_ready db 0
g_mouse_saved db 0
g_mouse_previous db 0
g_mouse_pressed db 0
g_mouse_buttons dw 0
g_mouse_x dw 580
g_mouse_y dw 435
g_mouse_state times 512 db 0
g_pointer_visible db 0
g_pointer_offset dw 0
g_pointer_color db 0
g_pointer_under times 384 db 0
g_font incbin 'src/com/setup_font.bin'

gt_mib_short db 'MiB',0

g_glyph_shift db 0

%include "src/com/setup_render.inc"
%define CIUKI_LOGO_RECT g_rect
%define CIUKI_LOGO_VBE g_vbe
%define CIUKI_LOGO_COMPOSING g_composing
%define CIUKI_LOGO_BAND_TOP g_comp_top
%define CIUKI_LOGO_BAND_BOTTOM g_comp_bottom
%define CIUKI_LOGO_ORIGIN_Y [g_origin_y]
%include "src/com/ciuki_logo.inc"
