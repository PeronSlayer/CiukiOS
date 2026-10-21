; H56 FCB probe (H50-03 cases on the FreeDOS FCB engine). Prints, for every
; step, AL, CX, the sequential and random record fields, file size, record
; size and the 59h error, so the pristine rebuild and the fork can be
; compared byte for byte. Assertions live in tests/host/dos/unified_binding.py.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
cpu 386
org 0x100

%macro STEP 1
    mov word [step], %1
%endmacro

start:
    mov sp, stack_top
    mov bx, (image_end - $$ + 0x100 + 15) >> 4
    mov ah, 0x4a
    int 0x21
    mov dx, dta
    mov ah, 0x1a
    int 0x21
    mov si, name_fcb
    mov di, fcb
    call fcb_init
    ; 1 create keeps curec/random
    STEP 1
    mov byte [fcb+32], 7
    mov dword [fcb+33], 0x12345678
    mov ah, 0x16
    call fcb_call
    ; 2 sequential write at record 127 rolls over to block 1
    STEP 2
    mov di, dta
    mov al, 'F'
    mov cx, 512
    rep stosb
    mov byte [fcb+32], 127
    mov word [fcb+12], 0
    mov ah, 0x15
    call fcb_call
    ; 3 block write of 255 one-byte records
    STEP 3
    mov word [fcb+14], 1
    mov dword [fcb+33], 0
    mov cx, 255
    mov ah, 0x28
    call fcb_call
    ; 4 set random record from the sequential position
    STEP 4
    mov ah, 0x24
    call fcb_call
    ; 5 block write of zero records truncates at the random record
    STEP 5
    mov dword [fcb+33], 5
    xor cx, cx
    mov ah, 0x28
    call fcb_call
    ; 6 random read with a 24-bit record (high byte ignored), partial pad
    STEP 6
    mov word [fcb+14], 128
    mov dword [fcb+33], 0x5a000000
    mov byte [fcb+32], 42
    mov di, dta
    mov al, 0xa7
    mov cx, 130
    rep stosb
    mov ah, 0x21
    call fcb_call
    call dump_dta
    ; 7 random read beyond EOF
    STEP 7
    mov dword [fcb+33], 1
    mov ah, 0x21
    call fcb_call
    ; 8 sequential partial read
    STEP 8
    mov word [fcb+12], 0
    mov byte [fcb+32], 0
    mov ah, 0x14
    call fcb_call
    ; 9 block read of 3 with one partial record
    STEP 9
    mov dword [fcb+33], 0
    mov cx, 3
    mov ah, 0x27
    call fcb_call
    ; 10 record size zero
    STEP 10
    mov word [fcb+14], 0
    mov ah, 0x14
    call fcb_call
    ; 11 current record 128 is not a valid position
    STEP 11
    mov word [fcb+14], 128
    mov byte [fcb+32], 128
    mov word [fcb+12], 0
    mov ah, 0x14
    call fcb_call
    mov byte [fcb+32], 0
    ; 12 DTA segment wrap
    STEP 12
    mov dx, 0xfff0
    mov ah, 0x1a
    int 0x21
    mov ah, 0x14
    call fcb_call
    mov dx, dta
    mov ah, 0x1a
    int 0x21
    ; 13 block transfer larger than 64 KiB
    STEP 13
    mov word [fcb+14], 65535
    mov cx, 2
    mov ah, 0x27
    call fcb_call
    ; 14 32-bit random record overflows the position (record size < 64)
    STEP 14
    mov word [fcb+14], 63
    mov dword [fcb+33], 0xffffffff
    mov ah, 0x22
    call fcb_call
    ; 15 close, close again
    STEP 15
    mov word [fcb+14], 128
    mov ah, 0x10
    call fcb_call
    STEP 16
    mov ah, 0x10
    call fcb_call
    ; 17 reopen, 18 file size with record size 3
    STEP 17
    mov ah, 0x0f
    call fcb_call
    STEP 18
    mov word [fcb+14], 3
    mov ah, 0x23
    call fcb_call
    ; 19 a forged SFT index in the reserved bytes
    STEP 19
    mov al, [fcb+24]
    mov [saved_sft], al
    mov byte [fcb+24], 0x7f
    mov ah, 0x14
    call fcb_call
    STEP 20
    mov ah, 0x10
    call fcb_call
    ; 35 the index of a handle-opened SFT entry (CON, handle 1)
    mov bx, 1
    mov ax, 0x1220
    int 0x2f
    mov al, [es:di]
    push cs
    pop es
    mov [fcb+24], al
    STEP 35
    mov ah, 0x15
    call fcb_call
    mov al, [saved_sft]
    mov [fcb+24], al
    ; 21 extended FCB create with hidden attribute
    STEP 21
    mov si, name_second
    mov di, xfcb+7
    call fcb_init
    mov byte [xfcb], 0xff
    mov byte [xfcb+6], 2
    mov dx, xfcb
    mov ah, 0x16
    call fcb_call_dx
    ; 22 renamed-in-place FCB used for I/O
    STEP 22
    mov byte [xfcb+7+1], 'X'
    mov dx, xfcb
    mov ah, 0x15
    call fcb_call_dx
    mov byte [xfcb+7+1], 'S'
    STEP 23
    mov dx, xfcb
    mov ah, 0x10
    call fcb_call_dx
    STEP 24
    mov word [fcb+14], 128
    mov ah, 0x10
    call fcb_call
    ; 25 wildcard search with an extended FCB and guarded DTA
    STEP 25
    mov si, name_wild
    mov di, xfcb+7
    call fcb_init
    mov byte [xfcb], 0xff
    mov byte [xfcb+6], 2
    mov di, dta-1
    mov al, 0xa7
    mov cx, 46
    rep stosb
    mov dx, xfcb
    mov ah, 0x11
    call fcb_call_dx
    call dump_find
    mov word [found], 1
.next:
    mov dx, xfcb
    mov ah, 0x12
    int 0x21
    or al, al
    jnz .done
    inc word [found]
    cmp word [found], 9
    jb .next
.done:
    STEP 26
    mov ax, [found]
    mov [last_al], ax
    call dump_value
    ; 27 rename by pattern, 28 open the new name, 29 delete by wildcard
    STEP 27
    mov si, name_fcb
    mov di, fcb
    call fcb_init
    mov si, name_new_pattern
    mov di, fcb+17
    mov cx, 11
    rep movsb
    mov ah, 0x17
    call fcb_call
    STEP 28
    mov si, name_new
    mov di, fcb
    call fcb_init
    mov ah, 0x0f
    call fcb_call
    mov ah, 0x10
    call fcb_call
    STEP 29
    mov si, name_wild
    mov di, xfcb+7
    call fcb_init
    mov byte [xfcb], 0xff
    mov byte [xfcb+6], 2
    mov dx, xfcb
    mov ah, 0x13
    call fcb_call_dx
    STEP 30
    mov dx, xfcb
    mov ah, 0x11
    call fcb_call_dx
    ; 31 invalid drive
    STEP 31
    mov si, name_bad
    mov di, fcb
    call fcb_init
    mov byte [fcb], 27
    mov ah, 0x0f
    call fcb_call
    ; 32-34 parse filename (29h)
    STEP 32
    mov di, parsed
    mov al, 0xa7
    mov cx, 37
    rep stosb
    mov si, parse1
    mov di, parsed
    mov ax, 0x2901
    int 0x21
    call dump_parse
    STEP 33
    mov si, parse2
    mov di, parsed
    mov ax, 0x290e
    int 0x21
    call dump_parse
    STEP 34
    mov si, parse3
    mov di, parsed
    mov ax, 0x2900
    int 0x21
    call dump_parse
    ; 36 the FCB size field becomes the entry size on close (local FAT)
    STEP 36
    mov si, name_meta
    mov di, fcb
    call fcb_init
    mov ah, 0x16
    call fcb_call
    mov ah, 0x15
    call fcb_call
    mov dword [fcb+16], 5
    mov ah, 0x10
    call fcb_call
    mov dx, path_meta
    call handle_size
    ; 37 a concurrent handle extension is overridden by the FCB close
    STEP 37
    mov ah, 0x0f
    call fcb_call
    mov dx, path_meta
    mov ax, 0x3d02
    int 0x21
    mov bx, ax
    mov ax, 0x4202
    xor cx, cx
    xor dx, dx
    int 0x21
    mov ah, 0x40
    mov dx, dta
    mov cx, 10
    int 0x21
    mov ah, 0x3e
    int 0x21
    mov dx, path_meta
    call handle_size
    mov ah, 0x10
    call fcb_call
    mov dx, path_meta
    call handle_size
    ; 38 a read-only file cannot be opened for FCB read/write
    STEP 38
    mov dx, path_ro
    mov cx, 1
    mov ah, 0x3c
    int 0x21
    mov bx, ax
    mov ah, 0x3e
    int 0x21
    mov si, name_ro
    mov di, fcb
    call fcb_init
    mov ah, 0x0f
    call fcb_call
    mov dx, path_ro
    mov ax, 0x4301
    xor cx, cx
    int 0x21
    mov dx, path_ro
    mov ah, 0x41
    int 0x21
    mov dx, path_meta
    mov ah, 0x41
    int 0x21
    mov si, msg_end
    call puts
    cmp byte [0x82], 'C'            ; " C" tail: run as a child, return
    je .child
    xor al, al
    out 0xea, al
.child:
    mov ax, 0x4c00
    int 0x21

; DX = path: print "FCB SIZE nn=<size>" via handle open/seek
handle_size:
    mov ax, 0x3d00
    int 0x21
    jc .bad
    mov bx, ax
    mov ax, 0x4202
    xor cx, cx
    xor dx, dx
    int 0x21
    push ax
    push dx
    mov ah, 0x3e
    int 0x21
    mov si, msg_size
    call puts
    mov ax, [step]
    call hex
    mov al, '='
    out 0xe9, al
    pop ax
    call hex
    pop ax
    call hex
    mov si, crlf
    jmp puts
.bad:
    mov si, msg_size
    call puts
    mov ax, 0xeeee
    call hex
    mov si, crlf
    jmp puts
; SI = 11-byte name, DI = 37-byte FCB body: drive C:, zeroed fields
fcb_init:
    push di
    push cx
    mov cx, 37
    xor al, al
    rep stosb
    pop cx
    pop di
    mov byte [di], 3
    push di
    inc di
    mov cx, 11
    rep movsb
    pop di
    ret
fcb_call:
    mov dx, fcb
fcb_call_dx:
    mov [cur_fcb], dx
    int 0x21
    mov [last_al], ax
    mov [last_cx], cx
    pushf
    pop word [last_flags]
    push ds
    push es
    push si
    push di
    mov ax, 0x5900                  ; destroys CL, DI, DS, ES, SI (RBIL)
    xor bx, bx
    int 0x21
    pop di
    pop si
    pop es
    pop ds
    mov [last_err], ax
    jmp dump

; "FCB nn al=.. cx=.... blk=.... rec=.. rnd=........ siz=........ rsz=.... e=...."
dump:
    mov si, msg_fcb
    call puts
    mov ax, [step]
    call hex
    mov si, msg_al
    call puts
    mov ax, [last_al]
    call hex2
    mov si, msg_cx
    call puts
    mov ax, [last_cx]
    call hex
    mov bx, [cur_fcb]
    cmp byte [bx], 0xff
    jne .body
    add bx, 7
.body:
    mov si, msg_blk
    call puts
    mov ax, [bx+12]
    call hex
    mov si, msg_rec
    call puts
    mov al, [bx+32]
    call hex2
    mov si, msg_rnd
    call puts
    mov ax, [bx+35]
    call hex
    mov ax, [bx+33]
    call hex
    mov si, msg_siz
    call puts
    mov ax, [bx+18]
    call hex
    mov ax, [bx+16]
    call hex
    mov si, msg_rsz
    call puts
    mov ax, [bx+14]
    call hex
    mov si, msg_err
    call puts
    mov ax, [last_err]
    call hex
    mov si, crlf
    call puts
    ret
dump_value:
    mov si, msg_fcb
    call puts
    mov ax, [step]
    call hex
    mov si, msg_val
    call puts
    mov ax, [last_al]
    call hex
    mov si, crlf
    jmp puts
; DTA[0..7], DTA[127..129] and the guard before it
dump_dta:
    mov si, msg_dta
    call puts
    mov al, [dta-1]
    call hex2
    mov cx, 8
    mov si, dta
    call bytes
    mov al, ' '
    out 0xe9, al
    mov cx, 3
    mov si, dta+127
    call bytes
    mov si, crlf
    jmp puts
; find result: guards, extended prefix, drive and name
dump_find:
    mov si, msg_find
    call puts
    mov al, [dta-1]
    call hex2
    mov cx, 20
    mov si, dta
    call bytes
    mov al, ' '
    out 0xe9, al
    mov al, [dta+44]
    call hex2
    mov si, crlf
    jmp puts
dump_parse:
    push si
    mov [last_al], ax
    mov si, msg_parse
    call puts
    mov ax, [step]
    call hex
    mov si, msg_al
    call puts
    mov ax, [last_al]
    call hex2
    mov si, msg_si
    call puts
    pop ax
    push ax
    call hex
    mov al, ' '
    out 0xe9, al
    mov cx, 16
    mov si, parsed
    call bytes
    mov al, ' '
    out 0xe9, al
    mov al, [parsed+32]
    call hex2
    mov si, crlf
    call puts
    pop si
    ret
bytes:
    lodsb
    call hex2
    loop bytes
    ret
puts:
    lodsb
    test al, al
    jz .done
    out 0xe9, al
    jmp puts
.done:
    ret
hex:
    push cx
    mov cx, 4
.d:
    rol ax, 4
    push ax
    and al, 15
    add al, '0'
    cmp al, '9'
    jbe .e
    add al, 7
.e:
    out 0xe9, al
    pop ax
    loop .d
    pop cx
    ret
hex2:
    push cx
    push ax
    mov cx, 2
    shl ax, 8
.d:
    rol ax, 4
    push ax
    and al, 15
    add al, '0'
    cmp al, '9'
    jbe .e
    add al, 7
.e:
    out 0xe9, al
    pop ax
    loop .d
    pop ax
    pop cx
    ret

msg_fcb db 'FCB ', 0
msg_al db ' al=', 0
msg_cx db ' cx=', 0
msg_blk db ' blk=', 0
msg_rec db ' rec=', 0
msg_rnd db ' rnd=', 0
msg_siz db ' siz=', 0
msg_rsz db ' rsz=', 0
msg_err db ' e=', 0
msg_val db ' value=', 0
msg_dta db 'FCB DTA ', 0
msg_find db 'FCB FIND ', 0
msg_parse db 'FCB PARSE ', 0
msg_si db ' si=', 0
msg_end db 'FCB END', 13, 10, 0
crlf db 13, 10, 0
name_fcb db 'FCB     DAT'
name_second db 'SECOND  DAT'
name_wild db '????????DAT'
name_new_pattern db 'NEW     ???'
name_new db 'NEW     DAT'
name_bad db 'BAD     DAT'
name_meta db 'META    DAT'
name_ro db 'RO      DAT'
path_meta db 'C:\META.DAT', 0
path_ro db 'C:\RO.DAT', 0
msg_size db 'FCB SIZE ', 0
parse1 db ' ; c:ab*.', 0x81, '? rest', 0
parse2 db 0
parse3 db 'Z:a', 0
align 2
step dw 0
last_al dw 0
last_cx dw 0
last_flags dw 0
last_err dw 0
cur_fcb dw 0
found dw 0
saved_sft db 0
fcb times 37 db 0
xfcb times 44 db 0
parsed times 40 db 0
    db 0xa7
dta times 512 db 0
    times 16 db 0
    times 768 db 0
stack_top:
image_end:
