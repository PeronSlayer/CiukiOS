; VMVIDEO.COM: DOS-side virtual VGA pattern fixture for probe f3:vm-video
; (directive f3-02). Each stage draws one deterministic frame, reports
; "VMVIDEO s f" on byte port E9 (s stage, f hex flags) and waits for a key.
; tests/host/dosvm_test.c (test_video_goldens) reproduces every stage
; through the same VGA model and pins the presenter digests. Change both,
; and the suite goldens, together.
;   1  mode 03h, blink off, cursor off, 2000 cells char=i attr=i>>3,
;      INT 10h/1300h string on row 24; flags: bit0 retrace edges seen,
;      bit1 DAC entry 41h read back.
;   2  INT 10h/1112h 8x8 font (80x50), cursor off, 4000 cells char=i attr=i>>4
;   3  mode 13h, DAC 40h-7Fh = (k, 63-k, 2k&63) by ports, pixel (x+y)&FFh;
;      flags: bit0 retrace edges seen
;   4  Mode X 320x240 (unchained), pixel (x+y)&FFh by plane
;   5  mode 12h, pixel colour ((x>>3)+(y>>3))&15 by plane
;   6  mode 03h restored, then exit 0
; Waits (f3-01 wait-loop rule): the stage key is INT 16h AH=00h, which
; blocks in the monitor until the BIOS buffer holds a key, and retrace is a
; bounded poll of 3DAh with interrupts enabled. No wait sleeps in the guest
; with HLT, so there is no test-then-HLT window for an IRQ to fall into; a
; future interrupt-driven wait must test with IF clear and sleep with STI; HLT.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
org 100h
start:
    cld
    push cs
    pop ds
    push cs
    pop es

; ---- stage 1: text 80x25 ----
    mov ax, 0003h
    int 10h
    mov ax, 1003h
    xor bl, bl
    int 10h
    mov ah, 01h
    mov cx, 2000h
    int 10h
    mov cx, 2000
    mov dl, 3
    call text_cells
    mov ax, 1300h
    mov bx, 001Eh
    mov cx, banner_len
    mov dx, 1800h
    mov bp, banner
    int 10h
    mov byte [flags], 0
    call retrace
    call dac_check
    mov al, '1'
    call stage

; ---- stage 2: text 80x50 ----
    mov ax, 1112h
    xor bl, bl
    int 10h
    mov ah, 01h
    mov cx, 2000h
    int 10h
    mov cx, 4000
    mov dl, 4
    call text_cells
    mov byte [flags], 0
    mov al, '2'
    call stage

; ---- stage 3: mode 13h ----
    mov ax, 0013h
    int 10h
    mov dx, 3C8h
    mov al, 40h
    out dx, al
    inc dx
    xor bl, bl
.dac:
    mov al, bl
    out dx, al
    mov al, 63
    sub al, bl
    out dx, al
    mov al, bl
    shl al, 1
    and al, 63
    out dx, al
    inc bl
    cmp bl, 64
    jb .dac
    xor bx, bx                  ; y
.row13:
    mov al, bl
    mov cx, 320
    mov ah, 1
    call fill_row
    mov ax, 320
    mul bx
    mov di, ax
    mov cx, 320
    call copy_row
    inc bx
    cmp bx, 200
    jb .row13
    mov byte [flags], 0
    call retrace
    mov al, '3'
    call stage

; ---- stage 4: Mode X 320x240 ----
    mov ax, 0013h
    int 10h
    mov dx, 3C4h
    mov ax, 0604h               ; SR04: chain-4 and odd/even off
    out dx, ax
    mov ax, 0F02h               ; map mask: all planes
    out dx, ax
    mov ax, 0A000h
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, 8000h
    rep stosw
    push cs
    pop es
    mov dx, 3D4h
    mov ax, 0014h               ; underline: dword mode off
    out dx, ax
    mov ax, 0E317h              ; mode control: byte mode
    out dx, ax
    mov dx, 3C2h
    mov al, 0E3h                ; misc output: 480-line sync polarity
    out dx, al
    mov dx, 3D4h
    mov si, modex_crtc
    mov cx, modex_count
.crtc:
    lodsw
    out dx, ax
    loop .crtc
    xor bp, bp                  ; plane
.plane_x:
    call map_plane
    xor bx, bx
.row_x:
    mov ax, bp
    add al, bl
    mov ah, 4
    mov cx, 80
    call fill_row
    mov ax, 80
    mul bx
    mov di, ax
    mov cx, 80
    call copy_row
    inc bx
    cmp bx, 240
    jb .row_x
    inc bp
    cmp bp, 4
    jb .plane_x
    mov byte [flags], 0
    mov al, '4'
    call stage

; ---- stage 5: mode 12h planar ----
    mov ax, 0012h
    int 10h
    xor bp, bp
.plane_12:
    call map_plane
    xor bx, bx
.row_12:
    mov di, rowbuf
    xor dx, dx                  ; byte column
    mov cx, bp
.col_12:
    mov ax, bx
    shr ax, 1
    shr ax, 1
    shr ax, 1
    add ax, dx
    and al, 15
    shr al, cl
    and al, 1
    neg al                      ; 0 or FFh
    stosb
    inc dx
    cmp dx, 80
    jb .col_12
    mov ax, 80
    mul bx
    mov di, ax
    mov cx, 80
    call copy_row
    inc bx
    cmp bx, 480
    jb .row_12
    inc bp
    cmp bp, 4
    jb .plane_12
    mov byte [flags], 0
    mov al, '5'
    call stage

; ---- stage 6: back to text and exit ----
    mov ax, 0003h
    int 10h
    mov byte [flags], 0
    mov al, '6'
    call report
    mov ax, 4C00h
    int 21h

; CX cells at B800:0000: character = i & FFh, attribute = (i >> DL) & FFh.
text_cells:
    push es
    mov ax, 0B800h
    mov es, ax
    xor di, di
    xor si, si                  ; i
.cell:
    push cx
    mov ax, si
    mov cl, dl
    shr ax, cl
    mov ah, al                  ; attribute
    mov cx, si
    mov al, cl                  ; character
    mov [es:di], ax
    pop cx
    add di, 2
    inc si
    loop .cell
    pop es
    ret

; CX bytes into rowbuf: AL first value, AH increment.
fill_row:
    push di
    mov di, rowbuf
.byte:
    stosb
    add al, ah
    loop .byte
    pop di
    ret

; CX bytes rowbuf -> A000:DI.
copy_row:
    push es
    push si
    mov si, 0A000h
    mov es, si
    mov si, rowbuf
    rep movsb
    pop si
    pop es
    ret

; Map mask = 1 << BP.
map_plane:
    mov cx, bp
    mov ah, 1
    shl ah, cl
    mov al, 02h
    mov dx, 3C4h
    out dx, ax
    ret

; Vertical retrace edges on input status 1: flags bit 0 when bit 3 is seen
; set and then clear within a bounded number of reads.
retrace:
    mov dx, 3DAh
    xor cx, cx
    mov si, 64
.wait_set:
    in al, dx
    test al, 8
    jnz .seen
    loop .wait_set
    dec si
    jnz .wait_set
    ret
.seen:
    xor cx, cx
    mov si, 64
.wait_clear:
    in al, dx
    test al, 8
    jz .edges
    loop .wait_clear
    dec si
    jnz .wait_clear
    ret
.edges:
    or byte [flags], 1
    ret

; DAC entry 41h written and read back through 3C8h/3C7h/3C9h: flags bit 1.
dac_check:
    mov dx, 3C8h
    mov al, 41h
    out dx, al
    inc dx
    mov al, 12
    out dx, al
    mov al, 34
    out dx, al
    mov al, 56
    out dx, al
    mov dx, 3C7h
    mov al, 41h
    out dx, al
    mov dx, 3C9h
    in al, dx
    cmp al, 12
    jne .done
    in al, dx
    cmp al, 34
    jne .done
    in al, dx
    cmp al, 56
    jne .done
    or byte [flags], 2
.done:
    ret

; Report stage AL and wait for the controller's key.
stage:
    call report
    xor ah, ah
    int 16h
    ret
report:
    mov [line + 8], al
    mov al, [flags]
    and al, 15
    add al, '0'
    cmp al, '9'
    jbe .digit
    add al, 7
.digit:
    mov [line + 10], al
    mov si, line
.out:
    lodsb
    test al, al
    jz .sent
    out 0E9h, al
    jmp .out
.sent:
    ret

modex_crtc dw 2C11h, 0D06h, 3E07h, 4109h, 0EA10h, 0AC11h, 0DF12h, 0E715h, 0616h
modex_count equ ($ - modex_crtc) / 2
banner db 'CiukiOS virtual VGA 80x25'
banner_len equ $ - banner
line db 'VMVIDEO 0 0', 10, 0
flags db 0
rowbuf times 320 db 0
