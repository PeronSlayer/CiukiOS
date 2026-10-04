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
%include "kernel_layout.inc"

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
    mov [cs:kernel_seg],ax
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
    ; Ancestors: the parent PSP chain, including the shell root. Its PSP
    ; block stays; its extra allocations are unused in this child VM.
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
    mov [di],ax
    add di,2
    mov es,ax
    cmp [es:16h],ax                     ; include the root as an owner, too:
    je .top                             ; its module blocks belong to the desktop
    mov bx,ax
    loop .up
.top:
    push cs
    pop es
    call restore_vectors
    call prepare_compaction
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
    ; Keep each ancestor's own PSP block: DOS still needs the parent chain.
    ; Other blocks with that owner (desktop modules, compositor, files) are
    ; unused in this VM and must be returned before a game asks for memory.
    push ax
    inc ax
    cmp ax,bx
    pop ax
    je .next
    inc ax
    mov es,ax                            ; data segment of the block
    mov ax,0F149h                        ; fork-only free by MCB owner BX
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
    cmp byte [compact_ready],0
    je .launch
    call compact_residents
    jc .compact_failed
.launch:
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
.compact_failed:
    mov dx,compaction_failed
    mov ah,9
    int 21h
    ; Relocation requires a destination below each resident. Best fit can
    ; pick a small hole above AUXSTACK when a packet driver is resident.
    ; First fit takes the freed low PSP interval instead.
    xor bx,bx
    jmp .report

; Reclaim the desktop's PSP in this private VM. The running VMFORK image is
; still intact above it. The copied shell will never run in this VM.
prepare_compaction:
    mov byte [compact_ready],0
    cmp word [ancestors],0
    je .done
    mov ah,62h
    int 21h
    mov [cs:old_psp],bx
    mov ax,[cs:kernel_seg]
    mov es,ax
    cmp [es:KL_EXEC_PSP],bx
    jne .done
    cmp [es:KL_CURRENT_PSP],bx
    jne .done
    ; A nested launcher (for example VMCTEST -> VMFORK) keeps its immediate
    ; parent's PSP above the resident hooks. Reclaim the root shell PSP,
    ; which is the lowest ancestor and leaves a gap below AUXSTACK/LFN.
    mov si,ancestors
.root:
    mov ax,[cs:si]
    test ax,ax
    jz .root_ready
    mov dx,ax
    add si,2
    cmp si,ancestors+16
    jb .root
.root_ready:
    mov ax,dx
    mov [es:KL_EXEC_PSP],ax
    mov [es:KL_CURRENT_PSP],ax
    mov es,ax
    mov bx,10h
    mov ax,0F14Ah
    int 21h
    jnc .ready
    mov ax,[cs:kernel_seg]
    mov es,ax
    mov ax,[cs:old_psp]
    mov [es:KL_EXEC_PSP],ax
    mov [es:KL_CURRENT_PSP],ax
    jmp .done
.ready:
    mov byte [cs:compact_ready],1
.done:
    push cs
    pop ds
    push cs
    pop es
    ret

; Move the two known resident COM images into the gap left by the desktop
; PSP. Their IVT hooks are private to this VM. A partial move ends only this
; private VM; it must never launch a game against a half-updated DOS arena.
compact_residents:
    mov ax,5800h
    int 21h
    mov [old_strategy],bx
    xor bx,bx                           ; first fit: freed low PSP gap
    mov ax,5801h
    int 21h
    mov bx,10h
    mov word [vector_slot],bx
    call move_vector_resident
    jc .done
    mov bx,21h
    mov [vector_slot],bx
    call move_vector_resident
    jc .done

    ; Allocate our own PSP immediately after the compacted residents. The
    ; old PSP can then be released without losing the EXEC return context.
    mov bx,(image_end-$$+100h+15)/16
    mov ah,48h
    int 21h
    jc .done
    mov [new_psp],ax
    mov es,ax
    push ds
    push cs
    pop ds
    xor si,si
    xor di,di
    mov cx,(image_end-$$+100h+1)/2
    rep movsw
    pop ds
    mov ax,[new_psp]
    mov es,ax
    mov [es:2],ax
    add word [es:2],(image_end-$$+100h+15)/16
    mov ax,[ancestors]
    mov [es:16h],ax
    mov ax,[kernel_seg]
    mov es,ax
    mov ax,[new_psp]
    mov [es:KL_EXEC_PSP],ax
    mov [es:KL_CURRENT_PSP],ax
    cli
    mov ss,ax
    mov sp,stack_top
    sti
    mov ds,ax
    mov es,ax
    push ax
    push word .rebased
    retf
.rebased:
    mov bx,[old_psp]
    mov es,bx
    mov ax,0F149h
    int 21h
    pushf
    push cs
    pop es
    push cs
    pop ds
    popf
    jnc .old_psp_done
    call old_psp_is_free
    jc .fatal
.old_psp_done:
    mov bx,[old_strategy]
    mov ax,5801h
    int 21h
    push cs
    pop ds
    push cs
    pop es
    jmp child.launch
.done:
    mov bx,[old_strategy]
    mov ax,5801h
    int 21h
    push cs
    pop ds
    push cs
    pop es
    stc
    ret
.fatal:
    mov bx,[old_strategy]
    mov ax,5801h
    int 21h
    jmp child.compact_failed

; A DOS allocator rebuild may already have coalesced the retired PSP into a
; free MCB. Accept AH=F149h's "block not found" only after checking the
; canonical MCB chain covers the old segment with a free block.
old_psp_is_free:
    mov ah,52h
    int 21h
    jc .bad
    mov ax,[es:bx-2]
    mov cx,64
.next:
    mov es,ax
    mov dl,[es:0]
    cmp dl,'M'
    je .kind_ok
    cmp dl,'Z'
    jne .bad
.kind_ok:
    mov bx,ax
    add bx,[es:3]
    inc bx
    cmp [cs:old_psp],ax
    jb .bad
    cmp [cs:old_psp],bx
    jae .advance
    cmp word [es:1],0
    jne .bad
    clc
    ret
.advance:
    cmp dl,'Z'
    je .bad
    mov ax,bx
    loop .next
.bad:
    stc
    ret

; BX = vector whose owner PSP is a low-memory resident COM image.
move_vector_resident:
    push ds
    xor ax,ax
    mov ds,ax
    shl bx,1
    shl bx,1
    mov ax,[bx+2]
    pop ds
    cmp ax,1200h
    jb .bad
    cmp ax,0A000h
    jae .bad
    mov [old_resident],ax
    dec ax
    mov es,ax
    mov ax,[old_resident]
    cmp [es:1],ax
    jne .bad
    mov ax,[es:3]
    cmp ax,20h
    jb .bad
    cmp ax,500h
    ja .bad
    mov [resident_paras],ax
    ; Only the boot AUXSTACK and this build's LFN hook may be relocated.
    mov bx,[vector_slot]
    cmp bx,10h
    jne .check_lfn
    cmp ax,4Fh
    jne .bad
    mov ax,[old_resident]
    mov es,ax
    cmp word [es:100h],0E3E9h
    jne .bad
    jmp .checked
.check_lfn:
    cmp bx,21h
    jne .bad
    mov ax,[old_resident]
    mov es,ax
    cmp word [es:103h],0FC80h
    jne .bad
    cmp byte [es:105h],71h
    jne .bad
.checked:
    ; CVSESSION claims a distinct Jemm UMB and records this VM as owner in
    ; one ring-0 operation.  Its KILL path releases the block even if this
    ; child never returns.  If no UMB fits, keep the proven low compaction.
    mov bx,[resident_paras]
    mov ax,VM_OP_VMM_UMB_ALLOC
    call far [entry]
    push cs
    pop ds
    push cs
    pop es
    jc .low_alloc
    mov [new_resident],bx
    mov byte [resident_high],1
    jmp .copy
.low_alloc:
    mov byte [resident_high],0
    mov ax,[resident_paras]
    mov bx,ax
    mov ah,48h
    int 21h
    jc .bad
    mov [new_resident],ax
    cmp ax,[old_resident]
    jae .reject_alloc
.copy:
    mov ax,[new_resident]
    mov es,ax
    ; AH=48h assigns the block to VMFORK's old PSP. This relocated TSR
    ; must own its block itself before that PSP is released below.
    cmp byte [resident_high],1
    je .owner_ready
    push ax
    dec ax
    mov es,ax
    pop ax
    mov [es:1],ax
    mov es,ax
.owner_ready:
    mov ax,[old_resident]
    mov ds,ax
    xor si,si
    xor di,di
    mov cx,[cs:resident_paras]
    ; The MCB size already includes the resident's PSP and program image.
    shl cx,3
    rep movsw
    push cs
    pop ds
    mov ax,[new_resident]
    mov es,ax
    mov bx,ax
    add bx,[resident_paras]
    mov [es:2],bx                     ; relocated PSP end segment
    mov dx,[ancestors]
    mov [es:16h],dx
    push ds
    xor dx,dx
    mov ds,dx
    mov bx,[cs:vector_slot]
    shl bx,1
    shl bx,1
    cli
    mov [bx+2],ax
    sti
    pop ds
    mov bx,[old_resident]
    mov es,bx
    mov ax,0F149h
    int 21h
    push cs
    pop ds
    push cs
    pop es
    ret
.reject_alloc:
    mov es,ax
    mov ah,49h
    int 21h
    jmp .bad
.bad:
    push cs
    pop ds
    push cs
    pop es
    stc
    ret

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
kernel_seg dw 0
old_psp dw 0
new_psp dw 0
old_resident dw 0
new_resident dw 0
resident_paras dw 0
resident_high db 0
vector_slot dw 0
compact_ready db 0
old_strategy dw 0
compaction_failed db 'VMFORK: DOS memory compaction failed.',13,10,'$'
params dw 0,child_tail,0,5Ch,0,6Ch,0
child_tail times 128 db 0
program times 128 db 0
ivt_copy times 1024 db 0
    align 2
    times 512 db 0
stack_top:
image_end:
