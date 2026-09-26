; Real DOS open/read/close path; report every file byte through FNV-1a and length.
; A host test independently reads the FAT image and compares both values.
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
    xor bx, bx
    mov bl, [80h]
    mov byte [81h+bx], 0
    mov dx, 81h
.space:
    mov bx, dx
    cmp byte [bx], ' '
    jne .open
    inc dx
    jmp .space
.open:
    mov ax, 3d00h
    int 21h
    jc fail
    mov [handle], ax
.read:
    mov bx, [handle]
    mov dx, buffer
    mov cx, 4096
    mov ah, 3fh
    int 21h
    jc fail
    test ax, ax
    jz .done
    movzx eax, ax
    add [total], eax
    mov cx, ax
    mov si, buffer
    mov ebp, [hash]
.hash:
    movzx eax, byte [si]
    inc si
    xor ebp, eax
    imul ebp, ebp, 16777619
    loop .hash
    mov [hash], ebp
    jmp .read
.done:
    mov bx, [handle]
    mov ah, 3eh
    int 21h
    jc fail
    mov dx, result
    mov ah, 9
    int 21h
    mov eax, [hash]
    call hex32
    mov dx, size_message
    mov ah, 9
    int 21h
    mov eax, [total]
    call hex32
    mov dx, newline
    mov ah, 9
    int 21h
    mov ax, 4c00h
    int 21h
fail:
    movzx eax, ax
    mov [hex_value], eax
    mov dx, error
    mov ah, 9
    int 21h
    mov eax, [hex_value]
    call hex32
    mov ax, 4c01h
    int 21h
hex32:
    mov [hex_value], eax
    mov byte [digits], 8
.digit:
    rol dword [hex_value], 4
    mov bx, [hex_value]
    and bx, 15
    mov dl, [hex_chars+bx]
    mov ah, 2
    int 21h
    dec byte [digits]
    jnz .digit
    ret
result db '[FILECHECK] FNV=', '$'
size_message db ' BYTES=', '$'
newline db 13, 10, '$'
error db '[FILECHECK] ERROR=', '$'
hex_chars db '0123456789ABCDEF'
digits db 0
handle dw 0
hash dd 0811c9dc5h
total dd 0
hex_value dd 0
buffer times 4096 db 0
stack times 2048 db 0
stack_top:
image_end:
