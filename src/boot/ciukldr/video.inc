video_select:
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
    mov ax, [B(vbe_ctrl)+16]
    mov fs, ax
    mov si, [B(vbe_ctrl)+14]
    xor bx, bx
.list:
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
    cmp eax, 32
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
    mov ax, [candidate_mode]
    cmp ax, [preferred_mode]
    je .preferred
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
mode_scores times 512 dw 0xffff
