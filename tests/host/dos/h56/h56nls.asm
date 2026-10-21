; H56 NLS probe (H50-04 cases): 65h subfunctions, lengths, guards, 38h,
; 66h and the agreement of 65h/20-22, A0, 29h and the snapshot tables.
; Prints values; assertions live in tests/host/dos/unified_binding.py.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
cpu 386
org 0x100

start:
    mov sp, stack_top              ; keeps its whole 64 KiB block: DS:FFFFh is ours
    ; 1: subfunctions 1-7 with CX=41, guarded buffer
    mov byte [subfn], 1
.sub:
    call fill
    mov al, [subfn]
    mov ah, 0x65
    mov bx, 437
    mov dx, 1
    mov cx, 41
    mov di, buf+1
    int 0x21
    call result
    mov si, msg_bytes
    call puts
    mov si, buf
    mov cx, 8
    call bytes
    mov al, ' '
    out 0xe9, al
    mov bx, [last_cx]
    mov al, [buf+1+bx]
    call hex2
    cmp byte [subfn], 1
    je .no_table
    mov al, ' '
    out 0xe9, al
    les si, [buf+2]
    mov cx, 4
    call bytes_es
    push cs
    pop es
.no_table:
    mov si, crlf
    call puts
    ; the same with CX=4, an unsupported page, country and a wrapping DI
    mov al, [subfn]
    mov ah, 0x65
    mov bx, 437
    mov dx, 1
    mov cx, 4
    mov di, buf+1
    int 0x21
    call result_line
    mov al, [subfn]
    mov ah, 0x65
    mov bx, 850
    mov dx, 1
    mov cx, 41
    mov di, buf+1
    int 0x21
    call result_line
    mov al, [subfn]
    mov ah, 0x65
    mov bx, 437
    mov dx, 39
    mov cx, 41
    mov di, buf+1
    int 0x21
    call result_line
    cmp byte [0x82], 'P'            ; pristine comparison: upstream faults here
    je .skip_wrap
    mov al, [subfn]
    mov ah, 0x65
    mov bx, 0xffff
    mov dx, 0xffff
    mov cx, 41
    mov di, 0xfffd
    int 0x21
    call result_line
.skip_wrap:
    inc byte [subfn]
    cmp byte [subfn], 8
    jb .sub
    ; 2: 65h/01 lengths 5..45: returned CX and the guard after it
    mov si, msg_len
    call puts
    mov byte [len], 3
.len:
    call fill
    mov ax, 0x6501
    mov bx, 437
    mov dx, 1
    xor ch, ch
    mov cl, [len]
    mov di, buf+1
    int 0x21
    jnc .ok
    mov al, 'E'
    out 0xe9, al
    jmp .next_len
.ok:
    mov ax, cx
    call hex2
    mov bx, cx
    mov al, [buf+1+bx]
    call hex2
.next_len:
    mov al, ' '
    out 0xe9, al
    inc byte [len]
    cmp byte [len], 46
    jb .len
    mov si, crlf
    call puts
    ; 3: 65h/20 for every byte (DH preserved) and 65h/A0
    mov si, msg_upper
    call puts
    xor cx, cx
.up:
    mov dl, cl
    mov dh, 0x5a
    mov ax, 0x6520
    push cx
    int 0x21
    pop cx
    cmp dh, 0x5a
    je .dh_ok
    mov byte [bad_dh], 1
.dh_ok:
    mov bx, cx
    mov [upper+bx], dl
    mov dl, cl
    mov ax, 0x65a0
    push cx
    int 0x21
    pop cx
    mov bx, cx
    mov [fupper+bx], dl
    mov al, dl
    cmp cx, 0x80
    jb .up_next
    call hex2
.up_next:
    inc cx
    cmp cx, 256
    jb .up
    mov si, msg_dh
    call puts
    mov al, [bad_dh]
    call hex2
    mov si, crlf
    call puts
    ; 4: 65h/21 over 0..255 agrees with 65h/20; 65h/A1 with A0
    xor bx, bx
.mk:
    mov [mem+bx], bl
    mov [fmem+bx], bl
    inc bx
    cmp bx, 256
    jb .mk
    mov ax, 0x6521
    mov dx, mem
    mov cx, 256
    int 0x21
    mov ax, 0x65a1
    mov dx, fmem
    mov cx, 256
    int 0x21
    mov si, mem
    mov di, upper
    mov cx, 256
    repe cmpsb
    setz [agree21]
    mov si, fmem
    mov di, fupper
    mov cx, 256
    repe cmpsb
    setz [agreea1]
    mov si, msg_agree
    call puts
    mov al, [agree21]
    call hex2
    mov al, [agreea1]
    call hex2
    mov si, crlf
    call puts
    ; 5: 65h/21 with CX=0 and CX=2 at DX=FFFFh; 65h/22 and A2
    mov ax, 0x6521
    mov dx, 0xffff
    xor cx, cx
    int 0x21
    call result_line
    mov ax, 0x6521
    mov dx, 0xffff
    mov cx, 2
    int 0x21
    call result_line
    mov ax, 0x6522
    mov dx, str1
    int 0x21
    call result
    mov si, msg_str
    call puts
    mov si, str1
    mov cx, 9
    call bytes
    mov si, crlf
    call puts
    mov byte [0xffff], 'x'
    mov ax, 0x6522
    mov dx, 0xffff
    int 0x21
    call result_line
    mov ax, 0x65a2
    mov dx, 0xffff
    int 0x21
    call result_line
    ; 6: 65h/23 yes/no
    mov si, msg_yn
    call puts
    mov si, yn
.yn:
    lodsb
    or al, al
    jz .yn_done
    mov dl, al
    mov ax, 0x6523
    push si
    int 0x21
    pop si
    call hex
    mov al, ' '
    out 0xe9, al
    jmp .yn
.yn_done:
    mov si, crlf
    call puts
    ; 7: 66h and 38h
    mov ax, 0x6601
    int 0x21
    call result
    mov si, msg_bxdx
    call puts
    mov ax, bx
    call hex
    mov ax, dx
    call hex
    mov si, crlf
    call puts
    mov ax, 0x6602
    mov bx, 437
    mov dx, 437
    int 0x21
    call result_line
    mov ax, 0x6602
    mov bx, 850
    mov dx, 850
    int 0x21
    call result_line
    call fill
    mov ax, 0x3800
    mov dx, buf+1
    int 0x21
    call result
    mov si, msg_bytes
    call puts
    mov ax, bx
    call hex
    mov al, ' '
    out 0xe9, al
    mov si, buf
    mov cx, 36
    call bytes
    mov si, crlf
    call puts
    mov ax, 0x3802
    mov dx, buf+1
    int 0x21
    call result_line
    ; 8: 29h alias of 81h agrees with 65h/A0
    mov si, parse
    mov di, fcb
    mov ax, 0x2900
    int 0x21
    mov si, msg_parse
    call puts
    mov al, [fcb+1]
    call hex2
    mov al, [fupper+0x81]
    call hex2
    mov si, crlf
    call puts
    mov si, msg_end
    call puts
    cmp byte [0x82], 'C'            ; " C" tail: run as a child, return
    je .child
    xor al, al
    out 0xea, al
.child:
    mov ax, 0x4c00
    int 0x21

fill:
    push cx
    push di
    mov di, buf
    mov al, 0xa7
    mov cx, 64
    rep stosb
    pop di
    pop cx
    ret
; record CF/AX/CX of the last call
result:
    mov [last_ax], ax
    mov [last_cx], cx
    pushf
    pop ax
    and ax, 1
    mov [last_cf], ax
    mov si, msg_nls
    call puts
    mov al, [subfn]
    call hex2
    mov si, msg_cf
    call puts
    mov ax, [last_cf]
    call hex2
    mov si, msg_ax
    call puts
    mov ax, [last_ax]
    call hex
    mov si, msg_cx
    call puts
    mov ax, [last_cx]
    jmp hex
result_line:
    call result
    mov si, crlf
    jmp puts
bytes:
    lodsb
    call hex2
    loop bytes
    ret
bytes_es:
    mov al, [es:si]
    inc si
    call hex2
    loop bytes_es
    ret
puts:
    push ax
.n:
    lodsb
    test al, al
    jz .d
    out 0xe9, al
    jmp .n
.d:
    pop ax
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

msg_nls db 'NLS ', 0
msg_cf db ' cf=', 0
msg_ax db ' ax=', 0
msg_cx db ' cx=', 0
msg_bytes db ' b=', 0
msg_len db 'NLS LEN ', 0
msg_upper db 'NLS UPPER ', 0
msg_dh db ' dh_bad=', 0
msg_agree db 'NLS AGREE ', 0
msg_str db ' s=', 0
msg_yn db 'NLS YN ', 0
msg_bxdx db ' bxdx=', 0
msg_parse db 'NLS PARSE ', 0
msg_end db 'NLS END', 13, 10, 0
crlf db 13, 10, 0
yn db 'YyNn?', 0
str1 db 'ab', 0x81, 0, 'after'
parse db 0x81, 0
subfn db 0
len db 0
bad_dh db 0
agree21 db 0
agreea1 db 0
align 2
last_ax dw 0
last_cx dw 0
last_cf dw 0
fcb times 37 db 0
buf times 64 db 0
upper times 256 db 0
fupper times 256 db 0
mem times 256 db 0
fmem times 256 db 0
    times 768 db 0
stack_top:
image_end:
