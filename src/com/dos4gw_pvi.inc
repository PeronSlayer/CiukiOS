; VSBHDA documents SETPVI/RESPVI for DOS/4GW games that freeze under sound
; IRQ reflection. Keep PVI local to Doom, restoring the caller's bit after
; the transient host exits. Other CR4 bits still belong to the DPMI host.
doom_pvi_enter:
    pushfd
    pushad
    smsw ax
    test al, 1
    jnz .done                   ; CR4 is privileged in protected/VM86 mode.
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 0x200000           ; Check CPUID before querying VME/PVI.
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax, ecx
    test eax, 0x200000
    jz .done
    mov eax, 1
    cpuid
    test edx, 2
    jz .done
    mov eax, cr4
    mov [cs:doom_pvi_old], al
    or al, 2
    mov cr4, eax
    mov byte [cs:doom_pvi_armed], 1
.done:
    popad
    popfd
    ret

doom_pvi_leave:
    pushfd
    push eax
    cmp byte [cs:doom_pvi_armed], 1
    jne .done
    mov eax, cr4
    and al, 0xFD
    test byte [cs:doom_pvi_old], 2
    jz .restore
    or al, 2
.restore:
    mov cr4, eax
    mov byte [cs:doom_pvi_armed], 0
.done:
    pop eax
    popfd
    ret
doom_pvi_old db 0
doom_pvi_armed db 0

