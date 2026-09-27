; Actual V86 CLI starvation probe.  No HLT, INT, I/O, DOS or BIOS runs
; inside any measured busy loop.  RDTSC is available on the target Pentium 3.
; Results are observations, not a claim that the scheduler passes.
bits 16
org 100h
%include "src/vm/session_abi.inc"
%include "src/vm/session_scheduler_abi.inc"

start:
    cli
    mov ax,cs
    mov ss,ax
    mov sp,stack_top
    sti
    mov ds,ax
    mov es,ax
    cld
    mov bx,(program_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    jc fail
%ifdef VM_CLI_SEMANTICS
    mov bx,1000h
    mov ah,48h
    int 21h
    jc fail
    mov [extra_stack],ax
%endif
    xor di,di
    mov es,di
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    int 2Fh
    mov ax,es
    or ax,di
    jz fail
    mov [entry],di
    mov [entry+2],es
    ; The virtual-IF profile is negotiated at run time, not chosen by build.
    xor cx,cx
    mov ax,VM_OP_QUERY
    call far [cs:entry]
    jc fail
    test bx,VM_CAP_V86_VIRTUAL_IF
    jnz .profile_offered
    mov si,profile_missing
    call serial_puts
    jmp fail
.profile_offered:
    in al,21h
    mov [physical_master],al
    in al,0A1h
    mov [physical_slave],al
    push cs
    pop es
    mov di,descriptor
    mov cx,CVSCHED_BYTES
    mov ax,VM_OP_BIND_SCHED
    call far [cs:entry]
    jc fail
    mov byte [bound_flag],1
    mov ax,VM_OP_BEGIN
    call far [cs:entry]
    jc fail
    mov byte [active],1
    ; Jemm activates the requested profile on its next return to V86.
    xor bx,bx
    mov ax,VM_OP_IF_PROFILE
    call far [cs:entry]
    jc fail
    mov [profile_flags],ebx
    and ebx,VM_IF_AVAILABLE|VM_IF_REQUESTED|VM_IF_ACTIVE
    cmp ebx,VM_IF_AVAILABLE|VM_IF_REQUESTED|VM_IF_ACTIVE
    jne fail
%ifdef VM_CLI_SEMANTICS
    mov si,keyboard_ready
    call serial_puts
    xor ah,ah
    int 16h
    cmp al,'x'
    jne fail
%endif
    mov ax,40h
    mov fs,ax
    mov di,results
    call sample
    call busy
    call sample
%ifdef VM_CLI_SEMANTICS
    call semantics
    jc fail
%endif
    cli
    call sample
    call busy
    call sample
    sti
    call sample
    call busy
    call sample
    ; Restore the exact monitor resources before serial or DOS output.
    mov ax,VM_OP_END
    call far [cs:entry]
    jc fail
    mov byte [active],0
    in al,21h
    cmp al,[physical_master]
    jne fail
    in al,0A1h
    cmp al,[physical_slave]
    jne fail
    ; END released the profile: nothing requested or active afterwards.
    xor bx,bx
    mov ax,VM_OP_IF_PROFILE
    call far [cs:entry]
    jc fail
    test ebx,VM_IF_REQUESTED|VM_IF_ACTIVE
    jnz fail
    ; A declining owner keeps the physical-IF behaviour for its whole session.
    mov bx,2
    mov ax,VM_OP_IF_PROFILE
    call far [cs:entry]
    jc fail
    mov ax,VM_OP_BEGIN
    call far [cs:entry]
    jc fail
    mov byte [active],1
    xor bx,bx
    mov ax,VM_OP_IF_PROFILE
    call far [cs:entry]
    jc fail
    test ebx,VM_IF_REQUESTED|VM_IF_ACTIVE
    jnz fail
    test cx,cx
    jnz fail
    mov ax,VM_OP_END
    call far [cs:entry]
    jc fail
    mov byte [active],0
    mov bx,1
    mov ax,VM_OP_IF_PROFILE
    call far [cs:entry]
    jc fail
    mov ax,VM_OP_UNBIND_SCHED
    call far [cs:entry]
    jc fail
    mov byte [bound_flag],0
    mov si,profile_message
    call serial_puts
    mov eax,[profile_flags]
    call hex32
    mov si,crlf
    call serial_puts
%ifdef VM_CLI_SEMANTICS
    mov ax,[extra_stack]
    mov es,ax
    mov ah,49h
    int 21h
    jc fail
    mov word [extra_stack],0
    mov si,semantics_pass
    call serial_puts
%endif
    mov si,message
    call serial_puts
    mov si,results
    mov cx,12
.print:
    lodsd
    call hex32
    mov al,' '
    call serial_char
    loop .print
    mov si,crlf
    call serial_puts
    mov ax,4C00h
    int 21h

sample:
    mov eax,[descriptor+CVSCHED_V86_TICKS]
    stosd
    mov eax,[fs:6Ch]
    stosd
    ret

busy:
    rdtsc
    mov ebx,eax
.loop:
    rdtsc
    sub eax,ebx
    cmp eax,3000000000
    jb .loop
    ret

fail:
    sti
%ifdef VM_CLI_SEMANTICS
    call restore_irq
%endif
    cmp byte [active],0
    je .unbind
    mov ax,VM_OP_END
    call far [cs:entry]
.unbind:
    cmp byte [bound_flag],0
    je .print
    mov ax,VM_OP_UNBIND_SCHED
    call far [cs:entry]
.print:
    push cs
    pop ds
    mov si,error
    call serial_puts
    mov ax,4C01h
    int 21h

%ifdef VM_CLI_SEMANTICS
semantics:
    ; Preserve the guest's real timer vector and use an ordinary DOS IRQ
    ; handler, so no host model callback can fabricate observed delivery.
    pushf
    cli
    xor ax,ax
    mov es,ax
    mov eax,[es:20h]
    mov [old_irq],eax
    mov word [es:20h],guest_irq
    mov [es:22h],cs
    mov byte [irq_hooked],1
    push cs
    pop es
    popf
    mov byte [test_stage],1
    pushf
    cli
    call busy
    pushf
    pop ax
    test ax,200h
    jnz .bad_pop
    popf
    call require_irq
    jc .bad
    inc byte [test_stage]
    pushfd
    cli
    call busy
    pushfd
    pop eax
    test eax,200h
    jnz .bad_pop32
    popfd
    call require_irq
    jc .bad
    ; POPFD restores the architectural arithmetic flags, not just IF.
    push dword 0247h
    popfd
    pushfd
    pop eax
    and eax,08D5h
    cmp eax,0045h
    jne .bad
    inc byte [test_stage]
    pushf
    push cs
    push word .iret16
    cli
    iret
.iret16:
    call require_if
    jc .bad
    inc byte [test_stage]
    pushfd
    xor eax,eax
    mov ax,cs
    push eax
    push dword .iret32
    cli
    iretd
.iret32:
    call require_if
    jc .bad
    inc byte [test_stage]
    ; SS differs from CS. The interrupt frame wraps between FFFE and 0000.
    pushf
    cli
    mov [saved_sp],sp
    mov ax,[extra_stack]
    mov ss,ax
    mov sp,4
    pushf
    push cs
    push word .wrapped_iret
    iret
.wrapped_iret:
    mov ax,cs
    mov ss,ax
    mov sp,[saved_sp]
    popf
    call require_if
    jc .bad
    ; The 32-bit IRET frame also crosses a 16-bit stack boundary.
    pushf
    cli
    mov [saved_sp],sp
    mov ax,[extra_stack]
    mov ss,ax
    mov sp,8
    pushfd
    xor eax,eax
    mov ax,cs
    push eax
    push dword .wrapped_iretd
    iretd
.wrapped_iretd:
    mov ax,cs
    mov ss,ax
    mov sp,[saved_sp]
    popf
    call require_if
    jc .bad
    inc byte [test_stage]
    mov ah,30h
    int 21h
    cmp al,0
    je .bad
    ; Queue an IRQ while IF=0, then verify its handler cannot observe the
    ; instruction immediately after STI as still unexecuted.
    inc byte [test_stage]
    cli
    call busy
    mov byte [shadow_guard],0
    mov byte [check_shadow],1
    sti
    mov byte [shadow_guard],1
    call require_irq
    mov byte [check_shadow],0
    jc .bad
    cmp word [shadow_violations],0
    jne .bad
    ; A trapped instruction immediately after STI must end the shadow too.
    cli
    call busy
    mov ah,30h
    sti
    int 21h
    call require_irq
    jc .bad
    cli
    call busy
    sti
    pushf
    pop ax
    test ax,100h
    jnz .bad
    call require_irq
    jc .bad
    ; Virtual IMR must suppress guest IRQ0 while physical host ticks run.
    inc byte [test_stage]
    in al,21h
    mov [old_mask],al
    or al,1
    out 21h,al
    mov eax,[irq_hits]
    mov [old_hits],eax
    mov eax,[descriptor+CVSCHED_V86_TICKS]
    mov [old_host],eax
    call busy
    mov eax,[irq_hits]
    cmp eax,[old_hits]
    jne .restore_mask_bad
    mov eax,[descriptor+CVSCHED_V86_TICKS]
    sub eax,[old_host]
    cmp eax,3
    jb .restore_mask_bad
    mov al,[old_mask]
    out 21h,al
    call require_irq
    jc .bad
    ; Without a virtual EOI, repeated IRQ0 edges stay pending. Explicit EOI
    ; must release the private ISR without depending on physical PIC ISR.
    inc byte [test_stage]
    mov byte [withhold_eoi],1
    call require_irq
    jc .bad
    mov eax,[irq_hits]
    mov [old_hits],eax
    call busy
    mov eax,[irq_hits]
    cmp eax,[old_hits]
    jne .bad_eoi
    mov al,0Bh
    out 20h,al
    in al,20h
    test al,1
    jz .bad_eoi
    mov byte [withhold_eoi],0
    mov al,60h
    out 20h,al
    call require_irq
    jc .bad
    ; CLI + HLT must keep host service live and must not reflect guest IRQs.
    inc byte [test_stage]
    cli
    mov eax,[irq_hits]
    mov [old_hits],eax
    mov eax,[descriptor+CVSCHED_V86_TICKS]
    mov [old_host],eax
    mov cx,4
.halt:
    hlt
    loop .halt
    mov eax,[irq_hits]
    cmp eax,[old_hits]
    jne .bad
    mov eax,[descriptor+CVSCHED_V86_TICKS]
    sub eax,[old_host]
    cmp eax,3
    jb .bad
    sti
    call require_irq
    jc .bad
    cmp word [irq_iopl_bad],0
    jne .bad
    call restore_irq
    clc
    ret
.restore_mask_bad:
    mov al,[old_mask]
    out 21h,al
    jmp .bad
.bad_eoi:
    mov byte [withhold_eoi],0
    mov al,20h
    out 20h,al
    jmp .bad
.bad_pop:
    popf
    jmp .bad
.bad_pop32:
    popfd
.bad:
    sti
    mov si,semantics_fail
    call serial_puts
    movzx eax,byte [test_stage]
    call hex32
    mov si,crlf
    call serial_puts
    stc
    ret
require_if:
    pushf
    pop ax
    test ax,200h
    jz .bad
    clc
    ret
.bad:
    stc
    ret
require_irq:
    mov eax,[irq_hits]
    push eax
    call busy
    pop eax
    cmp eax,[irq_hits]
    je .bad
    clc
    ret
.bad:
    stc
    ret
guest_irq:
    push ax
    push bp
    mov bp,sp
    mov ax,[ss:bp+8]
    and ax,3000h
    cmp ax,3000h
    je .iopl_ok
    inc word [cs:irq_iopl_bad]
.iopl_ok:
    pop bp
    inc dword [cs:irq_hits]
    cmp byte [cs:check_shadow],0
    je .eoi
    cmp byte [cs:shadow_guard],1
    je .eoi
    inc word [cs:shadow_violations]
.eoi:
    cmp byte [cs:withhold_eoi],0
    jne .done
    mov al,20h
    out 20h,al
.done:
    pop ax
    iret
restore_irq:
    cmp byte [cs:irq_hooked],0
    je .done
    pushf
    cli
    push ax
    push es
    xor ax,ax
    mov es,ax
    mov eax,[cs:old_irq]
    mov [es:20h],eax
    mov byte [cs:irq_hooked],0
    pop es
    pop ax
    popf
.done:
    ret
extra_stack dw 0
saved_sp dw 0
old_irq dd 0
irq_hooked db 0
old_mask db 0
test_stage db 0
irq_hits dd 0
old_hits dd 0
old_host dd 0
check_shadow db 0
shadow_guard db 1
withhold_eoi db 0
shadow_violations dw 0
irq_iopl_bad dw 0
semantics_pass db '[V86CLI] FLAGS/INT/IRET/stack/STI/PIC/EOI/HLT PASS',13,10,0
semantics_fail db '[V86CLI] SEMANTICS FAIL stage=',0
keyboard_ready db '[V86CLI] BIOS KEYBOARD READY',13,10,0
%endif

hex32:
    pushad
    mov ebx,eax
    mov cx,8
.digit:
    rol ebx,4
    mov al,bl
    and al,15
    add al,'0'
    cmp al,'9'
    jbe .emit
    add al,'A'-'9'-1
.emit:
    call serial_char
    loop .digit
    popad
    ret
serial_puts:
    lodsb
    test al,al
    jz .done
    call serial_char
    jmp serial_puts
.done:
    ret
serial_char:
    push ax
    push bx
    push cx
    push dx
    mov bl,al
    mov cx,65535
    mov dx,3FDh
.wait:
    in al,dx
    test al,20h
    jnz .send
    loop .wait
.send:
    mov dx,3F8h
    mov al,bl
    out dx,al
    pop dx
    pop cx
    pop bx
    pop ax
    ret

entry dd 0
profile_flags dd 0
profile_missing db '[V86CLI] VIRTUAL-IF PROFILE NOT OFFERED BY JEMM',13,10,0
profile_message db '[V86CLI] PROFILE NEGOTIATED ALLOW/DECLINE/RELEASE PASS flags=',0
physical_master db 0
physical_slave db 0
bound_flag db 0
active db 0
message db '[V86CLI] COUNTERS ',0
error db '[V86CLI] SETUP OR CLEANUP FAIL',13,10,0
crlf db 13,10,0
align 4
results times 12 dd 0
descriptor:
    dd CVSCHED_MAGIC
    dw CVSCHED_VERSION,CVSCHED_BYTES
    times CVSCHED_BYTES-8 db 0
times 512 db 0
stack_top:
program_end:
