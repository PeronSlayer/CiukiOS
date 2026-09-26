VC_MODE_LIMIT equ 256
VC_ROW_LIMIT equ 256
; VBE 2/3 banked and linear modes. Linear access is confined to the bursts
; in vbe_fb.inc; no flat segment or protected-mode state outlives a burst.
; EDID preferred timing bounds an LCD's usable desktop when firmware supplies it.
vc_auto:
    pushad
    push es
    push fs
    cmp byte [vc_auto_done],1
    je .cached
    mov byte [vc_auto_done],1
    push cs
    pop es
    mov di,vc_controller
    mov cx,256
    xor ax,ax
    rep stosw
    mov dword [vc_controller],'VBE2'
    mov di,vc_controller
    mov ax,0x4F00
    call vc_bios
    cmp ax,0x004F
    jne .cached
    cmp dword [vc_controller],'VESA'
    jne .cached
    cmp word [vc_controller+4],0x0200
    jb .cached
    ; Copy the far mode list before subsequent BIOS calls reuse scratch RAM.
    lfs si,[vc_controller+14]
    mov di,vc_mode_list
    mov cx,VC_MODE_LIMIT
.copy:
    mov ax,[fs:si]
    cmp ax,0xFFFF
    je .edid
    stosw
    add si,2
    jc .edid
    inc word [vc_mode_count]
    loop .copy
.edid:
    mov ax,0x4F15
    mov bx,1
    xor cx,cx
    xor dx,dx
    mov di,vc_edid
    call vc_bios
    cmp ax,0x004F
    jne .choose
    cmp dword [vc_edid],0xFFFFFF00
    jne .choose
    cmp dword [vc_edid+4],0x00FFFFFF
    jne .choose
    mov si,vc_edid
    mov cx,128
    xor al,al
.checksum:
    add al,[si]
    inc si
    loop .checksum
    test al,al
    jnz .choose
    cmp byte [vc_edid+18],1
    jne .choose
    test byte [vc_edid+24],2 ; preferred timing is valid
    jz .choose
    cmp word [vc_edid+54],0
    je .choose
    movzx ax,byte [vc_edid+58]
    and ax,0xF0
    shl ax,4
    mov al,[vc_edid+56]
    cmp ax,640
    jb .choose
    movzx dx,byte [vc_edid+61]
    and dx,0xF0
    shl dx,4
    mov dl,[vc_edid+59]
    cmp dx,480
    jb .choose
    mov [vc_max_width],ax
    mov [vc_max_height],dx
.choose:
    ; A controller mode list does not identify the attached monitor's limit.
    ; Without a valid preferred EDID timing, start within 800x600. Explicit
    ; profiles still resolve against the full checked firmware mode list.
    cmp word [vc_max_width],0
    jne .bounded
    mov word [vc_max_width],800
    mov word [vc_max_height],600
.bounded:
    xor bp,bp
.next:
    cmp bp,[vc_mode_count]
    jae .cached
    mov bx,bp
    shl bx,1
    mov ax,[vc_mode_list+bx]
    inc bp
    test ax,0xFE00 ; mode IDs only; no linear/preserve flags
    jnz .next
    mov [vc_mode],ax
    call vc_probe
    jc .next
    mov ax,[vc_info+18]
    cmp ax,[vc_max_width]
    ja .next
    mov dx,[vc_info+20]
    cmp dx,[vc_max_height]
    ja .next
    movzx eax,ax
    movzx edx,dx
    imul eax,edx
    cmp eax,[vc_best_area]
    jb .next
    ja .best
    mov dl,[vc_info+25]
    cmp dl,[vc_best_depth]
    jbe .next
.best:
    mov [vc_best_area],eax
    mov al,[vc_info+25]
    mov [vc_best_depth],al
    mov ax,[vc_mode]
    mov [vc_best_mode],ax
    jmp .next
.cached:
    mov ax,[vc_best_mode]
    mov [vc_mode],ax
    pop fs
    pop es
    popad
    ret

; Fill vc_controller once (VBE 4F00). Explicit profiles never enumerate
; modes, yet validation and two-page presentation need TotalMemory.
vc_controller_query:
    cmp dword [cs:vc_controller],'VESA'
    je .done
    pushad
    push es
    push cs
    pop es
    mov di,vc_controller
    mov dword [cs:di],'VBE2'
    mov ax,0x4F00
    call vc_bios
    cmp ax,0x004F
    je .queried
    mov dword [cs:vc_controller],0
    mov word [cs:vc_controller+18],0 ; unknown TotalMemory
.queried:
    pop es
    popad
.done:
    ret

; Resolve an exact native geometry using firmware modes, never a guessed ID.
; AX = width, DX = height. vc_mode = best supported depth, or zero if absent.
; Explicit profiles are allowed above EDID's preferred timing (the firmware
; must still provide a valid mode); AUTO alone follows monitor preference.
vc_resolve_mode:
    pushad
    mov [vc_request_width],ax
    mov [vc_request_height],dx
    call vc_auto
    mov word [vc_resolved_mode],0
    mov byte [vc_resolved_depth],0
    xor bp,bp
.next:
    cmp bp,[vc_mode_count]
    jae .done
    mov bx,bp
    shl bx,1
    mov ax,[vc_mode_list+bx]
    inc bp
    test ax,0xFE00
    jnz .next
    mov [vc_mode],ax
    call vc_probe
    jc .next
    mov ax,[vc_info+18]
    cmp ax,[vc_request_width]
    jne .next
    mov ax,[vc_info+20]
    cmp ax,[vc_request_height]
    jne .next
    mov al,[vc_info+25]
    cmp al,[vc_resolved_depth]
    jbe .next
    mov [vc_resolved_depth],al
    mov ax,[vc_mode]
    mov [vc_resolved_mode],ax
    jmp .next
.done:
    mov ax,[vc_resolved_mode]
    mov [vc_mode],ax
    popad
    ret

; VBE 3.0 may describe different banked and linear pixel formats. Keep the
; banked descriptor so a refused linear mode can restore it without another
; BIOS query. Normalize the chosen format into the fields used by renderers.
; Called only for fresh 4F01 data; active 4F06 updates use vc_validate_active.
vc_validate:
    mov ax,[vc_info+16]
    mov [vc_banked_layout],ax
    mov al,[vc_info+29]
    cmp word [vc_controller+4],0x0300
    jb .bank_pages
    mov al,[vc_info+52]
.bank_pages:
    mov [vc_banked_layout+2],al
    mov eax,[vc_info+31]
    mov [vc_banked_layout+3],eax
    mov eax,[vc_info+35]
    mov [vc_banked_layout+7],eax
    call vc_use_banked_layout
    call vc_validate_layout
    mov byte [vc_banked_valid],0
    jc .linear
    mov al,[vc_bank_ok]
    mov [vc_banked_valid],al
.linear:
    call vc_use_linear_layout
    call vc_validate_layout
    jc .banked
    cmp byte [vc_lfb_ok],1
    jne .banked
    mov al,[vc_banked_valid]
    mov [vc_bank_ok],al
    clc
    ret
.banked:
    call vc_use_banked_layout
    jmp vc_validate_active

vc_use_banked_layout:
    mov ax,[vc_banked_layout]
    mov [vc_info+16],ax
    mov al,[vc_banked_layout+2]
    mov [vc_info+29],al
    mov eax,[vc_banked_layout+3]
    mov [vc_info+31],eax
    mov eax,[vc_banked_layout+7]
    mov [vc_info+35],eax
    mov byte [vc_layout_linear],0
    ret

vc_use_linear_layout:
    mov byte [vc_layout_linear],1
    cmp word [vc_controller+4],0x0300
    jb .done                 ; VBE 2.0 has one shared format descriptor.
    mov ax,[vc_info+50]
    mov [vc_info+16],ax
    mov al,[vc_info+53]
    mov [vc_info+29],al
    mov eax,[vc_info+54]
    mov [vc_info+31],eax
    mov eax,[vc_info+58]
    mov [vc_info+35],eax
.done:
    ret

; The active access method must itself validate: a good bank descriptor must
; not make an invalid linear format acceptable, or vice versa.
vc_validate_active:
    call vc_validate_layout
    jc .fail
    cmp byte [vc_layout_linear],1
    je .linear
    mov byte [vc_lfb_ok],0
    cmp byte [vc_bank_ok],1
    jne .fail
    clc
    ret
.linear:
    mov al,[vc_banked_valid]
    mov [vc_bank_ok],al
    cmp byte [vc_lfb_ok],1
    jne .fail
    clc
    ret
.fail:
    stc
    ret

; Validate all parameters consumed by one normalized renderer layout.
vc_validate_layout:
    mov byte [vc_bank_ok],0
    mov byte [vc_lfb_ok],0
    mov ax,[vc_info]
    and ax,0x0019 ; supported, color, graphics
    cmp ax,0x0019
    jne .fail
    ; Linear framebuffer: attribute bit 7 and PhysBasePtr, from real mode
    ; only (a virtual-8086 monitor would trap the burst's CR0 writes).
    test byte [vc_info],0x80
    jz .window
    cmp dword [vc_info+40],0
    je .window
    smsw ax
    test al,1
    jnz .window
    mov byte [vc_lfb_ok],1
.window:
    test byte [vc_info],0x40 ; windowed access unavailable
    jnz .format
    mov al,[vc_info+2]
    and al,7
    cmp al,7 ; window A must be readable as well as writable (cursor save)
    jne .format
    cmp word [vc_info+6],64
    jb .format
    cmp word [vc_info+8],0xA000
    jne .format
    mov cx,[vc_info+4]
    test cx,cx
    jz .format
    mov ax,64
    xor dx,dx
    div cx
    test dx,dx
    jnz .format
    test ax,ax
    jz .format
    mov [vc_bank_step],ax
    mov byte [vc_bank_ok],1
.format:
    cmp byte [vc_info+24],1
    jne .fail
    mov al,[vc_info+25]
    mov byte [vc_bytes],1
    cmp al,8
    je .indexed
    cmp byte [vc_info+27],6
    jne .fail
    cmp al,15
    je .direct
    cmp al,16
    je .direct
    cmp al,24
    je .direct
    cmp al,32
    jne .fail
.direct:
    add al,7
    shr al,3
    mov [vc_bytes],al
    ; Require non-overlapping R/G/B fields within each pixel.
    xor edi,edi
    mov si,vc_info+31
    mov bp,3
.mask:
    mov cl,[si]
    cmp cl,5
    jb .fail
    cmp cl,8
    ja .fail
    mov al,cl
    add al,[si+1]
    jc .fail                ; malformed field positions must not wrap to zero
    cmp al,[vc_info+25]
    ja .fail
    mov eax,1
    shl eax,cl
    dec eax
    mov cl,[si+1]
    shl eax,cl
    test eax,edi
    jnz .fail
    or edi,eax
    add si,2
    dec bp
    jnz .mask
    jmp .size
.indexed:
    cmp byte [vc_info+27],4
    jne .fail
.size:
    mov ax,[vc_info+18]
    cmp ax,640
    jb .fail
    cmp ax,8192
    ja .fail
    mov dx,[vc_info+20]
    cmp dx,480
    jb .fail
    cmp dx,VC_ROW_LIMIT*16 ; console dirty-row tables hold VC_ROW_LIMIT rows
    ja .fail
    push ax
    shr ax,3
    shr dx,4
    mov [vc_cols],ax
    mov [vc_rows],dx
    mul dx
    test dx,dx
    pop ax
    jnz .fail
    movzx ecx,word [vc_cols]
    movzx edx,word [vc_rows]
    imul ecx,edx
    cmp ecx,32504               ; cells plus 512-byte glyph scratch + alignment
    ja .fail
    movzx eax,ax
    movzx ecx,byte [vc_bytes]
    imul eax,ecx
    movzx ecx,word [vc_info+16]
    cmp eax,ecx
    ja .fail
    mov [vc_pitch],cx
    ; The last bank's granularity-scaled number must fit VBE's DX register.
    ; Otherwise high modes can silently wrap into the beginning of VRAM.
    movzx eax,word [vc_info+20]
    imul eax,ecx
    mov [vc_frame_bytes],eax
    mov [vc_access_bytes],eax
    cmp byte [vc_bank_ok],1
    jne .linear
    dec eax
    shr eax,16
    movzx edx,word [vc_bank_step]
    imul eax,edx
    cmp eax,65535
    jbe .linear
    mov byte [vc_bank_ok],0
.linear:
    ; Two complete pages above PhysBasePtr must not wrap the 4 GiB space.
    mov eax,[vc_frame_bytes]
    shl eax,1
    jc .no_linear
    add eax,[vc_info+40]
    jnc .access
.no_linear:
    mov byte [vc_lfb_ok],0
.access:
    mov al,[vc_bank_ok]
    or al,[vc_lfb_ok]
    jz .fail
    ; AUTO/exact-mode discovery supplies TotalMemory in 64 KiB blocks.
    ; Standard explicit modes may not have queried the controller yet.
    movzx eax,word [vc_controller+18]
    test eax,eax
    jz .memory_ok
    shl eax,16
    cmp [vc_frame_bytes],eax
    ja .fail
.memory_ok:
    mov word [vc_window],0xA000
    clc
    ret
.fail:
    stc
    ret

; DS:DX = CX DAC RGB triples (6-bit channels), BX = 0. The UI reserves the
; first 16 colors for chrome and appends Ciuki's source-image palette. Other
; clients retain a 16-entry table and still initialize exactly 16 colors.
%ifndef VC_PALETTE_COLORS
%define VC_PALETTE_COLORS 16
%endif
%if VC_PALETTE_COLORS < 16 || VC_PALETTE_COLORS > 256
%error "VBE logical palette must contain between 16 and 256 entries"
%endif
vc_set_palette:
    pushad
    test cx,cx
    jz .invalid
    cmp cx,VC_PALETTE_COLORS
    ja .invalid
    test bx,bx
    jnz .invalid
    mov si,dx
    mov di,vc_colors
    xor bp,bp
.color:
    push cx
    movzx eax,bp
    cmp byte [vc_bytes],1
    je .store
    xor ebx,ebx
    push si
    mov dx,vc_info+31
    push bp
    mov bp,3
.channel:
    movzx eax,byte [si]
    mov ecx,eax
    shl eax,2
    shr ecx,4
    or eax,ecx
    push bx
    mov bx,dx
    mov cl,8
    sub cl,[bx]
    shr eax,cl
    mov cl,[bx+1]
    shl eax,cl
    pop bx
    or ebx,eax
    inc si
    add dx,2
    dec bp
    jnz .channel
    pop bp
    pop si
    mov eax,ebx
.store:
    mov [di],eax
    add di,4
    add si,3
    inc bp
    pop cx
    loop .color
    popad
    cmp byte [vc_bytes],1
    jne .done
    mov ax,0x1012
    call vc_bios
.done:
    ret
.invalid:
    popad
    ret

; AL = logical palette color, EDI = byte offset; preserve registers.
vc_put_index:
    push eax
    push ebx
    movzx ebx,al
    shl bx,2
    mov eax,[cs:vc_colors+bx]
    call vc_put_pixel
    pop ebx
    pop eax
    ret

; EAX = native pixel at framebuffer byte EDI; preserve registers.
vc_put_pixel:
    push ecx
    mov ecx,1
    call vc_span
    pop ecx
    ret

; Fill ECX pixels of native color EAX at EDI in one short burst.
vc_span:
    call vc_fb_begin
    call vc_fb_fill
    jmp vc_fb_end

vc_bytes db 1
vc_auto_done db 0
vc_best_mode dw 0
vc_best_depth db 0
vc_best_area dd 0
vc_max_width dw 0
vc_max_height dw 0
vc_force_safe db 0
vc_bios_bx dw 0
vc_bios_cx dw 0
vc_bios_dx dw 0
vc_mode_count dw 0
vc_frame_bytes dd 0
vc_access_bytes dd 0
vc_scan_lines dw 0
vc_request_width dw 0
vc_request_height dw 0
vc_resolved_mode dw 0
vc_resolved_depth db 0
vc_layout_linear db 0
vc_banked_valid db 0
vc_banked_layout times 11 db 0 ; pitch, image-pages-minus-one, four RGB/Rsvd pairs
vc_colors times VC_PALETTE_COLORS dd 0
vc_controller times 512 db 0
vc_edid times 128 db 0
vc_mode_list times VC_MODE_LIMIT dw 0
