%ifndef CIUKIOS_UI_THEME_INCLUDED
%define CIUKIOS_UI_THEME_INCLUDED
%include "src/com/ciuki_logo_palette.inc"
%include "src/com/ui_icons_palette.inc"
%define VC_PALETTE_COLORS (16 + CIUKI_LOGO_COLOR_COUNT + UI_ICONS_COLOR_COUNT)

; Shared desktop/setup palette. Pixel-aligned platinum controls, indigo active
; titles and a muted teal desktop; 16 DAC RGB triples, channels 0..63.
; 0 ink, 1 active title, 3 desktop, 7 control face, 8 shadow, 15 paper.
%macro CIUKIOS_PALETTE 0
    db 9,10,12, 13,18,30, 18,32,23, 13,23,24
    db 37,15,16, 31,24,37, 37,29,15, 51,52,53
    db 28,30,34, 27,36,48, 24,41,38, 39,49,48
    db 49,25,23, 43,34,46, 57,47,27, 61,61,60
%endmacro
%endif
