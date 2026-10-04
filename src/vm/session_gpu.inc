; Native VirtIO GPU ownership and Jemm memory services. C executes on the
; existing private device stack. No DOS/BIOS calls and no guest flat pointers.
; Research/contract: docs/validation/2026-10-03-native-gpu-presentation.md.
externdef c cvgpu_bind:near
externdef c cvgpu_present:near
externdef c cvgpu_damage:near
externdef c cvgpu_release:near
externdef c cvgpu_owned:near
externdef c cvgpu_status:byte
externdef c cvgpu_edid:byte
externdef c cvgpu_edid_bytes:dword
externdef c cvlegacy_bind:near
externdef c cvlegacy_present:near
externdef c cvlegacy_release:near
externdef c cvlegacy_vbe_alias:near

.data
fb_gpu dd 0
fb_legacy dd 0
gpu_dirty dd 0
gpu_mode dd 4 dup (0) ; optional validated VBE width, height, pitch, bpp
gpu_banked dd 0
display_packet db VM_DISPLAY_INFO_SIZE dup (0)

.code
; Orders WB DMA memory and uncached MMIO on the baseline CPU. The external
; call also prevents C compiler reordering around queue publication.
cvgpu_barrier proc c public
 lock or dword ptr [esp],0
 ret
cvgpu_barrier endp

cvgpu_alloc proc c public uses ebx esi edi bytes:dword
 mov ecx,bytes
 test ecx,ecx
 jz bad
 cmp ecx,01000000h
 ja bad
 call vmm_alloc_block
 ret
bad:
 xor eax,eax
 ret
cvgpu_alloc endp

cvgpu_free proc c public uses ebx esi edi storage:dword
 push 0
 push storage
 @VMMCall _PageFree
 add esp,8
 ret
cvgpu_free endp

; Translate a fixed, present Jemm allocation; never assume physical
; contiguity of a linear block. Virtqueue descriptors use these guest PFNs.
cvgpu_phys proc c public linear:dword
 mov edx,linear
 mov eax,edx
 shr eax,12
 mov eax,[PAGE_MAP+eax*4]
 test al,1
 jz bad
 and eax,0FFFFF000h
 and edx,0FFFh
 or eax,edx
 ret
bad:
 xor eax,eax
 ret
cvgpu_phys endp

cvgpu_clock_khz proc c public
 mov eax,vmm_tsc_khz
 ret
cvgpu_clock_khz endp

; The PCI driver checks capability spans against their BAR. This additional
; bound restricts mappings to small high-MMIO windows, never native RAM.
cvgpu_map_mmio proc c public uses ebx esi edi physical:dword, bytes:dword
 mov ebx,physical
 cmp ebx,80000000h
 jb bad
 mov ecx,bytes
 test ecx,ecx
 jz bad
 cmp ecx,20000h
 ja bad
 mov eax,ebx
 add eax,ecx
 jc bad
 mov esi,ebx
 and esi,0FFFh
 add ecx,esi
 add ecx,4095
 shr ecx,12
 mov edi,ecx
 push 0
 push ecx
 push PR_SYSTEM
 @VMMCall _PageReserve
 add esp,12
 cmp eax,-1
 je bad
 push eax
 shr eax,12
 shr ebx,12
 push PC_INCR or PC_WRITEABLE or PC_CACHEDIS
 push ebx
 push edi
 push eax
 @VMMCall _PageCommitPhys
 add esp,16
 pop edx
 test eax,eax
 jz failed
 mov eax,cr3
 mov cr3,eax
 lea eax,[edx+esi]
 ret
failed:
 push 0
 push edx
 @VMMCall _PageFree
 add esp,8
bad:
 xor eax,eax
 ret
cvgpu_map_mmio endp

cvgpu_unmap_mmio proc c public uses ebx esi edi linear:dword
 mov eax,linear
 and eax,0FFFFF000h
 push 0
 push eax
 @VMMCall _PageFree
 add esp,8
 ret
cvgpu_unmap_mmio endp

; Called after completed RAM writes. Deferring submission until PRESENT
; prevents partially composed bands and cursor save/restore from appearing.
gpu_damage_all proc
 cmp fb_gpu,0
 je done
 mov gpu_dirty,1
done:
 ret
gpu_damage_all endp

gpu_present proc
 cmp vmm_current,0
 jne unavailable
 cmp fb_gpu,0
 je legacy
 cmp gpu_dirty,0
 je present
 mov dev_args,0
 mov eax,fb_bytes
 mov [dev_args+4],eax
 mov eax,offset cvgpu_damage
 mov ecx,2
 call dev_call
 mov gpu_dirty,0
present:
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_EDX
 mov dev_args,eax
 mov eax,offset cvgpu_present
 mov ecx,1
 call dev_call
 test eax,eax
 jz done
 mov eax,VM_ERROR_MAPPING
 ret
legacy:
 cmp fb_legacy,0
 je unavailable
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_EDX
 mov dev_args,eax
 mov eax,offset cvlegacy_present
 mov ecx,1
 call dev_call
 test eax,eax
 jz done
 mov eax,VM_ERROR_MAPPING
 ret
unavailable:
 mov eax,VM_ERROR_FB_UNBOUND
done:
 ret
gpu_present endp

; Read-only system-VM display metadata. No device query, BIOS call, or
; hardware mutation occurs here; VirtIO state and EDID are host-owned snapshots.
gpu_display_info proc uses esi edi ebx
 cmp vmm_current,0
 jne unavailable
 movzx ecx,word ptr [ebp].Client_Reg_Struc.Client_ECX
 cmp ecx,VM_DISPLAY_INFO_SIZE
 jb bad_address
 mov ecx,VM_DISPLAY_INFO_SIZE
 call guest_destination
 test eax,eax
 jnz done
 mov ebx,edi                         ; validated flat guest destination
 mov edi,offset display_packet
 xor eax,eax
 mov ecx,VM_DISPLAY_INFO_SIZE/4
 rep stosd
 mov dword ptr [display_packet],VM_DISPLAY_INFO_MAGIC
 mov word ptr [display_packet+4],VM_ABI_VERSION
 mov word ptr [display_packet+6],VM_DISPLAY_INFO_SIZE
 mov eax,vmm_count
 dec eax
 jns @F
 xor eax,eax
@@:
 mov dword ptr [display_packet+56],eax
 mov eax,fb_physical
 mov dword ptr [display_packet+44],eax
 mov eax,fb_bytes
 mov dword ptr [display_packet+48],eax
 mov eax,gpu_mode
 mov dword ptr [display_packet+16],eax
 mov eax,[gpu_mode+4]
 mov dword ptr [display_packet+20],eax
 mov eax,[gpu_mode+8]
 mov dword ptr [display_packet+24],eax
 mov eax,[gpu_mode+12]
 mov dword ptr [display_packet+28],eax
 cmp fb_gpu,0
 je legacy_or_firmware
 mov dword ptr [display_packet+8],1
 mov eax,dword ptr cvgpu_status[16]    ; enabled phase
 mov dword ptr [display_packet+12],eax
 mov eax,dword ptr cvgpu_status[20]    ; width
 mov dword ptr [display_packet+16],eax
 mov eax,dword ptr cvgpu_status[24]    ; height
 mov dword ptr [display_packet+20],eax
 mov eax,dword ptr cvgpu_status[28]    ; pitch
 mov dword ptr [display_packet+24],eax
 mov dword ptr [display_packet+28],32
 mov eax,dword ptr cvgpu_status[48]    ; error_stage
 mov dword ptr [display_packet+32],eax
 mov eax,dword ptr cvgpu_status[36]    ; submits
 mov dword ptr [display_packet+36],eax
 mov eax,dword ptr cvgpu_status[40]    ; completions
 mov dword ptr [display_packet+40],eax
 cmp dword ptr cvgpu_edid_bytes,128
 jne copy_packet
 mov eax,128
 mov dword ptr [display_packet+52],eax
 mov esi,offset cvgpu_edid
 mov edi,offset display_packet+64
 mov ecx,128
 rep movsb
 jmp copy_packet
legacy_or_firmware:
 cmp fb_linear,0
 je @F
 mov dword ptr [display_packet+12],1
@@:
 cmp fb_legacy,0
 je copy_packet
 mov dword ptr [display_packet+8],2
 jmp copy_packet
unavailable:
 mov eax,VM_ERROR_OPERATION
 ret
bad_address:
 mov eax,VM_ERROR_ADDRESS
 ret
copy_packet:
 mov esi,offset display_packet
 mov edi,ebx
 mov ecx,VM_DISPLAY_INFO_SIZE
 rep movsb
 xor eax,eax
done:
 ret
gpu_display_info endp

gpu_legacy_bind proc
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
 mov eax,offset cvlegacy_bind
 mov ecx,6
 call dev_call
 mov fb_legacy,eax
 ret
gpu_legacy_bind endp
