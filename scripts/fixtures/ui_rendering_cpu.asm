; CPU-only regression fixture. Includes the production routines unchanged;
; Python supplies a banked/linear VBE BIOS and checks every framebuffer byte.
bits 16
org 0x100
jmp stop
db 'UIR1'
%macro export 1
    dw %1
    db %str(%1),0
%endmacro
export stop
export vc_validate
export vc_resolve_mode
export vc_mode
export vc_map
export vc_span
export vc_put_pixel
export vc_clear_video
export vc_queue_cell
export vc_flush
export vc_draw_cell
export vc_info
export vc_controller
export vc_active
export vc_bank
export vc_bytes
export vc_frame_bytes
export vc_access_bytes
export vc_scan_lines
export vc_pitch
export vc_bank_step
export vc_colors
export vc_cols
export vc_rows
export vc_cells_seg
export vc_font_seg
export vc_font_off
export vc_batch
export vc_dirty_first
export vc_dirty_last
export ui_comp_present
export ui_comp_draw
export ui_comp_band_setup
export ui_comp_damage
export test_comp_rect
export ui_comp_glyph
export ug_text_color
export ui_width
export ui_vbe
export ui_height
export ui_comp_seg
export ui_comp_capacity
export ui_comp_stride
export ui_comp_rows
export ui_comp_top
export ui_comp_bottom
export ui_comp_left
export ui_comp_right
export ui_comp_limit_top
export ui_comp_limit_bottom
export ui_comp_dirty
export ui_pages_begin
export ui_pages_prepare
export ui_pages_present
export ui_page_enabled
export ui_page_failed
export ui_comp_recover
export ui_front_base
export ui_present_base
export vc_lfb
export vc_lfb_ok
export vc_bank_ok
export vc_lfb_base
export vc_lfb_open
export vc_fb_depth
export vc_fb_put_rows
export vc_pointer_save
export vc_pointer_restore
export vc_pointer_draw
export test_pointer_masks
export test_scene_count
export test_scene_rects
dw 0
stop: hlt
test_comp_rect:
    pushad
    push es
    jmp ui_comp_rect
ui_sound_preference:
ui_pointer_hide:
ui_pointer_show:
ui_draw_topbar_visuals:
app_desktop_topbar_paint:
ui_window_bounds:
    ret
ui_draw_scene:
    pushad
    mov di,test_scene_rects
    mov bp,[test_scene_count]
    test bp,bp
    jz .done
.rect:
    mov bx,[di]
    mov dx,[di+2]
    mov cx,[di+4]
    mov si,[di+6]
    mov al,[di+8]
    call test_comp_rect
    add di,10
    dec bp
    jnz .rect
.done:
    popad
    ret
test_pointer_masks dw 0x8000,0xC000,0xE000,0xF000,0xF800,0xFC00,0xFE00,0xFF00,0xFF80,0xFFC0,0xFE00,0xEF00,0xCF00,0x8780,0x0780,0x0300
    dw 0,0,0x4000,0x6000,0x7000,0x7800,0x7C00,0x7E00,0x7F00,0x7C00,0x6C00,0x4600,0x0600,0x0300,0x0300,0
test_scene_count dw 0
test_scene_rects times 80 db 0
ui_vbe db 1
ui_width dw 2560
ui_height dw 1440
ui_mouse_x dw 0
ui_mouse_y dw 0
ui_fill_color db 0
ui_fill_width dw 0
ug_text_color db 0
ui_dragging db 0
app_overlay db 0
ui_hit_count dw 0
ui_wx dw 0
ui_wy dw 0
ui_ww dw 0
ui_wh dw 0
ui_window_drag db 0
ui_window_x times 4 dw 0
ui_window_y times 4 dw 0
%include "src/com/vbe_console.inc"
%include "src/com/shell_gui_compositor.inc"
