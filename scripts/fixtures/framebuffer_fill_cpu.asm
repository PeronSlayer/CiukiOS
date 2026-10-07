; Assemble the production CVFF and guest-span routines into a flat CPU fixture.
.386p
.model flat
option casemap:none
include session_abi.inc

Client_Reg_Struc struct
 Client_EDI dd ?
 Client_ESI dd ?
 Client_EBP dd ?
 Client_Reserved dd ?
 Client_EBX dd ?
 Client_EDX dd ?
 Client_ECX dd ?
 Client_EAX dd ?
 Client_Int dd ?
 Client_Error dd ?
 Client_EIP dd ?
 Client_CS dd ?
 Client_EFlags dd ?
 Client_ESP dd ?
 Client_SS dd ?
 Client_ES dd ?
Client_Reg_Struc ends
PAGE_MAP equ 100000h
@VMMCall macro service
 call fixture_vmm_call
endm

.code
fixture_header dd 046464643h, offset framebuffer_fill
 dd offset vmm_current, offset fb_linear, offset fb_bytes
 dd offset begin_calls, offset account_calls, offset damage_calls
 dd offset fixture_stop
 dd offset framebuffer_triangle, offset fb_savage
 dd offset triangle_result, offset triangle_calls, offset triangle_snapshot
 dd offset fill_result, offset sync_carry, offset native_fill_calls
 dd offset cvsavage_triangle
 dd offset unbind_framebuffer, offset active, offset release_result
 dd offset vmm_result, offset fb_pages
 dd offset framebuffer_page_copy, offset page_copy_result
 dd offset native_copy_calls, offset native_copy_snapshot
 dd offset cvsavage_page_copy
vmm_current dd 0
fb_linear dd 0
fb_bytes dd 0
begin_calls dd 0
account_calls dd 0
damage_calls dd 0
fb_savage dd 0
triangle_result dd 0
triangle_calls dd 0
triangle_snapshot db 64 dup (0)
fill_result dd 1
sync_carry dd 0
native_fill_calls dd 0
page_copy_result dd 1
native_copy_calls dd 0
native_copy_snapshot db 20 dup (0)
active dd 0
fb_gpu dd 0
fb_legacy dd 0
fb_physical dd 0
fb_pages dd 0
gpu_dirty dd 0
fb_cache_record db 16 dup (0)
release_result dd 0
vmm_result dd 1
dev_args dd 8 dup (0)

fb_copy_begin proc
 inc begin_calls
 ret
fb_copy_begin endp
fb_copy_account proc
 inc account_calls
 ret
fb_copy_account endp
gpu_damage_all proc
 inc damage_calls
 ret
gpu_damage_all endp
gpu_savage_fill proc
 inc native_fill_calls
 mov eax,fill_result
 ret
gpu_savage_fill endp
gpu_savage_cpu_begin proc
 cmp sync_carry,0
 jne sync_failed
 clc
 ret
sync_failed:
 stc
 ret
gpu_savage_cpu_begin endp
dev_call proc uses ebx esi edi
 call eax
 ret
dev_call endp
cvsavage_triangle proc uses esi edi
 inc triangle_calls
 mov esi,dev_args
 mov edi,offset triangle_snapshot
 mov ecx,16
 rep movsd
 mov eax,triangle_result
 ret
cvsavage_triangle endp
cvsavage_page_copy proc uses esi edi ecx
 inc native_copy_calls
 mov esi,offset dev_args
 mov edi,offset native_copy_snapshot
 mov ecx,5
 rep movsd
 mov eax,page_copy_result
 ret
cvsavage_page_copy endp
cvsavage_panel_probe proc
 ret
cvsavage_panel_probe endp
gpu_savage_release proc
 mov eax,release_result
 ret
gpu_savage_release endp
cvgpu_release proc
 xor eax,eax
 ret
cvgpu_release endp
cvlegacy_release proc
 xor eax,eax
 ret
cvlegacy_release endp
fb_cache_disable proc
 ret
fb_cache_disable endp
fixture_vmm_call proc
 mov eax,vmm_result
 ret
fixture_vmm_call endp
include guest_span_cpu.inc
include session_framebuffer_fill.inc
include session_framebuffer_triangle.inc
fixture_stop:
 hlt
end
