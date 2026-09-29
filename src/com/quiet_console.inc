; Quiet console for programs run while the boot splash is on screen.
; quiet_detect sets [quiet] on a graphics screen (a VESA mode or a graphics
; BIOS mode). silence then routes the console output of the programs it runs
; (INT 21h AH=02h, 06h output, 09h, 40h on handles 1/2; CiukiDOS draws
; AH=02h/09h on the screen whatever handle 1 is) to COM1 through an INT 21h
; hook in the IVT, and speak removes it. com1 writes AL to COM1.

quiet_detect:
    mov ax,4F03h
    int 10h
    cmp ax,004Fh
    jne .bda
    and bx,3FFFh
    cmp bx,100h
    jae .quiet
.bda:
    push ds
    push 40h
    pop ds
    mov al,[49h]
    pop ds
    cmp al,3
    jbe .text
    cmp al,7
    je .text
.quiet:
    mov byte [quiet],1
.text:
    ret

; Quiet: the children's console output (Jemm386 prints with INT 21h AH=02h,
; which CiukiDOS writes to the screen whatever handle 1 is) goes to COM1
; instead, through an INT 21h hook held while they run.
silence:
    cmp byte [quiet],0
    je .done
    ; The IVT entry itself: the kernel does not let AH=25h replace its own
    ; INT 21h vector.
    push es
    xor ax,ax
    mov es,ax
    cli
    mov ax,[es:21h*4]
    mov [old21],ax
    mov ax,[es:21h*4+2]
    mov [old21+2],ax
    mov word [es:21h*4],int21_quiet
    mov [es:21h*4+2],cs
    sti
    pop es
    mov byte [hooked],1
.done:
    ret

; Undo silence, unless something hooked INT 21h after us.
speak:
    cmp byte [hooked],0
    je .done
    push es
    xor ax,ax
    mov es,ax
    mov ax,cs
    cmp [es:21h*4+2],ax
    jne .other
    cmp word [es:21h*4],int21_quiet
    jne .other
    cli
    mov ax,[old21]
    mov [es:21h*4],ax
    mov ax,[old21+2]
    mov [es:21h*4+2],ax
    sti
    mov byte [hooked],0
.other:
    pop es
.done:
    ret

int21_quiet:
    cmp ah,2
    je .char
    cmp ah,9
    je .string
    cmp ah,6
    jne .not_direct
    cmp dl,0FFh
    jne .char
.not_direct:
    cmp ah,40h
    jne .chain
    cmp bx,1
    je .write
    cmp bx,2
    je .write
.chain:
    jmp far [cs:old21]
.char:
    push ax
    mov al,dl
    call com1
    pop ax
    mov al,dl
    iret
.string:
    push si
    mov si,dx
.next:
    lodsb
    cmp al,'$'
    je .string_done
    call com1
    jmp .next
.string_done:
    pop si
    iret
.write:
    push si
    push cx
    mov si,dx
    jcxz .written
.byte:
    lodsb
    call com1
    loop .byte
.written:
    pop cx
    pop si
    mov ax,cx
    push bp
    mov bp,sp
    and byte [bp+6],0FEh                ; CF clear in the caller's flags
    pop bp
    iret

; AL -> COM1. Preserves every register.
com1:
    push dx
    push ax
    mov dx,3FDh
.wait:
    in al,dx
    test al,20h
    jz .wait
    pop ax
    mov dx,3F8h
    out dx,al
    pop dx
    ret

quiet db 0
hooked db 0
old21 dd 0
