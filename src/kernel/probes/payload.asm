; Ring-3 probe payloads (docs/design/f0-acceptance.md). One flat image
; mapped read-only at 0x00401000 (one page); data page at 0x00400000;
; stack page at 0xBFFFF000. EAX selects the payload, EBX/ECX are arguments.
; Data page layout: +0 counter, +4 error flag, +8 sentinel copy,
; +0x10.. results, +0x40 16-byte scratch, +0x100 untouched pattern area.
; SPDX-License-Identifier: GPL-2.0-only
bits 32
org 0x00401000

%define DATA        0x00400000
%define SYS_EXIT    0
%define SYS_YIELD   1
%define SYS_DEBUG   2
%define SYS_QUERY   5

%macro SYSCALL 3            ; number, ebx, ecx
    mov eax, %1
    mov ebx, %2
    mov ecx, %3
    int 0x80
%endmacro

entry:
    cmp eax, (table_end - table) / 4
    jae bad_id
    jmp [table + eax * 4]
bad_id:
    SYSCALL SYS_EXIT, 99, 0

table:
    dd p_isolation      ; 0
    dd p_spin           ; 1
    dd p_kread          ; 2
    dd p_kwrite         ; 3
    dd p_cli            ; 4
    dd p_in             ; 5
    dd p_out            ; 6
    dd p_survivor       ; 7
    dd p_syslife        ; 8
    dd p_exit0          ; 9
    dd p_nullwrite      ; 10
    dd p_fpu            ; 11
    dd p_fpufault       ; 12
table_end:

; 0: store EBX at DATA, yield 200 times, exit 0 if it is unchanged.
p_isolation:
    mov [DATA], ebx
    mov [DATA + 8], ebx
    mov esi, ebx
    mov edi, 200
.loop:
    mov eax, SYS_YIELD
    int 0x80
    dec edi
    jnz .loop
    cmp [DATA], esi
    jne .bad
    SYSCALL SYS_EXIT, 0, 0
.bad:
    SYSCALL SYS_EXIT, 1, 0

; 1: never yields; keeps known values in registers, on the stack, in
; EFLAGS and in the segment registers, and checks them continuously.
; Counter at DATA; error bits at DATA+4: 1 registers, 2 stack, 4 flags,
; 8 selectors.
p_spin:
    mov esi, ebx
    mov edi, ebx
    not edi
    mov ebp, ebx
    xor ebp, 0x5A5A5A5A
    push ebx
.loop:
    inc dword [DATA]
    cmp esi, ebx
    jne .reg
    mov eax, ebx
    not eax
    cmp edi, eax
    jne .reg
    mov eax, ebx
    xor eax, 0x5A5A5A5A
    cmp ebp, eax
    jne .reg
    cmp [esp], ebx
    jne .stack
    pushfd
    pop eax
    and eax, 0x3600                 ; IOPL, IF, DF
    cmp eax, 0x0200                 ; IOPL 0, IF 1, DF 0
    jne .flags
    mov ax, cs
    cmp ax, 0x1B
    jne .sel
    mov ax, ss
    cmp ax, 0x23
    jne .sel
    mov ax, ds
    cmp ax, 0x23
    jne .sel
    jmp .loop
.reg:
    or dword [DATA + 4], 1
    jmp .bad
.stack:
    or dword [DATA + 4], 2
    jmp .bad
.flags:
    or dword [DATA + 4], 4
    jmp .bad
.sel:
    or dword [DATA + 4], 8
.bad:
    SYSCALL SYS_EXIT, 2, 0

; 2-6: privileged or forbidden operations; each must fault.
p_kread:
    mov eax, [0xC0100000]
    SYSCALL SYS_EXIT, 3, 0
p_kwrite:
    mov dword [0xC0100000], 0
    SYSCALL SYS_EXIT, 3, 0
p_cli:
    cli
    SYSCALL SYS_EXIT, 3, 0
p_in:
    in al, 0x80             ; POST diagnostic port: harmless, must be denied
    SYSCALL SYS_EXIT, 3, 0
p_out:
    out 0x80, al
    SYSCALL SYS_EXIT, 3, 0

; 7: survivor; sentinel EBX at DATA+8, counter at DATA, never exits.
p_survivor:
    mov [DATA + 8], ebx
.loop:
    inc dword [DATA]
    cmp [DATA + 8], ebx
    je .loop
    mov dword [DATA + 4], 1
    jmp .loop

; 8: invalid buffers through the real syscall paths. Results at DATA+0x10.
p_syslife:
    mov edi, DATA + 0x100   ; pattern area that must stay untouched
    mov ecx, 64
    mov al, 0xCC
.fill:
    mov [edi], al
    inc edi
    loop .fill
    SYSCALL SYS_DEBUG, 0x00800000, 16       ; a: unmapped
    mov [DATA + 0x10], eax
    SYSCALL SYS_DEBUG, 0xC0100000, 16       ; b: kernel
    mov [DATA + 0x14], eax
    SYSCALL SYS_DEBUG, 0xFFFFFFF0, 32       ; c: wrapping past the end of the address space
    mov [DATA + 0x18], eax
    SYSCALL SYS_DEBUG, 0x00401FF0, 32       ; d: straddles into unmapped 0x402000
    mov [DATA + 0x1C], eax
    SYSCALL 3, 0x00800000, 16               ; d2: probe_report from an unreadable source
    mov [DATA + 0x3C], eax
    SYSCALL SYS_QUERY, 0x00401000, 64       ; e: read-only destination
    mov [DATA + 0x20], eax
    SYSCALL SYS_QUERY, 0x00800000, 64       ; f: unmapped destination
    mov [DATA + 0x24], eax
    SYSCALL SYS_QUERY, DATA + 0x100, 8      ; g: too small -> ENOSPC, no write
    mov [DATA + 0x28], eax
    SYSCALL SYS_QUERY, DATA + 0x200, 5000   ; h: capacity above limit -> EINVAL
    mov [DATA + 0x2C], eax
    SYSCALL SYS_QUERY, DATA + 0x200, 4096   ; i: valid (positive control)
    mov [DATA + 0x30], eax
    SYSCALL SYS_DEBUG, DATA + 0x300, 300    ; j: too long -> EINVAL
    mov [DATA + 0x34], eax
    SYSCALL 99, 0, 0                        ; k: unknown call -> ENOSYS
    mov [DATA + 0x38], eax
    SYSCALL SYS_EXIT, 0, 0

; 9: normal exit. 10: ring-3 #PF at page zero.
p_exit0:
    SYSCALL SYS_EXIT, 0, 0
p_nullwrite:
    mov dword [0], 1
    SYSCALL SYS_EXIT, 3, 0

; 11: x87 (and SSE when ECX=1) state that must survive preemption.
; EBX = variant 1 or 2. Counter at DATA, error flag at DATA+4.
p_fpu:
    mov esi, ebx
    dec esi                         ; 0 or 1
    mov [DATA + 0x0C], ecx          ; sse flag
    fldcw [cw_table + esi * 2]
    fld qword [val_b + esi * 8]
    fld qword [val_a + esi * 8]     ; st0 = a, st1 = b
    test ecx, ecx
    jz .loop
    ldmxcsr [mx_table + esi * 4]
    mov eax, esi
    shl eax, 4
    movups xmm0, [xmm_table + eax]
.loop:
    inc dword [DATA]
    fstcw [DATA + 0x40]
    mov ax, [cw_table + esi * 2]
    cmp [DATA + 0x40], ax
    jne .bad_cw
    fst qword [DATA + 0x48]         ; st0 without pop
    mov eax, [DATA + 0x48]
    cmp eax, [val_a + esi * 8]
    jne .bad_st0
    mov eax, [DATA + 0x4C]
    cmp eax, [val_a + esi * 8 + 4]
    jne .bad_st0
    fxch
    fst qword [DATA + 0x48]
    fxch
    mov eax, [DATA + 0x48]
    cmp eax, [val_b + esi * 8]
    jne .bad_st1
    mov eax, [DATA + 0x4C]
    cmp eax, [val_b + esi * 8 + 4]
    jne .bad_st1
    cmp dword [DATA + 0x0C], 0
    je .loop
    stmxcsr [DATA + 0x40]
    mov eax, [mx_table + esi * 4]
    cmp [DATA + 0x40], eax
    jne .bad_mxcsr
    movups [DATA + 0x50], xmm0
    mov eax, esi
    shl eax, 4
    mov edx, [xmm_table + eax]
    cmp [DATA + 0x50], edx
    jne .bad_xmm
    mov edx, [xmm_table + eax + 12]
    cmp [DATA + 0x5C], edx
    jne .bad_xmm
    jmp .loop
.bad_cw:
    mov dword [DATA + 4], 1         ; observed value stays at DATA+0x40
    jmp .bad
.bad_st0:
    mov dword [DATA + 4], 2
    jmp .bad
.bad_st1:
    mov dword [DATA + 4], 4
    jmp .bad
.bad_mxcsr:
    mov dword [DATA + 4], 8
    jmp .bad
.bad_xmm:
    mov dword [DATA + 4], 16
.bad:
    SYSCALL SYS_EXIT, 3, 0

; 12: unmasked x87 zero-divide must raise #MF in this task only.
p_fpufault:
    fninit
    fldcw [cw_unmask_zd]
    fldz
    fld1
    fdiv st0, st1                   ; 1 / 0 with ZE unmasked: pending exception
    fwait                           ; delivers #MF
    SYSCALL SYS_EXIT, 4, 0

align 8
; All four constants have nonzero low and high dwords, so each 32-bit
; compare is meaningful.
val_a:      dq 3.14159265358979, -2.71828182845905
val_b:      dq 1234.5678, -0.0012345678
; Both control words keep 64-bit precision (PC=11): QEMU TCG rounds FLD m64
; to the current PC precision, unlike real hardware, so a 24-bit PC would
; change loaded values and test the emulator instead of state preservation.
cw_table:   dw 0x037F, 0x0F7F       ; default; round-toward-zero
cw_unmask_zd: dw 0x037B             ; ZM cleared
align 4
mx_table:   dd 0x1F80, 0x7F80       ; default; flush-to-zero set
align 16
xmm_table:  dd 0x11111111, 0x22222222, 0x33333333, 0x44444444
            dd 0xA5A5A5A5, 0x5A5A5A5A, 0x0F0F0F0F, 0xF0F0F0F0

%if ($ - $$) > 4096
%error payload exceeds one page
%endif
