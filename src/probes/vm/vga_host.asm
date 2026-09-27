; VGAHOST.COM: host-side owner for a monitored VGA session (test harness).
; Usage: VGAHOST <P|N>[H] <program> [arguments]
;   P = set a VBE linear-framebuffer mode and arm the bounded presenter
;   N = no presenter (model/trap validation only)
;   H = wait for a key before BEGIN and after END (observer snapshots)
; The child is an ordinary unmodified DOS program; it receives no CiukiOS API.
; An observation block ('VGAHOST1') lets the emulator harness read progress,
; the model's physical page list and final statistics without stopping or
; patching the guest.
bits 16
cpu 686
org 100h
%include "src/vm/session_abi.inc"
%include "src/vm/session_video_abi.inc"

start:
    cli
    mov ax,cs
    mov ss,ax
    mov sp,stack_top
    sti
    mov ds,ax
    mov es,ax
    cld
    mov bx,(program_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    jc fail_early
    call parse_tail
    jc fail_early
    xor di,di
    mov es,di
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    int 2Fh
    mov ax,es
    or ax,di
    jz fail_early
    mov [entry],di
    mov [entry+2],es
    push cs
    pop es
    xor cx,cx
    mov ax,VM_OP_QUERY
    call far [entry]
    jc fail_early
    cmp ax,VM_ABI_VERSION
    jne fail_early
    and bx,VM_VIDEO_CAPABILITIES
    cmp bx,VM_VIDEO_CAPABILITIES
    jne fail_early
    mov byte [step],1
    call measure_tsc
    mov [config+VM_VCFG_TSC_KHZ],eax
    mov [obs_tsc],eax
    mov bh,3
    call font_pointer
    mov [config+VM_VCFG_FONT_8X8],eax
    mov bh,4
    call font_pointer
    mov [config+VM_VCFG_FONT_8X8_HIGH],eax
    mov bh,2
    call font_pointer
    mov [config+VM_VCFG_FONT_8X14],eax
    mov bh,6
    call font_pointer
    mov [config+VM_VCFG_FONT_8X16],eax
    push cs
    pop es
    mov di,config
    mov ax,VM_OP_VIDEO_CONFIG
    call far [entry]
    jc fail_early
    mov byte [step],2
    cmp byte [present],0
    je .begin
    call setup_framebuffer
    jc fail_early
.begin:
    mov byte [step],3
    mov dword [obs_state],10h
    call hold_point
    mov ax,VM_OP_BEGIN
    call far [entry]
    jc fail_session
    mov byte [active],1
    ; A failed early UNBIND must retain the real LFB mapping. The following
    ; presenter frames and final cleanup exercise that same binding.
    cmp byte [fb_is_bound],0
    je .binding_checked
    mov ax,VM_OP_UNBIND_FB
    call far [entry]
    jnc fail_session
    cmp ax,VM_ERROR_ACTIVE
    jne fail_session
.binding_checked:
    mov byte [step],6
    push cs
    pop es
    mov di,obs_share
    mov ax,VM_OP_VIDEO_SHARE
    call far [entry]
    jc fail_session
    mov byte [shared],1
    mov byte [step],7
    cmp byte [present],0
    je .run
    push cs
    pop es
    mov di,packet
    mov ax,VM_OP_VIDEO_ARM
    call far [entry]
    jc fail_session
.run:
    mov byte [step],4
    mov dword [obs_state],1
    call run_child
    mov [obs_exit],ax
    mov dword [obs_state],2
    mov byte [step],5
    cmp ah,0F0h                      ; EXEC itself failed (AL = DOS error)
    jne .child_ran
    mov [last_error],ax
    call finish
    jmp fail_final
.child_ran:
    call finish
    jc fail_final
    mov dword [obs_state],11h
    call hold_point
    mov dword [obs_state],3
    call print_summary
    mov ax,4C00h
    int 21h

fail_session:
    mov [cs:last_error],ax
    call finish
fail_early:
fail_final:
    push cs
    pop ds
    mov dword [obs_state],0FFh
    mov dx,fail_text
    mov ah,9
    int 21h
    mov al,[step]
    call print_hex8
    mov dx,s_error
    mov ah,9
    int 21h
    mov eax,[last_error]
    call print_hex32
    call newline
    mov ax,4C01h
    int 21h

; Stop presentation, collect statistics, release sharing, END, unbind and
; hand the physical display back to text mode. CF=1 if cleanup failed.
finish:
    push cs
    pop ds
    push cs
    pop es
    cmp byte [active],0
    je .unbind
    mov ax,VM_OP_VIDEO_DISARM
    call far [entry]
    mov di,obs_video
    mov ax,VM_OP_VIDEO_STATE
    call far [entry]
    ; The guest's final text page: characters at even plane-0 addresses,
    ; attributes at the same plane-1 addresses (odd/even text layout).
    mov di,obs_text
    xor edx,edx
    mov cx,4000
    mov ax,VM_OP_VIDEO_READ
    call far [entry]
    mov di,obs_attributes
    mov edx,65536
    mov cx,4000
    mov ax,VM_OP_VIDEO_READ
    call far [entry]
    cmp byte [shared],0
    je .end
    mov ax,VM_OP_VIDEO_UNSHARE
    call far [entry]
    jc .failed
    mov byte [shared],0
.end:
    mov ax,VM_OP_END
    call far [entry]
    jc .failed
    mov byte [active],0
.unbind:
    cmp byte [fb_is_bound],0
    je .text
    mov ax,VM_OP_UNBIND_FB
    call far [entry]
    jc .failed
    mov byte [fb_is_bound],0
.text:
    cmp byte [present],0
    je .done
    mov ax,0003h
    int 10h
.done:
    clc
    ret
.failed:
    stc
    ret

; Parse "<P|N> <program> [tail]".
parse_tail:
    mov si,81h
    call skip_spaces
    lodsb
    and al,0DFh
    cmp al,'P'
    je .present
    cmp al,'N'
    jne .bad
    jmp .flag_done
.present:
    mov byte [present],1
.flag_done:
    mov al,[si]
    and al,0DFh
    cmp al,'H'                       ; optional: hold before BEGIN / after END
    jne .no_hold
    mov byte [hold],1
    inc si
.no_hold:
    call skip_spaces
    mov di,program
.name:
    lodsb
    cmp al,13
    je .name_end
    cmp al,' '
    je .name_end
    stosb
    cmp di,program+79
    jb .name
    jmp .bad
.name_end:
    mov byte [di],0
    cmp di,program
    je .bad
    dec si
    mov di,child_tail+1
    xor cx,cx
.tail:
    lodsb
    cmp al,13
    je .tail_end
    stosb
    inc cx
    cmp cx,120
    jb .tail
.tail_end:
    mov byte [di],13
    mov [child_tail],cl
    clc
    ret
.bad:
    stc
    ret

; With H, wait for a key so an observer can snapshot owned state.
hold_point:
    cmp byte [hold],0
    je .done
    xor ah,ah
    int 16h
.done:
    ret

skip_spaces:
    cmp byte [si],' '
    jne .done
    inc si
    jmp skip_spaces
.done:
    ret

; EAX = TSC kHz measured across nine BIOS ticks (54.9254 ms each).
measure_tsc:
    push es
    xor ax,ax
    mov es,ax
    sti
    mov ebx,[es:46Ch]
.edge:
    cmp ebx,[es:46Ch]
    je .edge
    rdtsc
    mov [tsc_start],eax
    mov [tsc_start+4],edx
    mov ebx,[es:46Ch]
    add ebx,9
.wait:
    cmp [es:46Ch],ebx
    jb .wait
    rdtsc
    sub eax,[tsc_start]
    sbb edx,[tsc_start+4]
    mov ecx,1000
    mul ecx
    mov ecx,494329                   ; 9 ticks in microseconds
    div ecx
    pop es
    ret

; BH = INT 10h/1130h selector; returns EAX = segment:offset of the font.
font_pointer:
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

; Select the deepest 800x600 direct-colour VBE mode with a linear frame-
; buffer from the controller's list (32 > 24 > 16 bpp), bind it to CVSESSION
; and build the presenter packet (640x480 window centred).
setup_framebuffer:
    push cs
    pop es
    mov di,vbe_info
    mov dword [vbe_info],'VBE2'
    mov ax,4F00h
    int 10h
    cmp ax,004Fh
    jne .bad
    lfs si,[vbe_info+14]
.next_mode:
    mov cx,[fs:si]
    add si,2
    cmp cx,0FFFFh
    je .chosen
    push si
    push cx
    mov di,mode_info
    mov ax,4F01h
    int 10h
    pop cx
    pop si
    cmp ax,004Fh
    jne .next_mode
    mov ax,[mode_info]
    and ax,0081h                     ; supported + linear framebuffer
    cmp ax,0081h
    jne .next_mode
    cmp word [mode_info+18],800
    jne .next_mode
    cmp word [mode_info+20],600
    jne .next_mode
    cmp byte [mode_info+27],6        ; direct colour memory model
    jne .next_mode
    mov al,[mode_info+25]
    cmp al,[best_bpp]
    jbe .next_mode
    cmp al,32
    ja .next_mode
    mov [best_bpp],al
    mov [best_mode],cx
    jmp .next_mode
.chosen:
    cmp word [best_mode],0
    je .bad
    mov cx,[best_mode]
    mov di,mode_info
    mov ax,4F01h
    int 10h
    cmp ax,004Fh
    jne .bad
    test word [mode_info],80h
    jz .bad
    cmp word [mode_info+18],800
    jne .bad
    cmp word [mode_info+20],600
    jne .bad
    mov al,[mode_info+25]
    add al,7
    shr al,3
    cmp al,2
    jb .bad
    cmp al,4
    ja .bad
    mov [packet+VM_VP_FORMAT+8],al
    mov eax,[mode_info+40]
    mov [binding+VM_FB_PACKET_PHYSICAL],eax
    mov [obs_fb_physical],eax
    movzx eax,word [mode_info+16]
    mov [packet+VM_VP_FORMAT],eax
    mov [obs_pitch],eax
    imul eax,600
    mov [binding+VM_FB_PACKET_LENGTH],eax
    mov word [packet+VM_VP_FORMAT+4],800
    mov word [packet+VM_VP_FORMAT+6],600
    mov si,mode_info+31
    mov di,packet+VM_VP_FORMAT+9
    mov cx,6
    rep movsb
    mov bx,[best_mode]
    or bh,40h                        ; linear framebuffer
    mov ax,4F02h
    int 10h
    cmp ax,004Fh
    jne .bad
    mov di,binding
    mov cx,VM_FB_PACKET_SIZE
    mov ax,VM_OP_BIND_FB
    call far [entry]
    jc .bad
    mov byte [fb_is_bound],1
    clc
    ret
.bad:
    stc
    ret

; Change to the program's directory (data files), EXEC it, restore the cwd.
; Returns AX = DOS return code (AH=type, AL=code) or F0xxh if EXEC failed.
run_child:
    mov si,saved_dir+1
    mov byte [saved_dir],'\'
    xor dl,dl
    mov ah,47h
    int 21h
    mov si,program
    xor bx,bx
.scan:
    lodsb
    test al,al
    jz .scanned
    cmp al,'\'
    jne .scan
    mov bx,si
    jmp .scan
.scanned:
    test bx,bx
    jz .exec
    dec bx
    cmp bx,program
    je .exec
    mov byte [bx],0
    mov dx,program
    mov ah,3Bh
    int 21h
    mov byte [bx],'\'
.exec:
    mov [parameters+4],cs
    mov [parameters+8],cs
    mov [parameters+12],cs
    mov [save_sp],sp
    mov [save_ss],ss
    push cs
    pop es
    mov dx,program
    mov bx,parameters
    mov ax,4B00h
    int 21h
    cli
    mov ss,[cs:save_ss]
    mov sp,[cs:save_sp]
    sti
    push cs
    pop ds
    push cs
    pop es
    jnc .exit_code
    mov ah,0F0h                      ; AX = F0xxh: EXEC error xx
    jmp .restore
.exit_code:
    mov ah,4Dh
    int 21h
.restore:
    push ax
    mov dx,saved_dir
    mov ah,3Bh
    int 21h
    pop ax
    ret

print_summary:
    mov dx,pass_text
    mov ah,9
    int 21h
    mov si,summary_fields
.field:
    lodsw
    test ax,ax
    jz .done
    mov dx,ax
    mov ah,9
    int 21h
    lodsw
    mov bx,ax
    mov eax,[bx]
    call print_hex32
    jmp .field
.done:
    jmp newline

print_hex32:
    push cx
    mov cx,8
.digit:
    rol eax,4
    push eax
    and al,15
    add al,'0'
    cmp al,'9'
    jbe .out
    add al,7
.out:
    mov dl,al
    mov ah,2
    int 21h
    pop eax
    loop .digit
    pop cx
    ret

print_hex8:
    shl eax,24
    push cx
    mov cx,2
    jmp print_hex32.digit

newline:
    mov dx,crlf
    mov ah,9
    int 21h
    ret

entry dd 0
last_error dd 0
step db 0
present db 0
hold db 0
active db 0
shared db 0
fb_is_bound db 0
save_sp dw 0
save_ss dw 0
tsc_start dd 0,0
parameters dw 0,child_tail,0,5Ch,0,6Ch,0
child_tail times 128 db 0
program times 80 db 0
saved_dir times 68 db 0
config:
    dd VM_VCFG_MAGIC
    dw VM_ABI_VIDEO_VERSION, VM_VCFG_BYTES
    times VM_VCFG_BYTES-8 db 0
binding:
    dd VM_FB_PACKET_MAGIC
    dw VM_ABI_VERSION, VM_FB_PACKET_SIZE
    dd 0,0
align 4
packet:
    dd VM_VPRES_MAGIC
    dw VM_ABI_VIDEO_VERSION, VM_VPRES_BYTES
    dw 0,0
    dd 0
    times 16 db 0                    ; format, filled from VBE mode info
    dw 80,60,720,540                 ; window: 640x480 centred in 800x600
    dw 0,0                           ; no clips: whole window visible
    times 128 db 0
    dd 0                             ; flags
    dd 64000                         ; pixel budget per tick
    times 32 db 0                    ; stats (written back)
    dd 14000                         ; at most ~70 presents per second
    times VM_VPRES_BYTES-216 db 0
mode_info times 256 db 0
vbe_info times 512 db 0
best_mode dw 0
best_bpp db 0
pass_text db '[VGAHOST] PASS$'
fail_text db '[VGAHOST] FAIL step=$'
crlf db 13,10,'$'
s_error db ' error=$'
s_exit db ' exit=$'
s_faults db ' faults=$'
s_instr db ' instructions=$'
s_elements db ' elements=$'
s_unsup db ' unsupported=$'
s_bios db ' bios=$'
s_bios_unsup db ' bios_unsupported=$'
s_last db ' bios_last=$'
s_modes db ' mode_sets=$'
s_mode db ' mode=$'
s_fatal db ' fatal=$'
s_presents db ' presents=$'
s_frames db ' frames=$'
s_ports db ' port_writes=$'
s_tsc db ' tsc_khz=$'
summary_fields:
    dw s_exit, obs_exit
    dw s_faults, obs_video+VM_VS_FAULTS
    dw s_instr, obs_video+VM_VS_INSTRUCTIONS
    dw s_elements, obs_video+VM_VS_ELEMENTS
    dw s_unsup, obs_video+VM_VS_UNSUPPORTED
    dw s_bios, obs_video+VM_VS_BIOS_CALLS
    dw s_bios_unsup, obs_video+VM_VS_BIOS_UNSUPPORTED
    dw s_last, obs_video+VM_VS_BIOS_LAST_AX
    dw s_modes, obs_video+VM_VS_MODE_SETS
    dw s_mode, obs_video+VM_VS_MODE
    dw s_fatal, obs_video+VM_VS_FATAL
    dw s_presents, obs_video+VM_VS_PRESENTS
    dw s_frames, obs_video+VM_VS_FRAMES
    dw s_ports, obs_video+VM_VS_PORT_WRITES
    dw s_tsc, obs_tsc
    dw 0
align 16
observation db 'VGAHOST1'
obs_state dd 0
obs_exit dd 0
obs_tsc dd 0
obs_fb_physical dd 0
obs_pitch dd 0
obs_packet dw packet, observation
obs_share times VM_VSHARE_BYTES db 0
obs_video times VM_VSTATE_BYTES db 0
obs_text times 4000 db 0
obs_attributes times 4000 db 0
align 16
stack_space times 2048 db 0
stack_top:
program_end:
