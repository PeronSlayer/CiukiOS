bits 16
org 100h
jmp stop
db 'CTR1'
%macro export 1
 dw %1
 db %str(%1),0
%endmacro
export stop
export ui_action
export ui_focus_next
export ui_focus_previous
export ui_hit_target
export ui_hit_owner
export ui_active_window
export ui_dialog
export ui_window_flags
export ui_focus
export ui_hit_count
export ui_hits
export module_present
export module_result
export route
export damage_calls
export slot_calls
export action_calls
export action_input
export opened_window
dw 0
stop: hlt
%define UI_WINDOW_COUNT 32
ui_hit_target db 0
ui_hit_owner db 0
ui_active_window db 8
ui_dialog db 0
ui_window_flags times 32 db 1
ui_focus dw 110
ui_focus_step dw 0
ui_focus_index dw 0
ui_assets_seg dw 1000h
ui_hit_count dw 0
ui_hits times 100 db 0
module_present db 1
module_result dw 250
route db 0
damage_calls dw 0
slot_calls dw 0
action_calls dw 0
action_input dw 0
opened_window db 0ffh
app_command db 'TEST',0

ui_damage_active:
 inc word [damage_calls]
 ret
app_slot:
 inc word [slot_calls]
 cmp byte [module_present],0
 je .absent
 clc
 ret
.absent:
 stc
 ret
app_action:
 inc word [action_calls]
 mov [action_input],ax
 mov ax,[module_result]
 ret
ui_windows_open:
 push ax
 mov al,[ui_dialog]
 mov [opened_window],al
 pop ax
 mov byte [route],1
 ret
ui_redraw:
 cmp byte [route],1
 je .done
 mov byte [route],2
.done:
 ret
ui_loop:
 mov byte [route],3
 ret
ui_launch_command:
 mov byte [route],4
 ret
%include "taskbar_routing_action.inc"
 ; The fixture deliberately stops at the native/non-module boundary.
 mov byte [route],5
 ret
%include "taskbar_routing_focus.inc"
