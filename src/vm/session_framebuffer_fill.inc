; CVFF native-pixel fills into the owned protected framebuffer. The packet
; is snapshotted before output; all validation precedes the first VRAM store.
; No firmware, mode switch or MMIO engine command is issued by this fallback.

framebuffer_fill proc uses esi edi ebx edx
 cmp vmm_current,0
 jne fill_not_vmm
 cmp fb_linear,0
 je fill_not_bound
 cmp word ptr [ebp].Client_Reg_Struc.Client_ECX,VM_FB_FILL_PACKET_SIZE
 jb fill_bad_address
 movzx edi,word ptr [ebp].Client_Reg_Struc.Client_EDI
 mov eax,edi
 add eax,VM_FB_FILL_PACKET_SIZE
 cmp eax,10000h
 ja fill_bad_address
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_ES
 shl eax,4
 add edi,eax
 mov ecx,VM_FB_FILL_PACKET_SIZE
 mov ebx,5
 call guest_span
 test eax,eax
 jnz fill_done
 sub esp,VM_FB_FILL_PACKET_SIZE
 mov esi,edi
 mov edi,esp
 mov ecx,VM_FB_FILL_PACKET_SIZE/4
 cld
 rep movsd
 cmp dword ptr [esp],VM_FB_FILL_MAGIC
 jne fill_abi
 cmp word ptr [esp+4],VM_ABI_VERSION
 jne fill_abi
 cmp word ptr [esp+6],VM_FB_FILL_PACKET_SIZE
 jne fill_abi
 mov eax,[esp+VM_FB_FILL_RESERVED0]
 or eax,[esp+VM_FB_FILL_RESERVED1]
 jnz fill_abi
 mov ebx,[esp+VM_FB_FILL_PIXEL_BYTES]
 test ebx,ebx
 jz fill_address
 cmp ebx,4
 ja fill_address
 mov ecx,[esp+VM_FB_FILL_PIXELS]
 test ecx,ecx
 jz fill_address
 mov eax,ecx
 mul ebx
 test edx,edx
 jnz fill_address
 cmp eax,VM_FB_ROWS_MAX_BYTES
 ja fill_address
 mov edi,[esp+VM_FB_FILL_OFFSET]
 add eax,edi
 jc fill_address
 cmp eax,fb_bytes
 ja fill_address
 add eax,fb_linear                 ; also prove the end address cannot wrap
 jc fill_address
 add edi,fb_linear
 jc fill_address
 mov esi,esp
 call gpu_savage_fill
 test eax,eax
 jz fill_hardware_written
 cmp eax,1
 jne fill_mapping
 call gpu_savage_cpu_begin
 jc fill_mapping
 call fb_copy_begin
 mov eax,[esp+VM_FB_FILL_COLOUR]
 cmp ebx,1
 je fill_byte
 cmp ebx,2
 je fill_word
 cmp ebx,4
 je fill_dword
 mov edx,eax
fill_triple:
 stosw
 shr eax,16
 stosb
 mov eax,edx
 dec ecx
 jnz fill_triple
 jmp fill_written
fill_byte:
 rep stosb
 jmp fill_written
fill_word:
 rep stosw
 jmp fill_written
fill_dword:
 rep stosd
fill_written:
 call fb_copy_account
fill_hardware_written:
 call gpu_damage_all
 add esp,VM_FB_FILL_PACKET_SIZE
 xor eax,eax
 ret
fill_abi:
 mov eax,VM_ERROR_ABI
 jmp fill_fail
fill_address:
 mov eax,VM_ERROR_ADDRESS
 jmp fill_fail
fill_mapping:
 mov eax,VM_ERROR_MAPPING
fill_fail:
 add esp,VM_FB_FILL_PACKET_SIZE
fill_done:
 ret
fill_not_vmm:
 mov eax,VM_ERROR_OPERATION
 ret
fill_not_bound:
 mov eax,VM_ERROR_FB_UNBOUND
 ret
fill_bad_address:
 mov eax,VM_ERROR_ADDRESS
 ret
framebuffer_fill endp
