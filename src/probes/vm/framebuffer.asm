; Exercise CVSESSION's protected framebuffer copy while DOS runs under Jemm.
; Firmware supplies the real physical aperture; no hardcoded video address.
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
    jc failed
    xor di,di
    mov es,di
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    int 2Fh
    mov ax,es
    or ax,di
    jz failed
    mov [cs:entry],di
    mov [cs:entry+2],es
    push cs
    pop es
    mov di,mode_info
    mov cx,115h             ; standard VBE 800x600x24
    mov ax,4F01h
    int 10h
    cmp ax,004Fh
    jne failed
    test word [mode_info],80h
    jz failed
    cmp word [mode_info+18],800
    jne failed
    cmp word [mode_info+20],600
    jne failed
    cmp byte [mode_info+25],24
    jne failed
    mov eax,[mode_info+40]
    mov [binding+8],eax
    movzx eax,word [mode_info+16]
    imul eax,600
    mov [binding+12],eax
    mov bx,4115h
    mov ax,4F02h
    int 10h
    cmp ax,004Fh
    jne failed
    mov byte [cs:video_changed],1
    push cs
    pop ds
    push cs
    pop es
    mov di,binding
    mov cx,VM_FB_PACKET_SIZE
    mov ax,VM_OP_BIND_FB
    call far [cs:entry]
    jc failed
    mov byte [fb_bound],1
    xor edx,edx
    mov edi,pattern
    mov cx,4096
    mov bx,1
    mov ax,VM_OP_FB_COPY
    call far [cs:entry]
    jc failed
    xor edx,edx
    mov edi,buffer
    mov cx,4096
    xor bx,bx
    mov ax,VM_OP_FB_COPY
    call far [cs:entry]
    jc failed
    mov si,pattern
    mov di,buffer
    mov cx,4096
    cld
    repe cmpsb
    jne failed
    ; Exactly one byte beyond the declared extent cannot become MMIO access.
    mov edx,[binding+12]
    mov edi,buffer
    mov cx,1
    xor bx,bx
    mov ax,VM_OP_FB_COPY
    call far [cs:entry]
    jnc failed
    cmp ax,VM_ERROR_ADDRESS
    jne failed
    cmp dword [buffer],44332211h
    jne failed
    mov byte [observation+4],1
    mov eax,[binding+8]
    mov [observation+8],eax
    xor ah,ah
    int 1Ah
    mov [tick],dx
.wait:
    mov ah,1
    int 16h
    jnz .key
    xor ah,ah
    int 1Ah
    sub dx,[cs:tick]
    cmp dx,18*8
    jb .wait
    jmp .finish
.key:
    xor ah,ah
    int 16h
.finish:
    mov ax,VM_OP_UNBIND_FB
    call far [cs:entry]
    jc failed
    mov byte [fb_bound],0
    mov ax,3
    int 10h
    push cs
    pop ds
    mov dx,pass_message
    mov ah,9
    int 21h
    mov ax,4C00h
    int 21h
failed:
    push cs
    pop ds
    cmp byte [fb_bound],0
    je .video
    mov ax,VM_OP_UNBIND_FB
    call far [cs:entry]
    jc .owned_failure
.video:
    cmp byte [video_changed],0
    je .print
    mov ax,3
    int 10h
.print:
    mov dx,fail_message
    mov ah,9
    int 21h
    mov ax,4C01h
    int 21h
.owned_failure:
    mov byte [cs:observation+4],0FFh
    sti
    hlt
    jmp .owned_failure
entry dd 0
fb_bound db 0
video_changed db 0
tick dw 0
binding db 'CVFB'
    dw 100h,16
    dd 0,0
observation db 'VMFB',0,0,0,0
    dd 0
pass_message db '[VMFRAME] Protected LFB copy, bounds and unbind PASS',13,10,'$'
fail_message db '[VMFRAME] FAIL',13,10,'$'
align 16
mode_info times 256 db 0
pattern times 1024 dd 44332211h
buffer times 4096 db 0
align 16
stack_space times 2048 db 0
stack_top:
program_end:
