; VM0-only binding. Never identify a drive by trying guessed ATA ports.
; AH48's temporary DPTE is copied before any further BIOS call (EDD 1.1).
; The native module must observe real firmware Device Control writes; a
; single BIOS reset followed by a fresh reference read may establish them.
disk_prepare_parent:
    pushfd
    pushad
    push ds
    push es
    push fs
    push cs
    pop ds
    push cs
    pop es
    mov word [disk_prepare_result],0100h
    call disk_query
    jc .api_failed
    mov byte [disk_status_enabled],1
    mov byte [disk_need_discovery],0
    mov [disk_packet+6],bl
    cmp dword [disk_snapshot+8],1
    je .ready
    ; Firmware stack is temporary, not copied into the child resident MZ.
    ; The ordinary VMFORK stack is only512 bytes and is not a BIOS stack.
    mov word [disk_prepare_result],0A00h
    mov bx,256
    mov ah,48h
    int 21h
    jc .api_failed
    mov [disk_bios_stack],ax
    mov word [disk_prepare_result],0200h
    mov dl,[disk_packet+6]
    mov bx,55AAh
    mov ah,41h
    call disk_bios_call
    jc .done
    cmp bx,0AA55h
    jne .done
    test cx,1
    jz .done
    mov [disk_extensions],cx
    mov word [disk_prepare_result],0300h
    mov si,disk_packet+8
    mov word [si],30
    mov dl,[disk_packet+6]
    mov ah,48h
    call disk_bios_call
    jc .done
    cmp word [disk_packet+8],26
    jb .done
    mov word [disk_prepare_result],0400h
    test word [disk_extensions],4      ; EDD config pointer is optional
    jz .discover
    cmp word [disk_packet+8],30
    jb .discover
    mov si,[disk_packet+34]
    movzx eax,word [disk_packet+36]
    shl eax,4
    movzx edx,si
    add eax,edx
    ; AH48 may put its temporary DPTE in the BIOS data area. The T23
    ; returns 0040:00C6 (physical04C6); EDD specifies no4KiB minimum.
    test eax,eax
    jz .discover
    cmp eax,0FFFF0h
    ja .discover
    ; Normalize the far pointer so a valid table crossing the caller's
    ; segment offset FFFFh is copied as contiguous physical bytes.
    mov edx,eax
    and dx,0Fh
    mov si,dx
    shr eax,4
    mov fs,ax
    mov di,disk_packet+40
    mov cx,16
.dpte:
    mov al,[fs:si]
    stosb
    inc si
    loop .dpte
    jmp .geometry
.discover:
    mov byte [disk_need_discovery],1
.geometry:
    mov word [disk_prepare_result],0500h
    mov dl,[disk_packet+6]
    mov ah,8
    call disk_bios_call
    jc .done
    movzx ax,dh
    inc ax
    mov [disk_packet+56],ax
    mov al,cl
    and ax,3Fh
    mov [disk_packet+58],ax
    movzx ax,ch
    movzx dx,cl
    and dx,0C0h
    shl dx,2
    or ax,dx
    inc ax
    mov [disk_packet+60],ax
    cmp byte [disk_need_discovery],0
    jne .observed_mapping
    mov word [disk_prepare_result],0600h
    mov ax,VM_OP_DISK_PREPARE
    mov di,disk_packet
    mov cx,64
    call far [entry]
    jc .api_failed
    mov word [disk_prepare_result],0700h
    call disk_reference_read
    jc .done
    call disk_query
    jc .done
    cmp dword [disk_snapshot+36],1       ; status.control_shadow_known
    je .bind
    mov word [disk_prepare_result],0900h
    mov dl,[disk_packet+6]
    xor ax,ax
    call disk_bios_call
    jc .done
    call disk_reference_read
    jc .done
.bind:
    mov word [disk_prepare_result],0800h
    mov di,disk_reference_sector
    mov cx,512
    mov ax,VM_OP_DISK_BIND
    call far [entry]
    jc .api_failed
    jmp .ready
.observed_mapping:
    ; The exact device is selected by actual BIOS task-file OUTs, never by
    ; probing guessed ports or by matching unrelated disk contents.
    mov word [disk_prepare_result],0B00h
    mov ax,VM_OP_DISK_DISCOVER_BEGIN
    mov di,disk_packet
    mov cx,64
    call far [entry]
    jc .api_failed
    mov word [disk_prepare_result],0C00h
    mov dl,[disk_packet+6]
    xor ax,ax
    call disk_bios_call
    jc .abort_discovery
    call disk_reference_read
    jc .abort_discovery
    mov word [disk_prepare_result],0D00h
    mov ax,VM_OP_DISK_DISCOVER_END
    mov di,disk_packet
    mov cx,64
    call far [entry]
    jc .api_failed
    jmp .bind
.abort_discovery:
    ; Always release passive callbacks, including failed firmware reads.
    mov ax,VM_OP_DISK_DISCOVER_END
    xor cx,cx
    call far [entry]
    jmp .done
.ready:
    mov word [disk_prepare_result],0
    jmp .done
.api_failed:
    mov [disk_prepare_result],al        ; high byte retains failed phase
.done:
    push cs
    pop ds
    push cs
    pop es
    cmp word [disk_bios_stack],0
    je .stack_released
    mov es,[disk_bios_stack]
    mov ah,49h
    int 21h
    mov word [disk_bios_stack],0
    push cs
    pop es
.stack_released:
    call disk_query
    jc .out
    ; Persist binding evidence once in VM0; child checkpoints use the
    ; existing parent mailbox and never introduce private-firmware writes.
    mov dx,disk_status_path
    xor cx,cx
    mov ah,3Ch
    int 21h
    jc .out
    mov bx,ax
    mov dx,disk_snapshot
    mov cx,VM_DISK_STATUS_SIZE
    mov ah,40h
    int 21h
    mov ah,3Eh
    int 21h
    ; Preserve the actual EDD capability flags and returned far pointer;
    ; phase alone must not conceal which optional BIOS data was missing.
    mov dx,disk_binding_path
    xor cx,cx
    mov ah,3Ch
    int 21h
    jc .out
    mov bx,ax
    mov dx,disk_binding_snapshot
    mov cx,80
    mov ah,40h
    int 21h
    mov ah,3Eh
    int 21h
.out:
    pop fs
    pop es
    pop ds
    popad
    popfd
    ret

disk_bios_call:
    push ds
    push es
    push si
    push di
    push bp
    pushfd
    pop dword [cs:disk_bios_flags]
    cli
    mov [cs:disk_bios_ss],ss
    mov [cs:disk_bios_sp],sp
    mov ss,[cs:disk_bios_stack]
    mov sp,1000h
    push dword [cs:disk_bios_flags]
    popfd
    int 13h
    pushfd
    pop dword [cs:disk_bios_flags]
    cli
    mov ss,[cs:disk_bios_ss]
    mov sp,[cs:disk_bios_sp]
    push dword [cs:disk_bios_flags]
    popfd
    pop bp
    pop di
    pop si
    pop es
    pop ds
    cld
    jnc .returned
    mov [cs:disk_prepare_result],ah
.returned:
    ret

disk_reference_read:
    mov word [disk_dap+2],1
    mov [disk_dap+6],cs
    mov si,disk_dap
    mov dl,[disk_packet+6]
    mov ax,4200h
    jmp disk_bios_call

disk_query:
    mov ax,VM_OP_DISK_STATUS
    mov di,disk_snapshot
    mov cx,VM_DISK_STATUS_SIZE
    call far [cs:entry]
    ret

; Snapshot the native counters into the spare CVFL bytes at every stage.
; It preserves the EXEC flags/result, which the checkpoint also preserves.
disk_trace_snapshot:
    pushfd
    pushad
    push ds
    push es
    push cs
    pop ds
    push cs
    pop es
    cmp byte [disk_status_enabled],0
    je .done
    call disk_query
    jc .done
    mov ax,[disk_snapshot+12]
    mov [trace_disk_state],ax
    mov ax,[disk_snapshot+16]
    mov [trace_disk_error],ax
    mov ax,[disk_snapshot+20]
    mov [trace_disk_ports],ax
    mov ax,[disk_snapshot+24]
    mov [trace_disk_ports+2],ax
    mov eax,[disk_snapshot+44]
    mov [trace_disk_reads],eax
    mov eax,[disk_snapshot+48]
    mov [trace_disk_writes],eax
    mov eax,[disk_snapshot+60]
    mov [trace_disk_lba],eax
    mov al,[disk_snapshot+40]
    mov ah,[disk_snapshot+36]
    mov [trace_disk_control],ax
    mov ax,[disk_prepare_result]
    mov [trace_disk_prepare],ax
.done:
    pop es
    pop ds
    popad
    popfd
    ret

align 4
disk_snapshot times VM_DISK_STATUS_SIZE db 0
disk_binding_snapshot:
    db 'CDB1'
    dw 1,80
disk_extensions dw 0
disk_prepare_result dw 0
disk_need_discovery db 0
    times 3 db 0
disk_packet:
    db 'ADP1'
    dw 64
    db 0,0
    times 56 db 0
disk_dap:
    db 10h,0
    dw 1,disk_reference_sector,0
    dq 0
disk_status_enabled db 0
align 2
disk_bios_stack dw 0
disk_bios_ss dw 0
disk_bios_sp dw 0
disk_bios_flags dd 0
disk_reference_sector times 512 db 0
disk_status_path db 'C:\SYSTEM\DISKATA.BIN',0
disk_binding_path db 'C:\SYSTEM\DISKBIND.BIN',0
