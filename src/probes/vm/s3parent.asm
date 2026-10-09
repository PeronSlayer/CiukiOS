; TEST ONLY. Model a resident whose installation parent no longer exists.
; Run after S3SHIM.COM has returned, so this cannot affect TSR termination.
; Refuse the real S3VBEFIX: only our forwarding shim carries this token.
bits 16
cpu 386
org 100h
    mov ax,3510h
    int 21h
    cmp bx,48h
    jne refused
    mov ax,es
    cmp ax,1200h
    jb refused
    cmp ax,0A000h-20h
    ja refused
    cmp dword [es:40h],00604B66h
    jne refused
    cmp dword [es:48h],463E802Eh
    jne refused
    cmp dword [es:70h],'CST1'
    jne refused
    cmp dword [es:74h],'PARN'
    jne refused
    push es
    dec ax
    mov es,ax
    cmp byte [es:0],'M'
    je .owner
    cmp byte [es:0],'Z'
    jne .bad_mcb
.owner:
    inc ax
    cmp [es:1],ax
    jne .bad_mcb
    cmp word [es:3],20h
    jne .bad_mcb
    pop es
    mov word [es:16h],0
    mov dx,changed
    mov ah,9
    int 21h
    mov ax,4C00h
    int 21h
.bad_mcb:
    pop es
refused:
    mov dx,denied
    mov ah,9
    int 21h
    mov ax,4C01h
    int 21h
changed db 'S3PARENT: TEST ONLY, resident parent set to zero.',13,10,'$'
denied db 'S3PARENT refused: verified S3SHIM resident required.',13,10,'$'
