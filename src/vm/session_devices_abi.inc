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
;            CH bit0 = write, EDX = value; returns EDX = read value and
;            ESI = device IRQ lines held for the protected-mode client.
;            Unclaimed ports are performed physically. The first DEV_IO
;            hands device IRQs to that client: from then on they are held
;            here instead of being injected into V86 (see DEV_PM_IRQ).
; DEV_KEY    Host-originated key for the focused guest: BL = Set-1 scan
;            code, CL bit0 = pressed, bit1 = E0 extended (e.g. window close).
; DEV_MOUSE  Host-originated mouse movement for the session: BX = X motion,
;            CX = Y motion (signed, PS/2 sense: Y up), DL = buttons (bit0
;            left, bit1 right, bit2 middle). Needs the mouse capability.
; DEV_PM_IRQ HDPMI adapter, protected-mode delivery of held device IRQs.
;            CL = 2: accept the highest-priority held line the virtual PIC
;            allows now (it becomes in service, as on V86 injection);
;            returns EBX = line (FFFFFFFFh if none), EDX = its PIC vector,
;            ESI = lines still held. CL = 0: release after the client
;            exited (DPMIRUN): device IRQs go back to V86 injection and the
;            lines still held for that client are dropped.
VM_OP_DEV_BEGIN        equ 30h
VM_OP_DEV_END          equ 31h
VM_OP_DEV_FOCUS        equ 32h
VM_OP_DEV_STATE        equ 33h
VM_OP_DEV_IO           equ 34h
VM_OP_DEV_KEY          equ 35h
VM_OP_DEV_PM_IRQ       equ 36h
VM_OP_DEV_MOUSE        equ 37h

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
