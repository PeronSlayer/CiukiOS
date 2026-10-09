; Input: ES:DI points at a complete CVFL record, BX is its generation.
; The enclosing foreground checkpoint preserves every register and FLAGS.
; A traced launch fails closed if publication or parent persistence fails.
launch_mailbox_publish:
    mov [cs:launch_mailbox_generation],bx
    xor cx,cx
    mov ax,VM_OP_VMM_LAUNCH_TRACE
    call far [cs:entry]
    jc launch_mailbox_failed
.wait:
    mov bx,[cs:launch_mailbox_generation]
    mov cx,3
    mov ax,VM_OP_VMM_LAUNCH_TRACE
    call far [cs:entry]
    jc launch_mailbox_failed
    test dx,dx
    jz .done
    ; HLT uses the existing canonical scheduler callback. Even if an IRQ
    ; wakes this child before VM0 runs, the loop cannot cross the checkpoint.
    sti
    hlt
    jmp .wait
.done:
    ret
launch_mailbox_failed:
    mov bx,0F0E1h
    mov ax,VM_OP_VMM_EXIT
    call far [cs:entry]
.halt:
    sti
    hlt
    jmp .halt
launch_mailbox_generation dw 0
