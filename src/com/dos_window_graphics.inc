; Cooperative indexed source-port graphics. This is not a V86/DOS emulator.
; The guest CPU runs the engine; its DPMI real-mode calls submit finished
; frames. Existing full-screen executables do not use this private ABI.
%include "src/com/graphics_bridge_abi.inc"

cg_initialize:
    mov ax,cs
    mov [cg_descriptor+CG_PALETTE+2],ax
    mov [cg_descriptor+CG_PRESENT+2],ax
    mov [cg_descriptor+CG_INPUT+2],ax
    mov ax,[cg_buffer_seg]
    mov [cg_descriptor+CG_PIXELS+2],ax
    movzx ax,byte [cg_bpp]
    mov [cg_descriptor+CG_HOST_BPP],ax
    ret

cg_multiplex:
    cmp bx,CG_DISCOVER_BX
    jne .chain
    cmp ax,CG_DISCOVER_AX
    je .discover
    cmp byte [cs:cg_active],1
    jne .chain
    cmp ax,CG_PRESENT_AX
    je .present
    cmp ax,CG_INPUT_AX
    je .input
    jmp .chain
.present:
    mov ax,cx
    push cs
    call cg_present
    iret
.input:
    push cs
    call cg_input
    push bp
    mov bp,sp
    jc .empty
    and word [ss:bp+6],~1
    jmp .return
.empty:
    or word [ss:bp+6],1
.return:
    pop bp
    iret
.discover:
    cmp byte [cs:guest_live],1
    jne .chain
    cmp byte [cs:cg_bpp],15
    jb .chain                    ; shared indexed desktop palette stays intact
    cmp word [cs:cg_buffer_seg],0
    je .chain
    mov byte [cs:cg_active],1
    and word [cs:errors],~DW_ERR_PROTECTED
    mov ax,CG_DISCOVER_REPLY
    push cs
    pop es
    mov di,cg_descriptor
    iret
.chain:
    jmp far [cs:cg_old_int2f]

; BIOS AT keyboard intercept. Preserve the old handler's flags/translation:
; only accepted raw Set1 bytes are queued. Never steal port60 from BIOS.
cg_keyboard_intercept:
    pushf
    cmp ah,0x4F
    jne .chain
    cmp byte [cs:cg_active],1
    jne .chain
    popf                        ; AH4F uses the BIOS caller's incoming CF
    pushf
    call far [cs:cg_old_int15]
    pushf
    push bp
    mov bp,sp
    pushad
    push ds
    push es
    push cs
    pop ds
    test word [ss:bp+2],1
    jz .done
    cmp al,0xE0
    je .extended
    cmp al,0xE1
    je .pause
    cmp byte [cg_pause],0
    je .event
    dec byte [cg_pause]
    jmp .done
.extended:
    mov byte [cg_extended],1
    jmp .done
.pause:
    mov byte [cg_pause],5
    mov byte [cg_extended],0
    jmp .done
.event:
    mov dl,al
    and ax,0x007F
    mov ah,[cg_extended]
    mov byte [cg_extended],0
    movzx bx,byte [cg_key_write]
    mov cl,bl
    inc cl
    and cl,63
    cmp cl,[cg_key_read]
    je .overflow
    shl bx,2
    mov [cg_keys+bx],ax
    shr dl,7
    xor dl,1
    mov [cg_keys+bx+2],dl
    mov [cg_key_write],cl
    jmp .done
.overflow:
    mov word [cg_descriptor+CG_STATUS],2
.done:
    pop es
    pop ds
    popad
    pop bp
    popf
    retf 2
.chain:
    popf
    jmp far [cs:cg_old_int15]

; Full register preservation, independent stack even on small DPMI stacks.
; FAR entry returns CF=0; status reports errors. Input uses its own return CF.
cg_present:
    pushfd
    pushad
    push ds
    push es
    push fs
    push gs
    cli
    push cs
    pop ds
    cmp byte [cg_entry_busy],0
    jne .restore
    mov byte [cg_entry_busy],1
    mov [cg_saved_ss],ss
    mov [cg_saved_sp],sp
    mov bx,cs
    mov ss,bx
    mov sp,cg_stack_top
    push ax
    test al,CG_PRESENT_PALETTE
    jz .clock
    call cg_convert_palette
.clock:
    call cg_update_clock
    pop ax
    test al,2                    ; polling never submits or counts a frame
    jnz .service
    mov dword [dirty_rows],0x01FFFFFF
    mov byte [cg_submitted],1
.service:
    call service_host
    call cg_update_flags
    cmp byte [cg_submitted],0
    je .stack
    mov byte [cg_submitted],0
    ; Host result bit3 is actual visible graphics paint completion.
    test word [last_host_result],8
    jz .stack
    inc dword [cg_descriptor+CG_PRESENTED_FRAMES]
.stack:
    mov ax,[cg_saved_ss]
    mov ss,ax
    mov sp,[cg_saved_sp]
    mov byte [cg_entry_busy],0
.restore:
    pop gs
    pop fs
    pop es
    pop ds
    popad
    popfd
    clc
    retf

cg_input:
    push bp
    mov bp,sp
    pushfd
    pushad
    push ds
    cli
    push cs
    pop ds
    call cg_update_clock
    call cg_update_flags
    movzx bx,byte [cg_key_read]
    cmp bl,[cg_key_write]
    je .empty
    mov cl,bl
    inc cl
    and cl,63
    mov [cg_key_read],cl
    shl bx,2
    mov ax,[cg_keys+bx]
    mov [ss:bp-8],ax            ; saved EAX after pushfd/pushad
    movzx ax,byte [cg_keys+bx+2]
    mov [ss:bp-20],ax           ; saved EBX
    pop ds
    popad
    popfd
    pop bp
    clc
    retf
.empty:
    pop ds
    popad
    popfd
    pop bp
    stc
    retf

cg_update_flags:
    movzx ax,byte [guest_focus]
    or ax,CG_FLAG_MULTIPLEX
    cmp byte [close_request],0
    je .save
    or ax,CG_FLAG_CLOSE
.save:
    mov [cg_descriptor+CG_FLAGS],ax
    ret

; Existing PIT0 remains in BIOS mode, divisor65536. Account for an IRQ0
; pending while interrupts are masked. No timer/RTC programming or TSC speed
; assumption. Convert elapsed PIT cycles with a remainder to avoid drift.
cg_update_clock:
    push es
    mov ax,0x40
    mov es,ax
    mov ebx,[es:0x6C]
    mov al,0xC2                 ; latch status and count of channel0 together
    out 0x43,al
    in al,0x40
    mov dl,al                   ; OUT and counting mode belong to this count
    in al,0x40
    mov ah,al
    in al,0x40
    xchg al,ah
    neg ax
    movzx esi,ax
    mov cl,dl
    and cl,6
    cmp cl,6                    ; mode3 square wave counts down by TWO
    jne .pending
    shr esi,1
    test dl,0x80
    jnz .pending
    add esi,32768               ; OUT low identifies the second half-period
.pending:
    mov al,0x0A
    out 0x20,al
    in al,0x20
    test al,1
    jz .cycles
    ; Early phase is after wrap; a late phase was latched just before the
    ; pending edge and must retain the preceding BIOS tick number.
    cmp esi,0x8000
    jae .cycles
    inc ebx
.cycles:
    shl ebx,16
    mov bx,si
    cmp byte [cg_clock_ready],1
    je .elapsed
    mov byte [cg_clock_ready],1
    mov [cg_previous_cycles],ebx
.elapsed:
    mov eax,ebx
    sub eax,[cg_previous_cycles]
    test eax,eax
    jns .forward
    ; The PIC may already have acknowledged an edge while the DPMI host
    ; has not yet reflected it into the BIOS tick. A sample from that one
    ; period must not turn into an unsigned 2^32-cycle / one-hour jump.
    cmp eax,-65536
    jb .clock_fault
    inc dword [cg_descriptor+40]
    pop es
    ret
.clock_fault:
    mov word [cg_descriptor+CG_STATUS],3
    pop es
    ret
.forward:
    mov [cg_previous_cycles],ebx
    mov ecx,1000
    mul ecx
    add eax,[cg_clock_remainder]
    adc edx,0
    mov ecx,1193182
    div ecx
    mov [cg_clock_remainder],edx
    add [cg_descriptor+CG_MILLISECONDS],eax
    pop es
    ret

cg_convert_palette:
    mov si,cg_palette
    mov di,cg_native_palette
    mov bp,256
.color:
    xor ebx,ebx
    mov dx,cg_masks
    push bp
    mov bp,3
.channel:
    movzx eax,byte [si]
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
    mov [di],ebx
    add di,4
    pop bp
    dec bp
    jnz .color
    ret

align 4
cg_descriptor:
    dw CG_ABI_VERSION,CG_DESCRIPTOR_BYTES,320,200,320,CG_FLAG_FOCUSED|CG_FLAG_MULTIPLEX
    dw 0,0,cg_palette,0,cg_present,0,cg_input,0
    dd 0,0
    dw 0,0
    dq 0
cg_old_int2f dd 0
cg_old_int15 dd 0
cg_saved_ss dw 0
cg_saved_sp dw 0
cg_entry_busy db 0
cg_submitted db 0
cg_extended db 0
cg_pause db 0
cg_key_write db 0
cg_key_read db 0
cg_clock_ready db 0
align 4
cg_previous_cycles dd 0
cg_clock_remainder dd 0
cg_keys times 64 dd 0
cg_palette times 768 db 0
cg_native_palette times 256 dd 0
align 16
cg_stack times 4096 db 0
cg_stack_top:
