; Production mode validation/selection; the test supplies BIOS responses and
; the separately-tested LFB A20/CPU eligibility opener. No renderer is replaced.
bits 16
org 0x100
jmp stop
db 'VBM1'
%macro export 1
    dw %1
    db %str(%1),0
%endmacro
export stop
export vc_validate
export vc_validate_active
export vc_begin
export vc_end
export vc_set_palette
export vc_info
export vc_controller
export vc_palette
export vc_colors
export vc_pitch
export vc_lfb
export vc_lfb_ok
export vc_bank_ok
export vc_layout_linear
export vc_banked_valid
export vc_mode
export vc_active
export vc_lfb_open
export vc_lfb_close
dw 0
stop: hlt
%define VC_GRAPHICS_ONLY 1
%include "src/com/vbe_console.inc"
