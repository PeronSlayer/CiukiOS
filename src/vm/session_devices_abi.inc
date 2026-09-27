; CiukiOS guest-device operations of the CVSESSION ABI (MASM/NASM syntax).
; Available only after BEGIN and only while Jemm's negotiated virtual-IF
; profile is active: device IRQs are raised into that single virtual PIC.
;
; DEV_BEGIN  EBX = cvgp capabilities (1 keyboard, 2 mouse, 8 DMA/SB16,
;            10h OPL), ECX flags (bit0: stream PCM to the ICH AC'97),
;            EDX = TSC kHz. Returns EBX = granted, ECX = audio status
;            (0 none, 1 running, 2 no AC'97, 3 no usable rate).
; DEV_END    Stops audio, removes traps and hooks, ends the model.
; DEV_FOCUS  BX = 1 focused / 0 unfocused (held keys are released).
; DEV_STATE  ES:DI = 128-byte cvdev_report (magic 'CVDV').
; DEV_IO     Bridge for the HDPMI adapter: BX = port, CL = width (1/2/4),
;            CH bit0 = write, EDX = value; returns EDX = read value.
;            Unclaimed ports are performed physically.
; DEV_KEY    Host-originated key for the focused guest: BL = Set-1 scan
;            code, CL bit0 = pressed, bit1 = E0 extended (e.g. window close).
VM_OP_DEV_BEGIN        equ 30h
VM_OP_DEV_END          equ 31h
VM_OP_DEV_FOCUS        equ 32h
VM_OP_DEV_STATE        equ 33h
VM_OP_DEV_IO           equ 34h
VM_OP_DEV_KEY          equ 35h

VM_CAP_GUEST_INPUT     equ 1000h
VM_CAP_GUEST_AUDIO     equ 2000h

VM_ERROR_DEV_PROFILE   equ 30h
VM_ERROR_DEV_MODEL     equ 31h
VM_ERROR_DEV_ACTIVE    equ 32h
VM_ERROR_DEV_INACTIVE  equ 33h

CVDEV_CAP_KEYBOARD     equ 01h
CVDEV_CAP_MOUSE        equ 02h
CVDEV_CAP_DMA_SB       equ 08h
CVDEV_CAP_OPL3         equ 10h
CVDEV_FLAG_AUDIO_OUT   equ 01h
CVDEV_REPORT_BYTES     equ 128
CVDEV_AUDIO_PAGES      equ 33
