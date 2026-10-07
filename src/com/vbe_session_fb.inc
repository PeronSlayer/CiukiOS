; Optional CVSESSION transport for the system VM's validated VBE linear
; framebuffer. Calls use the V86 far entry returned by INT 2Fh/1684h, and
; carry VM_OP_NO_SWITCH because the desktop may have its software pointer
; hidden while its two display pages are being synchronized.
%if VC_PROTECTED_LFB
%include "src/vm/session_abi.inc"

vc_session_find:
    pushad
    push es
    push ds
    push cs
    pop ds
    cmp dword [vc_session_entry],0
    jne .found
    ; INT 2Fh/1684h requires a null input pointer. Without a VMM the
    ; multiplex chain may leave ES:DI unchanged; never cache caller scratch.
    xor ax,ax
    mov es,ax
    xor di,di
    mov ax,0x1684
    mov bx,VM_DEVICE_ID
    int 0x2F
    mov ax,es
    test ax,ax
    jz .missing
    mov [vc_session_entry],di
    mov [vc_session_entry+2],es
.found:
    clc
    jmp .done
.missing:
    stc
.done:
    pop ds
    pop es
    popad
    ret

; Eligibility is a checked service contract, not just a multiplex pointer.
; V86 can select an actual LFB only if all pixel operations have a protected
; implementation, and the caller remains the system VM.
vc_session_available:
%if VC_PROTECTED_LFB
    pushad
    push es
    call vc_session_find
    jc .done
    push cs
    pop es
    mov di,vc_session_packet
    mov ax,VM_OP_QUERY | VM_OP_NO_SWITCH
    mov cx,VM_INFO_SIZE
    call far [cs:vc_session_entry]
    jc .done
    cmp dword [cs:vc_session_packet],VM_INFO_MAGIC
    jne .bad
    cmp word [cs:vc_session_packet+VM_INFO_BYTES],VM_INFO_SIZE
    jb .bad
    cmp word [cs:vc_session_packet+VM_INFO_VERSION],VM_ABI_VERSION
    jb .bad
    cmp dword [cs:vc_session_packet+VM_INFO_ACTIVE],0
    jne .bad
    mov eax,[cs:vc_session_packet+VM_INFO_CAPABILITIES]
    and eax,VM_CAP_DESKTOP_ROWS | VM_CAP_DESKTOP_FILL
    cmp eax,VM_CAP_DESKTOP_ROWS | VM_CAP_DESKTOP_FILL
    jne .bad
    clc
    jmp .done
.bad:
    stc
.done:
    pop es
    popad
    ret
%else
    stc
    ret
%endif

; Query CVMS and bind the physical base reported by VBE if it is not already
; bound. An existing binding is accepted only when both base and extent fit.
; CF=0 when row/copy acceleration is ready; otherwise ordinary VBE remains
; the fallback. Must run in the system VM, after ui_pages_begin established
; the final one- or two-frame vc_access_bytes extent.
vc_session_fb_bind:
    pushad
    push ds
    push cs
    pop ds
    push es
    push fs
    mov byte [vc_session_bound],0
    mov byte [vc_session_gpu],0
    mov eax,[cs:vc_info+40]       ; VBE ModeInfoBlock.PhysBasePtr
    test eax,eax
    jz .unavailable
    mov [vc_session_wanted_physical],eax
    mov eax,[cs:vc_access_bytes]
    test eax,eax
    jz .unavailable
    mov [vc_session_wanted_bytes],eax
    call vc_session_find
    jc .unavailable

    mov ax,VM_OP_QUERY | VM_OP_NO_SWITCH
    mov cx,VM_INFO_SIZE
    push cs
    pop es
    mov dword [vc_session_packet],VM_INFO_MAGIC
    mov dword [vc_session_packet+VM_INFO_VERSION],(VM_INFO_SIZE << 16) | VM_ABI_VERSION
    mov di,vc_session_packet
    call far [vc_session_entry]
    jc .unavailable
    cmp dword [vc_session_packet],VM_INFO_MAGIC
    jne .unavailable
    cmp word [vc_session_packet+VM_INFO_BYTES],VM_INFO_SIZE
    jb .unavailable
    cmp word [vc_session_packet+VM_INFO_VERSION],VM_ABI_VERSION
    jb .unavailable
    test dword [vc_session_packet+VM_INFO_CAPABILITIES],VM_CAP_DESKTOP_ROWS
    jz .unavailable
    cmp dword [vc_session_packet+VM_INFO_ACTIVE],0
    jne .unavailable
    mov eax,[vc_session_packet+VM_INFO_FB_BOUND]
    test eax,eax
    jz .bind
    mov eax,[vc_session_packet+VM_INFO_FB_PHYSICAL]
    cmp eax,[vc_session_wanted_physical]
    jne .unavailable             ; never displace a binding for another LFB
    mov eax,[vc_session_packet+VM_INFO_FB_BYTES]
    cmp eax,[vc_session_wanted_bytes]
    jb .expand_owned             ; only our own mapping may be enlarged
    test dword [vc_session_packet+VM_INFO_CAPABILITIES],VM_CAP_GPU_PRESENT
    setnz byte [vc_session_gpu]
    jz .queried_gpu
    test dword [vc_session_packet+VM_INFO_CAPABILITIES],VM_CAP_GPU_ASYNC
    jnz .queried_gpu
    inc byte [vc_session_gpu]
.queried_gpu:
    mov byte [vc_session_bound],1
    mov eax,[vc_session_wanted_physical]
    mov [vc_session_bound_physical],eax
    mov eax,[vc_session_packet+VM_INFO_FB_BYTES]
    mov [vc_session_bound_bytes],eax
    clc
    jmp .done
.expand_owned:
    cmp byte [vc_session_owned],1
    jne .unavailable
    call vc_session_fb_release
    jc .unavailable
.bind:
    mov dword [vc_session_packet],VM_FB_PACKET_MAGIC
    mov dword [vc_session_packet+VM_FB_PACKET_VERSION],(VM_FB_ACCEL_PACKET_SIZE << 16) | VM_ABI_VERSION
    mov eax,[vc_session_wanted_physical]
    mov [vc_session_packet+VM_FB_PACKET_PHYSICAL],eax
    mov eax,[vc_session_wanted_bytes]
    mov [vc_session_packet+VM_FB_PACKET_LENGTH],eax
    mov eax,[vc_info+18]         ; XResolution/YResolution
    mov [vc_session_packet+16],eax
    mov ax,[vc_pitch]
    mov [vc_session_packet+20],ax
    movzx ax,byte [vc_info+25]
    ; Reserve BPP bit15 as the current mode's banked-window marker. The host
    ; still requires a nonzero VBE PhysBasePtr and validates the reported BAR
    ; against a known QEMU VGA/virtio-vga alias before accepting banked mode.
    cmp byte [vc_lfb],0
    jne .format_ready
    or ax,0x8000
.format_ready:
    mov [vc_session_packet+22],ax
    movzx eax,word [vc_controller+18]
    shl eax,16
    mov [vc_session_packet+24],eax
    mov dword [vc_session_packet+28],0
    mov ax,VM_OP_BIND_FB | VM_OP_NO_SWITCH
    mov cx,VM_FB_ACCEL_PACKET_SIZE
    push cs
    pop es
    mov di,vc_session_packet
    mov byte [vc_session_owned],1 ; a failed bind may retain our mapping
    call far [vc_session_entry]
    jc .failed_bind
    mov [vc_session_gpu],bl
    mov byte [vc_session_bound],1
    mov eax,[vc_session_wanted_physical]
    mov [vc_session_bound_physical],eax
    mov eax,[vc_session_wanted_bytes]
    mov [vc_session_bound_bytes],eax
    clc
    jmp .done
.failed_bind:
    call vc_session_fb_release
.unavailable:
    stc
.done:
    pop fs
    pop es
    pop ds
    popad
    ret

; Release only a binding created by this renderer, after presentation stops.
vc_session_fb_release:
    pushad
    cmp byte [cs:vc_session_owned],1
    jne .detached
    mov ax,VM_OP_UNBIND_FB | VM_OP_NO_SWITCH
    call far [cs:vc_session_entry]
    jc .keep
    mov byte [cs:vc_session_owned],0
.detached:
    mov byte [cs:vc_session_bound],0
    mov byte [cs:vc_session_gpu],0
    clc
.keep:
    popad
    ret

; DX = completed viewport's first scanline. Native GPU queues/polls without
; waiting for a frame fence; VBE-only bindings return CF and use INT 10h.
vc_session_fb_present:
    pushad
    cmp byte [cs:vc_session_gpu],0
    je .unavailable
    mov ax,VM_OP_FB_PRESENT | VM_OP_NO_SWITCH
    call far [cs:vc_session_entry]
    popad
    ret
.unavailable:
    popad
    stc
    ret

%ifdef VC_TRACE_DOS
; Read-only runtime driver evidence, captured after the first completed paint.
vc_gpu_log:
    pushf
    pushad
    push ds
    push es
    push cs
    pop ds
    push cs
    pop es
    call vc_session_find
    jc .done
    mov di,file_buf
    mov cx,VM_DISPLAY_INFO_SIZE
    mov ax,VM_OP_DISPLAY_INFO | VM_OP_NO_SWITCH
    call far [vc_session_entry]
    jc .done
    cmp dword [file_buf],VM_DISPLAY_INFO_MAGIC
    jne .done
    mov dx,vc_gpu_log_path
    xor cx,cx
    mov ah,0x3C
    int 0x21
    jc .done
    mov bx,ax
    mov dx,file_buf
    mov cx,VM_DISPLAY_INFO_SIZE
    mov ah,0x40
    int 0x21
    mov ah,0x3E
    int 0x21
.done:
    pop es
    pop ds
    popad
    popf
    ret
vc_gpu_log_path db '\SYSTEM\VIDEO\GPU.LOG',0
%endif

; One-row vc_fb_xfer interface: FS:SI buffer, EDI framebuffer offset, CX
; byte count, direction from vc_fb_read. Reuses the multi-row packet path.
vc_session_xfer:
    pushad
    push fs
    mov bx,cx
    mov dx,1
    call vc_session_rows
    pop fs
    popad
    ret

; EAX native colour, ECX pixel count, EDI framebuffer byte offset.
; The shared 64-byte packet holds this bounded, exact 32-byte fill request.
vc_session_fill:
%if VC_PROTECTED_LFB
    pushad
    push es
    push eax
    call vc_session_guard
    jnc .ready
    call vc_session_fb_bind
    jc .unavailable
.ready:
    pop eax
    mov dword [cs:vc_session_packet],VM_FB_FILL_MAGIC
    mov dword [cs:vc_session_packet+4],(VM_FB_FILL_PACKET_SIZE << 16) | VM_ABI_VERSION
    mov [cs:vc_session_packet+8],edi
    mov [cs:vc_session_packet+12],ecx
    mov [cs:vc_session_packet+16],eax
    movzx eax,byte [cs:vc_bytes]
    mov [cs:vc_session_packet+20],eax
    mov dword [cs:vc_session_packet+24],0
    mov dword [cs:vc_session_packet+28],0
    push cs
    pop es
    mov di,vc_session_packet
    mov cx,VM_FB_FILL_PACKET_SIZE
    mov ax,VM_OP_FB_FILL | VM_OP_NO_SWITCH
    call far [cs:vc_session_entry]
    jnc .done
    mov byte [cs:vc_active],0
    jmp .done
.unavailable:
    pop eax
    mov byte [cs:vc_active],0
.done:
    pop es
    popad
    ret
%else
    stc
    ret
%endif

; row_bytes must be nonzero. Return CF for a row wider than one packet.
vc_session_batch:
    mov eax,[vc_session_row_bytes]
    cmp eax,VM_FB_ROWS_MAX_BYTES
    ja .wide
    mov ebx,eax
    mov eax,VM_FB_ROWS_MAX_BYTES
    xor edx,edx
    div ebx
    cmp eax,VM_FB_ROWS_MAX_COUNT
    jbe .ready
    mov eax,VM_FB_ROWS_MAX_COUNT
.ready:
    mov [vc_session_rows_per_call],eax
    clc
    ret
.wide:
    stc
    ret

; FS:SI source rows, BX source stride, CX bytes per row, DX rows, EDI
; framebuffer offset. A packet carries one row width and at most 64 rows / 16
; KiB total, so unusually wide rows use the ordinary VBE fallback.
; Return CF=1 for unavailable/failed service so callers use normal VBE I/O.
vc_session_rows:
    pushad
    push ds
    push cs
    pop ds
    push es
    push fs
    call vc_session_guard
    jc .unavailable
    movzx eax,cx
    test eax,eax
    jz .success
    mov [vc_session_row_bytes],eax
    movzx eax,dx
    test eax,eax
    jz .success
    mov [vc_session_rows_left],eax
    movzx eax,si
    mov [vc_session_source_base],eax
    movzx eax,bx
    mov [vc_session_source_stride],eax
    mov [vc_session_dest_base],edi
    mov dword [vc_session_y],0
    call vc_session_batch
    jc .unavailable
    mov ax,fs
    mov [vc_session_source_seg],ax
    movzx eax,byte [cs:vc_fb_read]
    xor eax,1                      ; ABI 0=GET, 1=PUT; vc_fb_read is inverse
    mov [vc_session_direction],ax
    mov eax,[vc_session_rows_left]
    mov [vc_session_chunk_rows_left],eax
.y_chunk:
    mov eax,[vc_session_chunk_rows_left]
    cmp eax,[vc_session_rows_per_call]
    jbe .row_count_ready
    mov eax,[vc_session_rows_per_call]
.row_count_ready:
    mov [vc_session_call_rows],eax
    ; Keep every CVFR source range inside its 64 KiB segment.
    mov eax,[vc_session_source_base]
    mov ebx,[vc_session_source_stride]
    imul ebx,[vc_session_y]
    add eax,ebx
    mov edx,[vc_session_call_rows]
    dec edx
    imul edx,[vc_session_source_stride]
    add eax,edx
    add eax,[vc_session_row_bytes]
    cmp eax,10000h
    ja .unavailable
    mov ax,[vc_session_source_seg]
    mov [vc_session_rows_packet+VM_FB_ROWS_SEG],ax
    mov eax,[vc_session_source_base]
    mov ebx,[vc_session_source_stride]
    imul ebx,[vc_session_y]
    add eax,ebx
    mov [vc_session_rows_packet+VM_FB_ROWS_OFF],ax
    mov eax,[vc_session_row_bytes]
    mov [vc_session_rows_packet+VM_FB_ROWS_ROW_BYTES],ax
    mov eax,[vc_session_call_rows]
    mov [vc_session_rows_packet+VM_FB_ROWS_COUNT],ax
    mov eax,[vc_session_source_stride]
    mov [vc_session_rows_packet+VM_FB_ROWS_SRC_STRIDE],ax
    mov ax,[vc_session_direction]
    mov [vc_session_rows_packet+VM_FB_ROWS_DIRECTION],ax
    movzx eax,word [cs:vc_pitch]
    mov [vc_session_rows_packet+VM_FB_ROWS_DST_PITCH],eax
    mov eax,[vc_session_dest_base]
    mov ebx,[vc_session_y]
    movzx edx,word [cs:vc_pitch]
    imul ebx,edx
    add eax,ebx
    mov [vc_session_rows_packet+VM_FB_ROWS_DST_OFFSET],eax
    mov dword [vc_session_rows_packet+VM_FB_ROWS_RESERVED],0
    mov dword [vc_session_rows_packet],VM_FB_ROWS_MAGIC
    mov dword [vc_session_rows_packet+4],(VM_FB_ROWS_PACKET_SIZE << 16) | VM_ABI_VERSION
    mov ax,VM_OP_FB_ROWS | VM_OP_NO_SWITCH
    mov cx,VM_FB_ROWS_PACKET_SIZE
    push cs
    pop es
    mov di,vc_session_rows_packet
    call far [vc_session_entry]
    mov [vc_session_row_status],ax
    jc .unavailable
    inc dword [vc_session_row_calls]
    mov eax,[vc_session_call_rows]
    add [vc_session_y],eax
    sub [vc_session_chunk_rows_left],eax
    jnz .y_chunk
.success:
    clc
    jmp .done
.unavailable:
    stc
.done:
    pop fs
    pop es
    pop ds
    popad
    ret

; ESI/EDI source/destination byte offsets, CX row bytes, DX row count. CVFC
; copies directly between the two pages of the same validated physical LFB.
; CF=1 requests the compositor's scratch get/put fallback.
vc_fb_copy_rows:
    pushad
    push ds
    push cs
    pop ds
    push es
    push fs
    call vc_session_guard
    jc .unavailable
    movzx eax,cx
    test eax,eax
    jz .success
    mov [vc_session_row_bytes],eax
    movzx eax,dx
    test eax,eax
    jz .success
    mov [vc_session_rows_left],eax
    mov [vc_session_source_base],esi
    mov [vc_session_dest_base],edi
    mov dword [vc_session_y],0
    call vc_session_batch
    jc .unavailable
    mov eax,[vc_session_rows_left]
    mov [vc_session_chunk_rows_left],eax
.copy_y:
    mov eax,[vc_session_chunk_rows_left]
    cmp eax,[vc_session_rows_per_call]
    jbe .copy_count_ready
    mov eax,[vc_session_rows_per_call]
.copy_count_ready:
    mov [vc_session_call_rows],eax
    mov eax,[vc_session_source_base]
    mov ebx,[vc_session_y]
    movzx edx,word [cs:vc_pitch]
    imul ebx,edx
    add eax,ebx
    mov [vc_session_page_packet+VM_FB_PAGE_SRC_OFFSET],eax
    mov eax,[vc_session_dest_base]
    mov ebx,[vc_session_y]
    movzx edx,word [cs:vc_pitch]
    imul ebx,edx
    add eax,ebx
    mov [vc_session_page_packet+VM_FB_PAGE_DST_OFFSET],eax
    mov eax,[vc_session_row_bytes]
    mov [vc_session_page_packet+VM_FB_PAGE_ROW_BYTES],ax
    mov eax,[vc_session_call_rows]
    mov [vc_session_page_packet+VM_FB_PAGE_COUNT],ax
    movzx eax,word [cs:vc_pitch]
    mov [vc_session_page_packet+VM_FB_PAGE_PITCH],eax
    mov dword [vc_session_page_packet+VM_FB_PAGE_RESERVED0],0
    mov dword [vc_session_page_packet+VM_FB_PAGE_RESERVED1],0
    mov dword [vc_session_page_packet],VM_FB_PAGE_MAGIC
    mov dword [vc_session_page_packet+4],(VM_FB_PAGE_PACKET_SIZE << 16) | VM_ABI_VERSION
    mov ax,VM_OP_FB_PAGE_COPY | VM_OP_NO_SWITCH
    mov cx,VM_FB_PAGE_PACKET_SIZE
    push cs
    pop es
    mov di,vc_session_page_packet
    call far [vc_session_entry]
    jc .unavailable
    mov eax,[vc_session_call_rows]
    add [vc_session_y],eax
    sub [vc_session_chunk_rows_left],eax
    jnz .copy_y
.success:
    clc
    jmp .done
.unavailable:
    stc
.done:
    pop fs
    pop es
    pop ds
    popad
    ret

; Recheck cached binding identity/extent on every acceleration attempt. A VBE
; mode can change before the next compositor bind; never write through the
; previous mode's still-live CVSESSION binding in that interval.
vc_session_guard:
    cmp byte [vc_session_bound],1
    jne .unavailable
    mov eax,[cs:vc_info+40]
    cmp eax,[vc_session_bound_physical]
    jne .unavailable
    mov eax,[cs:vc_access_bytes]
    cmp eax,[vc_session_bound_bytes]
    ja .unavailable
    clc
    ret
.unavailable:
    stc
    ret

align 4
vc_session_entry dd 0
vc_session_bound db 0
vc_session_gpu db 0
vc_session_owned db 0
vc_session_row_status dw 0
vc_session_row_calls dd 0
vc_session_packet times VM_INFO_SIZE db 0
vc_session_info equ vc_session_packet
vc_session_bind_packet equ vc_session_packet
vc_session_rows_packet equ vc_session_packet
vc_session_page_packet equ vc_session_packet
vc_session_wanted_physical dd 0
vc_session_wanted_bytes dd 0
vc_session_bound_physical dd 0
vc_session_bound_bytes dd 0
vc_session_source_seg dw 0
vc_session_direction dw 0
vc_session_source_base dd 0
vc_session_source_stride dd 0
vc_session_dest_base dd 0
vc_session_row_bytes dd 0
vc_session_rows_left dd 0
vc_session_rows_per_call dd 0
vc_session_chunk_rows_left dd 0
vc_session_call_rows dd 0
vc_session_y dd 0

%else
; Setup keeps the conventional real-mode VBE transports and omits CVSESSION's
; protected-mode client. Preserve the shared renderer's call sites with
; minimal stubs; no protected mapping can be reported as available.
vc_session_find:
vc_session_available:
vc_session_fb_bind:
vc_session_xfer:
vc_session_fill:
vc_session_rows:
    stc
    ret

vc_session_fb_release:
    clc
    ret

vc_session_fb_present:
    ret

vc_session_owned db 0
%endif
