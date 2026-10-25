; CIUKIEMM.SYS - the EMMXXXX0 character device of the DOS Box (unified guest).
; The VM manager provides LIM EMS 4.0 at INT 67h (dosvm_ems.c, vector
; F100:0000 with "EMMXXXX0" at offset 000Ah). Programs find an expanded
; memory manager in one of two ways (LIM EMS 4.0, Appendix B): the "get
; interrupt vector" test, which the VM manager already satisfies, or the
; "open handle" test: INT 21h AH=3Dh on EMMXXXX0, IOCTL 4400h (bit 7: a
; device), IOCTL 4407h (AL=FFh: ready), close. The second needs a device
; named EMMXXXX0 in the DOS device chain, as EMM386 installs; this driver is
; only that device. It loads from FDCONFIG.SYS (DEVICE=, conventional memory:
; the DFV2 final bind reads every device header there), keeps 6 paragraphs
; resident and answers like EMM386/Jemm: attribute C000h (character device,
; IOCTL), output status ready, open/close done, writes refused, IOCTL read
; and every other command refused (it carries no EMM386 private API). When
; INT 67h has no EMMXXXX0 manager, or its status (AH=40h) is not 00h, INIT
; keeps nothing (end address = start), so the device is not linked.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
org 0
header:
    dd -1                       ; next driver: DOS links it
    dw 0xC000                   ; character device, IOCTL supported (as EMM386)
    dw strategy
    dw interrupt
    db 'EMMXXXX0'
request dd 0
strategy:
    mov [cs:request], bx
    mov [cs:request + 2], es
    retf
interrupt:
    push ax
    push bx
    push ds
    lds bx, [cs:request]
    mov al, [bx + 2]            ; command code
    test al, al
    jz init
    cmp al, 14
    mov ah, 0x03                ; unknown command
    ja .reply
    push bx
    mov bx, errors - 1
    cs xlatb                    ; AL = error code for commands 1-14, 0 = done
    pop bx
    mov ah, al
.reply:
    mov al, ah
    mov ah, 0x81                ; error, done
    test al, al
    jnz .store
    mov ah, 0x01                ; done
.store:
    mov [bx + 3], ax
    pop ds
    pop bx
    pop ax
    retf
; Commands 1-14: output status (10, IOCTL 4407h) ready, open/close (13/14)
; done; write, write with verify, output flush (8/9/11) write fault (0Ah);
; IOCTL read (3) read fault (0Bh), as Jemm answers an unsupported IOCTL
; function; the rest unknown command (03h).
errors db 3, 3, 0x0B, 3, 3, 3, 3, 0x0A, 0x0A, 0, 0x0A, 3, 0, 0
align 16
resident_end:

; INIT (command 0), discarded after it returns. DS:BX = request.
init:
    push cx
    push si
    push di
    push es
    xor ax, ax
    mov es, ax
    mov es, [es:0x67 * 4 + 2]   ; "get interrupt vector" test, no DOS call
    mov di, 0x0A
    mov si, header + 0x0A
    mov cx, 8
    push ds
    push cs
    pop ds
    cld
    repe cmpsb
    pop ds
    mov ax, resident_end
    jne .absent
    push bx
    mov ah, 0x40                ; EMS status: 00h when the manager works
    int 0x67
    pop bx
    test ah, ah
    mov ax, resident_end
    jz .keep
.absent:
    xor ax, ax                  ; end address = start: not installed
.keep:
    mov [bx + 0x0E], ax         ; end address CS:AX
    mov [bx + 0x10], cs
    mov word [bx + 3], 0x0100   ; done
    pop es
    pop di
    pop si
    pop cx
    pop ds
    pop bx
    pop ax
    retf
