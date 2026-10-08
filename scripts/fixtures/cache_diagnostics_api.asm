; Call the real CVSESSION framebuffer-diagnostics API from the system VM or
; a VMFORK child. Build both forms with CVFD_GUEST undefined/defined.
; The caller is bounded to its own conventional buffer and surrounding guards.
bits 16
cpu 386
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
    jc failure

    xor di,di
    mov es,di
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    int 2Fh
    mov ax,es
    or ax,di
    jz failure
    mov [cs:entry],di
    mov [cs:entry+2],es
    call fill_guarded_buffer

%ifdef CVFD_GUEST
    ; A forked DOS VM is not authorized, regardless of request length or ES:DI.
    push cs
    pop es
    mov di,diag_packet
    mov cx,VM_FB_DIAG_SIZE-1
    mov ax,VM_OP_FB_DIAGNOSTICS | VM_OP_NO_SWITCH
    call far [cs:entry]
    jnc failure
    cmp ax,VM_ERROR_OPERATION
    jne failure
    call check_untouched

    mov ax,0FFFFh
    mov es,ax
    mov di,0FFFFh
    mov cx,VM_FB_DIAG_SIZE
    mov ax,VM_OP_FB_DIAGNOSTICS | VM_OP_NO_SWITCH
    call far [cs:entry]
    jnc failure
    cmp ax,VM_ERROR_OPERATION
    jne failure
    call check_untouched

    push cs
    pop es
    mov di,diag_packet
    mov cx,VM_FB_DIAG_SIZE
    mov ax,VM_OP_FB_DIAGNOSTICS | VM_OP_NO_SWITCH
    call far [cs:entry]
    jnc failure
    cmp ax,VM_ERROR_OPERATION
    jne failure
    call check_untouched
    mov dx,guest_pass
    jmp report_pass
%else
    ; A short caller extent must fail before touching even its first byte.
    push cs
    pop es
    mov di,diag_packet
    mov cx,VM_FB_DIAG_SIZE-1
    mov ax,VM_OP_FB_DIAGNOSTICS | VM_OP_NO_SWITCH
    call far [cs:entry]
    jnc failure
    cmp ax,VM_ERROR_ADDRESS
    jne failure
    call check_untouched

    ; An unaddressable ES:DI must also fail without touching the guarded data.
    mov ax,0FFFFh
    mov es,ax
    mov di,0FFFFh
    mov cx,VM_FB_DIAG_SIZE
    mov ax,VM_OP_FB_DIAGNOSTICS | VM_OP_NO_SWITCH
    call far [cs:entry]
    jnc failure
    cmp ax,VM_ERROR_ADDRESS
    jne failure
    call check_untouched

    ; A valid system-VM request returns the exact bounded versioned record.
    push cs
    pop es
    mov di,diag_packet
    mov cx,VM_FB_DIAG_SIZE
    mov ax,VM_OP_FB_DIAGNOSTICS | VM_OP_NO_SWITCH
    call far [cs:entry]
    jc failure
    test ax,ax
    jnz failure
    cmp dword [diag_packet],VM_FB_DIAG_MAGIC
    jne failure
    cmp word [diag_packet+4],VM_FB_DIAG_VERSION
    jne failure
    cmp word [diag_packet+6],VM_FB_DIAG_SIZE
    jne failure
    cmp dword [diag_packet+8],VM_FB_DIAG_CACHE_SIZE
    jne failure
    cmp dword [diag_packet+12],VM_FB_DIAG_TIME_SIZE
    jne failure
    cmp dword [diag_packet+16],0x42465643 ; CVFB
    jne failure
    cmp dword [diag_packet+24],VM_FB_DIAG_CACHE_SIZE
    jne failure
    cmp dword [diag_packet+592],0x42465643 ; CVFB
    jne failure
    cmp word [guard_before],0xA55A
    jne failure
    cmp word [guard_after],0x5AA5
    jne failure
    mov dx,system_pass
%endif

report_pass:
    push cs
    pop ds
    mov ah,09h
    int 21h
    mov ax,4C00h
    int 21h

fill_guarded_buffer:
    mov word [guard_before],0xA55A
    mov word [guard_after],0x5AA5
    push cs
    pop es
    mov di,diag_packet
    mov cx,VM_FB_DIAG_SIZE
    mov al,0xA5
    cld
    rep stosb
    ret

check_untouched:
    push cs
    pop ds
    cmp word [guard_before],0xA55A
    jne failure
    cmp word [guard_after],0x5AA5
    jne failure
    mov si,diag_packet
    mov cx,VM_FB_DIAG_SIZE
    mov al,0xA5
.byte:
    cmp [si],al
    jne failure
    inc si
    loop .byte
    ret

failure:
    push cs
    pop ds
    mov dx,fail_message
    mov ah,09h
    int 21h
    mov ax,4C01h
    int 21h

entry       dd 0
guard_before dw 0
diag_packet times VM_FB_DIAG_SIZE db 0
guard_after dw 0
system_pass db '[CVFD-API] SYSTEM PASS',13,10,'$'
guest_pass  db '[CVFD-API] CHILD PASS',13,10,'$'
fail_message db '[CVFD-API] FAIL',13,10,'$'
stack_space times 512 db 0
stack_top:
program_end:
