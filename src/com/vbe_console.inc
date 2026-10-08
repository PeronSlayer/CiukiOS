; Checked VBE console: indexed and 15/16/24/32-bit direct color, through a
; linear framebuffer when firmware and CPU mode allow it, else banked.
; Private framebuffer rendering only: no resident hooks or DOS ABI changes.
; Public calls preserve general/segment registers unless documented otherwise.
%ifndef VC_PROTECTED_LFB
%define VC_PROTECTED_LFB 1
%endif

vc_bios:
    pushf
    pushad
    push ds
    push es
    push fs
    push gs
    int 0x10
    mov [cs:vc_bios_result], ax
    mov [cs:vc_bios_bx], bx
    mov [cs:vc_bios_cx], cx
    mov [cs:vc_bios_dx], dx
    mov [cs:vc_bios_bp], bp
    mov [cs:vc_bios_es], es
    pop gs
    pop fs
    pop es
    pop ds
    popad
    mov ax, [cs:vc_bios_result]
    popf
    cld
    ret

; Read the common profile. AUTO follows EDID when supplied by firmware.
vc_load_profile:
    pushad
    push ds
    push cs
    pop ds
    mov word [vc_mode], 0
    cmp byte [vc_force_safe],0
    jne .done
    mov dword [vc_profile], 'AUTO'
    mov dx, vc_profile_path
    mov ax, 0x3D00
    int 0x21
    jc .automatic
    mov bx, ax
    mov dx, vc_profile
    mov cx, 4
    mov ah, 0x3F
    int 0x21
    pushf
    push ax
    mov ah, 0x3E
    int 0x21
    pop ax
    popf
    jc .done
    cmp ax, 4
    jne .done
    cmp byte [vc_profile],'M'
    jne .named_profile
    mov si,vc_profile
    call vc_profile_mode
    jc .done
    mov [vc_mode],ax
    jmp .done
.named_profile:
    cmp dword [vc_profile], 'AUTO'
    je .automatic
    mov ax,640
    mov dx,480
    cmp dword [vc_profile], '0640'
    je .resolve
    mov ax,800
    mov dx,600
    cmp dword [vc_profile], '0800'
    je .resolve
    mov ax,1024
    mov dx,768
    cmp dword [vc_profile], '1024'
    je .resolve
    mov ax,1280
    mov dx,1024
    cmp dword [vc_profile],'1280'
    je .resolve
    mov ax,1600
    mov dx,1200
    cmp dword [vc_profile],'1600'
    je .resolve
    mov ax,1920
    mov dx,1080
    cmp dword [vc_profile],'1920'
    je .resolve
    mov ax,2048
    mov dx,1152
    cmp dword [vc_profile],'2048'
    je .resolve
    mov ax,2560
    mov dx,1440
    cmp dword [vc_profile],'2560'
    je .resolve
    jmp .done
.resolve:
    call vc_resolve_mode
    jmp .done
.automatic:
    call vc_auto
.done:
    pop ds
    popad
    ret

; DS:SI = M followed by three hexadecimal digits. The four-byte exact-mode
; profile coexists with AUTO and the historical resolution preferences.
; CF=0/AX=mode only for firmware VBE IDs 100h..FFFh; never accepts mode flags.
vc_profile_mode:
    push bx
    push cx
    push si
    xor bx,bx
    mov cx,3
    inc si
.digit:
    lodsb
    sub al,'0'
    cmp al,9
    jbe .add
    sub al,7
    cmp al,10
    jb .bad
    cmp al,15
    ja .bad
.add:
    shl bx,4
    or bl,al
    loop .digit
    cmp bx,0x100
    jb .bad
    mov ax,bx
    clc
    jmp .done
.bad:
    stc
.done:
    pop si
    pop cx
    pop bx
    ret

; Validate the requested standard mode and its writable 64 KiB bank window.
vc_probe:
    pushad
    push es
    call vc_controller_query
    push cs
    pop es
    mov di, vc_info
    mov cx, 128
    xor ax, ax
    cld
    rep stosw
    mov di, vc_info
    mov cx, [cs:vc_mode]
    mov ax, 0x4F01
    call vc_bios
%ifdef VC_TRACE_DOS
    mov [vc_probe_status],ax
%endif
    cmp ax, 0x004F
    jne .fail
    call vc_validate
    jc .fail
    clc
    jmp .done
.fail:
    stc
.done:
    pop es
    popad
    ret

; Enter a checked mode. Failure always restores ordinary VGA text.
vc_begin:
    pushad
    push es
    ; Existing native ownership must drain before even an attempted mode set.
    cmp byte [vc_session_owned],1
    jne .unbound
    call vc_session_fb_release
    jc .done
.unbound:
    mov byte [vc_begin_stage],1
    call vc_controller_query
    call vc_probe
    jc .fail
    mov byte [vc_begin_stage],2
    cmp word [cs:vc_cells_seg], 0
    jne .allocated
    mov ax, [cs:vc_cols]
    mul word [cs:vc_rows]
    shl ax, 1
    mov [cs:vc_cell_off],ax
    add ax,512+15              ; transient glyph pixels follow cached cells
    jc .fail
    shr ax, 4
    mov bx, ax
    mov ah, 0x48
    int 0x21
    jc .fail
    mov [cs:vc_cells_seg], ax
.allocated:
    cmp byte [cs:vc_import_text], 1
    jne .font
    call vc_capture_text
.font:
    mov ax, 2
    int 0x33                 ; this console draws its own keyboard cursor
    mov ax, 0x1130
    mov bh, 6
    call vc_bios
    ; 1130h returns a far ROM-font pointer in ES:BP. Capture its outputs
    ; without keeping firmware scratch segments/upper registers in the UI.
    mov ax,[cs:vc_bios_bp]
    mov [cs:vc_font_off],ax
    mov ax,[cs:vc_bios_es]
    mov [cs:vc_font_seg],ax
    ; Prefer the linear framebuffer: every bank switch is a BIOS call, and
    ; under hardware virtualization a costly guest memory remap.
    mov byte [cs:vc_lfb], 0
    mov byte [vc_begin_stage],3
    mov bx, [cs:vc_mode]
    cmp byte [cs:vc_lfb_ok], 1
    jne .banked
    smsw ax
    test al,1
    jnz .protected_open
    call vc_lfb_open
    jc .banked
    mov byte [cs:vc_lfb],1
    jmp .linear_set
.protected_open:
    mov byte [cs:vc_lfb],2
.linear_set:
    mov eax, [cs:vc_info+40]
    mov [cs:vc_lfb_base], eax
    mov bx,[cs:vc_mode]
    or bx, 0x4000
    mov ax, 0x4F02
    call vc_bios
    cmp ax, 0x004F
    jne .linear_refused
    jmp .mode_set
.linear_refused:
    mov byte [cs:vc_lfb],0
    call vc_lfb_close
.banked:
    mov byte [vc_begin_stage],4
    ; A VBE 3.0 banked mode can have a different stride, page count and RGB
    ; layout. Restore its checked descriptor after either LFB-open/set failure.
    call vc_use_banked_layout
    call vc_validate_active
    jc .fail
    cmp byte [cs:vc_bank_ok], 1
    jne .fail
    mov bx, [cs:vc_mode]
    mov ax, 0x4F02
    call vc_bios
    cmp ax, 0x004F
    jne .fail
.mode_set:
    mov byte [vc_begin_stage],5
    ; A successful set alone does not establish the framebuffer geometry.
    ; VBE 2.0/3.0 Function 4F03h D14 reports the active access model.
    mov ax,0x4F03
    call vc_bios
    cmp ax,0x004F
    jne .fail
    mov ax,[vc_bios_bx]
    mov dx,ax
    shr dx,14
    and dl,1
    cmp byte [vc_lfb],0
    setne dh
    cmp dl,dh
    jne .fail
    and ax,0x3FFF
    cmp ax,[vc_mode]
    jne .fail
    mov ax,[vc_info+20]
    mov [vc_scan_lines],ax
    ; Broken firmware may omit 4F06: retain the checked mode-info pitch and
    ; only one visible frame's scanlines, preventing unverified page flips.
    mov ax,0x4F06
    mov bx,1
    call vc_bios
    cmp ax,0x004F
    jne .skip_4f06
    ; All 4F06 outputs form one byte-addressed tuple. The T23 may return
    ; a stride eight times too small despite AX=004F and plausible CX/DX.
    ; Reject that reply without poisoning the validated 4F01 layout.
    mov bx,[vc_bios_bx]
    mov ax,bx
    xor dx,dx
    movzx cx,byte [vc_bytes]
    div cx
    cmp ax,[vc_bios_cx]
    jne .skip_4f06
    cmp ax,[vc_info+18]
    jb .skip_4f06
    mov ax,[vc_bios_dx]
    cmp ax,[vc_info+20]
    jb .skip_4f06
    ; TotalMemory is in 64 KiB units. DX:AX / byte pitch yields rows;
    ; when its quotient would exceed 65535, every returned 16-bit DX fits.
    mov dx,[vc_controller+18]
    cmp dx,bx
    jae .scanline_extent_ok
    xor ax,ax
    div bx
    cmp ax,[vc_bios_dx]
    jb .skip_4f06
.scanline_extent_ok:
    mov ax,[vc_bios_dx]
    mov [vc_scan_lines],ax
    mov [vc_info+16],bx
.skip_4f06:
    mov byte [vc_begin_stage],6
    call vc_validate_active
    jc .fail
    mov al,[cs:vc_bank_ok]
    cmp byte [cs:vc_lfb],0
    je .access_checked
    mov al,[cs:vc_lfb_ok]
.access_checked:
    cmp al,1
    jne .fail
    cmp byte [cs:vc_lfb],2
    jne .mapping_ready
    mov byte [vc_begin_stage],7
    call vc_session_fb_bind
    jc .fail
.mapping_ready:
    ; A freshly cleared mode starts at page zero even if 4F07 is unavailable.
    mov ax,0x4F07
    xor bx,bx
    xor cx,cx
    xor dx,dx
    call vc_bios
    mov byte [cs:vc_active], 1
    call vc_batch_reset
    mov word [cs:vc_bank], 0xFFFF
    push cs
    pop es
    mov dx, vc_palette
    mov bx, 0
    mov cx, 16
    call vc_set_palette
    ; The mode set (D15 clear) has already cleared display memory; a second
    ; software clear of every page only delays the first frame.
    cmp byte [cs:vc_import_text], 1
    jne .blank
    call vc_redraw
    jmp .cleared
.blank:
    call vc_clear_cells
.cleared:
    mov byte [cs:vc_import_text], 0
    cmp byte [cs:vc_active], 1
    jne .fail
%ifdef VC_TRACE_DOS
    xor dl,dl
    call vc_trace
%endif
    clc
    jmp .done
.fail:
    mov [vc_failed_status],ax
    mov al,[vc_begin_stage]
    mov [vc_last_failure],al
    mov ax,[vc_mode]
    mov [vc_failed_mode],ax
%ifdef VC_TRACE_DOS
    mov dl,1
    call vc_trace
    call vc_gpu_log
%endif
    call vc_end
    stc
.done:
    pop es
    popad
    ret

vc_end:
    pushad
    push es
    call vc_session_fb_release
    jc .done                   ; retain mode/mapping while an engine is busy
    mov byte [cs:vc_active], 0
    cmp byte [cs:vc_graphics_transition],0
    jne .release
    mov ax, 3
    call vc_bios
.release:
    mov byte [cs:vc_lfb], 0
    call vc_lfb_close
    mov ax, [cs:vc_cells_seg]
    test ax, ax
    jz .done
    mov es, ax
    mov ah, 0x49
    int 0x21
    mov word [cs:vc_cells_seg], 0
    clc
    jmp .done
.done:
    pop es
    popad
    ret

%ifdef VC_TRACE_DOS
; Preserve every attempted raw descriptor, including a rejected AUTO mode.
; CVT1 records are16-byte headers plus256-byte ModeInfo; at most128 per boot.
vc_trace:
    pushf
    pushad
    push ds
    push cs
    pop ds
    cmp byte [vc_trace_count],128
    jae .done
    mov [file_buf+7],dl
    mov dword [file_buf],'CVT1'
    mov ax,[vc_mode]
    mov [file_buf+4],ax
    mov al,[vc_begin_stage]
    mov [file_buf+6],al
    mov ax,[vc_probe_status]
    mov [file_buf+8],ax
    mov al,[vc_lfb]
    mov ah,[vc_start_attempts]
    mov [file_buf+10],ax
    mov ax,[vc_failed_status]
    mov [file_buf+12],ax
    mov word [file_buf+14],0
    mov dx,vc_trace_path
    cmp byte [vc_trace_count],0
    jne .open
    xor cx,cx
    mov ah,0x3C
    jmp .request
.open:
    mov ax,0x3D02
.request:
    int 0x21
    jc .done
    mov bx,ax
    xor cx,cx
    xor dx,dx
    mov ax,0x4202
    int 0x21
    jc .close
    mov dx,file_buf
    mov cx,16
    mov ah,0x40
    int 0x21
    jc .close
    cmp ax,cx
    jne .close
    mov dx,vc_info
    mov cx,256
    mov ah,0x40
    int 0x21
    jc .close
    cmp ax,cx
    jne .close
    inc byte [vc_trace_count]
.close:
    mov ah,0x3E
    int 0x21
.done:
    pop ds
    popad
    popf
    ret
vc_probe_status dw 0
vc_trace_count db 0
vc_trace_path db '\SYSTEM\VIDEO\VBE.TRC',0
%endif

; EDI is the linear pixel offset. Map its 64 KiB page; preserve all registers.
vc_map:
    pushad
    cmp byte [cs:vc_active],1
    jne .inactive
    cmp byte [cs:vc_lfb],0
    jne .inactive
    cmp edi,[cs:vc_access_bytes]
    jae .fail
    mov eax, edi
    shr eax, 16
    cmp ax, [cs:vc_bank]
    je .ready
    mov [cs:vc_bank], ax
    mul word [cs:vc_bank_step]
    mov dx, ax
    xor bx, bx
    mov ax, 0x4F05
    call vc_bios
    cmp ax, 0x004F
    jne .fail
.ready:
    mov ax, [cs:vc_window]
    mov es, ax
    clc
    jmp .done
.fail:
    mov byte [cs:vc_active], 0
    mov ax, 3
    call vc_bios
.inactive:
    stc
.done:
    popad
    ret

vc_clear:
    call vc_clear_cells
    jmp vc_clear_video

vc_clear_cells:
    pushad
    push es
    mov es, [cs:vc_cells_seg]
    xor di, di
    mov ax, [cs:vc_cols]
    mul word [cs:vc_rows]
    mov cx, ax
    mov ax, 0x0720
    cld
    rep stosw
    mov word [cs:vc_x], 0
    mov word [cs:vc_y], 1
    mov byte [cs:vc_attr], 7
    pop es
    popad
    ret

vc_clear_video:
    ; Clear pitch padding as well, one scanline burst at a time. Treat the
    ; surface as bytes so an odd pitch still clears its final byte.
    pushad
    mov al,[cs:vc_bytes]
    push ax
    mov byte [cs:vc_bytes],1
    xor eax,eax
    xor edi,edi
    movzx ecx,word [cs:vc_pitch]
    mov dx,[cs:vc_info+20]
.row:
    call vc_span
    add edi,ecx
    dec dx
    jnz .row
    pop ax
    mov [cs:vc_bytes],al
    popad
    ret

; Snapshot a native text child's output before switching back to VBE.
%ifndef VC_GRAPHICS_ONLY
vc_capture_text:
    pushad
    push ds
    push es
    call vc_clear_cells
    mov ax, 0x0040
    mov es, ax
    mov dx, [es:0x50]
    movzx bp, byte [es:0x84]
    inc bp
    cmp bp, 50
    jbe .rows_ok
    mov bp, 25
.rows_ok:
    movzx ax, dh
    xor si, si
    cmp ax, [cs:vc_rows]
    jb .cursor_ok
    sub ax, [cs:vc_rows]
    inc ax
    sub bp, ax
    imul si, ax, 160
    mov ax, [cs:vc_rows]
    dec ax
    mov dh, al
.cursor_ok:
    movzx ax, dl
    mov [cs:vc_x], ax
    movzx ax, dh
    mov [cs:vc_y], ax
    cmp bp, [cs:vc_rows]
    jbe .copy
    mov bp, [cs:vc_rows]
.copy:
    mov es, [cs:vc_cells_seg]
    mov ax, 0xB800
    mov ds, ax
    xor di, di
.row:
    mov cx, 80
    rep movsw
    mov ax, [cs:vc_cols]
    sub ax, 80
    shl ax, 1
    add di, ax
    dec bp
    jnz .row
    pop es
    pop ds
    popad
    ret

vc_redraw:
    pushad
    push gs
    mov gs, [cs:vc_cells_seg]
    mov ax, [cs:vc_cols]
    mul word [cs:vc_rows]
    mov bp, ax
    mov bx, [cs:vc_cols]       ; the caller draws the title after this import
.cell:
    mov si, bx
    shl si, 1
    mov ax, [gs: si]
    cmp al, ' '
    jne .draw
    test ah, 0xF0
    jz .next                  ; clear_video already painted blank black cells
.draw:
    call vc_draw_cell
.next:
    inc bx
    cmp bx, bp
    jb .cell
    pop gs
    popad
    ret

; BX is a cell index. Compose one ROM 8x16 glyph with its background in
; memory, then write its sixteen rows.
vc_draw_cell:
    cmp byte [cs:vc_batch],1
    je vc_queue_cell
    pushad
    push es
    push fs
    push gs
    cmp byte [cs:vc_active], 1
    jne .done
    mov ax, bx
    xor dx, dx
    div word [cs:vc_cols]
    movzx edi, ax
    shl edi, 4
    movzx ecx, word [cs:vc_pitch]
    imul edi, ecx
    movzx eax, dx
    shl eax, 3
    movzx ecx, byte [cs:vc_bytes]
    imul eax, ecx
    add edi, eax
    mov gs, [cs:vc_cells_seg]
    mov fs, [cs:vc_font_seg]
    mov es,[cs:vc_cells_seg]
    cld
    push edi
    mov di,[cs:vc_cell_off]
    xor dx, dx
.scan:
    call vc_cell_scan
    inc dx
    cmp dx, 16
    jb .scan
    pop edi
    mov fs,[cs:vc_cells_seg]
    mov si,[cs:vc_cell_off]
    movzx cx, byte [cs:vc_bytes]
    shl cx, 3
    mov bx, cx
    mov dx, 16
    call vc_fb_put_rows
.done:
    pop gs
    pop fs
    pop es
    popad
    ret

; Compose scanline DX of cell BX (GS = cells, FS = font) at ES:DI,
; advancing DI by eight pixels.
vc_cell_scan:
    push eax
    push bx
    push cx
    push si
    shl bx, 1
    movzx si, byte [gs: bx]
    shl si, 4
    add si, [cs:vc_font_off]
    add si, dx
    mov cl, [fs: si]
    mov bl, [gs: bx + 1]
    mov ch, 8
.pixel:
    movzx si, bl
    shl cl, 1
    jc .foreground
    shr si, 4
.foreground:
    and si, 15
    shl si, 2
    mov eax, [cs:vc_colors + si]
    call vc_store_pixel
    dec ch
    jnz .pixel
    pop si
    pop cx
    pop bx
    pop eax
    ret

vc_cell_index:
    mov ax, [cs:vc_y]
    mul word [cs:vc_cols]
    add ax, [cs:vc_x]
    mov bx, ax
    ret

vc_cursor:
    pushad
    push es
    cmp byte [cs:vc_active], 1
    jne .done
    cmp byte [cs:vc_batch],1
    je .done
    movzx edi, word [cs:vc_y]
    shl edi, 4
    add edi, 14
    movzx eax, word [cs:vc_pitch]
    imul edi, eax
    movzx eax, word [cs:vc_x]
    shl eax, 3
    movzx ecx, byte [cs:vc_bytes]
    imul eax, ecx
    add edi, eax
    mov eax, [cs:vc_colors + 15*4]
    mov ecx, 8
    call vc_span
.done:
    pop es
    popad
    ret

vc_putc:
    pushad
    push gs
    mov gs, [cs:vc_cells_seg]
    push ds
    push es
    push cs
    pop ds
    cld
    mov [vc_char], al
    call vc_cell_index
    cmp byte [vc_char],32
    jae .printable
    call vc_draw_cell           ; remove cursor only for movement/control
.printable:
    mov al, [vc_char]
    cmp al, 13
    je .cr
    cmp al, 10
    je .lf
    cmp al, 8
    je .back
    cmp al, 9
    je .tab
    cmp al, 32
    jb .cursor
    mov ah, [vc_attr]
    shl bx, 1
    mov [gs: bx], ax
    shr bx, 1
    call vc_draw_cell
    inc word [vc_x]
    mov ax, [vc_x]
    cmp ax, [vc_cols]
    jb .cursor
.cr_wrap:
    mov word [vc_x], 0
.lf:
    inc word [vc_y]
    mov ax, [vc_y]
    cmp ax, [vc_rows]
    jb .cursor
    call vc_scroll
    dec word [vc_y]
    jmp .cursor
.cr:
    mov word [vc_x], 0
    jmp .cursor
.back:
    cmp word [vc_x], 0
    jne .back_col
    cmp word [vc_y], 1
    jbe .cursor
    dec word [vc_y]
    mov ax, [vc_cols]
    mov [vc_x], ax
.back_col:
    dec word [vc_x]
    jmp .cursor
.tab:
    add word [vc_x], 8
    and word [vc_x], 0xFFF8
    mov ax, [vc_x]
    cmp ax, [vc_cols]
    jae .cr_wrap
.cursor:
    call vc_cursor
    call vc_maybe_flush
    pop es
    pop ds
    pop gs
    popad
    ret

vc_scroll:
    pushad
    push gs
    mov gs, [cs:vc_cells_seg]
    ; Repaint only cells whose glyph/colour changed. Full-screen redraws
    ; on every newline make banked video unnecessarily slow on old VGA.
    mov bx, [cs:vc_cols]
    mov ax, [cs:vc_rows]
    dec ax
    mul word [vc_cols]
    mov bp, ax
.shift:
    mov si, bx
    add si, [cs:vc_cols]
    shl si, 1
    mov ax, [gs: si]
    mov di, bx
    shl di, 1
    cmp ax, [gs: di]
    je .next
    mov [gs: di], ax
    call vc_draw_cell
.next:
    inc bx
    cmp bx, bp
    jb .shift
    add bp, [cs:vc_cols]
.last:
    mov di, bx
    shl di, 1
    cmp word [gs: di], 0x0720
    je .last_next
    mov word [gs: di], 0x0720
    call vc_draw_cell
.last_next:
    inc bx
    cmp bx, bp
    jb .last
    pop gs
    popad
    ret

vc_title:
    pushad
    push gs
    mov gs, [cs:vc_cells_seg]
    xor bx, bx
.background:
    mov word [gs: bx], 0x1F20
    add bx, 2
    mov ax, [cs:vc_cols]
    shl ax, 1
    cmp bx, ax
    jb .background
    mov bx, [cs:vc_cols]
    sub bx, vc_title_end - vc_title_text
    and bx, 0xFFFE            ; center text, then convert index to byte offset
    mov si, vc_title_text
.letters:
    lodsb
    mov [gs: bx], al
    add bx, 2
    cmp si, vc_title_end
    jb .letters
    xor bx, bx
.draw:
    call vc_draw_cell
    inc bx
    cmp bx, [cs:vc_cols]
    jb .draw
    call vc_cursor
    pop gs
    popad
    ret

%else
; Graphics clients never import or print a DOS text surface.
vc_capture_text:
vc_redraw:
vc_batch_reset:
    ret
%endif
vc_graphics_transition db 0
vc_profile_path db '\SYSTEM\VIDEO\DISPLAY.CFG', 0
vc_profile dd 'AUTO'
vc_mode dw 0
vc_active db 0
vc_import_text db 0
vc_char db 0
vc_attr db 7
vc_x dw 0
vc_y dw 1
vc_cols dw 80
vc_rows dw 30
vc_pitch dw 640
vc_window dw 0xA000
vc_font_off dw 0
vc_font_seg dw 0
vc_bios_bp dw 0
vc_bios_es dw 0
vc_bank dw 0xFFFF
vc_bank_step dw 1
vc_bios_result dw 0
vc_title_text db 'CiukiOS'
vc_title_end:
vc_palette db 0,0,0, 0,0,32, 0,42,0, 0,42,42, 42,0,0, 42,0,42, 42,21,0, 42,42,42
           db 21,21,21, 21,21,63, 21,63,21, 21,63,63, 63,21,21, 63,21,63, 63,63,21, 63,63,63
align 4
vc_info times 256 db 0
vc_cells_seg dw 0 ; allocated only while the graphical console is active
vc_cell_off dw 0 ; 512-byte scratch lives in the allocated cells block

%include "src/com/vbe_modes.inc"
%include "src/com/vbe_fb.inc"
%ifndef VC_GRAPHICS_ONLY
%include "src/com/vbe_text_batch.inc"
%endif
