; DPMIRUN.COM - run a DPMI (DOS/4GW, DOS/32A, ...) program inside the
; CiukiOS DOS window.
;
;   DPMIRUN program [arguments]
;   DPMIRUN /W [/C command]      (the DOS window's command interpreter)
;   DPMIRUN /V [/Tkhz] [/C command]  (a DOS window that is its own VM)
;
; Window mode is how every desktop DOS window starts (Windows 95 style): the
; window's session gets its DPMI host for its whole life, then \COMMAND.COM
; runs with the same tail. Any DOS/4GW (or other DPMI) program started in the
; window finds the host, with the window's video and devices. Without a VM
; session (safe boot) only \COMMAND.COM runs.
;
; The window's CVSESSION session owns one scheduler descriptor, followed by
; the video share packet. The patched HDPMI 3.24 host must be bound to that
; descriptor (-cSSSS:OOOO) so protected-mode video reaches the shared model
; and protected-mode device ports reach the session's guest devices. This
; launcher asks CVSESSION for the descriptor, loads the host resident, runs
; the program from its own directory, then returns the session's device IRQs
; (held for the protected-mode client while it ran) to V86 and unloads the
; host so the window can end its session. Messages are English; errors
; return ERRORLEVEL 1.
;
; VM mode (/V) runs in a virtual machine the desktop forked for one DOS
; window (\VM\VMFORK.COM \VM\DPMIRUN.COM /V ...). There the window's
; session is this VM's own: DPMIRUN begins it (virtual VGA in text mode 03h,
; keyboard, mouse, Sound Blaster 16 and OPL3 on the AC'97), binds the DPMI
; host to it with VCPI memory (-v: Jemm's pool is one for all VMs, while each
; VM has its own copy of the kernel's XMS tables), runs \COMMAND.COM with the
; tail, and ends the session. The desktop paints the session into the window
; from the system VM. /T gives the TSC rate in kHz the desktop measured
; (measured here otherwise). While the program runs, the VM's IRQ0 handler
; hands timer ticks its DPMI host reflected to the VM manager (VMM_YIELD):
; a host in protected mode owns the IDT, so the desktop would not get the
; processor otherwise.
bits 16
cpu 386
org 100h

%include "src/vm/session_abi.inc"
%include "src/vm/session_devices_abi.inc"
%include "src/vm/session_video_abi.inc"
%include "src/vm/session_scheduler_abi.inc"

start:
    cld
    mov sp,stack_top
    mov bx,(image_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    mov si,no_memory
    jc fail
    ; Program path and its arguments from the command tail.
    mov si,81h
    call skip_blanks
    cmp al,'/'
    je window_mode
    ; A host is already resident (a DOS window): just run the program.
    mov ax,1687h
    int 2Fh
    push cs                         ; 1687h returns the host entry in ES:DI
    pop es
    test ax,ax
    jnz .no_host
    mov byte [have_host],1
.no_host:
    mov si,81h
    call skip_blanks
    cmp al,13
    jne .have_program
    mov si,usage
    jmp fail
.have_program:
    mov di,program
    mov cx,127
.copy_path:
    lodsb
    cmp al,' '
    je .path_done
    cmp al,9
    je .path_done
    cmp al,13
    je .path_back
    stosb
    loop .copy_path
.path_back:
    dec si
.path_done:
    mov byte [di],0
    call skip_blanks
    ; Child tail: " arguments" CR.
    mov di,child_tail+1
    xor cx,cx
    cmp al,13
    je .tail_done
    mov al,' '
    stosb
    inc cx
.copy_tail:
    lodsb
    cmp al,13
    je .tail_done
    stosb
    inc cx
    cmp cx,125
    jb .copy_tail
.tail_done:
    mov byte [di],13
    mov [child_tail],cl

    ; CVSESSION entry and the window's descriptor.
    call find_session
    mov si,no_session
    jc fail

    ; Resident protected-mode host bound to the window session.
    cmp byte [have_host],1
    je .host_ready
    call load_host
    mov si,host_failed
    jc fail
.host_ready:

    ; Run the program from its own directory (data files, DOS4GW.EXE).
    call save_directory
    call enter_program_dir
    mov si,log_run
    call log
    mov si,[exec_name]
    call log
    mov si,log_in
    call log
    mov si,directory
    call log
    mov si,log_crlf
    call log
    mov word [params+2],child_tail
    mov dx,[exec_name]
    call exec
    mov byte [exit_code],1
    jnc .ran
    mov di,exec_error_code
    call hex4
    mov si,exec_failed
    call print
    jmp .restore
.ran:
    mov ah,4Dh
    int 21h
    mov [exit_code],al
    mov di,log_code
    call hex4
    mov si,log_exit
    call log
.restore:
    call restore_directory

    cmp byte [have_host],1
    je .done
    call unload_host
.done:
    mov al,[exit_code]
    mov ah,4Ch
    int 21h

; The DOS window: host for the window's life, then \COMMAND.COM /W ... .
window_mode:
    mov al,[si+1]
    or al,20h
    cmp al,'v'
    je vm_mode
    mov di,child_tail
    mov si,80h
    movzx cx,byte [si]
    inc cx
    inc cx
    rep movsb                       ; count, text, CR
    call find_session
    jc .run
    call load_host
    jc .run
    mov byte [window_host],1
.run:
    ; A program started in the window runs in its own directory (data files,
    ; DOS4GW.EXE), as Windows does for a program's working directory.
    call command_program
    jc .exec
    call save_directory
    call enter_program_dir
    mov byte [moved],1
.exec:
    mov word [params+2],child_tail
    mov dx,command_path
    call exec
    mov byte [exit_code],1
    jc .end
    mov ah,4Dh
    int 21h
    mov [exit_code],al
.end:
    cmp byte [moved],1
    jne .unload
    call restore_directory
.unload:
    cmp byte [window_host],1
    jne .exit
    call unload_host
.exit:
    mov al,[exit_code]
    mov ah,4Ch
    int 21h

; ---- VM mode: a DOS window that is its own VM ----
vm_mode:
    add si,2
    call skip_blanks
    cmp al,'/'
    jne .tail
    mov al,[si+1]
    or al,20h
    cmp al,'t'
    jne .tail
    add si,2
    xor ebx,ebx
.digit:
    lodsb
    sub al,'0'
    cmp al,9
    ja .digits_done
    imul ebx,ebx,10
    movzx eax,al
    add ebx,eax
    jmp .digit
.digits_done:
    dec si
    mov [vm_tsc],ebx
    call skip_blanks
.tail:
    ; COMMAND.COM tail: " " + the rest, CR.
    mov di,child_tail+1
    xor cx,cx
    cmp byte [si],13
    je .tail_done
    mov al,' '
    stosb
    inc cx
.copy:
    lodsb
    cmp al,13
    je .tail_done
    stosb
    inc cx
    cmp cx,125
    jb .copy
.tail_done:
    mov byte [di],13
    mov [child_tail],cl
    call vm_begin
    jnc .session
    mov si,vm_no_session
    call print
    mov byte [exit_code],1
    jmp .exit
.session:
    call find_session
    jc .run
    mov byte [host_vcpi],1
    call load_host
    jc .run
    mov byte [window_host],1
.run:
    call command_program
    jc .exec
    call save_directory
    call enter_program_dir
    mov byte [moved],1
.exec:
    mov word [params+2],child_tail
    mov dx,command_path
    call exec
    mov byte [exit_code],1
    jc .end
    mov ah,4Dh
    int 21h
    mov [exit_code],al
.end:
    cmp byte [moved],1
    jne .unload
    call restore_directory
.unload:
    cmp byte [window_host],1
    jne .ended
    call unload_host
.ended:
    call vm_end
.exit:
    mov si,vm_log_end
    call log
    mov al,[exit_code]
    mov ah,4Ch
    int 21h

; Begin this VM's DOS window session. CF=1 when there is none.
vm_begin:
    mov byte [vm_begin_step],'I'
    xor di,di
    mov es,di
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    int 2Fh
    mov [entry],di
    mov [entry+2],es
    mov ax,es
    or ax,di
    push cs
    pop es
    stc
    jz .done
    movzx eax,word [entry+2]
    shl eax,4
    movzx edx,word [entry]
    add eax,edx
    mov [vm_entry_linear],eax
    xor cx,cx
    mov byte [vm_begin_step],'Q'
    mov ax,VM_OP_QUERY
    call vm_call
    jc .done
    and bx,VM_VIDEO_CAPABILITIES
    cmp bx,VM_VIDEO_CAPABILITIES
    stc
    jne .done
    ; Firmware fonts before the session serves INT 10h.
    mov bh,3
    call vm_font
    mov [vm_config+VM_VCFG_FONT_8X8],eax
    mov bh,4
    call vm_font
    mov [vm_config+VM_VCFG_FONT_8X8_HIGH],eax
    mov bh,2
    call vm_font
    mov [vm_config+VM_VCFG_FONT_8X14],eax
    mov bh,6
    call vm_font
    mov [vm_config+VM_VCFG_FONT_8X16],eax
    mov eax,[vm_tsc]
    test eax,eax
    jnz .tsc
    call vm_measure_tsc
    mov [vm_tsc],eax
.tsc:
    mov [vm_config+VM_VCFG_TSC_KHZ],eax
    push eax
    push cs
    pop es
    shr eax,16
    mov di,vm_tsc_hi
    call hex4
    mov ax,[vm_tsc]
    mov di,vm_tsc_lo
    call hex4
    mov si,vm_tsc_log
    call log
    pop eax
    mov di,vm_config
    mov byte [vm_begin_step],'C'
    mov ax,VM_OP_VIDEO_CONFIG
    call vm_call
    jc .done
    mov di,vm_scheduler
    mov cx,CVSCHED_BYTES
    mov ax,VM_OP_BIND_SCHED
    call vm_call
    jc .no_scheduler
    mov byte [vm_sched_bound],1
.no_scheduler:
    mov byte [vm_begin_step],'B'
    mov ax,VM_OP_BEGIN
    call vm_call
    jnc .begun
    call vm_unbind
    stc
    jmp .done
.begun:
    mov byte [vm_active],1
    ; Guest devices when Jemm's virtual-IF profile is there.
    xor cx,cx
    mov ax,VM_OP_QUERY
    call vm_call
    test bx,VM_CAP_GUEST_INPUT
    jz .no_devices
    mov ebx,CVDEV_CAP_KEYBOARD|CVDEV_CAP_MOUSE|CVDEV_CAP_DMA_SB|CVDEV_CAP_OPL3
    mov ecx,CVDEV_FLAG_AUDIO_OUT
    mov edx,[vm_tsc]
    mov ax,VM_OP_DEV_BEGIN
    call vm_call
    jc .dev_failed
    mov byte [vm_devices],1
    push ax
    push cs
    pop es
    mov ax,cx
    mov di,vm_audio_code
    call hex4
    mov si,vm_audio_log
    call log
    pop ax
    jmp .no_devices
.dev_failed:
    push ax
    push cs
    pop es
    mov di,vm_dev_fail_code
    call hex4
    mov si,vm_dev_fail_log
    call log
    pop ax
.no_devices:
    ; A protected-mode host maps the model through the packet after the
    ; scheduler descriptor (HDPMI -cSSSS:OOOO).
    cmp byte [vm_sched_bound],1
    jne .no_share
    mov di,vm_share
    mov ax,VM_OP_VIDEO_SHARE
    call vm_call
    jc .no_share
    mov byte [vm_shared],1
.no_share:
    mov ax,0003h                        ; the virtual VGA BIOS: text 80x25
    int 10h
    push cs
    pop ds
    push cs
    pop es
    ; Timer ticks the DPMI host reflects reach the VM manager.
    push es
    xor ax,ax
    mov es,ax
    cli
    mov eax,[es:08h*4]
    mov [vm_old08],eax
    mov word [es:08h*4],vm_irq0
    mov [es:08h*4+2],cs
    sti
    pop es
    mov byte [vm_hooked],1
    mov si,vm_log_session
    call log
    clc
.done:
    pushf
    jnc .retflags
    push ax
    push cs
    pop es
    mov di,vm_begin_code
    call hex4
    mov si,vm_begin_fail
    call log
    pop ax
.retflags:
    popf
    ret

; End the session (the reverse of vm_begin). A refused step is left to
; VMM_EXIT, which ends a session its VM still owns.
vm_end:
    cmp byte [vm_hooked],0
    je .unhooked
    push es
    xor ax,ax
    mov es,ax
    cli
    mov ax,cs
    cmp [es:08h*4+2],ax
    jne .restored                       ; hooked after us: leave it
    mov eax,[vm_old08]
    mov [es:08h*4],eax
.restored:
    sti
    pop es
    mov byte [vm_hooked],0
.unhooked:
    cmp byte [vm_devices],0
    je .devices_done
    mov ax,VM_OP_DEV_END
    call vm_call
.devices_done:
    cmp byte [vm_shared],0
    je .shared_done
    mov ax,VM_OP_VIDEO_UNSHARE
    call vm_call
.shared_done:
    cmp byte [vm_active],0
    je vm_unbind
    mov ax,VM_OP_END
    call vm_call
vm_unbind:
    cmp byte [vm_sched_bound],0
    je .done
    mov ax,VM_OP_UNBIND_SCHED
    call vm_call
.done:
    ret

; CVSESSION call with ES = DS = CS kept.
vm_call:
    push es
    call far [entry]
    pop es
    push cs
    pop ds
    ret

; BH = INT 10h/1130h font: EAX = its far pointer.
vm_font:
    push es
    push bp
    mov ax,1130h
    int 10h
    mov ax,es
    shl eax,16
    mov ax,bp
    pop bp
    pop es
    push cs
    pop ds
    ret

; EAX = TSC kHz from a firmware wait, with BIOS-tick fallback (0 if neither
; measurement is plausible). Virtual BIOS ticks can arrive in bursts in a VM.
vm_measure_tsc:
    ; CVSESSION calibrated the TSC in ring 0 against the PIT when it loaded.
    ; This VM's own wait is unreliable: other VMs run while it halts.
    mov ax,VM_OP_TSC_KHZ
    xor ecx,ecx
    call far [entry]
    push cs
    pop ds
    jc .own
    cmp ecx,1000
    jb .own
    cmp ecx,20000000
    ja .own
    mov eax,ecx
    ret
.own:
    push es
    ; INT 15h/86h waits against the firmware timer. Virtual BIOS ticks can
    ; arrive in bursts while this VM is scheduled, overstating the TSC rate.
    sti
    db 0Fh,31h
    mov [vm_scratch],eax
    mov [vm_scratch+4],edx
    mov ax,8600h
    xor cx,cx
    mov dx,20000
    int 15h
    push cs
    pop ds
    jc .ticks
    db 0Fh,31h
    sub eax,[vm_scratch]
    sbb edx,[vm_scratch+4]
    test edx,edx
    jnz .ticks
    xor edx,edx
    mov ecx,20
    div ecx
    cmp eax,1000
    jb .ticks
    cmp eax,10000000
    ja .ticks
    pop es
    ret
.ticks:
    xor ax,ax
    mov es,ax
    sti
    mov ebx,[es:46Ch]
.edge:
    cmp ebx,[es:46Ch]
    je .edge
    db 0Fh,31h                          ; RDTSC
    mov [vm_scratch],eax
    mov [vm_scratch+4],edx
    mov ebx,[es:46Ch]
    add ebx,2
.wait:
    cmp [es:46Ch],ebx
    jb .wait
    db 0Fh,31h                          ; RDTSC
    sub eax,[vm_scratch]
    sbb edx,[vm_scratch+4]
    mov ecx,1000
    mul ecx
    mov ecx,109851
    cmp edx,ecx
    jae .none
    div ecx
    cmp eax,1000
    jb .none
    cmp eax,10000000
    ja .none
    pop es
    ret
.none:
    xor eax,eax
    pop es
    ret

; IRQ0 of this VM: the BIOS tick (and its EOI) first, then the tick goes to
; the VM manager. A tick run by the monitor as a nested execution (the VM
; idled in HLT) returns into the monitor's breakpoint page: not yielded.
vm_irq0:
    pushf
    call far [cs:vm_old08]
    push eax
    push ebx
    push bp
    mov bp,sp
    movzx eax,word [ss:bp+12]           ; return CS
    shl eax,4
    movzx ebx,word [ss:bp+10]           ; return IP
    add eax,ebx
    sub eax,[cs:vm_entry_linear]
    jns .distance
    neg eax
.distance:
    cmp eax,1000h
    jb .nested
    mov ax,VM_OP_VMM_YIELD
    call far [cs:entry]
.nested:
    pop bp
    pop ebx
    pop eax
    iret

; The program named by "/C [RUN] path ..." in the window's tail -> [program].
; CF=1 when there is none or it has no directory part.
command_program:
    mov si,child_tail+1
.find_c:
    lodsb
    cmp al,13
    je .none
    cmp al,'/'
    jne .find_c
    lodsb
    or al,20h
    cmp al,'c'
    jne .find_c
    call skip_blanks
    mov ax,[si]
    or ax,2020h
    cmp ax,'ru'
    jne .path
    mov al,[si+2]
    or al,20h
    cmp al,'n'
    jne .path
    cmp byte [si+3],' '
    jne .path
    add si,3
    call skip_blanks
.path:
    mov di,program
    mov cx,127
    xor bx,bx
.copy:
    lodsb
    cmp al,' '
    jbe .copied
    cmp al,'\'
    jne .store
    inc bx
.store:
    stosb
    loop .copy
.copied:
    mov byte [di],0
    test bx,bx
    jz .none
    ; Absolute paths only: a relative one is resolved by COMMAND from here.
    cmp byte [program],'\'
    je .absolute
    cmp byte [program+1],':'
    jne .none
    cmp byte [program+2],'\'
    jne .none
.absolute:
    clc
    ret
.none:
    stc
    ret

save_directory:
    mov ah,19h
    int 21h
    mov [saved_drive],al
    mov byte [saved_dir],'\'
    mov si,saved_dir+1
    xor dl,dl
    mov ah,47h
    int 21h
    ret

restore_directory:
    mov dl,[saved_drive]
    mov ah,0Eh
    int 21h
    mov dx,saved_dir
    mov ah,3Bh
    int 21h
    ret

; CVSESSION entry and the window's descriptor into the host tail. CF=1: none.
find_session:
    xor di,di
    mov es,di
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    int 2Fh
    mov [entry],di
    mov [entry+2],es
    mov ax,es
    or ax,di
    push cs
    pop es
    stc
    jz .done
    mov ax,VM_OP_SCHED_INFO
    call far [entry]
    push cs
    pop ds
    push cs
    pop es
    jc .done
    mov ax,bx
    mov di,host_seg
    call hex4
    mov ax,dx
    mov di,host_off
    call hex4
    clc
.done:
    ret

; Load the host resident, bound to the session. CF=1 on failure.
load_host:
    mov si,log_host
    call log
    cmp byte [host_vcpi],0
    je .tail_ready
    mov dword [host_tail_end],0D762D20h ; " -v", CR: VCPI memory
    mov byte [host_tail],host_tail_end-host_tail-1+3
.tail_ready:
    mov word [params+2],host_tail
    mov dx,host_path
    call exec
    jc .done
    mov ah,4Dh
    int 21h
    cmp al,3                        ; 0-2 installed; 3 = an unbound host exists
    cmc
.done:
    ret

; Device IRQs back to V86, then unload the host so the session can end.
unload_host:
    mov ax,VM_OP_DEV_PM_IRQ
    xor cx,cx
    call far [entry]
    push cs
    pop ds
    push cs
    pop es
    mov word [params+2],unload_tail
    mov dx,host_path
    call exec
    jc .failed
    ; The host is gone: release its binding so the window's session can end.
    mov ax,VM_OP_DPMI_RELEASE
    call far [entry]
    push cs
    pop ds
    push cs
    pop es
    jnc .done
.failed:
    mov si,unload_failed
    call print
.done:
    ret

fail:
    call print
    mov ax,4C01h
    int 21h

; DS:SI -> blanks; returns AL = first other byte, SI at it.
skip_blanks:
    lodsb
    cmp al,' '
    je skip_blanks
    cmp al,9
    je skip_blanks
    dec si
    ret

; AX -> four uppercase hex digits at ES:DI.
hex4:
    mov cx,4
.digit:
    rol ax,4
    push ax
    and al,0Fh
    add al,'0'
    cmp al,'9'
    jbe .store
    add al,7
.store:
    stosb
    pop ax
    loop .digit
    ret

; Change drive/directory to the one named in [program], if any.
enter_program_dir:
    mov si,program
    cmp byte [si+1],':'
    jne .directory
    mov dl,[si]
    or dl,20h
    sub dl,'a'
    mov ah,0Eh
    int 21h
.directory:
    mov di,program
    xor bx,bx
.scan:
    mov al,[di]
    test al,al
    jz .found
    cmp al,'\'
    jne .next
    mov bx,di
.next:
    inc di
    jmp .scan
.found:
    mov word [exec_name],program
    test bx,bx
    jz .done
    ; A relative path is run by its file name once inside its directory.
    mov si,program
    cmp byte [si],'\'
    je .absolute
    cmp byte [si+1],':'
    jne .relative
    cmp byte [si+2],'\'
    je .absolute
.relative:
    lea ax,[bx+1]
    mov [exec_name],ax
.absolute:
    mov si,program
    mov di,directory
    cmp byte [si+1],':'
    jne .copy
    add si,2
.copy:
    cmp si,bx
    jae .terminate
    movsb
    jmp .copy
.terminate:
    cmp di,directory
    jne .end
    mov byte [di],'\'
    inc di
.end:
    mov byte [di],0
    mov dx,directory
    mov ah,3Bh
    int 21h
.done:
    ret

; DS:DX program, params -> tail. CF from EXEC.
exec:
    mov [params+4],cs
    mov [params+8],cs
    mov [params+12],cs
    mov bx,params
    mov ax,4B00h
    mov [saved_sp],sp
    int 21h
    cli
    mov bx,cs
    mov ss,bx
    mov sp,[cs:saved_sp]
    sti
    push cs
    pop ds
    push cs
    pop es
    cld
    ret

; Messages also go to COM1 (bounded polling) so a session log records them.
print:
    lodsb
    test al,al
    jz .done
    call serial
    mov dl,al
    mov ah,2
    int 21h
    jmp print
.done:
    ret

log:
    lodsb
    test al,al
    jz print.done
    call serial
    jmp log

serial:
    push ax
    push cx
    push dx
    mov ah,al
    mov cx,4000
    mov dx,3FDh
.wait:
    in al,dx
    test al,20h
    jnz .send
    loop .wait
.send:
    mov dx,3F8h
    mov al,ah
    out dx,al
    pop dx
    pop cx
    pop ax
    ret

log_host db '[DPMIRUN] HOST',13,10,0
log_run db '[DPMIRUN] RUN ',0
log_in db ' IN ',0
log_crlf db 13,10,0
log_exit db '[DPMIRUN] EXIT '
log_code db '0000',13,10,0
usage db 'Usage: DPMIRUN program [arguments]',13,10,0
no_memory db 'DPMIRUN: cannot resize its memory block.',13,10,0
no_session db 'DPMIRUN: no CiukiOS session is loaded (JLOAD CVSESS.DLL).',13,10,0
no_descriptor db 'DPMIRUN: the session has no scheduler; start the program in the DOS window.',13,10,0
host_failed db 'DPMIRUN: the protected-mode host \SBEMU\HDPMI32I.EXE could not be loaded.',13,10,0
exec_failed db 'DPMIRUN: the program could not be started, DOS error '
exec_error_code db '0000h.',13,10,0
unload_failed db 'DPMIRUN: the protected-mode host could not be unloaded.',13,10,0

host_path db '\SBEMU\HDPMI32I.EXE',0
command_path db '\COMMAND.COM',0
host_tail db host_tail_end-host_tail-1
    db ' -c'
host_seg db '0000:'
host_off db '0000 -r'
host_tail_end:
    db 13,0,0,0                         ; room for " -v" (VM mode)
unload_tail db 3,' -u',13

entry dd 0
exec_name dw program
params dw 0,0,0,5Ch,0,6Ch,0
saved_sp dw 0
saved_drive db 0
exit_code db 0
have_host db 0
window_host db 0
host_vcpi db 0
vm_active db 0
vm_devices db 0
vm_shared db 0
vm_sched_bound db 0
vm_hooked db 0
vm_no_session db 'DPMIRUN: this VM has no DOS window session.',13,10,0
vm_begin_fail db '[DPMIRUN] VM BEGIN FAIL '
vm_begin_step db 'I', ' '
vm_begin_code db '0000',13,10,0
vm_tsc_log db '[DPMIRUN] TSC KHz '
vm_tsc_hi db '0000'
vm_tsc_lo db '0000',13,10,0
vm_audio_log db '[DPMIRUN] AUDIO '
vm_audio_code db '0000',13,10,0
vm_dev_fail_log db '[DPMIRUN] DEVICE BEGIN ERROR '
vm_dev_fail_code db '0000',13,10,0
vm_log_session db '[DPMIRUN] VM SESSION',13,10,0
vm_log_end db '[DPMIRUN] VM END',13,10,0
    align 4
vm_tsc dd 0
vm_entry_linear dd 0
vm_old08 dd 0
vm_scratch dd 0,0
vm_config:
    dd VM_VCFG_MAGIC
    dw VM_ABI_VIDEO_VERSION, VM_VCFG_BYTES
    times VM_VCFG_BYTES-8 db 0
vm_scheduler:
    dd CVSCHED_MAGIC
    dw CVSCHED_VERSION, CVSCHED_BYTES
    times CVSCHED_BYTES-8 db 0
vm_share times VM_VSHARE_BYTES db 0     ; must follow the descriptor (HDPMI -c)
moved db 0
child_tail times 128 db 0
program times 128 db 0
directory times 128 db 0
saved_dir times 68 db 0
    align 2
    times 512 db 0
stack_top:
image_end:
