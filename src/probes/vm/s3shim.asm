; S3SHIM.COM - TEST ONLY: harmless pinned-S3-shaped INT10 chain fixture.
; Does not perform S3 hardware correction or invoke any hardware operations.
; Both branches tail-jump to the existing AUXSTACK INT10 handler.
; Install only on a private test image, before the first forked game.
bits 16
cpu 386
org 100h
start:
    push cs
    pop ds
    mov ax,3510h
    int 21h
    cmp bx,470h
    jne fail
    mov ax,es
    cmp ax,1200h
    jb fail
    cmp ax,0A000h-4Fh
    ja fail
    cmp word [es:100h],0E3E9h
    jne fail
    push es
    dec ax
    mov es,ax
    cmp byte [es:0],'M'
    jne .bad_mcb
    inc ax
    cmp [es:1],ax
    jne .bad_mcb
    cmp word [es:3],4Fh
    jne .bad_mcb
    pop es
    ; Resident header is in the unused PSP tail, matching the pinned layout.
    mov dword [40h],00604B66h
    mov word [44h],60h
    mov word [46h],0
    mov dword [48h],463E802Eh
    mov dword [4Ch],05740000h
    mov byte [50h],0EAh
    mov [51h],bx
    mov [53h],es
    mov byte [55h],0EAh
    mov [56h],bx
    mov [58h],es
    mov dword [70h],'CST1'             ; exclusive harmless-fixture token
    mov dword [74h],'PARN'
    mov dx,48h
    mov ax,2510h
    int 21h
    ; Match the real pinned installer's environment free. Its INT27 is
    ; translated to AH31 by LOADDRV; this later console fixture uses AH31.
    mov bx,[cs:2Ch]
    mov es,bx
    mov ah,49h
    int 21h
    push cs
    pop es
    mov dx,installed
    mov ah,9
    int 21h
    mov dx,20h                         ; keep PSP + harmless handler, 512B
    mov ax,3100h
    int 21h
.bad_mcb:
    pop es
fail:
    mov dx,unsupported
    mov ah,9
    int 21h
    mov ax,4C01h
    int 21h
installed db 'S3SHIM installed: TEST ONLY, forwarding INT10.',13,10,'$'
unsupported db 'S3SHIM refused: known AUXSTACK predecessor is required.',13,10,'$'
