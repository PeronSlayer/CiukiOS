; FSTEST.COM - fx-selftest DOS full-screen stimulus (docs/design/self-test.md).
; Stage 1: BIOS mode 13h (320x200x8) and a diagonal gradient, then waits for
; a key. Stage 2: VBE 4F02h mode 101h (640x480x8, windowed/banked model,
; Warcraft II's mode) and a pattern in the first 64 KiB window, then waits for
; a key. Stage 3: text mode 3, "FSTEST done" or "FSTEST VBE 101h refused",
; exit code 0 or 1. The kernel self-test types the keys; nothing is written
; to any disk. Interfaces: INT 10h AH=00h (IBM PC BIOS), INT 10h AX=4F02h
; (VESA BIOS Extension Core Functions 3.0), INT 16h AH=00h, INT 21h AH=09h/4Ch.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
org 100h
start:
    mov ax, 0013h
    int 10h
    mov ax, 0A000h
    mov es, ax
    xor di, di
    xor dx, dx              ; row
.row:
    xor cx, cx              ; column
.column:
    mov al, cl
    add al, dl
    stosb
    inc cx
    cmp cx, 320
    jb .column
    inc dx
    cmp dx, 200
    jb .row
    xor ax, ax
    int 16h                 ; stage 1 held until the controller's key
    mov ax, 4F02h
    mov bx, 0101h
    int 10h
    cmp ax, 004Fh
    jne .refused
    mov ax, 0A000h
    mov es, ax
    xor di, di
    mov cx, 32768
.fill:
    mov ax, di
    stosw
    loop .fill
    xor ax, ax
    int 16h                 ; stage 2 held until the controller's key
    mov ax, 0003h
    int 10h
    mov dx, done
    mov ah, 09h
    int 21h
    mov ax, 4C00h
    int 21h
.refused:
    mov ax, 0003h
    int 10h
    mov dx, refused
    mov ah, 09h
    int 21h
    mov ax, 4C01h
    int 21h
done db 'FSTEST done', 13, 10, '$'
refused db 'FSTEST VBE 101h refused', 13, 10, '$'
