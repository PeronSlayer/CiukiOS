; Execute the real file loader, lifetime handling and pixel reader. Only
; DOS interrupts and the existing rectangle callback are supplied by the test.
bits 16
org 0
dw enter_begin, enter_end, enter_draw, ui_rect
dw ui_icons_seg, ui_icons_handle, ui_vbe, ui_composing
dw ui_comp_top, ui_comp_bottom, ui_icons_fallback, ui_icons_extra
dw ui_icons_path, UI_ICONS_BYTES
enter_begin:
    call ui_icons_begin
    hlt
enter_end:
    call ui_icons_end
    hlt
enter_draw:
    call ui_icons_draw
    hlt
ui_rect:
    ret
ui_vbe db 1
ui_composing db 0
ui_comp_top dw 0
ui_comp_bottom dw 600
%define VC_PALETTE_COLORS 144
%define CIUKI_LOGO_COLOR_COUNT 64
%define UI_ICONS_COLOR_COUNT 64
%include "src/com/ui_icons.inc"
