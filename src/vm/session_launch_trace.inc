; Durable launch diagnostics are written by VM 0, never by the child.
; See docs/design/t23-parent-launch-mailbox.md. Calls are foreground only;
; this procedure never performs disk I/O or changes a client frame directly.
vmm_launch_trace proc
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_ECX
 cmp eax,0
 je child_publish
 cmp eax,3
 je child_wait
 cmp eax,1
 je parent_read
 cmp eax,2
 je parent_ack
 jmp bad
child_publish:
 call child_record
 test eax,eax
 jnz done
 cmp [esi].VMREC.launch_pending,0
 jne bad
 test [ebp].Client_Reg_Struc.Client_EFlags,200h
 jz bad                               ; no IRQ/virtual-CLI checkpoint waits
 ; Never ask the parent to write while a child DOS transaction is live.
 mov eax,vmm_kbase
 cmp byte ptr [eax+KL_INDOS],0
 jne bad
 cmp byte ptr [eax+KL_FAT_DIRTY],0
 jne bad
 mov ebx,esi
 mov ecx,256
 call guest_destination                 ; same bounds for this readable buffer
 test eax,eax
 jnz done
 cmp dword ptr [edi],04C465643h         ; CVFL
 jne bad
 cmp dword ptr [edi+4],01000001h        ; version1/256 bytes
 jne bad
 mov eax,vmm_current
 cmp word ptr [edi+8],ax
 jne bad
 mov eax,[ebx].VMREC.generation
 cmp word ptr [edi+10],ax
 jne bad
 mov esi,edi
 lea edi,[ebx].VMREC.launch_record
 mov ecx,64
 cld
 rep movsd
 inc [ebx].VMREC.launch_sequence
 jnz sequence_ready
 inc [ebx].VMREC.launch_sequence        ; reserve 0 for an unused mailbox
sequence_ready:
 mov eax,[ebx].VMREC.launch_sequence
 mov dword ptr [ebx].VMREC.launch_record+40,eax
 mov dword ptr [ebx].VMREC.launch_record+214,0474F4C50h ; PLOG transport marker
 mov [ebx].VMREC.launch_pending,1
 mov vmm_input_urgent,0                 ; existing canonical return/HLT switch
 xor eax,eax
 ret
child_wait:
 call child_record
 test eax,eax
 jnz done
 mov eax,[esi].VMREC.launch_pending
 mov word ptr [ebp].Client_Reg_Struc.Client_EDX,ax
 xor eax,eax
 ret
parent_read:
 call parent_record
 test eax,eax
 jnz done
 mov word ptr [ebp].Client_Reg_Struc.Client_ECX,0
 cmp [esi].VMREC.launch_pending,0
 je done
 mov ebx,esi
 mov ecx,256
 call guest_destination
 test eax,eax
 jnz done
 lea esi,[ebx].VMREC.launch_record
 mov ecx,64
 cld
 rep movsd
 mov word ptr [ebp].Client_Reg_Struc.Client_ECX,1
 xor eax,eax
 ret
parent_ack:
 call parent_record
 test eax,eax
 jnz done
 cmp [esi].VMREC.launch_pending,1
 jne bad
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_ESI
 mov edx,[ebp].Client_Reg_Struc.Client_ECX
 and edx,0FFFF0000h
 or eax,edx
 cmp eax,[esi].VMREC.launch_sequence
 jne bad
 mov [esi].VMREC.launch_pending,0
 xor eax,eax
 ret
child_record:
 mov eax,vmm_current
 test eax,eax
 jz bad
 cmp eax,VMM_MAX
 jae bad
 call vmm_record
 mov ax,word ptr [ebp].Client_Reg_Struc.Client_EBX
 jmp validate_record
parent_record:
 cmp vmm_current,0
 jne bad
 movzx eax,word ptr [ebp].Client_Reg_Struc.Client_EBX
 test eax,eax
 jz bad
 cmp eax,VMM_MAX
 jae bad
 call vmm_record
 mov ax,word ptr [ebp].Client_Reg_Struc.Client_EDX
validate_record:
 cmp ax,word ptr [esi].VMREC.generation
 jne bad
 cmp [esi].VMREC.state,VM_SLOT_READY
 jne bad
 cmp [esi].VMREC.launch_enabled,1
 jne bad
 xor eax,eax
 ret
bad:
 mov eax,VM_ERROR_OPERATION
done:
 ret
vmm_launch_trace endp
