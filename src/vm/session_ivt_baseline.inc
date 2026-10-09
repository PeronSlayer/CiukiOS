; Load-time ownership of raw IVT targets. No live IVT writes occur here.
; See docs/design/dosvm-ivt-baseline-ownership.md.
.data
ivt_baseline_ready dd 0
ivt_baseline_kbase dd 0
ivt_baseline_first dd 0
ivt_baseline_iret dd 0
ivt_baseline_count dd 0
ivt_baseline_free db 256 dup (0)
.code

; VMSTART, before GUI allocation: BX=kernel segment, CX=InDOS offset,
; DX=first MCB. Refuse invalid/conflicting registrations atomically.
vmm_ivt_baseline_register proc uses esi edi ebx
 pushfd
 cli
 mov eax,VM_ERROR_VMM_LAYOUT
 if KL_PRESENT eq 0
 jmp baseline_done
 else
 cmp vmm_current,0
 jne baseline_done
 movzx ecx,word ptr [ebp].Client_Reg_Struc.Client_ECX
 cmp ecx,KL_INDOS
 jne baseline_done
 movzx ebx,word ptr [ebp].Client_Reg_Struc.Client_EBX
 shl ebx,4
 movzx edx,word ptr [ebp].Client_Reg_Struc.Client_EDX
 cmp ivt_baseline_ready,0
 je baseline_new
 cmp ebx,ivt_baseline_kbase
 jne baseline_done
 cmp edx,ivt_baseline_first
 jne baseline_done
 xor eax,eax
 jmp baseline_done
baseline_new:
 cmp vmm_count,1                   ; only VM0 exists before the first fork
 jne baseline_done
 test ebx,ebx
 jz baseline_done
 test edx,edx
 jz baseline_done
 mov esi,edx
 shl esi,4
 lea edi,[ebx+KL_DEFAULT_IRET]
 cmp edi,esi
 jae baseline_done
 cmp byte ptr [edi],0CFh
 jne baseline_done
 lea edi,[ebx+KL_LAST_MCB+2]
 cmp edi,esi
 ja baseline_done
 movzx edi,word ptr [ebx+KL_LAST_MCB]
 cmp edx,edi
 ja baseline_done
 movzx ecx,word ptr ds:[0413h]
 cmp ecx,640
 ja baseline_done
 shl ecx,6                         ; BIOS conventional ceiling, paragraphs
 cmp edi,ecx
 jae baseline_done
 mov esi,edx
baseline_validate:
 cmp esi,edi
 ja baseline_invalid
 mov eax,esi
 shl eax,4
 cmp byte ptr [eax],'M'
 je baseline_kind_ok
 cmp byte ptr [eax],'Z'
 jne baseline_invalid
baseline_kind_ok:
 movzx edx,word ptr [eax+3]
 add edx,esi
 inc edx
 cmp edx,esi
 jbe baseline_invalid
 cmp edx,ecx
 ja baseline_invalid
 cmp byte ptr [eax],'Z'
 je baseline_terminal
 cmp esi,edi
 je baseline_invalid
 mov esi,edx
 jmp baseline_validate
baseline_terminal:
 cmp esi,edi
 jne baseline_invalid
 ; Whole chain validated. IRQs remain disabled until publication finishes.
 movzx edx,word ptr [ebp].Client_Reg_Struc.Client_EDX
 mov ivt_baseline_first,edx
 mov ivt_baseline_kbase,ebx
 mov eax,ebx
 shr eax,4
 shl eax,16
 or eax,KL_DEFAULT_IRET
 mov ivt_baseline_iret,eax
 mov ivt_baseline_count,0
 xor edi,edi
baseline_vector:
 mov byte ptr ivt_baseline_free[edi],0
 mov eax,ivt_snapshot[edi*4]
 cmp eax,[edi*4]
 jne baseline_next_vector
 mov ecx,eax
 shr ecx,16
 shl ecx,4
 and eax,0FFFFh
 add ecx,eax                       ; full linear address, no paragraph wrap
 mov esi,ivt_baseline_first
baseline_block:
 mov ebx,esi
 shl ebx,4
 movzx edx,word ptr [ebx+3]
 add edx,esi
 inc edx
 mov eax,edx
 shl eax,4
 cmp ecx,eax
 jae baseline_after_block
 lea eax,[ebx+16]                  ; MCB header itself is never disposable
 cmp ecx,eax
 jb baseline_next_vector
 cmp word ptr [ebx+1],0
 jne baseline_next_vector
 mov byte ptr ivt_baseline_free[edi],1
 inc ivt_baseline_count
 jmp baseline_next_vector
baseline_after_block:
 cmp byte ptr [ebx],'Z'
 je baseline_next_vector
 mov esi,edx
 jmp baseline_block
baseline_next_vector:
 inc edi
 cmp edi,256
 jb baseline_vector
 mov ivt_baseline_ready,1
 xor eax,eax
 jmp baseline_done
baseline_invalid:
 mov eax,VM_ERROR_VMM_LAYOUT
 endif
baseline_done:
 popfd
 ret
vmm_ivt_baseline_register endp

; EDI=validated guest destination, raw IVT already copied there.
; Only equal, load-time-unowned pointers are replaced in this private output.
vmm_ivt_baseline_filter proc uses esi ebx
 xor ecx,ecx                       ; substitutions in this private output
 mov edx,0FFFFh                    ; first substituted vector byte offset
 cmp ivt_baseline_ready,0
 je baseline_filter_done
 xor esi,esi
baseline_filter_next:
 cmp byte ptr ivt_baseline_free[esi],0
 je baseline_filter_skip
 mov eax,ivt_snapshot[esi*4]
 cmp eax,[esi*4]
 jne baseline_filter_skip
 mov ebx,ivt_baseline_iret
 mov [edi+esi*4],ebx
 test ecx,ecx
 jnz baseline_filter_count
 lea edx,[esi*4]
baseline_filter_count:
 inc ecx
baseline_filter_skip:
 inc esi
 cmp esi,256
 jb baseline_filter_next
baseline_filter_done:
 ret
vmm_ivt_baseline_filter endp
