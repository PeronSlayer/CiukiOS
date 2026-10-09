; Detach the pinned S3VBEFIX 0.6 conventional-memory copy in this child VM.
; The system VM owns the physical correction; this child will use virtual VGA.
; No TSR entry, video BIOS, device port or MMIO is invoked. See vmfork-s3-guest.md.
; Preserves registers and flags except returned CF. Unknown TSRs are untouched.
detach_s3_guest:
    pushf
    pushad
    push ds
    push es
    cld
    cmp word [cs:trace_vm],1
    jb .skip
    cmp word [cs:trace_vm],3
    ja .skip
    xor ax,ax
    mov ds,ax
    mov bx,[40h]
    mov dx,[42h]
    cmp dx,1200h
    jb .skip
    cmp dx,0A000h-20h
    ja .skip                          ; header stays in conventional RAM;
                                      ; UMBs/device windows are never touched.
    cmp bx,48h
    jne .skip
    mov es,dx
    cmp dword [es:40h],00604B66h      ; pinned fK / version0060h
    jne .skip
    mov word [cs:trace_reason],80h
    cmp dword [es:48h],463E802Eh
    jne .bad
    cmp dword [es:4Ch],05740000h
    jne .bad
    cmp byte [es:50h],0EAh
    jne .bad
    cmp word [es:46h],0              ; no inherited call is in progress
    jne .bad
    mov [cs:trace_s3_psp],dx
    mov ax,[es:51h]
    mov [cs:trace_s3_previous],ax
    mov ax,[es:53h]
    mov [cs:trace_s3_previous+2],ax
    mov ax,dx
    dec ax
    mov es,ax
    mov word [cs:trace_reason],81h
    cmp byte [es:0],'M'
    je .mcb_type
    cmp byte [es:0],'Z'
    jne .bad
.mcb_type:
    cmp [es:1],dx
    jne .bad
    mov cx,[es:3]
    cmp cx,20h
    jb .bad
    cmp cx,500h
    ja .bad
    mov [cs:trace_s3_paras],cx
    add cx,dx
    jc .bad
    cmp cx,0A000h
    ja .bad
    mov [cs:s3_guest_end],cx
    ; Require the known auxiliary-stack predecessor before changing the IVT.
    mov word [cs:trace_reason],82h
    cmp word [cs:trace_s3_previous],470h
    jne .bad
    mov ax,[cs:trace_s3_previous+2]
    cmp ax,1200h
    jb .bad
    cmp ax,0A000h-4Fh
    ja .bad
    mov es,ax
    cmp word [es:100h],0E3E9h
    jne .bad
    mov dx,ax
    dec ax
    mov es,ax
    cmp byte [es:0],'M'
    jne .bad
    cmp [es:1],dx
    jne .bad
    cmp word [es:3],4Fh
    jne .bad
    ; Walk the actual bounded arena: both blocks must be members, not merely
    ; plausible data at pointers. trace_initial_memory measured it read-only.
    mov word [cs:trace_reason],83h
    cmp word [cs:trace_memory_valid],1
    jne .bad
    mov ax,[cs:first_mcb]
    mov cx,64
    xor si,si
.arena:
    mov es,ax
    mov di,ax
    inc di
    cmp di,[cs:trace_s3_psp]
    jne .not_s3
    or si,1
.not_s3:
    cmp di,[cs:trace_s3_previous+2]
    jne .not_aux
    or si,2
.not_aux:
    cmp byte [es:0],'Z'
    je .arena_done
    cmp byte [es:0],'M'
    jne .bad
    add ax,[es:3]
    jc .bad
    inc ax
    jz .bad
    cmp ax,0A000h
    jae .bad
    loop .arena
    jmp .bad
.arena_done:
    cmp si,3
    jne .bad
    ; Refuse any other hook into this copy, including S3VBEFIX's optional /I.
    mov word [cs:trace_reason],84h
    mov dx,[cs:trace_s3_psp]
    dec dx
    xor bx,bx
.vector:
    cmp bx,40h
    je .next
    mov ax,[bx]
    shr ax,4
    add ax,[bx+2]
    jc .next
    cmp ax,dx
    jb .next
    cmp ax,[cs:s3_guest_end]
    jb .other_ref
.next:
    add bx,4
    cmp bx,1024
    jb .vector
    ; INT10 and the MCB live in the child's private pages00h..9Fh.
    mov eax,[cs:trace_s3_previous]
    pushf
    cli
    mov [40h],eax
    popf
    push cs
    pop ds
    mov bx,[trace_s3_psp]
    mov es,bx                         ; F149h's exact-block owner identity
    mov ax,0F149h
    int 21h
    call trace_dos_result
    mov word [cs:trace_reason],85h
    jc .bad
    or word [cs:trace_progress],400h
    mov word [cs:trace_reason],0
.skip:
    pop es
    pop ds
    popad
    popf
    clc
    ret
.other_ref:
    mov [cs:trace_vector_ref],bx
    jmp .bad
.bad:
    pop es
    pop ds
    popad
    popf
    stc
    ret
s3_guest_end dw 0
