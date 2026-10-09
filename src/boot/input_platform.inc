; Notebook input backend selection, before any native controller takeover.
; CF=1: retain OEM PS/2 firmware for the observed Armada E500 profile.
; CF=0: retain the normal native-first policy. No PCI/controller writes.
; Preserve all GPRs, segments and caller flags except the returned CF.
; Decision/source contract: docs/design/notebook-input-boot-policy.md.
input_platform_firmware_first:
    pushf
    pushad
    push ds
    push es
    push fs
    push gs

    mov ax, 0xB101
    xor edx, edx
    stc
    int 0x1A
    jc .native
    test ah, ah
    jnz .native
    cmp edx, 0x20494350           ; PCI BIOS signature "PCI "
    jne .native

    mov ax, 0xB102
    mov dx, 0x1002              ; ATI Mobility-P
    mov cx, 0x4C4D
    xor si, si
    stc
    int 0x1A
    jc .native
    test ah, ah
    jnz .native

    mov ax, 0xB102
    mov dx, 0x125D              ; ESS Maestro-2E
    mov cx, 0x1978
    xor si, si
    stc
    int 0x1A
    jc .native
    test ah, ah
    jnz .native

    mov ax, 0xB10A
    mov di, 0x002C              ; BX is the BIOS-returned ESS bus/devfn
    stc
    int 0x1A
    jc .native
    test ah, ah
    jnz .native
    cmp ecx, 0xB1120E11         ; subsystem device:vendor, not text order
    jne .native

    pop gs
    pop fs
    pop es
    pop ds
    popad
    popf
    stc
    ret
.native:
    pop gs
    pop fs
    pop es
    pop ds
    popad
    popf
    clc
    ret
