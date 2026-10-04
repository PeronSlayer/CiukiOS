; Binding diagnostics and narrowly validated QEMU framebuffer cache ownership.
; Layout: magic[8], bytes, live, physical, extent, first PTE, CPUID.1.EDX,
; CR0, CR4, PAT[2], MTRRCAP[2], MTRR_DEF_TYPE[2], 32 base/mask MSR pairs.
.586p
.data
fb_cache_record db 'CVFBCACH'
 dd 576,0
 dd 6 dup (0)
 dq 3 dup (0)
 dq 64 dup (0)
fb_cache_wc dd 0
fb_cache_vram dd 0
fb_cache_plan dd 32 dup (0)
.code

fb_cache_snapshot proc
 pushfd
 pushad
 mov dword ptr [fb_cache_record+12],1
 mov eax,fb_physical
 mov dword ptr [fb_cache_record+16],eax
 mov eax,fb_bytes
 mov dword ptr [fb_cache_record+20],eax
 mov eax,fb_linear
 shr eax,12
 mov eax,[PAGE_MAP+eax*4]
 mov dword ptr [fb_cache_record+24],eax
 mov eax,cr0
 mov dword ptr [fb_cache_record+32],eax
 pushfd
 pop eax
 mov ecx,eax
 xor eax,200000h
 push eax
 popfd
 pushfd
 pop eax
 push ecx
 popfd
 xor eax,ecx
 test eax,200000h
 jz cache_snapshot_done
 mov eax,1
 cpuid
 mov dword ptr [fb_cache_record+28],edx
 test edx,20h                       ; MSRs available
 jz cache_snapshot_done
 mov ebx,edx
 mov eax,cr4                        ; MSR-capable supported Pentium profile
 mov dword ptr [fb_cache_record+36],eax
 test ebx,10000h                    ; PAT
 jz cache_snapshot_mtrr
 mov ecx,277h
 rdmsr
 mov dword ptr [fb_cache_record+40],eax
 mov dword ptr [fb_cache_record+44],edx
cache_snapshot_mtrr:
 test ebx,1000h
 jz cache_snapshot_done
 mov ecx,0FEh
 rdmsr
 mov dword ptr [fb_cache_record+48],eax
 mov dword ptr [fb_cache_record+52],edx
 movzx ebx,al
 cmp ebx,32
 jbe cache_snapshot_count
 mov ebx,32
cache_snapshot_count:
 mov ecx,2FFh
 rdmsr
 mov dword ptr [fb_cache_record+56],eax
 mov dword ptr [fb_cache_record+60],edx
 mov esi,offset fb_cache_record+64
 mov ecx,200h
 shl ebx,1
 test ebx,ebx
 jz cache_snapshot_done
cache_snapshot_pair:
 rdmsr
 mov [esi],eax
 mov [esi+4],edx
 add esi,8
 inc ecx
 dec ebx
 jnz cache_snapshot_pair
cache_snapshot_done:
 popad
 popfd
 ret
fb_cache_snapshot endp

; Only the single-CPU QEMU/Bochs adapter and measured firmware layout are
; admitted. No guessed framebuffer extent or generic MMIO becomes cached.
fb_cache_enable proc
 pushad
 cmp dword ptr [fb_cache_record+48],508h
 jne cache_enable_done
 cmp dword ptr [fb_cache_record+56],0C06h
 jne cache_enable_done
 cmp dword ptr [fb_cache_record+60],0
 jne cache_enable_done
 cmp byte ptr [fb_cache_record+40],6 ; PAT[0] must be WB
 jne cache_enable_done
 cmp dword ptr [fb_cache_record+64],80000000h
 jne cache_enable_done
 cmp dword ptr [fb_cache_record+68],0
 jne cache_enable_done
 cmp dword ptr [fb_cache_record+72],80000800h
 jne cache_enable_done
 cmp dword ptr [fb_cache_record+76],0Fh
 jne cache_enable_done
 mov esi,offset fb_cache_record+80
 mov ecx,7
cache_check_unused:
 test dword ptr [esi+8],800h
 jnz cache_enable_done
 add esi,16
 loop cache_check_unused
 mov eax,1
 cpuid
 test ecx,80000000h                 ; hypervisor only
 jz cache_enable_done
 shr ebx,16
 and ebx,0FFh
 cmp ebx,1
 ja cache_enable_done
 ; Save the PCI configuration address register and inspect bus 0 functions.
 mov dx,0CF8h
 in eax,dx
 push eax
 mov ebx,80000000h
cache_pci_next:
 mov eax,ebx
 out dx,eax
 mov dx,0CFCh
 in eax,dx
 cmp eax,11111234h
 je cache_pci_found
 mov dx,0CF8h
 add ebx,800h
 cmp ebx,80010000h
 jb cache_pci_next
 jmp cache_pci_refused
cache_pci_found:
 mov dx,0CF8h
 lea eax,[ebx+4]
 out dx,eax
 mov dx,0CFCh
 in eax,dx
 test eax,2                        ; PCI memory decoding must be enabled
 jz cache_pci_refused
 mov dx,0CF8h
 lea eax,[ebx+10h]
 out dx,eax
 mov dx,0CFCh
 in eax,dx
 mov ecx,eax
 and ecx,0Fh
 cmp ecx,8                         ; 32-bit prefetchable memory BAR
 jne cache_pci_refused
 and eax,0FFFFFFF0h
 cmp eax,fb_physical
 jne cache_pci_refused
 mov dx,0CF8h
 pop eax
 out dx,eax
 ; DISPI memory size is read-only. Preserve the selected register.
 mov dx,1CEh
 in ax,dx
 push eax
 xor eax,eax
 out dx,ax
 inc dx
 in ax,dx
 cmp ax,0B0C4h
 jb cache_dispi_refused
 cmp ax,0B0C5h
 ja cache_dispi_refused
 dec dx
 mov ax,0Ah
 out dx,ax
 inc dx
 xor eax,eax
 in ax,dx
 shl eax,16
 mov ebx,eax
 dec dx
 pop eax
 out dx,ax
 cmp ebx,1000000h                  ; >=16 MiB: eight ranges suffice
 jb cache_enable_done
 cmp ebx,10000000h
 ja cache_enable_done
 lea eax,[ebx-1]
 test eax,ebx
 jnz cache_enable_done
 test eax,fb_physical
 jnz cache_enable_done
 cmp fb_physical,80000000h
 jb cache_enable_done
 cmp ebx,fb_bytes
 jb cache_enable_done
 mov fb_cache_vram,ebx
 call fb_cache_alias_free
 jc cache_enable_done
 mov edi,offset fb_cache_plan
 xor eax,eax
 mov ecx,32
 cld
 rep stosd
 mov edi,offset fb_cache_plan
 mov eax,fb_physical
 or eax,1                         ; WC target
 mov [edi],eax
 mov eax,ebx
 neg eax
 or eax,800h
 mov [edi+8],eax
 mov dword ptr [edi+12],0Fh
 add edi,16
cache_sibling:
 cmp ebx,80000000h
 jae cache_plan_ready
 mov eax,fb_physical
 xor eax,ebx
 mov ecx,ebx
 neg ecx
 and eax,ecx                       ; disjoint UC sibling at this tree level
 mov [edi],eax
 or ecx,800h
 mov [edi+8],ecx
 mov dword ptr [edi+12],0Fh
 add edi,16
 shl ebx,1
 jmp cache_sibling
cache_plan_ready:
 mov esi,offset fb_cache_plan
 xor ebp,ebp                       ; bound PTEs: PAT index 0 (WB)
 call fb_cache_apply
 mov fb_cache_wc,1
 jmp cache_enable_done
cache_dispi_refused:
 mov dx,1CEh
 pop eax
 out dx,ax
 jmp cache_enable_done
cache_pci_refused:
 mov dx,0CF8h
 pop eax
 out dx,eax
cache_enable_done:
 popad
 ret
fb_cache_enable endp

; Reject aliases of this physical aperture outside our owned mapping.
; Walk only present page tables through Jemm's recursive page-table map.
fb_cache_alias_free proc
 pushad
 mov esi,fb_linear
 shr esi,12
 lea esi,[PAGE_MAP+esi*4]
 mov ebx,fb_physical
 mov ecx,fb_pages
 test ecx,ecx
 jz cache_alias_refused
cache_binding_page:
 mov eax,[esi]
 and eax,3
 cmp eax,3
 jne cache_alias_refused
 mov eax,[esi]
 and eax,0FFFFF000h
 cmp eax,ebx
 jne cache_alias_refused
 add ebx,1000h
 add esi,4
 loop cache_binding_page
 xor edx,edx
cache_alias_directory:
 mov eax,[PAGE_MAP+3FE000h+edx*4]
 test eax,1
 jz cache_alias_next_directory
 test eax,80h                      ; no large-page assumptions
 jnz cache_alias_refused
 mov esi,edx
 shl esi,12
 add esi,PAGE_MAP
 xor ecx,ecx
cache_alias_pte:
 mov eax,[esi+ecx*4]
 test eax,1
 jz cache_alias_next_pte
 and eax,0FFFFF000h
 sub eax,fb_physical
 cmp eax,fb_cache_vram
 jae cache_alias_next_pte
 mov eax,edx
 shl eax,22
 mov ebx,ecx
 shl ebx,12
 add eax,ebx
 sub eax,fb_linear
 shr eax,12
 cmp eax,fb_pages
 jae cache_alias_refused
cache_alias_next_pte:
 inc ecx
 cmp ecx,1024
 jb cache_alias_pte
cache_alias_next_directory:
 inc edx
 cmp edx,1024
 jb cache_alias_directory
 popad
 clc
 ret
cache_alias_refused:
 popad
 stc
 ret
fb_cache_alias_free endp

; ESI = eight complete MTRR pairs, EBP = cache bits of still-present bound
; PTEs. One CPU is admitted above. The client's CR0/CR4 are restored exactly.
fb_cache_apply proc
 pushfd
 cli
 pushad
 lock or dword ptr [esp],0          ; drain outstanding WC stores
 mov ebx,cr0
 mov edi,cr4
 mov eax,ebx
 or eax,40000000h                  ; CD=1, NW=0
 and eax,not 20000000h
 mov cr0,eax
 wbinvd
 mov eax,edi
 and eax,not 80h                   ; PGE: invalidate global translations too
 mov cr4,eax
 mov eax,cr3
 mov cr3,eax
 mov ecx,2FFh
 mov eax,dword ptr [fb_cache_record+56]
 mov edx,dword ptr [fb_cache_record+60]
 and eax,not 0CFFh
 wrmsr
 mov ecx,200h
cache_apply_pairs:
 mov eax,[esi]
 mov edx,[esi+4]
 wrmsr
 add esi,8
 inc ecx
 cmp ecx,210h
 jb cache_apply_pairs
 cmp fb_cache_wc,0                 ; disable runs after successful _PageFree
 jne cache_apply_ptes_done
 mov esi,fb_linear
 shr esi,12
 lea esi,[PAGE_MAP+esi*4]
 mov ecx,fb_pages
cache_apply_ptes:
 test dword ptr [esi],1
 jz cache_apply_next
 and dword ptr [esi],not 98h        ; PAT, PCD, PWT -> selected cache bits
 or [esi],ebp
cache_apply_next:
 add esi,4
 loop cache_apply_ptes
cache_apply_ptes_done:
 mov eax,cr3
 mov cr3,eax
 mov ecx,2FFh
 mov eax,dword ptr [fb_cache_record+56]
 mov edx,dword ptr [fb_cache_record+60]
 wrmsr
 wbinvd
 mov eax,cr3
 mov cr3,eax
 mov cr0,ebx
 mov cr4,edi
 popad
 popfd
 ret
fb_cache_apply endp

fb_cache_disable proc
 cmp fb_cache_wc,0
 je cache_disable_done
 push esi
 push ebp
 mov esi,offset fb_cache_record+64
 mov ebp,10h
 call fb_cache_apply
 mov fb_cache_wc,0
 pop ebp
 pop esi
cache_disable_done:
 ret
fb_cache_disable endp

cvvid_fb_flush proc c public
 ; Protected-mode presenter/host-aperture writes share the RAM scanout too.
 ; Submission still happens only at the desktop's completed-frame boundary.
 cmp fb_gpu,0
 je cache_flush_wc
 mov gpu_dirty,1
cache_flush_wc:
 cmp fb_cache_wc,0
 je cache_flush_done
 lock or dword ptr [esp],0
cache_flush_done:
 ret
cvvid_fb_flush endp

.386p
