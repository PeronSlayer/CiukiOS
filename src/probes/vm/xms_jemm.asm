; Actual guest CPU/XMS regression for Jemm's A20 input trap.
; Run before and after JEMM386 LOAD. No private CiukiOS or JLM services used.
bits 16
org 100h
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
    jc fatal
    mov dx,begin_msg
    call puts
    mov eax,12345678h
    in al,92h
    and eax,0FFFFFF00h
    cmp eax,12345600h
    je .canary_ok
    mov dx,canary_bad
    call failure
.canary_ok:
    mov ax,4300h
    int 2Fh
    cmp al,80h
    jne fatal
    mov ax,4310h
    int 2Fh
    mov [xms_call],bx
    mov [xms_call+2],es
    mov ah,08h
    call far [xms_call]
    cmp bl,0
    jne .legacy_bad
    cmp ax,1024
    jb .legacy_bad
    cmp dx,ax
    jb .legacy_bad
    mov [largest],ax
    mov [available],dx
    mov ah,88h
    call far [xms_call]
    cmp bl,0
    jne .extended_bad
    movzx esi,word [largest]
    cmp eax,esi
    jne .extended_bad
    movzx esi,word [available]
    cmp edx,esi
    jne .extended_bad
    cmp ecx,03FFFFFFh          ; this probe's qualified 64-MiB CiukiDOS XMS pool
    jne .extended_bad
    mov dx,query_msg
    call puts
    mov dx,64                  ; 64 KiB, then verify real allocator accounting
    mov ah,09h
    call far [xms_call]
    cmp ax,1
    jne .allocation_bad
    test dx,dx
    jz .allocation_bad
    mov [handle],dx
    mov ah,08h
    call far [xms_call]
    mov cx,[available]
    sub cx,64
    cmp dx,cx
    jne .allocation_bad
    mov cx,[largest]
    sub cx,64
    cmp ax,cx
    jne .allocation_bad
    mov dx,[handle]
    mov ah,0Ch
    call far [xms_call]
    cmp ax,1
    jne .allocation_bad
    cmp dx,10h                 ; physical >= 1 MiB and < 64 MiB
    jb .allocation_bad
    cmp dx,400h
    jae .allocation_bad
    mov dx,[handle]
    mov ah,0Dh
    call far [xms_call]
    cmp ax,1
    jne .allocation_bad
    call release_block
    cmp ax,1
    jne .allocation_bad
    mov ah,88h
    call far [xms_call]
    movzx esi,word [available]
    cmp edx,esi
    jne .allocation_bad
    movzx esi,word [largest]
    cmp eax,esi
    jne .allocation_bad
    mov dx,allocation_msg
    call puts
    jmp finish
.legacy_bad:
    mov dx,legacy_bad
    call failure
    jmp finish
.extended_bad:
    mov dx,extended_bad
    call failure
    jmp finish
.allocation_bad:
    mov dx,allocation_bad
    call failure
    cmp word [handle],0
    je finish
    call release_block
finish:
    cmp byte [failed],0
    jne fatal
    mov dx,pass_msg
    call puts
    mov ax,4C00h
    int 21h
fatal:
    mov dx,fail_msg
    call puts
    mov ax,4C01h
    int 21h
release_block:
    mov dx,[handle]
    mov ah,0Ah
    call far [xms_call]
    cmp ax,1
    jne .return
    mov word [handle],0
.return:
    ret
failure:
    mov byte [failed],1
puts:
    mov ah,9
    int 21h
    ret
xms_call dd 0
largest dw 0
available dw 0
handle dw 0
failed db 0
begin_msg db '[XMSJEMM] BEGIN',13,10,'$'
canary_bad db '[XMSJEMM] FAIL IN AL,92h changed EAX high24',13,10,'$'
legacy_bad db '[XMSJEMM] FAIL AH08 free memory',13,10,'$'
extended_bad db '[XMSJEMM] FAIL AH88 free memory',13,10,'$'
allocation_bad db '[XMSJEMM] FAIL allocate/lock/unlock/free/accounting',13,10,'$'
query_msg db '[XMSJEMM] AH08/AH88 query PASS',13,10,'$'
allocation_msg db '[XMSJEMM] Allocate/lock/unlock/free/accounting PASS',13,10,'$'
pass_msg db '[XMSJEMM] PASS',13,10,'$'
fail_msg db '[XMSJEMM] FAIL',13,10,'$'
align 16
times 512 db 0
stack_top:
program_end:
