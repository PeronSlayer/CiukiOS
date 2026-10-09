; Framebuffer access shared by every VBE client: banked (64 KiB window via
; INT 10h/4F05) or linear (VBE 2.0 PhysBasePtr). Callers never touch the
; window directly; they use bursts of vc_fb_put/get/fill.
;
; A linear burst (vc_fb_begin .. vc_fb_end) clears IF/TF and enters protected
; mode for a few instructions to give ES a flat 4 GiB limit. The end of the
; burst reloads a 64 KiB limit the same way, then restores the caller's
; GDTR, IDTR, ES and flags, so no unreal segment outlives a burst and DOS children
; never inherit one. Bursts are nestable and callers bound them to one
; scanline, cell row or pointer, keeping interrupt latency short.
; Inside a burst ES is scratch. Reset its base to zero for linear accesses;
; its cached limit is restored to 64 KiB only by the outer protected exit.
; vc_lfb=1 local bursts require real mode and working A20. vc_lfb=2 is a
; true LFB mapped by CVSESSION; V86 routes its pixels through protected calls.
; A temporary dual-format IDT defers NMI until the burst has restored normal
; CPU state. It never changes the hardware NMI mask or the CMOS index.
%ifndef VC_PROTECTED_LFB
%define VC_PROTECTED_LFB 1
%endif

%macro VC_FB_COPY 0-1
    push ecx
    shr ecx,2
    %1 rep movsd
    pop ecx
    and ecx,3
    %1 rep movsb
%endmacro

vc_fb_begin:
    pushf
    cmp byte [cs:vc_lfb],1
    jne .unchanged
    inc byte [cs:vc_fb_depth]
    cmp byte [cs:vc_fb_depth],1
    jne .unchanged
    pop word [cs:vc_fb_flags]
    push ax
    mov ax,[cs:vc_fb_flags]
    and ax,0xFCFF             ; neither IRQ nor single-step inside the switch
    push ax
    popf
    pop ax
    mov [cs:vc_fb_es],es
    mov word [cs:vc_fb_nmi_pending],0
    o32 sidt [cs:vc_fb_saved_idtr]
    o32 lidt [cs:vc_fb_idtr]
    push bx
    mov bx,8
    call vc_fb_limits
    pop bx
    ; ES is now flat. Read the previous real-mode NMI vector even if the
    ; original IVT was relocated. An NMI during entry has only been deferred.
    push eax
    mov eax,[cs:vc_fb_saved_idtr+2]
    mov eax,[es:eax+8]
    mov [cs:vc_fb_nmi_chain],eax
    pop eax
    ret
.unchanged:
    popf
    ret

; Flags are restored to their state at the matching vc_fb_begin.
vc_fb_end:
    pushf
    cmp byte [cs:vc_lfb],1
    jne .unchanged
    dec byte [cs:vc_fb_depth]
    jnz .unchanged
    add sp,2                 ; the outer begin owns the flags restored below
    push bx
    mov bx,16
    call vc_fb_limits
    pop bx
    mov es,[cs:vc_fb_es]
    o32 lidt [cs:vc_fb_saved_idtr]
.pending:
    cmp word [cs:vc_fb_nmi_pending],0
    je .flags
    dec word [cs:vc_fb_nmi_pending]
    pushf
    call far [cs:vc_fb_nmi_chain]
    jmp .pending
.flags:
    push word [cs:vc_fb_flags]
    popf
    ret
.unchanged:
    popf
    ret

; BX = ES descriptor (8 flat, 16 ordinary). IF/TF must be clear and the
; temporary NMI IDT installed by the outer vc_fb_begin must still be active.
; Leaves ES = 0 in real mode with the chosen cached limit.
vc_fb_limits:
    push eax
    o32 sgdt [cs:vc_fb_saved_gdtr]
    o32 lgdt [cs:vc_fb_gdtr]
    mov eax,cr0
    or al,1
    mov cr0,eax
    jmp 24:.protected
.protected:
    mov es,bx
    and al,0xFE
    mov cr0,eax
vc_fb_rm_jump:
    jmp 0:vc_fb_real          ; real CS is patched once by vc_lfb_open
vc_fb_real:
    xor ax,ax
    mov es,ax
    o32 lgdt [cs:vc_fb_saved_gdtr]
    pop eax
    ret

; NMI is still deliverable with IF/TF clear. Its real-mode vector occupies
; bytes 8..11 of the temporary table; its protected-mode gate bytes 16..23.
; IRET in protected mode requires a valid CS selector, including an NMI
; immediately after PE is set but before the far jump has loaded CS.
vc_fb_nmi_pm:
    push bp
    mov bp,sp
    mov word [ss:bp+4],24
    ; Saturation retains a pending delivery even during an abnormal storm.
    cmp word [cs:vc_fb_nmi_pending],0xFFFF
    je .return
    inc word [cs:vc_fb_nmi_pending]
.return:
    pop bp
    iret

; Conversely, PE may already be clear while CS still names selector 24.
; Normalize that saved CS before a real-mode IRET reloads its base.
vc_fb_nmi_rm:
    push bp
    mov bp,sp
    push ax
    cmp word [ss:bp+4],24
    jne .defer
    mov ax,cs
    mov [ss:bp+4],ax
.defer:
    cmp word [cs:vc_fb_nmi_pending],0xFFFF
    je .return
    inc word [cs:vc_fb_nmi_pending]
.return:
    pop ax
    pop bp
    iret

; CX bytes between FS:SI and framebuffer byte EDI. vc_fb_put writes the
; framebuffer, vc_fb_get reads it. Preserves registers.
vc_fb_get:
    mov byte [cs:vc_fb_read],1
    jmp vc_fb_xfer
vc_fb_put:
    mov byte [cs:vc_fb_read],0
vc_fb_xfer:
    pushad
    push ds
    push es
    cld
    movzx ecx,cx
    movzx esi,si
    ; V86 uses the banked transport; real LFB transfers use the local burst.
    cmp byte [cs:vc_lfb],1
    je .linear
    cmp byte [cs:vc_lfb],2
    jne .protected_transfer
    cmp byte [cs:vc_active],1
    jne .protected_failed
.protected_transfer:
    ; CVSESSION owns a protected mapping of the validated physical VRAM.
    ; In V86 the desktop's BIOS layout is banked, but this transfer need not
    ; remap its 64-KiB window for every bank or scanline.
    call vc_session_xfer
    jnc .done
    cmp byte [cs:vc_lfb],2
    jne .banked
    call vc_session_fb_bind
    jc .protected_failed
    call vc_session_xfer
    jnc .done
.protected_failed:
    mov byte [cs:vc_active],0
    stc
    jmp .done                 ; a linear mode never falls into banked I/O
.linear:
    call vc_fb_linear
    jc .done
    or byte [cs:vc_transport_used],2
    xor ax,ax
    mov es,ax                 ; callers may use ES for scratch during a burst
    cmp byte [cs:vc_fb_read],1
    je .linear_read
    VC_FB_COPY fs a32
    jmp .done
.linear_read:
    ; Only the pointer save reads VRAM (at most 1.5 KiB): dwords, then bytes.
    mov edx,ecx
    shr ecx,2
    jz .read_tail
.read_dword:
    mov eax,[es:edi]
    mov [fs:si],eax
    add edi,4
    add si,4
    dec ecx
    jnz .read_dword
.read_tail:
    and dx,3
    jz .done
.read_byte:
    mov al,[es:edi]
    mov [fs:si],al
    inc edi
    inc si
    dec dx
    jnz .read_byte
    jmp .done
.banked:
    jecxz .done
    call vc_map
    jc .done
    or byte [cs:vc_transport_used],1
    movzx eax,di
    mov edx,65536
    sub edx,eax
    cmp edx,ecx
    jbe .chunk
    mov edx,ecx
.chunk:
    sub ecx,edx
    push ecx
    push edi
    mov ecx,edx
    cmp byte [cs:vc_fb_read],1
    je .banked_read
    VC_FB_COPY fs
    jmp .advance
.banked_read:
    push ds
    push es
    pop ds
    push fs
    pop es
    xchg si,di
    VC_FB_COPY
    xchg si,di
    pop ds
.advance:
    pop edi
    add edi,edx
    pop ecx
    jmp .banked
.done:
    pop es
    pop ds
    popad
    ret

; ECX bytes at framebuffer byte EDI must lie inside the validated surface.
; Returns EDI as a physical linear address, or CF set.
vc_fb_linear:
    mov eax,edi
    add eax,ecx
    jc .fail
    cmp eax,[cs:vc_access_bytes]
    ja .fail
    add edi,[cs:vc_lfb_base]
    clc
    ret
.fail:
    stc
    ret

; Fill ECX pixels of native colour EAX at framebuffer byte EDI. Preserves
; registers. Must run inside a burst.
vc_fb_fill:
    pushad
    push es
    cld
    test ecx,ecx
    jz .done
    mov ebx,eax
    cmp byte [cs:vc_lfb],2
    jne .ordinary
    cmp byte [cs:vc_active],1
    jne .done
    call vc_session_fill
    jmp .done
.ordinary:
    cmp byte [cs:vc_lfb],1
    jne .banked
    push ecx
    movzx eax,byte [cs:vc_bytes]
    imul ecx,eax
    call vc_fb_linear
    pop ecx
    jc .done
    xor ax,ax
    mov es,ax
    mov eax,ebx
    cmp byte [cs:vc_bytes],2
    jb .linear1
    je .linear2
    cmp byte [cs:vc_bytes],4
    je .linear4
.linear3:
    a32 stosw
    shr eax,16
    a32 stosb
    mov eax,ebx
    dec ecx
    jnz .linear3
    jmp .done
.linear1:
    a32 rep stosb
    jmp .done
.linear2:
    a32 rep stosw
    jmp .done
.linear4:
    a32 rep stosd
    jmp .done
.banked:
    mov ebp,ecx
.span:
    call vc_map
    jc .done
    movzx eax,di
    mov ecx,65536
    sub ecx,eax
    mov eax,ecx
    xor edx,edx
    movzx ecx,byte [cs:vc_bytes]
    div ecx
    test eax,eax
    jz .split
    cmp eax,ebp
    jbe .count
    mov eax,ebp
.count:
    mov ecx,eax
    sub ebp,eax
    push edi
    push ecx
    mov eax,ebx
    cmp byte [cs:vc_bytes],4
    je .dwords
    cmp byte [cs:vc_bytes],2
    je .words
    cmp byte [cs:vc_bytes],3
    je .triples
    rep stosb
    jmp .next
.words:
    rep stosw
    jmp .next
.dwords:
    rep stosd
    jmp .next
.triples:
    stosw
    shr eax,16
    stosb
    mov eax,ebx
    loop .triples
.next:
    pop ecx
    pop edi
    movzx eax,byte [cs:vc_bytes]
    imul eax,ecx
    add edi,eax
    jmp .remaining
.split:
    ; Fewer than one pixel remains in this bank: store its bytes singly.
    push edi
    movzx cx,byte [cs:vc_bytes]
    mov eax,ebx
.split_byte:
    call vc_map
    jc .split_done
    mov [es:di],al
    shr eax,8
    inc edi
    loop .split_byte
.split_done:
    pop edi
    movzx eax,byte [cs:vc_bytes]
    add edi,eax
    dec ebp
.remaining:
    test ebp,ebp
    jnz .span
.done:
    pop es
    popad
    ret

; FS:SI rows (stride BX) to/from the framebuffer at EDI with vc_pitch;
; CX bytes per row, DX rows. One burst per row. Preserves registers.
vc_fb_put_rows:
    mov byte [cs:vc_fb_read],0
    jmp vc_fb_rows
vc_fb_get_rows:
    mov byte [cs:vc_fb_read],1
vc_fb_rows:
    pushad
    cmp byte [cs:vc_lfb],2
    jne .active
    cmp byte [cs:vc_active],1
    je .active
    stc
    jmp .done
.active:
    test dx,dx
    jz .done
    call vc_session_rows
    jnc .done
.fallback:
.row:
    call vc_fb_begin
    call vc_fb_xfer
    call vc_fb_end
    add si,bx
    movzx eax,word [cs:vc_pitch]
    add edi,eax
    dec dx
    jnz .row
.done:
    popad
    ret

; Store native pixel EAX at ES:DI and advance DI by one pixel.
vc_store_pixel:
    cmp byte [cs:vc_bytes],2
    jb .one
    je .two
    cmp byte [cs:vc_bytes],4
    je .four
    stosw
    shr eax,16
    stosb
    ret
.one:
    stosb
    ret
.two:
    stosw
    ret
.four:
    stosd
    ret

; Software arrow for direct/indexed VBE modes, DS = CS. EDI = framebuffer
; byte of its top-left corner, FS:SI = 24x16 save area (24*vc_bytes per row).
; vc_pointer_draw also takes BX = 16 outline words followed by 16 fill words.
vc_pointer_save:
    ; Each displayed page owns its saved rectangle. Retain the transfer
    ; geometry with the pixels: restore must not use a later pointer or
    ; display size. Offset/pitch math also excludes framebuffer row padding.
    pushad
    mov bp,vc_pointer_clip
    mov eax,edi
    cmp eax,[cs:vc_frame_bytes]
    jb .page
    sub eax,[cs:vc_frame_bytes]
    add bp,12
.page:
    mov [cs:bp],si                    ; source save area also identifies flips
    movzx ecx,word [cs:vc_pitch]
    mov [cs:bp+8],cx
    xor edx,edx
    div ecx
    mov cx,[cs:vc_info+20]
    sub cx,ax
    cmp cx,16
    jbe .rows
    mov cx,16
.rows:
    mov [cs:bp+6],cx
    movzx ax,byte [cs:vc_bytes]
    imul cx,ax,24
    mov [cs:bp+10],cx
    imul ax,[cs:vc_info+18]
    sub ax,dx
    cmp cx,ax
    jbe .columns
    mov cx,ax
.columns:
    mov [cs:bp+4],cx
    popad
    mov byte [cs:vc_fb_read],1
    jmp vc_pointer_xfer
vc_pointer_restore:
    mov byte [cs:vc_fb_read],0
vc_pointer_xfer:
    pushad
    push fs
    call vc_pointer_geometry
    push word [cs:vc_pitch]
    mov [cs:vc_pitch],ax
    call vc_fb_rows
    pop word [cs:vc_pitch]
    pop fs
    popad
    ret

vc_pointer_geometry:
    mov bp,vc_pointer_clip
    cmp si,[cs:vc_pointer_clip+12]
    jne .first
    add bp,12
.first:
    mov cx,[cs:bp+4]
    mov dx,[cs:bp+6]
    mov ax,[cs:bp+8]
    mov bx,[cs:bp+10]
    ret

; RAM save-area offset + reserved, row bytes, rows, framebuffer pitch,
; RAM stride. Use the source area for restore: page catch-up can restore
; the front cursor's background onto the other physical display page.
vc_pointer_clip times 2 dw 0,0,0,0,0,0
vc_pointer_skip_x db 0
vc_pointer_skip_y db 0

; Compose the whole arrow in RAM, then submit its rows in one transfer.
; A cursor update must not pay sixteen monitor entries/bank operations.
vc_pointer_draw:
    pushad
    push es
    push fs
    cld
    push si
    push edi
%ifdef VC_POINTER_WORKSPACE_EXTERNAL
    push fs
%else
    push ds
%endif
    pop es
    movzx cx,byte [cs:vc_bytes]
    imul cx,cx,24
    shl cx,4
    mov di,vc_pointer_row
    push ds
    push fs
    pop ds
    rep movsb
    pop ds
    mov di,vc_pointer_row
    mov bp,16
    movzx ax,byte [cs:vc_pointer_skip_y]
    sub bp,ax
    shl ax,1
    add bx,ax
.row:
    mov si,[bx]
    mov dx,[bx+32]
    mov cl,[cs:vc_pointer_skip_x]
    shl si,cl
    shl dx,cl
    movzx ax,cl
    mov cx,16
    sub cx,ax
.pixel:
    shl si,1
    jc .outline
    shl dx,1
    movzx ax,byte [cs:vc_bytes]
    add di,ax
    jmp .next
.outline:
    mov eax,[cs:vc_colors]
    shl dx,1
    jnc .store
    mov eax,[cs:vc_colors+15*4]
.store:
    call vc_store_pixel
.next:
    loop .pixel
    movzx ax,byte [cs:vc_pointer_skip_x]
    add ax,8                   ; transparent tail includes left-clipped pixels
    movzx cx,byte [cs:vc_bytes]
    imul ax,cx
    add di,ax
    add bx,2
    dec bp
    jnz .row
    pop edi
    pop si
%ifndef VC_POINTER_WORKSPACE_EXTERNAL
    push ds
    pop fs
%endif
    call vc_pointer_geometry
    mov si,vc_pointer_row
    push word [cs:vc_pitch]
    mov [cs:vc_pitch],ax
    call vc_fb_put_rows
    pop word [cs:vc_pitch]
    pop fs
    pop es
    popad
    ret

; Linear framebuffer bursts require A20. Prefer the resident XMS manager's
; local enable (balanced in vc_lfb_close), then verify the 1 MiB wrap.
; CF set when linear access must not be used.
vc_lfb_open:
    pushad
    push ds
    push es
    xor eax,eax
    mov ax,cs
    shl eax,4
    mov [cs:vc_fb_code+2],ax
    push eax
    shr eax,16
    mov [cs:vc_fb_code+4],al
    pop eax
    add eax,vc_fb_idt
    mov [cs:vc_fb_idtr+2],eax
    sub eax,vc_fb_idt
    add eax,vc_fb_gdt
    mov [cs:vc_fb_gdtr+2],eax
    mov [cs:vc_fb_rm_jump+3],cs
    mov [cs:vc_fb_idt+10],cs
    mov byte [cs:vc_fb_depth],0
    mov byte [cs:vc_lfb_xms],0
    ; A valid real-mode IDT must contain vector 2 without wrapping its base.
    o32 sidt [cs:vc_fb_saved_idtr]
    cmp word [cs:vc_fb_saved_idtr],11
    jb .invalid_idt
    cmp dword [cs:vc_fb_saved_idtr+2],0xFFFFFFF4
    ja .invalid_idt
    mov ax,0x4300
    int 0x2F
    cmp al,0x80
    jne .test
    mov ax,0x4310
    int 0x2F
    mov [cs:vc_xms_entry],bx
    mov [cs:vc_xms_entry+2],es
    mov ah,5
    call far [cs:vc_xms_entry]
    cmp ax,1
    jne .test
    mov byte [cs:vc_lfb_xms],1
.test:
    xor ax,ax
    mov ds,ax
    dec ax
    mov es,ax
    pushf
    cli
    push word [0x04F0]
    mov word [0x04F0],0x5A17
    cmp word [es:0x0500],0x5A17
    jne .enabled
    not word [0x04F0]
    cmp word [es:0x0500],0xA5E8
    jne .enabled
    pop word [0x04F0]
    popf
    call vc_lfb_close
    stc
    jmp .done
.enabled:
    pop word [0x04F0]
    popf
    clc
    jmp .done
.invalid_idt:
    stc
.done:
    pop es
    pop ds
    popad
    ret

vc_lfb_close:
    pushad
    cmp byte [cs:vc_lfb_xms],0
    je .done
    mov byte [cs:vc_lfb_xms],0
    mov ah,6
    call far [cs:vc_xms_entry]
.done:
    popad
    ret

align 8
vc_fb_gdt:
    dq 0
    dw 0xFFFF,0
    db 0,0x92,0x8F,0
    dw 0xFFFF,0
    db 0,0x92,0,0
vc_fb_code:
    dw 0xFFFF,0
    db 0,0x9A,0,0
vc_fb_gdtr dw 31
    dd 0
vc_fb_saved_gdtr dw 0
    dd 0
vc_fb_idt:
    dq 0
    dw vc_fb_nmi_rm,0
    dd 0
    dw vc_fb_nmi_pm,24
    db 0,0x86
    dw 0
vc_fb_idtr dw 23
    dd 0
vc_fb_saved_idtr dw 0
    dd 0
vc_fb_nmi_chain dd 0
vc_fb_nmi_pending dw 0
vc_lfb_base dd 0
vc_xms_entry dd 0
vc_lfb db 0
vc_lfb_ok db 0
vc_bank_ok db 0
vc_lfb_xms db 0
vc_fb_depth db 0
vc_fb_read db 0
vc_transport_used db 0 ; validated renderer path entered: bank window=1, local LFB=2
vc_fb_flags dw 0
vc_fb_es dw 0
%ifndef VC_POINTER_WORKSPACE_EXTERNAL
vc_pointer_row times 24*16*4 db 0
%endif

%include "src/com/vbe_session_fb.inc"
