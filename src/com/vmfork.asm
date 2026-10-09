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
    cmp word [si],0502Fh               ; desktop-only /P followed by space
    jne .ordinary
    cmp byte [si+2],' '
    jne .ordinary
    mov byte [trace_parent],1
    add si,3
    call skip_blanks
.ordinary:
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
    ; Bind the exact BIOS boot device while still in VM0. All native disk
    ; code resides in CVSESSION, outside the constrained DOS kernel.
    call disk_prepare_parent
    mov ax,VM_OP_VMM_CREATE
    cmp byte [trace_parent],0
    je .create
    mov ax,VM_OP_VMM_CREATE_TRACE
.create:
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
    mov [trace_vm],bx
    mov ax,VM_OP_VMM_EXIT_CODE
    call far [entry]
    mov [cs:trace_generation],cx
    push cs
    pop ds
    push cs
    pop es
    cmp byte [trace_parent],0
    je .initial_memory
    mov word [trace_stage],15           ; before BIOS INT12 / DOS arena walk
    call trace_checkpoint
.initial_memory:
    call trace_initial_memory
    ; Ancestors: the parent PSP chain, including the shell root. Retain their
    ; PSP context and any extra allocation still referenced by an IVT entry.
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
    mov si,ancestors
    xor ax,ax
.count_ancestors:
    cmp word [si],0
    je .counted
    inc ax
    add si,2
    jmp .count_ancestors
.counted:
    mov [trace_parent_count],ax
    mov word [trace_stage],1
    or word [trace_progress],1
    call trace_checkpoint
    call restore_vectors
    jc .prepare_failed
    mov word [trace_stage],13
    call trace_checkpoint
    call detach_s3_guest
    jc .prepare_failed
    mov word [trace_stage],14
    call trace_checkpoint
    call prepare_compaction
    jc .prepare_failed
    mov word [trace_stage],2
    or word [trace_progress],2
    call trace_checkpoint
    cmp byte [compact_ready],0
    je .compaction_skipped
    mov word [trace_reason],0
    mov word [trace_stage],3
    or word [trace_progress],4
    call trace_checkpoint
    call compact_residents
    jc .compact_failed
.compaction_skipped:
    ; Free unreferenced extra blocks in this VM's copy. The chain can merge
    ; while blocks are freed, so validate and walk it again after each one.
.again:
    mov word [trace_stage],4
    or word [trace_progress],8
    ; Only persist once: each successful free starts this walk again.
    test word [trace_progress],16
    jnz .cleanup_walk
    or word [trace_progress],16
    call trace_checkpoint
.cleanup_walk:
    mov ah,52h
    int 21h
    call trace_dos_result
    mov word [cs:trace_reason],10h
    jc .cleanup_failed
    mov ax,[es:bx-2]
    mov [first_mcb],ax
    call validate_mcb_chain
    jc .cleanup_failed
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
    ; Other blocks with that owner (desktop modules, compositor, files) can
    ; be returned before EXEC when no inherited vector still references them.
    push ax
    inc ax
    cmp ax,bx
    pop ax
    je .next
    ; A live inherited vector keeps its complete allocation and owner PSP.
    ; Reclaim only blocks with no IVT reference, including segment aliases.
    call allocation_has_ivt_reference
    jnc .free_unreferenced
    call record_pinned_block
    jmp .next
.free_unreferenced:
    inc ax
    mov es,ax                            ; data segment of the block
    mov word [trace_reason],70h
    mov ax,0F149h                        ; fork-only free by MCB owner BX
    int 21h
    call trace_dos_result
    push cs
    pop es
    jnc .again
    cmp byte [compact_ready],0
    jne .cleanup_failed
    mov word [trace_stage],9
    or word [trace_progress],512
    mov word [trace_cleanup_reason],70h
    call trace_checkpoint
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
    call trace_remaining_memory
    jmp .launch
.launch:
    mov word [trace_reason],0
    mov word [trace_stage],5
    or word [trace_progress],32
    call trace_checkpoint
    mov word [trace_reason],0
    mov word [trace_stage],6
    or word [trace_progress],64
    call trace_checkpoint
    ; The program.
    mov [params+4],cs
    mov [params+8],cs
    mov [params+12],cs
    mov dx,program
    mov bx,params
    mov ax,4B00h
    int 21h
    call trace_dos_result
    push cs
    pop ds
    push cs
    pop es
    mov bx,-1
    jc .exec_failed
    mov ah,4Dh
    int 21h
    call trace_dos_result
    xor bh,bh
    mov bl,al
    mov word [trace_stage],8
    or word [trace_progress],256
    mov [trace_exit],bx
    call trace_checkpoint
    jmp .report
.exec_failed:
    mov bx,[trace_dos_ax]               ; separate launch failure from exit2
    and bx,0FFFh
    or bx,VM_EXIT_EXEC_ERROR
    mov word [trace_stage],7
    or word [trace_progress],128
    mov [trace_exit],bx
    call trace_checkpoint
.report:
    mov ax,VM_OP_VMM_EXIT
    call far [entry]
.idle:
    hlt                                 ; the VM manager switches away
    jmp .idle
.prepare_failed:
    mov word [trace_stage],10
    jmp .failure_trace
.cleanup_failed:
    mov word [trace_stage],12
    jmp .failure_trace
.compact_failed:
    mov word [trace_stage],11
.failure_trace:
    mov bx,[trace_reason]
    cmp bx,3Fh
    je .ancestor_vector_exit
    cmp bx,46h
    je .lfn_immediate_exit
    or bx,0F000h
    jmp .failure_exit_ready
.ancestor_vector_exit:
    ; The parent persists this even when the child's CVFL file cannot be
    ; written. F1xx identifies the exact unresolved interrupt number.
    mov bx,[trace_vector_ref]
    shr bx,2
    or bx,0F100h
    jmp .failure_exit_ready
.lfn_immediate_exit:
    ; F2xx preserves the actual failed byte of the LFN cmp ah,71h guard.
    mov bx,[trace_lfn_immediate]
    or bx,0F200h
.failure_exit_ready:
    mov [trace_exit],bx
    call trace_checkpoint
    mov dx,compaction_failed
    mov ah,9
    int 21h
    ; Relocation requires a destination below each resident. Best fit can
    ; pick a small hole above AUXSTACK when a packet driver is resident.
    ; First fit takes the freed low PSP interval instead.
    mov bx,[trace_exit]
    jmp .report

; Reclaim the desktop's PSP in this private VM. The running VMFORK image is
; still intact above it. The copied shell will never run in this VM.
prepare_compaction:
    mov byte [compact_ready],0
    cmp word [ancestors],0
    mov word [cs:trace_reason],01h
    je .skip
    mov ah,62h
    int 21h
    call trace_dos_result
    mov [cs:old_psp],bx
    mov ax,[cs:kernel_seg]
    mov es,ax
    cmp [es:KL_EXEC_PSP],bx
    mov word [cs:trace_reason],02h
    jne .skip
    cmp [es:KL_CURRENT_PSP],bx
    mov word [cs:trace_reason],03h
    jne .skip
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
    mov [cs:root_psp],ax
    call validate_root_reclaim
    jc .unsafe
    ; Preserve a referenced root tail before allocating a staging PSP or
    ; changing any DOS execution identity. The original VMFORK stays live.
    call root_tail_is_referenced
    jnc .stage
    or word [cs:trace_progress],800h
    mov ax,[cs:root_psp]
    dec ax
    mov es,ax
    call record_pinned_block
    mov word [cs:trace_reason],04h
    jmp .skip
.stage:
    ; A separately owned VMFORK PSP already provides a safe staging area.
    ; Reuse it only after proving its entire image/stack is outside the root;
    ; embedded launchers keep the existing allocate/copy/rebase path.
    call use_owned_staging
    jnc .stage_done
    ; VMFORK can itself be embedded in the shell's large PSP block. Copy its
    ; PSP, image, and stack into a separately allocated block before shrinking
    ; that block. The staging PSP keeps execution outside the reclaimed tail
    ; until the residents and VMFORK can move into the resulting low gap.
    jmp stage_vmfork
.stage_done:
    ; Check both recognized residents before releasing the root, but do not
    ; allocate their destinations yet. The conventional fallback needs the
    ; low hole that shrinking this validated, unreferenced root creates.
    mov byte [resident_validate_only],1
    mov bx,10h
    mov word [cs:vector_slot],bx
    call move_vector_resident
    jc .unsafe
    mov bx,21h
    mov [cs:vector_slot],bx
    call move_vector_resident
    jc .unsafe
    mov byte [resident_validate_only],0
    call root_tail_is_referenced
    jc .unsafe
    mov ax,[cs:kernel_seg]
    mov es,ax
    mov ax,[cs:root_psp]
    mov [es:KL_EXEC_PSP],ax
    mov [es:KL_CURRENT_PSP],ax
    mov es,ax
    mov bx,10h
    mov ax,0F14Ah
    int 21h
    call trace_dos_result
    mov word [cs:trace_reason],08h
    jc .restore_context
    call reparent_retained_ancestors
    ; The source PSP may have been inside the released shell tail, or it may
    ; have had its own surviving MCB. Release it if present before compacting.
    mov ax,[cs:source_psp]
    mov [cs:old_psp],ax
    cmp ax,[cs:stage_psp]
    je .source_released                ; in-place staging is still executing
    mov bx,ax
    mov es,bx
    mov ax,0F149h
    int 21h
    call trace_dos_result
    jnc .source_released
    call old_psp_is_free
    jc .restore_context
.source_released:
    mov ax,[cs:stage_psp]
    mov [cs:old_psp],ax
    mov ax,[cs:kernel_seg]
    mov es,ax
    mov ax,[cs:stage_psp]
    mov [es:KL_EXEC_PSP],ax
    mov [es:KL_CURRENT_PSP],ax
    jmp .ready
.restore_context:
    mov ax,[cs:kernel_seg]
    mov es,ax
    mov ax,[cs:stage_psp]
    mov [es:KL_EXEC_PSP],ax
    mov [es:KL_CURRENT_PSP],ax
    jmp .unsafe
.ready:
    mov word [cs:trace_reason],0
    mov byte [cs:compact_ready],1
    clc
    jmp .done
.unsafe:
    stc
    jmp .done
.skip:
    clc
.done:
    push cs
    pop ds
    push cs
    pop es
    ret

; CF=0: our existing conventional PSP is the staging block. No allocation,
; ownership, stack, PSP, or IVT mutation occurs until every predicate passes.
use_owned_staging:
    push ax
    push bx
    push cx
    push dx
    push es
    mov ax,cs
    cmp ax,[cs:old_psp]
    jne .copy
    cmp ax,[cs:root_end]
    jb .copy
    mov dx,ss
    cmp dx,ax
    jne .copy
    mov es,ax
    cmp word [es:0],20CDh
    jne .copy
    mov dx,[es:2]
    mov bx,ax
    dec bx
    mov es,bx
    mov bl,[es:0]
    cmp bl,'M'
    je .kind_ok
    cmp bl,'Z'
    jne .copy
.kind_ok:
    cmp [es:1],ax
    jne .copy
    mov bx,[es:3]
    cmp bx,(image_end-$$+100h+15)/16
    jb .copy
    add bx,ax
    jc .copy
    cmp bx,0A000h
    ja .copy
    ; PSP:2 is the initial EXEC ceiling; AH4A changes only the actual MCB.
    cmp dx,bx
    jb .copy
    cmp dx,0A000h
    ja .copy
    ; A dependent allocation would keep this old PSP live after rebasing.
    ; Such callers retain the established copied-staging path.
    mov bx,[cs:first_mcb]
    mov cx,64
.owners:
    mov es,bx
    cmp [es:1],ax
    jne .owner_next
    mov dx,bx
    inc dx
    cmp dx,ax
    jne .copy
.owner_next:
    cmp byte [es:0],'Z'
    je .owned
    add bx,[es:3]
    inc bx
    loop .owners
    jmp .copy
.owned:
    mov [cs:source_psp],ax
    mov [cs:stage_psp],ax
    clc
    jmp .done
.copy:
    stc
.done:
    pop es
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Stage a live copy before reclaiming an ancestor block. Entry is a jump from
; prepare_compaction, so the only return address on the old stack is the
; caller's return into child. Recreate that one-word stack after rebasing.
stage_vmfork:
    push bp
    mov bp,sp
    mov ax,[ss:bp+2]
    mov [cs:source_return],ax
    pop bp
    mov ax,[cs:old_psp]
    mov [cs:source_psp],ax
    mov bx,(image_end-$$+100h+15)/16
    mov ah,48h
    int 21h
    call trace_dos_result
    mov word [cs:trace_reason],30h
    jc .failed
    mov [cs:stage_psp],ax
    mov es,ax
    push ds
    mov ax,[cs:source_psp]
    mov ds,ax
    xor si,si
    xor di,di
    mov cx,(image_end-$$+100h+1)/2
    rep movsw
    pop ds
    mov ax,[cs:stage_psp]
    mov es,ax
    mov [es:2],ax
    add word [es:2],(image_end-$$+100h+15)/16
    mov ax,[cs:compact_parent]
    mov [es:16h],ax
    mov word [es:38h],0
    mov [es:3Ah],ax
    mov ax,[cs:stage_psp]
    dec ax
    mov es,ax
    inc ax
    mov [es:1],ax
    mov ax,[cs:kernel_seg]
    mov es,ax
    mov ax,[cs:stage_psp]
    mov [es:KL_EXEC_PSP],ax
    mov [es:KL_CURRENT_PSP],ax
    mov bx,ax
    mov ah,50h
    int 21h
    call trace_dos_result
    mov ax,[cs:stage_psp]
    push ax
    push word .rebased
    retf
.rebased:
    cli
    mov ax,cs
    mov ss,ax
    mov sp,stack_top
    sti
    push cs
    pop ds
    push cs
    pop es
    mov ax,[cs:stage_psp]
    mov [cs:old_psp],ax
    push word [cs:source_return]
    jmp prepare_compaction.stage_done
.failed:
    push cs
    pop ds
    push cs
    pop es
    stc
    ret

; The first user PSP is the first MCB's data segment. Only reclaim it when it
; is the known SHELL.COM root, owns that first MCB, and its PSP remains intact.
; MZ_LOAD_LIMIT_SEG is not used as a kernel boundary: the live runtime and DOS
; fixed data end immediately below COM_LOAD_SEG, the first MCB's data segment.
validate_root_reclaim:
    push ax
    push bx
    push dx
    push es
    mov ah,52h
    int 21h
    call trace_dos_result
    mov word [cs:trace_reason],10h
    jc .bad
    mov ax,[es:bx-2]
    mov [cs:first_mcb],ax
    inc ax
    cmp ax,[cs:root_psp]
    mov word [cs:trace_reason],11h
    jne .bad
    mov ax,[cs:root_psp]
    mov es,ax
    cmp word [es:0],0x20CD
    mov word [cs:trace_reason],12h
    jne .bad
    cmp [es:16h],ax
    mov word [cs:trace_reason],13h
    jne .bad
    cmp byte [es:80h],0
    mov word [cs:trace_reason],14h
    jne .bad
    ; SHELL.COM begins CLI / MOV AX,CS / MOV SS,AX. Combined with the
    ; authoritative first-MCB relationship, this excludes arbitrary PSPs.
    cmp word [es:100h],0x8CFA
    mov word [cs:trace_reason],15h
    jne .bad
    cmp word [es:102h],0x8EC8
    mov word [cs:trace_reason],16h
    jne .bad
    cmp word [es:104h],0xBCD0
    mov word [cs:trace_reason],17h
    jne .bad
    mov ax,[cs:root_psp]
    dec ax
    mov es,ax
    mov al,[es:0]
    cmp al,'M'
    je .mcb_type_ok
    cmp al,'Z'
    mov word [cs:trace_reason],18h
    jne .bad
.mcb_type_ok:
    mov ax,[cs:root_psp]
    cmp [es:1],ax
    mov word [cs:trace_reason],19h
    jne .bad
    mov dx,[es:3]
    cmp dx,10h
    mov word [cs:trace_reason],1Ah
    jbe .bad
    add ax,dx
    mov word [cs:trace_reason],1Bh
    jc .bad
    cmp ax,0A000h
    mov word [cs:trace_reason],1Ch
    ja .bad
    mov [cs:root_end],ax
    call validate_mcb_chain
    jc .bad
    mov si,ancestors
.check_ancestor:
    mov ax,[cs:si]
    or ax,ax
    jz .ancestors_valid
    cmp ax,[cs:root_psp]
    je .ancestors_valid
    mov es,ax
    cmp word [es:0],0x20CD
    mov word [cs:trace_reason],1Eh
    jne .bad
    add si,2
    jmp .check_ancestor
.ancestors_valid:
    mov ax,[cs:ancestors]
    or ax,ax
    jz .root_parent
    cmp ax,[cs:root_psp]
    je .parent_ready
    cmp ax,[cs:root_psp]
    jbe .parent_ready
    cmp ax,[cs:root_end]
    jae .parent_ready
.root_parent:
    mov ax,[cs:root_psp]
.parent_ready:
    mov [cs:compact_parent],ax
    clc
    jmp .exit
.bad:
    stc
.exit:
    pop es
    pop dx
    pop bx
    pop ax
    ret

; Validate the entire DOS chain before a PSP resize or any resident move.
; Every MCB must advance monotonically and the terminal Z block must end
; below the conventional-memory ceiling.
validate_mcb_chain:
    push ax
    push bx
    push cx
    push dx
    push es
    mov ax,[cs:first_mcb]
    mov cx,64
.next:
    cmp ax,0A000h
    mov word [cs:trace_reason],27h
    jae .bad
    mov es,ax
    mov dl,[es:0]
    cmp dl,'M'
    je .kind_ok
    cmp dl,'Z'
    mov word [cs:trace_reason],24h
    jne .bad
.kind_ok:
    mov bx,[es:3]
    add ax,bx
    mov word [cs:trace_reason],25h
    jc .bad
    inc ax
    mov word [cs:trace_reason],26h
    jz .bad
    cmp ax,0A000h
    mov word [cs:trace_reason],27h
    ja .bad
    cmp dl,'Z'
    je .good
    loop .next
    mov word [cs:trace_reason],28h
.bad:
    stc
    jmp .exit
.good:
    clc
.exit:
    pop es
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Parent PSPs embedded in the released shell block are disposable in this
; fork. Reparent retained descendants directly to the kept root PSP so DOS
; memory walks never follow a PSP pointer into reclaimed memory.
reparent_retained_ancestors:
    push ax
    push bx
    push si
    push es
    mov si,ancestors
.next:
    mov ax,[cs:si]
    or ax,ax
    jz .ok
    cmp ax,[cs:root_psp]
    je .advance
    cmp ax,[cs:root_psp]
    jbe .advance
    cmp ax,[cs:root_end]
    jb .advance
    mov es,ax
    mov bx,[es:16h]
    cmp bx,[cs:root_psp]
    jbe .advance
    cmp bx,[cs:root_end]
    jae .advance
    mov bx,[cs:root_psp]
    mov [es:16h],bx
    mov word [es:38h],0
    mov [es:3Ah],bx
.advance:
    add si,2
    jmp .next
.ok:
    pop es
    pop si
    pop bx
    pop ax
    ret

; All vectors into an ancestor-owned block must have been restored to a
; surviving VMM vector before the root shell block is shortened.
vectors_clear_ancestor_refs:
    push ax
    push bx
    push dx
    push ds
    xor bx,bx
.scan:
    push ds
    xor ax,ax
    mov ds,ax
    mov ax,[bx]
    mov dx,[bx+2]
    pop ds
    call in_ancestor_block
    jc .bad
    add bx,4
    cmp bx,1024
    jb .scan
    clc
    jmp .exit
.bad:
    mov word [cs:trace_reason],3Fh
    mov [cs:trace_vector_ref],bx
    mov [cs:trace_vector_target],ax
    mov [cs:trace_vector_target+2],dx
    stc
.exit:
    pop ds
    pop dx
    pop bx
    pop ax
    ret

; Move the two known resident COM images into the gap left by the desktop
; PSP. Their IVT hooks are private to this VM. A partial move ends only this
; private VM; it must never launch a game against a half-updated DOS arena.
compact_residents:
    mov byte [strategy_saved],0
    mov ax,5800h
    int 21h
    call trace_dos_result
    mov word [cs:trace_reason],50h
    jc .failed
    mov [old_strategy],bx
    mov byte [strategy_saved],1
    xor bx,bx                           ; first fit: freed low PSP gap
    mov ax,5801h
    int 21h
    call trace_dos_result
    mov word [cs:trace_reason],51h
    jc .failed
    mov bx,10h
    mov word [vector_slot],bx
    call move_vector_resident
    jc .failed
    mov bx,21h
    mov [vector_slot],bx
    call move_vector_resident
    jc .failed

    ; Allocate our own PSP immediately after the compacted residents. The
    ; old PSP can then be released without losing the EXEC return context.
    mov bx,(image_end-$$+100h+15)/16
    mov ah,48h
    int 21h
    call trace_dos_result
    mov word [cs:trace_reason],54h
    jc .failed
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
    mov ax,[compact_parent]
    mov [es:16h],ax
    mov word [es:38h],0
    mov [es:3Ah],ax
    ; The final PSP must own its allocation before the old execution PSP is
    ; retired. Otherwise an allocator rebuild retains that old owner PSP.
    mov ax,[new_psp]
    dec ax
    mov es,ax
    inc ax
    mov [es:1],ax
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
    ; Publish the rebased execution PSP's exact extent before retiring the
    ; old one. A direct EXEC PSP has no heap-table entry for F149 to remove;
    ; resize rebuilds the chain and drops that no-longer-active arena.
    push cs
    pop es
    mov bx,(image_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    call trace_dos_result
    mov word [cs:trace_reason],57h
    jc .fatal
    mov bx,[old_psp]
    mov es,bx
    mov ax,0F149h
    int 21h
    call trace_dos_result
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
    call trace_dos_result
    mov word [cs:trace_reason],56h
    jc .fatal
    push cs
    pop ds
    push cs
    pop es
    mov word [trace_reason],0
    clc
    jmp child.again
.failed:
    cmp byte [strategy_saved],0
    je .failed_return
    mov bx,[old_strategy]
    mov ax,5801h
    int 21h
.failed_return:
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
    push cs
    pop ds
    push cs
    pop es
    jmp child.compact_failed

; A DOS allocator rebuild may already have coalesced the retired PSP into a
; free MCB. Accept AH=F149h's "block not found" only after checking the
; canonical MCB chain covers the old segment with a free block.
old_psp_is_free:
    mov ah,52h
    int 21h
    call trace_dos_result
    mov word [cs:trace_reason],60h
    jc .bad
    mov ax,[es:bx-2]
    mov cx,64
.next:
    mov es,ax
    mov dl,[es:0]
    cmp dl,'M'
    je .kind_ok
    cmp dl,'Z'
    mov word [cs:trace_reason],61h
    jne .bad
.kind_ok:
    mov bx,ax
    add bx,[es:3]
    inc bx
    cmp [cs:old_psp],ax
    mov word [cs:trace_reason],62h
    jb .bad
    cmp [cs:old_psp],bx
    jae .advance
    cmp word [es:1],0
    mov word [cs:trace_reason],63h
    jne .bad
    clc
    ret
.advance:
    cmp dl,'Z'
    mov word [cs:trace_reason],64h
    je .bad
    mov ax,bx
    loop .next
    mov word [cs:trace_reason],65h
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
    mov ax,[bx]
    mov [cs:trace_vector_target],ax
    mov ax,[bx+2]
    mov [cs:trace_vector_target+2],ax
    pop ds
    cmp ax,1200h
    jb .skip_resident
    cmp ax,0A000h
    jae .skip_resident
    mov [old_resident],ax
    dec ax
    mov es,ax
    mov ax,[old_resident]
    cmp [es:1],ax
    mov word [cs:trace_reason],40h
    jne .bad
    mov ax,[es:3]
    cmp ax,20h
    mov word [cs:trace_reason],41h
    jb .bad
    cmp ax,500h
    mov word [cs:trace_reason],42h
    ja .bad
    mov [resident_paras],ax
    ; Only the boot AUXSTACK and this build's LFN hook may be relocated.
    mov bx,[vector_slot]
    cmp bx,10h
    jne .check_lfn
    cmp ax,4Fh
    mov word [cs:trace_reason],43h
    jne .bad
    mov ax,[old_resident]
    mov es,ax
    cmp word [es:100h],0E3E9h
    mov word [cs:trace_reason],44h
    jne .bad
    jmp .checked
.check_lfn:
    cmp bx,21h
    jne .skip_resident
    mov ax,[old_resident]
    mov es,ax
    movzx dx,byte [es:105h]
    mov [cs:trace_lfn_immediate],dx
    cmp word [es:103h],0FC80h
    mov word [cs:trace_reason],45h
    jne .bad
    cmp byte [es:105h],71h
    mov word [cs:trace_reason],46h
    jne .bad
.checked:
    cmp byte [resident_validate_only],1
    je .skip_resident
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
    call trace_dos_result
    mov word [cs:trace_reason],47h
    jc .bad
    mov [new_resident],ax
    cmp ax,[old_resident]
    mov word [cs:trace_reason],48h
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
    mov dx,[compact_parent]
    mov [es:16h],dx
    mov word [es:38h],0
    mov [es:3Ah],dx
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
    call trace_dos_result
    push cs
    pop ds
    push cs
    pop es
    clc
    ret
.reject_alloc:
    mov es,ax
    mov ah,49h
    int 21h
    call trace_dos_result
    jmp .bad
.skip_resident:
    push cs
    pop ds
    push cs
    pop es
    clc
    ret
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
    mov word [trace_ivt_tag],'IV'
    mov [trace_ivt_ready],bx
    mov [trace_ivt_classified],cx
    mov [trace_ivt_filtered],dx
    mov [trace_ivt_first],si
    mov ah,52h
    int 21h
    call trace_dos_result
    mov word [cs:trace_reason],10h
    jc .failed
    mov ax,[es:bx-2]
    mov [first_mcb],ax
    call validate_mcb_chain
    jc .failed
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
    clc
    ret
.failed:
    push cs
    pop ds
    push cs
    pop es
    stc
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

%include "src/com/vmfork_trace.inc"
%include "src/com/vmfork_disk.inc"
%include "src/com/vmfork_s3_guest.inc"
%include "src/com/vmfork_pins.inc"
%include "src/com/launch_mailbox.inc"

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
source_psp dw 0
stage_psp dw 0
source_return dw 0
root_psp dw 0
root_end dw 0
compact_parent dw 0
new_psp dw 0
old_resident dw 0
new_resident dw 0
resident_paras dw 0
resident_high db 0
resident_validate_only db 0
vector_slot dw 0
compact_ready db 0
old_strategy dw 0
strategy_saved db 0
compaction_failed db 'VMFORK: DOS memory compaction failed.',13,10,'$'
params dw 0,child_tail,0,5Ch,0,6Ch,0
child_tail times 128 db 0
program times 128 db 0
ivt_copy times 1024 db 0
    align 2
    times 512 db 0
stack_top:
image_end:
