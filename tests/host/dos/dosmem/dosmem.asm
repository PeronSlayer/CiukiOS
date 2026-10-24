; DOSMEM.COM - DOS Box memory map probe (host test, tests/host/dos/dos_memory.py).
; Reports, on the VM record port (E9h) and the console, what a real-mode game
; sees when it is started from the DOS Box prompt: INT 12h, the largest free
; conventional block (INT 21h/48h BX=FFFFh), the whole MCB chain (owner, size,
; name), the DOS version/HMA flag (21h/3306h), the UMB link state (21h/5802h),
; XMS (2Fh/4300h, 4310h; functions 00h, 08h, 10h query) and LIM EMS (INT 67h
; EMMXXXX0, functions 40h-42h, 46h). It allocates nothing and ends with
; "DOSMEM PASS"; the host test checks the figures. Lines stay below the
; record channel's 16 lines per second and 80 characters.
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
    mov ah, 0x4A
    int 0x21
; ---- conventional ----
    int 0x12
    mov di, f_k12
    call hex_word
    mov bx, 0xFFFF
    mov ah, 0x48
    int 0x21
    mov ax, bx
    mov di, f_largest
    call hex_word
    mov ax, 0x3306
    xor bx, bx
    xor dx, dx
    int 0x21
    mov ax, bx
    xchg al, ah
    mov di, f_dos
    call hex_word
    mov al, dh
    mov di, f_flags
    call hex_byte
    mov ax, 0x5802
    int 0x21
    mov di, f_link
    call hex_byte
    mov si, l_conv
    call line
; ---- MCB chain: three blocks per line ----
    mov ah, 0x52
    int 0x21
    mov ax, [es:bx-2]
    mov [mcb], ax
    push cs
    pop es
    xor ax, ax
    mov [freesum], ax
    mov [count], al
    mov word [cursor], l_mcb + 7
.mcb_next:
    mov fs, [mcb]
    mov al, [fs:0]
    cmp al, 'M'
    je .mcb_ok
    cmp al, 'Z'
    jne .mcb_bad
.mcb_ok:
    mov di, [cursor]
    mov ax, fs
    call hex_word
    mov al, ':'
    stosb
    mov ax, [fs:1]
    call hex_word
    mov al, ':'
    stosb
    mov ax, [fs:3]
    call hex_word
    mov al, ':'
    stosb
    cmp word [fs:1], 0
    jne .mcb_used
    mov ax, [fs:3]
    add [freesum], ax
.mcb_used:
    mov si, 8
    mov cx, 8
.mcb_name:
    mov al, [fs:si]
    inc si
    cmp al, 0x21
    jb .mcb_dot
    cmp al, 0x7E
    jbe .mcb_char
.mcb_dot:
    mov al, '.'
.mcb_char:
    stosb
    loop .mcb_name
    mov al, ' '
    stosb
    mov [cursor], di
    inc byte [count]
    cmp byte [count], 3
    jb .mcb_step
    call mcb_flush
.mcb_step:
    mov ax, fs
    add ax, [fs:3]
    inc ax
    cmp byte [fs:0], 'Z'
    je .mcb_last
    mov [mcb], ax
    cmp ax, 0xA000
    jb .mcb_next
    ; the UMB link MCB at 9FFFh continues into upper memory
    cmp ax, 0xF000
    jb .mcb_next
.mcb_bad:
    mov byte [f_bad], 'B'
.mcb_last:
    cmp byte [count], 0
    je .mcb_done
    call mcb_flush
.mcb_done:
    mov ax, [freesum]
    mov di, f_free
    call hex_word
; ---- XMS ----
    mov ax, 0x4300
    int 0x2F
    mov di, f_2f
    call hex_byte
    cmp al, 0x80
    jne .xms_done
    push es
    mov ax, 0x4310
    int 0x2F
    mov [xms], bx
    mov [xms + 2], es
    pop es
    xor ah, ah
    call far [xms]
    mov di, f_xver
    call hex_word
    mov ax, dx
    mov di, f_xhma
    call hex_byte
    mov ah, 0x08
    xor bl, bl
    call far [xms]
    mov di, f_xlarge
    call hex_word
    mov ax, dx
    mov di, f_xtotal
    call hex_word
    mov ah, 0x10
    mov dx, 0xFFFF
    xor bx, bx
    call far [xms]
    push dx
    mov di, f_uax
    call hex_word
    mov al, bl
    mov di, f_ubl
    call hex_byte
    pop ax
    mov di, f_udx
    call hex_word
.xms_done:
; ---- EMS ----
    mov ax, 0x3567
    int 0x21
    mov di, 10
    mov si, emm_name
    mov cx, 8
    repe cmpsb
    push cs
    pop es
    jne .ems_done
    mov byte [f_ems], '1'
    mov ah, 0x40
    int 0x67
    mov al, ah
    mov di, f_est
    call hex_byte
    mov ah, 0x41
    int 0x67
    mov ax, bx
    mov di, f_frame
    call hex_word
    mov ah, 0x42
    int 0x67
    push dx
    mov ax, bx
    mov di, f_efree
    call hex_word
    pop ax
    mov di, f_etotal
    call hex_word
    mov ah, 0x46
    int 0x67
    mov di, f_ever
    call hex_byte
.ems_done:
    mov si, l_tail
    call line
    mov si, l_xms
    call line
    mov si, l_ems
    call line
    mov si, l_pass
    call line
    mov ax, 0x4C00
    int 0x21

mcb_flush:
    mov di, [cursor]
    xor al, al
    stosb
    mov si, l_mcb
    call line
    mov word [cursor], l_mcb + 7
    mov byte [count], 0
    ret

; DS:SI zero-terminated text: console (INT 21h/02h) and record port, then CR LF.
line:
    push ax
    push dx
    push si
.next:
    lodsb
    test al, al
    jz .end
    mov dl, al
    mov ah, 2
    int 0x21
    mov al, dl
    out 0xE9, al
    jmp .next
.end:
    mov dl, 13
    mov ah, 2
    int 0x21
    mov dl, 10
    mov ah, 2
    int 0x21
    mov al, 10
    out 0xE9, al
    pop si
    pop dx
    pop ax
    ret

hex_word:
    xchg al, ah
    call hex_byte
    xchg al, ah
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

emm_name db 'EMMXXXX0'
l_conv  db 'DOSMEM CONV k12='
f_k12   db '0000 largest='
f_largest db '0000 dos='
f_dos   db '0000 flags='
f_flags db '00 link='
f_link  db '00',0
l_tail  db 'DOSMEM MCB end'
f_bad   db ' free='
f_free  db '0000',0
l_xms   db 'DOSMEM XMS 2F='
f_2f    db '00 ver='
f_xver  db '0000 hma='
f_xhma  db '00 freeKB='
f_xlarge db '0000/'
f_xtotal db '0000 umb='
f_uax   db '0000/'
f_ubl   db '00/'
f_udx   db '0000',0
l_ems   db 'DOSMEM EMS '
f_ems   db '0 st='
f_est   db '00 frame='
f_frame db '0000 pages='
f_efree db '0000/'
f_etotal db '0000 ver='
f_ever  db '00',0
l_pass  db      'DOSMEM PASS',0
l_mcb   db      'DOSMEM '
        times 3*24+2 db 0
align 2
xms     dw 0, 0
mcb     dw 0
cursor  dw 0
freesum dw 0
count   db 0
align 16
stack times 512 db 0
stack_top:
image_end:
