bits 16
org 100h
jmp stop
db 'CVQ1'
%macro export 1
 dw %1
 db %str(%1),0
%endmacro
export stop
export ui_video_wait_release
export ui_comp_begin
export ui_comp_end
export ui_pages_failure
export ui_mode_switch
export ui_active
export vc_session_owned
export release_remaining
export release_calls
export bind_fail
export bind_calls
export bios_calls
export init_calls
export wall_end_calls
export ui_comp_seg
export ui_comp_capacity
export ui_page_enabled
export ui_page_pending
export ui_front_base
export vc_access_bytes
export vc_frame_bytes
export ui_composing
export ui_comp_dirty
export ui_comp_recover
export ui_full_redraw
export ui_present_base
export vc_lfb
export ui_pointer_visible
export ui_mode_request
export vc_graphics_transition
dw 0
stop: hlt
ui_active db 1
vc_session_owned db 1
release_remaining dw 0
release_calls dw 0
bind_fail db 0
bind_calls dw 0
bios_calls dw 0
init_calls dw 0
wall_end_calls dw 0
ui_comp_seg dw 0
ui_comp_capacity dw 0
ui_page_enabled db 1
ui_page_pending db 1
ui_page_failed db 0
ui_front_base dd 11223344h
vc_frame_bytes dd 40000h
vc_access_bytes dd 80000h
ui_composing db 1
ui_comp_dirty db 1
ui_comp_recover db 0
ui_full_redraw db 0
ui_present_base dd 55667788h
vc_lfb db 2
ui_pointer_visible db 1
ui_mode_request dw 140h
vc_graphics_transition db 0
ui_vbe db 1
vc_bytes db 4
ui_width dw 800
ui_comp_stride dw 0
ui_comp_try_paras dw 0
ui_comp_rows dw 0
ui_xms dw xms_stub,0

vc_session_fb_release:
 inc word [release_calls]
 cmp word [release_remaining],0
 je .released
 cmp word [release_remaining],0ffffh
 je .busy
 dec word [release_remaining]
.busy:
 stc
 ret
.released:
 mov byte [vc_session_owned],0
 clc
 ret
vc_session_fb_bind:
 inc word [bind_calls]
 mov byte [vc_session_owned],1
 cmp byte [bind_fail],0
 jne .bad
 clc
 ret
.bad:
 stc
 ret
ui_umb_alloc:
 stc
 ret
ui_pages_begin:
 ret
vc_bios:
 inc word [bios_calls]
 ret
wp_end:
 inc word [wall_end_calls]
 ret
ui_video_begin:
 inc word [init_calls]
 mov byte [ui_active],1
 ret
xms_stub:
 retf
%include "gui_video_quarantine.inc"
