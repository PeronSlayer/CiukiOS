; Exercise DOS AH=58h allocation placement and rebuilt MCB ownership.
bits 16
org 100h

start:
    mov byte [stage],'0'
    cli
    mov ax,cs
    mov ss,ax
    mov sp,stack_top
    sti
    mov ds,ax
    mov es,ax
    mov bx,(image_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    jc fail
    mov ax,5800h
    int 21h
    jc fail
    mov [old_strategy],bx
    mov byte [stage],'U'
    ; A DOS UMB link must not claim success while AH=48h only owns the
    ; conventional MCB arena.  Jemm's separate XMS UMB API is not DOS=UMB.
    mov ax,5802h
    int 21h
    jc fail
    cmp al,0
    jne fail
    mov bx,1
    mov ax,5803h
    int 21h
    jnc fail
    cmp ax,1
    jne fail
    mov bx,40h
    mov ax,5801h
    int 21h
    jnc fail
    cmp ax,1
    jne fail
    mov ax,5800h
    int 21h
    jc fail
    cmp bx,[old_strategy]
    jne fail
    mov byte [stage],'1'
    xor bx,bx
    mov ax,5801h
    int 21h
    jc fail

    mov bx,20h
    call alloc
    mov [block_a],ax
    mov bx,60h
    call alloc
    mov [block_b],ax
    mov bx,20h
    call alloc
    mov [block_c],ax
    mov bx,40h
    call alloc
    mov [block_d],ax
    mov bx,20h
    call alloc
    mov [block_e],ax
    mov byte [stage],'2'

    ; Occupy the remaining high interval so the two freed holes are the
    ; only candidates for first/best/last placement.
    mov bx,0FFFFh
    mov ah,48h
    int 21h
    jnc fail
    cmp ax,8
    jne fail
    call alloc
    mov [guard],ax
    mov byte [stage],'3'

    mov es,[block_b]
    mov ah,49h
    int 21h
    jc fail
    mov es,[block_d]
    mov ah,49h
    int 21h
    jc fail
    mov byte [stage],'4'

    ; First fit selects the first hole and carves from its low end.
    mov bx,10h
    call alloc
    cmp ax,[block_b]
    jne fail
    mov bx,10h
    call check_mcb
    mov byte [stage],'5'

    ; Best fit selects the smaller D hole, also from its low end.
    mov bx,1
    mov ax,5801h
    int 21h
    jc fail
    mov ax,5800h
    int 21h
    cmp bx,1
    jne fail
    mov bx,8
    call alloc
    cmp ax,[block_d]
    jne fail
    mov byte [stage],'B'
    mov bx,8
    call check_mcb
    mov byte [stage],'6'

    ; Last fit takes the high end of the last remaining hole.
    mov bx,2
    mov ax,5801h
    int 21h
    jc fail
    mov bx,8
    call alloc
    mov dx,[block_d]
    add dx,38h
    cmp ax,dx
    jne fail
    mov bx,8
    call check_mcb
    mov byte [stage],'7'

    mov bx,[old_strategy]
    mov ax,5801h
    int 21h
    jc fail
    mov dx,passed
    mov ah,9
    int 21h
    mov ax,4C00h
    int 21h

alloc:
    mov ah,48h
    int 21h
    jc fail
    ret

; AX=data segment, BX=requested paragraphs. Verify DOS owns the exact block.
check_mcb:
    push ax
    push es
    dec ax
    mov es,ax
    cmp byte [es:0],'M'
    jne fail
    cmp [es:3],bx
    jne fail
    mov ax,cs
    cmp [es:1],ax
    jne fail
    pop es
    pop ax
    ret

fail:
    push ax
    push cs
    pop ds
    mov dl,[stage]
    mov ah,2
    int 21h
    pop ax
    call print_hex16
    mov dl,'/'
    mov ah,2
    int 21h
    mov ax,[block_d]
    call print_hex16
    mov dx,failed
    mov ah,9
    int 21h
    mov ax,4C01h
    int 21h

print_hex16:
    push ax
    push cx
    mov cx,4
.digit:
    rol ax,4
    push ax
    and al,15
    cmp al,10
    jb .number
    add al,'A'-10
    jmp .emit
.number:
    add al,'0'
.emit:
    mov dl,al
    mov ah,2
    int 21h
    pop ax
    loop .digit
    pop cx
    pop ax
    ret

old_strategy dw 0
stage db '?'
block_a dw 0
block_b dw 0
block_c dw 0
block_d dw 0
block_e dw 0
guard dw 0
passed db '[MEMSTRAT] PASS first/best/last MCB placement',13,10,'$'
failed db '[MEMSTRAT] FAIL',13,10,'$'
times 512 db 0
stack_top:
image_end:
