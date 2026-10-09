; SAV3D.COM - native SuperSavage screen-space triangle diagnostic.
; Run in the system shell while its verified LFB mapping remains owned.
; No mode switch, framebuffer rebind, software renderer or OpenGL emulation.
; The monitor's completed GPU operation counter must advance before PASS.
bits 16
cpu 386
org 100h
%include "src/vm/session_abi.inc"

start:
    cld
    push cs
    pop ds
    push cs
    pop es
    mov sp,stack_top
    mov si,81h
.skip_space:
    lodsb
    cmp al,' '
    je .skip_space
    cmp al,13
    je .arguments_done
    dec si
    mov di,qualify_option
.option:
    lodsb
    cmp al,'a'
    jb .compare
    cmp al,'z'
    ja .compare
    sub al,32
.compare:
    scasb
    jne usage
    cmp byte [di],0
    jne .option
.option_end:
    lodsb
    cmp al,' '
    je .option_end
    cmp al,13
    jne usage
    mov byte [qualify_mode],1
.arguments_done:
    mov bx,(image_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    ; Discover the monitored host; the far entry is not an INT handler.
    xor di,di
    mov es,di
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    int 2Fh
    mov [entry],di
    mov [entry+2],es
    mov ax,es
    or ax,di
    push cs
    pop es
    jz unsupported
    mov ax,VM_OP_QUERY | VM_OP_NO_SWITCH
    mov cx,VM_INFO_SIZE
    mov di,query_packet
    call session_call
    jc session_failed
    cmp dword [query_packet],VM_INFO_MAGIC
    jne session_failed
    cmp word [query_packet+VM_INFO_VERSION],VM_ABI_VERSION
    jne session_failed
    cmp word [query_packet+VM_INFO_BYTES],VM_INFO_SIZE
    jb session_failed
    cmp dword [query_packet+VM_INFO_ACTIVE],0
    jne session_failed
    cmp byte [qualify_mode],0
    je .capture
    call qualify
    jc qualification_failed
    mov ax,VM_OP_QUERY | VM_OP_NO_SWITCH
    mov cx,VM_INFO_SIZE
    mov di,query_packet
    call session_call
    jc session_failed
.capture:
    ; Capture the actual driver state even when its hardware probe failed.
    mov di,log_before
    call display_info
    jc session_failed
    mov eax,[query_packet+VM_INFO_CAPABILITIES]
    test eax,VM_CAP_NATIVE_TRIANGLES
    jz unsupported
    cmp dword [query_packet+VM_INFO_FB_BOUND],1
    jne unsupported
    mov si,log_before
    call validate_native
    jc unsupported
    mov ax,VM_OP_FB_TRIANGLE | VM_OP_NO_SWITCH
    mov cx,VM_FB_TRIANGLE_PACKET_SIZE
    mov di,triangle_packet
    call session_call
    jc hardware_failed
    test ax,ax
    jnz hardware_failed
    mov di,log_after
    call display_info
    jc session_failed
    mov si,log_after
    call validate_native
    jc hardware_failed
    ; Native status triangle count at snapshot+56; user operations exclude
    ; the private probes. Exactly one submitted triangle must have completed.
    mov eax,[log_before+64+56]
    inc eax
    cmp [log_after+64+56],eax
    jne hardware_failed
    mov eax,[log_after+64+52]       ; completed hardware tiled/linear blits
    sub eax,[log_before+64+52]
    cmp eax,2
    jb hardware_failed
    mov eax,[log_after+64+88]       ; actual PIO words submitted
    sub eax,[log_before+64+88]
    cmp eax,13
    jb hardware_failed
    mov dword [log_result],0
    mov dx,completed_message
    call print
    call write_log
    jc log_failed
    mov dx,log_message
    call print
    mov dx,key_message
    call print
    xor ah,ah
    int 16h
    mov ax,4C00h
    int 21h

; Persist the phase BEFORE entering hardware. If a physical engine wedges,
; the next capture still identifies the last entered phase. The first three
; phases restore the initial state. The fourth retains only verified caps.
qualify:
    mov word [log_file],qualify_path
    mov word [qualify_stage],1
.next:
    mov di,log_before
    call display_info
    jc .failed
    movzx eax,word [qualify_stage]
    or eax,100h
    mov [log_result],eax
    call write_log
    jc .failed
    mov ax,VM_OP_GPU_QUALIFY | VM_OP_NO_SWITCH
    mov bx,[qualify_stage]
    call session_call
    jc .failed
    test ax,ax
    jnz .failed
    mov di,log_after
    call display_info
    jc .failed
    movzx eax,word [qualify_stage]
    or eax,200h
    mov [log_result],eax
    call write_log
    jc .failed
    inc word [qualify_stage]
    cmp word [qualify_stage],4
    jbe .next
    mov word [log_file],log_path
    clc
    ret
.failed:
    mov [log_error],ax
    movzx eax,word [qualify_stage]
    or eax,300h
    mov [log_result],eax
    mov di,log_after
    call display_info
    call write_log
    stc
    ret
qualification_failed:
    ; A failed stage can retain a busy engine; disk evidence remains safe.
    mov ax,4C04h
    int 21h
usage:
    mov dx,usage_message
    call print
    mov ax,4C01h
    int 21h

unsupported:
    mov dword [log_result],1
    mov dx,unsupported_message
    call print
    call write_log
    jc log_failed
    mov ax,4C01h
    int 21h
session_failed:
    mov [log_error],ax
    mov dword [log_result],2
    mov dx,session_message
    call print
    call write_log
    jc log_failed
    mov ax,4C02h
    int 21h
hardware_failed:
    mov [log_error],ax
    mov dword [log_result],3
    ; A timeout can retain a busy engine. Do not print into its framebuffer:
    ; capture the read-only status and disk log, then let the owning shell
    ; drain/release hardware before it restores or repaints the console.
    ; Read-only snapshot may still expose the retained hardware error.
    mov di,log_after
    call display_info
    call write_log
    jc log_failed
    mov ax,4C02h
    int 21h
log_failed:
    cmp dword [log_result],3
    je .return
    mov dx,log_failure_message
    call print
.return:
    mov ax,4C03h
    int 21h

session_call:
    push ds
    push es
    call far [cs:entry]
    pop es
    pop ds
    ret

display_info:
    mov ax,VM_OP_DISPLAY_INFO | VM_OP_NO_SWITCH
    mov cx,VM_DISPLAY_INFO_SIZE
    push di
    call session_call
    pop di
    jc .done
    cmp dword [di],VM_DISPLAY_INFO_MAGIC
    jne .bad
    cmp word [di+4],VM_ABI_VERSION
    jne .bad
    cmp word [di+6],VM_DISPLAY_INFO_SIZE
    jne .bad
    clc
.done:
    ret
.bad:
    stc
    ret

validate_native:
    cmp dword [si+8],3            ; owned SuperSavage, not compiled presence
    jne .bad
    cmp dword [si+12],1
    jne .bad
    cmp dword [si+16],193         ; x=192 and y=224 are strictly inside mode
    jb .bad
    cmp dword [si+20],225
    jb .bad
    mov eax,[si+28]
    cmp eax,16
    je .format_ok
    cmp eax,32
    jne .bad
.format_ok:
    cmp dword [si+32],0           ; no unresolved native driver error
    jne .bad
    cmp dword [si+56],0           ; no active DOS session owns display
    jne .bad
    test dword [si+60],80000000h  ; native status snapshot, not EDID
    jz .bad
    cmp dword [si+64],1           ; actual driver ready
    jne .bad
    test dword [si+64+4],2        ; native hardware triangle capability
    jz .bad
    cmp dword [si+64+12],08C2E5333h
    jne .bad
    cmp dword [si+64+76],0        ; private fill readback succeeded
    je .bad
    cmp dword [si+64+92],1        ; engine state remains owned
    jne .bad
    cmp dword [si+64+96],0        ; independent private triangle readback
    je .bad
    clc
    ret
.bad:
    stc
    ret

print:
    mov ah,9
    int 21h
    ret

write_log:
    mov dx,[log_file]
    xor cx,cx
    mov ah,3Ch
    int 21h
    jc .done
    mov bx,ax
    mov dx,log_header
    mov cx,log_end-log_header
    mov ah,40h
    int 21h
    jc .close_bad
    cmp ax,log_end-log_header
    jne .close_bad
    mov ah,3Eh
    int 21h
    ret
.close_bad:
    mov ah,3Eh
    int 21h
    stc
.done:
    ret

qualify_mode db 0
qualify_stage dw 0
qualify_option db '/QUALIFY',0
qualify_path db '\SYSTEM\VIDEO\S3QUAL.LOG',0
log_file dw log_path
usage_message db 'Usage: SAV3D [/QUALIFY]',13,10,'$'
entry dw 0,0
query_packet times VM_INFO_SIZE db 0
triangle_packet:
    dd VM_FB_TRIANGLE_MAGIC,VM_FB_TRIANGLE_PACKET_SIZE,0,0
    ; Three exact IEEE754 screen-space vertices, area bounded below 65,536.
    ; X/Y/Z and ARGB8888: red, green, blue. Z test is disabled in native state.
    dd 042000000h,042800000h,03F000000h,0FFFF0000h ; (32,64)
    dd 043400000h,042800000h,03F000000h,0FF00FF00h ; (192,64)
    dd 042000000h,043600000h,03F000000h,0FF0000FFh ; (32,224)
log_header:
    db 'CG3D'
    dw 0100h,16
log_result dd 0FFFFFFFFh
log_error dd 0
log_before times VM_DISPLAY_INFO_SIZE db 0
log_after times VM_DISPLAY_INFO_SIZE db 0
log_end:
log_path db '\SYSTEM\VIDEO\GPU3D.LOG',0
completed_message db '[SAV3D] Native S3 triangle completed by GPU.',13,10,'$'
unsupported_message db '[SAV3D] Native S3 triangle unsupported on this active display.',13,10,'$'
session_message db '[SAV3D] Requires the system shell with an inactive DOS session.',13,10,'$'
log_message db '[SAV3D] Verified counters saved to SYSTEM\VIDEO\GPU3D.LOG.',13,10,'$'
key_message db '[SAV3D] Press any key to return to the shell.',13,10,'$'
log_failure_message db '[SAV3D] Could not write SYSTEM\VIDEO\GPU3D.LOG.',13,10,'$'
align 2
times 512 db 0
stack_top:
image_end:
