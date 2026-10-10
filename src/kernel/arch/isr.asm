; Interrupt/exception stubs, syscall entry, double-fault task, context
; switch, user entry and user-copy helpers with fault fixups.
; SPDX-License-Identifier: GPL-2.0-only

bits 32
section .text

extern proc_trap_dispatch
extern df_handler

%macro ISR_NOERR 1
isr_%1:
    push dword 0
    push dword %1
    jmp isr_common
%endmacro
%macro ISR_ERR 1
isr_%1:
    push dword %1
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
%assign i 20
%rep 28
ISR_NOERR i
%assign i i+1
%endrep

global isr_syscall
isr_syscall:
    push dword 0
    push dword 0x80
    jmp isr_common

isr_common:
    pusha
    push ds
    push es
    push fs
    push gs
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    cld
    push esp
    call proc_trap_dispatch
    add esp, 4
global trap_return
trap_return:
    ; F1 V86: hardware cleared the protected-mode segment registers on
    ; entry. Real-mode ES/DS/FS/GS are in the extended IRET frame, not in
    ; these four selector saves. Do not load real-mode values as selectors.
    test dword [esp + 64], 0x20000
    jnz .v86
    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8
    iret
.v86:
    add esp, 16
    popa
    add esp, 8
    iretd

section .rodata
global isr_table
isr_table:
%assign i 0
%rep 48
    dd isr_ %+ i
%assign i i+1
%endrep

section .text
; Double fault arrives through a task gate on its own TSS and stack.
global df_task_entry
df_task_entry:
    cli
    call df_handler
.h: hlt
    jmp .h

; void switch_context(uint32_t *old_esp, uint32_t new_esp)
global switch_context
switch_context:
    mov eax, [esp + 4]
    mov edx, [esp + 8]
    push ebp
    push ebx
    push esi
    push edi
    mov [eax], esp
    mov esp, edx
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; First activation of a task: the prepared kernel stack holds a full
; trap_frame; return through trap_return.
global task_first_entry
task_first_entry:
    jmp trap_return

; int copy_user(void *dst, const void *src, uint32_t len)
; returns 0, or -14 (EFAULT) when a fault hits the copy instruction.
global copy_user, copy_user_fault_start, copy_user_fault_end, copy_user_fixup
copy_user:
    push esi
    push edi
    mov edi, [esp + 12]
    mov esi, [esp + 16]
    mov ecx, [esp + 20]
copy_user_fault_start:
    rep movsb
copy_user_fault_end:
    xor eax, eax
    pop edi
    pop esi
    ret
copy_user_fixup:
    mov eax, -14
    pop edi
    pop esi
    ret

; Probe helpers with narrowly matched expected faults.
; void probe_write_byte(uint32_t va)   -- write 0x5A
global probe_write_byte, probe_write_insn, probe_write_insn_end, probe_write_resume
probe_write_byte:
    mov edx, [esp + 4]
probe_write_insn:
    mov byte [edx], 0x5A
probe_write_insn_end:
probe_write_resume:
    ret
; uint32_t probe_read_dword(uint32_t va)
global probe_read_dword, probe_read_insn, probe_read_insn_end, probe_read_resume
probe_read_dword:
    mov edx, [esp + 4]
    xor eax, eax
probe_read_insn:
    mov eax, [edx]
probe_read_insn_end:
probe_read_resume:
    ret
