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
externdef c cvsavage_bind:near
externdef c cvsavage_release:near
externdef c cvsavage_sync:near
externdef c cvsavage_fill:near
externdef c cvsavage_page_copy:near
externdef c cvsavage_triangle:near
externdef c cvsavage_status:byte
externdef c cvsavage_owned:near
externdef c cvsavage_panel_probe:near

.data
fb_gpu dd 0
fb_legacy dd 0
fb_savage dd 0
gpu_dirty dd 0
gpu_mode dd 4 dup (0) ; optional validated VBE width, height, pitch, bpp
gpu_banked dd 0
gpu_usable_bytes dd 0
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
; SuperSavage's documented register aperture is 512 KiB (status at 48C60h).
cvgpu_map_mmio proc c public uses ebx esi edi physical:dword, bytes:dword
 mov ebx,physical
 cmp ebx,00100000h
 jb bad
 mov ecx,bytes
 test ecx,ecx
 jz bad
 cmp ecx,80000h
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

; System-VM display metadata. VirtIO state is a host-owned snapshot; the
; known S3 LCD probe preserves index selectors and never changes timings.
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
 je savage_diagnostics
 mov dword ptr [display_packet+8],2
savage_diagnostics:
 mov eax,offset cvsavage_panel_probe
 xor ecx,ecx
 call dev_call
 ; Read-only snapshot, including an unsupported/failed probe. Backend 3
 ; requires actual engine ownership; a compiled driver alone is backend 0.
 mov dword ptr [display_packet+60],80000000h
 mov eax,dword ptr cvsavage_status[4]
 or dword ptr [display_packet+60],eax
 cmp fb_savage,1
 jne copy_savage_snapshot
 mov dword ptr [display_packet+8],3
 mov eax,dword ptr cvsavage_status
 mov dword ptr [display_packet+12],eax
 mov eax,dword ptr cvsavage_status[8]
 mov dword ptr [display_packet+32],eax
 mov eax,dword ptr cvsavage_status[88] ; command words submitted
 mov dword ptr [display_packet+36],eax
 mov eax,dword ptr cvsavage_status[48] ; completed fills/blits/triangles
 add eax,dword ptr cvsavage_status[52]
 add eax,dword ptr cvsavage_status[56]
 mov dword ptr [display_packet+40],eax
copy_savage_snapshot:
 mov esi,offset cvsavage_status
 mov edi,offset display_packet+64
 mov ecx,128
 rep movsb
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

; A chip-specific engine is considered only after firmware selected a real
; linear framebuffer and CVSESSION mapped its validated extent. No banked
; alias, mode programming, application-owned physical pointer or CR0 burst.
gpu_savage_bind proc
 mov fb_savage,0
 cmp vmm_current,0
 jne done
 cmp gpu_banked,0
 jne done
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
 mov eax,offset cvsavage_bind
 mov ecx,7
 call dev_call
 cmp eax,1
 je ready
 cmp eax,2
 je retained_failure
 cmp eax,-1
 je failed
 xor eax,eax
 ret
ready:
 mov fb_savage,1
 xor eax,eax
 ret
retained_failure:
 mov fb_savage,1                       ; commands may still own MMIO/scratch
failed:
 mov eax,VM_ERROR_MAPPING
 ret
done:
 xor eax,eax
 ret
gpu_savage_bind endp

gpu_savage_capabilities proc uses ebx
 xor eax,eax
 cmp fb_savage,1
 jne done
 cmp dword ptr cvsavage_status,1       ; ready, not mere owned state
 jne done
 mov ebx,dword ptr cvsavage_status[4] ; hardware capabilities
 test ebx,1
 jz @F
 or eax,VM_CAP_NATIVE_FILL
@@:
 test ebx,2
 jz done
 or eax,VM_CAP_NATIVE_TRIANGLES
done:
 ret
gpu_savage_capabilities endp

; Preserve the CPU copy's complete register state. A timeout refuses all
; subsequent CPU access rather than racing an outstanding hardware command.
gpu_savage_cpu_begin proc
 pushad
 cmp fb_savage,0
 je idle
 mov eax,offset cvsavage_sync
 xor ecx,ecx
 call dev_call
 test eax,eax
 jnz failed
idle:
 popad
 clc
 ret
failed:
 popad
 stc
 ret
gpu_savage_cpu_begin endp

; ESI points to the snapshotted, validated CVFF packet. Returns 0 completed
; by the engine, 1 not applicable (use protected CPU stores), >1 fatal.
gpu_savage_fill proc uses esi edi ebx ecx edx
 cmp fb_savage,0
 je unsupported
 mov eax,[esi+VM_FB_FILL_OFFSET]
 mov dev_args,eax
 mov eax,[esi+VM_FB_FILL_PIXELS]
 mov [dev_args+4],eax
 mov eax,[esi+VM_FB_FILL_COLOUR]
 mov [dev_args+8],eax
 mov eax,[esi+VM_FB_FILL_PIXEL_BYTES]
 mov [dev_args+12],eax
 mov eax,offset cvsavage_fill
 mov ecx,4
 call dev_call
 ret
unsupported:
 mov eax,1
 ret
gpu_savage_fill endp

; ESI points to a validated, private CVFC packet. The backend can copy a
; rectangle between any two bound display pages without CPU VRAM traffic.
; Return 0 completed, 1 safely inapplicable, >1 retained hardware failure.
gpu_savage_page_copy proc uses esi edi ebx ecx edx
 cmp fb_savage,0
 je unsupported
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
 mov eax,offset cvsavage_page_copy
 mov ecx,5
 call dev_call
 ret
unsupported:
 mov eax,1
 ret
gpu_savage_page_copy endp

gpu_savage_release proc
 cmp fb_savage,0
 je released
 mov eax,offset cvsavage_release
 xor ecx,ecx
 call dev_call
 test eax,eax
 jnz failed
 mov fb_savage,0
released:
 xor eax,eax
 ret
failed:
 mov eax,VM_ERROR_MAPPING
 ret
gpu_savage_release endp
