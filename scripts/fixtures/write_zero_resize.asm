; Exercise DOS AH=40h/CX=0 using actual handles, seek, read and copy calls.
; The host supplies known data and independently checks FAT chains afterwards.
bits 16
org 100h
start:
    cli
    mov ax, cs
    mov ss, ax
    mov sp, stack_top
    sti
    mov ds, ax
    mov es, ax
    mov bx, (image_end - $$ + 100h + 15) >> 4
    mov ah, 4ah
    int 21h
    jc fail
    mov si, cases
.case:
    cmp word [si], 0
    je complete
    mov [current_case], si
    mov dx, [si]
    mov ax, 3d02h
    int 21h
    jc fail
    mov [source_handle], ax
    mov bx, ax
    mov si, [current_case]
    mov dx, [si+4]
    mov cx, [si+6]
    mov ax, 4200h
    int 21h
    jc fail
    mov dx, 0a55ah
    xor cx, cx
    mov ah, 40h
    int 21h
%ifdef FULL_VOLUME
    jnc fail
    cmp ax, 5
    jne fail
%else
    jc fail
    test ax, ax
    jnz fail
%endif
    cmp dx, 0a55ah
    jne fail
    test cx, cx
    jnz fail
    ; The zero-count write must retain the current file position.
    mov ax, 4201h
    xor cx, cx
    xor dx, dx
    int 21h
    jc fail
    mov si, [current_case]
    cmp ax, [si+4]
    jne fail
    cmp dx, [si+6]
    jne fail
    mov ah, 3eh
    int 21h
    jc fail
    mov dx, [si]
    mov ax, 3d00h
    int 21h
    jc fail
    mov [source_handle], ax
%ifndef FULL_VOLUME
    mov dx, [si+2]
    xor cx, cx
    mov ah, 3ch
    int 21h
    jc fail
    mov [dest_handle], ax
%endif
    mov dword [total], 0
.read:
    mov bx, [source_handle]
    mov dx, buffer
    mov cx, 1024
    mov ah, 3fh
    int 21h
    jc fail
    test ax, ax
    jz .eof
    movzx eax, ax
    add [total], eax
%ifndef FULL_VOLUME
    mov cx, ax
    mov [chunk], ax
    mov bx, [dest_handle]
    mov dx, buffer
    mov ah, 40h
    int 21h
    jc fail
    cmp ax, [chunk]
    jne fail
%endif
    jmp .read
.eof:
    mov si, [current_case]
    mov eax, [total]
%ifdef FULL_VOLUME
    cmp eax, 9468
%else
    cmp eax, [si+4]
%endif
    jne fail
    mov bx, [source_handle]
    mov ah, 3eh
    int 21h
    jc fail
%ifndef FULL_VOLUME
    mov bx, [dest_handle]
    mov ah, 3eh
    int 21h
    jc fail
%endif
    mov dx, case_ok
    mov ah, 9
    int 21h
    mov si, [current_case]
    add si, 8
    jmp .case
complete:
    mov dx, all_ok
    mov ah, 9
    int 21h
    mov ax, 4c00h
    int 21h
fail:
    mov dx, error
    mov ah, 9
    int 21h
    mov ax, 4c01h
    int 21h
cases:
%ifdef FULL_VOLUME
    dw name_full, 0
    dd 70013
%else
    dw name_ext1, copy_ext1
    dd 16124
    dw name_ext2, copy_ext2
    dd 70013
    dw name_shrink, copy_shrink
    dd 4097
    dw name_boundary, copy_boundary
    dd 8192
    dw name_zero, copy_zero
    dd 0
    dw name_from, copy_from
    dd 12345
%endif
    dw 0
name_ext1 db 'CXEXT1.BIN', 0
name_ext2 db 'CXEXT2.BIN', 0
name_shrink db 'CXSHRK.BIN', 0
name_boundary db 'CXBND.BIN', 0
name_zero db 'CXZERO.BIN', 0
name_from db 'CXFROM.BIN', 0
name_full db 'CXFULL.BIN', 0
copy_ext1 db 'CXEXT1.CPY', 0
copy_ext2 db 'CXEXT2.CPY', 0
copy_shrink db 'CXSHRK.CPY', 0
copy_boundary db 'CXBND.CPY', 0
copy_zero db 'CXZERO.CPY', 0
copy_from db 'CXFROM.CPY', 0
case_ok db '[CX0] CASE PASS', 13, 10, '$'
all_ok db '[CX0] ALL PASS', 13, 10, '$'
error db '[CX0] FAIL', 13, 10, '$'
source_handle dw 0
dest_handle dw 0
current_case dw 0
chunk dw 0
total dd 0
buffer times 1024 db 0
stack times 2048 db 0
stack_top:
image_end:
