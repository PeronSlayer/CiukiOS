; f3-06 workload launcher RUNREC.COM: "RUNREC <dir> <program> [arguments]".
; Changes to <dir>, EXECs <dir>\<program> with the arguments as its command
; tail (INT 21h/4B00h, inherited environment), reads the child's return code
; with INT 21h/4Dh, returns to the root directory and reports one line on the
; VM record port (and the screen):
;   RUNREC PASS                 child exit code 0, normal termination
;   RUNREC FAIL Rcc Ttt         code cc, termination type tt (hex)
;   RUNREC FAIL Ennnn           EXEC failed with DOS error nnnn
;   RUNREC FAIL Dnnnn           CHDIR failed with DOS error nnnn
;   RUNREC FAIL U               missing or oversized operands
; The kernel's verdict latch (dosvm_expect "RUNREC") keeps "Rcc_Ttt" as the
; detail. DOS 3.3 EXEC parameter block, PSP command tail at 80h and AH=4Dh:
; Microsoft MS-DOS Programmer's Reference (INT 21h functions 3Bh, 4Ah, 4Bh,
; 4Ch, 4Dh); the child is the fourth CIUKIDOS EXEC frame (f3-04 note).
; SPDX-License-Identifier: GPL-2.0-only
bits 16
org 0x100
start:
    cli
    mov ax, cs
    mov ss, ax
    mov sp, stack_top
    sti
    mov ds, ax
    mov es, ax
    cld
    mov bx, (image_end - $$ + 0x100 + 15) >> 4
    mov ah, 0x4A                  ; release everything above this image
    int 0x21
    jc usage
    mov si, 0x81
    xor ch, ch
    mov cl, [0x80]
    cmp cl, 0x7E
    ja usage
    mov bx, si
    add bx, cx
    mov byte [bx], 0              ; the tail ends here (CR replaced)
    call skip_spaces
    mov di, dir_path
    mov dx, dir_path + 63
    call copy_token               ; directory, e.g. \DOOM
    jc usage
    cmp di, dir_path
    je usage
    mov byte [di], 0
    ; program path = directory + '\' + program
    mov si, dir_path
    mov di, program_path
.copy_dir:
    lodsb
    test al, al
    jz .dir_done
    stosb
    jmp .copy_dir
.dir_done:
    cmp byte [di - 1], '\'
    je .separator_done
    mov al, '\'
    stosb
.separator_done:
    mov si, [cursor]
    call skip_spaces
    mov bx, di
    mov dx, program_path + 95
    call copy_token               ; program file name
    jc usage
    cmp di, bx
    je usage
    mov byte [di], 0
    ; command tail: the rest, with its leading space, length-prefixed, CR
    mov si, [cursor]
    mov di, tail + 1
    xor cx, cx
.tail:
    lodsb
    test al, al
    jz .tail_done
    cmp cx, 125
    jae usage
    stosb
    inc cx
    jmp .tail
.tail_done:
    mov al, 13
    stosb
    mov [tail], cl
    mov [block_tail + 2], cs
    mov [block_fcb1 + 2], cs
    mov [block_fcb2 + 2], cs
    mov dx, dir_path
    mov ah, 0x3B
    int 0x21
    jc chdir_failed
    mov [saved_sp], sp
    mov dx, program_path
    mov bx, exec_block
    mov ax, 0x4B00
    int 0x21
    cli                           ; EXEC may return with any SS:SP
    mov bx, cs
    mov ss, bx
    mov sp, [cs:saved_sp]
    sti
    mov ds, bx
    mov es, bx
    cld
    jc exec_failed
    mov ah, 0x4D
    int 0x21
    mov [child_status], ax
    call to_root
    mov ax, [child_status]
    test ax, ax
    jnz child_failed
    mov dx, msg_pass
    jmp report
child_failed:
    mov di, msg_child + 13
    call hex_byte                 ; AL: exit code
    mov di, msg_child + 17
    mov al, ah
    call hex_byte                 ; AH: termination type
    mov dx, msg_child
    jmp report
exec_failed:
    mov [child_status], ax
    call to_root
    mov ax, [child_status]
    mov dx, msg_exec
    jmp error_word
chdir_failed:
    mov dx, msg_chdir
error_word:                       ; DX = message, AX = DOS error
    mov di, dx
    add di, 13
    push ax
    mov al, ah
    call hex_byte
    pop ax
    call hex_byte
    jmp report
usage:
    mov dx, msg_usage
report:
    call record_dollar
    mov ax, 0x4C00
    int 0x21

to_root:
    mov dx, root_path
    mov ah, 0x3B
    int 0x21
    ret
; DS:SI -> first non-space byte; also kept in [cursor].
skip_spaces:
    lodsb
    cmp al, ' '
    je skip_spaces
    cmp al, 9
    je skip_spaces
    dec si
    mov [cursor], si
    ret
; Copy [cursor].. to DI until space/tab/NUL; DX = last usable byte.
; CF set when the token does not fit. Updates [cursor].
copy_token:
    mov si, [cursor]
.next:
    lodsb
    test al, al
    jz .end
    cmp al, ' '
    je .end
    cmp al, 9
    je .end
    cmp di, dx
    jae .long
    stosb
    jmp .next
.end:
    dec si
    mov [cursor], si
    clc
    ret
.long:
    stc
    ret
hex_byte:
    push ax
    push cx
    mov ah, al
    mov cl, 4
    shr al, cl
    call .digit
    mov al, ah
    and al, 15
    call .digit
    pop cx
    pop ax
    ret
.digit:
    add al, '0'
    cmp al, '9'
    jbe .store
    add al, 7
.store:
    stosb
    ret

%include "src/probes/doscompat/record.inc"
msg_pass  db 'RUNREC PASS', 13, 10, '$'
msg_child db 'RUNREC FAIL R00 T00', 13, 10, '$'
msg_exec  db 'RUNREC FAIL E0000', 13, 10, '$'
msg_chdir db 'RUNREC FAIL D0000', 13, 10, '$'
msg_usage db 'RUNREC FAIL U', 13, 10, '$'
root_path db '\', 0
exec_block:
    dw 0                          ; inherit the environment
block_tail dw tail, 0
block_fcb1 dw 0x5C, 0
block_fcb2 dw 0x6C, 0
cursor dw 0
saved_sp dw 0
child_status dw 0
dir_path times 64 db 0
program_path times 96 db 0
tail times 128 db 0
align 16
stack times 512 db 0
stack_top:
image_end:
