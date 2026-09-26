; Cooperative source-port graphics bridge, ABI1. All offsets module-relative
; to ES:DI returned by the exact INT2F discovery AX/BX pair.
%ifndef CIUKIOS_GRAPHICS_BRIDGE_ABI
%define CIUKIOS_GRAPHICS_BRIDGE_ABI 1
CG_DISCOVER_AX equ 0xD74A
CG_DISCOVER_BX equ 0x4347
CG_DISCOVER_REPLY equ 0x4743
CG_PRESENT_AX equ 0xD74B             ; INT2F alternative, CX=present flags
CG_INPUT_AX equ 0xD74C               ; INT2F alternative, AX/BX/CF=input result
CG_ABI_VERSION equ 1
CG_DESCRIPTOR_BYTES equ 48
CG_VERSION equ 0                    ; word
CG_SIZE equ 2                       ; word
CG_WIDTH equ 4                      ; word,320
CG_HEIGHT equ 6                     ; word,200
CG_PITCH equ 8                      ; word,320 indexed pixels
CG_FLAGS equ 10                     ; word,focused/close bits
CG_PIXELS equ 12                    ; FAR pointer,64000bytes
CG_PALETTE equ 16                   ; FAR pointer,256 RGB888 triples
CG_PRESENT equ 20                   ; FAR procedure; AXbit0palette changed
CG_INPUT equ 24                     ; FAR procedure; CF1empty else AXscan,BXdown
CG_MILLISECONDS equ 28              ; dword,host monotonic clock
CG_PRESENTED_FRAMES equ 32          ; dword,completed visible presentations
CG_STATUS equ 36                    ; word,0success; nonzero bridge failure
CG_HOST_BPP equ 38                  ; word,current physical pixel depth
CG_FLAG_FOCUSED equ 1
CG_FLAG_CLOSE equ 2
CG_FLAG_MULTIPLEX equ 4              ; INT2F supported through DPMI0300
CG_PRESENT_PALETTE equ 1
CG_PRESENT_POLL equ 2               ; service clock/input only,no frame/counter
CG_SCAN_EXTENDED equ 0x0100          ; normalized Set1 scan,ALlow7bits
; Client uses DPMI INT31/0301 RETF service, zero SS:SP for DPMI initial stack.
; Host entry switches to its own8KiB stack before retained presentation.
; INPUT returns CF0 AXscan(bit8E0),BX1make/0break; CF1when queue empty.
; No video ports, IRQ/PIT ownership, DOS calls or allocations in host callback.
%endif
