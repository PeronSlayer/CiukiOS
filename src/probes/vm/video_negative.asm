; Negative qualification for the actual CVSESSION JLM, not a model of it.
; No DOS/BIOS video output occurs while this program owns an active session.
; All framebuffer packets below must fail before mapping physical memory.
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
    mov byte [cs:entry_found],1
    call query
    jc failure
    cmp ax,VM_ABI_VERSION
    jne failure
    cmp dword [info+VM_INFO_ACTIVE],0
    jne failure
    cmp dword [info+VM_INFO_FB_BOUND],0
    jne failure

    mov byte [step],1
    mov ax,VM_OP_END
    mov bp,VM_ERROR_INACTIVE
    call expect_error
    mov ax,VM_OP_UNBIND_FB
    mov bp,VM_ERROR_FB_UNBOUND
    call expect_error
    mov edi,buffer
    mov cx,1
    xor edx,edx
    mov ax,VM_OP_READBACK
    mov bp,VM_ERROR_INACTIVE
    call expect_error
    xor bx,bx
    mov ax,VM_OP_FB_COPY
    mov bp,VM_ERROR_FB_UNBOUND
    call expect_error
    mov ax,7FFFh
    mov bp,VM_ERROR_OPERATION
    call expect_error

    ; Reject lower memory, misalignment, empty/oversize/overflowing extents.
    mov byte [step],2
    mov dword [binding+VM_FB_PACKET_PHYSICAL],7FFFF000h
    mov dword [binding+VM_FB_PACKET_LENGTH],1000h
    call expect_bad_bind_address
    mov dword [binding+VM_FB_PACKET_PHYSICAL],80000001h
    call expect_bad_bind_address
    mov dword [binding+VM_FB_PACKET_PHYSICAL],80000000h
    mov dword [binding+VM_FB_PACKET_LENGTH],0
    call expect_bad_bind_address
    mov dword [binding+VM_FB_PACKET_LENGTH],VM_FB_MAX_BYTES+1
    call expect_bad_bind_address
    mov dword [binding+VM_FB_PACKET_PHYSICAL],0FFFFF000h
    mov dword [binding+VM_FB_PACKET_LENGTH],2000h
    call expect_bad_bind_address

    ; The packet header and its conventional-memory extent are checked too.
    mov byte [step],3
    mov dword [binding+VM_FB_PACKET_PHYSICAL],80000000h
    mov dword [binding+VM_FB_PACKET_LENGTH],1000h
    mov dword [binding],0
    mov bp,VM_ERROR_ABI
    call expect_bad_bind
    mov dword [binding],VM_FB_PACKET_MAGIC
    mov word [binding+VM_FB_PACKET_VERSION],101h
    call expect_bad_bind
    mov word [binding+VM_FB_PACKET_VERSION],VM_ABI_VERSION
    mov word [binding+VM_FB_PACKET_BYTES],VM_FB_PACKET_SIZE-1
    call expect_bad_bind
    mov word [binding+VM_FB_PACKET_BYTES],VM_FB_PACKET_SIZE
    mov edi,binding
    mov cx,VM_FB_PACKET_SIZE-1
    mov ax,VM_OP_BIND_FB
    mov bp,VM_ERROR_ADDRESS
    call expect_error
    mov edi,0FFFFh
    mov cx,VM_FB_PACKET_SIZE
    mov ax,VM_OP_BIND_FB
    call expect_error
    call query
    jc failure
    cmp dword [info+VM_INFO_FB_BOUND],0
    jne failure
    cmp dword [binding_guard],0DEADBEEFh
    jne failure

    mov byte [step],4
    mov ax,VM_OP_BEGIN
    call far [cs:entry]
    jc failure
    mov ax,VM_OP_BEGIN
    mov bp,VM_ERROR_ACTIVE
    call expect_error
    ; The otherwise well-formed bind must stop at the active-session check.
    mov bp,VM_ERROR_ACTIVE
    call expect_bad_bind
    ; UNBIND must obey the same lifetime rule, even if no FB was bound.
    mov ax,VM_OP_UNBIND_FB
    mov bp,VM_ERROR_ACTIVE
    call expect_error
    call query
    jc failure
    cmp dword [info+VM_INFO_FATAL_CODE],0
    jne failure

    ; Supported string I/O: a REP OUTSB DAC load reaches the VGA model.
    mov byte [step],5
    mov dx,3C8h
    mov al,40h
    out dx,al
    inc dx
    mov si,port_stream
    mov cx,3
    cld
    rep outsb
    test cx,cx
    jnz failure
    mov dx,3C7h
    mov al,40h
    out dx,al
    mov dx,3C9h
    in al,dx
    cmp al,[port_stream]
    jne failure
    in al,dx
    cmp al,[port_stream+1]
    jne failure
    call query
    jc failure
    cmp dword [info+VM_INFO_FATAL_CODE],0
    jne failure
    ; A transfer that cannot complete stays an explicit sticky fatal: a word
    ; element at 3DFh would reach 3E0h, outside the trapped VGA range.
    mov byte [step],7
    mov si,port_stream
    mov cx,1
    mov dx,3DFh
    mov eax,0A1B2C3D4h
    rep outsw
    cmp eax,0A1B2C3D4h
    jne failure
    call query
    jc failure
    cmp dword [info+VM_INFO_FATAL_CODE],VM_FATAL_STRING_IO
    jne failure
    cmp dword [info+VM_INFO_FATAL_PORT],3DFh
    jne failure
    mov eax,[info+VM_INFO_FATAL_IO_TYPE]
    and eax,64h                 ; STRING_IO | REP_IO | OUTPUT, upstream Jemm
    cmp eax,64h
    jne failure
    mov dword [buffer],0DEADBEEFh
    mov edi,buffer
    xor edx,edx
    mov cx,4
    mov ax,VM_OP_READBACK
    mov bp,VM_ERROR_OPERATION
    call expect_error
    cmp dword [buffer],0DEADBEEFh
    jne failure

    mov byte [step],6
    mov ax,VM_OP_END
    call far [cs:entry]
    jc failure
    call query
    jc failure
    cmp dword [info+VM_INFO_ACTIVE],0
    jne failure
    cmp dword [info+VM_INFO_FATAL_CODE],VM_FATAL_STRING_IO
    jne failure
    mov edi,buffer
    xor edx,edx
    mov cx,4
    mov ax,VM_OP_READBACK
    mov bp,VM_ERROR_INACTIVE
    call expect_error
    mov ax,VM_OP_BEGIN
    call far [cs:entry]
    jc failure
    call query
    jc failure
    cmp dword [info+VM_INFO_ACTIVE],1
    jne failure
    cmp dword [info+VM_INFO_FATAL_CODE],0
    jne failure
    cmp dword [info+VM_INFO_FATAL_PORT],0
    jne failure
    cmp dword [info+VM_INFO_FATAL_IO_TYPE],0
    jne failure
    ; A new session is a freshly set text mode 03h: cleared cells at B800.
    mov edi,buffer
    mov edx,18000h
    mov cx,4
    mov ax,VM_OP_READBACK
    call far [cs:entry]
    jc failure
    cmp dword [buffer],07200720h
    jne failure
    mov ax,VM_OP_END
    call far [cs:entry]
    jc failure
    call query
    jc failure
    cmp dword [info+VM_INFO_ACTIVE],0
    jne failure
    cmp dword [info+VM_INFO_FB_BOUND],0
    jne failure
    mov ax,VM_OP_END
    mov bp,VM_ERROR_INACTIVE
    call expect_error
    mov dx,pass_message
    mov ah,9
    int 21h
    mov ax,4C00h
    int 21h

expect_bad_bind_address:
    mov bp,VM_ERROR_ADDRESS
expect_bad_bind:
    push cs
    pop es
    mov edi,binding
    mov cx,VM_FB_PACKET_SIZE
    mov ax,VM_OP_BIND_FB
expect_error:
    call far [cs:entry]
    jnc failure
    cmp ax,bp
    jne failure
    ret

query:
    push cs
    pop es
    mov edi,info
    mov cx,VM_INFO_SIZE
    mov ax,VM_OP_QUERY
    call far [cs:entry]
    ret

failure:
    push cs
    pop ds
    push cs
    pop es
    cmp byte [entry_found],0
    je .print
    ; Discover actual ownership even if an allegedly rejected call succeeded.
    call query
    jc owned_failure
    cmp dword [info+VM_INFO_ACTIVE],0
    je .unbind
    mov ax,VM_OP_END
    call far [cs:entry]
    jc owned_failure
.unbind:
    cmp dword [info+VM_INFO_FB_BOUND],0
    je .print
    mov ax,VM_OP_UNBIND_FB
    call far [cs:entry]
    jc owned_failure
.print:
    mov al,[step]
    add al,'0'
    mov [failure_stage],al
    mov dx,fail_message
    mov ah,9
    int 21h
    mov ax,4C01h
    int 21h
owned_failure:
    ; Never return to DOS with callbacks/memory still owned but unreported.
    mov byte [observation+4],0FFh
.wait:
    sti
    hlt
    jmp .wait

entry dd 0
entry_found db 0
step db 0
observation db 'VMNG',0,0,0,0
pass_message db '[VMNEG] Lifecycle, rejected I/O and reset PASS',13,10,'$'
fail_message db '[VMNEG] FAIL stage '
failure_stage db '0',13,10,'$'
binding dd VM_FB_PACKET_MAGIC
    dw VM_ABI_VERSION,VM_FB_PACKET_SIZE
    dd 0,0
binding_guard dd 0DEADBEEFh
port_stream db 37h,15h,2Ah
info times VM_INFO_SIZE db 0
buffer times 16 db 0
align 16
stack_space times 2048 db 0
stack_top:
program_end:
