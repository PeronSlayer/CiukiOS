; CIUKISH.COM - the DOS Box session shell (SHELL= of the unified guest).
; Runs the FreeCOM interpreter non-permanently (/K runs FDAUTO.BAT, then the
; prompt), so EXIT ends that interpreter; the wrapper then asks the VM manager
; for a clean VM exit (control port 0EAh, reason EXIT) and the DOS Box closes
; its window, as an MS-DOS Prompt does in Windows 98 with "Close on exit" on.
; The primary FreeCOM with /P ignores EXIT. The EXEC happens before DOS ready,
; so it is not counted as a user program (dosvm.c, f3-61 exec_pending).
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
    mov [params + 4], ax
    mov [params + 8], ax
    mov [params + 12], ax
    mov bx, (image_end - $$ + 0x100 + 15) >> 4
    mov ah, 0x4A
    int 0x21
    jc fatal
    mov dx, command
    mov bx, params
    mov ax, 0x4B00
    int 0x21
    jc fatal
    mov dx, 0xEA
    xor al, al
    out dx, al
    mov ax, 0x4C00
    int 0x21
fatal:
    mov dx, 0xFFFF
    xor al, al
    out dx, al
    hlt
    jmp fatal
command db '\COMMAND.COM',0
params dw 0,tail,0,0x5C,0,0x6C,0
tail db tail_end - tail_text
tail_text db ' C:\ /E:1024 /K C:\FDAUTO.BAT'
tail_end db 13
align 16
stack times 256 db 0
stack_top:
image_end:
