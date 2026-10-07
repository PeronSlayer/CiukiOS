; Full-HDD real-mode regression for production VBE linear copy/fill routines.
; A DOS-owned conventional block provides a safe 128 KiB flat-memory target.
bits 16
org 0x100
jmp start
db 'LFB1'

start:
    push cs
    pop ds
    push cs
    pop es
    push cs
    pop ss
    mov sp,program_end+2048
    cld

    ; Shrink this COM's block, then reserve an owned 128 KiB DOS block.
    ; Its physical address is segment<<4; offset 0xFFFC stays within it.
    mov ax,cs
    mov es,ax
    mov bx,(program_end-$$+0x100+2048+15)/16
    mov ah,0x4A
    int 0x21
    jc fail_resize
    mov bx,0x2000
    mov ah,0x48
    int 0x21
    jc fail_alloc
    mov [cs:buffer_segment],ax
    movzx eax,ax
    shl eax,4
    mov [cs:vc_lfb_base],eax
    mov dword [cs:vc_access_bytes],0x00020000

    call vc_lfb_open
    jc fail_lfb_open
    mov byte [cs:vc_lfb],1

    ; Non-default caller ES catches incorrect segment bases. All CPU
    ; operations share one burst, including ordinary real-mode ES reloads.
    mov ax,0x3456
    mov es,ax
    push cs
    pop fs
    call vc_fb_begin

    mov byte [cs:vc_bytes],1
    mov edi,0x18
    mov ecx,16
    mov eax,0xA7
    call vc_fb_fill
    mov ax,[cs:buffer_segment]
    mov es,ax                 ; the renderer also uses ES for scratch rows
    mov edi,0xFFF0
    mov ecx,32
    mov eax,0xA7
    call vc_fb_fill

    mov byte [cs:vc_bytes],2
    mov edi,0x20
    mov ecx,4
    mov eax,0x4433
    call vc_fb_fill

    mov edi,0xFFFC
    mov si,copy_pattern
    mov cx,16
    call vc_fb_put

    mov edi,0xFFF0
    mov si,read_window
    mov cx,32
    call vc_fb_get
    mov edi,0x18
    mov si,read_fill
    mov cx,16
    call vc_fb_get

    ; Direct physical reads verify that transfer and fill reached the exact
    ; intended address, even if a paired copy/readback bug could mask itself.
    cmp word [es:0x20],0x4433  ; independent conventional segment addressing
    jne fail_live_fill
    xor ax,ax
    mov es,ax
    mov edi,[cs:vc_lfb_base]
    cmp byte [es:edi+0xFFFB],0xA7
    jne fail_live_canary
    cmp dword [es:edi+0xFFFC],0x43424140
    jne fail_live_copy
    cmp dword [es:edi+0x10008],0x4F4E4D4C
    jne fail_live_copy
    cmp byte [es:edi+0x1000C],0xA7
    jne fail_live_canary
    cmp word [es:edi+0x20],0x4433
    jne fail_live_fill
    cmp word [es:edi+0x26],0x4433
    jne fail_live_fill
    call vc_fb_end

    mov ax,es
    cmp ax,0x3456
    jne fail_es_restore

    push es
    push cs
    pop es
    mov si,read_window
    mov di,expected_window
    mov cx,32
    repe cmpsb
    pop es
    jne fail_window

    push es
    push cs
    pop es
    mov si,read_fill
    mov di,expected_fill
    mov cx,16
    repe cmpsb
    pop es
    jne fail_fill

    call vc_lfb_close
    mov si,pass_text
    jmp report

fail_window:
    call vc_lfb_close
    mov si,window_text
    jmp report
fail_fill:
    call vc_lfb_close
    mov si,fill_text
    jmp report
fail_es_restore:
    call vc_lfb_close
    mov si,es_text
    jmp report
fail_lfb_open:
    mov si,lfb_text
    jmp report
fail_live_copy:
    call vc_fb_end
    call vc_lfb_close
    mov si,live_copy_text
    jmp report
fail_live_canary:
    call vc_fb_end
    call vc_lfb_close
    mov si,live_canary_text
    jmp report
fail_live_fill:
    call vc_fb_end
    call vc_lfb_close
    mov si,live_fill_text
    jmp report
fail_resize:
    mov si,resize_text
    jmp report
fail_alloc:
    mov si,alloc_text
report:
    cld
.next:
    lodsb
    test al,al
    jz .exit
    out 0xE9,al
    jmp .next
.exit:
    mov dx,0xF4
    mov al,0x10
    out dx,al
    mov ax,0x4C00
    int 0x21

copy_pattern db 0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47
              db 0x48,0x49,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F
expected_window times 12 db 0xA7
              db 0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47
              db 0x48,0x49,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F
              times 4 db 0xA7
expected_fill times 8 db 0xA7
              times 4 dw 0x4433
read_window times 32 db 0
read_fill times 16 db 0
buffer_segment dw 0
pass_text db '[LFBES] PASS copy/fill, ES base and 64KiB boundary',0
window_text db '[LFBES] FAIL copy/canary bytes',0
fill_text db '[LFBES] FAIL fill bytes',0
es_text db '[LFBES] FAIL caller ES restore',0
lfb_text db '[LFBES] FAIL LFB burst setup',0
live_copy_text db '[LFBES] FAIL direct copy check',0
live_canary_text db '[LFBES] FAIL direct canary check',0
live_fill_text db '[LFBES] FAIL direct fill check',0
resize_text db '[LFBES] FAIL shrinking DOS block',0
alloc_text db '[LFBES] FAIL DOS buffer allocation',0

%define VC_GRAPHICS_ONLY 1
%include "src/com/vbe_console.inc"
program_end:
