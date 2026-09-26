; Execute the production sprite reader unchanged, intercepting only its
; existing rectangle callback. Inputs model desktop and Setup compositor bands.
bits 16
org 0
dw entry16, entry24, entry64, rect_callback
dw test_vbe, test_composing, test_top, test_bottom, test_origin_y
dw ciuki_logo16_pixels, ciuki_logo24_pixels, ciuki_logo64_pixels, ciuki_logo_fallback
entry16:
    call ciuki_logo16
    hlt
entry24:
    call ciuki_logo24
    hlt
entry64:
    call ciuki_logo64
    hlt
rect_callback:
    ret
test_vbe db 1
test_composing db 0
test_top dw 0
test_bottom dw 600
test_origin_y dw 0
%define CIUKI_LOGO_RECT rect_callback
%define CIUKI_LOGO_VBE test_vbe
%define CIUKI_LOGO_COMPOSING test_composing
%define CIUKI_LOGO_BAND_TOP test_top
%define CIUKI_LOGO_BAND_BOTTOM test_bottom
%define CIUKI_LOGO_ORIGIN_Y [test_origin_y]
%include "src/com/ciuki_logo.inc"
