video_select:
    mov dword [B(display)+ciuki_boot_display.tag], CBI_DISPLAY_TAG
    mov dword [B(display)+ciuki_boot_display.size], ciuki_boot_display_size
    mov dword [B(vbe_ctrl)], 'VBE2'
    mov di, B(vbe_ctrl)
    mov ax, 0x4f00
    int 0x10
    cmp ax, 0x004f
    jne .text
    cmp dword [B(vbe_ctrl)], 'VESA'
    jne .text
    cmp word [B(vbe_ctrl)+4], 0x0200
    jb .text
    or dword [B(flags)], CBI_F_VBE_CTRL_VALID
    call display_controller_copy
    mov ax, [B(vbe_ctrl)+16]
    mov fs, ax
    mov si, [B(vbe_ctrl)+14]
    xor bx, bx
.list:
    call progress_check
    jc .text
    cmp si, 65534
    ja .text
    mov ax, [fs:si]
    cmp ax, 0xffff
    je .inventory
    cmp bx, 1024
    jae .text
    mov [mode_list+bx], ax
    add si, 2
    add bx, 2
    jmp .list
.inventory:
    mov [mode_bytes], bx
    xor bp, bp
.mode:
    call progress_check
    jc .text
    cmp bp, [mode_bytes]
    jae .pick
    mov cx, [ds:mode_list+bp]
    mov [candidate_mode], cx
    mov word [ds:mode_scores+bp], 0xffff
    push bp
    call mode_query
    jc .reject
    call mode_validate
    jc .reject
    xor dx, dx
    call display_mode_copy
    call mode_rank
    pop bp
    mov [ds:mode_scores+bp], ax
    jmp .next
.reject:
    mov si, video_reject
    call print
.skip:
    pop bp
.next:
    add bp, 2
    jmp .mode
.pick:
    xor bx, bx
    mov dx, 0xffff
    mov bp, 0xffff
.best:
    call progress_check
    jc .text
    cmp bx, [mode_bytes]
    jae .try
    cmp [mode_scores+bx], dx
    jae .not_best
    mov dx, [mode_scores+bx]
    mov bp, bx
.not_best:
    add bx, 2
    jmp .best
.try:
    call progress_check
    jc .text
    cmp bp, 0xffff
    je .text
    mov word [ds:mode_scores+bp], 0xffff
    mov cx, [ds:mode_list+bp]
    mov [candidate_mode], cx
    call mode_query
    jc .set_reject
    call mode_validate
    jc .set_reject
    ; Preserve the validated pre-set info to detect changed geometry/masks.
    mov si, mode_buffer
    mov di, B(vbe_mode_info)
    mov cx, 256
    rep movsb
    mov bx, [candidate_mode]
    or bx, 0x4000
    mov ax, 0x4f02
    int 0x10
    cmp ax, 0x004f
    jne .set_reject
    mov ax, 0x4f03
    int 0x10
    cmp ax, 0x004f
    jne .set_reject
    test bx, 0x4000
    jz .set_reject
    and bx, 0x3fff
    cmp bx, [candidate_mode]
    jne .set_reject
    call mode_query
    jc .set_reject
    call mode_validate
    jc .set_reject
    ; Geometry, bpp, pitch, masks and base must agree with the selected mode.
    mov si, mode_buffer+16
    mov di, B(vbe_mode_info)+16
    mov cx, 49
    repe cmpsb
    jne .set_reject
    mov eax, [mode_buffer+40]
    mov [B(fb_phys)], eax
    mov eax, [valid_pitch]
    mov [B(fb_pitch)], eax
    mov eax, [mode_buffer+18]
    mov [B(fb_width)], eax
    mov al, [mode_buffer+25]
    mov [B(fb_bpp)], al
    mov si, [mask_offset]
    add si, mode_buffer
    mov di, B(fb_red_size)
    mov cx, 7
    rep movsb
    cmp byte [B(fb_bpp)], 8
    jne .active
    mov di, B(fb_red_size)
    mov cx, 7
    xor al, al
    rep stosb
.active:
    mov dx, 1 ; keep the boot mode even if the table was full
    call display_mode_copy
    mov ax, [candidate_mode]
    mov [B(vbe_mode)], ax
    jmp .edid
.set_reject:
    mov si, video_reject
    call print
    jmp .pick
.text:
    mov ax, 3
    int 0x10
    or dword [B(flags)], CBI_F_TEXT_MODE
    mov di, B(fb_phys)
    mov cx, 22
    xor ax, ax
    rep stosb
    mov di, B(vbe_mode_info)
    mov cx, 256
    rep stosb
.edid:
    call progress_check
    jc .bad_edid
    mov ax, 0x4f15
    mov bx, 1
    xor cx, cx
    xor dx, dx
    mov di, B(edid)
    int 0x10
    cmp ax, 0x004f
    jne .bad_edid
    cmp dword [B(edid)], 0xffffff00
    jne .bad_edid
    cmp dword [B(edid)+4], 0x00ffffff
    jne .bad_edid
    mov si, B(edid)
    mov cx, 128
    xor al, al
.checksum:
    add al, [si]
    inc si
    loop .checksum
    test al, al
    jnz .bad_edid
    or dword [B(flags)], CBI_F_EDID_VALID
    ret
.bad_edid:
    mov di, B(edid)
    mov cx, 128
    xor al, al
    rep stosb
    ret

mode_query:
    mov di, mode_buffer
    mov cx, 256
    xor ax, ax
    rep stosb
    mov cx, [candidate_mode]
    mov di, mode_buffer
    mov ax, 0x4f01
    int 0x10
    cmp ax, 0x004f
    jne .bad
    clc
    ret
.bad:
    stc
    ret

mode_validate:
    cmp byte [mode_buffer+24], 1
    jne .bad
    mov ax, [mode_buffer]
    and ax, 0x91
    cmp ax, 0x91
    jne .bad
    mov al, [mode_buffer+25]
    cmp al, 32
    je .direct
    cmp al, 24
    je .direct
    cmp al, 16
    je .direct
    cmp al, 8
    jne .bad
    cmp byte [mode_buffer+27], 4
    jne .bad
    jmp .pitch
.direct:
    cmp byte [mode_buffer+27], 6
    jne .bad
.pitch:
    mov bx, 16
    mov word [mask_offset], 31
    cmp word [B(vbe_ctrl)+4], 0x0300
    jb .vbe2
    mov bx, 50
    mov word [mask_offset], 54
.vbe2:
    movzx eax, word [mode_buffer+bx]
    test eax, eax
    jz .bad
    mov [valid_pitch], eax
    movzx ecx, word [mode_buffer+18]
    test ecx, ecx
    jz .bad
    movzx edx, byte [mode_buffer+25]
    shr edx, 3
    imul ecx, edx
    cmp eax, ecx
    jb .bad
    movzx ecx, word [mode_buffer+20]
    test ecx, ecx
    jz .bad
    mul ecx
    test edx, edx
    jnz .bad
    movzx ecx, word [B(vbe_ctrl)+18]
    shl ecx, 16
    cmp eax, ecx
    ja .bad
    mov edx, [mode_buffer+40]
    test edx, edx
    jz .bad
    add edx, eax
    jc .bad
    cmp byte [mode_buffer+25], 8
    je .palette
    mov si, [mask_offset]
    add si, mode_buffer
    xor ebx, ebx ; Accumulated masks.
    mov bp, 4
.mask:
    movzx eax, byte [si]
    mov cl, [si+1]
    test eax, eax
    jnz .nonzero
    cmp bp, 1 ; Reserved component may have zero width.
    jne .bad
    jmp .mask_next
.nonzero:
    cmp eax, 8 ; integer presenter supports components up to eight bits
    ja .bad
    movzx edx, cl
    add edx, eax
    movzx edi, byte [mode_buffer+25]
    cmp edx, edi
    ja .bad
    push cx
    mov cl, al
    mov eax, -1
    cmp cl, 32
    je .full
    mov eax, 1
    shl eax, cl
    dec eax
.full:
    pop cx
    shl eax, cl
    test ebx, eax
    jnz .bad
    or ebx, eax
.mask_next:
    add si, 2
    dec bp
    jnz .mask
    clc
    ret
.palette:
    clc
    ret
.bad:
    stc
    ret

mode_rank:
    test dword [B(flags)], CBI_F_SAFE_MODE
    jnz .resolution
    mov ax, [candidate_mode]
    cmp ax, [preferred_mode]
    je .preferred
.resolution:
    mov ax, [mode_buffer+18]
    mov dx, [mode_buffer+20]
    xor bx, bx
    cmp ax, 1024
    jne .r800
    cmp dx, 768
    jne .none
    mov bx, 1
    test dword [B(flags)], CBI_F_SAFE_MODE
    jz .bpp
    mov bx, 5
    jmp .bpp
.r800:
    cmp ax, 800
    jne .r640
    cmp dx, 600
    jne .none
    mov bx, 3
    jmp .bpp
.r640:
    cmp ax, 640
    jne .none
    cmp dx, 480
    jne .none
    mov bx, 5
    test dword [B(flags)], CBI_F_SAFE_MODE
    jz .bpp
    mov bx, 1
.bpp:
    mov al, [mode_buffer+25]
    cmp al, 32
    je .rank
    cmp al, 24
    je .second
    add bx, 20
    cmp al, 16
    je .rank
    add bx, 10
    jmp .rank
.second:
    inc bx
.rank:
    mov ax, bx
    ret
.none:
    mov ax, 0xffff
    ret
.preferred:
    xor ax, ax
    ret
video_reject db 'L:VBE_SKIP',13,10,0
candidate_mode dw 0
mode_bytes dw 0
valid_pitch dd 0
mask_offset dw 0

; f1-23: export only validated direct-colour LFB modes in the desktop range.
; DX=1 keeps the actual boot mode in a full table (last slot replacement).
display_mode_copy:
    pusha
    cmp word [candidate_mode], 0x100
    jb .done
    cmp word [candidate_mode], 0x200
    jae .done
    cmp byte [mode_buffer+25], 16
    jb .done
    cmp word [mode_buffer+18], 640
    jb .done
    cmp word [mode_buffer+18], 1600
    ja .done
    cmp word [mode_buffer+20], 480
    jb .done
    cmp word [mode_buffer+20], 1200
    ja .done
    mov bx, [B(display)+ciuki_boot_display.mode_count]
    mov di, B(display)+ciuki_boot_display.modes
    mov cx, bx
    mov ax, [candidate_mode]
.find:
    jcxz .new
    cmp [di], ax
    je .write
    add di, 12
    loop .find
.new:
    cmp bx, CBI_DISPLAY_MAX
    jb .append
    test dx, dx
    jz .done
    sub di, 12
    jmp .write
.append:
    inc dword [B(display)+ciuki_boot_display.mode_count]
.write:
    mov [di], ax
    mov eax, [mode_buffer+18]
    mov [di+2], eax
    movzx ax, byte [mode_buffer+25]
    mov [di+6], ax
    mov ax, [valid_pitch]
    mov [di+8], ax
    mov word [di+10], 1 ; linear=1, reserved=0
.done:
    popa
    ret

display_controller_copy:
    movzx eax, word [B(vbe_ctrl)+4]
    mov [B(display)+ciuki_boot_display.vbe_version], eax
    movzx eax, word [B(vbe_ctrl)+18]
    shl eax, 16
    mov [B(display)+ciuki_boot_display.vram_bytes], eax
    mov eax, [B(vbe_ctrl)+6]
    mov di, B(display)+ciuki_boot_display.oem
    call display_string_copy
    mov eax, [B(vbe_ctrl)+22]
    mov di, B(display)+ciuki_boot_display.vendor
    call display_string_copy
    mov eax, [B(vbe_ctrl)+26]
    mov di, B(display)+ciuki_boot_display.product
    call display_string_copy
    ret

; Convert a far pointer, bound it to real-mode memory and copy printable ASCII.
; Buffers were zeroed at entry, so truncation at 63 bytes remains terminated.
display_string_copy:
    push fs
    movzx edx, ax
    shr eax, 16
    shl eax, 4
    add eax, edx
    test eax, eax
    jz .done
    cmp eax, 0x100000
    jae .done
    mov edx, eax
    and dx, 15
    mov si, dx
    shr eax, 4
    mov fs, ax
    mov cx, 63
.copy:
    cmp byte [fs:si], 0
    je .done
    mov al, [fs:si]
    cmp al, 32
    jb .sanitize
    cmp al, 126
    jbe .store
.sanitize:
    mov al, '?'
.store:
    stosb
    inc si
    ; Check the actual normalized segment + offset against the 1 MiB end.
    mov ax, fs
    movzx eax, ax
    shl eax, 4
    movzx ebx, si
    add eax, ebx
    cmp eax, 0x100000
    jae .done
    loop .copy
.done:
    pop fs
    ret
