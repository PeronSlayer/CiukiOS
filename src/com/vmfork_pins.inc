; IVT references keep copied conventional allocations alive in this child.
; No vector is exempted, rewritten or invoked. See t23-intda-retained-allocations.md.

; AX inclusive first paragraph, DX exclusive end. The caller validated the
; conventional arena. Preserve every register and FLAGS except returned CF.
interval_has_ivt_reference:
    pushfd
    pushad
    push ds
    mov bp,ax
    mov di,dx
    xor ax,ax
    mov ds,ax
    xor bx,bx
.scan:
    mov ax,[bx]
    shr ax,4
    add ax,[bx+2]
    jc .next                         ; above 1 MiB is not conventional memory
    cmp ax,bp
    jb .next
    cmp ax,di
    jae .next
    mov word [cs:trace_reason],3Fh
    mov [cs:trace_vector_ref],bx
    mov eax,[bx]
    mov [cs:trace_vector_target],eax
    pop ds
    popad
    popfd
    stc
    ret
.next:
    add bx,4
    cmp bx,1024
    jb .scan
    pop ds
    popad
    popfd
    clc
    ret

root_tail_is_referenced:
    pushfd
    pushad
    push es
    mov ax,[cs:root_psp]
    add ax,10h
    mov dx,[cs:root_end]
    call interval_has_ivt_reference
    jc .found
    ; A live extra allocation also keeps its owner PSP. Some launcher PSPs
    ; are embedded in the root tail without a separate self-owned MCB.
    ; Retain their backing root if a referenced block belongs to one of them.
    mov ax,[cs:first_mcb]
    mov cx,64
.block:
    mov es,ax
    mov bx,[es:1]
    mov dx,[cs:root_psp]
    add dx,10h
    cmp bx,dx
    jb .next
    cmp bx,[cs:root_end]
    jae .next
    mov si,ancestors
.owner:
    cmp word [cs:si],0
    je .next
    cmp bx,[cs:si]
    je .check_reference
    add si,2
    jmp .owner
.check_reference:
    call allocation_has_ivt_reference
    jnc .next
    call record_pinned_block
    jmp .found
.next:
    cmp byte [es:0],'Z'
    je .none
    add ax,[es:3]
    inc ax
    loop .block
.none:
    pop es
    popad
    popfd
    clc
    ret
.found:
    pop es
    popad
    popfd
    stc
    ret

; AX and ES name the MCB whose type/end was validated in this cleanup sweep.
allocation_has_ivt_reference:
    push dx
    mov dx,ax
    add dx,[es:3]
    inc dx
    call interval_has_ivt_reference
    pop dx
    ret

; AX/ES name a pinned MCB. Retain the first pin's complete diagnostic tuple;
; later sweeps may observe the same block again, so count observations only.
record_pinned_block:
    pushfd
    pushad
    cmp word [cs:trace_pin_events],0
    jne .count
    mov [cs:trace_pin_mcb],ax
    mov ax,[es:1]
    mov [cs:trace_pin_owner],ax
    mov ax,[es:3]
    mov [cs:trace_pin_paras],ax
    mov ax,[cs:trace_vector_ref]
    mov [cs:trace_pin_vector],ax
    mov eax,[cs:trace_vector_target]
    mov [cs:trace_pin_target],eax
.count:
    inc word [cs:trace_pin_events]
    or word [cs:trace_progress],1000h
    popad
    popfd
    ret
