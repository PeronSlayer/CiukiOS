; An ordinary, unchanged DOS COM workload: no knowledge of CiukiOS, Jemm, JLM,
; window handles or a cooperative graphics API. Exercises BIOS, VGA ports,
; direct A000/B800 writes, timer/keyboard BIOS and DOS file I/O/exit.
bits 16
org 100h
start:
    mov ax,13h
    int 10h
    mov ax,0A000h
    mov es,ax
    xor di,di
    mov cx,64000
    mov al,2Ah
    cld
    rep stosb
    mov dx,3C8h
    mov al,2Ah
    out dx,al
    inc dx
    mov al,63
    out dx,al
    xor al,al
    out dx,al
    out dx,al
    ; Scalar IN must preserve the unaffected accumulator bits and guest CF.
    ; Jemm writes the callback's full EAX back, even for byte/word opcodes.
    mov dx,3C6h
    mov al,5Ah
    out dx,al
    mov eax,0A1B2C300h
    stc
    in al,dx
    pushf
    pop bx
    test bl,1
    jz failed
    cmp eax,0A1B2C35Ah
    jne failed
    mov dx,3C4h
    mov ax,5A02h
    out dx,ax
    mov eax,0ABCD0000h
    in ax,dx
    cmp eax,0ABCD5A02h
    jne failed
    mov eax,0FFFFFFFFh
    in eax,dx
    cmp eax,005A5A02h
    jne failed
    mov ax,0B800h
    mov es,ax
    mov word [es:0],1F56h
    mov word [es:2],1F4Dh
    push cs
    pop ds
    mov dx,path
    xor cx,cx
    mov ah,3Ch
    int 21h
    jc failed
    mov bx,ax
    mov dx,content
    mov cx,content_end-content
    mov ah,40h
    int 21h
    jc close_failed
    cmp ax,content_end-content
    jne close_failed
    mov ah,3Eh
    int 21h
    jc failed
    mov byte [cs:observation+4],1
    xor ah,ah
    int 1Ah
    mov [cs:start_tick],dx
wait_key:
    inc word [cs:observation+6]
    mov ah,1
    int 16h
    jnz consume
    xor ah,ah
    int 1Ah
    sub dx,[cs:start_tick]
    cmp dx,18*8
    jb wait_key
    jmp done
consume:
    xor ah,ah
    int 16h
    mov [cs:observation+8],ax
done:
    mov byte [cs:observation+4],2
    mov ax,4C00h
    int 21h
close_failed:
    mov ah,3Eh
    int 21h
failed:
    mov byte [cs:observation+4],3
    mov ax,4C01h
    int 21h
start_tick dw 0
path db '\VMGUEST.TXT',0
content db 'Ordinary DOS child: BIOS video, ports, VRAM and file output.',13,10
content_end:
observation db 'VMGS',0,0
    dw 0,0
