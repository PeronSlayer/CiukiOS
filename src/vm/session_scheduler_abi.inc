; CiukiOS monitored-session scheduler descriptor.
; This conventional-memory record is owned by the real-mode session parent.
; Jemm and the selected HDPMI 3.24 host may update it only while the matching
; CVSESSION generation is armed.  It is observability/control state for one
; foreground DOS execution context, not an independent VM or a security wall.

CVSCHED_MAGIC              equ 043535643h ; bytes CVSC
CVSCHED_VERSION            equ 0100h
CVSCHED_BYTES              equ 256

CVSCHED_MAGIC_OFS          equ 0
CVSCHED_VERSION_OFS        equ 4
CVSCHED_BYTES_OFS          equ 6
CVSCHED_GENERATION         equ 8
CVSCHED_STATE              equ 12
CVSCHED_FLAGS              equ 16
CVSCHED_LAST_ERROR         equ 20
CVSCHED_V86_TICKS          equ 24
CVSCHED_DPMI_TICKS         equ 28
CVSCHED_SERVICES           equ 32
CVSCHED_REENTRIES          equ 36
CVSCHED_DPMI_HANDLE        equ 40
CVSCHED_INSTALLS           equ 44
CVSCHED_REMOVES            equ 48
CVSCHED_CLIENT_ENTRIES     equ 52
CVSCHED_CLIENT_EXITS       equ 56
CVSCHED_FAULT_EXITS        equ 60
CVSCHED_PTE_CHECKS         equ 64
CVSCHED_PTE_REPAIRS        equ 68
CVSCHED_SHADOW_BYTES       equ 72
CVSCHED_SCHEDULER_BYTES    equ 76
CVSCHED_ADAPTER_BYTES      equ 80
CVSCHED_CALLBACK_BYTES     equ 84
CVSCHED_MODE_TRANSITIONS   equ 88
CVSCHED_FAILURES           equ 92
CVSCHED_PTES               equ 96
CVSCHED_PTE_COUNT          equ 32
CVSCHED_REQUEST_GENERATION equ 224
CVSCHED_SERVICE_GENERATION equ 228
CVSCHED_LAST_CR3           equ 232
CVSCHED_LAST_EIP           equ 236
CVSCHED_LAST_FLAGS         equ 240
CVSCHED_LAST_PORT          equ 244
CVSCHED_RESERVED0          equ 248
CVSCHED_RESERVED1          equ 252

CVSCHED_STATE_BOUND        equ 00000001h
CVSCHED_STATE_SESSION      equ 00000002h
CVSCHED_STATE_JEMM         equ 00000004h
CVSCHED_STATE_DPMI_HOST    equ 00000008h
CVSCHED_STATE_DPMI_CLIENT  equ 00000010h
CVSCHED_STATE_CALLBACK     equ 00000020h
CVSCHED_STATE_FATAL        equ 80000000h

CVSCHED_FLAG_V86_IRQ       equ 00000001h
CVSCHED_FLAG_DPMI_IRQ      equ 00000002h
CVSCHED_FLAG_HDP324_ABI    equ 00000004h
CVSCHED_FLAG_PTE_GUARD     equ 00000008h

CVSCHED_ERROR_NONE         equ 0
CVSCHED_ERROR_DESCRIPTOR   equ 1
CVSCHED_ERROR_INSTALL      equ 2
CVSCHED_ERROR_REMOVE       equ 3
CVSCHED_ERROR_PTES         equ 4
CVSCHED_ERROR_REENTRY      equ 5
CVSCHED_ERROR_STRING_IO    equ 6
CVSCHED_ERROR_CONTEXT      equ 7
CVSCHED_ERROR_HOST_OWNED   equ 8

