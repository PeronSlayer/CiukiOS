; Host-side qualification of CVSESSION. The child uses only standard DOS/VGA.
bits 16
org 100h
%include "src/vm/session_abi.inc"
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
%ifdef VM_DPMI_PROBE
    mov ax,1687h
    int 2Fh
    test ax,ax
    jz fail                    ; a stale DPMI page table cannot own this session
%endif
    push cs
    pop es
    mov di,info_packet
    mov cx,VM_INFO_SIZE
    mov ax,VM_OP_QUERY
    call far [cs:entry]
    jc fail
    ; Read-only harness observes physical VGA and page tables before BEGIN,
    ; then advances this real client through a physical keyboard event.
    mov byte [cs:observation+4],1
    xor ah,ah
    int 16h
    mov ax,VM_OP_BEGIN
    call far [cs:entry]
    jc fail
    mov byte [cs:active],1
    mov ax,VM_OP_BEGIN
    call far [cs:entry]
    jnc fail
    cmp ax,VM_ERROR_ACTIVE
    jne fail

%ifdef VM_DPMI_PROBE
    mov byte [dpmi_observation+4],1
    mov byte [expect_tsr],1
    mov word [parameters+2],resident_tail
    mov dx,hdpmi_path
    call execute_child
    jc fail
    mov byte [hdpmi_live],1
    mov byte [expect_tsr],0
    mov ax,1687h
    int 2Fh
    test ax,ax
    jnz fail
    mov byte [dpmi_observation+4],2
    ; DPMIVGA needs the real-mode monitor entry for its PM fault bridge.
    mov ax,[cs:entry+2]
    mov di,map_tail+7
    call put_hex4
    mov ax,[cs:entry]
    mov di,map_tail+12
    call put_hex4
    mov word [parameters+2],map_tail
    mov dx,dpmi_path
    call execute_child
    jc fail
    mov byte [dpmi_observation+4],3
    mov word [parameters+2],unload_tail
    mov dx,hdpmi_path
    call execute_child
    jc fail
    mov byte [hdpmi_live],0
    mov byte [dpmi_observation+4],4
    mov edi,buffer
    xor edx,edx
    mov cx,4
    mov ax,VM_OP_READBACK
    call far [cs:entry]
    jc fail
    ; Text mode 03h decodes B8000-BFFFF only: the PM A000 store reached no
    ; plane, exactly as on VGA.
    cmp dword [buffer],0FFFFFFFFh
    jne fail
    mov edi,buffer
    mov edx,18000h
    mov cx,4
    mov ax,VM_OP_READBACK
    call far [cs:entry]
    jc fail
    cmp dword [buffer],31564D50h
    jne fail
%endif
    mov word [parameters+2],tail
    mov dx,guest_path
    call execute_child
    jc fail

    mov edi,buffer
    xor edx,edx
    mov cx,4096
    mov ax,VM_OP_READBACK
    call far [cs:entry]
    jc fail
    mov di,buffer
    mov cx,4096
    mov al,2Ah
    cld
    repe scasb
    jne fail
    mov edi,buffer
    mov edx,18000h
    mov cx,4
    mov ax,VM_OP_READBACK
    call far [cs:entry]
    jc fail
    ; The child left mode 13h active: B8000 is outside that memory map, so
    ; its B800 stores never reached planes (as on VGA) and reads return FFh.
    cmp dword [buffer],0FFFFFFFFh
    jne fail
    ; Aperture overflow must reject without touching the caller's buffer.
    mov dword [buffer],0DEADBEEFh
    mov edi,buffer
    mov edx,1FFFFh
    mov cx,2
    mov ax,VM_OP_READBACK
    call far [cs:entry]
    jnc fail
    cmp ax,VM_ERROR_ADDRESS
    jne fail
    cmp dword [buffer],0DEADBEEFh
    jne fail
    ; A 32-bit source-offset wrap and a destination segment wrap are errors.
    mov edx,0FFFFFFFFh
    mov ax,VM_OP_READBACK
    call far [cs:entry]
    jnc fail
    cmp ax,VM_ERROR_ADDRESS
    jne fail
    xor edx,edx
    mov edi,0FFFFh
    mov cx,2
    mov ax,VM_OP_READBACK
    call far [cs:entry]
    jnc fail
    cmp ax,VM_ERROR_ADDRESS
    jne fail
    mov edi,buffer
    xor cx,cx
    mov ax,VM_OP_READBACK
    call far [cs:entry]
    jnc fail
    cmp ax,VM_ERROR_ADDRESS
    jne fail
    mov cx,4097
    mov ax,VM_OP_READBACK
    call far [cs:entry]
    jnc fail
    cmp ax,VM_ERROR_ADDRESS
    jne fail
    cmp dword [buffer],0DEADBEEFh
    jne fail
    mov ax,VM_OP_END
    call far [cs:entry]
    jc fail
    mov byte [active],0
    mov byte [observation+4],2
    xor ah,ah
    int 16h
    mov ax,VM_OP_QUERY
    xor cx,cx
    call far [cs:entry]
    jc fail
    test dx,dx
    jnz fail
    mov dx,passed
    mov ah,9
    int 21h
    mov ax,4C00h
    int 21h
fail:
    push cs
    pop ds
%ifdef VM_DPMI_PROBE
    cmp byte [hdpmi_live],0
    je .end_session
    mov word [parameters+2],unload_tail
    mov dx,hdpmi_path
    call execute_child
    jnc .end_session
    ; A PM owner still references the shadow. Keep it allocated and leave a
    ; failure observation for the emulator; never free live mappings/code.
    mov byte [observation+4],0FFh
    jmp owned_failure
.end_session:
%endif
    cmp byte [active],0
    je .print
    mov ax,VM_OP_END
    call far [cs:entry]
    jc owned_failure
.print:
    mov dx,failed
    mov ah,9
    int 21h
    mov ax,4C01h
    int 21h

owned_failure:
    ; Failed cleanup retains the session's live mappings. Do not release the
    ; owner or return to a shell that would assume physical video is restored.
    mov byte [cs:observation+4],0FFh
    sti
    hlt
    jmp owned_failure

%ifdef VM_DPMI_PROBE
; AX -> four uppercase hex digits at CS:DI.
put_hex4:
    mov cx,4
.digit:
    rol ax,4
    mov bl,al
    and bl,15
    add bl,'0'
    cmp bl,'9'
    jbe .store
    add bl,7
.store:
    mov [cs:di],bl
    inc di
    loop .digit
    ret
%endif

execute_child:
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
    jc .return
    mov ah,4Dh
    int 21h
    mov [cs:exit_status],ax
    cmp byte [expect_tsr],1
    jne .normal_exit
    cmp ah,3
    jne .bad_exit
    ; Official HDPMI returns its successful memory strategy in AL:
    ; RAWMODE=0, XMSMODE=1, VCPIMODE=2 (INIT.ASM / HDPMI.INC).
    cmp al,2
    ja .bad_exit
    clc
    ret
.normal_exit:
    test al,al
    jz .return
.bad_exit:
    stc
.return:
    ret
entry dd 0
active db 0
save_sp dw 0
save_ss dw 0
dpmi_observation db 'VMDP',0,0
exec_ax dw 0
exec_flags dw 0
exit_status dw 0
expect_tsr db 0
parameters dw 0,tail,0,5Ch,0,6Ch,0
tail db 0,13
guest_path db '\VMGUEST.COM',0
%ifdef VM_DPMI_PROBE
hdpmi_live db 0
hdpmi_path db '\SBEMU\HDPMI32I.EXE',0
dpmi_path db '\DPMIVGA.EXE',0
resident_tail db 6,' -r -v',13
unload_tail db 3,' -u',13
map_tail db 15,' -map 0000:0000',13
%endif
passed db '[VMVIDEO] Original DOS child, VGA shadow/readback and cleanup PASS',13,10,'$'
failed db '[VMVIDEO] FAIL',13,10,'$'
observation db 'VMPH',0,0,0,0
info_packet times VM_INFO_SIZE db 0
buffer times 4096 db 0
align 16
stack_space times 2048 db 0
stack_top:
program_end:
