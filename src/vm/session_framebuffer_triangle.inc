; Native CVT3 triangle submission. Copy the conventional packet into a
; private stack snapshot before entering chip-specific code. Unsupported
; adapters return an error; this operation never draws a software imitation.
framebuffer_triangle proc uses esi edi ebx edx
 cmp vmm_current,0
 jne triangle_not_vmm
 cmp fb_linear,0
 je triangle_not_bound
 cmp fb_savage,1
 jne triangle_not_vmm
 cmp word ptr [ebp].Client_Reg_Struc.Client_ECX,VM_FB_TRIANGLE_PACKET_SIZE
 jb triangle_bad_address
 movzx edi,word ptr [ebp].Client_Reg_Struc.Client_EDI
 mov eax,edi
 add eax,VM_FB_TRIANGLE_PACKET_SIZE
 cmp eax,10000h
 ja triangle_bad_address
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_ES
 shl eax,4
 add edi,eax
 mov ecx,VM_FB_TRIANGLE_PACKET_SIZE
 mov ebx,5
 call guest_span
 test eax,eax
 jnz triangle_done
 sub esp,VM_FB_TRIANGLE_PACKET_SIZE
 mov esi,edi
 mov edi,esp
 mov ecx,VM_FB_TRIANGLE_PACKET_SIZE/4
 cld
 rep movsd
 cmp dword ptr [esp],VM_FB_TRIANGLE_MAGIC
 jne triangle_abi
 cmp dword ptr [esp+VM_FB_TRIANGLE_SIZE],VM_FB_TRIANGLE_PACKET_SIZE
 jne triangle_abi
 mov eax,[esp+VM_FB_TRIANGLE_FLAGS]
 or eax,[esp+VM_FB_TRIANGLE_RESERVED]
 jnz triangle_abi
 mov eax,esp
 mov dev_args,eax
 mov eax,offset cvsavage_triangle
 mov ecx,1
 call dev_call
 test eax,eax
 jz triangle_written
 cmp eax,1
 je triangle_unsupported
 mov eax,VM_ERROR_MAPPING
 jmp triangle_fail
triangle_written:
 call gpu_damage_all
 add esp,VM_FB_TRIANGLE_PACKET_SIZE
 xor eax,eax
 ret
triangle_abi:
 mov eax,VM_ERROR_ABI
 jmp triangle_fail
triangle_unsupported:
 mov eax,VM_ERROR_OPERATION
triangle_fail:
 add esp,VM_FB_TRIANGLE_PACKET_SIZE
triangle_done:
 ret
triangle_not_vmm:
 mov eax,VM_ERROR_OPERATION
 ret
triangle_not_bound:
 mov eax,VM_ERROR_FB_UNBOUND
 ret
triangle_bad_address:
 mov eax,VM_ERROR_ADDRESS
 ret
framebuffer_triangle endp
