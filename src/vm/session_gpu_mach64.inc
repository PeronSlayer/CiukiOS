; Independent Mach64 engine dispatch. Firmware scanout/panel stay untouched.
; Explicit qualification is the same four-stage native display ABI, without
; Savage-specific restored prefixes or triangle capability.
gpu_mach64_args proc
 mov eax,fb_physical
 mov dev_args,eax
 mov eax,fb_bytes
 mov [dev_args+4],eax
 mov eax,gpu_mode
 mov [dev_args+8],eax
 mov eax,[gpu_mode+4]
 mov [dev_args+12],eax
 mov eax,[gpu_mode+8]
 mov [dev_args+16],eax
 mov eax,[gpu_mode+12]
 mov [dev_args+20],eax
 mov eax,gpu_usable_bytes
 mov [dev_args+24],eax
 ret
gpu_mach64_args endp

gpu_mach64_bind proc
 mov fb_mach64,0
 mov mach64_qualification,0
 cmp vmm_current,0
 jne done
 cmp gpu_banked,0
 jne done
 cmp fb_savage,0
 jne done
 call gpu_mach64_args
 mov eax,offset cvmach64_bind
 mov ecx,7
 call dev_call
 cmp eax,2
 jne done
 mov fb_mach64,1
 mov eax,VM_ERROR_MAPPING
 ret
done:
 xor eax,eax
 ret
gpu_mach64_bind endp

gpu_mach64_qualify proc
 cmp vmm_current,0
 jne bad
 cmp vmm_count,1
 ja bad
 cmp active,0
 jne bad
 cmp fb_linear,0
 je bad
 cmp gpu_banked,0
 jne bad
 cmp fb_gpu,0
 jne bad
 cmp fb_legacy,0
 jne bad
 cmp fb_savage,0
 jne bad
 cmp fb_mach64,0
 jne bad
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_EBX
 cmp eax,1
 je restart
 cmp eax,4
 ja bad
 mov edx,mach64_qualification
 inc edx
 cmp eax,edx
 jne bad
 jmp run
restart:
 mov mach64_qualification,0
run:
 mov [dev_args+28],eax
 call gpu_mach64_args
 mov eax,offset cvmach64_bind_step
 mov ecx,8
 call dev_call
 cmp eax,2
 je retained
 cmp eax,1
 jne failed
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_EBX
 mov mach64_qualification,eax
 cmp eax,4
 jne success
 mov fb_mach64,1
success:
 xor eax,eax
 ret
retained:
 mov fb_mach64,1
failed:
 mov mach64_qualification,0
 mov eax,VM_ERROR_MAPPING
 ret
bad:
 mov eax,VM_ERROR_OPERATION
 ret
gpu_mach64_qualify endp

gpu_mach64_fill proc
 mov eax,[esi+VM_FB_FILL_OFFSET]
 mov dev_args,eax
 mov eax,[esi+VM_FB_FILL_PIXELS]
 mov [dev_args+4],eax
 mov eax,[esi+VM_FB_FILL_COLOUR]
 mov [dev_args+8],eax
 mov eax,[esi+VM_FB_FILL_PIXEL_BYTES]
 mov [dev_args+12],eax
 mov eax,offset cvmach64_fill
 mov ecx,4
 call dev_call
 ret
gpu_mach64_fill endp

gpu_mach64_page_copy proc
 mov eax,[esi+VM_FB_PAGE_SRC_OFFSET]
 mov dev_args,eax
 mov eax,[esi+VM_FB_PAGE_DST_OFFSET]
 mov [dev_args+4],eax
 movzx eax,word ptr [esi+VM_FB_PAGE_ROW_BYTES]
 mov [dev_args+8],eax
 movzx eax,word ptr [esi+VM_FB_PAGE_COUNT]
 mov [dev_args+12],eax
 mov eax,[esi+VM_FB_PAGE_PITCH]
 mov [dev_args+16],eax
 mov eax,offset cvmach64_page_copy
 mov ecx,5
 call dev_call
 ret
gpu_mach64_page_copy endp
