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

    ; Fill every other conventional gap, not merely the largest one.  This
    ; leaves the deliberately freed B and D blocks as the only fit candidates.
.fill_guards:
    mov bx,0FFFFh
    mov ah,48h
    int 21h
    jnc fail
    cmp ax,8
    jne fail
    or bx,bx
    jz .guards_done
    cmp word [guard_count],20
    jae fail
    call alloc
    mov si,[guard_count]
    shl si,1
    mov [guards+si],ax
    inc word [guard_count]
    jmp .fill_guards
.guards_done:
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
    mov [actual],ax
    mov byte [stage],'A'
    cmp ax,[block_b]
    jne fail
    mov byte [stage],'M'
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
    mov dx,diag_a
    mov ah,9
    int 21h
    mov ax,[block_a]
    call print_hex16
    mov dx,diag_b
    mov ah,9
    int 21h
    mov ax,[block_b]
    call print_hex16
    mov dx,diag_c
    mov ah,9
    int 21h
    mov ax,[block_c]
    call print_hex16
    mov dx,diag_d
    mov ah,9
    int 21h
    mov ax,[block_d]
    call print_hex16
    mov dx,diag_e
    mov ah,9
    int 21h
    mov ax,[block_e]
    call print_hex16
    mov dx,diag_actual
    mov ah,9
    int 21h
    mov ax,[actual]
    call print_hex16
    or ax,ax
    jz .mcb_done
    dec ax
    mov es,ax
    mov dx,diag_mcb
    mov ah,9
    int 21h
    xor ax,ax
    mov al,[es:0]
    call print_hex16
    mov dx,diag_owner
    mov ah,9
    int 21h
    mov ax,[es:1]
    call print_hex16
    mov dx,diag_size
    mov ah,9
    int 21h
    mov ax,[es:3]
    call print_hex16
.mcb_done:
    mov dx,diag_end
    mov ah,9
    int 21h
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
guard_count dw 0
guards times 20 dw 0
actual dw 0
passed db '[MEMSTRAT] PASS first/best/last MCB placement',13,10,'$'
failed db '[MEMSTRAT] FAIL',13,10,'$'
diag_a db ' A=','$'
diag_b db ' B=','$'
diag_c db ' C=','$'
diag_d db ' D=','$'
diag_e db ' E=','$'
diag_actual db ' X=','$'
diag_mcb db ' T=','$'
diag_owner db ' O=','$'
diag_size db ' S=','$'
diag_end db 13,10,'$'
times 512 db 0
stack_top:
image_end:
