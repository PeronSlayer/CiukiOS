; Test the production VBE palette routine with the real shared UI/Ciuki palette.
bits 16
org 0x100
%include "src/com/ui_theme.inc"
%ifdef TEST_PALETTE_LIMIT
%undef VC_PALETTE_COLORS
%if TEST_PALETTE_LIMIT != 16
%define VC_PALETTE_COLORS TEST_PALETTE_LIMIT
%endif
%endif
jmp stop
db 'UIR1'
%macro export 1
    dw %1
    db %str(%1),0
%endmacro
export stop
export vc_set_palette
export vc_bytes
export vc_colors
export vc_controller
export vc_info
export test_palette
export test_palette_end
dw 0
stop: hlt
test_palette:
    CIUKIOS_PALETTE
    CIUKI_LOGO_PALETTE
    UI_ICONS_PALETTE
test_palette_end:
%define VC_GRAPHICS_ONLY
%include "src/com/vbe_console.inc"
