; Execute production AUTO/profile selection; the host supplies firmware data.
bits 16
org 0x100
jmp stop
db 'VBM1'
%macro export 1
    dw %1
    db %str(%1),0
%endmacro
export stop
export vc_auto
export vc_resolve_mode
export vc_mode
export vc_info
export vc_controller
export vc_max_width
export vc_max_height
export vc_mode_count
export vc_lfb_open
export vc_lfb_close
dw 0
stop: hlt
%define VC_GRAPHICS_ONLY 1
%include "src/com/vbe_console.inc"
