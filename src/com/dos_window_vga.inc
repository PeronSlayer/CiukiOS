; Monitored-VGA session mode of the DOS window runtime (video/presenter track).
;
; Used only when the runtime is installed under a V86 monitor with CVSESSION
; loaded. The unmodified DOS program then runs with every A0000-BFFFF cycle,
; VGA port and INT 10h served by the software VGA model in the JLM. The
; native compositor keeps its banked VBE path: while it paints (foreground,
; or at IRQ0 time around the guest) the JLM "host mode" executes its A000
; window writes straight into the owned protected LFB mapping and emulates
; its INT 10h/4F05 bank switches. This adapter never switches CR0, never
; calls firmware from interrupt time and never maps plain RAM over VGA.
;
; Lifecycle: INSTALL (foreground) = CONFIG, BIND_FB, BEGIN, HOST_ENTER.
; The shell's own EXEC of the guest leaves host mode for exactly its duration
; (INT 21h/4Bh hook); IRQ0 ticks re-enter host mode around the shell callback.
; UNINSTALL = HOST_LEAVE, END, UNBIND and a physical bank resynchronisation.

%include "src/vm/session_abi.inc"
%include "src/vm/session_video_abi.inc"
%include "src/vm/session_devices_abi.inc"
%include "src/vm/session_scheduler_abi.inc"

VGA_WINDOW_WIDTH equ 640              ; shell DOS window client area
VGA_WINDOW_HEIGHT equ 400
VGA_BAND_HEIGHT equ 16                ; one DW dirty-row bit per 16 pixels

; Foreground install under V86. DS=CS. CF=1 can retain owned resources if
; rollback is refused. DW_INSTALLED stays set until UNINSTALL really succeeds.
vga_session_install:
    cmp word [host_callback+2],0
    je .fail
    cmp word [host_ds],0
    je .fail
    xor di,di
    mov es,di
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    int 2Fh
    push cs
    pop ds
    mov ax,es
    or ax,di
    jz .fail
    mov [vga_entry],di
    mov [vga_entry+2],es
    xor cx,cx
    mov ax,VM_OP_QUERY
    call far [vga_entry]
    jc .fail
    and bx,VM_VIDEO_CAPABILITIES
    cmp bx,VM_VIDEO_CAPABILITIES
    jne .fail
    ; Firmware fonts and VBE state are read before the session exists.
    mov bh,3
    call vga_font_pointer
    mov [vga_config+VM_VCFG_FONT_8X8],eax
    mov bh,4
    call vga_font_pointer
    mov [vga_config+VM_VCFG_FONT_8X8_HIGH],eax
    mov bh,2
    call vga_font_pointer
    mov [vga_config+VM_VCFG_FONT_8X14],eax
    mov bh,6
    call vga_font_pointer
    mov [vga_config+VM_VCFG_FONT_8X16],eax
    call vga_measure_tsc
    mov [vga_config+VM_VCFG_TSC_KHZ],eax
    mov [DW_TSC_KHZ],eax
    call vga_query_vbe
    jc .fail
    push cs
    pop es
    mov di,vga_config
    mov ax,VM_OP_VIDEO_CONFIG
    call far [vga_entry]
    jc .fail
    mov di,vga_binding
    mov cx,VM_FB_PACKET_SIZE
    ; Keep an invariant keyboard path even if setup fails before hooks exist.
    push es
    xor ax,ax
    mov es,ax
    mov eax,[es:0x16*4]
    mov [old_int16],eax
    pop es
    mov ax,VM_OP_BIND_FB
    call far [vga_entry]
    jc .fail
    mov byte [vga_session],2         ; bound, BEGIN not yet committed
    mov byte [installed],1
    ; The host scheduler (and with it Jemm's run-time negotiated virtual-IF
    ; profile that guest devices need) is armed by BEGIN only when bound.
    push cs
    pop es
    mov di,vga_scheduler
    mov cx,CVSCHED_BYTES
    mov ax,VM_OP_BIND_SCHED
    call far [vga_entry]
    jc .no_scheduler
    mov byte [vga_sched_bound],1
.no_scheduler:
    mov ax,VM_OP_BEGIN
    call vga_cleanup_call
    jc .begin_failed
    mov byte [vga_session],1
    call vga_host_enter
    jc .cleanup
    ; Guest keyboard/mouse/SB16/OPL when Jemm's virtual-IF profile was
    ; negotiated; otherwise the BIOS INT 16h focus path below stays in use.
    call vga_device_begin
    ; Vectors: timer (host service), BIOS keyboard (focus), mouse (absent to
    ; the guest; the desktop owns it) and DOS (the host's EXEC boundary).
    xor ax,ax
    mov es,ax
    mov eax,[es:0x08*4]
    mov [old_int08],eax
    mov eax,[es:0x16*4]
    mov [old_int16],eax
    mov eax,[es:0x33*4]
    mov [old_int33],eax
    mov eax,[es:0x21*4]
    mov [vga_old_int21],eax
    call disable_physical_mouse_callback
    mov byte [vga_hooks],1
    cli
    xor ax,ax
    mov es,ax
    mov word [es:0x08*4],timer_handler
    mov word [es:0x16*4],keyboard_handler
    mov word [es:0x33*4],mouse_handler
    mov word [es:0x21*4],vga_int21
    mov ax,cs
    mov [es:0x08*4+2],ax
    mov [es:0x16*4+2],ax
    mov [es:0x33*4+2],ax
    mov [es:0x21*4+2],ax
    ; Keystrokes typed for the host (for example the Run field) must not be
    ; replayed to the guest.
    call keyboard_discard
    mov byte [vga_session],1
    mov byte [vga_exec_depth],0
    mov byte [cg_active],1           ; the compositor calls DW_GFX_DRAW
    mov byte [installed],1
    sti
    clc
    ret
.begin_failed:
    ; BEGIN can itself retain ownership after a failed rollback.
    xor cx,cx
    mov ax,VM_OP_QUERY
    call far [vga_entry]
    jc .begin_owned
    test dx,dx
    jz .cleanup
.begin_owned:
    mov byte [vga_session],1
.cleanup:
    call vga_session_uninstall
.fail:
    push cs
    pop ds
    stc
    ret

; Foreground transaction. END failure retains active mapping/hooks and restores
; host mode before returning CF=1. END success is committed before UNBIND, so
; a refused UNBIND retries only that stage. No success flag precedes ownership.
vga_session_uninstall:
    cmp byte [vga_session],1
    jne .unbind
    call vga_device_end
    jc .failed
    cmp byte [vga_end_started],0
    jne .leave
    push cs
    pop es
    mov di,vga_state
    mov ax,VM_OP_VIDEO_STATE
    call vga_cleanup_call
    jc .failed
    mov byte [vga_end_started],1     ; preserve cached bank across partial END
.leave:
    call vga_host_leave
    jc .failed
.end:
    mov ax,VM_OP_END
    call vga_cleanup_call
    jnc .ended
    call vga_host_enter
    jnc .failed                    ; retained model still serves host writes
    ; A partially completed monitor rollback can no longer serve host pixels.
    ; Never return to a compositor in that state. Keep allocations/hooks,
    ; report through serial/debug output, and retry on saved BIOS Enter only.
    mov si,vga_cleanup_notice
    call vga_cleanup_serial
.wait:
    mov ah,0
    pushf
    call far [cs:old_int16]
    cmp ax,1C0Dh
    jne .wait
    push cs
    pop ds
    jmp .end
.ended:
    mov byte [vga_session],2
    mov byte [cg_active],0
    cmp byte [vga_hooks],0
    je .resync
    pushf
    cli
    xor ax,ax
    mov es,ax
    mov eax,[old_int08]
    mov [es:0x08*4],eax
    mov eax,[old_int16]
    mov [es:0x16*4],eax
    mov eax,[old_int33]
    mov [es:0x33*4],eax
    mov eax,[vga_old_int21]
    mov [es:0x21*4],eax
    mov byte [vga_hooks],0
    ; Keys the guest left in the shared BIOS ring (e.g. a peeked close Esc)
    ; belong to the exited child, never to the desktop.
    call keyboard_discard
    popf
    call restore_physical_mouse_callback
.resync:
    ; Physical window = the bank the compositor believes is selected.
    mov eax,[vga_state+VM_VS_HOST_BANK]
    xor edx,edx
    div dword [vga_host_packet+VM_VH_GRANULE]
    mov dx,ax
    xor bx,bx
    mov ax,4F05h
    int 10h
    cmp ax,004Fh
    je .bank_ok
    mov word [DW_CLEANUP_OP],4F05h
    mov [DW_CLEANUP_ERROR],ax
    mov si,vga_cleanup_notice
    call vga_cleanup_serial
.bank_wait:
    mov ah,0
    pushf
    call far [cs:old_int16]
    cmp ax,1C0Dh
    jne .bank_wait
    jmp .resync
.bank_ok:
    mov byte [vga_end_started],0
.unbind:
    cmp byte [vga_sched_bound],0
    je .unbind_fb
    mov ax,VM_OP_UNBIND_SCHED
    call vga_cleanup_call
    jc .failed
    mov byte [vga_sched_bound],0
.unbind_fb:
    mov ax,VM_OP_UNBIND_FB
    call vga_cleanup_call
    jc .failed
    push cs
    pop ds
    mov byte [vga_session],0
    mov byte [cg_active],0
    mov byte [installed],0
    clc
    ret
.failed:
    push cs
    pop ds
    cmp byte [vga_host_entered],0
    je .diagnostic
    mov byte [vga_end_started],0     ; next retry re-reads the host's new bank
.diagnostic:
    or word [errors],DW_ERR_LIFECYCLE
    stc
    ret

; Record only failed operations, so a successful recovery HOST_ENTER does not
; erase the original END error. AX/CF preserve the actual monitor result.
vga_cleanup_call:
    push bx
    push ax
    call far [cs:vga_entry]
    pop bx
    jnc .done
    mov [cs:DW_CLEANUP_OP],bx
    mov [cs:DW_CLEANUP_ERROR],ax
.done:
    pop bx
    ret

; No BIOS video, aperture stores, mode changes or allocator activity. UART
; polling is bounded; lack of a connected serial terminal cannot hang cleanup.
vga_cleanup_serial:
    pushad
.next:
    lodsb
    test al,al
    jz .done
    out 0E9h,al
    mov bl,al
    mov cx,256
    mov dx,3FDh
.ready:
    in al,dx
    test al,20h
    jnz .send
    loop .ready
    jmp .next
.send:
    mov dx,3F8h
    mov al,bl
    out dx,al
    jmp .next
.done:
    popad
    ret

vga_host_enter:
    pushad
    push ds
    push es
    push cs
    pop ds
    push cs
    pop es
    mov di,vga_host_packet
    mov ax,VM_OP_VIDEO_HOST_ENTER
    call far [cs:vga_entry]
    jc .done
    mov byte [cs:vga_host_entered],1
    mov byte [cs:DW_V86_HOST],1
    ; Later entries keep the bank the compositor selected meanwhile.
    mov dword [cs:vga_host_packet+VM_VH_WINDOW],0FFFFFFFFh
    clc
.done:
    pop es
    pop ds
    popad
    ret

vga_host_leave:
    pushad
    push ds
    push es
    cmp byte [cs:vga_host_entered],0
    je .clear
    mov ax,VM_OP_VIDEO_HOST_LEAVE
    call vga_cleanup_call
    jc .done
    mov byte [cs:vga_host_entered],0
    mov byte [cs:DW_V86_HOST],0
.clear:
    clc
.done:
    pop es
    pop ds
    popad
    ret

; Host service wrappers used by service_host (DS=CS, private stack).
vga_before_callback:
    cmp byte [vga_session],1
    jne .done
    call vga_host_enter
    pushad
    push es
    call vga_client_height
    movzx edx,ax
    mov ecx,edx                      ; 25 dirty bands cover the client
    add ecx,24
    push eax
    mov eax,ecx
    xor edx,edx
    mov ecx,25
    div ecx
    mov ecx,eax
    pop eax
    movzx edx,ax
    mov ax,VM_OP_VIDEO_DAMAGE
    call far [cs:vga_entry]
    jc .no_damage
    and edx,01FFFFFFh
    or [dirty_rows],edx
.no_damage:
    pop es
    popad
.done:
    ret

vga_after_callback:
    cmp byte [cs:vga_session],1
    jne .done
    call vga_host_leave
.done:
    ret

; INT 21h: only the host's outermost EXEC changes ownership of the window.
vga_int21:
    cmp ah,4Bh
    jne .chain
    cmp byte [cs:vga_exec_depth],0
    jne .chain
    cmp byte [cs:vga_host_entered],1
    jne .chain
    inc byte [cs:vga_exec_depth]
    call vga_host_leave
    pushf
    call far [cs:vga_old_int21]
    pushf
    call vga_host_enter
    dec byte [cs:vga_exec_depth]
    popf
    retf 2
.chain:
    jmp far [cs:vga_old_int21]

; Compositor band painter (DW_GFX_DRAW contract): BX/DX = client origin on
; screen, band described by DW_CELL_*; band pixel (0,0) is screen
; (DW_CELL_LEFT, DW_CELL_TOP). The JLM renders the model scaled into the
; 640x400 client area, clipped to the band. Preserves registers and IF.
vga_draw_band:
    pushad
    push ds
    push es
    push cs
    pop ds
    push cs
    pop es
    movzx eax,word [DW_CELL_STRIDE]
    mov [vga_band_packet+VM_VP_FORMAT],eax
    mov ax,[DW_CELL_RIGHT]
    sub ax,[DW_CELL_LEFT]
    jbe .done
    mov [vga_band_packet+VM_VP_FORMAT+4],ax
    mov ax,[DW_CELL_BOTTOM]
    sub ax,[DW_CELL_TOP]
    jbe .done
    mov [vga_band_packet+VM_VP_FORMAT+6],ax
    movzx ecx,ax
    movzx eax,word [DW_CELL_STRIDE]
    imul eax,ecx
    cmp eax,10000h
    ja .done
    mov [vga_band_packet+VM_VP_BAND_BYTES],eax
    mov al,[DW_CELL_BYTES]
    mov [vga_band_packet+VM_VP_FORMAT+8],al
    mov eax,[DW_GFX_MASKS]
    mov [vga_band_packet+VM_VP_FORMAT+9],eax
    mov ax,[DW_GFX_MASKS+4]
    mov [vga_band_packet+VM_VP_FORMAT+13],ax
    mov ax,[DW_CELL_BAND_SEG]
    mov [vga_band_packet+VM_VP_BAND_SEGMENT],ax
    mov word [vga_band_packet+VM_VP_BAND_TOP],0
    mov [DW_CLIENT_X],bx
    mov [DW_CLIENT_Y],dx
    mov ax,bx
    sub ax,[DW_CELL_LEFT]
    mov [vga_band_packet+VM_VP_REQUEST],ax
    push ax
    call vga_client_width
    mov cx,ax
    pop ax
    add ax,cx
    mov [vga_band_packet+VM_VP_REQUEST+4],ax
    mov ax,dx
    sub ax,[DW_CELL_TOP]
    mov [vga_band_packet+VM_VP_REQUEST+2],ax
    push ax
    call vga_client_height
    mov cx,ax
    pop ax
    add ax,cx
    mov [vga_band_packet+VM_VP_REQUEST+6],ax
    mov di,vga_band_packet
    mov ax,VM_OP_VIDEO_BAND
    call far [vga_entry]
    jc .done
    mov byte [DW_GFX_PAINTED],1
.done:
    pop es
    pop ds
    popad
    retf

; AX = client pixel width/height the shell currently gives the window.
vga_client_width:
    mov ax,[cs:DW_CLIENT_WIDTH]
    test ax,ax
    jnz .done
    mov ax,VGA_WINDOW_WIDTH
.done:
    ret
vga_client_height:
    mov ax,[cs:DW_CLIENT_HEIGHT]
    test ax,ax
    jnz .done
    mov ax,VGA_WINDOW_HEIGHT
.done:
    ret

; ---- Guest devices (CVSESSION peripheral model) ----

vga_device_begin:
    pushad
    push es
    xor cx,cx
    mov ax,VM_OP_QUERY
    call far [cs:vga_entry]
    jc .done
    test bx,VM_CAP_GUEST_INPUT
    jz .done
    mov ebx,CVDEV_CAP_KEYBOARD|CVDEV_CAP_MOUSE|CVDEV_CAP_DMA_SB|CVDEV_CAP_OPL3
    mov ecx,CVDEV_FLAG_AUDIO_OUT
    mov edx,[cs:DW_TSC_KHZ]
    mov ax,VM_OP_DEV_BEGIN
    call far [cs:vga_entry]
    mov [cs:DW_DEVICE_RESULT],ax
    jc .done
    mov [cs:DW_DEVICES],bx
    mov [cs:DW_DEVICE_AUDIO],cl
    mov byte [cs:vga_devices],1
    mov byte [cs:vga_sent_focus],0FFh
    call vga_device_focus
.done:
    pop es
    popad
    ret

; CF=1 when the monitor refused (devices remain owned for a retry).
vga_device_end:
    cmp byte [cs:vga_devices],0
    je .done
    mov ax,VM_OP_DEV_END
    call vga_cleanup_call
    jc .failed
    mov byte [cs:vga_devices],0
    mov word [cs:DW_DEVICES],0
    mov byte [cs:DW_DEVICE_AUDIO],0
.done:
    clc
    ret
.failed:
    stc
    ret

; Tell the model when keyboard focus changes; releases held keys on loss.
vga_device_focus:
    pushad
    cmp byte [cs:vga_devices],0
    je .done
    movzx bx,byte [cs:guest_focus]
    cmp bl,[cs:vga_sent_focus]
    je .done
    mov [cs:vga_sent_focus],bl
    mov ax,VM_OP_DEV_FOCUS
    call far [cs:vga_entry]
.done:
    popad
    ret

; Window close: one real Esc make/break through the device model reaches
; both controller readers and (through the BIOS INT 9) INT 16h readers.
; CF=1 when sent; otherwise the caller queues a BIOS keystroke instead.
vga_device_close_key:
    pushad
    cmp byte [cs:vga_devices],0
    je .none
    mov bl,1
    mov cl,1
    mov ax,VM_OP_DEV_KEY
    call far [cs:vga_entry]
    jc .none
    mov bl,1
    xor cl,cl
    mov ax,VM_OP_DEV_KEY
    call far [cs:vga_entry]
    popad
    stc
    ret
.none:
    popad
    clc
    ret

; ---- Guest-visible virtual INT 33h mouse (VGA session mode) ----
; The desktop keeps the physical pointer; the guest sees the host position
; mapped into its current video mode's range while the pointer is over the
; client area, and buttons only while the window has focus.

vga_guest_mouse:
    push ds
    push cs
    pop ds
    cmp ax,0
    je .reset
    cmp ax,21h
    je .reset
    cmp ax,1
    je .show
    cmp ax,2
    je .hide
    cmp ax,3
    je .get
    cmp ax,4
    je .setpos
    cmp ax,5
    je .press
    cmp ax,6
    je .release
    cmp ax,7
    je .xrange
    cmp ax,8
    je .yrange
    cmp ax,0Bh
    je .mickeys
    cmp ax,0Ch
    je .handler
    cmp ax,0Fh
    je .ratio
    cmp ax,14h
    je .swap
    cmp ax,15h
    je .state_size
    cmp ax,1Ah
    je .set_sensitivity
    cmp ax,1Bh
    je .get_sensitivity
    cmp ax,1Dh
    je .set_page
    cmp ax,1Eh
    je .get_page
    cmp ax,24h
    je .version
    jmp .done
.reset:
    call vga_mouse_defaults
    mov byte [DW_GUEST_MOUSE],1
    mov byte [vm_sync],1             ; adopt the pointer without a motion event
    call vga_mouse_update
    mov ax,0FFFFh
    mov bx,2
    jmp .done
.show:
    cmp word [vm_visible],0
    je .done
    inc word [vm_visible]
    jmp .done
.hide:
    dec word [vm_visible]
    jmp .done
.get:
    call vga_mouse_update
    mov bx,[vm_buttons]
    mov cx,[vm_x]
    mov dx,[vm_y]
    jmp .done
.setpos:
    mov [vm_x],cx
    mov [vm_y],dx
    call vga_mouse_clamp
    jmp .done
.press:
    call vga_mouse_update
    push si
    mov si,bx
    and si,3
    shl si,1
    mov bx,[vm_press_count+si]
    mov cx,[vm_press_x+si]
    mov dx,[vm_press_y+si]
    mov word [vm_press_count+si],0
    pop si
    mov ax,[vm_buttons]
    jmp .done
.release:
    call vga_mouse_update
    push si
    mov si,bx
    and si,3
    shl si,1
    mov bx,[vm_release_count+si]
    mov cx,[vm_release_x+si]
    mov dx,[vm_release_y+si]
    mov word [vm_release_count+si],0
    pop si
    mov ax,[vm_buttons]
    jmp .done
.xrange:
    cmp cx,dx
    jle .xorder
    xchg cx,dx
.xorder:
    mov [vm_xmin],cx
    mov [vm_xmax],dx
    call vga_mouse_clamp
    jmp .done
.yrange:
    cmp cx,dx
    jle .yorder
    xchg cx,dx
.yorder:
    mov [vm_ymin],cx
    mov [vm_ymax],dx
    call vga_mouse_clamp
    jmp .done
.mickeys:
    call vga_mouse_update
    xor cx,cx
    xor dx,dx
    xchg cx,[vm_mickey_x]
    xchg dx,[vm_mickey_y]
    jmp .done
.handler:
    mov [vm_mask],cx
    mov [vm_handler],dx
    mov [vm_handler+2],es
    jmp .done
.swap:
    xchg cx,[vm_mask]
    xchg dx,[vm_handler]
    push ax
    mov ax,es
    xchg ax,[vm_handler+2]
    mov es,ax
    pop ax
    jmp .done
.state_size:
    xor bx,bx
    jmp .done
.ratio:
.set_sensitivity:
    mov [vm_sensitivity],bx
    mov [vm_sensitivity+2],cx
    mov [vm_sensitivity+4],dx
    jmp .done
.get_sensitivity:
    mov bx,[vm_sensitivity]
    mov cx,[vm_sensitivity+2]
    mov dx,[vm_sensitivity+4]
    jmp .done
.set_page:
    mov [vm_page],bx
    jmp .done
.get_page:
    mov bx,[vm_page]
    jmp .done
.version:
    mov bx,0805h
    mov cx,0400h                     ; PS/2 pointing device
.done:
    pop ds
    iret

; Ranges and position for the current video mode (BDA 449h). DS=CS.
vga_mouse_defaults:
    pushad
    push es
    xor ax,ax
    mov es,ax
    mov al,[es:449h]
    mov word [vm_xmin],0
    mov word [vm_ymin],0
    mov word [vm_xmax],639
    mov word [vm_ymax],199
    mov byte [vm_text],0
    cmp al,3
    jbe .text
    cmp al,7
    je .text
    cmp al,11h
    je .tall
    cmp al,12h
    je .tall
    cmp al,0Fh
    je .medium
    cmp al,10h
    je .medium
    jmp .ranges
.tall:
    mov word [vm_ymax],479
    jmp .ranges
.medium:
    mov word [vm_ymax],349
    jmp .ranges
.text:
    mov byte [vm_text],1
    movzx ax,byte [es:484h]
    inc ax
    shl ax,3
    dec ax
    cmp ax,199
    jae .text_rows
    mov ax,199
.text_rows:
    mov [vm_ymax],ax
.ranges:
    mov word [vm_x],320
    mov word [vm_y],100
    mov word [vm_buttons],0
    mov word [vm_visible],0FFFFh
    mov word [vm_mask],0
    mov dword [vm_handler],0
    mov word [vm_mickey_x],0
    mov word [vm_mickey_y],0
    mov word [vm_events],0
    push cs
    pop es
    mov di,vm_press_count
    mov cx,(vm_counts_end-vm_press_count)/2
    xor ax,ax
    rep stosw
    call vga_mouse_clamp
    pop es
    popad
    ret

vga_mouse_clamp:
    push ax
    mov ax,[vm_x]
    cmp ax,[vm_xmin]
    jge .x_low
    mov ax,[vm_xmin]
.x_low:
    cmp ax,[vm_xmax]
    jle .x_high
    mov ax,[vm_xmax]
.x_high:
    mov [vm_x],ax
    mov ax,[vm_y]
    cmp ax,[vm_ymin]
    jge .y_low
    mov ax,[vm_ymin]
.y_low:
    cmp ax,[vm_ymax]
    jle .y_high
    mov ax,[vm_ymax]
.y_high:
    mov [vm_y],ax
    pop ax
    ret

; Map the host pointer into the guest range; record motion/button events.
; DS=CS. Preserves registers.
vga_mouse_update:
    pushad
    cmp byte [DW_GUEST_MOUSE],0
    je .done
    ; Only real host pointer/button changes move the guest pointer: window
    ; repaints, moves or origin updates are not guest mouse motion.
    cmp byte [vm_sync],0
    jne .host_changed
    mov ax,[mouse_x]
    cmp ax,[vm_host_x]
    jne .host_changed
    mov ax,[mouse_y]
    cmp ax,[vm_host_y]
    jne .host_changed
    mov ax,[mouse_buttons]
    cmp ax,[vm_host_buttons]
    je .done
.host_changed:
    mov ax,[mouse_x]
    mov [vm_host_x],ax
    mov ax,[mouse_y]
    mov [vm_host_y],ax
    mov ax,[mouse_buttons]
    mov [vm_host_buttons],ax
    xor si,si                        ; event bits
    mov bx,[vm_x]
    mov bp,[vm_y]
    xor di,di                        ; buttons
    mov ax,[mouse_x]
    sub ax,[DW_CLIENT_X]
    jb .mapped
    call vga_client_width
    mov cx,ax
    mov ax,[mouse_x]
    sub ax,[DW_CLIENT_X]
    cmp ax,cx
    jae .mapped
    push cx
    mov ax,[mouse_y]
    sub ax,[DW_CLIENT_Y]
    jb .outside_y
    call vga_client_height
    mov cx,ax
    mov ax,[mouse_y]
    sub ax,[DW_CLIENT_Y]
    cmp ax,cx
    jae .outside_y
    ; y = ymin + dy * (ymax - ymin + 1) / height
    movzx eax,ax
    movzx ecx,cx
    movzx edx,word [vm_ymax]
    sub dx,[vm_ymin]
    inc edx
    imul eax,edx
    xor edx,edx
    div ecx
    add ax,[vm_ymin]
    mov bp,ax
    pop cx
    mov ax,[mouse_x]
    sub ax,[DW_CLIENT_X]
    movzx eax,ax
    movzx ecx,cx
    movzx edx,word [vm_xmax]
    sub dx,[vm_xmin]
    inc edx
    imul eax,edx
    xor edx,edx
    div ecx
    add ax,[vm_xmin]
    mov bx,ax
    cmp byte [vm_text],1
    jne .graphics
    and bx,0FFF8h
    and bp,0FFF8h
.graphics:
    cmp byte [guest_focus],1
    je .focused
    mov bx,[vm_x]                    ; unfocused: the guest's pointer stays put
    mov bp,[vm_y]
    jmp .mapped
.focused:
    mov di,[mouse_buttons]
    and di,7
    jmp .mapped
.outside_y:
    pop cx
.mapped:
    cmp byte [vm_sync],0
    je .motion
    mov byte [vm_sync],0
    mov [vm_x],bx
    mov [vm_y],bp
    mov [vm_buttons],di
    jmp .store
.motion:
    mov ax,bx
    sub ax,[vm_x]
    mov cx,bp
    sub cx,[vm_y]
    mov dx,ax
    or dx,cx
    jz .buttons
    or si,1
    add [vm_mickey_x],ax
    add [vm_mickey_y],cx
    mov [vm_x],bx
    mov [vm_y],bp
.buttons:
    mov ax,di
    xor ax,[vm_buttons]              ; changed buttons
    jz .store
    xor bx,bx                        ; 2 * button index
    xor cx,cx                        ; button index
.button:
    bt ax,cx
    jnc .next_button
    call vga_mouse_edge
.next_button:
    add bx,2
    inc cx
    cmp cx,3
    jb .button
.store:
    mov [vm_buttons],di
    or [vm_events],si
    mov ax,[vm_x]
    mov [DW_GUEST_MOUSE_X],ax
    mov ax,[vm_y]
    mov [DW_GUEST_MOUSE_Y],ax
    mov [DW_GUEST_MOUSE_BUTTONS],di
.done:
    popad
    ret

; BX = 2*button, CX = button, DI = new buttons: count the edge at the current
; position and add press (2<<2i) or release (4<<2i) to the event bits in SI.
vga_mouse_edge:
    push ax
    push dx
    mov ax,[vm_x]
    mov dx,[vm_y]
    bt di,cx
    jnc .up
    inc word [vm_press_count+bx]
    mov [vm_press_x+bx],ax
    mov [vm_press_y+bx],dx
    mov ax,2
    jmp .bit
.up:
    inc word [vm_release_count+bx]
    mov [vm_release_x+bx],ax
    mov [vm_release_y+bx],dx
    mov ax,4
.bit:
    shl ax,cl
    shl ax,cl
    or si,ax
    pop dx
    pop ax
    ret

; From the host service (guest interrupted, DS=CS, private stack): call the
; guest's INT 33h event handler like a mouse driver's IRQ would.
vga_mouse_events:
    pushad
    push es
    cmp byte [vga_session],1
    jne .done
    cmp byte [DW_GUEST_MOUSE],0
    je .done
    call vga_mouse_update
    mov ax,[vm_events]
    and ax,[vm_mask]
    mov word [vm_events],0
    jz .done
    cmp word [vm_handler+2],0
    je .done
    cmp byte [vm_in_handler],0
    jne .done
    mov byte [vm_in_handler],1
    inc dword [DW_MOUSE_EVENTS]
    mov bx,[vm_buttons]
    mov cx,[vm_x]
    mov dx,[vm_y]
    mov si,[vm_mickey_x]
    mov di,[vm_mickey_y]
    push ds
    call far [cs:vm_handler]
    pop ds
    mov byte [cs:vm_in_handler],0
.done:
    pop es
    popad
    ret

; BH = INT 10h/1130h selector; EAX = segment:offset of the firmware font.
vga_font_pointer:
    push es
    push bp
    mov ax,1130h
    int 10h
    mov ax,es
    shl eax,16
    mov ax,bp
    pop bp
    pop es
    ret

; EAX = TSC kHz across two BIOS ticks (109.85 ms); 0 if RDTSC is unusable.
vga_measure_tsc:
    push es
    xor ax,ax
    mov es,ax
    sti
    mov ebx,[es:46Ch]
.edge:
    cmp ebx,[es:46Ch]
    je .edge
    rdtsc
    mov [vga_tsc],eax
    mov [vga_tsc+4],edx
    mov ebx,[es:46Ch]
    add ebx,2
.wait:
    cmp [es:46Ch],ebx
    jb .wait
    rdtsc
    sub eax,[vga_tsc]
    sbb edx,[vga_tsc+4]
    mov ecx,1000
    mul ecx
    mov ecx,109851
    cmp edx,ecx
    jae .none
    div ecx
    pop es
    ret
.none:
    xor eax,eax
    pop es
    ret

; Current VBE mode: granularity, current window A, LFB extent for binding.
vga_query_vbe:
    push cs
    pop es
    mov di,vga_vbe_info
    mov dword [vga_vbe_info],'VBE2'
    mov ax,4F00h
    int 10h
    cmp ax,004Fh
    jne .bad
    mov ax,4F03h
    int 10h
    cmp ax,004Fh
    jne .bad
    and bx,3FFFh
    mov cx,bx
    push cs
    pop es
    mov di,vga_mode_info
    mov ax,4F01h
    int 10h
    cmp ax,004Fh
    jne .bad
    test byte [vga_mode_info],80h
    jz .bad
    mov eax,[vga_mode_info+40]
    test eax,eax
    jz .bad
    mov [vga_binding+VM_FB_PACKET_PHYSICAL],eax
    movzx eax,word [vga_vbe_info+18]  ; total memory in 64 KiB units
    shl eax,16
    cmp eax,VM_FB_MAX_BYTES
    jbe .extent
    mov eax,VM_FB_MAX_BYTES
.extent:
    mov [vga_binding+VM_FB_PACKET_LENGTH],eax
    movzx eax,word [vga_mode_info+4] ; window granularity in KiB
    shl eax,10
    jz .bad
    mov [vga_host_packet+VM_VH_GRANULE],eax
    mov bx,0100h                      ; get window A position
    mov ax,4F05h
    int 10h
    cmp ax,004Fh
    jne .bad
    movzx edx,dx
    mov [vga_host_packet+VM_VH_WINDOW],edx
    clc
    ret
.bad:
    stc
    ret

vga_session equ DW_CLEANUP_PHASE
vga_devices db 0
vga_sched_bound db 0
vga_sent_focus db 0FFh
vm_text db 0
vm_in_handler db 0
vm_sync db 0
align 2
vm_host_x dw 0
vm_host_y dw 0
vm_host_buttons dw 0
align 2
vm_x dw 0
vm_y dw 0
vm_buttons dw 0
vm_xmin dw 0
vm_xmax dw 639
vm_ymin dw 0
vm_ymax dw 199
vm_visible dw 0FFFFh
vm_mask dw 0
vm_handler dd 0
vm_mickey_x dw 0
vm_mickey_y dw 0
vm_events dw 0
vm_page dw 0
vm_sensitivity dw 50,50,50
vm_press_count dw 0,0,0
vm_press_x dw 0,0,0
vm_press_y dw 0,0,0
vm_release_count dw 0,0,0
vm_release_x dw 0,0,0
vm_release_y dw 0,0,0
vm_counts_end:
vga_hooks db 0
vga_end_started db 0
vga_exec_depth db 0
vga_host_entered db 0
vga_cleanup_notice db '[DOSWIN] Cleanup retained; display paused. Enter retries. No memory freed.',13,10,0
align 4
vga_entry dd 0
vga_old_int21 dd 0
vga_tsc dd 0,0
vga_config:
    dd VM_VCFG_MAGIC
    dw VM_ABI_VIDEO_VERSION, VM_VCFG_BYTES
    times VM_VCFG_BYTES-8 db 0
vga_binding:
    dd VM_FB_PACKET_MAGIC
    dw VM_ABI_VERSION, VM_FB_PACKET_SIZE
    dd 0,0
vga_host_packet:
    dd VM_VHOST_MAGIC, 0, 0, 0
align 4
vga_band_packet:
    dd VM_VPRES_MAGIC
    dw VM_ABI_VIDEO_VERSION, VM_VPRES_BYTES
    dw 0,0
    dd 0
    times 16 db 0                     ; format
    dw 0,0,0,0                        ; window (band coordinates)
    dw 0,0                            ; no clip list: band bounds clip
    times 128 db 0
    dd 8                              ; CVP_NO_DAMAGE: compositor-driven
    dd 0                              ; unbounded within one band
    times VM_VPRES_BYTES-180 db 0
vga_state times VM_VSTATE_BYTES db 0
align 4
vga_scheduler:
    dd CVSCHED_MAGIC
    dw CVSCHED_VERSION, CVSCHED_BYTES
    times CVSCHED_BYTES-8 db 0
vga_mode_info times 256 db 0
vga_vbe_info times 512 db 0
