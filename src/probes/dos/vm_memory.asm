; Canonical DOS-side memory conformance fixture; all results via byte port E9.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
org 100h
%macro XCALL 1
    mov byte [last], %1
    mov ah, %1
    call far [xms]
%endmacro
%macro XOK 1
    XCALL %1
    cmp ax, 1
    jne fail
%endmacro
%macro ECALL 1
    mov byte [last], (%1 >> 8)
    mov ax, %1
    int 67h
%endmacro
%macro EOK 1
    ECALL %1
    test ah, ah
    jnz fail
%endmacro
start:
    cld
    push cs
    pop ds
    push cs
    pop es
    mov ax, 4300h
    int 2Fh
    cmp al, 80h
    jne fail
    mov ax, 4310h
    int 2Fh
    mov [xms], bx
    mov [xms+2], es
    push cs
    pop es
    XCALL 0
    cmp ax, 0300h
    jne fail
    cmp dx, 1
    jne fail
    mov dx, 0FFFFh
    XOK 1
    XCALL 1
    cmp bl, 91h
    jne fail
    XOK 2
    XCALL 2
    cmp bl, 93h
    jne fail
    XOK 3
    XOK 5
    XCALL 4
    cmp bl, 94h
    jne fail
    XOK 6
    XCALL 7
    test ax, ax
    mov byte [last], 0A1h
    jnz fail
    ; Preserve a conventional word, test actual HMA/low aliasing in V86.
    xor ax, ax
    mov es, ax
    mov ax, [es:0800h]
    mov [low_saved], ax
    mov word [es:0800h], 5A3Ch
    mov ax, 0FFFFh
    mov es, ax
    cmp word [es:0810h], 5A3Ch
    mov byte [last], 0A2h
    jne fail
    mov al, 2
    out 92h, al
    mov word [es:0810h], 6B4Dh
    xor ax, ax
    mov es, ax
    cmp word [es:0800h], 5A3Ch
    mov byte [last], 0A3h
    jne fail
    mov ax, 2400h
    int 15h
    mov byte [last], 0A4h
    jc fail
    mov ax, 0FFFFh
    mov es, ax
    cmp word [es:0810h], 5A3Ch
    mov byte [last], 0A5h
    jne fail
    mov al, 0D1h
    out 64h, al
    mov al, 3
    out 60h, al
    cmp word [es:0810h], 6B4Dh
    mov byte [last], 0A6h
    jne fail
    mov al, 0D0h
    out 64h, al
    in al, 64h
    test al, 1
    mov byte [last], 0A7h
    jz fail
    in al, 60h
    test al, 2
    mov byte [last], 0A8h
    jz fail
    mov ax, 2402h
    int 15h
    cmp ax, 1
    mov byte [last], 0A9h
    jne fail
    mov ax, 2403h
    int 15h
    cmp bx, 3
    mov byte [last], 0AAh
    jne fail
    mov ax, 2401h
    int 15h
    mov byte [last], 0ABh
    jc fail
    xor ax, ax
    mov es, ax
    mov ax, [low_saved]
    mov [es:0800h], ax
    push cs
    pop es
    XCALL 8
    test ax, ax
    jz fail
    mov [quota], dx
    XCALL 88h
    cmp edx, 24576
    jne fail
    cmp ecx, 0190FFFFh
    jne fail
    mov dx, 64
    XOK 9
    mov [xh], dx
    mov edx, 64
    XOK 89h
    mov [xh2], dx
    ; A conventional pattern moves through EMB storage and back.
    mov di, pattern
    mov cx, 512
    mov ax, 1234h
.fill:
    stosw
    add ax, 101h
    loop .fill
    mov ax, ds
    mov [move+8], ax
    mov word [move+6], pattern
    mov dx, [xh]
    mov [move+10], dx
    mov si, move
    XOK 0Bh
    mov word [move+4], dx
    mov dword [move+6], 0
    mov word [move+10], 0
    mov word [move+12], result
    mov ax, ds
    mov [move+14], ax
    XOK 0Bh
    call verify
    ; A 32-bit, 64 KiB EMB-to-EMB move, beyond a 16-bit byte count.
    mov dword [move], 10000h
    mov dx, [xh2]
    mov [move+10], dx
    mov dword [move+12], 0
    XOK 0Bh
    mov dword [move], 1024
    mov dx, [xh2]
    mov [move+4], dx
    mov word [move+10], 0
    mov word [move+12], result
    mov ax, ds
    mov [move+14], ax
    XOK 0Bh
    call verify
    mov dx, [xh]
    mov bx, 128
    XOK 0Fh
    mov ebx, 32
    XOK 8Fh
    XOK 0Eh
    cmp dx, 32
    jne fail
    mov dx, [xh]
    XOK 8Eh
    cmp edx, 32
    jne fail
    mov dx, [xh]
    mov ebx, 0FFFFFFFFh
    XCALL 8Fh
    cmp bl, 0A0h
    jne fail
    mov edx, 24577
    XCALL 89h
    cmp bl, 0A0h
    jne fail
    ; Confirm failed resize did not change the old data.
    mov dx, [xh]
    mov [move+4], dx
    XOK 0Bh
    call verify
    mov dx, [xh]
    XOK 0Ch
    cmp dx, 0191h
    jae fail
    mov dx, [xh]
    XCALL 0Ah
    cmp bl, 0ABh
    jne fail
    XOK 0Dh
    XCALL 0Dh
    cmp bl, 0AAh
    jne fail
    XOK 0Ah
    mov dx, [xh2]
    XOK 0Ah
    mov dx, 0FFFFh
    XCALL 10h
    cmp bl, 0B0h
    jne fail
    mov dx, 17
    XOK 10h
    cmp bx, 0C900h
    jb fail
    cmp bx, 0E000h
    jae fail
    mov [umb], bx
    mov dx, bx
    mov bx, 512
    XOK 12h
    mov dx, [umb]
    XOK 11h
    XCALL 11h
    cmp bl, 0B2h
    jne fail
    mov ax, 5802h
    int 21h
    jc fail
    test ax, ax
    jnz fail
    mov ax, 5801h
    mov bx, 40h
    int 21h
    jnc fail
    ; Reserved 3.0 extension holes must fail truthfully.
    mov byte [last], 8Ah
.holes:
    mov ah, [last]
    call far [xms]
    cmp bl, 80h
    jne fail
    inc byte [last]
    cmp byte [last], 8Eh
    jb .holes
    ; EMS discovery includes the interrupt handler's device signature.
    xor ax, ax
    mov es, ax
    mov bx, [es:67h*4]
    mov ax, [es:67h*4+2]
    mov es, ax
    cmp dword [es:bx+10], 'EMMX'
    jne fail
    push cs
    pop es
    EOK 4000h
    EOK 4100h
    mov [frame_seg], bx
    EOK 4200h
    cmp dx, 512
    jne fail
    mov bx, 2
    EOK 4300h
    mov [eh], dx
    EOK 4600h
    cmp al, 40h
    jne fail
    xor bx, bx
    mov cx, 4
    xor ax, ax
.map:
    push ax
    mov ah, 44h
    int 67h
    test ah, ah
    jnz fail
    pop ax
    inc al
    loop .map
    mov es, [frame_seg]
    mov word [es:0], 0ABCDh
    cmp word [es:4000h], 0ABCDh
    jne fail
    cmp word [es:8000h], 0ABCDh
    jne fail
    cmp word [es:0C000h], 0ABCDh
    jne fail
    push cs
    pop es
    EOK 4700h
    mov bx, 0FFFFh
    EOK 4400h
    EOK 4800h
    EOK 4B00h
    cmp bx, 1
    jne fail
    EOK 4C00h
    cmp bx, 2
    jne fail
    mov di, maps
    EOK 4D00h
    EOK 4E00h
    mov si, maps
    EOK 4E01h
    EOK 4E02h
    EOK 4E03h
    mov ax, [frame_seg]
    mov di, partial+2
    mov cx, 4
.segs:
    stosw
    add ax, 400h
    loop .segs
    mov si, partial
    mov di, maps
    EOK 4F00h
    mov si, maps
    EOK 4F01h
    mov bx, 4
    EOK 4F02h
    mov si, pairs
    mov cx, 2
    EOK 5000h
    mov ax, [frame_seg]
    mov [pairs+2], ax
    add ax, 400h
    mov [pairs+6], ax
    EOK 5001h
    mov bx, 3
    EOK 5100h
    EOK 5200h
    xor bx, bx
    EOK 5201h
    EOK 5202h
    mov si, name
    EOK 5301h
    mov di, maps
    EOK 5300h
    cmp dword [maps], 'CIUK'
    jne fail
    EOK 5400h
    mov si, name
    EOK 5401h
    EOK 5402h
    ; Copy/exchange conventional and expanded regions without changing maps.
    mov ax, ds
    mov [emove+9], ax
    mov dx, [eh]
    mov [emove+12], dx
    mov si, emove
    EOK 5700h
    EOK 5701h
    mov di, maps
    EOK 5800h
    EOK 5801h
    cmp cx, 4
    jne fail
    ; Control transfers through the real ROM/IRET and RETF boundaries.
    mov ax, cs
    mov [jump+2], ax
    mov [callmap+2], ax
    mov [jump+7], ax
    mov [callmap+7], ax
    mov [callmap+12], ax
    mov dx, [eh]
    mov si, jump
    EOK 5500h
    jmp fail
after_jump:
    mov si, callmap
    EOK 5600h
    cmp word [called], 1
    jne fail
    EOK 5602h
    mov byte [last], 49h
    mov ah, 49h
    int 67h
    cmp ah, 84h
    jne fail
    mov ah, 4Ah
    int 67h
    cmp ah, 84h
    jne fail
    ; OS/E hardware entry points are explicitly access-denied.
    mov byte [last], 59h
.ose:
    mov ah, [last]
    xor al, al
    int 67h
    cmp ah, 0A4h
    jne fail
    inc byte [last]
    cmp byte [last], 5Eh
    jb .ose
    mov dx, [eh]
    EOK 4500h
    mov si, pass_text
    call record
    mov ax, 4C00h
    int 21h
far_target:
    inc word [called]
    retf
verify:
    push si
    push di
    push cx
    mov si, pattern
    mov di, result
    mov cx, 512
    repe cmpsw
    jne fail
    pop cx
    pop di
    pop si
    ret
fail:
    push cs
    pop ds
    push cs
    pop es
    mov al, [last]
    mov di, fail_text + 14
    call hex_byte
    mov si, fail_text
    call record
    mov ax, 4C01h
    int 21h
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
record:
    lodsb
    test al, al
    jz .done
    out 0E9h, al
    jmp record
.done:
    ret
xms dd 0
xh dw 0
xh2 dw 0
eh dw 0
umb dw 0
frame_seg dw 0
low_saved dw 0
quota dw 0
last db 0
called dw 0
move dd 1024
     dw 0
     dd 0
     dw 0
     dd 0
emove dd 1024
      db 0
      dw 0, pattern, 0
      db 1
      dw 0, 0, 0
pairs dw 1,0,0,1
partial dw 4,0,0,0,0
jump dw after_jump,0
     db 0
     dw pairs,0
callmap dw far_target,0
        db 0
        dw pairs,0
        db 0
        dw pairs,0
        times 8 db 0
name db 'CIUKIMEM'
pass_text db 'VMMEMORY PASS',10,0
fail_text db 'VMMEMORY FAIL 00',10,0
maps times 1024 db 0
pattern times 1024 db 0
result times 1024 db 0
