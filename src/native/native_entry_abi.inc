; Trusted ring-0 ABI for the bounded CN32 CPL3 execution boundary.
; This is not an asynchronous process scheduler.
NE_OK equ 0
NE_FAULT equ 1
NE_BUDGET equ 2
NE_CONFIG equ 3
NE_BUSY equ 4
NE_MAX_STEPS equ 256
NATIVE_ENTRY_CONTEXT struct
 cr3_physical dd ?
 entry_linear dd ?
 stack_linear dd ?
 data_linear dd ?
 data_bytes dd ?
 budget dd ?
 result dd ?
 fault_vector dd ?
 fault_error dd ?
 fault_address dd ?
 reports dd ?
 report_tag dd ?
 report_value dd ?
 steps dd ?
 exit_status dd ?
NATIVE_ENTRY_CONTEXT ends

; Separate persistent trusted ABI. The original 60-byte entry ABI is intact.
; A manager owns this memory and its private CR3/pages across NE_BUDGET.
NATIVE_CONT_BYTES equ 136
NATIVE_CONT_MAGIC equ 54434E43h        ; "CNCT"
NC_MAGIC equ 60
NC_STATE equ 64
NC_EDI equ 68
NC_ESI equ 72
NC_EBP equ 76
NC_EBX equ 80
NC_EDX equ 84
NC_ECX equ 88
NC_EAX equ 92
NC_EIP equ 96
NC_ESP equ 100
NC_FLAGS equ 104
NC_DS equ 108
NC_ES equ 112
NC_FS equ 116
NC_GS equ 120
NC_QUANTA equ 124
NC_STEPS equ 128
NC_CR3 equ 132
NC_FRESH equ 0
NC_SUSPENDED equ 1
NC_TERMINAL equ 2
NC_SEG_NULL equ 0
NC_SEG_DATA equ 1
NC_SEG_CODE equ 2
