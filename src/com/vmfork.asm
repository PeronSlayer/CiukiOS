; VMFORK.COM - run a DOS program in a new virtual machine.
;
;   VMFORK program [arguments]
;
; EXEC'd by the system VM (the desktop, a test). It asks the CVSESSION VM
; manager for a fork: the new VM is a copy of this moment. In the system VM
; VMFORK returns at once (ERRORLEVEL 0) and its caller carries on; in the new
; VM it frees the memory of its ancestors (the desktop is never resumed
; there) after giving the interrupt vectors that point into it their value
; from when the VM manager loaded, runs the program and reports the exit
; code to the VM manager, which ends the VM. Messages are English; errors return ERRORLEVEL 1.
bits 16
cpu 386
org 100h
%include "src/vm/session_abi.inc"

start:
    cld
    mov sp,stack_top
    mov bx,(image_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    ; Program path and tail.
    mov si,81h
    call skip_blanks
    cmp al,13
    mov dx,usage
    je fail
    mov di,program
    mov cx,127
.path:
    lodsb
    cmp al,' '
    jbe .path_end
    stosb
    loop .path
.path_end:
    dec si
    mov byte [di],0
    call skip_blanks
    mov di,child_tail+1
    xor cx,cx
    cmp al,13
    je .tail_end
    mov al,' '
    stosb
    inc cx
.tail:
    lodsb
    cmp al,13
    je .tail_end
    stosb
    inc cx
    cmp cx,125
    jb .tail
.tail_end:
    mov byte [di],13
    mov [child_tail],cl
    ; VM manager.
    xor di,di
    mov es,di
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    int 2Fh
    mov [entry],di
    mov [entry+2],es
    mov ax,es
    or ax,di
    push cs
    pop es
    mov dx,no_vmm
    jz fail
    ; The VM manager learns the kernel's layout (again: it checks it is the
    ; same kernel): its segment and InDOS offset (DOSMGR table), first MCB.
    mov ax,1607h
    mov bx,15h
    xor cx,cx
    int 2Fh
    mov ax,es
    mov si,[es:bx+6]
    push ax
    mov ah,52h
    int 21h
    mov dx,[es:bx-2]
    pop bx
    push cs
    pop es
    movzx ebx,bx
    movzx ecx,si
    movzx edx,dx
    mov ax,VM_OP_VMM_INIT
    call far [entry]
    push cs
    pop ds
    push cs
    pop es
    mov dx,no_layout
    jc fail
    mov ax,VM_OP_VMM_CREATE
    call far [entry]
    push cs
    pop ds
    push cs
    pop es
    mov dx,fork_failed
    jc fail
    test cx,cx
    jnz child
    mov ax,4C00h                        ; system VM: done
    int 21h

child:
    ; Ancestors: the parent PSP chain, without the root (its own parent).
    mov ah,62h
    int 21h
    mov di,ancestors
    mov cx,8
.up:
    mov es,bx
    mov ax,[es:16h]                     ; parent of BX
    test ax,ax
    jz .top
    cmp ax,bx
    je .top
    mov es,ax
    cmp [es:16h],ax                     ; the root is its own parent
    je .top
    mov [di],ax
    add di,2
    mov bx,ax
    loop .up
.top:
    push cs
    pop es
    call restore_vectors
    ; Free every block they own in this VM's copy (the chain can merge
    ; while blocks are freed, so walk it again after each one).
.again:
    mov ah,52h
    int 21h
    mov ax,[es:bx-2]
    push cs
    pop es
.walk:
    mov es,ax
    mov dl,[es:0]
    mov bx,[es:1]
    mov si,ancestors
.check:
    mov cx,[cs:si]
    jcxz .next
    cmp cx,bx
    je .free
    add si,2
    jmp .check
.free:
    inc ax
    mov es,ax
    mov ah,49h
    int 21h
    push cs
    pop es
    jnc .again
    jmp .freed                          ; refused: keep what is left
.next:
    cmp dl,'Z'
    je .freed
    add ax,[es:3]
    inc ax
    jmp .walk
.freed:
    push cs
    pop es
    ; The program.
    mov [params+4],cs
    mov [params+8],cs
    mov [params+12],cs
    mov dx,program
    mov bx,params
    mov ax,4B00h
    int 21h
    push cs
    pop ds
    push cs
    pop es
    mov bx,-1
    jc .report
    mov ah,4Dh
    int 21h
    xor bh,bh
    mov bl,al
.report:
    mov ax,VM_OP_VMM_EXIT
    call far [entry]
.idle:
    hlt                                 ; the VM manager switches away
    jmp .idle

; Vectors that point into a block an ancestor owns would run freed memory
; in this VM: they get the value they had when the VM manager loaded
; (VMM_IVT), unless that one points into such a block too.
restore_vectors:
    mov di,ivt_copy
    mov ax,VM_OP_VMM_IVT
    call far [entry]
    push cs
    pop ds
    push cs
    pop es
    jc .done
    mov ah,52h
    int 21h
    mov ax,[es:bx-2]
    mov [first_mcb],ax
    push cs
    pop es
    xor bx,bx
.vector:
    push ds
    xor ax,ax
    mov ds,ax
    mov ax,[bx]
    mov dx,[bx+2]
    pop ds
    call in_ancestor_block
    jnc .next
    mov ax,[ivt_copy+bx]
    mov dx,[ivt_copy+bx+2]
    call in_ancestor_block
    jc .next
    push ds
    push cx
    xor cx,cx
    mov ds,cx
    cli
    mov [bx],ax
    mov [bx+2],dx
    sti
    pop cx
    pop ds
.next:
    add bx,4
    cmp bx,1024
    jb .vector
.done:
    ret

; DX:AX far pointer. CF=1 when it lies in a memory block owned by one of
; the ancestors. Preserves BX, DX, AX, DS.
in_ancestor_block:
    push ax
    push cx
    push si
    push di
    push es
    mov cx,ax
    shr cx,4
    add cx,dx                           ; paragraph of the pointer
    mov ax,[first_mcb]
.block:
    mov es,ax
    mov di,[es:1]                       ; owner
    mov si,ancestors
.owner:
    cmp word [si],0
    je .not_owned
    cmp [si],di
    je .owned
    add si,2
    jmp .owner
.owned:
    mov di,ax
    inc di                              ; first paragraph of the block
    cmp cx,di
    jb .not_owned
    add di,[es:3]
    cmp cx,di
    jb .inside
.not_owned:
    cmp byte [es:0],'Z'
    je .outside
    add ax,[es:3]
    inc ax
    jmp .block
.inside:
    stc
    jmp .out
.outside:
    clc
.out:
    pop es
    pop di
    pop si
    pop cx
    pop ax
    ret

fail:
    mov ah,9
    int 21h
    mov ax,4C01h
    int 21h

skip_blanks:
    lodsb
    cmp al,' '
    je skip_blanks
    cmp al,9
    je skip_blanks
    dec si
    ret

usage db 'Usage: VMFORK program [arguments]',13,10,'$'
no_vmm db 'VMFORK: the VM manager (CVSESS.DLL) is not loaded.',13,10,'$'
no_layout db 'VMFORK: the VM manager does not match this DOS kernel.',13,10,'$'
fork_failed db 'VMFORK: a new virtual machine could not be created.',13,10,'$'
entry dd 0
ancestors times 9 dw 0
first_mcb dw 0
params dw 0,child_tail,0,5Ch,0,6Ch,0
child_tail times 128 db 0
program times 128 db 0
ivt_copy times 1024 db 0
    align 2
    times 512 db 0
stack_top:
image_end:
