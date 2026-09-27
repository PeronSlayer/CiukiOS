; End-to-end owner for the real Jemm + official HDPMI 3.24 integration.
; One conventional descriptor follows one foreground DOS execution context.
bits 16
org 100h
%include "src/vm/session_abi.inc"
%include "src/vm/session_scheduler_abi.inc"
%include "src/vm/session_video_abi.inc"

start:
    cli
    mov ax,cs
    mov ss,ax
    mov sp,stack_top
    sti
    mov ds,ax
    mov es,ax
    mov bx,(program_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    jc fail

    xor di,di
    mov es,di
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    int 2Fh
    mov ax,es
    or ax,di
    jz fail
    mov [cs:entry],di
    mov [cs:entry+2],es
    xor cx,cx
    mov ax,VM_OP_QUERY
    call far [cs:entry]
    jc fail
    cmp ax,VM_ABI_VERSION
    jne fail
    test bx,VM_CAP_HOST_SCHEDULER
    jz fail

    ; A stale DPMI host could own a different page table and IOPB.
    mov ax,1687h
    int 2Fh
    test ax,ax
    jz fail

    push cs
    pop es
    mov di,descriptor
    mov cx,CVSCHED_BYTES
    xor ax,ax
    cld
    rep stosb
    mov dword [descriptor+CVSCHED_MAGIC_OFS],CVSCHED_MAGIC
    mov word [descriptor+CVSCHED_VERSION_OFS],CVSCHED_VERSION
    mov word [descriptor+CVSCHED_BYTES_OFS],CVSCHED_BYTES
    call format_tails
    mov di,descriptor
    mov cx,CVSCHED_BYTES
    mov ax,VM_OP_BIND_SCHED
    call far [cs:entry]
    jc fail
    mov byte [bound_flag],1

    ; Let the read-only harness capture the owner's original low PTEs before
    ; BEGIN replaces them.  The query packet carries the authoritative CR3.
    push cs
    pop es
    mov di,info_packet
    mov cx,VM_INFO_SIZE
    mov ax,VM_OP_QUERY
    call far [cs:entry]
    jc fail
    mov byte [observation+4],10h
    xor ah,ah
    int 16h

    mov byte [stage],11h
    mov ax,VM_OP_BEGIN
    call far [cs:entry]
    jc fail
    mov byte [active],1
    mov byte [observation+4],1
    mov si,mark_begin
    call serial_puts
	; Hold the post-BEGIN/pre-host boundary until the external harness has
	; proved that no HDPMI ownership exists yet.
	xor ah,ah
	int 16h

    push cs
    pop es
    mov di,share_packet
    mov cx,VM_VSHARE_BYTES
    mov ax,VM_OP_VIDEO_SHARE
    call far [cs:entry]
    jc fail
    cmp dword [share_packet],VM_VSHARE_MAGIC
    jne fail
    cmp dword [share_packet+4],67
    jne fail
    mov byte [video_shared],1

    mov byte [stage],12h
    mov si,mark_host_start
    call serial_puts
    mov byte [expect_tsr],1
    mov word [parameters+2],host_tail
    mov dx,hdpmi_path
    call execute_child
    jc fail
    mov si,mark_host_return
    call serial_puts
    mov byte [expect_tsr],0
    mov byte [host_live],1
    mov byte [stage],13h
    mov ax,1687h
    int 2Fh
    test ax,ax
    jnz fail
    call check_host_idle
    jc fail
    mov si,mark_host_live
    call serial_puts
    mov byte [observation+4],2

    mov word [parameters+2],life_tail
    mov dx,life_path
    call execute_child
    jc fail
    mov ax,1
    call check_counts
    jc fail
    cmp dword [descriptor+CVSCHED_DPMI_TICKS],3
    jb fail
    mov byte [observation+4],3

    ; Repeated launch must install and remove a fresh exact handle.
    mov word [parameters+2],life_tail
    mov dx,life_path
    call execute_child
    jc fail
    mov ax,2
    call check_counts
    jc fail
    mov byte [observation+4],4

    ; UD2 reaches HDPMI's fatal client path, which shares the same detach.
    mov word [parameters+2],empty_tail
    mov dx,fault_path
    call execute_child_raw
    mov byte [stage],21h
    ; HDPMI reports a fatal protected-mode client exception back through the
    ; EXEC call as DOS status 3.  Accept that one exact result only; the
    ; adapter counters below prove that the client actually entered, faulted,
    ; removed its owned callback, and completed the detach path.
    jnc .fault_unwound
    cmp ax,3
    jne fail
.fault_unwound:
    mov byte [stage],22h
    mov ax,3
    call check_counts
    jc fail
    mov byte [stage],23h
    cmp dword [descriptor+CVSCHED_FAULT_EXITS],1
    jne fail
    mov byte [stage],24h
    mov byte [observation+4],5
    ; Hold a stable post-fault boundary so the external harness can prove the
    ; exact callback removal and restored host page table before another EXEC.
    xor ah,ah
    int 16h

    ; This is the unmodified original DOS Doom extender payload shipped in
    ; APPS/DOOMVAN, not a source-port callback and not another background VM.
    mov byte [stage],14h
    mov word [parameters+2],doom_tail
    mov dx,doom_path
    ; This Doom build reports timedemo completion through its normal error
    ; exit, so require its exact termination tuple rather than accepting an
    ; arbitrary nonzero child result.
    call execute_child_raw
    jc fail
    cmp word [exit_status],0001h
    jne fail
    mov ax,4
    call check_counts
    jc fail
    mov byte [observation+4],6
    mov byte [stage],15h

    ; END is forbidden while the resident host still owns the descriptor.
    mov ax,VM_OP_END
    call far [cs:entry]
    jnc fail
    cmp ax,VM_ERROR_DPMI_OWNED
    jne fail

	mov byte [stage],16h
    mov word [parameters+2],unload_tail
    mov dx,hdpmi_path
    call execute_child
    jc fail
    mov byte [host_live],0
	mov byte [stage],17h
    mov ax,1687h
    int 2Fh
    test ax,ax
    jz fail
	mov byte [stage],18h
    mov ax,VM_OP_VIDEO_UNSHARE
    call far [cs:entry]
    jc fail
    mov byte [video_shared],0
	mov byte [stage],19h
    mov ax,VM_OP_DPMI_RELEASE
    call far [cs:entry]
    jc fail
	mov byte [stage],1Ah
    test dword [descriptor+CVSCHED_STATE],CVSCHED_STATE_DPMI_HOST | CVSCHED_STATE_DPMI_CLIENT | CVSCHED_STATE_CALLBACK
    jnz fail

	mov byte [stage],1Bh
    mov ax,VM_OP_END
    call far [cs:entry]
    jc fail
    mov byte [active],0
    mov byte [stage],30h
    cmp dword [descriptor+CVSCHED_STATE],CVSCHED_STATE_BOUND
    jne fail
    mov byte [stage],31h
    cmp dword [descriptor+CVSCHED_INSTALLS],5
    jne fail
    mov byte [stage],32h
    cmp dword [descriptor+CVSCHED_REMOVES],5
    jne fail
    mov byte [stage],33h
    cmp dword [descriptor+CVSCHED_CLIENT_ENTRIES],5
    jne fail
    mov byte [stage],34h
    cmp dword [descriptor+CVSCHED_CLIENT_EXITS],5
    jne fail
    mov byte [stage],35h
    cmp dword [descriptor+CVSCHED_SHADOW_BYTES],20000h
    jne fail
    mov byte [stage],36h
    cmp dword [descriptor+CVSCHED_SCHEDULER_BYTES],CVSCHED_BYTES+4096
    jb fail
    mov byte [stage],37h
    cmp dword [descriptor+CVSCHED_ADAPTER_BYTES],0
    je fail
    mov byte [stage],38h
    cmp dword [descriptor+CVSCHED_CALLBACK_BYTES],0
    je fail
    mov byte [observation+4],7

	mov byte [stage],39h
    mov ax,VM_OP_UNBIND_SCHED
    call far [cs:entry]
    jc fail
    mov byte [bound_flag],0
    mov byte [stage],3Ah
    cmp dword [descriptor+CVSCHED_STATE],0
    jne fail
    mov si,passed_serial
    call serial_puts
    ; Keep the unbound descriptor resident for the external read-only PTE
    ; and physical-aperture comparisons.  Returning this deeply nested EXEC
    ; parent is a separate CiukiOS DOS-kernel limitation.
final_halt:
    sti
    hlt
    jmp final_halt

; AX=expected completed clients.  Host must be idle and all exact callback
; handles removed.  Fault count is checked separately after the UD2 launch.
check_counts:
    push eax
    mov cx,36
.retry:
    call check_host_idle
    jc .wait
    movzx eax,word [esp]
    cmp dword [descriptor+CVSCHED_INSTALLS],eax
    jne .wait
    cmp dword [descriptor+CVSCHED_REMOVES],eax
    jne .wait
    cmp dword [descriptor+CVSCHED_CLIENT_ENTRIES],eax
    jne .wait
    cmp dword [descriptor+CVSCHED_CLIENT_EXITS],eax
    jne .wait
    pop eax
    clc
    ret
.wait:
    sti
    hlt
    loop .retry
.bad:
    pop eax
    stc
    ret

check_host_idle:
    cmp dword [descriptor+CVSCHED_STATE],CVSCHED_STATE_BOUND | CVSCHED_STATE_SESSION | CVSCHED_STATE_JEMM | CVSCHED_STATE_DPMI_HOST
    jne .bad
    cmp dword [descriptor+CVSCHED_DPMI_HANDLE],0
    jne .bad
    clc
    ret
.bad:
    stc
    ret

format_tails:
    mov ax,cs
    mov di,host_seg
    call hex4
    mov ax,descriptor
    mov di,host_off
    call hex4
    mov ax,cs
    mov di,life_seg
    call hex4
    mov ax,descriptor
    mov di,life_off
    call hex4
    ret

; AX value, DS:DI four uppercase digits.
hex4:
    push ax
    push cx
    push dx
    mov cx,4
.next:
    rol ax,4
    mov dl,al
    and dl,0Fh
    add dl,'0'
    cmp dl,'9'
    jbe .store
    add dl,'A'-'9'-1
.store:
    mov [di],dl
    inc di
    loop .next
    pop dx
    pop cx
    pop ax
    ret

; Execute and accept any child return code.  CF only reports EXEC failure.
execute_child_raw:
    mov byte [accept_any],1
    jmp execute_common
execute_child:
    mov byte [accept_any],0
execute_common:
    mov ax,cs
    mov ds,ax
    mov es,ax
    mov [parameters+4],ax
    mov [parameters+8],ax
    mov [parameters+12],ax
    mov [save_sp],sp
    mov [save_ss],ss
    mov bx,parameters
    mov ax,4B00h
    int 21h
    mov [cs:exec_ax],ax
    pushf
    pop word [cs:exec_flags]
    cli
    mov ss,[cs:save_ss]
    mov sp,[cs:save_sp]
    sti
    push cs
    pop ds
    push cs
    pop es
    test word [exec_flags],1
    jnz .exec_error
    mov ah,4Dh
    int 21h
    mov [exit_status],ax
    cmp byte [expect_tsr],1
    jne .normal
    cmp ah,3
    jne .bad
    cmp al,2
    ja .bad
    clc
    ret
.normal:
    cmp byte [accept_any],1
    je .ok
    test al,al
    jnz .bad
.ok:
    clc
    ret
.bad:
    stc
.return:
    ret
.exec_error:
    mov ax,[exec_ax]
    stc
    ret

fail:
    push cs
    pop ds
    mov [fail_ax],ax
    mov si,failed_serial
    call serial_puts
    mov al,[stage]
    call serial_hex8
    mov al,' '
    call serial_putc
    mov ax,[fail_ax]
    mov al,ah
    call serial_hex8
    mov ax,[fail_ax]
    call serial_hex8
    mov al,' '
    call serial_putc
    mov al,byte [descriptor+CVSCHED_STATE]
    call serial_hex8
    mov al,' '
    call serial_putc
    mov al,byte [descriptor+CVSCHED_LAST_ERROR]
    call serial_hex8
    mov al,' '
    call serial_putc
    mov al,byte [descriptor+CVSCHED_INSTALLS]
    call serial_hex8
    mov al,'/'
    call serial_putc
    mov al,byte [descriptor+CVSCHED_REMOVES]
    call serial_hex8
    mov al,' '
    call serial_putc
    mov al,byte [descriptor+CVSCHED_CLIENT_ENTRIES]
    call serial_hex8
    mov al,'/'
    call serial_putc
    mov al,byte [descriptor+CVSCHED_CLIENT_EXITS]
    call serial_hex8
    mov al,13
    call serial_putc
    mov al,10
    call serial_putc
    mov dx,failed_stage
    mov ah,9
    int 21h
    mov al,[stage]
    call print_hex8
    mov dl,' '
    mov ah,2
    int 21h
    mov ax,[fail_ax]
    mov al,ah
    call print_hex8
    mov ax,[fail_ax]
    call print_hex8
    mov dl,' '
    mov ah,2
    int 21h
    mov al,byte [descriptor+CVSCHED_STATE]
    call print_hex8
    mov dl,' '
    mov ah,2
    int 21h
    mov al,byte [descriptor+CVSCHED_LAST_ERROR]
    call print_hex8
    mov dx,newline
    mov ah,9
    int 21h
    mov byte [observation+4],0FFh
	; Preserve the failed ownership state for the external debugger.  A key
	; acknowledges the snapshot and permits the normal fail-closed unwind.
	xor ah,ah
	int 16h
    ; Only remove the host after discovery proves it is present.  A failed
    ; exact callback removal intentionally leaves ownership and halts below.
    cmp byte [host_live],0
    je .unshare
    mov word [parameters+2],unload_tail
    mov dx,hdpmi_path
    call execute_child_raw
    mov ax,1687h
    int 2Fh
    test ax,ax
    jz owned_failure
    mov byte [host_live],0
.unshare:
    cmp byte [video_shared],0
    je .release
    mov ax,VM_OP_VIDEO_UNSHARE
    call far [cs:entry]
    jc owned_failure
    mov byte [video_shared],0
.release:
    cmp byte [bound_flag],0
    je .print
    mov ax,VM_OP_DPMI_RELEASE
    call far [cs:entry]
    jc owned_failure
    cmp byte [active],0
    je .unbind
    mov ax,VM_OP_END
    call far [cs:entry]
    jc owned_failure
    mov byte [active],0
.unbind:
    mov ax,VM_OP_UNBIND_SCHED
    call far [cs:entry]
    jc owned_failure
    mov byte [bound_flag],0
.print:
    mov dx,failed
    mov ah,9
    int 21h
    mov ax,4C01h
    int 21h

owned_failure:
    mov byte [observation+4],0FEh
    sti
    hlt
    jmp owned_failure

print_hex8:
    push ax
    push bx
    mov bl,al
    shr al,4
    call print_nibble
    mov al,bl
    and al,0Fh
    call print_nibble
    pop bx
    pop ax
    ret

serial_puts:
    lodsb
    test al,al
    jz .done
    call serial_putc
    jmp serial_puts
.done:
    ret

serial_hex8:
    push ax
    push bx
    mov bl,al
    ror al,4
    call .nibble
    mov al,bl
    call .nibble
    pop bx
    pop ax
    ret
.nibble:
    and al,0Fh
    add al,'0'
    cmp al,'9'
    jbe serial_putc
    add al,'A'-'9'-1
serial_putc:
    push dx
    push ax
    mov dx,03FDh
.wait:
    in al,dx
    test al,20h
    jz .wait
    pop ax
    mov dx,03F8h
    out dx,al
    pop dx
    ret
print_nibble:
    add al,'0'
    cmp al,'9'
    jbe .emit
    add al,'A'-'9'-1
.emit:
    mov dl,al
    mov ah,2
    int 21h
    ret

entry dd 0
active db 0
bound_flag db 0
host_live db 0
video_shared db 0
expect_tsr db 0
accept_any db 0
stage db 0
save_sp dw 0
save_ss dw 0
exit_status dw 0
exec_ax dw 0
exec_flags dw 0
fail_ax dw 0
parameters dw 0,empty_tail,0,5Ch,0,6Ch,0

host_tail db host_tail_end-host_tail_text
host_tail_text db ' -c'
host_seg db '0000'
db ':'
host_off db '0000'
db ' -r -v'
host_tail_end:
db 13

life_tail db life_tail_end-life_tail_text
life_tail_text db ' '
life_seg db '0000'
db ':'
life_off db '0000'
life_tail_end:
db 13

unload_tail db unload_tail_end-unload_tail_text
unload_tail_text db ' -u'
unload_tail_end:
db 13
empty_tail db 0,13

doom_tail db doom_tail_end-doom_tail_text
doom_tail_text db ' -timedemo cvtest -nosound -nomouse -iwad doom.wad'
doom_tail_end:
db 13

hdpmi_path db '\HDPMI32.EXE',0
life_path db '\DPMILIF.EXE',0
fault_path db '\DPMIFLT.EXE',0
doom_path db '\DPMIDOOM.EXE',0
passed_serial db '[DPMILIFE] PASS repeated/fault/original client, owned removal, full unwind',13,10,0
failed db '[DPMILIFE] PARENT FAIL',13,10,'$'
failed_stage db '[DPMILIFE] PARENT FAIL stage/ax=','$'
failed_serial db '[DPMILIFE] PARENT FAIL stage/ax=',0
mark_begin db '[DPMILIFE] BEGIN PASS',13,10,0
mark_host_start db '[DPMILIFE] HOST START',13,10,0
mark_host_return db '[DPMILIFE] HOST RETURN',13,10,0
mark_host_live db '[DPMILIFE] HOST LIVE',13,10,0
newline db 13,10,'$'
observation db 'CVLP',0,0,0,0
info_packet times VM_INFO_SIZE db 0
align 16
descriptor times CVSCHED_BYTES db 0
share_packet times VM_VSHARE_BYTES db 0
align 16
stack_space times 4096 db 0
stack_top:
program_end:
