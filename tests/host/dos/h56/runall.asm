; H56 battery runner: for every "TAG FILE.COM" line of C:\D48\LIST.TXT,
; change to C:\D48TEST\TAG and EXEC C:\D48\FILE.COM (the unchanged f3-48
; probes print their own results to port E9h). Ends the run on port EAh.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
cpu 386
org 0x100
start:
    mov sp, stack_top
    mov bx, (image_end - $$ + 0x100 + 15) >> 4
    mov ah, 0x4a
    int 0x21
    mov dx, list_name
    mov ax, 0x3d00
    int 0x21
    jc fail
    mov bx, ax
    mov ah, 0x3f
    mov cx, 4000
    mov dx, list
    int 0x21
    jc fail
    mov [list_len], ax
    mov ah, 0x3e
    int 0x21
    mov si, list
line:
    mov ax, si
    sub ax, list
    cmp ax, [list_len]
    jae done
    mov di, dir_tail
.tag:
    lodsb
    cmp al, ' '
    je .tag_end
    stosb
    jmp .tag
.tag_end:
    xor al, al
    stosb
    mov di, prog_tail
.file:
    lodsb
    cmp al, 13
    je .file_end
    stosb
    jmp .file
.file_end:
    xor al, al
    stosb
    lodsb                           ; LF
    push si
    mov dx, dir_path
    mov ah, 0x3b
    int 0x21
    jc .cd_fail
    mov word [block], 0
    mov word [block+2], empty_tail
    mov [block+4], cs
    mov word [block+6], 0x5c
    mov [block+8], cs
    mov word [block+10], 0x6c
    mov [block+12], cs
    push cs
    pop es
    mov [save_sp], sp
    mov dx, prog_path
    mov bx, block
    mov ax, 0x4b00
    int 0x21
    cli
    mov bx, cs
    mov ss, bx
    mov sp, [cs:save_sp]
    sti
    push cs
    pop ds
    push cs
    pop es
    jnc .next
    mov si, msg_exec
    call puts
    mov si, dir_tail
    call puts
    mov si, crlf
    call puts
.next:
    pop si
    jmp line
.cd_fail:
    mov si, msg_cd
    call puts
    mov si, dir_tail
    call puts
    mov si, crlf
    call puts
    pop si
    jmp line
fail:
    mov si, msg_list
    call puts
done:
    mov si, msg_end
    call puts
    xor al, al
    out 0xea, al
.halt:
    jmp .halt
puts:
    lodsb
    test al, al
    jz .done
    out 0xe9, al
    jmp puts
.done:
    ret
list_name db 'C:\D48\LIST.TXT', 0
dir_path db 'C:\D48TEST\'
dir_tail times 16 db 0
prog_path db 'C:\D48\'
prog_tail times 16 db 0
empty_tail db 0, 13
msg_exec db 'RUNALL EXECFAIL ', 0
msg_cd db 'RUNALL CDFAIL ', 0
msg_list db 'RUNALL LISTFAIL', 13, 10, 0
msg_end db 'RUNALL END', 13, 10, 0
crlf db 13, 10, 0
align 2
list_len dw 0
save_sp dw 0
block times 14 db 0
list times 4000 db 0
    times 512 db 0
stack_top:
image_end:
